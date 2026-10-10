#include "input/gamepad_hub.h"

#include <algorithm>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "core/log.h"
#include "input/gamepad_map.h"

namespace dal {

GamepadHub::GamepadHub(std::shared_ptr<GamepadBackend> backend, std::shared_ptr<KeyboardPadState> state,
                       GamepadHubOptions options)
    : backend_(std::move(backend)), state_(std::move(state)), options_(std::move(options)) {}

void GamepadHub::step(KeyboardPadState::Clock::time_point now) {
    const std::uint64_t generation = backend_->deviceGeneration();
    if (generation_ != generation) {
        // Recorded before devices(): an arrival in between only costs one more refresh.
        generation_ = generation;
        refreshDevices();
    }

    const bool foreground = !options_.is_foreground || options_.is_foreground();
    updateFocus(foreground);
    if (!foreground) return;

    publishReadings(now);
    forwardRumble(false);
}

void GamepadHub::refreshDevices() {
    const std::vector<GamepadDeviceInfo> devices = backend_->devices();
    const std::vector<GamepadSelection> selection = selectGamepads(devices, options_.steam);

    std::vector<Used> used;
    std::map<std::uint64_t, GamepadUse> uses;
    for (std::size_t i = 0; i < devices.size() && i < selection.size(); ++i) {
        const GamepadDeviceInfo& info = devices[i];
        const GamepadSelection& chosen = selection[i];
        uses[info.id] = chosen.use;

        const auto before = uses_.find(info.id);
        if (before == uses_.end() || before->second != chosen.use) {
            if (chosen.steam) {
                logInfo("dal: pad: gamepad {} {} (steam slot {})", formatGamepadVidPid(info),
                        gamepadUseName(chosen.use), chosen.steam->slot);
            } else {
                logInfo("dal: pad: gamepad {} {}", formatGamepadVidPid(info), gamepadUseName(chosen.use));
            }
        }
        if (chosen.use != GamepadUse::Used) continue;

        Used device{.id = info.id, .rumble = info.rumble};
        const auto existing = std::ranges::find(used_, info.id, &Used::id);
        if (existing != used_.end()) device.sent = existing->sent;
        used.push_back(device);
    }
    // A device that is still connected but no longer used (Steam's virtual pad took over) must not keep
    // vibrating. A device that disappeared needs nothing; its record goes with it.
    for (const Used& old : used_) {
        const bool still_used = std::ranges::any_of(used, [&](const Used& u) { return u.id == old.id; });
        const bool still_connected =
                std::ranges::any_of(devices, [&](const GamepadDeviceInfo& d) { return d.id == old.id; });
        if (still_used || !still_connected) continue;
        if (old.rumble && old.sent.value_or(GamepadRumble{}) != GamepadRumble{}) {
            backend_->setRumble(old.id, GamepadRumble{});
        }
    }
    used_ = std::move(used);
    uses_ = std::move(uses);
    state_->setGamepadConnected(!used_.empty());
}

void GamepadHub::updateFocus(bool foreground) {
    // The window is created later on the RSX thread, so the process usually starts in the background:
    // the first step only records the state, and no pause or resume line belongs to it.
    const bool changed = foreground_.has_value() && *foreground_ != foreground;
    foreground_ = foreground;
    if (!changed) return;

    if (foreground) {
        logDebug("dal: pad: gamepad input resumed");
        return;
    }
    // Released at once, without the minimum visible time: the game must not see a button that the
    // player can no longer let go of.
    state_->releaseAll(InputSource::Gamepad);
    state_->setAnalog(PadAnalog{});
    published_buttons_ = 0;
    published_analog_ = PadAnalog{};
    forwardRumble(true);
    logDebug("dal: pad: gamepad input paused (background)");
}

void GamepadHub::publishReadings(KeyboardPadState::Clock::time_point now) {
    mapped_.clear();  // a member, so the 4 ms loop does not allocate
    for (const Used& device : used_) {
        // A device without a reading contributes nothing.
        if (const std::optional<GamepadReading> reading = backend_->read(device.id)) {
            mapped_.push_back(mapGamepadReading(*reading));
        }
    }
    // With no pad this is MappedPad{}: buttons are released with the normal minimum visible time.
    const MappedPad merged = mergeMappedPads(mapped_);
    if (merged.buttons != published_buttons_) {
        state_->setHeld(InputSource::Gamepad, merged.buttons, now);
        published_buttons_ = merged.buttons;
    }
    if (merged.analog != published_analog_) {
        state_->setAnalog(merged.analog);
        published_analog_ = merged.analog;
    }
}

void GamepadHub::forwardRumble(bool stop) {
    GamepadRumble wanted;
    if (!stop) {
        const auto [small_motor, large_motor] = state_->vibration();
        wanted = rumbleFromCellPad(small_motor, large_motor);
    }
    for (Used& device : used_) {
        if (device.rumble) sendRumble(device, wanted);
    }
}

void GamepadHub::sendRumble(Used& device, const GamepadRumble& rumble) {
    // A device that was never told anything is at rest, so a first zero needs no call.
    if (device.sent.value_or(GamepadRumble{}) == rumble) return;
    backend_->setRumble(device.id, rumble);
    device.sent = rumble;
}

void startGamepadHub(GamepadBackendFactory make_backend, std::shared_ptr<KeyboardPadState> state,
                     GamepadThreadOptions options) {
    if (!state && !options.probe) return;
    std::thread([make_backend = std::move(make_backend), state = std::move(state), options = std::move(options)] {
        auto backend = make_backend();
        if (!backend || !*backend) {
            logWarn("dal: pad: gameinput unavailable: {}; gamepads disabled",
                    backend ? std::string("no backend") : backend.error());
            return;
        }
        SteamContext steam;
        if (options.detect_steam) steam = options.detect_steam();
        logInfo("dal: pad: gameinput {}", (*backend)->description());
        logInfo("dal: pad: steam {}", formatSteamContext(steam));
        if (options.probe) logSteamVirtualGamepadInfo(steam);

        std::optional<GamepadHub> hub;
        if (state)
            hub.emplace(*backend, state, GamepadHubOptions{.is_foreground = options.is_foreground, .steam = steam});
        std::optional<GamepadProbe> probe;
        if (options.probe) probe.emplace(*backend);

        auto next_probe = KeyboardPadState::Clock::now();
        for (;;) {
            const auto now = KeyboardPadState::Clock::now();
            if (hub) hub->step(now);
            // The probe is slower than the hub: reading every device at 4 ms would only add backend calls.
            if (probe && now >= next_probe) {
                probe->step();
                next_probe = now + kGamepadProbeInterval;
            }
            std::this_thread::sleep_for(kGamepadPollInterval);
        }
    }).detach();
}

}  // namespace dal

// The gamepad thread's work: devices -> selection -> readings -> host pad state, plus the rumble
// forwarding and the focus gate. Pure core code over GamepadBackend, so tests drive it synchronously
// with a fake backend and an injected clock.
//
// Every used pad drives port 0 together with the keyboard and the script; ports 1..6 stay empty (the
// game is single-player).
//
// Threading: step() and the backend belong to one thread (the backend allows one calling thread), and
// that thread is the only writer of the Gamepad source of the state. Guest threads only read atomics.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "input/gamepad_backend.h"
#include "input/gamepad_map.h"
#include "input/gamepad_probe.h"
#include "input/gamepad_select.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"

namespace dal {

inline constexpr std::chrono::milliseconds kGamepadPollInterval{4};

struct GamepadHubOptions {
    // Whether this process owns the foreground window; null means always (tests). Gamepad input is
    // released and rumble stopped while it returns false.
    std::function<bool()> is_foreground = nullptr;
    SteamContext steam = {};
};

class GamepadHub {
public:
    GamepadHub(std::shared_ptr<GamepadBackend> backend, std::shared_ptr<KeyboardPadState> state,
               GamepadHubOptions options);

    // One pass: refreshes the device selection when the backend's generation changed, applies the
    // focus gate, publishes the merged reading of the used pads and forwards the vibration.
    void step(KeyboardPadState::Clock::time_point now);

    std::size_t usedCount() const { return used_.size(); }

private:
    struct Used {
        std::uint64_t id = 0;
        bool rumble = false;
        // What this device was last told; nullopt until the first call, which counts as zero.
        std::optional<GamepadRumble> sent = std::nullopt;
    };

    void refreshDevices();
    void updateFocus(bool foreground);
    void publishReadings(KeyboardPadState::Clock::time_point now);
    void forwardRumble(bool stop);
    void sendRumble(Used& device, const GamepadRumble& rumble);

    std::shared_ptr<GamepadBackend> backend_;
    std::shared_ptr<KeyboardPadState> state_;
    GamepadHubOptions options_;

    std::optional<std::uint64_t> generation_;
    std::map<std::uint64_t, GamepadUse> uses_;  // last logged use per device
    std::vector<Used> used_;
    std::vector<MappedPad> mapped_;  // scratch for publishReadings
    // nullopt until the first step has recorded the focus state.
    std::optional<bool> foreground_;
    PadButtons published_buttons_ = 0;
    PadAnalog published_analog_;
};

// Kept apart from GamepadHubOptions: the Steam context only exists once the backend has been created
// on the thread, so the thread detects it itself.
struct GamepadThreadOptions {
    std::function<bool()> is_foreground = nullptr;  // as GamepadHubOptions
    // Runs right after the backend exists, so `overlay loaded` reflects the state at GameInput
    // initialisation. Null: no Steam context.
    std::function<SteamContext()> detect_steam = nullptr;
    // Also logs the content of SteamVirtualGamepadInfo and drives a GamepadProbe from the same thread.
    bool probe = false;
};

// Starts a detached host thread that creates the backend (GameInput takes about a second, which must
// not delay the boot), logs the start lines, then runs a hub every kGamepadPollInterval. A null
// `state` runs the probe alone (nothing is written to a pad). A backend that cannot be created is
// one warning and the end of the thread. The thread owns its shared state and nothing joins it: the
// process ends with std::_Exit.
void startGamepadHub(GamepadBackendFactory make_backend, std::shared_ptr<KeyboardPadState> state,
                     GamepadThreadOptions options);

}  // namespace dal

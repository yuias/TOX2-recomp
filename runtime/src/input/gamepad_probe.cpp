#include "input/gamepad_probe.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "core/log.h"

namespace tox2 {
namespace {

constexpr std::size_t kInfoFileLimit = 4096;

constexpr const char* kButtonNames[] = {"Menu",
                                        "View",
                                        "A",
                                        "B",
                                        "X",
                                        "Y",
                                        "DPadUp",
                                        "DPadDown",
                                        "DPadLeft",
                                        "DPadRight",
                                        "LeftShoulder",
                                        "RightShoulder",
                                        "LeftThumbstick",
                                        "RightThumbstick"};
static_assert(std::size(kButtonNames) == static_cast<std::size_t>(GamepadButton::Count));

std::string buttonNames(GamepadButtons buttons) {
    std::string text;
    for (std::size_t i = 0; i < std::size(kButtonNames); ++i) {
        if ((buttons & (1u << i)) == 0) continue;
        if (!text.empty()) text += '+';
        text += kButtonNames[i];
    }
    return text.empty() ? "none" : text;
}

}  // namespace

std::string formatGamepadValue(float value) {
    // Adding 0.0f turns -0.0 into 0.0.
    return std::format("{:.2f}", std::round(value * 20.0f) / 20.0f + 0.0f);
}

std::string formatGamepadReading(const GamepadReading& r) {
    return std::format("buttons {} lt {} rt {} ls ({},{}) rs ({},{})", buttonNames(r.buttons),
                       formatGamepadValue(r.left_trigger), formatGamepadValue(r.right_trigger),
                       formatGamepadValue(r.left_x), formatGamepadValue(r.left_y), formatGamepadValue(r.right_x),
                       formatGamepadValue(r.right_y));
}

std::string formatGamepadKinds(std::uint32_t kinds) {
    const bool gamepad = (kinds & kGamepadKindGamepad) != 0;
    const bool controller = (kinds & kGamepadKindController) != 0;
    if (gamepad && controller) return "gamepad+controller";
    if (gamepad) return "gamepad";
    if (controller) return "controller";
    return "none";
}

std::string formatGamepadVidPid(const GamepadDeviceInfo& info) {
    return std::format("{:04x}:{:04x}", info.vendor_id, info.product_id);
}

std::string formatSteamContext(const SteamContext& steam) {
    std::string text = std::format("overlay {}", steam.overlay_loaded ? "loaded" : "not loaded");
    bool first = true;
    for (const auto& [name, value] : steam.env) {
        text += first ? "; " : " ";
        first = false;
        text += name == "SteamVirtualGamepadInfo" ? std::format("{}=set", name) : std::format("{}={}", name, value);
    }
    return text;
}

void GamepadProbe::step() {
    const std::vector<GamepadDeviceInfo> devices = backend_->devices();
    std::erase_if(devices_, [&](const auto& entry) {
        return std::ranges::none_of(devices, [&](const GamepadDeviceInfo& d) { return d.id == entry.first; });
    });

    for (const GamepadDeviceInfo& info : devices) {
        DeviceState& state = devices_[info.id];
        const std::optional<GamepadReading> reading = backend_->read(info.id);
        if (!reading) {
            if (!state.reported_missing) {
                state.reported_missing = true;
                logInfo("tox2: pad: probe {} {} no gamepad reading", info.id, formatGamepadVidPid(info));
            }
            continue;
        }
        std::string text = formatGamepadReading(*reading);
        if (text == state.last_text) continue;
        logInfo("tox2: pad: probe {} {} {}", info.id, formatGamepadVidPid(info), text);
        state.last_text = std::move(text);
    }
}

void logSteamVirtualGamepadInfo(const SteamContext& steam) {
    const auto it = steam.env.find("SteamVirtualGamepadInfo");
    if (it == steam.env.end()) {
        logInfo("tox2: pad: probe SteamVirtualGamepadInfo unset");
        return;
    }
    const std::u8string utf8(it->second.begin(), it->second.end());
    std::ifstream file{std::filesystem::path(utf8), std::ios::binary};
    if (!file) {
        logInfo("tox2: pad: probe SteamVirtualGamepadInfo unreadable");
        return;
    }
    std::string content(kInfoFileLimit, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(file.gcount()));
    if (content.empty()) {
        logInfo("tox2: pad: probe SteamVirtualGamepadInfo empty");
        return;
    }
    std::string_view rest = content;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.ends_with('\r')) line.remove_suffix(1);
        logInfo("tox2: pad: probe SteamVirtualGamepadInfo: {}", line);
    }
}

}  // namespace tox2

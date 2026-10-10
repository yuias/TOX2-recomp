// --pad-probe: logs what the gamepad backend reports, independent of what the game uses. Also the
// formatting that tox2_padtool and the device log lines share, so the texts match everywhere.

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "input/gamepad_backend.h"

namespace tox2 {

inline constexpr std::chrono::milliseconds kGamepadProbeInterval{50};

// Sticks and triggers are rounded to 0.05 steps (a negative zero prints as 0.00), so noise below a
// step does not produce lines.
std::string formatGamepadValue(float value);
// `buttons <names> lt <f> rt <f> ls (<x>,<y>) rs (<x>,<y>)`
std::string formatGamepadReading(const GamepadReading& reading);
// `gamepad`, `controller` or `gamepad+controller` (`none` for no kind).
std::string formatGamepadKinds(std::uint32_t kinds);
// `045e:028e`
std::string formatGamepadVidPid(const GamepadDeviceInfo& info);
// `overlay <loaded|not loaded>[; <NAME>=<value> ...]`; SteamVirtualGamepadInfo prints as `set`
// because its value is a file path (the probe logs the file's content instead).
std::string formatSteamContext(const SteamContext& steam);

// Reads every device the backend lists, used by the game or not, and logs
// `tox2: pad: probe <id> <vid:pid> <formatted reading>` at Info when the text changed, and
// `tox2: pad: probe <id> <vid:pid> no gamepad reading` once per device that has none.
class GamepadProbe {
public:
    explicit GamepadProbe(std::shared_ptr<GamepadBackend> backend) : backend_(std::move(backend)) {}
    // One pass; the probe thread calls it every kGamepadProbeInterval and the tests call it directly.
    void step();

private:
    struct DeviceState {
        std::string last_text;
        bool reported_missing = false;
    };
    std::shared_ptr<GamepadBackend> backend_;
    std::map<std::uint64_t, DeviceState> devices_;
};

// Logs the content of the file named by SteamVirtualGamepadInfo (first 4 KiB, line by line), or that
// the variable is unset or the file unreadable.
void logSteamVirtualGamepadInfo(const SteamContext& steam);

using GamepadBackendFactory = std::function<std::expected<std::shared_ptr<GamepadBackend>, std::string>()>;

}  // namespace tox2

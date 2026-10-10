// Which of the connected devices the game reads as its gamepads, given what the process knows about
// Steam. Pure functions over GamepadDeviceInfo and SteamContext; the hub applies them.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "input/gamepad_backend.h"

namespace tox2 {

// Steam's virtual pad, presented to a hooked process as an Xbox 360 pad (VID 045E, PID 028E).
struct SteamVirtualPad {
    std::uint16_t real_vendor_id = 0, real_product_id = 0;  // the physical pad behind it
    std::uint32_t slot = 0;
    std::uint32_t process_id = 0;  // 0 when the path has none
};

// Recognises Steam's virtual pad by its pnpPath:
//   \\.\pipe\HID#VID_045E&PID_028E&IG_<n>#<vid hex>&<pid hex>&<handle hex>#<slot>#<process id>
// Prefix and hex digits are case-insensitive, slot and process id are decimal, the process id is
// optional.
std::optional<SteamVirtualPad> parseSteamVirtualPath(std::string_view pnp_path);

// SDL_GAMECONTROLLER_IGNORE_DEVICES: comma-separated `0xVVVV/0xPPPP` entries; malformed entries are
// skipped.
std::vector<std::pair<std::uint16_t, std::uint16_t>> parseIgnoreDevices(std::string_view value);

enum class GamepadUse : std::uint8_t {
    Used,            // read and mapped to port 0
    HandledBySteam,  // physical pad that Steam's virtual pad stands for
    OtherProcess,    // Steam virtual pad of another process
    NotGamepad,      // no gamepad kind
};

struct GamepadSelection {
    std::uint64_t id = 0;
    GamepadUse use = GamepadUse::Used;
    // The virtual pad itself, or the one that stands for a HandledBySteam device (empty when the
    // device was dropped by the ignore list alone).
    std::optional<SteamVirtualPad> steam;
};

// One entry per device, in the order of `devices`:
// 1. no gamepad kind -> NotGamepad;
// 2. a Steam virtual pad -> OtherProcess when its process id is non-zero and differs from
//    steam.process_id, else Used;
// 3. a physical device -> HandledBySteam when its VID/PID is the real VID/PID of a virtual pad in
//    the list, or, with the overlay loaded, is in SDL_GAMECONTROLLER_IGNORE_DEVICES; else Used.
// Without any Steam marker every gamepad-kind device is Used. Two identical physical pads of which
// Steam handles one are both dropped (single player).
std::vector<GamepadSelection> selectGamepads(std::span<const GamepadDeviceInfo> devices, const SteamContext& steam);

std::string_view gamepadUseName(GamepadUse use);  // "used", "handled by steam", ...

}  // namespace tox2

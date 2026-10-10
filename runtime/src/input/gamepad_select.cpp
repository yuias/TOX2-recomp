#include "input/gamepad_select.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace tox2 {

namespace {

// Steam's virtual pad is an Xbox 360 pad. No virtual pad has been observed on a real system, so
// this layout is SDL's reading of Steam's path and is only unit-tested; the IG index is accepted
// loosely for that reason.
constexpr std::string_view kVirtualPathPrefix = R"(\\.\pipe\HID#VID_045E&PID_028E&IG_)";
constexpr std::string_view kIgnoreDevicesVar = "SDL_GAMECONTROLLER_IGNORE_DEVICES";

bool startsWithNoCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

// Whole-string number; from_chars rejects signs and "0x" for unsigned types.
template <typename T>
std::optional<T> parseNumber(std::string_view text, int base) {
    T value{};
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (text.empty() || ec != std::errc{} || ptr != end) return std::nullopt;
    return value;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t at = text.find(separator);
        parts.push_back(text.substr(0, at));
        if (at == std::string_view::npos) return parts;
        text.remove_prefix(at + 1);
    }
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// `0x` followed by one to four hex digits.
std::optional<std::uint16_t> parseHexId(std::string_view text) {
    if (text.size() < 3 || text.size() > 6 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return std::nullopt;
    }
    return parseNumber<std::uint16_t>(text.substr(2), 16);
}

}  // namespace

std::optional<SteamVirtualPad> parseSteamVirtualPath(std::string_view pnp_path) {
    if (!startsWithNoCase(pnp_path, kVirtualPathPrefix)) return std::nullopt;
    pnp_path.remove_prefix(kVirtualPathPrefix.size());

    // <ig index>#<vid>&<pid>&<handle>#<slot>[#<process id>[#...]]
    const std::vector<std::string_view> fields = split(pnp_path, '#');
    if (fields.size() < 3 || !parseNumber<std::uint32_t>(fields[0], 16)) return std::nullopt;

    const std::vector<std::string_view> ids = split(fields[1], '&');
    if (ids.size() != 3) return std::nullopt;
    const auto real_vendor = parseNumber<std::uint16_t>(ids[0], 16);
    const auto real_product = parseNumber<std::uint16_t>(ids[1], 16);
    if (!real_vendor || !real_product || !parseNumber<std::uint64_t>(ids[2], 16)) return std::nullopt;

    const auto slot = parseNumber<std::uint32_t>(fields[2], 10);
    if (!slot) return std::nullopt;

    SteamVirtualPad pad;
    pad.real_vendor_id = *real_vendor;
    pad.real_product_id = *real_product;
    pad.slot = *slot;
    if (fields.size() > 3) {
        const auto process_id = parseNumber<std::uint32_t>(fields[3], 10);
        if (!process_id) return std::nullopt;
        pad.process_id = *process_id;
    }
    return pad;
}

std::vector<std::pair<std::uint16_t, std::uint16_t>> parseIgnoreDevices(std::string_view value) {
    std::vector<std::pair<std::uint16_t, std::uint16_t>> ids;
    for (std::string_view entry : split(value, ',')) {
        const std::vector<std::string_view> halves = split(trim(entry), '/');
        if (halves.size() != 2) continue;
        const auto vendor = parseHexId(halves[0]);
        const auto product = parseHexId(halves[1]);
        if (vendor && product) ids.emplace_back(*vendor, *product);
    }
    return ids;
}

std::vector<GamepadSelection> selectGamepads(std::span<const GamepadDeviceInfo> devices, const SteamContext& steam) {
    std::vector<std::optional<SteamVirtualPad>> virtual_pads;
    virtual_pads.reserve(devices.size());
    for (const GamepadDeviceInfo& device : devices) virtual_pads.push_back(parseSteamVirtualPath(device.pnp_path));

    std::vector<std::pair<std::uint16_t, std::uint16_t>> ignored;
    if (steam.overlay_loaded) {
        if (const auto it = steam.env.find(kIgnoreDevicesVar); it != steam.env.end()) {
            ignored = parseIgnoreDevices(it->second);
        }
    }

    std::vector<GamepadSelection> result;
    result.reserve(devices.size());
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const GamepadDeviceInfo& device = devices[i];
        GamepadSelection selection;
        selection.id = device.id;

        if ((device.kinds & kGamepadKindGamepad) == 0) {
            selection.use = GamepadUse::NotGamepad;
        } else if (virtual_pads[i]) {
            selection.steam = virtual_pads[i];
            const std::uint32_t owner = virtual_pads[i]->process_id;
            selection.use = owner != 0 && owner != steam.process_id ? GamepadUse::OtherProcess : GamepadUse::Used;
        } else {
            for (const std::optional<SteamVirtualPad>& pad : virtual_pads) {
                if (pad && pad->real_vendor_id == device.vendor_id && pad->real_product_id == device.product_id) {
                    selection.use = GamepadUse::HandledBySteam;
                    selection.steam = pad;
                    break;
                }
            }
            if (selection.use == GamepadUse::Used &&
                std::ranges::find(ignored, std::pair{device.vendor_id, device.product_id}) != ignored.end()) {
                selection.use = GamepadUse::HandledBySteam;
            }
        }
        result.push_back(selection);
    }
    return result;
}

std::string_view gamepadUseName(GamepadUse use) {
    switch (use) {
        case GamepadUse::Used: return "used";
        case GamepadUse::HandledBySteam: return "handled by steam";
        case GamepadUse::OtherProcess: return "other process";
        case GamepadUse::NotGamepad: return "not a gamepad";
    }
    return "unknown";
}

}  // namespace tox2

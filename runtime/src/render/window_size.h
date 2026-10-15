#pragma once

// Window size presets and the arithmetic that fits them to a screen. Header-only and free of Vulkan and Windows
// headers, so the app layer and the unit tests can use it without the renderer.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tox2 {

// Named after the pixel resolution of the client area. Sizes are physical pixels; DPI scaling does not change them.
enum class WindowSizePreset : std::uint8_t { Size720p, Size1080p, Size1440p, Size2160p };

inline constexpr std::array<WindowSizePreset, 4> kWindowSizePresets = {
        WindowSizePreset::Size720p,
        WindowSizePreset::Size1080p,
        WindowSizePreset::Size1440p,
        WindowSizePreset::Size2160p,
};

struct ClientSize {
    std::uint32_t width = 0, height = 0;
    bool operator==(const ClientSize&) const = default;
};

constexpr ClientSize presetClientSize(WindowSizePreset preset) {
    switch (preset) {
        case WindowSizePreset::Size720p: return {1280, 720};
        case WindowSizePreset::Size1080p: return {1920, 1080};
        case WindowSizePreset::Size1440p: return {2560, 1440};
        case WindowSizePreset::Size2160p: return {3840, 2160};
    }
    return {1280, 720};
}

constexpr std::string_view windowSizeName(WindowSizePreset preset) {
    switch (preset) {
        case WindowSizePreset::Size720p: return "720p";
        case WindowSizePreset::Size1080p: return "1080p";
        case WindowSizePreset::Size1440p: return "1440p";
        case WindowSizePreset::Size2160p: return "2160p";
    }
    return "720p";
}

// The match is exact (case-sensitive); nullopt for anything else.
constexpr std::optional<WindowSizePreset> parseWindowSize(std::string_view text) {
    for (const WindowSizePreset preset : kWindowSizePresets) {
        if (text == windowSizeName(preset)) return preset;
    }
    return std::nullopt;
}

// The client size a preset gets when only `available` client pixels fit (the monitor work area minus the window
// frame): `wanted` itself when it fits, else the largest 16:9 size inside `available` with a width that is a
// multiple of 16 (so the height is exact), never below 16x9.
constexpr ClientSize fitClientSize(ClientSize wanted, ClientSize available) {
    if (wanted.width <= available.width && wanted.height <= available.height) return wanted;
    // 64 bits so a huge `available` cannot wrap the 16:9 product.
    const std::uint64_t width_for_height = std::uint64_t{available.height} * 16 / 9;
    const std::uint64_t limit = std::min<std::uint64_t>(available.width, width_for_height);
    const std::uint32_t width = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(limit / 16 * 16));
    return {width, width / 16 * 9};
}

// Screen coordinates, edges as in a Win32 RECT (right and bottom are exclusive).
struct ScreenRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const ScreenRect&) const = default;
};

// `window` moved, keeping its size, the least distance that puts it inside `work`; a window larger than `work`
// is aligned to its left and top edges.
constexpr ScreenRect placeInWorkArea(ScreenRect window, ScreenRect work) {
    // 64 bits so extreme coordinates cannot wrap.
    const std::int64_t width = std::int64_t{window.right} - window.left;
    const std::int64_t height = std::int64_t{window.bottom} - window.top;
    // Pull the right and bottom edges in first; the left and top edges then win for an oversized window.
    const std::int64_t left =
            std::max<std::int64_t>(work.left, std::min<std::int64_t>(window.left, work.right - width));
    const std::int64_t top = std::max<std::int64_t>(work.top, std::min<std::int64_t>(window.top, work.bottom - height));
    return {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top), static_cast<std::int32_t>(left + width),
            static_cast<std::int32_t>(top + height)};
}

// What the window reports to the overlay. Written on the UI thread, read from any thread.
struct WindowStatus {
    WindowSizePreset preset = WindowSizePreset::Size720p;  // the last preset applied or requested
    ClientSize client;                                     // current client area (0x0 while minimised)
    ClientSize available;  // client pixels the work area allows with the windowed frame at the window's DPI
    bool fullscreen = false;
};

}  // namespace tox2

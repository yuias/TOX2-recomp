#pragma once

// Pixel arithmetic shared by the renderer's passes: guest rectangles (clip pixels) to image pixels.
//
// Future rules for features that depend on the pixel position, to apply when they are added:
//  - WPOS must be gl_FragCoord.xy divided by the pass's PixelScale, DDX/DDY results multiplied by it.
//  - A pixel-centre offset of half a guest pixel is half a PixelScale in image pixels.
//  - A depth-bias slope factor must be multiplied by the PixelScale (the slope is per image pixel).
//  - Any surface-to-guest-memory path must read the layout-size data (sample the image at each layout pixel's
//    centre or box-average it), never the image itself.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace tox2 {

inline constexpr std::uint32_t kMaxResolutionScale = 4;

// How surfaces in AA mode 3 are rendered. Supersample: a single-sample image in the memory layout, shaded per
// sample. Msaa: a 2-sample image at the clip size, shaded per pixel, copied into the layout when read.
enum class AaMode : std::uint8_t { Supersample, Msaa };

constexpr std::string_view aaModeName(AaMode mode) { return mode == AaMode::Msaa ? "msaa" : "supersample"; }

// The two names above; nullopt for anything else (the match is exact).
constexpr std::optional<AaMode> parseAaMode(std::string_view text) {
    if (text == "supersample") return AaMode::Supersample;
    if (text == "msaa") return AaMode::Msaa;
    return std::nullopt;
}

// Image pixels per clip pixel of one pass: the target's resolution scale times the samples per pixel of the
// surface's AA layout.
struct PixelScale {
    std::uint32_t x = 1, y = 1;
};

// A rectangle in image pixels.
struct ImageRect {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};

// The guest rectangle (x, y, width, height) in image pixels, clamped to an area of limit_w x limit_h image
// pixels: x' = min(x * s.x, limit_w), width' = min(width * s.x, limit_w - x'), the same for y. Products are
// formed in 64 bits so the register values of a hostile guest cannot wrap. With s = (1, 1) this is the clamp
// the renderer always applied to scissors.
constexpr ImageRect scaleRect(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, PixelScale s,
                              std::uint32_t limit_w, std::uint32_t limit_h) {
    const auto clampAxis = [](std::uint32_t origin, std::uint32_t extent, std::uint32_t factor, std::uint32_t limit,
                              std::uint32_t& out_origin, std::uint32_t& out_extent) {
        const std::uint64_t scaled_origin = std::min<std::uint64_t>(std::uint64_t{origin} * factor, limit);
        const std::uint64_t scaled_extent =
                std::min<std::uint64_t>(std::uint64_t{extent} * factor, limit - scaled_origin);
        out_origin = static_cast<std::uint32_t>(scaled_origin);
        out_extent = static_cast<std::uint32_t>(scaled_extent);
    };
    ImageRect rect;
    clampAxis(x, width, s.x, limit_w, rect.x, rect.width);
    clampAxis(y, height, s.y, limit_h, rect.y, rect.height);
    return rect;
}

}  // namespace tox2

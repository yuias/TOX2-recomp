#pragma once

// PNG to RGBA8 conversion for host-side images (save data icons). libpng stays private to this
// library; the guest-facing PNG decoder in hle/cell_png_dec.cpp has its own codec.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tox2 {

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // R, G, B, A rows, top first
};

// Any PNG libpng reads, converted to 8-bit RGBA. nullopt on a decode error or when a side exceeds
// `max_side`.
std::optional<RgbaImage> decodePngRgba8(std::span<const std::uint8_t> png, std::uint32_t max_side = 1024);

}  // namespace tox2

#include "rsx/rsx_texture_layout.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "rsx/rsx_decode.h"

namespace tox2 {
namespace {

constexpr std::uint64_t kLayerAlignment = 128;  // RPCS3 `get_subresources_layout_impl`

constexpr bool isPowerOfTwo(std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Moves the low 16 bits of v to the even bit positions.
constexpr std::uint32_t spreadBits(std::uint32_t v) {
    v &= 0xFFFF;
    v = (v | (v << 8)) & 0x00FF00FF;
    v = (v | (v << 4)) & 0x0F0F0F0F;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

}  // namespace

std::uint32_t rsxTextureMipCount(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t mips) {
    if (mips == 0 || width == 0 || height == 0) return 1;
    std::uint32_t chain;
    if (rsxTextureBlockBytes(format) != 0) {
        // A compressed chain ends when either side is down to one block (RPCS3 assumes the same for every host API).
        const std::uint32_t blocks = std::min(width / 4, height / 4);
        chain = blocks == 0 ? 1 : std::bit_width(blocks);
    } else {
        chain = std::bit_width(std::max(width, height));
    }
    return std::min(mips, chain);
}

std::uint32_t rsxSwizzleIndex3d(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t log2_w,
                                std::uint32_t log2_h, std::uint32_t log2_d) {
    // One bit of each coordinate per round, skipping the ones that have no bits left.
    std::uint32_t index = 0, shift = 0;
    for (std::uint32_t bit = 0; bit < std::max({log2_w, log2_h, log2_d}); bit++) {
        if (bit < log2_w) index |= ((x >> bit) & 1u) << shift++;
        if (bit < log2_h) index |= ((y >> bit) & 1u) << shift++;
        if (bit < log2_d) index |= ((z >> bit) & 1u) << shift++;
    }
    return index;
}

std::uint32_t rsxSwizzleIndex(std::uint32_t x, std::uint32_t y, std::uint32_t log2_w, std::uint32_t log2_h) {
    const std::uint32_t shared = std::min(log2_w, log2_h);
    const std::uint32_t mask = (1u << shared) - 1;
    std::uint32_t index = spreadBits(x & mask) | (spreadBits(y & mask) << 1);
    // The longer side keeps counting above the interleaved bits.
    if (log2_w > log2_h) {
        index |= (x >> shared) << (2 * shared);
    } else if (log2_h > log2_w) {
        index |= (y >> shared) << (2 * shared);
    }
    return index;
}

bool rsxTextureLayout(std::uint32_t format, bool linear, std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                      std::uint32_t pitch, std::uint32_t layers, RsxTextureLayout& out, std::uint32_t depth) {
    const std::uint32_t dxt_bytes = rsxTextureBlockBytes(format);
    const bool compressed = dxt_bytes != 0;
    const std::uint32_t block_bytes =
            compressed ? dxt_bytes : ((format & kRsxTextureBaseMask) == kRsxTextureA8R8G8B8 ? 4 : 0);
    if (block_bytes == 0 || width == 0 || height == 0 || layers == 0 || depth == 0) return false;

    const bool swizzled = !compressed && !linear;
    if (swizzled && (!isPowerOfTwo(width) || !isPowerOfTwo(height) || (depth > 1 && !isPowerOfTwo(depth)))) {
        return false;
    }

    const std::uint32_t edge = compressed ? 4 : 1;
    const std::uint64_t first_row = static_cast<std::uint64_t>((width + edge - 1) / edge) * block_bytes;
    // Only the linear flag gives rows a stride of their own.
    const std::uint64_t padded_pitch = linear ? (pitch == 0 ? first_row : pitch) : 0;
    if (linear && padded_pitch < first_row) return false;

    RsxTextureLayout layout;
    layout.mip_count = rsxTextureMipCount(format, width, height, mips);
    layout.layer_count = layers;
    layout.block_bytes = block_bytes;
    std::uint64_t guest = 0;
    for (std::uint32_t layer = 0; layer < layers; layer++) {
        std::uint32_t w = width, h = height, d = depth;
        for (std::uint32_t level = 0; level < layout.mip_count; level++) {
            RsxTextureLevel l;
            l.layer = layer;
            l.level = level;
            l.width = w;
            l.height = h;
            l.depth = d;
            l.blocks_w = (w + edge - 1) / edge;
            l.blocks_h = (h + edge - 1) / edge;
            l.row_bytes = static_cast<std::uint64_t>(l.blocks_w) * block_bytes;
            l.guest_pitch = linear ? padded_pitch : l.row_bytes;
            l.guest_offset = guest;
            l.guest_bytes = l.guest_pitch * (static_cast<std::uint64_t>(l.blocks_h) * d - 1) + l.row_bytes;
            l.packed_offset = layout.packed_bytes;
            l.packed_bytes = l.row_bytes * l.blocks_h * d;
            l.swizzled = swizzled;
            l.vtc = compressed && d > 1 && isPowerOfTwo(w) && isPowerOfTwo(h);
            layout.levels.push_back(l);
            layout.packed_bytes += l.packed_bytes;
            layout.guest_bytes = std::max(layout.guest_bytes, l.guest_offset + l.guest_bytes);
            guest += l.guest_pitch * l.blocks_h * d;
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            d = std::max(d / 2, 1u);
        }
        if (!linear) guest = (guest + kLayerAlignment - 1) & ~(kLayerAlignment - 1);
    }
    out = std::move(layout);
    return true;
}

bool rsxExtractTextureLevel(const RsxTextureLevel& level, std::uint32_t block_bytes,
                            std::span<const std::uint8_t> guest, std::span<std::uint8_t> out) {
    if (block_bytes == 0 || level.guest_offset + level.guest_bytes > guest.size() || out.size() < level.packed_bytes) {
        return false;
    }
    const std::uint8_t* source = guest.data() + level.guest_offset;
    const std::uint64_t slice_blocks = static_cast<std::uint64_t>(level.blocks_w) * level.blocks_h;
    if (level.vtc) {
        // Blocks of one position are adjacent across the 4 slices of a group; the last group holds the
        // remaining depth % 4 slices (RPCS3 `copy_unmodified_block_vtc`).
        for (std::uint32_t z = 0; z < level.depth; z++) {
            const std::uint32_t group_start = z & ~3u;
            const std::uint32_t group_size = std::min(4u, level.depth - group_start);
            const std::uint64_t group_base = static_cast<std::uint64_t>(group_start) * slice_blocks;
            for (std::uint64_t block = 0; block < slice_blocks; block++) {
                const std::uint64_t from = (group_base + block * group_size + (z - group_start)) * block_bytes;
                std::memcpy(out.data() + (z * slice_blocks + block) * block_bytes, source + from, block_bytes);
            }
        }
        return true;
    }
    if (!level.swizzled) {
        for (std::uint32_t row = 0; row < level.blocks_h * level.depth; row++) {
            std::memcpy(out.data() + row * level.row_bytes, source + row * level.guest_pitch, level.row_bytes);
        }
        return true;
    }
    // Swizzled levels are power-of-two sized (checked by rsxTextureLayout).
    const std::uint32_t log2_w = static_cast<std::uint32_t>(std::bit_width(level.width)) - 1;
    const std::uint32_t log2_h = static_cast<std::uint32_t>(std::bit_width(level.height)) - 1;
    const std::uint32_t log2_d = static_cast<std::uint32_t>(std::bit_width(level.depth)) - 1;
    for (std::uint32_t z = 0; z < level.depth; z++) {
        for (std::uint32_t y = 0; y < level.height; y++) {
            for (std::uint32_t x = 0; x < level.width; x++) {
                const std::uint32_t index = level.depth == 1 ? rsxSwizzleIndex(x, y, log2_w, log2_h)
                                                             : rsxSwizzleIndex3d(x, y, z, log2_w, log2_h, log2_d);
                const std::uint64_t to = ((static_cast<std::uint64_t>(z) * level.height + y) * level.width + x);
                std::memcpy(out.data() + to * block_bytes, source + static_cast<std::uint64_t>(index) * block_bytes,
                            block_bytes);
            }
        }
    }
    return true;
}

}  // namespace tox2

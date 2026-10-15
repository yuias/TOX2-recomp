#pragma once

// Guest memory layout of a 2D texture with its mip chain, and the conversion to tightly packed rows.
// Rules follow RPCS3 `Emu/RSX/Common/TextureUtils.cpp` (`get_subresources_layout_impl`,
// `get_texture_size`, `get_format_packed_pitch`), `Emu/RSX/RSXTexture.cpp` (`get_exact_mipmap_count`) and
// `Emu/RSX/Utils/image_utils.hpp` (`convert_linear_swizzle`):
//  - a level is max(size / 2, 1) in each dimension;
//  - a linear (LN) texture keeps the pitch of level 0 for every level, so a level is pitch * block rows
//    bytes and the next one follows without padding;
//  - a swizzled texture has no row padding: a level is width * height texels, stored in Morton order
//    (x and y bits interleaved up to the shorter side, the rest of the longer side above them);
//  - a block-compressed (DXT) texture is never swizzled; with the swizzle flag its block rows are
//    contiguous (the pitch register is ignored), with the linear flag they use the pitch register;
//  - after each layer (a cube face) a texture without the linear flag is aligned to 128 bytes;
//  - a 3D level is `depth` slices of the 2D level one after the other, and the depth is halved (to at least 1)
//    from one level to the next like the sides; a swizzled 3D level interleaves the x, y and z bits (one bit of
//    each per round while it has bits left) so its three sizes must be powers of two; a DXT level of a 3D
//    texture whose width and height are powers of two and whose depth is above 1 is stored in the NVIDIA VTC
//    order: blocks of the same position in 4 consecutive slices are adjacent (a final group of 1 to 3 slices
//    holds that many), which the extraction undoes.
// Only textures of 4-byte texels (A8R8G8B8), 1-byte texels (B8) and DXT1/DXT23/DXT45 are described.

#include <cstdint>
#include <span>
#include <vector>

namespace tox2 {

struct RsxTextureLevel {
    std::uint32_t layer = 0, level = 0;
    std::uint32_t width = 0, height = 0, depth = 1;     // texels; depth in slices
    std::uint32_t blocks_w = 0, blocks_h = 0;           // 4x4 blocks for DXT, texels otherwise
    std::uint64_t guest_offset = 0;                     // from the texture base address
    std::uint64_t guest_pitch = 0;                      // bytes from one row to the next in guest memory
    std::uint64_t guest_bytes = 0;                      // bytes read from guest_offset (no padding after the last row)
    std::uint64_t row_bytes = 0;                        // blocks_w * bytes per block
    std::uint64_t packed_offset = 0, packed_bytes = 0;  // the same level in tightly packed rows and slices
    bool swizzled = false;
    bool vtc = false;  // DXT slices interleaved in groups of 4 (see above)
};

struct RsxTextureLayout {
    std::uint32_t mip_count = 0, layer_count = 0;
    std::uint32_t block_bytes = 0;  // bytes of one texel (A8R8G8B8, B8) or of one 4x4 block (DXT)
    std::uint64_t guest_bytes = 0;  // guest range [0, guest_bytes) the levels read
    std::uint64_t packed_bytes = 0;
    std::vector<RsxTextureLevel> levels;  // layer by layer, level 0 first
};

// Number of levels the texture really has: the register value (0 counts as 1) clamped to the chain that
// ends at one texel, or at one block for DXT (RPCS3 `get_exact_mipmap_count`).
std::uint32_t rsxTextureMipCount(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t mips);

// Layout of `layers` (1, or 6 for a cube map) images of `depth` slices (1 unless the texture is 3D) with
// `mips` levels. `format` is the 8-bit texture format field; the layout comes from `linear`, not from the
// flag bit in `format`. pitch 0 stands for the row size of level 0. False (and `out` untouched) for a format
// other than A8R8G8B8, B8 and DXT, an empty size, a pitch smaller than a row of level 0, or a swizzled texture
// (DXT excepted) whose size, or depth when above 1, is not a power of two.
bool rsxTextureLayout(std::uint32_t format, bool linear, std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                      std::uint32_t pitch, std::uint32_t layers, RsxTextureLayout& out, std::uint32_t depth = 1);

// Position of texel (x, y) in a swizzled image of 2^log2_w x 2^log2_h texels, in texels.
std::uint32_t rsxSwizzleIndex(std::uint32_t x, std::uint32_t y, std::uint32_t log2_w, std::uint32_t log2_h);

// The same for texel (x, y, z) of a swizzled 3D image of 2^log2_w x 2^log2_h x 2^log2_d texels (RPCS3
// `calculate_z_index`). With log2_d 0 it equals rsxSwizzleIndex.
std::uint32_t rsxSwizzleIndex3d(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t log2_w,
                                std::uint32_t log2_h, std::uint32_t log2_d);

// Copies one level out of the guest bytes `guest` (the texture range, level offsets as in the layout) to
// `out`, packed_bytes long, in tightly packed rows and slices, de-swizzling a swizzled level and undoing
// the VTC order of a DXT level. False when `guest` is too short.
bool rsxExtractTextureLevel(const RsxTextureLevel& level, std::uint32_t block_bytes,
                            std::span<const std::uint8_t> guest, std::span<std::uint8_t> out);

}  // namespace tox2

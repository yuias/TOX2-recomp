#pragma once

// Pure decoders from the RSX register file to the plain structs in rsx/rsx_frame.h. Values the
// renderer cannot handle yet are counted with rsxNoteUnimplemented and mapped to the Unsupported
// enumerators. Register numbers, bit layouts and enum values follow RPCS3 `Emu/RSX/rsx_decode.h`
// (one `registers_decoder` per register), `Emu/RSX/gcm_enums.h` and `Emu/RSX/RSXTexture.cpp`; the
// surface size rule follows `Emu/RSX/RSXThread.cpp` (`thread::get_framebuffer_layout`).

#include <array>
#include <cstdint>
#include <vector>

#include "rsx/rsx_frame.h"
#include "rsx/rsx_registers.h"

namespace tox2 {

// CLEAR_SURFACE mask bits (RPCS3 `RSX_GCM_CLEAR_*_BIT`).
inline constexpr std::uint32_t kRsxClearDepth = 0x01;
inline constexpr std::uint32_t kRsxClearStencil = 0x02;
inline constexpr std::uint32_t kRsxClearRed = 0x10;
inline constexpr std::uint32_t kRsxClearGreen = 0x20;
inline constexpr std::uint32_t kRsxClearBlue = 0x40;
inline constexpr std::uint32_t kRsxClearAlpha = 0x80;

// SET_SURFACE_FORMAT codes (RPCS3 `CELL_GCM_SURFACE_*`).
inline constexpr std::uint32_t kRsxSurfaceColorA8R8G8B8 = 8;
inline constexpr std::uint32_t kRsxSurfaceColorFX32 = 13;
inline constexpr std::uint32_t kRsxSurfaceDepthZ16 = 1;
inline constexpr std::uint32_t kRsxSurfaceDepthZ24S8 = 2;

// Surface DMA contexts (RPCS3 `CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER` / `_HOST_BUFFER`).
inline constexpr std::uint32_t kRsxContextDmaLocal = 0xFEED0000;
inline constexpr std::uint32_t kRsxContextDmaMain = 0xFEED0001;

// 8-bit texture format field of SET_TEXTURE_FORMAT: base format, layout flag, normalization flag.
inline constexpr std::uint32_t kRsxTextureBaseMask = 0x9F;
inline constexpr std::uint32_t kRsxTextureLinearFlag = 0x20;  // CELL_GCM_TEXTURE_LN; clear = swizzled
inline constexpr std::uint32_t kRsxTextureUnnormalizedFlag =
        0x40;  // CELL_GCM_TEXTURE_UN; clear = _NR (normalized coords)
// One unsigned byte per texel (the planes of the game's decoded movies).
inline constexpr std::uint32_t kRsxTextureB8 = 0x81;
inline constexpr std::uint32_t kRsxTextureA8R8G8B8 = 0x85;
inline constexpr std::uint32_t kRsxTextureDxt1 = 0x86;   // S3TC BC1, 8-byte blocks
inline constexpr std::uint32_t kRsxTextureDxt23 = 0x87;  // BC2, 16-byte blocks
inline constexpr std::uint32_t kRsxTextureDxt45 = 0x88;  // BC3, 16-byte blocks

// One 32-bit float per texel. Only a render target supplies it (rsxTextureFormatRenderTargetOnly).
inline constexpr std::uint32_t kRsxTextureX32Float = 0x9C;

// Bytes of one 4x4 block of a DXT format (the base format after masking the layout and normalization
// flags); 0 for any other format.
constexpr std::uint32_t rsxTextureBlockBytes(std::uint32_t format) {
    switch (format & kRsxTextureBaseMask) {
        case kRsxTextureDxt1: return 8;
        case kRsxTextureDxt23:
        case kRsxTextureDxt45: return 16;
        default: return 0;
    }
}

// Block-compressed textures are never swizzled and are stored as rows of 4x4 blocks, each row
// ceil(width / 4) blocks wide. Both functions return 0 for a non-DXT format.
constexpr std::uint64_t rsxCompressedRowBytes(std::uint32_t format, std::uint32_t width) {
    return static_cast<std::uint64_t>((width + 3) / 4) * rsxTextureBlockBytes(format);
}
constexpr std::uint64_t rsxCompressedSize(std::uint32_t format, std::uint32_t width, std::uint32_t height) {
    return rsxCompressedRowBytes(format, width) * ((height + 3) / 4);
}

// Formats whose bytes the frame builder copies and the renderer samples.
constexpr bool rsxTextureFormatSampleable(std::uint32_t format) {
    const std::uint32_t base = format & kRsxTextureBaseMask;
    return base == kRsxTextureA8R8G8B8 || base == kRsxTextureB8 || rsxTextureBlockBytes(format) != 0;
}

// Formats that only a render target can supply: the renderer reads them from the GPU image of the target at
// the texture's address. The frame builder counts one that is not at a known target.
constexpr bool rsxTextureFormatRenderTargetOnly(std::uint32_t format) {
    return (format & kRsxTextureBaseMask) == kRsxTextureX32Float;
}

// SET_INDEX_ARRAY_DMA type field (RPCS3 `CELL_GCM_DRAW_INDEX_ARRAY_TYPE_*`).
inline constexpr std::uint32_t kRsxIndexTypeU32 = 0;
inline constexpr std::uint32_t kRsxIndexTypeU16 = 1;

inline constexpr std::uint32_t kRsxTextureUnitCount = 16;
inline constexpr std::uint32_t kRsxVertexSlotCount = 16;

// Name tables shared with the argument census; nullptr for a value outside the enumeration.
const char* rsxSurfaceColorName(std::uint32_t v);
const char* rsxSurfaceDepthName(std::uint32_t v);
const char* rsxVertexTypeName(std::uint32_t v);
const char* rsxPrimitiveName(std::uint32_t v);  // 0 is "END"
const char* rsxBlendFactorName(std::uint32_t v);
const char* rsxBlendEquationName(std::uint32_t v);

// Render targets. Size is the clip rectangle (SET_SURFACE_CLIP_*), not the pitch or the log2 size
// fields of SET_SURFACE_FORMAT; the pitch is the register value. Color target 0 (none) clears
// has_color, 1 selects surface A, 2 selects surface B; multiple render targets decode surface A and
// are counted.
RsxSurfaceState decodeSurfaceState(const RsxRegisters& regs);

// Fixed-function state. Registers keep their raw enum words (blend factors, compare functions, cull
// and front face, shade model) except alpha_ref, which is the 8-bit unorm reference of an A8R8G8B8
// surface (0..255), and the color mask, which keeps the register layout (B bit 0, G bit 8, R bit 16,
// A bit 24).
RsxRasterState decodeRasterState(const RsxRegisters& regs);

// True for the seven stencil op words other than KEEP. KEEP and every word outside the GCM set are false, which
// matches the renderer's KEEP fallback for unknown words.
bool rsxStencilOpWrites(std::uint32_t op_word);

// True when a draw can change the depth surface: the depth test and depth write are on, or the stencil can
// write (RPCS3 `RSXThread.cpp`, the zeta write decision). The stencil writes only
// with a Z24S8 surface, the test on, and a nonzero write mask together with at least one op that is not KEEP.
// With two-sided stencil on, the back mask and ops are ORed in separately (not per face) when the front mask
// is 0 or the front ops are all KEEP. The compare function is not consulted.
bool rsxZetaWriteEnabled(const RsxRasterState& raster, RsxDepthFormat depth_format);

// One entry per slot whose array is enabled (size != 0); data_offset stays 0 for the builder to fill.
std::vector<RsxVertexAttrib> decodeVertexAttribs(const RsxRegisters& regs);

// Four floats per slot from SET_VERTEX_DATA4F_M. The register file cannot tell "written with zeros"
// from "never written": an unbound, never-written slot is the renderer's call.
std::array<float, kRsxVertexSlotCount * 4> decodeDefaultAttribs(const RsxRegisters& regs);

// True when SET_TEXTURE_CONTROL0 enables the unit.
bool rsxTextureEnabled(const RsxRegisters& regs, std::uint32_t unit);

// Layout fields of a texture unit; the caller decides whether the layout is supported.
RsxTextureDesc decodeTexture(const RsxRegisters& regs, std::uint32_t unit);

// How a unit's texture is sampled. As in RPCS3 `get_extended_texture_dimension`, dimension 3 is a 3D
// texture whatever the cube bit says, and the cube bit makes a dimension-2 texture a cube map.
// Unsupported: 1D and any dimension value outside 1..3.
enum class RsxTextureKind : std::uint8_t { Tex2D, Tex3D, Cube, Unsupported };
constexpr RsxTextureKind rsxTextureKind(const RsxTextureDesc& texture) {
    switch (texture.dimension) {
        case 2: return texture.cubemap ? RsxTextureKind::Cube : RsxTextureKind::Tex2D;
        case 3: return RsxTextureKind::Tex3D;
        default: return RsxTextureKind::Unsupported;
    }
}

// Texture address word (SET_TEXTURE_ADDRESS) and filter word (SET_TEXTURE_FILTER) fields; enum values
// as in RPCS3 `gcm_enums.h` (WRAP = 1 ... MIRROR_ONCE_CLAMP = 8, NEAREST = 1 ... LINEAR_LINEAR = 6).
constexpr std::uint32_t rsxTextureWrapS(std::uint32_t address) { return address & 0xF; }
constexpr std::uint32_t rsxTextureWrapT(std::uint32_t address) { return (address >> 8) & 0xF; }
constexpr std::uint32_t rsxTextureMinFilter(std::uint32_t filter) { return (filter >> 16) & 7; }
constexpr std::uint32_t rsxTextureMagFilter(std::uint32_t filter) { return (filter >> 24) & 7; }

struct RsxIndexArray {
    std::uint32_t location = 0;  // 0 local, 1 main
    std::uint32_t offset = 0;
    std::uint8_t type = 0;  // kRsxIndexTypeU32 / kRsxIndexTypeU16
};
RsxIndexArray decodeIndexArray(const RsxRegisters& regs);

// DRAW_ARRAYS / DRAW_INDEX_ARRAY argument: first in the low 24 bits, count - 1 in the top 8.
struct RsxDrawRange {
    std::uint32_t first = 0, count = 0;
};
constexpr RsxDrawRange decodeDrawRange(std::uint32_t value) { return {value & 0xFFFFFF, (value >> 24) + 1}; }

// True when a restart index can be honoured: the primitive is a strip or fan and the index is the
// all-ones value of the index type (the only one Vulkan restarts at).
bool rsxPrimitiveRestartSupported(RsxPrimitive primitive, std::uint8_t index_type, std::uint32_t restart_index);

// SET_BEGIN_END argument. False for 0 (END) and for values outside 1..10 (the latter is counted).
bool decodePrimitive(std::uint32_t value, RsxPrimitive& out);

// CLEAR_SURFACE with the current state. color_argb and depth_stencil are the raw clear-value words:
// color is A8R8G8B8 (blue in the low byte); depth_stencil holds Z24 in bits 8..31 and stencil in bits
// 0..7 for Z24S8, Z16 in bits 0..15 for Z16. The scissor is {x, y, w, h} as written; the frame
// builder clips it to the surface and trims the mask.
RsxClear decodeClear(const RsxRegisters& regs, std::uint32_t mask);

// Group name of a written method that has render semantics the frame builder ignores (immediate
// vertex data other than 4F, element and inline arrays, blit triggers); nullptr otherwise.
const char* rsxIgnoredMethodName(std::uint32_t method);

}  // namespace tox2

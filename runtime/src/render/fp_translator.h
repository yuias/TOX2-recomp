#pragma once

// RSX fragment program -> GLSL 4.50 (Vulkan) translator. Pure text transformation, no Vulkan.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "render/vp_translator.h"

namespace tox2 {

// ---- Interface contract shared with the vertex translator and the renderer ----

// Descriptor set 0 bindings used by the fragment stage.
inline constexpr std::uint32_t kFpConstantsBinding = 1;          // UBO: vec4 fc[N], std140, N = inline constants
inline constexpr std::uint32_t kFpDrawBinding = kVpDrawBinding;  // same buffer as the vertex stage
inline constexpr std::uint32_t kFpTextureBindingBase = 3;        // sampler2D of unit n at base + n
inline constexpr std::uint32_t kFpTextureUnitCount = 16;
inline constexpr std::uint32_t kFpDrawBlockOffset = sizeof(VpDrawUniforms);  // 64

// Fragment-stage per-draw values, stored in binding 2 from byte kFpDrawBlockOffset (std140; the
// vertex stage declares only the first 64 bytes). The bound range must be at least
// kFpDrawBlockOffset + sizeof(FpDrawUniforms) bytes.
//   offset  0  uint  alpha_func   GCM compare enum & 7 (0 never, 1 less, 2 equal, 3 lequal,
//                                 4 greater, 5 notequal, 6 gequal, 7 always). Write 7 when
//                                 ALPHA_TEST_ENABLE (0x0304) is 0; the ALPHA_FUNC register (0x0308)
//                                 keeps its old value after the test is disabled, so do not copy it
//                                 blindly.
//   offset  4  float alpha_ref    reference value already decoded for the surface format
//                                 (unorm8: ref / 255)
//   offset  8  uint  flags        kFpFlag* bits
//   offset 12  uint  reserved
//   offset 16  vec4  texture_scale[16]  per texture unit: xy = 1 / width, 1 / height for
//                                 unnormalised textures, (1, 1) otherwise. The shader always
//                                 multiplies the sampled coordinate by it.
//
// kFpFlagTwoSided: set from the two-side lighting enable register (0x142C). The colour inputs then
// pick the back colours when !gl_FrontFacing, so the Vulkan front face must be set from
// SET_FRONT_FACE (0x1834) consistently with the vertex stage's coordinate convention; otherwise
// gl_FrontFacing disagrees with the RSX facing.
// kFpFlagRound8Bit: set for 8-bit render targets. The exported colour is rounded to 8 bits before
// the alpha compare and the output.
inline constexpr std::uint32_t kFpFlagTwoSided = 1u << 0;
inline constexpr std::uint32_t kFpFlagRound8Bit = 1u << 1;

struct FpDrawUniforms {
    std::uint32_t alpha_func;
    float alpha_ref;
    std::uint32_t flags;
    std::uint32_t reserved;
    float texture_scale[kFpTextureUnitCount][4];
};
static_assert(kFpDrawBlockOffset % 16 == 0);
static_assert(sizeof(FpDrawUniforms) == 16 + 16 * kFpTextureUnitCount);
static_assert(offsetof(FpDrawUniforms, alpha_ref) == 4);
static_assert(offsetof(FpDrawUniforms, flags) == 8);
static_assert(offsetof(FpDrawUniforms, texture_scale) == 16);

// SET_SHADER_CONTROL bits the translator reads.
inline constexpr std::uint32_t kFpControlDepthExport = 0xe;   // any bit: gl_FragDepth = clamp(R1.z, 0, 1)
inline constexpr std::uint32_t kFpControl32BitExport = 0x40;  // colour from R0 (set) or H0 (clear)

// Program words are stored with their 16-bit halves swapped. RsxDraw::fp_ucode and ::fp_constants
// keep that form; the translator undoes it for the instructions, and whoever uploads `fc[]` must
// apply this to every constant word before treating it as a float.
constexpr std::uint32_t fpFixupWord(std::uint32_t stored) { return std::rotl(stored, 16); }

// `ucode` is RsxDraw::fp_ucode as stored: 4 words per instruction, 4 more (zeroed, the values are in
// RsxDraw::fp_constants) after an instruction that has a constant operand, ending at the instruction
// with the END bit. `control` is RsxDraw::fp_control (SET_SHADER_CONTROL) and `texcoord_control` is
// RsxDraw::texcoord_control (bit n = texture coordinate n is 2D: z = 0, w = fragment w).
// GlslResult::texture_units is the mask of sampled units. TEX/TXP always use sampler2D; until a
// dimension parameter exists the caller must check each sampled unit's dimension and skip draws
// that sample a 3D or cube texture. Inputs use the vertex translator's varying
// locations. Inline constants read `fc[i]` in program order; the array covers the entries the
// program reads (at most RsxDraw::fp_constants.size() / 4), so bind at least that many.
GlslResult translateFragmentProgram(std::span<const std::uint32_t> ucode, std::uint32_t control,
                                    std::uint32_t texcoord_control);

}  // namespace tox2

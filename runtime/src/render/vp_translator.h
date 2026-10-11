#pragma once

// RSX vertex program -> GLSL 4.50 (Vulkan) translator. Pure text transformation, no Vulkan.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tox2 {

struct GlslResult {
    std::string glsl;                        // empty unless ok
    std::vector<std::string> unimplemented;  // e.g. "vp opcode ARL"; the draw is skipped and counted
    std::uint32_t texture_units = 0;         // mask of sampled units (fragment programs only)
    std::uint32_t inputs_used = 0;           // vertex programs: bit n = `layout(location = n) in vec4`
    bool ok = false;
};

// ---- Interface contract shared with the fragment translator and the renderer ----

// Descriptor set 0 bindings used by the vertex stage.
inline constexpr std::uint32_t kVpConstantsBinding = 0;  // UBO: vec4 c[kRsxVpConstantCount], std140
inline constexpr std::uint32_t kVpDrawBinding = 2;       // UBO: VpDrawUniforms, std140

// Per-draw values read by the vertex stage (std140; mirrored in the GLSL block `VpDraw`).
//   offset  0  vec4  viewport_scale   raw RSX viewport scale (x, y, z; w unused)
//   offset 16  vec4  viewport_offset  raw RSX viewport offset (x, y, z; w unused)
//   offset 32  vec4  clip_params      (surface clip width, clip height, depth clip min, depth clip max)
//   offset 48  uint  output_mask      SET_VERTEX_ATTRIB_OUTPUT_MASK register value
//   offset 52  float point_size       used when the program does not export a point size
//   offset 56  uint  branch_bits      SET_TRANSFORM_BRANCH_BITS register value (BRB and CLB read bit n)
//   offset 60  uint  user_clip         SET_USER_CLIP_PLANE_CONTROL register value (plane n in bits 4n..4n+3)
// Bytes 64.. of binding 2 are free for the fragment stage's per-draw values (the vertex shader only
// declares the first 64 bytes).
struct VpDrawUniforms {
    float viewport_scale[4];
    float viewport_offset[4];
    float clip_width;
    float clip_height;
    float clip_min;
    float clip_max;
    std::uint32_t output_mask;
    float point_size;
    std::uint32_t branch_bits;
    std::uint32_t user_clip;
};
static_assert(sizeof(VpDrawUniforms) == 64);
static_assert(offsetof(VpDrawUniforms, clip_width) == 32);
static_assert(offsetof(VpDrawUniforms, output_mask) == 48);
static_assert(offsetof(VpDrawUniforms, branch_bits) == 56);
static_assert(offsetof(VpDrawUniforms, user_clip) == 60);

// Vertex output slot (the `o[n]` destination of the program) -> varying `layout(location = N)`
// shared by the vertex outputs and the fragment inputs. -1 for slots without a varying.
//   slot 1 front diffuse -> 10    slot 2 front specular -> 12
//   slot 3 back diffuse  -> 11    slot 4 back specular  -> 13
//   slot 5 fog           -> 14 (fog value broadcast to all lanes)
//   slot 7..14 TEX0..TEX7 -> 0..7    slot 15 TEX8 -> 8
// TEX9 (location 9) is declared but always (0, 0, 0, 1). Slot 0 (position) and slot 6 (point size)
// are builtins.
inline constexpr std::uint32_t kVaryingFrontDiffuse = 10;
inline constexpr std::uint32_t kVaryingBackDiffuse = 11;
inline constexpr std::uint32_t kVaryingFrontSpecular = 12;
inline constexpr std::uint32_t kVaryingBackSpecular = 13;
inline constexpr std::uint32_t kVaryingFog = 14;
inline constexpr std::uint32_t kVaryingTexCoordCount = 10;  // locations 0..9

constexpr int vpOutputSlotLocation(std::uint32_t slot) {
    switch (slot) {
        case 1: return kVaryingFrontDiffuse;
        case 2: return kVaryingFrontSpecular;
        case 3: return kVaryingBackDiffuse;
        case 4: return kVaryingBackSpecular;
        case 5: return kVaryingFog;
        case 15: return 8;
        default: return slot >= 7 && slot <= 14 ? static_cast<int>(slot - 7) : -1;
    }
}

struct VpTranslateOptions {
    // The device has shaderClipDistance: the program writes gl_ClipDistance[6]. The flag is constant for a
    // device, so programs are still keyed by the ucode hash alone.
    bool clip_distances = false;
};

// `ucode` holds the program words in host order (RsxDraw::vp_ucode), starting at the first
// instruction (4 words each) and ending at the last instruction the program can reach: the one with
// the END bit, or a later one when a branch or call leads past it. Branch and call targets are
// instruction indices into `ucode`, and instructions no path reaches are ignored. `input_mask` is accepted
// for the interface but not used: every attribute the program reads becomes a vertex input at
// location = attribute slot, and the renderer binds default values for unbound slots.
//
// User clip planes (options.clip_distances, and only when the program writes slot 5 or 6): plane n takes
// its distance from o[5].y/.z/.w (n = 0..2) or o[6].y/.z/.w (n = 3..5), as RPCS3 does. It is live when
// the control nibble (draw.user_clip >> 4n) & 0xF is 1 (less than: distance -v) or 2 (greater or equal:
// distance v), draw.output_mask bit 6+n is set and the program writes the plane's slot; any other plane
// gets the distance 0.5 so that it never clips. RPCS3 leaves such distances unassigned.
GlslResult translateVertexProgram(std::span<const std::uint32_t> ucode, std::uint32_t input_mask,
                                  const VpTranslateOptions& options = {});

}  // namespace tox2

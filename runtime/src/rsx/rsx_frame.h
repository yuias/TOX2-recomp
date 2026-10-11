#pragma once

// Decoded RSX state and the commands the frame builder hands to the renderer. Plain data only: the
// renderer never sees guest memory, so every guest byte range a command needs is copied into it.
// Bit layouts of the source registers follow RPCS3 `Emu/RSX/rsx_decode.h` and `gcm_enums.h`.

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace tox2 {

enum class RsxColorFormat : std::uint8_t { A8R8G8B8, Unsupported };
enum class RsxDepthFormat : std::uint8_t { None, Z24S8, Z16, Unsupported };
// RSX BEGIN_END values 1..10.
enum class RsxPrimitive : std::uint8_t {
    Points = 1,
    Lines,
    LineLoop,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
    Quads,
    QuadStrip,
    Polygon
};

struct RsxSurface {  // location 0 = local, 1 = main
    std::uint32_t location = 0, offset = 0, pitch = 0, width = 0, height = 0;
    RsxColorFormat format = RsxColorFormat::Unsupported;
};

struct RsxSurfaceState {
    RsxSurface color;
    bool has_color = false;
    RsxSurface depth;
    RsxDepthFormat depth_format = RsxDepthFormat::None;
    std::uint32_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
};

struct RsxVertexAttrib {
    std::uint8_t slot = 0, type = 0, size = 0;
    std::uint16_t stride = 0;
    std::uint32_t location = 0, offset = 0;
    std::uint32_t data_offset = 0;  // into RsxDraw::vertex_data; meaningful only when has_data
    bool has_data = false;          // false for a type the builder cannot copy (no bytes in vertex_data)
};

struct RsxTextureDesc {
    std::uint8_t unit = 0, format = 0 /* raw 8-bit */, mips = 0, dimension = 0;
    bool linear_layout = false, normalized = true, cubemap = false;
    // Slices of a 3D texture (dimension 3); 1 for any other dimension. A zero in the register counts as 1.
    std::uint16_t depth = 1;
    // control0/remap/filter/address/border are the raw SET_TEXTURE_CONTROL0/CONTROL1/FILTER/ADDRESS/BORDER_COLOR
    // words. For a texture with bytes, pitch is the row size of level 0 in the copied (tightly packed) data.
    std::uint32_t location = 0, offset = 0, pitch = 0, width = 0, height = 0, control0 = 0, remap = 0, filter = 0,
                  address = 0, border = 0;
    std::int32_t rt_alias = -1;  // index into the builder's known surfaces, -1 = bytes in texture_data
    // The copy in RsxDraw::texture_data holds every mip level one after the other (for a cube map, the six
    // faces one after the other), each level in tightly packed rows and slices, de-swizzled (rsxTextureLayout
    // with packed offsets); data_size 0 means no bytes were copied.
    std::uint32_t data_offset = 0, data_size = 0;
};

struct RsxRasterState {
    bool blend = false, alpha_test = false, depth_test = false, depth_write = false, cull = false;
    std::uint32_t blend_sfactor_rgb = 0, blend_dfactor_rgb = 0, blend_sfactor_a = 0, blend_dfactor_a = 0,
                  blend_eq_rgb = 0, blend_eq_a = 0, blend_color = 0, alpha_func = 0, alpha_ref = 0, depth_func = 0,
                  cull_face = 0, front_face = 0, color_mask = 0, shade_model = 0;
    float viewport_scale[4] = {}, viewport_offset[4] = {};
    float clip_min = 0.0f, clip_max = 1.0f;  // SET_CLIP_MIN / SET_CLIP_MAX: the depth range of the viewport
    float point_size = 1.0f;                 // SET_POINT_SIZE
    bool two_sided_lighting = false;         // SET_TWO_SIDE_LIGHT_EN
    std::uint32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0, window_origin = 0, pixel_center = 0;
};

struct RsxClear {
    RsxSurfaceState surfaces;
    std::uint32_t mask = 0, color_argb = 0, depth_stencil = 0;
    std::uint32_t scissor[4] = {};
};

struct RsxDraw {
    RsxSurfaceState surfaces;
    RsxRasterState raster;
    RsxPrimitive primitive = RsxPrimitive::Triangles;
    // Contiguous DRAW_* ranges are merged; otherwise count `state: multiple ranges` and keep the first.
    // Indexed: index_data already starts at `first` (draw it with firstIndex 0, count indices), and the
    // index values are raw: element 0 of each vertex slot is vertex `min_index`, so the renderer uses
    // vertexOffset = -min_index. Array draws: element 0 is vertex `first` (= min_index), same offset.
    bool indexed = false;
    std::uint8_t index_type = 0;
    std::uint32_t first = 0, count = 0, min_index = 0;
    // Largest vertex index of the draw (first + count - 1 for array draws). For indexed draws the smallest
    // and largest value exclude the restart index; each vertex array holds max_index - min_index + 1
    // elements. restart_used: the index data contains restart_index, with SET_RESTART_INDEX_ENABLE on.
    std::uint32_t max_index = 0;
    bool restart_used = false;
    std::uint32_t restart_index = 0;
    std::vector<RsxVertexAttrib> attribs;
    std::vector<std::uint8_t> vertex_data;  // guest bytes, BE
    std::vector<std::uint8_t> index_data;   // guest bytes, BE
    // SET_VERTEX_DATA*_M values for unbound slots; default_written bit n says slot n was written. A slot
    // that was not reads as (0, 0, 0, 1) whatever default_attribs holds.
    std::array<float, 16 * 4> default_attribs{};
    std::uint32_t default_written = 0;
    std::vector<RsxTextureDesc> textures;
    std::vector<std::uint8_t> texture_data;
    std::uint64_t vp_hash = 0;
    std::vector<std::uint32_t> vp_ucode;
    std::uint32_t vp_input_mask = 0, vp_output_mask = 0;
    std::uint32_t vp_branch_bits = 0;         // SET_TRANSFORM_BRANCH_BITS: the boolean constants of BRB and CLB
    std::vector<std::uint32_t> vp_constants;  // full constant snapshot, kRsxVpConstantCount * 4 words
    std::uint64_t fp_hash = 0;                // hash of the ucode with inline constants zeroed
    std::vector<std::uint32_t> fp_ucode;      // decoded words
    std::vector<std::uint32_t>
            fp_constants;  // inline constants in program order; halves still swapped, apply fpFixupWord before use
    std::uint32_t fp_control = 0, texcoord_control = 0;  // SHADER_CONTROL, texcoord 2D/3D/centroid masks
};

struct RsxFlip {
    std::uint32_t buffer = 0, offset = 0, pitch = 0, width = 0, height = 0;
    std::uint64_t flip_index = 0;
};

using RsxCommand = std::variant<RsxClear, RsxDraw, RsxFlip>;

}  // namespace tox2

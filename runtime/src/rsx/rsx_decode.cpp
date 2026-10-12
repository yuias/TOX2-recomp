#include "rsx/rsx_decode.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>
#include <string>

#include "rsx/fifo_parser.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

// RPCS3 `gcm_enums.h` values used for validation.
constexpr std::uint32_t kCompareNever = 0x200;
constexpr std::uint32_t kCompareAlways = 0x207;
constexpr std::uint32_t kCullFront = 0x404;
constexpr std::uint32_t kCullBack = 0x405;
constexpr std::uint32_t kCullFrontAndBack = 0x408;
constexpr std::uint32_t kFrontFaceCw = 0x900;
constexpr std::uint32_t kFrontFaceCcw = 0x901;
constexpr std::uint32_t kShadeModelSmooth = 0x1D01;          // CELL_GCM_SMOOTH; CELL_GCM_FLAT is 0x1D00
constexpr std::uint32_t kBlendEquationSignedFirst = 0xF005;  // REVERSE_SUBTRACT_SIGNED .. REVERSE_ADD_SIGNED
constexpr std::uint32_t kBlendEquationSignedLast = 0xF007;

// SET_SURFACE_COLOR_TARGET values.
constexpr std::uint32_t kTargetNone = 0;
constexpr std::uint32_t kTargetA = 1;
constexpr std::uint32_t kTargetB = 2;
constexpr std::uint32_t kTargetMrt1 = 0x13;
constexpr std::uint32_t kTargetMrt2 = 0x17;
constexpr std::uint32_t kTargetMrt3 = 0x1F;

// SET_SURFACE_FORMAT raster type.
constexpr std::uint32_t kSurfaceTypeLinear = 1;

float asFloat(std::uint32_t bits) { return std::bit_cast<float>(bits); }

void noteState(const std::string& key) { rsxNoteUnimplemented("state", key); }

std::uint32_t surfaceLocation(std::uint32_t dma, const char* what) {
    if (dma == kRsxContextDmaMain) return 1;
    if (dma != kRsxContextDmaLocal) noteState(std::format("{} dma {:#x}", what, dma));
    return 0;
}

// RPCS3 `gcm_enums.h` `comparison_function` conversion: the word is ORed with 0x200 and must then lie in
// 0x200..0x207, so 0x000..0x007 are accepted too.
bool validCompare(std::uint32_t v) {
    const std::uint32_t word = v | kCompareNever;
    return word >= kCompareNever && word <= kCompareAlways;
}

constexpr std::uint32_t kStencilOpKeep = 0x1E00;

// The eight GCM stencil op words, compared as full 32-bit words (RPCS3 `gcm_enums.h` `stencil_op`).
bool validStencilOp(std::uint32_t v) {
    switch (v) {
        case 0x1E00:  // KEEP
        case 0x0000:  // ZERO
        case 0x1E01:  // REPLACE
        case 0x1E02:  // INCR
        case 0x1E03:  // DECR
        case 0x150A:  // INVERT
        case 0x8507:  // INCR_WRAP
        case 0x8508:  // DECR_WRAP
            return true;
        default: return false;
    }
}

// RPCS3 aborts on an unknown compare or op word; here it is counted and the renderer falls back to
// `func & 7` and KEEP.
void noteStencilFace(const RsxStencilFace& face) {
    if (!validCompare(face.func)) noteState(std::format("stencil func ?{:#x}", face.func));
    for (const std::uint32_t op : {face.op_fail, face.op_zfail, face.op_zpass}) {
        if (!validStencilOp(op)) noteState(std::format("stencil op ?{:#x}", op));
    }
}

RsxStencilFace decodeStencilFace(const RsxRegisters& regs, std::uint32_t mask_method, std::uint32_t func_method,
                                 std::uint32_t ref_method, std::uint32_t func_mask_method,
                                 std::uint32_t op_fail_method) {
    RsxStencilFace face;
    face.func = regs.reg(func_method);
    face.op_fail = regs.reg(op_fail_method);
    face.op_zfail = regs.reg(op_fail_method + 4);
    face.op_zpass = regs.reg(op_fail_method + 8);
    face.ref = static_cast<std::uint8_t>(regs.reg(ref_method) & 0xFF);
    face.func_mask = static_cast<std::uint8_t>(regs.reg(func_mask_method) & 0xFF);
    face.write_mask = static_cast<std::uint8_t>(regs.reg(mask_method) & 0xFF);
    return face;
}

struct MethodRange {
    std::uint32_t first;
    std::uint32_t count;  // registers
    const char* name;
};

// Methods the frame builder does not turn into commands: the integer immediate vertex data forms (the
// float forms are decoded into the default attribute registers by RsxRegisters::apply), element and
// inline arrays, and the data-moving trigger of the NV3089 and NV0039 blit engines. Counts are registers:
// 16 attributes times the words per attribute (RPCS3 `bind_range` of the `NV4097_SET_VERTEX_DATA*_M`
// methods). NV308A_COLOR is not listed: the consumer writes it into guest memory and counts only the
// formats it cannot copy.
constexpr MethodRange kIgnoredMethods[] = {
        {kMethodNv4097SetVertexDataScaled4sM, 32, "NV4097_SET_VERTEX_DATA_SCALED4S_M"},
        {kMethodNv4097ArrayElement16, 1, "NV4097_ARRAY_ELEMENT16"},
        {kMethodNv4097ArrayElement32, 1, "NV4097_ARRAY_ELEMENT32"},
        {kMethodNv4097InlineArray, 1, "NV4097_INLINE_ARRAY"},
        {kMethodNv4097SetVertexData2sM, 16, "NV4097_SET_VERTEX_DATA2S_M"},
        {kMethodNv4097SetVertexData4ubM, 16, "NV4097_SET_VERTEX_DATA4UB_M"},
        {kMethodNv4097SetVertexData4sM, 32, "NV4097_SET_VERTEX_DATA4S_M"},
        {kMethodNv0039BufferNotify, 1, "NV0039_BUFFER_NOTIFY"},
        {kMethodNv3089ImageIn, 1, "NV3089_IMAGE_IN"},
};

}  // namespace

// Names as in RPCS3 `gcm_enums.h` without the CELL_GCM_ prefix.
const char* rsxPrimitiveName(std::uint32_t v) {
    static constexpr const char* kNamesByValue[] = {"END",        "POINTS",     "LINES",          "LINE_LOOP",
                                                    "LINE_STRIP", "TRIANGLES",  "TRIANGLE_STRIP", "TRIANGLE_FAN",
                                                    "QUADS",      "QUAD_STRIP", "POLYGON"};
    return v < std::size(kNamesByValue) ? kNamesByValue[v] : nullptr;
}

const char* rsxSurfaceColorName(std::uint32_t v) {
    switch (v) {
        case 1: return "X1R5G5B5_Z1R5G5B5";
        case 2: return "X1R5G5B5_O1R5G5B5";
        case 3: return "R5G6B5";
        case 4: return "X8R8G8B8_Z8R8G8B8";
        case 5: return "X8R8G8B8_O8R8G8B8";
        case 8: return "A8R8G8B8";
        case 9: return "B8";
        case 10: return "G8B8";
        case 11: return "F_W16Z16Y16X16";
        case 12: return "F_W32Z32Y32X32";
        case 13: return "F_X32";
        case 14: return "X8B8G8R8_Z8B8G8R8";
        case 15: return "X8B8G8R8_O8B8G8R8";
        case 16: return "A8B8G8R8";
        default: return nullptr;
    }
}

const char* rsxSurfaceDepthName(std::uint32_t v) {
    switch (v) {
        case 0: return "none";
        case 1: return "Z16";
        case 2: return "Z24S8";
        default: return nullptr;
    }
}

const char* rsxVertexTypeName(std::uint32_t v) {
    switch (v) {
        case 0: return "ub256(0)";
        case 1: return "s1";
        case 2: return "f";
        case 3: return "sf";
        case 4: return "ub";
        case 5: return "s32k";
        case 6: return "cmp";
        case 7: return "ub256";
        default: return nullptr;
    }
}

const char* rsxBlendFactorName(std::uint32_t v) {
    switch (v) {
        case 0: return "ZERO";
        case 1: return "ONE";
        case 0x300: return "SRC_COLOR";
        case 0x301: return "ONE_MINUS_SRC_COLOR";
        case 0x302: return "SRC_ALPHA";
        case 0x303: return "ONE_MINUS_SRC_ALPHA";
        case 0x304: return "DST_ALPHA";
        case 0x305: return "ONE_MINUS_DST_ALPHA";
        case 0x306: return "DST_COLOR";
        case 0x307: return "ONE_MINUS_DST_COLOR";
        case 0x308: return "SRC_ALPHA_SATURATE";
        case 0x8001: return "CONSTANT_COLOR";
        case 0x8002: return "ONE_MINUS_CONSTANT_COLOR";
        case 0x8003: return "CONSTANT_ALPHA";
        case 0x8004: return "ONE_MINUS_CONSTANT_ALPHA";
        default: return nullptr;
    }
}

const char* rsxBlendEquationName(std::uint32_t v) {
    switch (v) {
        case 0x8006: return "ADD";
        case 0x8007: return "MIN";
        case 0x8008: return "MAX";
        case 0x800A: return "SUBTRACT";
        case 0x800B: return "REVERSE_SUBTRACT";
        case 0xF005: return "REVERSE_SUBTRACT_SIGNED";
        case 0xF006: return "ADD_SIGNED";
        case 0xF007: return "REVERSE_ADD_SIGNED";
        default: return nullptr;
    }
}

// RPCS3 `rsx_decode.h` registers_decoder<NV4097_SET_SURFACE_FORMAT>, <..._CLIP_HORIZONTAL/VERTICAL>,
// <..._PITCH_*>, <..._COLOR_*OFFSET>, <..._ZETA_OFFSET>, <NV4097_SET_CONTEXT_DMA_COLOR_*/ZETA>;
// size = clip rectangle per `thread::get_framebuffer_layout`.
RsxSurfaceState decodeSurfaceState(const RsxRegisters& regs) {
    RsxSurfaceState s;
    const std::uint32_t clip_h = regs.reg(kMethodNv4097SetSurfaceClipHorizontal);
    const std::uint32_t clip_v = regs.reg(kMethodNv4097SetSurfaceClipVertical);
    s.clip_x = clip_h & 0xFFFF;
    s.clip_w = clip_h >> 16;
    s.clip_y = clip_v & 0xFFFF;
    s.clip_h = clip_v >> 16;

    const std::uint32_t format = regs.reg(kMethodNv4097SetSurfaceFormat);
    const std::uint32_t color_code = format & 0x1F;
    const std::uint32_t depth_code = (format >> 5) & 7;
    const std::uint32_t type = (format >> 8) & 0xF;
    const std::uint32_t antialias = (format >> 12) & 0xF;
    if (type != kSurfaceTypeLinear) {
        noteState(type == 2 ? std::string("swizzled surface") : std::format("surface type {:#x}", type));
    }
    s.aa_mode = static_cast<std::uint8_t>(antialias);
    // Mode 3 stores two samples side by side, modes 4 and 5 a 2x2 block per pixel (the renderer draws that layout).
    // Other modes keep one sample per pixel and are counted.
    if (antialias != 0 && antialias != 3 && antialias != 4 && antialias != 5) {
        noteState(std::format("antialiasing {:#x}", antialias));
    }
    const std::uint8_t samples_x = antialias == 3 || antialias == 4 || antialias == 5 ? 2 : 1;
    const std::uint8_t samples_y = antialias == 4 || antialias == 5 ? 2 : 1;

    std::uint32_t color_offset_method = kMethodNv4097SetSurfaceColorAOffset;
    std::uint32_t color_pitch_method = kMethodNv4097SetSurfacePitchA;
    std::uint32_t color_dma_method = kMethodNv4097SetContextDmaColorA;
    const std::uint32_t target = regs.reg(kMethodNv4097SetSurfaceColorTarget);
    switch (target) {
        case kTargetNone: break;
        case kTargetA: s.has_color = true; break;
        case kTargetB:
            s.has_color = true;
            color_offset_method = kMethodNv4097SetSurfaceColorBOffset;
            color_pitch_method = kMethodNv4097SetSurfacePitchB;
            color_dma_method = kMethodNv4097SetContextDmaColorB;
            break;
        case kTargetMrt1:
        case kTargetMrt2:
        case kTargetMrt3:
            noteState(std::format("multiple render targets {:#x}", target));
            s.has_color = true;
            break;
        default: noteState(std::format("surface target {:#x}", target)); break;
    }

    if (s.has_color) {
        s.color.location = surfaceLocation(regs.reg(color_dma_method), "color surface");
        s.color.offset = regs.reg(color_offset_method);
        s.color.pitch = regs.reg(color_pitch_method);
        s.color.width = s.clip_w;
        s.color.height = s.clip_h;
        s.color.samples_x = samples_x;
        s.color.samples_y = samples_y;
        if (color_code == kRsxSurfaceColorA8R8G8B8) {
            s.color.format = RsxColorFormat::A8R8G8B8;
        } else if (color_code == kRsxSurfaceColorFX32) {
            s.color.format = RsxColorFormat::F_X32;
        } else {
            s.color.format = RsxColorFormat::Unsupported;
            const char* name = rsxSurfaceColorName(color_code);
            rsxNoteUnimplemented("surface format",
                                 name != nullptr ? std::string(name) : std::format("?{:#x}", color_code));
        }
    }

    switch (depth_code) {
        case 0: s.depth_format = RsxDepthFormat::None; break;
        case kRsxSurfaceDepthZ16: s.depth_format = RsxDepthFormat::Z16; break;
        case kRsxSurfaceDepthZ24S8: s.depth_format = RsxDepthFormat::Z24S8; break;
        default:
            s.depth_format = RsxDepthFormat::Unsupported;
            rsxNoteUnimplemented("depth format", std::format("?{:#x}", depth_code));
            break;
    }
    if (s.depth_format != RsxDepthFormat::None) {
        s.depth.location = surfaceLocation(regs.reg(kMethodNv4097SetContextDmaZeta), "depth surface");
        s.depth.offset = regs.reg(kMethodNv4097SetSurfaceZetaOffset);
        s.depth.pitch = regs.reg(kMethodNv4097SetSurfacePitchZ);
        s.depth.width = s.clip_w;
        s.depth.height = s.clip_h;
        s.depth.samples_x = samples_x;
        s.depth.samples_y = samples_y;
        s.depth.depth_format = s.depth_format;
    }
    return s;
}

// RPCS3 `rsx_decode.h` registers_decoder for SET_BLEND_*, SET_COLOR_MASK, SET_ALPHA_*, SET_DEPTH_*,
// SET_CULL_FACE*, SET_FRONT_FACE, SET_SHADE_MODE, SET_VIEWPORT_*, SET_SCISSOR_*, SET_SHADER_WINDOW,
// SET_CLIP_MIN / SET_CLIP_MAX (float bit patterns), SET_POINT_SIZE and SET_ANTI_ALIASING_CONTROL.
RsxRasterState decodeRasterState(const RsxRegisters& regs) {
    RsxRasterState r;
    r.blend = regs.reg(kMethodNv4097SetBlendEnable) != 0;
    r.alpha_test = regs.reg(kMethodNv4097SetAlphaTestEnable) != 0;
    r.depth_test = regs.reg(kMethodNv4097SetDepthTestEnable) != 0;
    r.depth_write = regs.reg(kMethodNv4097SetDepthMask) != 0;
    r.cull = regs.reg(kMethodNv4097SetCullFaceEnable) != 0;

    const std::uint32_t sfactor = regs.reg(kMethodNv4097SetBlendFuncSfactor);
    const std::uint32_t dfactor = regs.reg(kMethodNv4097SetBlendFuncDfactor);
    const std::uint32_t equation = regs.reg(kMethodNv4097SetBlendEquation);
    r.blend_sfactor_rgb = sfactor & 0xFFFF;
    r.blend_sfactor_a = sfactor >> 16;
    r.blend_dfactor_rgb = dfactor & 0xFFFF;
    r.blend_dfactor_a = dfactor >> 16;
    r.blend_eq_rgb = equation & 0xFFFF;
    r.blend_eq_a = equation >> 16;
    r.blend_color = regs.reg(kMethodNv4097SetBlendColor);
    r.alpha_func = regs.reg(kMethodNv4097SetAlphaFunc);
    r.alpha_ref = regs.reg(kMethodNv4097SetAlphaRef) & 0xFF;
    r.depth_func = regs.reg(kMethodNv4097SetDepthFunc);
    r.cull_face = regs.reg(kMethodNv4097SetCullFace);
    r.front_face = regs.reg(kMethodNv4097SetFrontFace);
    r.color_mask = regs.reg(kMethodNv4097SetColorMask);
    r.shade_model = regs.reg(kMethodNv4097SetShadeMode);
    r.clip_min = asFloat(regs.reg(kMethodNv4097SetClipMin));
    r.clip_max = asFloat(regs.reg(kMethodNv4097SetClipMax));
    r.point_size = asFloat(regs.reg(kMethodNv4097SetPointSize));
    r.two_sided_lighting = regs.reg(kMethodNv4097SetTwoSideLightEn) != 0;

    const std::uint32_t aa_control = regs.reg(kMethodNv4097SetAntiAliasingControl);
    r.msaa = (aa_control & 0x1) != 0;
    r.alpha_to_coverage = (aa_control & 0x10) != 0;
    r.alpha_to_one = (aa_control & 0x100) != 0;
    r.sample_mask = static_cast<std::uint16_t>(aa_control >> 16);

    for (std::uint32_t i = 0; i < 4; i++) {
        r.viewport_scale[i] = asFloat(regs.reg(kMethodNv4097SetViewportScale + i * 4));
        r.viewport_offset[i] = asFloat(regs.reg(kMethodNv4097SetViewportOffset + i * 4));
    }

    const std::uint32_t scissor_h = regs.reg(kMethodNv4097SetScissorHorizontal);
    const std::uint32_t scissor_v = regs.reg(kMethodNv4097SetScissorVertical);
    r.scissor_x = scissor_h & 0xFFFF;
    r.scissor_w = scissor_h >> 16;
    r.scissor_y = scissor_v & 0xFFFF;
    r.scissor_h = scissor_v >> 16;

    const std::uint32_t shader_window = regs.reg(kMethodNv4097SetShaderWindow);
    r.window_origin = (shader_window >> 12) & 0xF;
    r.pixel_center = (shader_window >> 16) & 0xF;

    if (r.blend) {
        for (const std::uint32_t factor :
             {r.blend_sfactor_rgb, r.blend_sfactor_a, r.blend_dfactor_rgb, r.blend_dfactor_a}) {
            if (rsxBlendFactorName(factor) == nullptr)
                rsxNoteUnimplemented("blend", std::format("factor ?{:#x}", factor));
        }
        for (const std::uint32_t eq : {r.blend_eq_rgb, r.blend_eq_a}) {
            const char* name = rsxBlendEquationName(eq);
            if (name == nullptr) {
                rsxNoteUnimplemented("blend", std::format("equation ?{:#x}", eq));
            } else if (eq >= kBlendEquationSignedFirst && eq <= kBlendEquationSignedLast) {
                rsxNoteUnimplemented("blend", std::format("equation {}", name));
            }
        }
    }
    if (r.alpha_test && !validCompare(r.alpha_func)) noteState(std::format("alpha func ?{:#x}", r.alpha_func));
    if (r.depth_test && !validCompare(r.depth_func)) noteState(std::format("depth func ?{:#x}", r.depth_func));
    if (r.cull && r.cull_face != kCullFront && r.cull_face != kCullBack && r.cull_face != kCullFrontAndBack) {
        noteState(std::format("cull face ?{:#x}", r.cull_face));
    }
    if (r.front_face != kFrontFaceCw && r.front_face != kFrontFaceCcw)
        noteState(std::format("front face ?{:#x}", r.front_face));
    r.stencil_test = regs.reg(kMethodNv4097SetStencilTestEnable) != 0;
    r.two_sided_stencil = regs.reg(kMethodNv4097SetTwoSidedStencilTestEnable) != 0;
    r.stencil_front = decodeStencilFace(regs, kMethodNv4097SetStencilMask, kMethodNv4097SetStencilFunc,
                                        kMethodNv4097SetStencilFuncRef, kMethodNv4097SetStencilFuncMask,
                                        kMethodNv4097SetStencilOpFail);
    // With two-sided stencil off the back face uses the front registers (RPCS3 `VKGSRender.cpp`).
    r.stencil_back = r.two_sided_stencil
                             ? decodeStencilFace(regs, kMethodNv4097SetBackStencilMask, kMethodNv4097SetBackStencilFunc,
                                                 kMethodNv4097SetBackStencilFuncRef,
                                                 kMethodNv4097SetBackStencilFuncMask, kMethodNv4097SetBackStencilOpFail)
                             : r.stencil_front;
    if (r.stencil_test) {
        noteStencilFace(r.stencil_front);
        if (r.two_sided_stencil) noteStencilFace(r.stencil_back);
    }
    // Plane n sits in bits 4n..4n+3. RPCS3 aborts on a nibble above 2 (`gcm_enums.h`); counting it and
    // treating the plane as off is our fallback.
    r.user_clip = regs.reg(kMethodNv4097SetUserClipPlaneControl);
    for (std::uint32_t plane = 0; plane < 6; plane++) {
        const std::uint32_t op = (r.user_clip >> (4 * plane)) & 0xF;
        if (op > 2) noteState(std::format("user clip plane op ?{:#x}", op));
    }
    // Enabled by the game but not modelled by the renderer.
    if (regs.reg(kMethodNv4097SetLogicOpEnable) != 0) noteState("logic op");
    if (regs.reg(kMethodNv4097SetPolyOffsetFillEnable) != 0) noteState("polygon offset");
    if (regs.reg(kMethodNv4097SetDepthBoundsTestEnable) != 0) noteState("depth bounds test");
    // SET_ZMIN_MAX_CONTROL (`rsx_decode.h`): clip enable in bits 0..3, clamp in bits 4..7. The device has no
    // depth clamp, so a primitive is always clipped to the depth range.
    const std::uint32_t zmin_max_control = regs.reg(kMethodNv4097SetZminMaxControl);
    if ((zmin_max_control & 0xF) == 0) noteState("depth clip disabled");
    if ((zmin_max_control & 0xF0) != 0) noteState("depth clamp");
    if (r.shade_model != kShadeModelSmooth) noteState("flat shading");
    return r;
}

bool rsxStencilOpWrites(std::uint32_t op_word) { return op_word != kStencilOpKeep && validStencilOp(op_word); }

bool rsxZetaWriteEnabled(const RsxRasterState& raster, RsxDepthFormat depth_format) {
    if (raster.depth_test && raster.depth_write) return true;
    if (!raster.stencil_test || depth_format != RsxDepthFormat::Z24S8) return false;
    const auto faceActive = [](const RsxStencilFace& face) {
        return rsxStencilOpWrites(face.op_fail) || rsxStencilOpWrites(face.op_zfail) ||
               rsxStencilOpWrites(face.op_zpass);
    };
    std::uint32_t mask = raster.stencil_front.write_mask;
    bool active = faceActive(raster.stencil_front);
    // RPCS3 folds the back face in only when the front one cannot write, and folds its mask and ops in
    // separately, so a front mask with all-KEEP ops plus a back op with mask 0 counts as a write.
    if ((mask == 0 || !active) && raster.two_sided_stencil) {
        mask |= raster.stencil_back.write_mask;
        active = active || faceActive(raster.stencil_back);
    }
    return mask != 0 && active;
}

// RPCS3 `rsx_decode.h` vertex_array_helper (type bits 0-2, size 4-7, stride 8-15, frequency 16-31) and
// vertex_array_offset_helper; the location is bit 31 of the offset word
// (`draw_command_processor::analyse_inputs_interleaved`, `Core/RSXDrawCommands.cpp`).
std::vector<RsxVertexAttrib> decodeVertexAttribs(const RsxRegisters& regs) {
    std::vector<RsxVertexAttrib> out;
    for (std::uint32_t slot = 0; slot < kRsxVertexSlotCount; slot++) {
        const std::uint32_t format = regs.reg(kMethodNv4097SetVertexDataArrayFormat + slot * 4);
        const std::uint32_t size = (format >> 4) & 0xF;
        if (size == 0) continue;  // array disabled

        RsxVertexAttrib attrib;
        attrib.slot = static_cast<std::uint8_t>(slot);
        attrib.type = static_cast<std::uint8_t>(format & 7);
        attrib.size = static_cast<std::uint8_t>(size);
        attrib.stride = static_cast<std::uint16_t>((format >> 8) & 0xFF);
        const std::uint32_t offset = regs.reg(kMethodNv4097SetVertexDataArrayOffset + slot * 4);
        attrib.location = offset >> 31;
        attrib.offset = offset & 0x7FFFFFFF;
        if (attrib.type == 0 || attrib.type == 6) {
            rsxNoteUnimplemented("vertex type", rsxVertexTypeName(attrib.type));
        }
        if ((format >> 16) != 0) noteState("vertex frequency");
        out.push_back(attrib);
    }
    return out;
}

// RPCS3 `NV47/HW/nv4097.h` set_vertex_data_impl: the 4F method puts word i of attribute n in
// register_vertex_info[n].data[i], i.e. register NV4097_SET_VERTEX_DATA4F_M + n * 4 + i.
std::array<float, kRsxVertexSlotCount * 4> decodeDefaultAttribs(const RsxRegisters& regs) {
    std::array<float, kRsxVertexSlotCount * 4> out{};
    for (std::uint32_t i = 0; i < out.size(); i++) out[i] = asFloat(regs.reg(kMethodNv4097SetVertexData4fM + i * 4));
    return out;
}

bool rsxTextureEnabled(const RsxRegisters& regs, std::uint32_t unit) {
    return (regs.reg(kMethodNv4097SetTextureControl0 + unit * 0x20) >> 31) != 0;
}

// RPCS3 `RSXTexture.cpp` fragment_texture accessors (pitch is the 20-bit `fragment_texture::pitch`) and
// `rsx_decode.h` texture_format_helper, texture_image_rect_helper. `_UN` = unnormalized coordinates
// (`CELL_GCM_TEXTURE_UN`, scaled by 1/width and 1/height in `texture_cache.h` `upload_texture`).
RsxTextureDesc decodeTexture(const RsxRegisters& regs, std::uint32_t unit) {
    const std::uint32_t base = unit * 0x20;
    const std::uint32_t format_word = regs.reg(kMethodNv4097SetTextureFormat + base);
    const std::uint32_t format = (format_word >> 8) & 0xFF;

    RsxTextureDesc t;
    t.unit = static_cast<std::uint8_t>(unit);
    t.format = static_cast<std::uint8_t>(format);
    t.mips = static_cast<std::uint8_t>(std::min<std::uint32_t>(format_word >> 16, 255));
    t.dimension = static_cast<std::uint8_t>((format_word >> 4) & 0xF);
    t.cubemap = ((format_word >> 2) & 1) != 0;
    t.linear_layout = (format & kRsxTextureLinearFlag) != 0;
    t.normalized = (format & kRsxTextureUnnormalizedFlag) == 0;

    const std::uint32_t location = format_word & 3;  // 1 = local, 2 = main
    if (location == 1 || location == 2) {
        t.location = location - 1;
    } else {
        noteState(std::format("texture location {:#x}", location));
    }
    t.offset = regs.reg(kMethodNv4097SetTextureOffset + base) & 0x7FFFFFFF;
    t.pitch = regs.reg(kMethodNv4097SetTextureControl3 + unit * 4) & 0xFFFFF;

    const std::uint32_t rect = regs.reg(kMethodNv4097SetTextureImageRect + base);
    t.width = rect >> 16;
    t.height = t.dimension == 1 ? 1 : (rect & 0xFFFF);
    // The depth shares SET_TEXTURE_CONTROL3 with the pitch (RPCS3 `fragment_texture::depth`). Games leave
    // unused dimensions at 0, so a zero depth counts as one slice.
    if (t.dimension == 3) {
        t.depth = static_cast<std::uint16_t>(
                std::max<std::uint32_t>(regs.reg(kMethodNv4097SetTextureControl3 + unit * 4) >> 20, 1));
    }
    t.control0 = regs.reg(kMethodNv4097SetTextureControl0 + base);
    t.remap = regs.reg(kMethodNv4097SetTextureControl1 + base);
    t.filter = regs.reg(kMethodNv4097SetTextureFilter + base);
    t.address = regs.reg(kMethodNv4097SetTextureAddress + base);
    t.border = regs.reg(kMethodNv4097SetTextureBorderColor + base);

    // Unnormalized coordinates are supported: the renderer scales them by 1 / size.
    if (!rsxTextureFormatSampleable(format) && !rsxTextureFormatRenderTargetOnly(format)) {
        rsxNoteUnimplemented("texture format", rsxTextureFormatName(format));
    }
    return t;
}

// RPCS3 `rsx_decode.h` registers_decoder<NV4097_SET_INDEX_ARRAY_ADDRESS> (29-bit offset) and
// <NV4097_SET_INDEX_ARRAY_DMA> (location bits 0-3, type bits 4-11).
RsxIndexArray decodeIndexArray(const RsxRegisters& regs) {
    RsxIndexArray a;
    const std::uint32_t dma = regs.reg(kMethodNv4097SetIndexArrayDma);
    a.offset = regs.reg(kMethodNv4097SetIndexArrayAddress) & 0x1FFFFFFF;
    a.location = dma & 0xF;
    const std::uint32_t type = (dma >> 4) & 0xFF;
    if (a.location > 1) {
        noteState(std::format("index array location {:#x}", a.location));
        a.location = 0;
    }
    if (type == kRsxIndexTypeU32 || type == kRsxIndexTypeU16) {
        a.type = static_cast<std::uint8_t>(type);
    } else {
        noteState(std::format("index type {:#x}", type));
        a.type = static_cast<std::uint8_t>(kRsxIndexTypeU32);
    }
    return a;
}

bool rsxPrimitiveRestartSupported(RsxPrimitive primitive, std::uint8_t index_type, std::uint32_t restart_index) {
    const bool restartable = primitive == RsxPrimitive::LineStrip || primitive == RsxPrimitive::TriangleStrip ||
                             primitive == RsxPrimitive::TriangleFan || primitive == RsxPrimitive::QuadStrip;
    const std::uint32_t all_ones = index_type == kRsxIndexTypeU16 ? 0xFFFFu : 0xFFFFFFFFu;
    return restartable && restart_index == all_ones;
}

bool decodePrimitive(std::uint32_t value, RsxPrimitive& out) {
    if (value == 0) return false;
    if (value > static_cast<std::uint32_t>(RsxPrimitive::Polygon)) {
        rsxNoteUnimplemented("primitive", std::format("?{:#x}", value));
        return false;
    }
    out = static_cast<RsxPrimitive>(value);
    return true;
}

// RPCS3 `rsx_decode.h` registers_decoder<NV4097_SET_COLOR_CLEAR_VALUE>, <..._ZSTENCIL_CLEAR_VALUE>;
// the clear rectangle is the scissor (`VKGSRender::clear_surface` uses `m_scissor`).
RsxClear decodeClear(const RsxRegisters& regs, std::uint32_t mask) {
    RsxClear c;
    c.surfaces = decodeSurfaceState(regs);
    c.mask = mask;
    c.color_argb = regs.reg(kMethodNv4097SetColorClearValue);
    c.depth_stencil = regs.reg(kMethodNv4097SetZstencilClearValue);
    const std::uint32_t scissor_h = regs.reg(kMethodNv4097SetScissorHorizontal);
    const std::uint32_t scissor_v = regs.reg(kMethodNv4097SetScissorVertical);
    c.scissor[0] = scissor_h & 0xFFFF;
    c.scissor[1] = scissor_v & 0xFFFF;
    c.scissor[2] = scissor_h >> 16;
    c.scissor[3] = scissor_v >> 16;
    return c;
}

const char* rsxIgnoredMethodName(std::uint32_t method) {
    for (const MethodRange& range : kIgnoredMethods) {
        if (method >= range.first && method < range.first + range.count * 4) return range.name;
    }
    return nullptr;
}

}  // namespace tox2

#include "rsx/rsx_registers.h"

#include <atomic>

#include "rsx/fifo_parser.h"

namespace tox2 {

namespace {

// Shared by all RsxRegisters so that names from different instances (a capture's registers and the replay's) never
// collide. Starts at 1: 0 means "not named yet".
std::uint64_t nextStoreGeneration() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Non-zero register defaults: RPCS3 `Emu/RSX/rsx_methods.cpp` `rsx_state::init`, i.e. the state
// after the commands cellGcmInit injects, plus the init-only SET_SHADER_PROGRAM and
// SET_REFERENCE values. Registers of the other engines (2D, copy, memory-to-memory; method offsets
// from 0x2000) are omitted because nothing decodes them. `count` registers starting at `method`,
// `stride` bytes apart, all get `value`.
struct RegisterDefault {
    std::uint16_t method;
    std::uint8_t count;
    std::uint8_t stride;
    std::uint32_t value;
};

constexpr RegisterDefault kRegisterDefaults[] = {
        {0x0000, 1, 0x00, 0x31337000},  // SET_OBJECT
        {0x0050, 1, 0x00, 0xFFFFFFFF},  // SET_REFERENCE
        {0x0060, 1, 0x00, 0x66616661},  // NV406E_SET_CONTEXT_DMA_SEMAPHORE
        {0x0064, 1, 0x00, 0x30},        // SEMAPHORE_OFFSET
        {0x0068, 1, 0x00, 0x1},         // SEMAPHORE_ACQUIRE
        {0x0180, 1, 0x00, 0x66604200},  // SET_CONTEXT_DMA_NOTIFIES
        {0x0184, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_A
        {0x0188, 1, 0x00, 0xFEED0001},  // SET_CONTEXT_DMA_B
        {0x018C, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_B
        {0x0194, 3, 0x04, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_A
        {0x01A0, 1, 0x00, 0xFEED0001},  // SET_CONTEXT_DMA_VERTEX_B
        {0x01A4, 1, 0x00, 0x66606660},  // NV4097_SET_CONTEXT_DMA_SEMAPHORE
        {0x01A8, 1, 0x00, 0x66626660},  // SET_CONTEXT_DMA_REPORT
        {0x01B4, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_C
        {0x01B8, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_D
        {0x0208, 1, 0x00, 0x121},       // SET_SURFACE_FORMAT
        {0x020C, 1, 0x00, 0x40},        // SET_SURFACE_PITCH_A
        {0x021C, 1, 0x00, 0x40},        // SET_SURFACE_PITCH_B
        {0x0220, 1, 0x00, 0x1},         // SET_SURFACE_COLOR_TARGET
        {0x0224, 1, 0x00, 0x80},       {0x0228, 1, 0x00, 0x100},
        {0x022C, 1, 0x00, 0x40},                                 // SET_SURFACE_PITCH_Z
        {0x0240, 1, 0x00, 0xFFFF},     {0x0280, 1, 0x00, 0x40},  // SET_SURFACE_PITCH_C
        {0x0284, 1, 0x00, 0x40},                                 // SET_SURFACE_PITCH_D
        {0x02C0, 16, 0x04, 0xFFF0000},                           // SET_WINDOW_CLIP_HORIZONTAL
        {0x0300, 1, 0x00, 0x1},                                  // SET_DITHER_ENABLE
        {0x0308, 1, 0x00, 0x207},                                // SET_ALPHA_FUNC
        {0x0314, 1, 0x00, 0x10001},                              // SET_BLEND_FUNC_SFACTOR
        {0x0320, 1, 0x00, 0x80068006},                           // SET_BLEND_EQUATION
        {0x0324, 1, 0x00, 0x1010101},                            // SET_COLOR_MASK
        {0x032C, 1, 0x00, 0xFF},                                 // SET_STENCIL_MASK
        {0x0330, 1, 0x00, 0x207},                                // SET_STENCIL_FUNC
        {0x0338, 1, 0x00, 0xFF},                                 // SET_STENCIL_FUNC_MASK
        {0x033C, 3, 0x04, 0x1E00},                               // SET_STENCIL_OP_FAIL
        {0x034C, 1, 0x00, 0xFF},                                 // SET_BACK_STENCIL_MASK
        {0x0350, 1, 0x00, 0x207},                                // SET_BACK_STENCIL_FUNC
        {0x0358, 1, 0x00, 0xFF},                                 // SET_BACK_STENCIL_FUNC_MASK
        {0x035C, 3, 0x04, 0x1E00},                               // SET_BACK_STENCIL_OP_FAIL
        {0x0368, 1, 0x00, 0x1D01},                               // SET_SHADE_MODE
        {0x0378, 1, 0x00, 0x1503},                               // SET_LOGIC_OP
        {0x0388, 1, 0x00, 0x3F800000},                           // SET_DEPTH_BOUNDS_MAX
        {0x0398, 1, 0x00, 0x3F800000},                           // SET_CLIP_MAX
        {0x03B0, 1, 0x00, 0x100000},                             // SET_CONTROL0
        {0x03B8, 1, 0x00, 0x8},                                  // SET_LINE_WIDTH
        {0x03C0, 16, 0x04, 0x10101},                             // SET_ANISO_SPREAD
        {0x0400, 16, 0x04, 0x7421},    {0x0440, 1, 0x00, 0x9AABAA98},
        {0x0444, 1, 0x00, 0x66666789}, {0x0448, 1, 0x00, 0x98766666},
        {0x044C, 1, 0x00, 0x89AABAA9}, {0x0450, 1, 0x00, 0x99999999},
        {0x0454, 1, 0x00, 0x88888889}, {0x0458, 1, 0x00, 0x98888888},
        {0x045C, 1, 0x00, 0x99999999}, {0x0460, 1, 0x00, 0x56676654},
        {0x0464, 1, 0x00, 0x33333345}, {0x0468, 1, 0x00, 0x54333333},
        {0x046C, 1, 0x00, 0x45667665}, {0x0470, 1, 0x00, 0xAABBBA99},
        {0x0474, 1, 0x00, 0x66667899}, {0x0478, 1, 0x00, 0x99876666},
        {0x047C, 1, 0x00, 0x99ABBBAA}, {0x08C0, 1, 0x00, 0x10000000},  // SET_SCISSOR_HORIZONTAL
        {0x08C4, 1, 0x00, 0x10000000},                                 // SET_SCISSOR_VERTICAL
        {0x08CC, 1, 0x00, 0x800},                                      // SET_FOG_MODE
        {0x08E4, 1, 0x00, 0x1},                                        // SET_SHADER_PROGRAM
        {0x0904, 4, 0x20, 0x1BC21},                                    // SET_VERTEX_TEXTURE_FORMAT
        {0x0908, 4, 0x20, 0x101},                                      // SET_VERTEX_TEXTURE_ADDRESS
        {0x090C, 4, 0x20, 0x60000},                                    // SET_VERTEX_TEXTURE_CONTROL0
        {0x0910, 4, 0x20, 0x8},                                        // SET_VERTEX_TEXTURE_CONTROL3
        {0x0918, 4, 0x20, 0x80008},                                    // SET_VERTEX_TEXTURE_IMAGE_RECT
        {0x0A00, 1, 0x00, 0x10000000},                                 // SET_VIEWPORT_HORIZONTAL
        {0x0A04, 1, 0x00, 0x10000000},                                 // SET_VIEWPORT_VERTICAL
        {0x0A20, 1, 0x00, 0x45000000},                                 // SET_VIEWPORT_OFFSET
        {0x0A24, 1, 0x00, 0x45000000}, {0x0A28, 1, 0x00, 0x3F000000},
        {0x0A30, 1, 0x00, 0x45000000},  // SET_VIEWPORT_SCALE
        {0x0A34, 1, 0x00, 0x45000000}, {0x0A38, 1, 0x00, 0x3F000000},
        {0x0A6C, 1, 0x00, 0x201},       // SET_DEPTH_FUNC
        {0x0A70, 1, 0x00, 0x1},         // SET_DEPTH_MASK
        {0x0B00, 16, 0x04, 0x2DC8},     // SET_TEXTURE_CONTROL2
        {0x1428, 1, 0x00, 0x1},         // SET_SPECULAR_ENABLE
        {0x1450, 1, 0x00, 0x80003},     // SET_PERFORMANCE_PARAMS
        {0x145C, 1, 0x00, 0x1},         // SET_EDGE_FLAG
        {0x1740, 16, 0x04, 0x2},        // SET_VERTEX_DATA_ARRAY_FORMAT
        {0x1828, 1, 0x00, 0x1B02},      // SET_FRONT_POLYGON_MODE
        {0x182C, 1, 0x00, 0x1B02},      // SET_BACK_POLYGON_MODE
        {0x1830, 1, 0x00, 0x405},       // SET_CULL_FACE
        {0x1834, 1, 0x00, 0x901},       // SET_FRONT_FACE
        {0x1840, 16, 0x04, 0x100008},   // SET_TEXTURE_CONTROL3
        {0x1A04, 16, 0x20, 0x18429},    // SET_TEXTURE_FORMAT
        {0x1A08, 16, 0x20, 0x30101},    // SET_TEXTURE_ADDRESS
        {0x1A0C, 16, 0x20, 0x60000},    // SET_TEXTURE_CONTROL0
        {0x1A10, 16, 0x20, 0xAAE4},     // SET_TEXTURE_CONTROL1
        {0x1A14, 16, 0x20, 0x2052000},  // SET_TEXTURE_FILTER
        {0x1A18, 16, 0x20, 0x80008},    // SET_TEXTURE_IMAGE_RECT
        {0x1D64, 1, 0x00, 0x2000000},   // SET_INDEXED_CONSTANT_READ_LIMITS
        {0x1D78, 1, 0x00, 0x1},         // SET_ZMIN_MAX_CONTROL
        {0x1D7C, 1, 0x00, 0xFFFF0000},  // SET_ANTI_ALIASING_CONTROL
        {0x1D80, 1, 0x00, 0x3},         // SET_SURFACE_COMPRESSION
        {0x1D84, 1, 0x00, 0x3},         // SET_ZCULL_EN
        {0x1D88, 1, 0x00, 0x1000},      // SET_SHADER_WINDOW
        {0x1D8C, 1, 0x00, 0xFFFFFF00},  // SET_ZSTENCIL_CLEAR_VALUE
        {0x1D98, 1, 0x00, 0xFFF0000},   // SET_CLEAR_RECT_HORIZONTAL
        {0x1D9C, 1, 0x00, 0xFFF0000},   // SET_CLEAR_RECT_VERTICAL
        {0x1DB0, 1, 0x00, 0xFFFFFFFF},  // SET_RESTART_INDEX
        {0x1E94, 1, 0x00, 0x11},        // SET_TRANSFORM_EXECUTION_MODE
        {0x1E98, 1, 0x00, 0x1000000},   // SET_RENDER_ENABLE
        {0x1EA4, 1, 0x00, 0x10},        // SET_ZCULL_CONTROL0
        {0x1EA8, 1, 0x00, 0x1000100},   // SET_ZCULL_CONTROL1
        {0x1EAC, 1, 0x00, 0xFF000002},  // SET_SCULL_CONTROL
        {0x1EE0, 1, 0x00, 0x3F800000},  // SET_POINT_SIZE
        {0x1FC4, 1, 0x00, 0x6144321},   // SET_ATTRIB_COLOR
        {0x1FC8, 1, 0x00, 0xEDCBA987},  // SET_ATTRIB_TEX_COORD
        {0x1FCC, 1, 0x00, 0x6F},        // SET_ATTRIB_TEX_COORD_EX
        {0x1FD0, 1, 0x00, 0x171615},    // SET_ATTRIB_UCLIP0
        {0x1FD4, 1, 0x00, 0x1B1A19},    // SET_ATTRIB_UCLIP1
        {0x1FE0, 1, 0x00, 0x1},         // SET_REDUCE_DST_COLOR
        {0x1FF0, 1, 0x00, 0xFFFF},      // SET_VERTEX_ATTRIB_INPUT_MASK
        {0x1FF4, 1, 0x00, 0x3FFFFF},    // SET_VERTEX_ATTRIB_OUTPUT_MASK
};

// Values written by RPCS3 `rsx_state::reset` (rsx_methods.cpp), which `flip_command` runs through
// `thread::reset` on every flip. Every explicit assignment is listed, zeros included, sorted by method;
// registers it does not assign (texture offset/format/control1/rect/control3, vertex texture
// format/control3/rect, ZCULL_EN, ZCULL_CONTROL, SCULL_CONTROL, SHADER_PROGRAM, REFERENCE,
// SEMAPHORE_ACQUIRE) keep their value, and SEMAPHORE_OFFSET differs from the init value. Where RPCS3
// assigns a register twice the last value counts (SET_CONTEXT_DMA_SEMAPHORE). The texture address of
// unit 2 is missing on purpose: RPCS3 writes unit 1 twice and never unit 2.
constexpr RegisterDefault kContextResetValues[] = {
        {0x0000, 1, 0x00, 0x31337000},  // SET_OBJECT
        {0x0060, 1, 0x00, 0x66616661},  // SET_CONTEXT_DMA_SEMAPHORE
        {0x0064, 1, 0x00, 0x10},        // SEMAPHORE_OFFSET
        {0x0100, 1, 0x00, 0x0},         // NO_OPERATION
        {0x0180, 1, 0x00, 0x66604200},  // SET_CONTEXT_DMA_NOTIFIES
        {0x0184, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_A
        {0x0188, 1, 0x00, 0xFEED0001},  // SET_CONTEXT_DMA_B
        {0x018C, 1, 0x00, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_B
        {0x0190, 1, 0x00, 0x0},         // SET_CONTEXT_DMA_STATE
        {0x0194, 3, 0x04, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_A
        {0x01A0, 1, 0x00, 0xFEED0001},  // SET_CONTEXT_DMA_VERTEX_B
        {0x01A4, 1, 0x00, 0x66606660},  // SET_CONTEXT_DMA_SEMAPHORE
        {0x01A8, 1, 0x00, 0x66626660},  // SET_CONTEXT_DMA_REPORT
        {0x01AC, 2, 0x04, 0x0},         // SET_CONTEXT_DMA_CLIP_ID
        {0x01B4, 2, 0x04, 0xFEED0000},  // SET_CONTEXT_DMA_COLOR_C
        {0x0200, 2, 0x04, 0x0},         // SET_SURFACE_CLIP_HORIZONTAL
        {0x0208, 1, 0x00, 0x121},       // SET_SURFACE_FORMAT
        {0x020C, 1, 0x00, 0x40},        // SET_SURFACE_PITCH_A
        {0x0210, 3, 0x04, 0x0},         // SET_SURFACE_COLOR_AOFFSET
        {0x021C, 1, 0x00, 0x40},        // SET_SURFACE_PITCH_B
        {0x0220, 1, 0x00, 0x1},         // SET_SURFACE_COLOR_TARGET
        {0x0224, 1, 0x00, 0x80},        // 0x224 / 4
        {0x0228, 1, 0x00, 0x100},       // 0x228 / 4
        {0x022C, 1, 0x00, 0x40},        // SET_SURFACE_PITCH_Z
        {0x0230, 2, 0x08, 0x0},         // 0x230 / 4
        {0x0240, 1, 0x00, 0xFFFF},      // 0x240 / 4
        {0x0244, 3, 0x04, 0x0},         // 0x244 / 4
        {0x0280, 2, 0x04, 0x40},        // SET_SURFACE_PITCH_C
        {0x0288, 2, 0x04, 0x0},         // SET_SURFACE_COLOR_COFFSET
        {0x02B8, 2, 0x04, 0x0},         // SET_WINDOW_OFFSET
        {0x02C0, 16, 0x04, 0xFFF0000},  // 0x02c0 / 4
        {0x0300, 1, 0x00, 0x1},         // SET_DITHER_ENABLE
        {0x0304, 1, 0x00, 0x0},         // SET_ALPHA_TEST_ENABLE
        {0x0308, 1, 0x00, 0x207},       // SET_ALPHA_FUNC
        {0x030C, 2, 0x04, 0x0},         // SET_ALPHA_REF
        {0x0314, 1, 0x00, 0x10001},     // SET_BLEND_FUNC_SFACTOR
        {0x0318, 2, 0x04, 0x0},         // SET_BLEND_FUNC_DFACTOR
        {0x0320, 1, 0x00, 0x80068006},  // SET_BLEND_EQUATION
        {0x0324, 1, 0x00, 0x1010101},   // SET_COLOR_MASK
        {0x0328, 1, 0x00, 0x0},         // SET_STENCIL_TEST_ENABLE
        {0x032C, 1, 0x00, 0xFF},        // SET_STENCIL_MASK
        {0x0330, 1, 0x00, 0x207},       // SET_STENCIL_FUNC
        {0x0334, 1, 0x00, 0x0},         // SET_STENCIL_FUNC_REF
        {0x0338, 1, 0x00, 0xFF},        // SET_STENCIL_FUNC_MASK
        {0x033C, 3, 0x04, 0x1E00},      // SET_STENCIL_OP_FAIL
        {0x0348, 1, 0x00, 0x0},         // SET_TWO_SIDED_STENCIL_TEST_ENABLE
        {0x034C, 1, 0x00, 0xFF},        // SET_BACK_STENCIL_MASK
        {0x0350, 1, 0x00, 0x207},       // SET_BACK_STENCIL_FUNC
        {0x0354, 1, 0x00, 0x0},         // SET_BACK_STENCIL_FUNC_REF
        {0x0358, 1, 0x00, 0xFF},        // SET_BACK_STENCIL_FUNC_MASK
        {0x035C, 3, 0x04, 0x1E00},      // SET_BACK_STENCIL_OP_FAIL
        {0x0368, 1, 0x00, 0x1D01},      // SET_SHADE_MODE
        {0x036C, 3, 0x04, 0x0},         // SET_BLEND_ENABLE_MRT
        {0x0378, 1, 0x00, 0x1503},      // SET_LOGIC_OP
        {0x037C, 3, 0x04, 0x0},         // SET_BLEND_COLOR2
        {0x0388, 1, 0x00, 0x3F800000},  // SET_DEPTH_BOUNDS_MAX
        {0x0394, 1, 0x00, 0x0},         // SET_CLIP_MIN
        {0x0398, 1, 0x00, 0x3F800000},  // SET_CLIP_MAX
        {0x03B0, 1, 0x00, 0x100000},    // SET_CONTROL0
        {0x03B8, 1, 0x00, 0x8},         // SET_LINE_WIDTH
        {0x03BC, 1, 0x00, 0x0},         // SET_LINE_SMOOTH_ENABLE
        {0x03C0, 16, 0x04, 0x10101},    // SET_ANISO_SPREAD
        {0x0400, 16, 0x04, 0x7421},     // 0x400 / 4
        {0x0440, 1, 0x00, 0x9AABAA98},  // 0x440 / 4
        {0x0444, 1, 0x00, 0x66666789},  // 0x444 / 4
        {0x0448, 1, 0x00, 0x98766666},  // 0x448 / 4
        {0x044C, 1, 0x00, 0x89AABAA9},  // 0x44c / 4
        {0x0450, 1, 0x00, 0x99999999},  // 0x450 / 4
        {0x0454, 1, 0x00, 0x88888889},  // 0x454 / 4
        {0x0458, 1, 0x00, 0x98888888},  // 0x458 / 4
        {0x045C, 1, 0x00, 0x99999999},  // 0x45c / 4
        {0x0460, 1, 0x00, 0x56676654},  // 0x460 / 4
        {0x0464, 1, 0x00, 0x33333345},  // 0x464 / 4
        {0x0468, 1, 0x00, 0x54333333},  // 0x468 / 4
        {0x046C, 1, 0x00, 0x45667665},  // 0x46c / 4
        {0x0470, 1, 0x00, 0xAABBBA99},  // 0x470 / 4
        {0x0474, 1, 0x00, 0x66667899},  // 0x474 / 4
        {0x0478, 1, 0x00, 0x99876666},  // 0x478 / 4
        {0x047C, 1, 0x00, 0x99ABBBAA},  // 0x47c / 4
        {0x08C0, 2, 0x04, 0x10000000},  // SET_SCISSOR_HORIZONTAL
        {0x08CC, 1, 0x00, 0x800},       // SET_FOG_MODE
        {0x08D0, 3, 0x04, 0x0},         // SET_FOG_PARAMS
        {0x0908, 1, 0x00, 0x101},       // SET_VERTEX_TEXTURE_ADDRESS
        {0x090C, 1, 0x00, 0x60000},     // SET_VERTEX_TEXTURE_CONTROL0
        {0x0914, 2, 0x08, 0x0},         // SET_VERTEX_TEXTURE_FILTER
        {0x0928, 1, 0x00, 0x101},       // SET_VERTEX_TEXTURE_ADDRESS
        {0x092C, 1, 0x00, 0x60000},     // SET_VERTEX_TEXTURE_CONTROL0
        {0x0934, 2, 0x08, 0x0},         // SET_VERTEX_TEXTURE_FILTER
        {0x0948, 1, 0x00, 0x101},       // SET_VERTEX_TEXTURE_ADDRESS
        {0x094C, 1, 0x00, 0x60000},     // SET_VERTEX_TEXTURE_CONTROL0
        {0x0954, 2, 0x08, 0x0},         // SET_VERTEX_TEXTURE_FILTER
        {0x0968, 1, 0x00, 0x101},       // SET_VERTEX_TEXTURE_ADDRESS
        {0x096C, 1, 0x00, 0x60000},     // SET_VERTEX_TEXTURE_CONTROL0
        {0x0974, 2, 0x08, 0x0},         // SET_VERTEX_TEXTURE_FILTER
        {0x0A00, 2, 0x04, 0x10000000},  // SET_VIEWPORT_HORIZONTAL
        {0x0A0C, 1, 0x00, 0x0},         // 0xa0c / 4
        {0x0A20, 2, 0x04, 0x45000000},  // SET_VIEWPORT_OFFSET
        {0x0A28, 1, 0x00, 0x3F000000},  // SET_VIEWPORT_OFFSET
        {0x0A2C, 1, 0x00, 0x0},         // SET_VIEWPORT_OFFSET
        {0x0A30, 2, 0x04, 0x45000000},  // SET_VIEWPORT_SCALE
        {0x0A38, 1, 0x00, 0x3F000000},  // SET_VIEWPORT_SCALE
        {0x0A3C, 2, 0x24, 0x0},         // SET_VIEWPORT_SCALE
        {0x0A64, 2, 0x04, 0x0},         // SET_POLY_OFFSET_LINE_ENABLE
        {0x0A6C, 1, 0x00, 0x201},       // SET_DEPTH_FUNC
        {0x0A70, 1, 0x00, 0x1},         // SET_DEPTH_MASK
        {0x0A74, 3, 0x04, 0x0},         // SET_DEPTH_TEST_ENABLE
        {0x0B00, 16, 0x04, 0x2DC8},     // SET_TEXTURE_CONTROL2
        {0x0B40, 10, 0x04, 0x0},        // SET_TEX_COORD_CONTROL
        {0x1428, 1, 0x00, 0x1},         // 0x1428 / 4
        {0x142C, 1, 0x00, 0x0},         // SET_TWO_SIDE_LIGHT_EN
        {0x1450, 1, 0x00, 0x80003},     // 0x1450 / 4
        {0x1454, 1, 0x00, 0x0},         // 0x1454 / 4
        {0x145C, 1, 0x00, 0x1},         // 0x145c / 4
        {0x1478, 2, 0x04, 0x0},         // SET_USER_CLIP_PLANE_CONTROL
        {0x1680, 16, 0x04, 0x0},        // SET_VERTEX_DATA_ARRAY_OFFSET
        {0x1738, 2, 0x04, 0x0},         // SET_VERTEX_DATA_BASE_OFFSET
        {0x1740, 16, 0x04, 0x2},        // SET_VERTEX_DATA_ARRAY_FORMAT
        {0x17CC, 1, 0x00, 0x0},         // SET_ZPASS_PIXEL_COUNT_ENABLE
        {0x1828, 2, 0x04, 0x1B02},      // SET_FRONT_POLYGON_MODE
        {0x1830, 1, 0x00, 0x405},       // SET_CULL_FACE
        {0x1834, 1, 0x00, 0x901},       // SET_FRONT_FACE
        {0x1838, 2, 0x04, 0x0},         // SET_POLY_SMOOTH_ENABLE
        {0x1A08, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1A0C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1A14, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1A1C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1A28, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1A2C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1A34, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1A3C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1A4C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1A54, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1A5C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1A68, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1A6C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1A74, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1A7C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1A88, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1A8C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1A94, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1A9C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1AA8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1AAC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1AB4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1ABC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1AC8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1ACC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1AD4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1ADC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1AE8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1AEC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1AF4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1AFC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1B08, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1B0C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1B14, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1B1C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1B28, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1B2C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1B34, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1B3C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1B48, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1B4C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1B54, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1B5C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1B68, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1B6C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1B74, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1B7C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1B88, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1B8C, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1B94, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1B9C, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1BA8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1BAC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1BB4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1BBC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1BC8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1BCC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1BD4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1BDC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1BE8, 1, 0x00, 0x30101},     // SET_TEXTURE_ADDRESS
        {0x1BEC, 1, 0x00, 0x60000},     // SET_TEXTURE_CONTROL0
        {0x1BF4, 1, 0x00, 0x2052000},   // SET_TEXTURE_FILTER
        {0x1BFC, 1, 0x00, 0x0},         // SET_TEXTURE_BORDER_COLOR
        {0x1D64, 1, 0x00, 0x2000000},   // 0x1d64 / 4
        {0x1D78, 1, 0x00, 0x1},         // SET_ZMIN_MAX_CONTROL
        {0x1D7C, 1, 0x00, 0xFFFF0000},  // SET_ANTI_ALIASING_CONTROL
        {0x1D80, 1, 0x00, 0x3},         // 0x1d80 / 4
        {0x1D88, 1, 0x00, 0x1000},      // SET_SHADER_WINDOW
        {0x1D8C, 1, 0x00, 0xFFFFFF00},  // SET_ZSTENCIL_CLEAR_VALUE
        {0x1D90, 2, 0x04, 0x0},         // SET_COLOR_CLEAR_VALUE
        {0x1D98, 2, 0x04, 0xFFF0000},   // 0x1d98 / 4
        {0x1DA4, 2, 0x08, 0x0},         // 0x1da4 / 4
        {0x1DB0, 1, 0x00, 0xFFFFFFFF},  // SET_RESTART_INDEX
        {0x1DB4, 1, 0x00, 0x0},         // SET_LINE_STIPPLE
        {0x1E94, 1, 0x00, 0x11},        // 0x1e94 / 4
        {0x1E98, 1, 0x00, 0x1000000},   // SET_RENDER_ENABLE
        {0x1EE0, 1, 0x00, 0x3F800000},  // SET_POINT_SIZE
        {0x1EE4, 2, 0x04, 0x0},         // SET_POINT_PARAMS_ENABLE
        {0x1FC0, 1, 0x00, 0x0},         // SET_FREQUENCY_DIVIDER_OPERATION
        {0x1FC4, 1, 0x00, 0x6144321},   // SET_ATTRIB_COLOR
        {0x1FC8, 1, 0x00, 0xEDCBA987},  // SET_ATTRIB_TEX_COORD
        {0x1FCC, 1, 0x00, 0x6F},        // SET_ATTRIB_TEX_COORD_EX
        {0x1FD0, 1, 0x00, 0x171615},    // SET_ATTRIB_UCLIP0
        {0x1FD4, 1, 0x00, 0x1B1A19},    // SET_ATTRIB_UCLIP1
        {0x1FE0, 1, 0x00, 0x1},         // SET_REDUCE_DST_COLOR
        {0x1FE8, 2, 0x04, 0x0},         // SET_NO_PARANOID_TEXTURE_FETCHES
        {0x1FF0, 1, 0x00, 0xFFFF},      // SET_VERTEX_ATTRIB_INPUT_MASK
        {0x1FF4, 1, 0x00, 0x3FFFFF},    // SET_VERTEX_ATTRIB_OUTPUT_MASK
        {0x1FF8, 1, 0x00, 0x0},         // SET_TRANSFORM_BRANCH_BITS
        {0x2000, 1, 0x00, 0x31337303},  // NV0039_SET_OBJECT
        {0x2180, 1, 0x00, 0x66604200},  // NV0039_SET_CONTEXT_DMA_NOTIFIES
        {0x2184, 1, 0x00, 0xFEED0001},  // NV0039_SET_CONTEXT_DMA_BUFFER_IN
        {0x2188, 1, 0x00, 0xFEED0000},  // NV0039_SET_CONTEXT_DMA_BUFFER_OUT
        {0x6000, 1, 0x00, 0x313371C3},  // NV3062_SET_OBJECT
        {0x6180, 1, 0x00, 0x66604200},  // NV3062_SET_CONTEXT_DMA_NOTIFIES
        {0x6184, 2, 0x04, 0xFEED0000},  // NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE
        {0x8000, 1, 0x00, 0x31337A73},  // 0x8000 / 4
        {0x8180, 1, 0x00, 0x66604200},  // 0x8180 / 4
        {0x8184, 1, 0x00, 0xFEED0000},  // 0x8184 / 4
        {0xA000, 1, 0x00, 0x31337808},  // 0xa000 / 4
        {0xA180, 1, 0x00, 0x66604200},  // 0xa180 / 4
        {0xA184, 6, 0x04, 0x0},         // 0xa184 / 4
        {0xA19C, 1, 0x00, 0x313371C3},  // 0xa19c / 4
        {0xA2FC, 1, 0x00, 0x3},         // 0xa2fc / 4
        {0xA300, 1, 0x00, 0x4},         // 0xa300 / 4
        {0xC000, 1, 0x00, 0x3137AF00},  // 0xc000 / 4
        {0xC180, 1, 0x00, 0x66604200},  // 0xc180 / 4
        {0xE000, 1, 0x00, 0xCAFEBABE},  // GCM_SET_DRIVER_OBJECT
};

}  // namespace

std::uint64_t RsxRegisters::vpUcodeGeneration() const {
    if (vp_ucode_generation == 0) vp_ucode_generation = nextStoreGeneration();
    return vp_ucode_generation;
}

std::uint64_t RsxRegisters::vpConstantsGeneration() const {
    if (vp_constants_generation == 0) vp_constants_generation = nextStoreGeneration();
    return vp_constants_generation;
}

void RsxRegisters::markVpStoresChanged() {
    vp_ucode_generation = 0;
    vp_constants_generation = 0;
}

void RsxRegisters::reset() {
    regs.fill(0);
    vp_ucode.fill(0);
    vp_constants.fill(0);
    markVpStoresChanged();
    vp_load = vp_start = constant_load = 0;
    vertex_default_written = 0;
    for (const RegisterDefault& entry : kRegisterDefaults) {
        for (std::uint32_t i = 0; i < entry.count; i++) regs[(entry.method + i * entry.stride) >> 2] = entry.value;
    }
}

void RsxRegisters::contextReset() {
    for (const RegisterDefault& entry : kContextResetValues) {
        for (std::uint32_t i = 0; i < entry.count; i++)
            regs[((entry.method + i * entry.stride) >> 2) & 0x3FFF] = entry.value;
    }
}

namespace {

struct ImmediateVertexForm {
    std::uint32_t first;       // first method of the block
    std::uint32_t registers;   // registers in the block
    std::uint32_t per_slot;    // registers per attribute slot
    std::uint32_t components;  // components the form carries (3F pads its fourth register)
};

// RPCS3 `NV47/HW/nv4097.h` set_vertex_data_impl: attribute = index / registers per attribute,
// component = index % registers per attribute.
constexpr ImmediateVertexForm kImmediateVertexForms[] = {
        {kMethodNv4097SetVertexData3fM, 64, 4, 3},
        {kMethodNv4097SetVertexData2fM, 32, 2, 2},
        {kMethodNv4097SetVertexData4fM, 64, 4, 4},
        {kMethodNv4097SetVertexData1fM, 16, 1, 1},
};

}  // namespace

void RsxRegisters::apply(std::uint32_t method, std::uint32_t value) {
    method &= 0xFFFC;
    regs[method >> 2] = value;

    for (const ImmediateVertexForm& form : kImmediateVertexForms) {
        if (method < form.first || method >= form.first + form.registers * 4) continue;
        const std::uint32_t index = (method - form.first) >> 2;
        const std::uint32_t slot = index / form.per_slot, component = index % form.per_slot;
        if (component >= form.components) return;
        constexpr std::uint32_t kOne = 0x3F800000;
        const std::uint32_t base = (kMethodNv4097SetVertexData4fM >> 2) + slot * 4;
        regs[base + component] = value;
        for (std::uint32_t c = form.components; c < 4; c++) regs[base + c] = c == 3 ? kOne : 0;
        vertex_default_written |= 1u << slot;
        return;
    }

    if (method == kMethodNv4097SetTransformProgramLoad) {
        vp_load = value;
    } else if (method == kMethodNv4097SetTransformProgramStart) {
        vp_start = value;
    } else if (method == kMethodNv4097SetTransformConstantLoad) {
        constant_load = value;
    } else if (method >= kMethodNv4097SetTransformProgram &&
               method < kMethodNv4097SetTransformProgram + kRsxTransformWindow * 4) {
        // Only the slot within an instruction counts: a block of any length restarts at the load
        // pointer, which moves on after each completed 128-bit instruction.
        const std::uint32_t lane = ((method - kMethodNv4097SetTransformProgram) >> 2) & 3;
        const std::uint64_t slot = static_cast<std::uint64_t>(vp_load) * 4 + lane;
        if (slot < vp_ucode.size()) {
            vp_ucode[slot] = value;
            vp_ucode_generation = 0;
            // RPCS3 clips the block at the end of the store, so the load pointer stops there too.
            if (lane == 3) {
                vp_load++;
                regs[kMethodNv4097SetTransformProgramLoad >> 2] = vp_load;
            }
        }
    } else if (method >= kMethodNv4097SetTransformConstant &&
               method < kMethodNv4097SetTransformConstant + kRsxTransformWindow * 4) {
        const std::uint32_t index = (method - kMethodNv4097SetTransformConstant) >> 2;
        const std::uint64_t slot = (static_cast<std::uint64_t>(constant_load) + index / 4) * 4 + index % 4;
        if (slot < vp_constants.size()) {
            vp_constants[slot] = value;
            vp_constants_generation = 0;
        }
    }
}

}  // namespace tox2

#include "rsx/rsx_census.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <thread>
#include <tuple>

#include <yyjson.h>

#include "core/census.h"
#include "core/log.h"
#include "rsx/fifo_parser.h"
#include "rsx/rsx_decode.h"

namespace tox2 {
namespace {

constexpr std::size_t kValueCap = 256;      // distinct values kept per register
constexpr std::size_t kValueListed = 64;    // values listed per register in the JSON
constexpr std::size_t kTextCap = 4096;      // distinct keys per text category
constexpr std::size_t kMaxSeconds = 86400;  // length of the per-second series

struct NameEntry {
    std::uint32_t method;
    std::uint32_t count;
    std::uint32_t stride;  // bytes between slots; 4 when the slots are consecutive registers
    const char* name;
};

// Arrays: the 16 texture units are 0x20 bytes apart, the vertex textures (4 units) as well; the
// others are consecutive registers.
constexpr NameEntry kNames[] = {
        {0x0000, 1, 0, "NV4097_SET_OBJECT"},
        {0x0050, 1, 0, "NV406E_SET_REFERENCE"},
        {0x0060, 1, 0, "NV406E_SET_CONTEXT_DMA_SEMAPHORE"},
        {0x0064, 1, 0, "NV406E_SEMAPHORE_OFFSET"},
        {0x0068, 1, 0, "NV406E_SEMAPHORE_ACQUIRE"},
        {0x006C, 1, 0, "NV406E_SEMAPHORE_RELEASE"},
        {0x0100, 1, 0, "NV4097_NO_OPERATION"},
        {0x0104, 1, 0, "NV4097_NOTIFY"},
        {0x0110, 1, 0, "NV4097_WAIT_FOR_IDLE"},
        {0x0140, 1, 0, "NV4097_PM_TRIGGER"},
        {0x0180, 1, 0, "NV4097_SET_CONTEXT_DMA_NOTIFIES"},
        {0x0184, 1, 0, "NV4097_SET_CONTEXT_DMA_A"},
        {0x0188, 1, 0, "NV4097_SET_CONTEXT_DMA_B"},
        {0x018C, 1, 0, "NV4097_SET_CONTEXT_DMA_COLOR_B"},
        {0x0190, 1, 0, "NV4097_SET_CONTEXT_DMA_STATE"},
        {0x0194, 1, 0, "NV4097_SET_CONTEXT_DMA_COLOR_A"},
        {0x0198, 1, 0, "NV4097_SET_CONTEXT_DMA_ZETA"},
        {0x019C, 1, 0, "NV4097_SET_CONTEXT_DMA_VERTEX_A"},
        {0x01A0, 1, 0, "NV4097_SET_CONTEXT_DMA_VERTEX_B"},
        {0x01A4, 1, 0, "NV4097_SET_CONTEXT_DMA_SEMAPHORE"},
        {0x01A8, 1, 0, "NV4097_SET_CONTEXT_DMA_REPORT"},
        {0x01AC, 1, 0, "NV4097_SET_CONTEXT_DMA_CLIP_ID"},
        {0x01B0, 1, 0, "NV4097_SET_CONTEXT_DMA_CULL_DATA"},
        {0x01B4, 1, 0, "NV4097_SET_CONTEXT_DMA_COLOR_C"},
        {0x01B8, 1, 0, "NV4097_SET_CONTEXT_DMA_COLOR_D"},
        {0x0200, 1, 0, "NV4097_SET_SURFACE_CLIP_HORIZONTAL"},
        {0x0204, 1, 0, "NV4097_SET_SURFACE_CLIP_VERTICAL"},
        {0x0208, 1, 0, "NV4097_SET_SURFACE_FORMAT"},
        {0x020C, 1, 0, "NV4097_SET_SURFACE_PITCH_A"},
        {0x0210, 1, 0, "NV4097_SET_SURFACE_COLOR_AOFFSET"},
        {0x0214, 1, 0, "NV4097_SET_SURFACE_ZETA_OFFSET"},
        {0x0218, 1, 0, "NV4097_SET_SURFACE_COLOR_BOFFSET"},
        {0x021C, 1, 0, "NV4097_SET_SURFACE_PITCH_B"},
        {0x0220, 1, 0, "NV4097_SET_SURFACE_COLOR_TARGET"},
        {0x022C, 1, 0, "NV4097_SET_SURFACE_PITCH_Z"},
        {0x0234, 1, 0, "NV4097_INVALIDATE_ZCULL"},
        {0x0238, 1, 0, "NV4097_SET_CYLINDRICAL_WRAP"},
        {0x023C, 1, 0, "NV4097_SET_CYLINDRICAL_WRAP1"},
        {0x0280, 1, 0, "NV4097_SET_SURFACE_PITCH_C"},
        {0x0284, 1, 0, "NV4097_SET_SURFACE_PITCH_D"},
        {0x0288, 1, 0, "NV4097_SET_SURFACE_COLOR_COFFSET"},
        {0x028C, 1, 0, "NV4097_SET_SURFACE_COLOR_DOFFSET"},
        {0x02B8, 1, 0, "NV4097_SET_WINDOW_OFFSET"},
        {0x02BC, 1, 0, "NV4097_SET_WINDOW_CLIP_TYPE"},
        {0x02C0, 1, 0, "NV4097_SET_WINDOW_CLIP_HORIZONTAL"},
        {0x02C4, 1, 0, "NV4097_SET_WINDOW_CLIP_VERTICAL"},
        {0x0300, 1, 0, "NV4097_SET_DITHER_ENABLE"},
        {0x0304, 1, 0, "NV4097_SET_ALPHA_TEST_ENABLE"},
        {0x0308, 1, 0, "NV4097_SET_ALPHA_FUNC"},
        {0x030C, 1, 0, "NV4097_SET_ALPHA_REF"},
        {0x0310, 1, 0, "NV4097_SET_BLEND_ENABLE"},
        {0x0314, 1, 0, "NV4097_SET_BLEND_FUNC_SFACTOR"},
        {0x0318, 1, 0, "NV4097_SET_BLEND_FUNC_DFACTOR"},
        {0x031C, 1, 0, "NV4097_SET_BLEND_COLOR"},
        {0x0320, 1, 0, "NV4097_SET_BLEND_EQUATION"},
        {0x0324, 1, 0, "NV4097_SET_COLOR_MASK"},
        {0x0328, 1, 0, "NV4097_SET_STENCIL_TEST_ENABLE"},
        {0x032C, 1, 0, "NV4097_SET_STENCIL_MASK"},
        {0x0330, 1, 0, "NV4097_SET_STENCIL_FUNC"},
        {0x0334, 1, 0, "NV4097_SET_STENCIL_FUNC_REF"},
        {0x0338, 1, 0, "NV4097_SET_STENCIL_FUNC_MASK"},
        {0x033C, 1, 0, "NV4097_SET_STENCIL_OP_FAIL"},
        {0x0340, 1, 0, "NV4097_SET_STENCIL_OP_ZFAIL"},
        {0x0344, 1, 0, "NV4097_SET_STENCIL_OP_ZPASS"},
        {0x0348, 1, 0, "NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE"},
        {0x034C, 1, 0, "NV4097_SET_BACK_STENCIL_MASK"},
        {0x0350, 1, 0, "NV4097_SET_BACK_STENCIL_FUNC"},
        {0x0354, 1, 0, "NV4097_SET_BACK_STENCIL_FUNC_REF"},
        {0x0358, 1, 0, "NV4097_SET_BACK_STENCIL_FUNC_MASK"},
        {0x035C, 1, 0, "NV4097_SET_BACK_STENCIL_OP_FAIL"},
        {0x0360, 1, 0, "NV4097_SET_BACK_STENCIL_OP_ZFAIL"},
        {0x0364, 1, 0, "NV4097_SET_BACK_STENCIL_OP_ZPASS"},
        {0x0368, 1, 0, "NV4097_SET_SHADE_MODE"},
        {0x036C, 1, 0, "NV4097_SET_BLEND_ENABLE_MRT"},
        {0x0370, 1, 0, "NV4097_SET_COLOR_MASK_MRT"},
        {0x0374, 1, 0, "NV4097_SET_LOGIC_OP_ENABLE"},
        {0x0378, 1, 0, "NV4097_SET_LOGIC_OP"},
        {0x037C, 1, 0, "NV4097_SET_BLEND_COLOR2"},
        {0x0380, 1, 0, "NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE"},
        {0x0384, 1, 0, "NV4097_SET_DEPTH_BOUNDS_MIN"},
        {0x0388, 1, 0, "NV4097_SET_DEPTH_BOUNDS_MAX"},
        {0x0394, 1, 0, "NV4097_SET_CLIP_MIN"},
        {0x0398, 1, 0, "NV4097_SET_CLIP_MAX"},
        {0x03B0, 1, 0, "NV4097_SET_CONTROL0"},
        {0x03B8, 1, 0, "NV4097_SET_LINE_WIDTH"},
        {0x03BC, 1, 0, "NV4097_SET_LINE_SMOOTH_ENABLE"},
        {0x03C0, 1, 0, "NV4097_SET_ANISO_SPREAD"},
        {0x08C0, 1, 0, "NV4097_SET_SCISSOR_HORIZONTAL"},
        {0x08C4, 1, 0, "NV4097_SET_SCISSOR_VERTICAL"},
        {0x08CC, 1, 0, "NV4097_SET_FOG_MODE"},
        {0x08D0, 2, 4, "NV4097_SET_FOG_PARAMS"},
        {0x08E4, 1, 0, "NV4097_SET_SHADER_PROGRAM"},
        {0x0900, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_OFFSET"},
        {0x0904, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_FORMAT"},
        {0x0908, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_ADDRESS"},
        {0x090C, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_CONTROL0"},
        {0x0910, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_CONTROL3"},
        {0x0914, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_FILTER"},
        {0x0918, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_IMAGE_RECT"},
        {0x091C, 4, 0x20, "NV4097_SET_VERTEX_TEXTURE_BORDER_COLOR"},
        {0x0A00, 1, 0, "NV4097_SET_VIEWPORT_HORIZONTAL"},
        {0x0A04, 1, 0, "NV4097_SET_VIEWPORT_VERTICAL"},
        {0x0A0C, 1, 0, "NV4097_SET_POINT_CENTER_MODE"},
        {0x0A1C, 1, 0, "NV4097_ZCULL_SYNC"},
        {0x0A20, 4, 4, "NV4097_SET_VIEWPORT_OFFSET"},
        {0x0A30, 4, 4, "NV4097_SET_VIEWPORT_SCALE"},
        {0x0A60, 1, 0, "NV4097_SET_POLY_OFFSET_POINT_ENABLE"},
        {0x0A64, 1, 0, "NV4097_SET_POLY_OFFSET_LINE_ENABLE"},
        {0x0A68, 1, 0, "NV4097_SET_POLY_OFFSET_FILL_ENABLE"},
        {0x0A6C, 1, 0, "NV4097_SET_DEPTH_FUNC"},
        {0x0A70, 1, 0, "NV4097_SET_DEPTH_MASK"},
        {0x0A74, 1, 0, "NV4097_SET_DEPTH_TEST_ENABLE"},
        {0x0A78, 1, 0, "NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR"},
        {0x0A7C, 1, 0, "NV4097_SET_POLYGON_OFFSET_BIAS"},
        {0x0A80, 32, 4, "NV4097_SET_VERTEX_DATA_SCALED4S_M"},
        {0x0B00, 1, 0, "NV4097_SET_TEXTURE_CONTROL2"},
        {0x0B40, 1, 0, "NV4097_SET_TEX_COORD_CONTROL"},
        {0x0B80, 32, 4, "NV4097_SET_TRANSFORM_PROGRAM"},
        {0x1428, 1, 0, "NV4097_SET_SPECULAR_ENABLE"},
        {0x142C, 1, 0, "NV4097_SET_TWO_SIDE_LIGHT_EN"},
        {0x1438, 1, 0, "NV4097_CLEAR_ZCULL_SURFACE"},
        {0x1450, 1, 0, "NV4097_SET_PERFORMANCE_PARAMS"},
        {0x1454, 1, 0, "NV4097_SET_FLAT_SHADE_OP"},
        {0x145C, 1, 0, "NV4097_SET_EDGE_FLAG"},
        {0x1478, 1, 0, "NV4097_SET_USER_CLIP_PLANE_CONTROL"},
        {0x147C, 1, 0, "NV4097_SET_POLYGON_STIPPLE"},
        {0x1480, 32, 4, "NV4097_SET_POLYGON_STIPPLE_PATTERN"},
        {0x1500, 48, 4, "NV4097_SET_VERTEX_DATA3F_M"},
        {0x1680, 16, 4, "NV4097_SET_VERTEX_DATA_ARRAY_OFFSET"},
        {0x1710, 1, 0, "NV4097_INVALIDATE_VERTEX_CACHE_FILE"},
        {0x1714, 1, 0, "NV4097_INVALIDATE_VERTEX_FILE"},
        {0x1718, 1, 0, "NV4097_PIPE_NOP"},
        {0x1738, 1, 0, "NV4097_SET_VERTEX_DATA_BASE_OFFSET"},
        {0x173C, 1, 0, "NV4097_SET_VERTEX_DATA_BASE_INDEX"},
        {0x1740, 16, 4, "NV4097_SET_VERTEX_DATA_ARRAY_FORMAT"},
        {0x17C8, 1, 0, "NV4097_CLEAR_REPORT_VALUE"},
        {0x17CC, 1, 0, "NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE"},
        {0x1800, 1, 0, "NV4097_GET_REPORT"},
        {0x1804, 1, 0, "NV4097_SET_ZCULL_STATS_ENABLE"},
        {0x1808, 1, 0, "NV4097_SET_BEGIN_END"},
        {0x180C, 1, 0, "NV4097_ARRAY_ELEMENT16"},
        {0x1810, 1, 0, "NV4097_ARRAY_ELEMENT32"},
        {0x1814, 1, 0, "NV4097_DRAW_ARRAYS"},
        {0x1818, 1, 0, "NV4097_INLINE_ARRAY"},
        {0x181C, 1, 0, "NV4097_SET_INDEX_ARRAY_ADDRESS"},
        {0x1820, 1, 0, "NV4097_SET_INDEX_ARRAY_DMA"},
        {0x1824, 1, 0, "NV4097_DRAW_INDEX_ARRAY"},
        {0x1828, 1, 0, "NV4097_SET_FRONT_POLYGON_MODE"},
        {0x182C, 1, 0, "NV4097_SET_BACK_POLYGON_MODE"},
        {0x1830, 1, 0, "NV4097_SET_CULL_FACE"},
        {0x1834, 1, 0, "NV4097_SET_FRONT_FACE"},
        {0x1838, 1, 0, "NV4097_SET_POLY_SMOOTH_ENABLE"},
        {0x183C, 1, 0, "NV4097_SET_CULL_FACE_ENABLE"},
        {0x1840, 16, 4, "NV4097_SET_TEXTURE_CONTROL3"},
        {0x1880, 32, 4, "NV4097_SET_VERTEX_DATA2F_M"},
        {0x1900, 16, 4, "NV4097_SET_VERTEX_DATA2S_M"},
        {0x1940, 16, 4, "NV4097_SET_VERTEX_DATA4UB_M"},
        {0x1980, 32, 4, "NV4097_SET_VERTEX_DATA4S_M"},
        {0x1A00, 16, 0x20, "NV4097_SET_TEXTURE_OFFSET"},
        {0x1A04, 16, 0x20, "NV4097_SET_TEXTURE_FORMAT"},
        {0x1A08, 16, 0x20, "NV4097_SET_TEXTURE_ADDRESS"},
        {0x1A0C, 16, 0x20, "NV4097_SET_TEXTURE_CONTROL0"},
        {0x1A10, 16, 0x20, "NV4097_SET_TEXTURE_CONTROL1"},
        {0x1A14, 16, 0x20, "NV4097_SET_TEXTURE_FILTER"},
        {0x1A18, 16, 0x20, "NV4097_SET_TEXTURE_IMAGE_RECT"},
        {0x1A1C, 16, 0x20, "NV4097_SET_TEXTURE_BORDER_COLOR"},
        {0x1C00, 64, 4, "NV4097_SET_VERTEX_DATA4F_M"},
        {0x1D00, 1, 0, "NV4097_SET_COLOR_KEY_COLOR"},
        {0x1D60, 1, 0, "NV4097_SET_SHADER_CONTROL"},
        {0x1D64, 1, 0, "NV4097_SET_INDEXED_CONSTANT_READ_LIMITS"},
        {0x1D6C, 1, 0, "NV4097_SET_SEMAPHORE_OFFSET"},
        {0x1D70, 1, 0, "NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE"},
        {0x1D74, 1, 0, "NV4097_TEXTURE_READ_SEMAPHORE_RELEASE"},
        {0x1D78, 1, 0, "NV4097_SET_ZMIN_MAX_CONTROL"},
        {0x1D7C, 1, 0, "NV4097_SET_ANTI_ALIASING_CONTROL"},
        {0x1D80, 1, 0, "NV4097_SET_SURFACE_COMPRESSION"},
        {0x1D84, 1, 0, "NV4097_SET_ZCULL_EN"},
        {0x1D88, 1, 0, "NV4097_SET_SHADER_WINDOW"},
        {0x1D8C, 1, 0, "NV4097_SET_ZSTENCIL_CLEAR_VALUE"},
        {0x1D90, 1, 0, "NV4097_SET_COLOR_CLEAR_VALUE"},
        {0x1D94, 1, 0, "NV4097_CLEAR_SURFACE"},
        {0x1D98, 1, 0, "NV4097_SET_CLEAR_RECT_HORIZONTAL"},
        {0x1D9C, 1, 0, "NV4097_SET_CLEAR_RECT_VERTICAL"},
        {0x1DA4, 1, 0, "NV4097_SET_CLIP_ID_TEST_ENABLE"},
        {0x1DAC, 1, 0, "NV4097_SET_RESTART_INDEX_ENABLE"},
        {0x1DB0, 1, 0, "NV4097_SET_RESTART_INDEX"},
        {0x1DB4, 1, 0, "NV4097_SET_LINE_STIPPLE"},
        {0x1DB8, 1, 0, "NV4097_SET_LINE_STIPPLE_PATTERN"},
        {0x1E40, 16, 4, "NV4097_SET_VERTEX_DATA1F_M"},
        {0x1E94, 1, 0, "NV4097_SET_TRANSFORM_EXECUTION_MODE"},
        {0x1E98, 1, 0, "NV4097_SET_RENDER_ENABLE"},
        {0x1E9C, 1, 0, "NV4097_SET_TRANSFORM_PROGRAM_LOAD"},
        {0x1EA0, 1, 0, "NV4097_SET_TRANSFORM_PROGRAM_START"},
        {0x1EA4, 1, 0, "NV4097_SET_ZCULL_CONTROL0"},
        {0x1EA8, 1, 0, "NV4097_SET_ZCULL_CONTROL1"},
        {0x1EAC, 1, 0, "NV4097_SET_SCULL_CONTROL"},
        {0x1EE0, 1, 0, "NV4097_SET_POINT_SIZE"},
        {0x1EE4, 1, 0, "NV4097_SET_POINT_PARAMS_ENABLE"},
        {0x1EE8, 1, 0, "NV4097_SET_POINT_SPRITE_CONTROL"},
        {0x1EF8, 1, 0, "NV4097_SET_TRANSFORM_TIMEOUT"},
        {0x1EFC, 1, 0, "NV4097_SET_TRANSFORM_CONSTANT_LOAD"},
        {0x1F00, 32, 4, "NV4097_SET_TRANSFORM_CONSTANT"},
        {0x1FC0, 1, 0, "NV4097_SET_FREQUENCY_DIVIDER_OPERATION"},
        {0x1FC4, 1, 0, "NV4097_SET_ATTRIB_COLOR"},
        {0x1FC8, 1, 0, "NV4097_SET_ATTRIB_TEX_COORD"},
        {0x1FCC, 1, 0, "NV4097_SET_ATTRIB_TEX_COORD_EX"},
        {0x1FD0, 1, 0, "NV4097_SET_ATTRIB_UCLIP0"},
        {0x1FD4, 1, 0, "NV4097_SET_ATTRIB_UCLIP1"},
        {0x1FD8, 1, 0, "NV4097_INVALIDATE_L2"},
        {0x1FE0, 1, 0, "NV4097_SET_REDUCE_DST_COLOR"},
        {0x1FE8, 1, 0, "NV4097_SET_NO_PARANOID_TEXTURE_FETCHES"},
        {0x1FEC, 1, 0, "NV4097_SET_SHADER_PACKER"},
        {0x1FF0, 1, 0, "NV4097_SET_VERTEX_ATTRIB_INPUT_MASK"},
        {0x1FF4, 1, 0, "NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK"},
        {0x1FF8, 1, 0, "NV4097_SET_TRANSFORM_BRANCH_BITS"},
        {0x2000, 1, 0, "NV0039_SET_OBJECT"},
        {0x2180, 1, 0, "NV0039_SET_CONTEXT_DMA_NOTIFIES"},
        {0x2184, 1, 0, "NV0039_SET_CONTEXT_DMA_BUFFER_IN"},
        {0x2188, 1, 0, "NV0039_SET_CONTEXT_DMA_BUFFER_OUT"},
        {0x230C, 1, 0, "NV0039_OFFSET_IN"},
        {0x2310, 1, 0, "NV0039_OFFSET_OUT"},
        {0x2314, 1, 0, "NV0039_PITCH_IN"},
        {0x2318, 1, 0, "NV0039_PITCH_OUT"},
        {0x231C, 1, 0, "NV0039_LINE_LENGTH_IN"},
        {0x2320, 1, 0, "NV0039_LINE_COUNT"},
        {0x2324, 1, 0, "NV0039_FORMAT"},
        {0x2328, 1, 0, "NV0039_BUFFER_NOTIFY"},
        {0x6000, 1, 0, "NV3062_SET_OBJECT"},
        {0x6180, 1, 0, "NV3062_SET_CONTEXT_DMA_NOTIFIES"},
        {0x6184, 1, 0, "NV3062_SET_CONTEXT_DMA_IMAGE_SOURCE"},
        {0x6188, 1, 0, "NV3062_SET_CONTEXT_DMA_IMAGE_DESTIN"},
        {0x6300, 1, 0, "NV3062_SET_COLOR_FORMAT"},
        {0x6304, 1, 0, "NV3062_SET_PITCH"},
        {0x6308, 1, 0, "NV3062_SET_OFFSET_SOURCE"},
        {0x630C, 1, 0, "NV3062_SET_OFFSET_DESTIN"},
        {0xA000, 1, 0, "NV308A_SET_OBJECT"},
        {0xA180, 1, 0, "NV308A_SET_CONTEXT_DMA_NOTIFIES"},
        {0xA184, 1, 0, "NV308A_SET_CONTEXT_COLOR_KEY"},
        {0xA188, 1, 0, "NV308A_SET_CONTEXT_CLIP_RECTANGLE"},
        {0xA18C, 1, 0, "NV308A_SET_CONTEXT_PATTERN"},
        {0xA190, 1, 0, "NV308A_SET_CONTEXT_ROP"},
        {0xA194, 1, 0, "NV308A_SET_CONTEXT_BETA1"},
        {0xA198, 1, 0, "NV308A_SET_CONTEXT_BETA4"},
        {0xA19C, 1, 0, "NV308A_SET_CONTEXT_SURFACE"},
        {0xA2F8, 1, 0, "NV308A_SET_COLOR_CONVERSION"},
        {0xA2FC, 1, 0, "NV308A_SET_OPERATION"},
        {0xA300, 1, 0, "NV308A_SET_COLOR_FORMAT"},
        {0xA304, 1, 0, "NV308A_POINT"},
        {0xA308, 1, 0, "NV308A_SIZE_OUT"},
        {0xA30C, 1, 0, "NV308A_SIZE_IN"},
        {0xA400, 1, 0, "NV308A_COLOR"},
        {0xC000, 1, 0, "NV3089_SET_OBJECT"},
        {0xC180, 1, 0, "NV3089_SET_CONTEXT_DMA_NOTIFIES"},
        {0xC184, 1, 0, "NV3089_SET_CONTEXT_DMA_IMAGE"},
        {0xC188, 1, 0, "NV3089_SET_CONTEXT_PATTERN"},
        {0xC18C, 1, 0, "NV3089_SET_CONTEXT_ROP"},
        {0xC190, 1, 0, "NV3089_SET_CONTEXT_BETA1"},
        {0xC194, 1, 0, "NV3089_SET_CONTEXT_BETA4"},
        {0xC198, 1, 0, "NV3089_SET_CONTEXT_SURFACE"},
        {0xC2FC, 1, 0, "NV3089_SET_COLOR_CONVERSION"},
        {0xC300, 1, 0, "NV3089_SET_COLOR_FORMAT"},
        {0xC304, 1, 0, "NV3089_SET_OPERATION"},
        {0xC308, 1, 0, "NV3089_CLIP_POINT"},
        {0xC30C, 1, 0, "NV3089_CLIP_SIZE"},
        {0xC310, 1, 0, "NV3089_IMAGE_OUT_POINT"},
        {0xC314, 1, 0, "NV3089_IMAGE_OUT_SIZE"},
        {0xC318, 1, 0, "NV3089_DS_DX"},
        {0xC31C, 1, 0, "NV3089_DT_DY"},
        {0xC400, 1, 0, "NV3089_IMAGE_IN_SIZE"},
        {0xC404, 1, 0, "NV3089_IMAGE_IN_FORMAT"},
        {0xC408, 1, 0, "NV3089_IMAGE_IN_OFFSET"},
        {0xC40C, 1, 0, "NV3089_IMAGE_IN"},
        {0xE000, 1, 0, "GCM_SET_DRIVER_OBJECT"},
        {0xE920, 2, 4, "GCM_FLIP_HEAD"},
        {0xE940, 8, 4, "GCM_DRIVER_QUEUE"},
        {0xEB00, 2, 4, "GCM_SET_USER_COMMAND"},
        {0xFEAC, 1, 0, "GCM_FLIP_COMMAND"},
};

// Pixel and vertex data streams: only their write counts are interesting.
bool countOnly(std::uint32_t method) {
    struct Range {
        std::uint32_t first, last;
    };
    static constexpr Range kRanges[] = {
            {0x0A80, 0x0AFF}, {0x0B80, 0x0BFF}, {0x1480, 0x14FF}, {0x1500, 0x15BF}, {0x180C, 0x1818}, {0x1880, 0x19FF},
            {0x1C00, 0x1CFF}, {0x1E40, 0x1E7F}, {0x1F00, 0x1F7F}, {0xA400, 0xBFFF}, {0xC40C, 0xC40C},
    };
    return std::ranges::any_of(kRanges, [&](const Range& r) { return method >= r.first && method <= r.last; });
}

const char* methodClass(std::uint32_t method) {
    if (method < 0x100) return "NV406E";
    if (method < 0x2000) return "NV4097";
    if (method < 0x3000) return "NV0039";
    if (method >= 0x6000 && method < 0x7000) return "NV3062";
    if (method >= 0xA000 && method < 0xB000) return "NV308A";
    if (method >= 0xC000 && method < 0xD000) return "NV3089";
    if (method >= 0xE000) return "GCM";
    return "other";
}

struct NameMatch {
    const NameEntry* entry = nullptr;
    std::uint32_t slot = 0;
};

NameMatch findName(std::uint32_t method) {
    for (const NameEntry& e : kNames) {
        if (e.count == 1) {
            if (method == e.method) return {&e, 0};
            continue;
        }
        const std::uint32_t stride = e.stride;
        const std::uint32_t span = stride == 4 ? e.count * 4 : (e.count - 1) * stride + 4;
        if (method < e.method || method >= e.method + span) continue;
        if ((method - e.method) % stride != 0) continue;
        return {&e, (method - e.method) / stride};
    }
    return {};
}

// Value -> name for the small enumerations; "?<hex>" otherwise.
std::string named(const char* name, std::uint32_t value) {
    return name != nullptr ? std::string(name) : std::format("?{:#x}", value);
}

const char* locationName(std::uint32_t location) {
    switch (location) {
        case 0: return "local";
        case 1: return "main";
        default: return nullptr;
    }
}

const char* surfaceTypeName(std::uint32_t v) {
    switch (v) {
        case 1: return "linear";
        case 2: return "swizzle";
        default: return nullptr;
    }
}

const char* antialiasName(std::uint32_t v) {
    switch (v) {
        case 0: return "center_1";
        case 3: return "diagonal_centered_2";
        case 4: return "square_centered_4";
        case 5: return "square_rotated_4";
        default: return nullptr;
    }
}

const char* compareName(std::uint32_t v) {
    static constexpr const char* kFuncs[] = {"NEVER",   "LESS",     "EQUAL",  "LEQUAL",
                                             "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
    return v >= 0x200 && v < 0x208 ? kFuncs[v - 0x200] : nullptr;
}

const char* logicOpName(std::uint32_t v) {
    static constexpr const char* kOps[] = {
            "CLEAR", "AND",   "AND_REVERSE", "COPY",       "AND_INVERTED",  "NOOP",        "XOR",  "OR",
            "NOR",   "EQUIV", nullptr,       "OR_REVERSE", "COPY_INVERTED", "OR_INVERTED", "NAND", "SET"};
    return v >= 0x1500 && v < 0x1510 ? kOps[v - 0x1500] : nullptr;
}

const char* wrapName(std::uint32_t v) {
    static constexpr const char* kWrap[] = {nullptr,
                                            "WRAP",
                                            "MIRROR",
                                            "CLAMP_TO_EDGE",
                                            "BORDER",
                                            "CLAMP",
                                            "MIRROR_ONCE_CLAMP_TO_EDGE",
                                            "MIRROR_ONCE_BORDER",
                                            "MIRROR_ONCE_CLAMP"};
    return v < std::size(kWrap) ? kWrap[v] : nullptr;
}

const char* filterName(std::uint32_t v) {
    static constexpr const char* kFilter[] = {nullptr,          "NEAREST",        "LINEAR",        "NEAREST_NEAREST",
                                              "LINEAR_NEAREST", "NEAREST_LINEAR", "LINEAR_LINEAR", "CONVOLUTION"};
    return v < std::size(kFilter) ? kFilter[v] : nullptr;
}

const char* textureDimensionName(std::uint32_t v) {
    switch (v) {
        case 1: return "1D";
        case 2: return "2D";
        case 3: return "3D";
        default: return nullptr;
    }
}

std::string cullFaceName(std::uint32_t v) {
    switch (v) {
        case 0x404: return "FRONT";
        case 0x405: return "BACK";
        case 0x408: return "FRONT_AND_BACK";
        default: return std::format("?{:#x}", v);
    }
}

std::string colorMaskName(std::uint32_t v) {
    std::string out;
    out += (v & (1u << 16)) != 0 ? 'R' : '-';
    out += (v & (1u << 8)) != 0 ? 'G' : '-';
    out += (v & 1u) != 0 ? 'B' : '-';
    out += (v & (1u << 24)) != 0 ? 'A' : '-';
    return out;
}

std::string clearFlags(std::uint32_t v) {
    std::string out;
    if ((v & kRsxClearDepth) != 0) out += 'Z';
    if ((v & kRsxClearStencil) != 0) out += 'S';
    if ((v & kRsxClearRed) != 0) out += 'R';
    if ((v & kRsxClearGreen) != 0) out += 'G';
    if ((v & kRsxClearBlue) != 0) out += 'B';
    if ((v & kRsxClearAlpha) != 0) out += 'A';
    return out.empty() ? "none" : out;
}

std::string surfaceTargetName(std::uint32_t v) {
    switch (v) {
        case 0: return "NONE";
        case 1: return "TARGET_0";
        case 2: return "TARGET_1";
        case 0x13: return "MRT1";
        case 0x17: return "MRT2";
        case 0x1F: return "MRT3";
        default: return std::format("?{:#x}", v);
    }
}

std::string pairDecode(std::uint32_t value, const char* (*name)(std::uint32_t)) {
    return std::format("{} / {}", named(name(value & 0xFFFF), value & 0xFFFF), named(name(value >> 16), value >> 16));
}

std::string onOff(std::uint32_t v) { return v != 0 ? "on" : "off"; }

const char* const kFpOpcodeNames[] = {
        "NOP",   "MOV",   "MUL",   "ADD",    "MAD",    "DP3",    "DP4",  "DST",       "MIN", "MAX", "SLT",  "SGE",
        "SLE",   "SGT",   "SNE",   "SEQ",    "FRC",    "FLR",    "KIL",  "PK4",       "UP4", "DDX", "DDY",  "TEX",
        "TXP",   "TXD",   "RCP",   "RSQ",    "EX2",    "LG2",    "LIT",  "LRP",       "STR", "SFL", "COS",  "SIN",
        "PK2",   "UP2",   "POW",   "PKB",    "UPB",    "PK16",   "UP16", "BEM",       "PKG", "UPG", "DP2A", "TXL",
        nullptr, "TXB",   nullptr, "TEXBEM", "TXPBEM", "BEMLUM", "REFL", "TIMESWTEX", "DP2", "NRM", "DIV",  "DIVSQ",
        "LIF",   "FENCT", "FENCB", nullptr,  "BRK",    "CAL",    "IFE",  "LOOP",      "REP", "RET"};

const char* const kVpVecNames[] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3",   "DPH",   "DP4", "DST",
                                   "MIN", "MAX", "SLT", "SGE", "ARL", "FRC",   "FLR",   "SEQ", "SFL",
                                   "SGT", "SLE", "SNE", "STR", "SSG", nullptr, nullptr, "TXL"};
const char* const kVpScaNames[] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL",
                                   "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"};

std::string opcodeName(const char* const* table, std::size_t size, std::uint32_t opcode) {
    if (opcode < size && table[opcode] != nullptr) return table[opcode];
    return std::format("OP_{:#x}", opcode);
}

std::string hashName(std::uint64_t hash) { return std::format("{:016x}", hash); }

std::uint32_t rotateHalves(std::uint32_t word) { return std::rotl(word, 16); }

}  // namespace

std::string rsxMethodName(std::uint32_t method) {
    const NameMatch match = findName(method);
    if (match.entry == nullptr) return std::format("{:#06x}", method);
    if (match.entry->count == 1) return match.entry->name;
    return std::format("{}[{}]", match.entry->name, match.slot);
}

std::string rsxTextureFormatName(std::uint32_t format) {
    const char* base = nullptr;
    switch (format & kRsxTextureBaseMask) {
        case 0x81: base = "B8"; break;
        case 0x82: base = "A1R5G5B5"; break;
        case 0x83: base = "A4R4G4B4"; break;
        case 0x84: base = "R5G6B5"; break;
        case 0x85: base = "A8R8G8B8"; break;
        case 0x86: base = "DXT1"; break;
        case 0x87: base = "DXT23"; break;
        case 0x88: base = "DXT45"; break;
        case 0x8B: base = "G8B8"; break;
        case 0x8D: base = "B8R8_G8R8"; break;
        case 0x8E: base = "R8B8_R8G8"; break;
        case 0x8F: base = "R6G5B5"; break;
        case 0x90: base = "DEPTH24_D8"; break;
        case 0x91: base = "DEPTH24_D8_FLOAT"; break;
        case 0x92: base = "DEPTH16"; break;
        case 0x93: base = "DEPTH16_FLOAT"; break;
        case 0x94: base = "X16"; break;
        case 0x95: base = "Y16_X16"; break;
        case 0x97: base = "R5G5B5A1"; break;
        case 0x98: base = "HILO8"; break;
        case 0x99: base = "HILO_S8"; break;
        case 0x9A: base = "W16_Z16_Y16_X16_FLOAT"; break;
        case 0x9B: base = "W32_Z32_Y32_X32_FLOAT"; break;
        case 0x9C: base = "X32_FLOAT"; break;
        case 0x9D: base = "D1R5G5B5"; break;
        case 0x9E: base = "D8R8G8B8"; break;
        case 0x9F: base = "Y16_X16_FLOAT"; break;
        default: break;
    }
    std::string out = base != nullptr ? std::string(base) : std::format("?{:#x}", format & kRsxTextureBaseMask);
    out += (format & kRsxTextureLinearFlag) != 0 ? "_LN" : "_SZ";
    out += (format & kRsxTextureUnnormalizedFlag) != 0 ? "_UN" : "_NR";
    return out;
}

std::string rsxFpOpcodeName(std::uint32_t opcode) {
    return opcodeName(kFpOpcodeNames, std::size(kFpOpcodeNames), opcode);
}
std::string rsxVpVecOpcodeName(std::uint32_t opcode) { return opcodeName(kVpVecNames, std::size(kVpVecNames), opcode); }
std::string rsxVpScaOpcodeName(std::uint32_t opcode) { return opcodeName(kVpScaNames, std::size(kVpScaNames), opcode); }

std::uint64_t rsxHash64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * 0x100000001B3ull;
    }
    return hash;
}

std::string rsxDecodeArgument(std::uint32_t method, std::uint32_t value) {
    const NameMatch match = findName(method);
    if (match.entry == nullptr) return {};
    const std::uint32_t base = match.entry->method;
    const std::string_view name = match.entry->name;
    const auto is = [&](std::uint32_t reg) { return base == reg; };

    if (is(0x0208)) {
        return std::format("color={} depth={} type={} aa={} size={}x{}",
                           named(rsxSurfaceColorName(value & 0x1F), value & 0x1F),
                           named(rsxSurfaceDepthName((value >> 5) & 7), (value >> 5) & 7),
                           named(surfaceTypeName((value >> 8) & 0xF), (value >> 8) & 0xF),
                           named(antialiasName((value >> 12) & 0xF), (value >> 12) & 0xF), 1u << ((value >> 16) & 0xFF),
                           1u << ((value >> 24) & 0xFF));
    }
    if (is(0x0220)) return surfaceTargetName(value);
    if (is(0x0200) || is(0x02C0) || is(0x08C0) || is(0x0A00) || is(0x1D98)) {
        return std::format("x={} w={}", value & 0xFFFF, value >> 16);
    }
    if (is(0x0204) || is(0x02C4) || is(0x08C4) || is(0x0A04) || is(0x1D9C)) {
        return std::format("y={} h={}", value & 0xFFFF, value >> 16);
    }
    if (is(0x08E4)) {
        const std::uint32_t loc = (value & 3) - 1;
        return std::format("location={} offset={:#x}", named(locationName(loc), loc), value & ~3u);
    }
    if (is(0x1A04) || is(0x0904)) {
        const std::uint32_t loc = (value & 3) - 1;
        const std::uint32_t dim = (value >> 4) & 0xF;
        return std::format("location={} cubemap={} border={} dim={} format={} mipmaps={}",
                           named(locationName(loc), loc), (value >> 2) & 1, (value >> 3) & 1,
                           named(textureDimensionName(dim), dim), rsxTextureFormatName((value >> 8) & 0xFF),
                           value >> 16);
    }
    if (is(0x1A08) || is(0x0908)) {
        return std::format("wrap_s={} wrap_t={} wrap_r={}", named(wrapName(value & 0xF), value & 0xF),
                           named(wrapName((value >> 8) & 0xF), (value >> 8) & 0xF),
                           named(wrapName((value >> 16) & 0xF), (value >> 16) & 0xF));
    }
    if (is(0x1A0C) || is(0x090C)) {
        return std::format("enable={} min_lod_raw={} max_lod_raw={} aniso={} alpha_kill={}", value >> 31,
                           (value >> 19) & 0xFFF, (value >> 7) & 0xFFF, (value >> 4) & 7, (value >> 2) & 1);
    }
    if (is(0x1A14) || is(0x0914)) {
        return std::format("min={} mag={} bias_raw={:#x}", named(filterName((value >> 16) & 7), (value >> 16) & 7),
                           named(filterName((value >> 24) & 7), (value >> 24) & 7), value & 0x1FFF);
    }
    if (is(0x1A18) || is(0x0918)) return std::format("width={} height={}", value >> 16, value & 0xFFFF);
    if (is(0x1840) || is(0x0910)) return std::format("pitch={} depth={}", value & 0xFFFF, value >> 20);
    if (is(0x1740)) {
        return std::format("type={} size={} stride={} frequency={}", named(rsxVertexTypeName(value & 7), value & 7),
                           (value >> 4) & 0xF, (value >> 8) & 0xFF, value >> 16);
    }
    if (is(0x1680)) {
        return std::format("location={} offset={:#x}", value >> 31 != 0 ? "main" : "local", value & 0x7FFFFFFF);
    }
    if (is(0x1808)) return named(rsxPrimitiveName(value), value);
    if (is(0x1814) || is(0x1824)) return std::format("first={} count={}", value & 0xFFFFFF, (value >> 24) + 1);
    if (is(0x1820)) {
        const std::uint32_t loc = value & 0xF;
        return std::format("location={} type={}", named(locationName(loc), loc),
                           ((value >> 4) & 0xFF) == 1 ? "u16" : "u32");
    }
    if (is(0x1800)) return std::format("type={} offset={:#x}", value >> 24, value & 0xFFFFFF);
    if (is(0x1D94)) return clearFlags(value);
    if (is(0x0314) || is(0x0318)) return pairDecode(value, rsxBlendFactorName);
    if (is(0x0320)) return pairDecode(value, rsxBlendEquationName);
    if (is(0x0308) || is(0x0A6C) || is(0x0330) || is(0x0350)) return named(compareName(value), value);
    if (is(0x0324)) return colorMaskName(value);
    if (is(0x0378)) return named(logicOpName(value), value);
    if (is(0x1830)) return cullFaceName(value);
    if (is(0x1834)) return value == 0x900 ? "CW" : value == 0x901 ? "CCW" : std::format("?{:#x}", value);
    if (name.find("_ENABLE") != std::string_view::npos) return onOff(value);
    return {};
}

RsxFpScan scanFragmentProgram(const RsxFpWordReader& read) {
    RsxFpScan scan;
    for (std::uint32_t i = 0; i < kRsxFpMaxInstructions; i++) {
        std::uint32_t raw[4];
        for (std::uint32_t w = 0; w < 4; w++) {
            if (!read(i * 4 + scan.constant_words * 4 + w, raw[w])) return scan;
            scan.words.push_back(raw[w]);
        }
        // Each word is stored with its 16-bit halves swapped.
        std::uint32_t x[4];
        for (int w = 0; w < 4; w++) x[w] = rotateHalves(raw[w]);
        const bool end = (x[0] & 1) != 0;
        const std::uint32_t opcode = ((x[0] >> 24) & 0x3F) | ((x[2] >> 31) << 6);
        scan.instructions++;
        scan.opcodes[opcode]++;
        const bool constant = (x[1] & 3) == 2 || (x[2] & 3) == 2 || (x[3] & 3) == 2;
        if (constant) {
            for (std::uint32_t w = 0; w < 4; w++) {
                std::uint32_t word = 0;
                if (!read(i * 4 + scan.constant_words * 4 + 4 + w, word)) return scan;
                scan.words.push_back(word);
            }
            scan.constant_words++;
        }
        if (end) {
            scan.terminated = true;
            return scan;
        }
    }
    return scan;
}

RsxVpScan scanVertexProgram(const std::uint32_t* ucode, std::uint32_t start) {
    RsxVpScan scan;
    for (std::uint32_t i = start; i < kRsxVpInstructionCount; i++) {
        const std::uint32_t d1 = ucode[i * 4 + 1], d3 = ucode[i * 4 + 3];
        scan.instructions++;
        scan.vec_opcodes[(d1 >> 22) & 0x1F]++;
        scan.sca_opcodes[(d1 >> 27) & 0x1F]++;
        if ((d3 & 1) != 0) {
            scan.terminated = true;
            return scan;
        }
    }
    return scan;
}

RsxVpFlow analyzeVertexProgramFlow(const std::uint32_t* ucode, std::uint32_t instruction_count, std::uint32_t entry) {
    RsxVpFlow flow;
    flow.reached.assign(instruction_count, 0);
    flow.last = entry;

    std::vector<std::uint32_t> pending{entry};
    while (!pending.empty()) {
        std::uint32_t pc = pending.back();
        pending.pop_back();

        // Return addresses of the calls on this path; the hardware stack is not modelled beyond that.
        std::vector<std::uint32_t> returns;
        while (true) {
            // Falling off the end of the store without an end bit leaves `terminated` unset.
            if (pc >= instruction_count) break;
            if (flow.reached[pc] != 0) {
                // Already walked (a loop, or a tail shared by several paths).
                if (returns.empty()) break;
                pc = returns.back();
                returns.pop_back();
                continue;
            }
            flow.reached[pc] = 1;
            flow.last = std::max(flow.last, pc);

            const std::uint32_t* instruction = ucode + pc * 4;
            const std::uint32_t op = rsxVpScaOpcode(instruction);
            const std::uint32_t condition = rsxVpCondition(instruction);
            const std::uint32_t target = rsxVpBranchTarget(instruction);
            const bool is_call = op == kRsxVpScaCal || op == kRsxVpScaCli || op == kRsxVpScaClb;
            const bool is_branch = op == kRsxVpScaBri || op == kRsxVpScaBrb;
            const bool is_return = op == kRsxVpScaRet;
            if (is_call || is_branch || is_return) flow.has_flow_control = true;
            if ((is_call || is_branch) && (target < entry || target >= instruction_count)) {
                flow.bad_target = true;
            } else if (is_branch) {
                if (op == kRsxVpScaBri && condition == 7) {
                    // Always taken: nothing falls through.
                    pc = target;
                    continue;
                }
                pending.push_back(target);
            } else if (is_call) {
                returns.push_back(pc + 1);
                pc = target;
                continue;
            } else if (is_return && condition == 7) {
                if (returns.empty()) break;
                pc = returns.back();
                returns.pop_back();
                continue;
            }

            if (rsxVpIsEnd(instruction)) {
                flow.terminated = true;
                break;
            }
            pc++;
        }
    }
    return flow;
}

// ---- collector ----

RsxArgCensus::RsxArgCensus(std::filesystem::path json_path, std::filesystem::path dump_dir, MemoryReader reader,
                           std::uint64_t start_us)
    : json_path_(std::move(json_path)),
      dump_dir_(std::move(dump_dir)),
      reader_(std::move(reader)),
      start_us_(start_us) {}

std::size_t RsxArgCensus::secondOf(std::uint64_t now_us) const {
    return now_us > start_us_ ? static_cast<std::size_t>((now_us - start_us_) / 1000000) : 0;
}

void RsxArgCensus::bump(std::vector<std::uint32_t>& series, std::uint64_t now_us) {
    const std::size_t second = std::min(secondOf(now_us), kMaxSeconds - 1);
    if (series.size() <= second) series.resize(second + 1, 0);
    series[second]++;
}

void RsxArgCensus::addText(const std::string& category, const std::string& key) {
    auto& bucket = text_[category];
    const auto it = bucket.find(key);
    if (it != bucket.end()) {
        it->second++;
    } else if (bucket.size() < kTextCap) {
        bucket.emplace(key, 1);
    } else {
        bucket["(other)"]++;
    }
}

void RsxArgCensus::addSize(const std::string& category, std::uint32_t count) {
    const char* primitive = rsxPrimitiveName(primitive_);
    const std::uint32_t bucket = std::bit_ceil(std::max<std::uint32_t>(count, 1));
    addText(category + ".by_primitive", std::format("{} <= {}", primitive != nullptr ? primitive : "?", bucket));
}

void RsxArgCensus::onWrite(std::uint32_t method, std::uint32_t value) {
    method &= 0xFFFC;
    std::lock_guard lock(mutex_);
    regs_[method >> 2] = value;
    MethodStat& stat = methods_[method];
    stat.writes++;
    if (!countOnly(method)) {
        ValueCounts& values = stat.values;
        const auto it = values.counts.find(value);
        if (it != values.counts.end()) {
            it->second++;
        } else if (values.counts.size() < kValueCap) {
            values.counts.emplace(value, 1);
        } else {
            values.overflow++;
        }
    }

    if (method >= kMethodNv4097SetTransformProgram && method < kMethodNv4097SetTransformProgram + 32 * 4) {
        onVertexProgramWord(method, value);
        return;
    }
    switch (method) {
        case kMethodNv4097SetBeginEnd:
            if (value != 0) {
                primitive_ = value;
                onBegin();
            }
            break;
        case kMethodNv4097DrawArrays: onDrawArrays(value); break;
        case kMethodNv4097DrawIndexArray: onDrawIndexArray(value); break;
        case kMethodNv4097SetTransformConstantLoad: addText("transform_constant_load", std::format("{}", value)); break;
        default: break;
    }
    // Where textures and vertex arrays live: the offsets alone say little.
    if (method >= kMethodNv4097SetTextureOffset && method < kMethodNv4097SetTextureOffset + 16 * 0x20 &&
        (method - kMethodNv4097SetTextureOffset) % 0x20 == 0) {
        const std::uint32_t unit = (method - kMethodNv4097SetTextureOffset) / 0x20;
        const std::uint32_t loc = (regs_[(kMethodNv4097SetTextureOffset + unit * 0x20 + 4) >> 2] & 3) - 1;
        addText("class.texture_offset", named(locationName(loc), loc));
    } else if (method >= kMethodNv4097SetVertexDataArrayOffset &&
               method < kMethodNv4097SetVertexDataArrayOffset + 16 * 4) {
        addText("class.vertex_array_offset", (value >> 31) != 0 ? "main" : "local");
    }
}

void RsxArgCensus::onVertexProgramWord(std::uint32_t method, std::uint32_t value) {
    const std::uint32_t index = (method - kMethodNv4097SetTransformProgram) / 4;
    std::uint32_t& load = regs_[kMethodNv4097SetTransformProgramLoad >> 2];
    const std::uint64_t slot = static_cast<std::uint64_t>(load) * 4 + (index & 3);
    if (slot < vp_ucode_.size()) vp_ucode_[slot] = value;
    // The load pointer moves on after each completed 128-bit instruction.
    if ((index & 3) == 3) load++;
}

void RsxArgCensus::onDrawArrays(std::uint32_t value) {
    const std::uint32_t first = value & 0xFFFFFF, count = (value >> 24) + 1;
    addText("draw_arrays.first", std::format("{}", first));
    addText("draw_arrays.count", std::format("{}", count));
    addSize("draw_arrays", count);
}

void RsxArgCensus::onDrawIndexArray(std::uint32_t value) {
    const std::uint32_t count = (value >> 24) + 1;
    addText("draw_index_array.count", std::format("{}", count));
    addText("draw_index_array.format",
            rsxDecodeArgument(kMethodNv4097SetIndexArrayDma, regs_[kMethodNv4097SetIndexArrayDma >> 2]));
    addSize("draw_index_array", count);
}

std::string RsxArgCensus::snapshotFragmentProgram() {
    const std::uint32_t value = regs_[kMethodNv4097SetShaderProgram >> 2];
    const std::uint32_t location = (value & 3) - 1, offset = value & ~3u;
    if (location > 1) {
        unreadable_fragment_programs_++;
        return "fp_none";
    }
    RsxFpScan scan = scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
        return reader_ && reader_(location, offset + index * 4, word);
    });
    if (!scan.terminated) {
        unreadable_fragment_programs_++;
        return "fp_unreadable";
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(scan.words.size() * 4);
    for (const std::uint32_t word : scan.words) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    }
    const std::uint64_t hash = rsxHash64(bytes.data(), bytes.size());
    auto [it, inserted] = fragment_programs_.try_emplace(hash);
    FragmentProgram& program = it->second;
    if (inserted) {
        program.location = location;
        program.offset = offset;
        program.zero_filled = std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0; });
        program.scan = std::move(scan);
        program.bytes = std::move(bytes);
    }
    program.uses++;
    return "fp_" + hashName(hash);
}

std::string RsxArgCensus::snapshotVertexProgram() {
    const std::uint32_t start = regs_[kMethodNv4097SetTransformProgramStart >> 2];
    if (start >= kRsxVpInstructionCount) return "vp_none";
    RsxVpScan scan = scanVertexProgram(vp_ucode_.data(), start);
    if (!scan.terminated) unterminated_vertex_programs_++;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(scan.instructions * 16);
    for (std::uint32_t i = 0; i < scan.instructions * 4; i++) {
        const std::uint32_t word = vp_ucode_[start * 4 + i];
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    }
    const std::uint64_t hash = rsxHash64(bytes.data(), bytes.size());
    auto [it, inserted] = vertex_programs_.try_emplace(hash);
    VertexProgram& program = it->second;
    if (inserted) {
        program.start = start;
        program.scan = std::move(scan);
        program.bytes = std::move(bytes);
    }
    program.uses++;
    return "vp_" + hashName(hash);
}

void RsxArgCensus::onBegin() {
    draws_++;
    frame_draws_++;
    bump(draws_per_second_, systemTimeUs());
    addText("draw.primitive", named(rsxPrimitiveName(primitive_), primitive_));
    addText("draw.surface",
            rsxDecodeArgument(kMethodNv4097SetSurfaceFormat, regs_[kMethodNv4097SetSurfaceFormat >> 2]) +
                    " target=" + surfaceTargetName(regs_[0x0220 >> 2]));

    std::string layout;
    for (std::uint32_t slot = 0; slot < 16; slot++) {
        const std::uint32_t format = regs_[(kMethodNv4097SetVertexDataArrayFormat >> 2) + slot];
        if (((format >> 4) & 0xF) == 0) continue;
        const std::uint32_t offset = regs_[(kMethodNv4097SetVertexDataArrayOffset >> 2) + slot];
        layout += std::format("{}{}:{}x{}/stride{}/freq{}@{}", layout.empty() ? "" : " ", slot,
                              named(rsxVertexTypeName(format & 7), format & 7), (format >> 4) & 0xF,
                              (format >> 8) & 0xFF, format >> 16, (offset >> 31) != 0 ? "main" : "local");
    }
    addText("draw.vertex_layout", layout.empty() ? "(none)" : layout);

    std::string textures;
    for (std::uint32_t unit = 0; unit < 16; unit++) {
        const std::uint32_t base = (kMethodNv4097SetTextureOffset >> 2) + unit * 8;
        const std::uint32_t control0 = regs_[base + 3];
        if ((control0 >> 31) == 0) continue;
        const std::uint32_t format = regs_[base + 1], rect = regs_[base + 6];
        const std::uint32_t loc = (format & 3) - 1;
        std::string entry = std::format("u{} {}{} {}x{} mips{} @{}", unit, rsxTextureFormatName((format >> 8) & 0xFF),
                                        ((format >> 2) & 1) != 0 ? " cube" : "", rect >> 16, rect & 0xFFFF,
                                        format >> 16, named(locationName(loc), loc));
        textures += (textures.empty() ? "" : "; ") + entry;
        addText("draw.texture_unit", entry);
    }
    addText("draw.textures", textures.empty() ? "(none)" : textures);

    const auto reg = [&](std::uint32_t method) { return regs_[method >> 2]; };
    const std::string state = std::format(
            "blend={} {} / {} eq {} | alpha_test={} {} ref {:#x} | depth_test={} {} mask {} | stencil={} {} | cull={} {} {} | "
            "colormask={} | logic={} {}",
            onOff(reg(0x0310)), rsxDecodeArgument(0x0314, reg(0x0314)), rsxDecodeArgument(0x0318, reg(0x0318)),
            rsxDecodeArgument(0x0320, reg(0x0320)), onOff(reg(0x0304)), rsxDecodeArgument(0x0308, reg(0x0308)),
            reg(0x030C), onOff(reg(0x0A74)), rsxDecodeArgument(0x0A6C, reg(0x0A6C)), reg(0x0A70), onOff(reg(0x0328)),
            rsxDecodeArgument(0x0330, reg(0x0330)), onOff(reg(0x183C)), cullFaceName(reg(0x1830)),
            rsxDecodeArgument(0x1834, reg(0x1834)), colorMaskName(reg(0x0324)), onOff(reg(0x0374)),
            rsxDecodeArgument(0x0378, reg(0x0378)));
    addText("draw.state", state);

    const std::string fp = snapshotFragmentProgram();
    const std::string vp = snapshotVertexProgram();
    addText("draw.program_pair", vp + " + " + fp);
}

void RsxArgCensus::onFlip(std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    flips_++;
    bump(flips_per_second_, now_us);
    draws_per_frame_[frame_draws_]++;
    frame_draws_ = 0;
}

void RsxArgCensus::onVBlank(std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    vblanks_++;
    bump(vblanks_per_second_, now_us);
}

std::uint64_t RsxArgCensus::writesOf(std::uint32_t method) {
    std::lock_guard lock(mutex_);
    const auto it = methods_.find(method);
    return it == methods_.end() ? 0 : it->second.writes;
}

std::size_t RsxArgCensus::distinctValuesOf(std::uint32_t method) {
    std::lock_guard lock(mutex_);
    const auto it = methods_.find(method);
    return it == methods_.end() ? 0 : it->second.values.counts.size();
}

std::uint64_t RsxArgCensus::draws() {
    std::lock_guard lock(mutex_);
    return draws_;
}

std::size_t RsxArgCensus::fragmentProgramCount() {
    std::lock_guard lock(mutex_);
    return fragment_programs_.size();
}

std::size_t RsxArgCensus::vertexProgramCount() {
    std::lock_guard lock(mutex_);
    return vertex_programs_.size();
}

// ---- JSON ----

namespace {

// Keys are copied: they are built at run time.
void addUint(yyjson_mut_doc* doc, yyjson_mut_val* obj, const std::string& key, std::uint64_t value) {
    yyjson_mut_obj_add(obj, yyjson_mut_strncpy(doc, key.data(), key.size()), yyjson_mut_uint(doc, value));
}

void addStr(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const std::string& value) {
    yyjson_mut_obj_add(obj, yyjson_mut_str(doc, key), yyjson_mut_strncpy(doc, value.data(), value.size()));
}

yyjson_mut_val* seriesArray(yyjson_mut_doc* doc, const std::vector<std::uint32_t>& series) {
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    for (const std::uint32_t v : series) yyjson_mut_arr_add_uint(doc, arr, v);
    return arr;
}

yyjson_mut_val* opcodeObject(yyjson_mut_doc* doc, const std::map<std::uint32_t, std::uint32_t>& opcodes,
                             std::string (*name)(std::uint32_t)) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    for (const auto& [opcode, count] : opcodes) addUint(doc, obj, name(opcode), count);
    return obj;
}

}  // namespace

std::string RsxArgCensus::formatJson() {
    std::lock_guard lock(mutex_);
    return formatJsonLocked();
}

std::string RsxArgCensus::formatJsonLocked() {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* summary = yyjson_mut_obj_add_obj(doc, root, "summary");
    std::uint64_t writes = 0;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> classes;  // writes, distinct registers
    for (const auto& [method, stat] : methods_) {
        writes += stat.writes;
        auto& entry = classes[methodClass(method)];
        entry.first += stat.writes;
        entry.second++;
    }
    addUint(doc, summary, "register_writes", writes);
    addUint(doc, summary, "draws", draws_);
    addUint(doc, summary, "flips", flips_);
    addUint(doc, summary, "vblanks", vblanks_);
    addUint(doc, summary, "unreadable_fragment_programs", unreadable_fragment_programs_);
    addUint(doc, summary, "unterminated_vertex_programs", unterminated_vertex_programs_);

    yyjson_mut_val* class_obj = yyjson_mut_obj_add_obj(doc, root, "classes");
    for (const auto& [name, entry] : classes) {
        yyjson_mut_val* c = yyjson_mut_obj(doc);
        addUint(doc, c, "writes", entry.first);
        addUint(doc, c, "registers", entry.second);
        yyjson_mut_obj_add(class_obj, yyjson_mut_strncpy(doc, name.data(), name.size()), c);
    }

    std::vector<std::uint32_t> order;
    order.reserve(methods_.size());
    for (const auto& entry : methods_) order.push_back(entry.first);
    std::ranges::sort(order);
    yyjson_mut_val* registers = yyjson_mut_obj_add_arr(doc, root, "registers");
    for (const std::uint32_t method : order) {
        const MethodStat& stat = methods_.at(method);
        yyjson_mut_val* r = yyjson_mut_arr_add_obj(doc, registers);
        addStr(doc, r, "method", std::format("{:#06x}", method));
        addStr(doc, r, "name", rsxMethodName(method));
        addUint(doc, r, "writes", stat.writes);
        if (countOnly(method)) continue;
        addUint(doc, r, "distinct", stat.values.counts.size());
        addUint(doc, r, "overflow_writes", stat.values.overflow);
        std::vector<std::pair<std::uint32_t, std::uint64_t>> values(stat.values.counts.begin(),
                                                                    stat.values.counts.end());
        std::ranges::sort(values, [](const auto& a, const auto& b) {
            return std::tie(b.second, a.first) < std::tie(a.second, b.first);
        });
        yyjson_mut_val* list = yyjson_mut_obj_add_arr(doc, r, "values");
        for (std::size_t i = 0; i < values.size() && i < kValueListed; i++) {
            yyjson_mut_val* v = yyjson_mut_arr_add_obj(doc, list);
            addStr(doc, v, "value", std::format("{:#010x}", values[i].first));
            addUint(doc, v, "count", values[i].second);
            const std::string decoded = rsxDecodeArgument(method, values[i].first);
            if (!decoded.empty()) addStr(doc, v, "decoded", decoded);
        }
    }

    yyjson_mut_val* text = yyjson_mut_obj_add_obj(doc, root, "histograms");
    for (const auto& [category, bucket] : text_) {
        std::vector<std::pair<std::string, std::uint64_t>> entries(bucket.begin(), bucket.end());
        std::ranges::stable_sort(entries, [](const auto& a, const auto& b) { return a.second > b.second; });
        yyjson_mut_val* list = yyjson_mut_arr(doc);
        for (const auto& [key, count] : entries) {
            yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, list);
            addStr(doc, e, "key", key);
            addUint(doc, e, "count", count);
        }
        yyjson_mut_obj_add(text, yyjson_mut_strncpy(doc, category.data(), category.size()), list);
    }

    yyjson_mut_val* fps = yyjson_mut_obj_add_arr(doc, root, "fragment_programs");
    for (const auto& [hash, program] : fragment_programs_) {
        yyjson_mut_val* p = yyjson_mut_arr_add_obj(doc, fps);
        addStr(doc, p, "hash", hashName(hash));
        addStr(doc, p, "file", "fp_" + hashName(hash) + ".bin");
        addStr(doc, p, "location", program.location == 0 ? "local" : "main");
        addStr(doc, p, "offset", std::format("{:#x}", program.offset));
        addUint(doc, p, "instructions", program.scan.instructions);
        addUint(doc, p, "inline_constants", program.scan.constant_words);
        addUint(doc, p, "size_bytes", program.bytes.size());
        addUint(doc, p, "uses", program.uses);
        yyjson_mut_obj_add_bool(doc, p, "zero_filled", program.zero_filled);
        yyjson_mut_obj_add(p, yyjson_mut_str(doc, "opcodes"),
                           opcodeObject(doc, program.scan.opcodes, &rsxFpOpcodeName));
    }
    yyjson_mut_val* vps = yyjson_mut_obj_add_arr(doc, root, "vertex_programs");
    for (const auto& [hash, program] : vertex_programs_) {
        yyjson_mut_val* p = yyjson_mut_arr_add_obj(doc, vps);
        addStr(doc, p, "hash", hashName(hash));
        addStr(doc, p, "file", "vp_" + hashName(hash) + ".bin");
        addUint(doc, p, "start", program.start);
        addUint(doc, p, "instructions", program.scan.instructions);
        addUint(doc, p, "size_bytes", program.bytes.size());
        addUint(doc, p, "uses", program.uses);
        yyjson_mut_obj_add_bool(doc, p, "terminated", program.scan.terminated);
        yyjson_mut_obj_add(p, yyjson_mut_str(doc, "vec_opcodes"),
                           opcodeObject(doc, program.scan.vec_opcodes, &rsxVpVecOpcodeName));
        yyjson_mut_obj_add(p, yyjson_mut_str(doc, "sca_opcodes"),
                           opcodeObject(doc, program.scan.sca_opcodes, &rsxVpScaOpcodeName));
    }

    yyjson_mut_val* frames = yyjson_mut_obj_add_obj(doc, root, "frames");
    yyjson_mut_obj_add(frames, yyjson_mut_str(doc, "flips_per_second"), seriesArray(doc, flips_per_second_));
    yyjson_mut_obj_add(frames, yyjson_mut_str(doc, "vblanks_per_second"), seriesArray(doc, vblanks_per_second_));
    yyjson_mut_obj_add(frames, yyjson_mut_str(doc, "draws_per_second"), seriesArray(doc, draws_per_second_));
    yyjson_mut_val* per_frame = yyjson_mut_obj_add_arr(doc, frames, "draws_per_frame");
    for (const auto& [count, frames_with] : draws_per_frame_) {
        yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, per_frame);
        addUint(doc, e, "draws", count);
        addUint(doc, e, "frames", frames_with);
    }

    size_t length = 0;
    char* text_out = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &length);
    std::string result = text_out != nullptr ? std::string(text_out, length) : std::string();
    std::free(text_out);
    yyjson_mut_doc_free(doc);
    return result;
}

bool RsxArgCensus::writeFiles() {
    // The RSX thread may be the one that stopped the process while holding the lock; do not wait for it forever.
    std::unique_lock lock(mutex_, std::defer_lock);
    bool locked = false;
    for (int attempt = 0; attempt < 50 && !locked; attempt++) {
        locked = lock.try_lock();
        if (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!locked) {
        logError("tox2: rsx census: the RSX thread holds the census, nothing written");
        return false;
    }
    const std::string json = formatJsonLocked();

    std::error_code ec;
    if (!json_path_.parent_path().empty()) std::filesystem::create_directories(json_path_.parent_path(), ec);
    std::filesystem::create_directories(dump_dir_, ec);
    bool ok = true;
    const auto dump = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(dump_dir_ / name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ok = ok && out.good();
    };
    for (const auto& [hash, program] : fragment_programs_) dump("fp_" + hashName(hash) + ".bin", program.bytes);
    for (const auto& [hash, program] : vertex_programs_) dump("vp_" + hashName(hash) + ".bin", program.bytes);
    {
        std::ofstream out(json_path_, std::ios::binary | std::ios::trunc);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        ok = ok && out.good();
    }
    if (ok) {
        logInfo("tox2: rsx census: wrote {} ({} fragment and {} vertex programs in {})", json_path_.string(),
                fragment_programs_.size(), vertex_programs_.size(), dump_dir_.string());
    } else {
        logError("tox2: rsx census: could not write {}", json_path_.string());
    }
    return ok;
}

}  // namespace tox2

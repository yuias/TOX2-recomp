#pragma once

// Guest vertex attribute and index conversion. Guest arrays are big-endian; the host reads them as
// native little-endian vertex buffers. Element sizes follow RPCS3 `Emu/RSX/RSXThread.cpp`
// `get_vertex_type_size_on_host` (a three-component 16-bit or 8-bit attribute is padded to four
// components). Byte order follows RPCS3 `VK/VKVertexBuffers.cpp` and `Core/RSXDrawCommands.cpp`
// (`draw_command_processor::fill_vertex_layout_state`): data is passed in guest order with the swap flag
// set, so every 16-bit and 32-bit component is byte-swapped and 8-bit components keep their stored
// order (x is the first byte). Base type names follow RPCS3 `gcm_enums.h` `vertex_base_type`.
// Pure data conversion; Vulkan only supplies the format enumerators.

#include <cstdint>
#include <optional>
#include <span>

#include <volk.h>

namespace dal {

// Vertex base types of SET_VERTEX_DATA_ARRAY_FORMAT bits 0..2.
inline constexpr std::uint8_t kVertexTypeSnorm16 = 1;
inline constexpr std::uint8_t kVertexTypeFloat = 2;
inline constexpr std::uint8_t kVertexTypeHalf = 3;
inline constexpr std::uint8_t kVertexTypeUnorm8 = 4;
inline constexpr std::uint8_t kVertexTypeSint16 = 5;
inline constexpr std::uint8_t kVertexTypeCmp = 6;
inline constexpr std::uint8_t kVertexTypeUint8 = 7;

struct VertexAttribFormat {
    std::uint8_t type = 0;
    std::uint8_t size = 0;                  // components in the guest array, 1..4
    VkFormat format = VK_FORMAT_UNDEFINED;  // as the vertex shader reads the converted data
    std::uint32_t guest_size = 0;           // bytes per guest element
    std::uint32_t host_size = 0;            // bytes per converted element
    bool widen = false;                     // components are converted to 32-bit floats, see widenedFormat
};

// Nullopt for a type or size that has no conversion (compressed, type 0, ub256 other than 4 components).
std::optional<VertexAttribFormat> vertexAttribFormat(std::uint8_t type, std::uint8_t size);

// The same attribute converted to four 32-bit floats per element (R32G32B32A32_SFLOAT, missing components
// 0, 0, 0, 1) for the types whose scaled-integer formats a device may not offer as vertex formats (s32k and
// ub256). Other types are returned unchanged.
VertexAttribFormat widenedFormat(const VertexAttribFormat& format);

// Converts up to `count` elements, element k read at src[k * stride] (stride 0: one element shared by
// every vertex, only element 0 is converted). The output is tightly packed at host_size per element, so
// `dst` must hold count * host_size bytes. A padded fourth component is 1.0. Returns the number of
// elements written, fewer than requested when `src` ends first.
std::uint32_t convertVertexElements(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
                                    std::uint32_t stride, std::uint32_t count, std::span<std::uint8_t> dst);

// Guest elements that fit in `bytes` bytes: (bytes - element) / stride + 1, or 0 when not even one
// element fits. Stride 0 gives 1 (or 0).
std::uint32_t vertexElementsAvailable(const VertexAttribFormat& format, std::uint32_t stride, std::size_t bytes);

// Big-endian u16 or u32 indices to native order; both spans hold count indices.
void convertIndices(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::span<std::uint8_t> dst);

// Smallest and largest index. False when count is 0 or src is shorter than count indices.
bool indexRange(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::uint32_t& min_index,
                std::uint32_t& max_index);

// True when src holds count indices and each one lies in [min_index, max_index] or, with restart set, equals
// restart_index.
bool indicesInRange(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::uint32_t min_index,
                    std::uint32_t max_index, bool restart, std::uint32_t restart_index);

}  // namespace dal

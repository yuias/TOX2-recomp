#include "render/vertex_convert.h"

#include <algorithm>
#include <cstring>

namespace dal {
namespace {

std::uint32_t componentBytes(std::uint8_t type) {
    switch (type) {
        case kVertexTypeFloat: return 4;
        case kVertexTypeSnorm16:
        case kVertexTypeHalf:
        case kVertexTypeSint16: return 2;
        default: return 1;
    }
}

// Value of a padded fourth component, in the converted format's own encoding: 1.0 for the float and
// normalized formats and 1 for the scaled one.
std::uint32_t paddedWComponent(std::uint8_t type) {
    switch (type) {
        case kVertexTypeHalf: return 0x3C00;
        case kVertexTypeSnorm16: return 0x7FFF;
        case kVertexTypeSint16: return 1;
        default: return 0xFF;  // unorm8
    }
}

VkFormat pickFormat(std::uint8_t type, std::uint8_t size) {
    switch (type) {
        case kVertexTypeFloat: {
            constexpr VkFormat kFormats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                             VK_FORMAT_R32G32B32A32_SFLOAT};
            return kFormats[size - 1];
        }
        case kVertexTypeHalf: {
            constexpr VkFormat kFormats[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                                             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
            return kFormats[size - 1];
        }
        case kVertexTypeSnorm16: {
            constexpr VkFormat kFormats[] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM,
                                             VK_FORMAT_R16G16B16A16_SNORM};
            return kFormats[size - 1];
        }
        case kVertexTypeSint16: {
            constexpr VkFormat kFormats[] = {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                             VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
            return kFormats[size - 1];
        }
        case kVertexTypeUnorm8: {
            constexpr VkFormat kFormats[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                             VK_FORMAT_R8G8B8A8_UNORM};
            return kFormats[size - 1];
        }
        case kVertexTypeUint8: return VK_FORMAT_R8G8B8A8_USCALED;
        default: return VK_FORMAT_UNDEFINED;
    }
}

}  // namespace

std::optional<VertexAttribFormat> vertexAttribFormat(std::uint8_t type, std::uint8_t size) {
    if (size < 1 || size > 4) return std::nullopt;
    if (type == kVertexTypeUint8 && size != 4) return std::nullopt;  // RPCS3 requires four components
    const VkFormat format = pickFormat(type, size);
    if (format == VK_FORMAT_UNDEFINED) return std::nullopt;

    VertexAttribFormat out;
    out.type = type;
    out.size = size;
    out.format = format;
    out.guest_size = componentBytes(type) * size;
    out.host_size = componentBytes(type) * (size == 3 && type != kVertexTypeFloat ? 4 : size);
    return out;
}

VertexAttribFormat widenedFormat(const VertexAttribFormat& format) {
    if (format.type != kVertexTypeSint16 && format.type != kVertexTypeUint8) return format;
    VertexAttribFormat out = format;
    out.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    out.host_size = 16;
    out.widen = true;
    return out;
}

namespace {

// Signed 16-bit (big-endian) or unsigned 8-bit components as floats; the components the attribute lacks are
// 0, 0, 0, 1.
std::uint32_t widenVertexElements(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
                                  std::uint32_t stride, std::uint32_t n, std::span<std::uint8_t> dst) {
    const std::uint32_t component = componentBytes(format.type);
    std::uint32_t written = 0;
    for (std::uint32_t k = 0; k < n; k++) {
        const std::uint64_t offset = static_cast<std::uint64_t>(k) * stride;
        if (offset + format.guest_size > src.size()) break;
        const std::uint8_t* in = src.data() + offset;
        float values[4];
        for (std::uint32_t c = 0; c < 4; c++) {
            if (c >= format.size) {
                values[c] = c == 3 ? 1.0f : 0.0f;
            } else if (component == 2) {
                values[c] = static_cast<float>(
                        static_cast<std::int16_t>(static_cast<std::uint16_t>(in[c * 2] << 8 | in[c * 2 + 1])));
            } else {
                values[c] = static_cast<float>(in[c]);
            }
        }
        std::memcpy(dst.data() + static_cast<std::size_t>(k) * 16, values, 16);
        written++;
    }
    return written;
}

}  // namespace

std::uint32_t convertVertexElements(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
                                    std::uint32_t stride, std::uint32_t count, std::span<std::uint8_t> dst) {
    if (format.widen)
        return widenVertexElements(format, src, stride, stride == 0 ? std::min<std::uint32_t>(count, 1) : count, dst);
    const std::uint32_t component = componentBytes(format.type);
    const std::uint32_t out_components = format.host_size / component;
    const std::uint32_t n = stride == 0 ? std::min<std::uint32_t>(count, 1) : count;
    std::uint32_t written = 0;
    for (std::uint32_t k = 0; k < n; k++) {
        const std::uint64_t offset = static_cast<std::uint64_t>(k) * stride;
        if (offset + format.guest_size > src.size()) break;
        const std::uint8_t* in = src.data() + offset;
        std::uint8_t* out = dst.data() + static_cast<std::size_t>(k) * format.host_size;
        for (std::uint32_t c = 0; c < out_components; c++) {
            std::uint8_t* o = out + c * component;
            if (c >= format.size) {
                const std::uint32_t value = paddedWComponent(format.type);
                if (component == 2) {
                    o[0] = static_cast<std::uint8_t>(value);
                    o[1] = static_cast<std::uint8_t>(value >> 8);
                } else {
                    o[0] = static_cast<std::uint8_t>(value);
                }
                continue;
            }
            const std::uint8_t* i = in + c * component;
            if (component == 4) {
                o[0] = i[3];
                o[1] = i[2];
                o[2] = i[1];
                o[3] = i[0];
            } else if (component == 2) {
                o[0] = i[1];
                o[1] = i[0];
            } else {
                o[0] = i[0];
            }
        }
        written++;
    }
    return written;
}

std::uint32_t vertexElementsAvailable(const VertexAttribFormat& format, std::uint32_t stride, std::size_t bytes) {
    if (bytes < format.guest_size) return 0;
    if (stride == 0) return 1;
    return static_cast<std::uint32_t>(std::min<std::size_t>((bytes - format.guest_size) / stride + 1, 0xFFFFFFFFu));
}

void convertIndices(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::span<std::uint8_t> dst) {
    const std::uint32_t size = is_u16 ? 2 : 4;
    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint8_t* in = src.data() + static_cast<std::size_t>(i) * size;
        std::uint8_t* out = dst.data() + static_cast<std::size_t>(i) * size;
        for (std::uint32_t b = 0; b < size; b++) out[b] = in[size - 1 - b];
    }
}

bool indexRange(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::uint32_t& min_index,
                std::uint32_t& max_index) {
    const std::uint32_t size = is_u16 ? 2 : 4;
    if (count == 0 || src.size() < static_cast<std::size_t>(count) * size) return false;
    min_index = 0xFFFFFFFF;
    max_index = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint8_t* p = src.data() + static_cast<std::size_t>(i) * size;
        const std::uint32_t index =
                is_u16 ? (static_cast<std::uint32_t>(p[0]) << 8 | p[1])
                       : (static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
                          static_cast<std::uint32_t>(p[2]) << 8 | p[3]);
        min_index = std::min(min_index, index);
        max_index = std::max(max_index, index);
    }
    return true;
}

bool indicesInRange(bool is_u16, std::span<const std::uint8_t> src, std::uint32_t count, std::uint32_t min_index,
                    std::uint32_t max_index, bool restart, std::uint32_t restart_index) {
    const std::uint32_t size = is_u16 ? 2 : 4;
    if (src.size() < static_cast<std::size_t>(count) * size) return false;
    for (std::uint32_t i = 0; i < count; i++) {
        const std::uint8_t* p = src.data() + static_cast<std::size_t>(i) * size;
        const std::uint32_t index =
                is_u16 ? (static_cast<std::uint32_t>(p[0]) << 8 | p[1])
                       : (static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
                          static_cast<std::uint32_t>(p[2]) << 8 | p[3]);
        if (restart && index == restart_index) continue;
        if (index < min_index || index > max_index) return false;
    }
    return true;
}

}  // namespace dal

#include "render/vertex_convert.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>

namespace tox2 {
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

static_assert(std::endian::native == std::endian::little, "converted vertex data is written in host byte order");

// Signed 16-bit (big-endian) or unsigned 8-bit components as floats; the components the attribute lacks are
// 0, 0, 0, 1. Component-at-a-time loop for formats the specialised loops below do not cover.
std::uint32_t widenVertexElementsGeneric(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
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

// Byte-at-a-time loop for formats whose sizes the specialised loops below do not cover (none that
// vertexAttribFormat returns).
std::uint32_t convertVertexElementsGeneric(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
                                           std::uint32_t stride, std::uint32_t n, std::span<std::uint8_t> dst) {
    const std::uint32_t component = componentBytes(format.type);
    const std::uint32_t out_components = format.host_size / component;
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

// Elements, at most n, that fit in a source of src_bytes. Element k ends at k * stride + guest_bytes, so the ones
// that fit are a prefix and the loops need no per-element bounds check.
std::uint32_t fittingElements(std::size_t src_bytes, std::uint32_t guest_bytes, std::uint32_t stride, std::uint32_t n) {
    if (src_bytes < guest_bytes) return 0;
    if (stride == 0) return n;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(n, (src_bytes - guest_bytes) / stride + 1));
}

template <typename T>
T loadComponent(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

template <typename T>
void storeComponent(std::uint8_t* p, T value) {
    std::memcpy(p, &value, sizeof(T));
}

// Components of Component bytes: Guest of them in the guest element, Host in the converted one (Guest or, for a
// three-component 8-bit or 16-bit type, 4 with Pad as the last). Components wider than a byte are byte-swapped.
// The converted element is built in a local buffer and stored whole: the upload memory is write-combined and
// single-byte writes to it are slow.
template <std::uint32_t Component, std::uint32_t Guest, std::uint32_t Host, std::uint32_t Pad>
std::uint32_t convertPacked(std::span<const std::uint8_t> src, std::uint32_t stride, std::uint32_t n,
                            std::span<std::uint8_t> dst) {
    using T = std::conditional_t<Component == 4, std::uint32_t,
                                 std::conditional_t<Component == 2, std::uint16_t, std::uint8_t>>;
    constexpr std::uint32_t kGuestBytes = Component * Guest;
    constexpr std::uint32_t kHostBytes = Component * Host;
    const std::uint32_t m = fittingElements(src.size(), kGuestBytes, stride, n);
    const std::uint8_t* in = src.data();
    std::uint8_t* out = dst.data();

    if constexpr (Host == Guest) {
        // Tightly packed elements without padding are one run of components.
        if (stride == kGuestBytes) {
            if constexpr (Component == 1) {
                if (m != 0) std::memcpy(out, in, static_cast<std::size_t>(m) * kGuestBytes);
            } else {
                const std::size_t total = static_cast<std::size_t>(m) * Guest;
                for (std::size_t i = 0; i < total; i++) {
                    storeComponent<T>(out + i * sizeof(T), std::byteswap(loadComponent<T>(in + i * sizeof(T))));
                }
            }
            return m;
        }
    }

    for (std::uint32_t k = 0; k < m; k++) {
        const std::uint8_t* element = in + static_cast<std::size_t>(k) * stride;
        T values[Host];
        for (std::uint32_t c = 0; c < Guest; c++) {
            const T raw = loadComponent<T>(element + c * sizeof(T));
            values[c] = Component == 1 ? raw : std::byteswap(raw);
        }
        for (std::uint32_t c = Guest; c < Host; c++) values[c] = static_cast<T>(Pad);
        std::memcpy(out + static_cast<std::size_t>(k) * kHostBytes, values, kHostBytes);
    }
    return m;
}

using ConvertFn = std::uint32_t (*)(std::span<const std::uint8_t>, std::uint32_t, std::uint32_t,
                                    std::span<std::uint8_t>);

template <std::uint32_t Component>
ConvertFn unpaddedBySize(std::uint32_t size) {
    switch (size) {
        case 1: return &convertPacked<Component, 1, 1, 0>;
        case 2: return &convertPacked<Component, 2, 2, 0>;
        case 3: return &convertPacked<Component, 3, 3, 0>;
        case 4: return &convertPacked<Component, 4, 4, 0>;
        default: return nullptr;
    }
}

// The specialised loop for a format whose sizes follow vertexAttribFormat, or null.
ConvertFn selectPacked(const VertexAttribFormat& format) {
    const std::uint32_t component = componentBytes(format.type);
    if (format.size < 1 || format.size > 4 || format.guest_size != component * format.size ||
        format.host_size % component != 0) {
        return nullptr;
    }
    const std::uint32_t host = format.host_size / component;
    if (host == format.size) {
        switch (component) {
            case 4: return unpaddedBySize<4>(format.size);
            case 2: return unpaddedBySize<2>(format.size);
            default: return unpaddedBySize<1>(format.size);
        }
    }
    // Only a three-component 8-bit or 16-bit attribute is padded, to four components.
    if (format.size != 3 || host != 4) return nullptr;
    const std::uint32_t pad = paddedWComponent(format.type);
    if (component == 2) {
        switch (pad) {
            case 0x3C00: return &convertPacked<2, 3, 4, 0x3C00>;
            case 0x7FFF: return &convertPacked<2, 3, 4, 0x7FFF>;
            case 1: return &convertPacked<2, 3, 4, 1>;
            default: return nullptr;
        }
    }
    return component == 1 && pad == 0xFF ? &convertPacked<1, 3, 4, 0xFF> : nullptr;
}

// Signed 16-bit (Component 2) or unsigned 8-bit (Component 1) components as floats, Guest of them stored and
// the rest 0, 0, 0, 1.
template <std::uint32_t Component, std::uint32_t Guest>
std::uint32_t widenPacked(std::span<const std::uint8_t> src, std::uint32_t stride, std::uint32_t n,
                          std::span<std::uint8_t> dst) {
    using T = std::conditional_t<Component == 2, std::int16_t, std::uint8_t>;
    const std::uint32_t m = fittingElements(src.size(), Component * Guest, stride, n);
    for (std::uint32_t k = 0; k < m; k++) {
        const std::uint8_t* element = src.data() + static_cast<std::size_t>(k) * stride;
        float values[4];
        for (std::uint32_t c = 0; c < 4; c++) {
            if (c < Guest) {
                T raw = loadComponent<T>(element + c * sizeof(T));
                if constexpr (Component == 2) raw = std::byteswap(raw);
                values[c] = static_cast<float>(raw);
            } else {
                values[c] = c == 3 ? 1.0f : 0.0f;
            }
        }
        std::memcpy(dst.data() + static_cast<std::size_t>(k) * 16, values, 16);
    }
    return m;
}

template <std::uint32_t Component>
ConvertFn widenBySize(std::uint32_t size) {
    switch (size) {
        case 1: return &widenPacked<Component, 1>;
        case 2: return &widenPacked<Component, 2>;
        case 3: return &widenPacked<Component, 3>;
        case 4: return &widenPacked<Component, 4>;
        default: return nullptr;
    }
}

ConvertFn selectWiden(const VertexAttribFormat& format) {
    const std::uint32_t component = componentBytes(format.type);
    if (format.size < 1 || format.size > 4 || format.guest_size != component * format.size) return nullptr;
    if (component == 2) return widenBySize<2>(format.size);
    return component == 1 ? widenBySize<1>(format.size) : nullptr;
}

}  // namespace

std::uint32_t convertVertexElements(const VertexAttribFormat& format, std::span<const std::uint8_t> src,
                                    std::uint32_t stride, std::uint32_t count, std::span<std::uint8_t> dst) {
    const std::uint32_t n = stride == 0 ? std::min<std::uint32_t>(count, 1) : count;
    if (format.widen) {
        if (const ConvertFn convert = selectWiden(format)) return convert(src, stride, n, dst);
        return widenVertexElementsGeneric(format, src, stride, n, dst);
    }
    if (const ConvertFn convert = selectPacked(format)) return convert(src, stride, n, dst);
    return convertVertexElementsGeneric(format, src, stride, n, dst);
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

}  // namespace tox2

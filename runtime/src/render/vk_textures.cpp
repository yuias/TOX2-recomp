#include "render/vk_textures.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>
#include <vector>

#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

constexpr VkDeviceSize kCacheBudgetBytes = 256ull << 20;
constexpr std::size_t kMaxScratchImages = 4;

// Callers pass UNDEFINED as the old layout before a copy that overwrites the whole image.
void imageBarrier(VkCommandBuffer cmd, VkImage image, std::uint32_t levels, std::uint32_t layers, VkImageLayout from,
                  VkImageLayout to, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    // Everything recorded or submitted earlier may use the image (an earlier draw sampling it, an earlier
    // frame still in flight), so the source scope is the widest one.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkSamplerAddressMode wrapMode(std::uint32_t gcm) {
    switch (gcm) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3:
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6:
        case 7:
        case 8:
            // RPCS3 maps these to MIRROR_CLAMP_TO_EDGE, a feature the device is created without.
            rsxNoteUnimplemented("texture sampler", "mirror once wrap");
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default:
            rsxNoteUnimplemented("texture sampler", std::format("wrap {:#x}", gcm));
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

}  // namespace

// Facts from RPCS3: `Emu/RSX/Utils/color_utils.hpp` `decode_remap_encoding` (channel and control tables)
// and `texture_channel_remap_t::remap` (a control of 2 reads the channel, 0 forces zero, 1 forces one),
// `Emu/RSX/VK/VKFormats.cpp` `get_component_mapping` (A8R8G8B8 base order A, R, G, B) and
// `Emu/RSX/VK/vkutils/image_helpers.cpp` `apply_swizzle_remap` (the mapping is {final[1], final[2],
// final[3], final[0]}). The images are B8G8R8A8, so the base channels read as they are.
VkComponentMapping textureComponentMapping(std::uint32_t remap) {
    constexpr VkComponentSwizzle kBase[4] = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                             VK_COMPONENT_SWIZZLE_B};
    VkComponentSwizzle final_map[4];
    for (std::uint32_t channel = 0; channel < 4; channel++) {
        const std::uint32_t from = (remap >> (channel * 2)) & 3;
        const std::uint32_t control = (remap >> (8 + channel * 2)) & 3;
        final_map[channel] = control == 0   ? VK_COMPONENT_SWIZZLE_ZERO
                             : control == 1 ? VK_COMPONENT_SWIZZLE_ONE
                                            : kBase[from];
    }
    return {final_map[1], final_map[2], final_map[3], final_map[0]};
}

// RPCS3 `Emu/RSX/RSXTexture.cpp` `fragment_texture::remapped_border_color` passes the border colour
// through the texture's remap (an identity word leaves it unchanged); the channels are A, R, G, B from the
// top byte down, as in `texture_channel_remap_t::remap`.
std::uint32_t remapBorderColor(std::uint32_t border, std::uint32_t remap) {
    const std::uint32_t channels[4] = {border >> 24, (border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF};
    std::uint32_t out[4];
    for (std::uint32_t channel = 0; channel < 4; channel++) {
        const std::uint32_t from = (remap >> (channel * 2)) & 3;
        const std::uint32_t control = (remap >> (8 + channel * 2)) & 3;
        out[channel] = control == 0 ? 0 : control == 1 ? 0xFF : channels[from];
    }
    return (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3];
}

// Facts from RPCS3: `Emu/RSX/VK/VKFormats.cpp` `get_min_filter` (filter, mipmap mode and whether the
// filter samples mip levels), `get_mag_filter`, `vk_wrap_mode`, `get_border_color`,
// `Emu/RSX/VK/vkutils/sampler.cpp` `get_closest_border_color_enum` (the substitute for a border colour
// without the custom border colour extension), and `Emu/RSX/VK/VKHelpers.cpp` `update_fragment_sampler`
// (level range and bias are used only for a mip-sampling filter on a texture with several levels; the
// range ends at the last level and a NEAREST mip mode rounds the bias to half a level) with
// `Emu/RSX/RSXTexture.cpp` `min_lod`, `max_lod`, `bias` (unsigned 4.8 fixed point fields of
// SET_TEXTURE_CONTROL0, signed 5.8 in SET_TEXTURE_FILTER). Enum values from `Emu/RSX/gcm_enums.h`; the
// field positions are in rsx_decode.h.
SamplerSettings samplerSettings(const RsxTextureDesc& texture) {
    SamplerSettings settings;

    const std::uint32_t min_filter = rsxTextureMinFilter(texture.filter);
    bool sample_mips = false;
    switch (min_filter) {
        case 3:
        case 5: sample_mips = true; [[fallthrough]];
        case 1: settings.min_filter = VK_FILTER_NEAREST; break;
        case 4:
        case 6: sample_mips = true; [[fallthrough]];
        case 2:
        case 7: settings.min_filter = VK_FILTER_LINEAR; break;
        default: rsxNoteUnimplemented("texture sampler", std::format("min filter {:#x}", min_filter)); break;
    }
    const std::uint32_t levels = rsxTextureMipCount(texture.format, texture.width, texture.height, texture.mips);
    if (sample_mips && levels > 1) {
        const float last_level = static_cast<float>(levels - 1);
        settings.mipmap_mode = min_filter >= 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        settings.min_lod = std::min(static_cast<float>((texture.control0 >> 19) & 0xFFF) / 256.0f, last_level);
        settings.max_lod = std::min(static_cast<float>((texture.control0 >> 7) & 0xFFF) / 256.0f, last_level);
        settings.max_lod = std::max(settings.max_lod, settings.min_lod);
        std::uint32_t bias_bits = texture.filter & 0x1FFF;
        float bias = static_cast<float>(bias_bits) / 256.0f;
        if ((bias_bits & 0x1000) != 0) bias = -static_cast<float>((0u - bias_bits) & 0x1FFF) / 256.0f;
        if (settings.mipmap_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST) bias = std::floor(bias * 2.0f + 0.5f) * 0.5f;
        settings.lod_bias = bias;
    }
    const std::uint32_t mag_filter = rsxTextureMagFilter(texture.filter);
    switch (mag_filter) {
        case 1: settings.mag_filter = VK_FILTER_NEAREST; break;
        case 2:
        case 4: settings.mag_filter = VK_FILTER_LINEAR; break;
        default: rsxNoteUnimplemented("texture sampler", std::format("mag filter {:#x}", mag_filter)); break;
    }

    settings.wrap_s = wrapMode(rsxTextureWrapS(texture.address));
    settings.wrap_t = wrapMode(rsxTextureWrapT(texture.address));
    settings.wrap_r = wrapMode((texture.address >> 16) & 0xF);

    const bool uses_border = settings.wrap_s == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             settings.wrap_t == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                             settings.wrap_r == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (uses_border) {
        const std::uint32_t border = remapBorderColor(texture.border, texture.remap);
        switch (border) {
            case 0x00000000: settings.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
            case 0xFFFFFFFF: settings.border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
            case 0xFF000000: settings.border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
            default: {
                rsxNoteUnimplemented("texture sampler", "custom border color");
                const float r = static_cast<float>((border >> 16) & 0xFF) / 255.0f;
                const float g = static_cast<float>((border >> 8) & 0xFF) / 255.0f;
                const float b = static_cast<float>(border & 0xFF) / 255.0f;
                const float a = static_cast<float>(border >> 24) / 255.0f;
                settings.border = (r + g + b) > 1.35f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                  : a > 0.5f          ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                      : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                break;
            }
        }
    }
    return settings;
}

TextureImage::~TextureImage() {
    if (context == nullptr) return;
    for (const auto& entry : views) vkDestroyImageView(context->device(), entry.second, nullptr);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
}

TextureCache::TextureCache(VkContext& context) : context_(context) {}

TextureCache::~TextureCache() {
    images_.clear();
    scratch_.clear();
    for (const auto& entry : samplers_) vkDestroySampler(context_.device(), entry.second, nullptr);
}

void TextureCache::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

VkFormat textureVkFormat(std::uint32_t rsx_format) {
    switch (rsx_format & kRsxTextureBaseMask) {
        case kRsxTextureA8R8G8B8: return VK_FORMAT_B8G8R8A8_UNORM;
        case kRsxTextureDxt1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case kRsxTextureDxt23: return VK_FORMAT_BC2_UNORM_BLOCK;
        case kRsxTextureDxt45: return VK_FORMAT_BC3_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool TextureCache::formatUsable(VkFormat format) {
    const auto it = format_usable_.find(format);
    if (it != format_usable_.end()) return it->second;
    bool usable = true;
    if (format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || format == VK_FORMAT_BC2_UNORM_BLOCK ||
        format == VK_FORMAT_BC3_UNORM_BLOCK) {
        // The feature has to be enabled on the device, not only supported by it.
        usable = context_.hasTextureBC();
        if (usable) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
            constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            usable = (props.optimalTilingFeatures & kNeeded) == kNeeded;
        }
    }
    format_usable_[format] = usable;
    return usable;
}

std::shared_ptr<TextureImage> TextureCache::create(std::uint32_t width, std::uint32_t height, VkFormat format,
                                                   VkImageUsageFlags usage, std::uint32_t levels, std::uint32_t depth,
                                                   std::uint32_t layers, VkImageViewType view_type) {
    const VkPhysicalDeviceLimits& limits = context_.deviceProperties().limits;
    // A 3D image is limited by maxImageDimension3D on all three sides; a cube by maxImageDimensionCube.
    const std::uint32_t max_side = view_type == VK_IMAGE_VIEW_TYPE_3D     ? limits.maxImageDimension3D
                                   : view_type == VK_IMAGE_VIEW_TYPE_CUBE ? limits.maxImageDimensionCube
                                                                          : limits.maxImageDimension2D;
    if (width == 0 || height == 0 || width > max_side || height > max_side || depth > max_side ||
        layers > limits.maxImageArrayLayers) {
        rsxNoteUnimplemented("texture layout", view_type == VK_IMAGE_VIEW_TYPE_3D
                                                       ? std::format("size {}x{}x{}", width, height, depth)
                                                       : std::format("size {}x{}", width, height));
        return nullptr;
    }
    auto texture = std::make_shared<TextureImage>();
    texture->context = &context_;
    texture->format = format;
    texture->width = width;
    texture->height = height;
    texture->depth = depth;
    texture->layers = layers;
    texture->view_type = view_type;
    const std::uint32_t block_bytes = format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK                                     ? 8
                                      : format == VK_FORMAT_BC2_UNORM_BLOCK || format == VK_FORMAT_BC3_UNORM_BLOCK ? 16
                                                                                                                   : 0;
    texture->levels = levels;
    texture->bytes = 0;
    for (std::uint32_t level = 0, w = width, h = height, d = depth; level < levels;
         level++, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u), d = std::max(d / 2, 1u)) {
        texture->bytes += (block_bytes != 0 ? static_cast<VkDeviceSize>((w + 3) / 4) * ((h + 3) / 4) * block_bytes
                                            : static_cast<VkDeviceSize>(w) * h * 4) *
                          d * layers;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image_info.flags = view_type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    image_info.format = format;
    image_info.extent = {width, height, depth};
    image_info.mipLevels = levels;
    image_info.arrayLayers = layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(context_.allocator(), &image_info, &alloc_info, &texture->image, &texture->allocation,
                       nullptr) != VK_SUCCESS) {
        texture->image = VK_NULL_HANDLE;
        fail("vmaCreateImage failed for a texture");
        return nullptr;
    }
    return texture;
}

std::shared_ptr<TextureImage> TextureCache::acquire(const RsxTextureDesc& texture) {
    const VkFormat format = textureVkFormat(texture.format);
    if (format == VK_FORMAT_UNDEFINED) {
        rsxNoteUnimplemented("texture format", rsxTextureFormatName(texture.format));
        return nullptr;
    }
    if (!formatUsable(format)) {
        rsxNoteUnimplemented("texture format",
                             std::format("{} (device lacks block compression)", rsxTextureFormatName(texture.format)));
        return nullptr;
    }
    const RsxTextureKind kind = rsxTextureKind(texture);
    if (kind == RsxTextureKind::Unsupported) {
        rsxNoteUnimplemented("texture layout", std::format("dimension {}", texture.dimension));
        return nullptr;
    }
    if (kind == RsxTextureKind::Cube && texture.width != texture.height) {
        // Vulkan cube images have square faces.
        rsxNoteUnimplemented("texture layout", "cubemap that is not square");
        return nullptr;
    }
    const std::uint32_t levels = rsxTextureMipCount(texture.format, texture.width, texture.height, texture.mips);
    const std::uint32_t depth = kind == RsxTextureKind::Tex3D ? texture.depth : 1;
    const std::uint32_t layers = kind == RsxTextureKind::Cube ? 6 : 1;
    const VkImageViewType view_type = kind == RsxTextureKind::Cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                      : kind == RsxTextureKind::Tex3D ? VK_IMAGE_VIEW_TYPE_3D
                                                                      : VK_IMAGE_VIEW_TYPE_2D;
    const Key key{texture.location, texture.offset, texture.format,
                  texture.width,    texture.height, texture.pitch,
                  levels,           depth,          static_cast<std::uint32_t>(kind)};
    const auto it = images_.find(key);
    if (it != images_.end()) {
        it->second->last_used = ++tick_;
        return it->second;
    }
    std::shared_ptr<TextureImage> created =
            create(texture.width, texture.height, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   levels, depth, layers, view_type);
    if (!created) return nullptr;
    created->last_used = ++tick_;
    images_[key] = created;
    return created;
}

std::shared_ptr<TextureImage> TextureCache::scratch(std::uint32_t width, std::uint32_t height) {
    const auto key = std::make_pair(width, height);
    const auto it = scratch_.find(key);
    if (it != scratch_.end()) return it->second;
    std::shared_ptr<TextureImage> created = create(width, height, VK_FORMAT_B8G8R8A8_UNORM,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!created) return nullptr;
    if (scratch_.size() >= kMaxScratchImages) scratch_.clear();
    scratch_[key] = created;
    return created;
}

VkImageView TextureCache::createView(VkImage image, VkFormat format, std::uint32_t remap, std::uint32_t levels,
                                     VkImageViewType type, std::uint32_t layers) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = type;
    view_info.format = format;
    view_info.components = textureComponentMapping(remap);
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &view) != VK_SUCCESS) {
        fail("vkCreateImageView failed for a texture");
        return VK_NULL_HANDLE;
    }
    return view;
}

VkImageView TextureCache::view(TextureImage& image, std::uint32_t remap) {
    const std::uint32_t key = remap & 0xFFFF;
    const auto it = image.views.find(key);
    if (it != image.views.end()) return it->second;
    const VkImageView created = createView(image.image, image.format, key, image.levels, image.view_type, image.layers);
    if (created != VK_NULL_HANDLE) image.views[key] = created;
    return created;
}

VkSampler TextureCache::sampler(const RsxTextureDesc& texture) {
    const SamplerSettings settings = samplerSettings(texture);
    const auto it = samplers_.find(settings);
    if (it != samplers_.end()) return it->second;

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = settings.mag_filter;
    info.minFilter = settings.min_filter;
    info.addressModeU = settings.wrap_s;
    info.addressModeV = settings.wrap_t;
    info.addressModeW = settings.wrap_r;
    info.mipmapMode = settings.mipmap_mode;
    info.mipLodBias = std::clamp(settings.lod_bias, -context_.deviceProperties().limits.maxSamplerLodBias,
                                 context_.deviceProperties().limits.maxSamplerLodBias);
    info.minLod = settings.min_lod;
    info.maxLod = settings.max_lod;
    info.borderColor = settings.border;
    VkSampler created = VK_NULL_HANDLE;
    if (vkCreateSampler(context_.device(), &info, nullptr, &created) != VK_SUCCESS) {
        fail("vkCreateSampler failed");
        return VK_NULL_HANDLE;
    }
    samplers_[settings] = created;
    return created;
}

void TextureCache::recordUpload(VkCommandBuffer cmd, TextureImage& image, VkBuffer staging, VkDeviceSize offset,
                                std::span<const RsxTextureLevel> levels) {
    imageBarrier(cmd, image.image, image.levels, image.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(levels.size());
    for (const RsxTextureLevel& level : levels) {
        VkBufferImageCopy region{};
        region.bufferOffset = offset + level.packed_offset;  // rows are tightly packed: bufferRowLength stays 0
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level.level, level.layer, 1};
        region.imageExtent = {level.width, level.height, level.depth};
        regions.push_back(region);
    }
    vkCmdCopyBufferToImage(cmd, staging, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    imageBarrier(cmd, image.image, image.levels, image.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void TextureCache::recordCopy(VkCommandBuffer cmd, TextureImage& image, VkImage source) {
    imageBarrier(cmd, image.image, image.levels, image.layers, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {image.width, image.height, 1};
    vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);
    imageBarrier(cmd, image.image, image.levels, image.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void TextureCache::trim() {
    VkDeviceSize total = 0;
    for (const auto& entry : images_) total += entry.second->bytes;
    while (total > kCacheBudgetBytes && !images_.empty()) {
        auto oldest = images_.begin();
        for (auto it = images_.begin(); it != images_.end(); ++it) {
            if (it->second->last_used < oldest->second->last_used) oldest = it;
        }
        total -= oldest->second->bytes;
        images_.erase(oldest);
    }
}

}  // namespace tox2

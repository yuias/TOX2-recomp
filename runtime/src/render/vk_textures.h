#pragma once

// Sampled images of the RSX draw path: images for textures read from guest memory, the copy of a render
// target used when a draw samples the target it renders to, the samplers, and the mapping from the
// texture's channel remap to a Vulkan component mapping. Commands are recorded by the caller's command
// buffer, outside a rendering instance.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "render/vk_context.h"
#include "rsx/rsx_frame.h"

namespace tox2 {

// Sampled images are B8G8R8A8 for render targets and A8R8G8B8 guest textures (after a 32-bit byte swap
// of each texel on upload, guest bytes A, R, G, B become B, G, R, A), and BC1/BC2/BC3 for DXT1/DXT23/DXT45
// guest textures (block bytes uploaded as stored). Their channels read as the guest's R, G, B, A, and the
// identity remap gives an identity view (which keeps border colours defined).
//
// Component mapping of a view for the texture's SET_TEXTURE_CONTROL1 word: bits 0..7 select the source
// channel of A, R, G, B (two bits each: A = 0, R = 1, G = 2, B = 3) and bits 8..15 say whether the
// channel reads it (2), is forced to 1 or to 0. The identity word is 0xAAE4. Only the low 16 bits are used;
// the override bits of the high half matter for formats other than A8R8G8B8.
VkComponentMapping textureComponentMapping(std::uint32_t remap);

// Vulkan format of the image that holds a guest texture of RSX format `rsx_format` (flag bits ignored);
// UNDEFINED for a format that is not sampled.
VkFormat textureVkFormat(std::uint32_t rsx_format);

// The border colour word (ARGB) as the remap delivers it to the shader.
std::uint32_t remapBorderColor(std::uint32_t border, std::uint32_t remap);

struct SamplerSettings {
    VkFilter min_filter = VK_FILTER_NEAREST;
    VkFilter mag_filter = VK_FILTER_NEAREST;
    VkSamplerAddressMode wrap_s = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrap_t = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode wrap_r = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    auto operator<=>(const SamplerSettings&) const = default;
};

// Filter, address and border words of a texture unit to sampler state. Values outside the tables and
// features the device does not enable (mirror-once wrap, custom border colours) are counted with
// rsxNoteUnimplemented and replaced by the closest supported state.
SamplerSettings samplerSettings(const RsxTextureDesc& texture);

struct TextureImage {
    VkContext* context = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0, height = 0;
    VkDeviceSize bytes = 0;
    // Layout at the end of the commands recorded so far (the frame's command buffer is submitted in order).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint64_t last_used = 0;
    std::map<std::uint32_t, VkImageView> views;  // by remap word (low 16 bits)

    TextureImage() = default;
    TextureImage(const TextureImage&) = delete;
    TextureImage& operator=(const TextureImage&) = delete;
    ~TextureImage();
};

class TextureCache {
public:
    explicit TextureCache(VkContext& context);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Image for a texture whose bytes the draw carries, cached by (location, offset, format, width, height,
    // pitch). Null with a counted note when the texture cannot be sampled (format, size).
    std::shared_ptr<TextureImage> acquire(const RsxTextureDesc& texture);

    // Image the size of a render target that receives a copy of it. One per size.
    std::shared_ptr<TextureImage> scratch(std::uint32_t width, std::uint32_t height);

    // View with the component mapping of `remap`; null on a Vulkan failure.
    VkImageView view(TextureImage& image, std::uint32_t remap);
    // View of an image the cache does not own, with the same mapping rule.
    VkImageView createView(VkImage image, VkFormat format, std::uint32_t remap);

    VkSampler sampler(const RsxTextureDesc& texture);

    // Copies `row_length` texels per source row from the staging buffer into the whole image, which ends in
    // SHADER_READ_ONLY. The previous contents are discarded. The staging texels must already be byte
    // swapped (see above).
    void recordUpload(VkCommandBuffer cmd, TextureImage& image, VkBuffer staging, VkDeviceSize offset,
                      std::uint32_t row_length);
    // Copies a whole B8G8R8A8 image of the same size, which is in TRANSFER_SRC_OPTIMAL, into `image`; the
    // image ends in SHADER_READ_ONLY.
    void recordCopy(VkCommandBuffer cmd, TextureImage& image, VkImage source);

    // Drops the least recently used images while the cached bytes exceed the budget. Commands that already
    // use a dropped image keep it alive through their own references.
    void trim();

private:
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;

    std::shared_ptr<TextureImage> create(std::uint32_t width, std::uint32_t height, VkFormat format,
                                         VkImageUsageFlags usage);
    bool formatUsable(VkFormat format);
    void fail(std::string message);

    VkContext& context_;
    std::map<VkFormat, bool> format_usable_;
    std::string error_;
    std::uint64_t tick_ = 0;
    std::map<Key, std::shared_ptr<TextureImage>> images_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::shared_ptr<TextureImage>> scratch_;
    std::map<SamplerSettings, VkSampler> samplers_;
};

}  // namespace tox2

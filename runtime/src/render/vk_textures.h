#pragma once

// Sampled images of the RSX draw path: images for textures read from guest memory, the copy of a render
// target used when a draw samples the target it renders to, the samplers, and the mapping from the
// texture's channel remap to a Vulkan component mapping. Commands are recorded by the caller's command
// buffer, outside a rendering instance.

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>

#include "render/image_pool.h"
#include "render/vk_context.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_texture_layout.h"

namespace tox2 {

// Sampled images are B8G8R8A8 for render targets and A8R8G8B8 guest textures (after a 32-bit byte swap
// of each texel on upload, guest bytes A, R, G, B become B, G, R, A), and BC1/BC2/BC3 for DXT1/DXT23/DXT45
// guest textures (block bytes uploaded as stored). Their channels read as the guest's R, G, B, A, and the
// identity remap gives an identity view (which keeps border colours defined). A guest texture has its whole
// mip chain (rsxTextureMipCount levels); render targets and their copies have one level.
//
// A guest texture is a 2D image, a 3D image or a cube map (a 2D image of six layers with a cube view), as
// rsxTextureKind says. The cube faces are in the order +X, -X, +Y, -Y, +Z, -Z, which is both the RSX and the
// Vulkan layer order.
//
// Component mapping of a view for the texture's SET_TEXTURE_CONTROL1 word: bits 0..7 select the source
// channel of A, R, G, B (two bits each: A = 0, R = 1, G = 2, B = 3) and bits 8..15 say whether the
// channel reads it (2), is forced to 1 or to 0. The identity word is 0xAAE4. Only the low 16 bits are used;
// the override bits of the high half matter for formats other than A8R8G8B8.
VkComponentMapping textureComponentMapping(std::uint32_t remap);

// The same for a single-channel float image (X32_FLOAT, R32_SFLOAT): every source channel is the image's red
// channel, so only the forced zero/one controls of the remap matter (RPCS3 `VKFormats.cpp`
// `get_component_mapping`, X32_FLOAT: R, R, R, R; `RSXTexture.cpp` `decoded_remap` ignores the channel
// selects of float textures).
VkComponentMapping singleChannelComponentMapping(std::uint32_t remap);

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
    // The mip chain is sampled only for a mip-mapping min filter and a texture with more than one level;
    // otherwise the range is level 0 alone.
    VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    float min_lod = 0.0f, max_lod = 0.0f, lod_bias = 0.0f;

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
    std::uint32_t width = 0, height = 0, levels = 1;
    std::uint32_t depth = 1;                            // slices of a 3D image
    std::uint32_t layers = 1;                           // 6 for a cube map
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;  // 3D for a volume even when it has one slice
    VkDeviceSize bytes = 0;
    // Layout at the end of the commands recorded so far (the frame's command buffer is submitted in order).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint64_t last_used = 0;
    // RsxTextureDesc::content_id of the blob the image was last uploaded from; 0 = never (or not from a blob).
    std::uint64_t content_id = 0;
    std::map<std::uint32_t, VkImageView> views;  // by remap word (low 16 bits)
    // Set for images that go back to the pool instead of being destroyed. The last reference drops only after the
    // slots that used the image were waited, so the GPU is done with it by then.
    std::shared_ptr<ImagePool> pool;
    PooledImageKey pool_key{};

    TextureImage() = default;
    TextureImage(const TextureImage&) = delete;
    TextureImage& operator=(const TextureImage&) = delete;
    ~TextureImage();
};

class TextureCache {
public:
    // With a pool, renderable() images are taken from it and returned to it.
    explicit TextureCache(VkContext& context, std::shared_ptr<ImagePool> pool = nullptr);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Image for a texture whose bytes the draw carries, cached by (location, offset, format, width, height,
    // pitch, levels, depth, kind). Null with a counted note when the texture cannot be sampled (format, size,
    // a cube map that is not square).
    std::shared_ptr<TextureImage> acquire(const RsxTextureDesc& texture);

    // Image the size of a render target that receives a copy of it. One per size.
    std::shared_ptr<TextureImage> scratch(std::uint32_t width, std::uint32_t height);

    // A B8G8R8A8 image the renderer draws into and then samples (SAMPLED and COLOR_ATTACHMENT usage), owned by
    // the caller. Null with a counted note when the size is not supported. With a pool the image is reused from it
    // when one of the size is there; `*pooled` then says whether it was (without a pool: no hit, NoEntry). The
    // contents and layout of a reused image are undefined, as for a new one.
    std::shared_ptr<TextureImage> renderable(std::uint32_t width, std::uint32_t height,
                                             ImagePoolResult* pooled = nullptr);

    // Images created by acquire() and scratch() so far, and their total size in bytes; renderable() images are
    // the caller's to count.
    std::uint64_t imagesCreated() const { return images_created_; }
    std::uint64_t imageBytesCreated() const { return image_bytes_created_; }

    // View with the component mapping of `remap`; null on a Vulkan failure.
    VkImageView view(TextureImage& image, std::uint32_t remap);
    // View of an image the cache does not own, with the same mapping rule. An R32_SFLOAT image gets the
    // single-channel mapping.
    VkImageView createView(VkImage image, VkFormat format, std::uint32_t remap, std::uint32_t levels = 1,
                           VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D, std::uint32_t layers = 1);

    // `format` is the format of the image the sampler reads: a linear filter is replaced by NEAREST when the
    // device cannot filter that format linearly (R32_SFLOAT is such a format on some devices).
    VkSampler sampler(const RsxTextureDesc& texture, VkFormat format = VK_FORMAT_UNDEFINED);

    // Copies every level (and cube face) of the image from the staging buffer, which holds them tightly packed
    // at the offsets of `levels` (the packed layout of rsx_texture_layout.h) from `offset`. The image ends in
    // SHADER_READ_ONLY. The previous contents are discarded. The staging texels must already be byte
    // swapped (see above).
    void recordUpload(VkCommandBuffer cmd, TextureImage& image, VkBuffer staging, VkDeviceSize offset,
                      std::span<const RsxTextureLevel> levels);
    // Copies a whole B8G8R8A8 image of the same size, which is in TRANSFER_SRC_OPTIMAL, into `image`; the
    // image ends in SHADER_READ_ONLY.
    void recordCopy(VkCommandBuffer cmd, TextureImage& image, VkImage source);

    // Drops the least recently used images while the cached bytes exceed the budget. Commands that already
    // use a dropped image keep it alive through their own references.
    void trim();

private:
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t, std::uint32_t, std::uint32_t>;

    std::shared_ptr<TextureImage> create(std::uint32_t width, std::uint32_t height, VkFormat format,
                                         VkImageUsageFlags usage, std::uint32_t levels = 1, std::uint32_t depth = 1,
                                         std::uint32_t layers = 1, VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
                                         ImagePoolResult* pooled = nullptr);
    bool formatUsable(VkFormat format);
    bool linearFilterable(VkFormat format);
    void fail(std::string message);

    VkContext& context_;
    std::shared_ptr<ImagePool> pool_;
    std::map<VkFormat, bool> format_usable_;
    std::map<VkFormat, bool> linear_filterable_;
    std::string error_;
    std::uint64_t tick_ = 0;
    std::uint64_t images_created_ = 0;
    std::uint64_t image_bytes_created_ = 0;
    std::map<Key, std::shared_ptr<TextureImage>> images_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::shared_ptr<TextureImage>> scratch_;
    std::map<SamplerSettings, VkSampler> samplers_;
};

}  // namespace tox2

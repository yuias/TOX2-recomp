#include "render/vk_renderer.h"

#include <algorithm>
#include <cassert>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/log.h"
#include "render/dlaa_math.h"
#include "render/dlaa_pass.h"
#include "render/fp_translator.h"
#include "render/frame_dump.h"
#include "render/object_motion.h"
#include "render/object_motion_history.h"
#include "render/render_scale.h"
#include "render/shader_compiler.h"
#include "render/vertex_convert.h"
#include "render/vk_textures.h"
#include "render/vp_translator.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ull;
constexpr std::size_t kUploadAlignment = 256;  // covers every uniform and vertex buffer offset alignment
constexpr std::size_t kUploadBlockSize = 8u << 20;
constexpr std::uint64_t kImageLogFlips = 600;  // flips between the image-creation log lines
// The only operations that write a swapchain image: the black clear and the blit.
constexpr VkPipelineStageFlags2 kPresentWriteStages = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
constexpr std::uint32_t kIdentityRemap = 0xAAE4;  // RSX_TEXTURE_REMAP_IDENTITY (RPCS3 `gcm_enums.h`)

bool formatHasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// Bytes per texel of the formats a render target uses (the 32-bit depth-stencil format is counted as 8: padded).
std::uint32_t targetTexelBytes(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
        default: return 4;
    }
}

VkImageAspectFlags depthAspect(VkFormat format) {
    return formatHasStencil(format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                    : VK_IMAGE_ASPECT_DEPTH_BIT;
}

// GCM blend factor words to Vulkan, as RPCS3 `VK/VKGSRender.cpp` `vk::get_blend_factor` (a word outside
// the table was counted by the state decoder; it blends as ZERO here).
VkBlendFactor blendFactor(std::uint32_t value) {
    switch (value) {
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ZERO;
    }
}

// GCM stencil op words to Vulkan (RPCS3 `VK/VKGSRender.cpp` `vk::get_stencil_op`; INCR and DECR clamp, the
// WRAP words wrap). The word is compared in full. KEEP is also the fallback for a word outside the table,
// which the state decoder counted; RPCS3 aborts there.
VkStencilOp stencilOp(std::uint32_t word) {
    switch (word) {
        case 0x0000: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

// RPCS3 `vk::get_blend_op`: the signed equations are emulated with their unsigned counterparts.
VkBlendOp blendOp(std::uint32_t value) {
    switch (value) {
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B:
        case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;  // ADD, ADD_SIGNED, REVERSE_ADD_SIGNED
    }
}

// RPCS3 `VK/VKVertexBuffers.cpp` `vk::get_appropriate_topology`. Primitives RPCS3 emulates with index
// expansion (line loop, quads, polygon) are not modelled and return false.
bool topologyFor(RsxPrimitive primitive, VkPrimitiveTopology& topology, VkPrimitiveTopology& topology_class) {
    switch (primitive) {
        case RsxPrimitive::Points: topology = topology_class = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
        case RsxPrimitive::Lines: topology = topology_class = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
        case RsxPrimitive::LineStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            topology_class = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            return true;
        case RsxPrimitive::Triangles: topology = topology_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
        case RsxPrimitive::TriangleStrip:
        case RsxPrimitive::QuadStrip:  // a quad strip rasterizes as a triangle strip
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            topology_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            return true;
        case RsxPrimitive::TriangleFan:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            topology_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            return true;
        default: return false;
    }
}

// Color mask register layout: B bit 0, G bit 8, R bit 16, A bit 24 (RPCS3 `rsx_decode.h`
// registers_decoder<NV4097_SET_COLOR_MASK>).
VkColorComponentFlags colorWriteMask(std::uint32_t reg) {
    VkColorComponentFlags mask = 0;
    if ((reg & (1u << 16)) != 0) mask |= VK_COLOR_COMPONENT_R_BIT;
    if ((reg & (1u << 8)) != 0) mask |= VK_COLOR_COMPONENT_G_BIT;
    if ((reg & 1u) != 0) mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((reg & (1u << 24)) != 0) mask |= VK_COLOR_COMPONENT_A_BIT;
    return mask;
}

// SET_BLEND_COLOR for 8-bit surfaces: blue bits 0..7, green 8..15, red 16..23, alpha 24..31
// (`rsx_decode.h` registers_decoder<NV4097_SET_BLEND_COLOR>).
void blendConstants(std::uint32_t reg, float (&out)[4]) {
    out[0] = static_cast<float>((reg >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((reg >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(reg & 0xFF) / 255.0f;
    out[3] = static_cast<float>(reg >> 24) / 255.0f;
}

}  // namespace

struct RsxRenderer::RenderTarget {
    VkContext* context = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    // Guest memory layout in pixels, the size the cache identifies a target by and texture sizes are compared with.
    std::uint32_t layout_width = 0, layout_height = 0;
    std::uint32_t scale = 1;                          // image pixels per layout pixel
    std::uint32_t image_width = 0, image_height = 0;  // layout size x scale: the VkImage extent
    std::uint32_t pitch = 0;  // the surface's row pitch; a resize only inherits contents across equal pitches
    bool depth = false;
    // Layout at the end of the commands recorded so far (the frame's command buffer is submitted in order).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Views with a non-identity channel remap for sampling the target, by remap word (low 16 bits).
    std::map<std::uint32_t, VkImageView> sampled_views;
    bool sampled = false;  // the image has SAMPLED usage
    // Depth targets only. Single-aspect views for reading the depth and the stencil in a shader, and the
    // surface rewritten as the A8R8G8B8 texels the RSX reads from the same memory (see depthAsArgb). The copy
    // is current while argb_serial equals write_serial, which every clear and every draw that can write the
    // depth bumps.
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageView stencil_view = VK_NULL_HANDLE;
    std::shared_ptr<TextureImage> argb;
    std::uint64_t write_serial = 0;
    std::uint64_t argb_serial = ~0ull;

    // The image an anti-aliased pass (mode 3) draws into: 2 samples in AaMode::Msaa, 1 in AaMode::Dlaa. The image above
    // stays the guest memory layout (two samples of a pixel side by side) that every read uses; the sidecar is copied
    // into it by syncLayout when something reads it. Same format, at the clip size times the scale.
    struct MultisampleImage {
        VkContext* context = nullptr;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_2_BIT;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;  // attachment view; also the sampler2DMS view of a colour target
        // Depth targets only: single-aspect views for sampler2DMS / usampler2DMS (depthAsArgb).
        VkImageView depth_view = VK_NULL_HANDLE;
        VkImageView stencil_view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageUsageFlags usage = 0;
        // The image goes back to the pool: the owner is destroyed only after its slots' fences, so the GPU is done.
        std::shared_ptr<ImagePool> pool;
        PooledImageKey pool_key{};
        std::uint64_t pool_bytes = 0;
        ~MultisampleImage() {
            if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), depth_view, nullptr);
            if (stencil_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), stencil_view, nullptr);
            if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
            if (image == VK_NULL_HANDLE) return;
            if (pool) {
                pool->give(pool_key, pool_bytes, image, allocation);
            } else {
                vmaDestroyImage(context->allocator(), image, allocation);
            }
        }
    };
    std::unique_ptr<MultisampleImage> msaa;
    bool layout_stale = false;    // msaa holds writes the layout image lacks
    bool msaa_stale = false;      // the layout image holds writes msaa lacks
    bool layout_written = false;  // the layout image was written after its initial clear

    // Dlaa mode, colour targets: the depth target bound by the latest draw or clear that wrote the sidecar, which DLAA
    // needs beside the colour. Held as a pointer because the game's resolve rebinds the same depth address as another
    // surface, which replaces the target in the cache. Dropped when the evaluation has used it.
    std::shared_ptr<RenderTarget> dlaa_depth;
    // The sidecar has been through DLAA since its latest write (and no layout pass has written the layout image since).
    bool dlaa_evaluated = false;

    // Extent of the image a pass over this target draws into.
    std::uint32_t passWidth(bool sidecar) const { return sidecar ? layout_width / 2 * scale : image_width; }
    std::uint32_t passHeight(bool sidecar) const { return sidecar ? layout_height * scale : image_height; }

    std::shared_ptr<ImagePool> pool;  // where the image goes when the target is destroyed (see MultisampleImage)
    PooledImageKey pool_key{};
    std::uint64_t pool_bytes = 0;

    RenderTarget() = default;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    ~RenderTarget() {
        for (const auto& entry : sampled_views) vkDestroyImageView(context->device(), entry.second, nullptr);
        if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), depth_view, nullptr);
        if (stencil_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), stencil_view, nullptr);
        if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
        if (image == VK_NULL_HANDLE) return;
        if (pool) {
            pool->give(pool_key, pool_bytes, image, allocation);
        } else {
            vmaDestroyImage(context->allocator(), image, allocation);
        }
    }
};

struct RsxRenderer::FrameSlot {
    struct UploadBlock {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::uint8_t* mapped = nullptr;
        std::size_t size = 0;
        std::size_t used = 0;
    };

    VkCommandPool pool = VK_NULL_HANDLE;
    // Linear allocators, reset when the slot's fence has signaled and its command buffer is rewritten.
    std::vector<UploadBlock> uploads;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;  // submitted and not yet waited for
    // Targets recorded into this slot's command buffer; kept alive until its fence has signaled, so a
    // target dropped from the cache by a resize outlives the commands that use it.
    std::vector<std::shared_ptr<RenderTarget>> in_use;
    // The same for texture images and render target copies.
    std::vector<std::shared_ptr<TextureImage>> texture_refs;
    // Images retired while this slot was recording (a sidecar replaced by a mode switch); kept until the fence has
    // signaled for the same reason as in_use.
    std::vector<std::shared_ptr<void>> keep_alive;
    // Host-visible copy of the display buffer written at the flip that submitted this slot.
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_allocation = nullptr;
    void* readback_mapped = nullptr;
    VkDeviceSize readback_size = 0;
    // GPU timing (RenderOptions::gpu_timing). Queries 0 and 1 are the frame's first and last command; each timed
    // instance takes two more.
    struct TimedInstance {
        GpuInstanceLabel label;
        std::uint32_t begin_query = 0;
    };
    VkQueryPool queries = VK_NULL_HANDLE;
    std::vector<TimedInstance> timed;
    std::uint32_t queries_used = 0;
    std::uint32_t untimed = 0;  // instances that found the pool full
    bool timed_submitted = false;
};

constexpr std::uint32_t kGpuQueryCapacity = 4096;

// Empty when DLAA can run; else the Streamline status or the first image format the pass cannot use. The blit
// formats are the scene image's layout copy, the storage and sampled ones are DLSS's output and motion vectors.
static std::string dlaaGap(const VkContext& context) {
    if (context.streamline() == nullptr) {
        return context.streamlineStatus().empty() ? "Streamline was not requested" : context.streamlineStatus();
    }
    struct FormatNeed {
        VkFormat format;
        VkFormatFeatureFlags features;
        const char* what;
    };
    constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const FormatNeed needs[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, kBlit, "blit"},
            {VK_FORMAT_R32_SFLOAT, kBlit, "blit"},
            {context.depthStencilFormat(), kBlit, "blit"},
            {VK_FORMAT_D16_UNORM, kBlit, "blit"},
            {VK_FORMAT_R8G8B8A8_UNORM,
             VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | kBlit,
             "storage, linear sampling and blit"},
            {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
             "sampling and transfer"},
    };
    for (const FormatNeed& need : needs) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), need.format, &props);
        if ((props.optimalTilingFeatures & need.features) != need.features) {
            return std::format("format {} lacks {} support", static_cast<int>(need.format), need.what);
        }
    }
    return {};
}

RsxRenderer::RsxRenderer(VkContext& context, RenderOptions options) : context_(context), options_(std::move(options)) {
    if (!options_.shader_cache.empty()) setShaderCacheDir(options_.shader_cache);
    dumper_ = std::make_unique<FrameDumper>();
    if (options_.resolution_scale < 1 || options_.resolution_scale > kMaxResolutionScale) {
        fail(std::format("resolution scale {} is outside 1..{}", options_.resolution_scale, kMaxResolutionScale));
        return;
    }
    if (anisotropyName(options_.anisotropy).empty()) {
        fail(std::format("anisotropy level {} is not off, 2, 4, 8 or 16", options_.anisotropy));
        return;
    }
    resolution_scale_ = options_.resolution_scale;
    aa_mode_ = options_.aa_mode;
    frame_gen_target_ = options_.frame_gen;
    dlaa_gap_ = dlaaGap(context_);
    if (aa_mode_ == AaMode::Dlaa && !dlaaAvailable()) {
        aa_mode_ = msaaAvailable() ? AaMode::Msaa : AaMode::Supersample;
        logWarn("tox2: render: DLAA unavailable ({}); anti-aliased surfaces use {}", dlaa_gap_, aaModeName(aa_mode_));
        rsxNoteUnimplemented("state", "DLAA unavailable");
    }
    if (aa_mode_ == AaMode::Msaa && !msaaAvailable()) {
        aa_mode_ = AaMode::Supersample;
        logWarn("tox2: render: 2-sample multisampling unavailable ({}); anti-aliased surfaces are supersampled",
                context_.twoSampleSupportGap());
        rsxNoteUnimplemented("state", "2-sample multisampling unavailable");
    }
    pipelines_ = std::make_unique<DrawPipelines>(context_, options_.shader_cache.empty()
                                                                   ? std::filesystem::path()
                                                                   : options_.shader_cache / "pipelines.bin");
    if (!pipelines_->valid()) {
        fail(pipelines_->error());
        return;
    }
    image_pool_ = std::make_shared<ImagePool>(context_);
    textures_ = std::make_unique<TextureCache>(context_, image_pool_);
    setAnisotropy(options_.anisotropy);
    if (anisotropy_ > 1) logInfo("tox2: render: anisotropic filtering {}x", anisotropy_);
    const VkDevice device = context_.device();
    if (options_.gpu_timing) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &family_count, families.data());
        const std::uint32_t valid_bits = context_.graphicsQueueFamily() < family_count
                                                 ? families[context_.graphicsQueueFamily()].timestampValidBits
                                                 : 0;
        const float period = context_.deviceProperties().limits.timestampPeriod;
        if (valid_bits == 0) {
            gpu_timing_gap_ = "the graphics queue family has no timestamp support (timestampValidBits is 0)";
        } else if (period <= 0.0f) {
            gpu_timing_gap_ = "the device reports a timestampPeriod of 0";
        } else {
            gpu_timing_ = true;
            timestamp_period_ns_ = period;
            timestamp_mask_ = valid_bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << valid_bits) - 1;
        }
        if (!gpu_timing_) logWarn("tox2: render: GPU timing unavailable ({})", gpu_timing_gap_);
    }
    for (std::unique_ptr<FrameSlot>& slot_ptr : slots_) {
        slot_ptr = std::make_unique<FrameSlot>();
        FrameSlot& slot = *slot_ptr;
        if (gpu_timing_) {
            VkQueryPoolCreateInfo query_info{};
            query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_info.queryCount = kGpuQueryCapacity;
            if (vkCreateQueryPool(device, &query_info, nullptr, &slot.queries) != VK_SUCCESS) {
                fail("vkCreateQueryPool failed");
                return;
            }
        }

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = context_.graphicsQueueFamily();
        if (vkCreateCommandPool(device, &pool_info, nullptr, &slot.pool) != VK_SUCCESS) {
            fail("vkCreateCommandPool failed");
            return;
        }
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = slot.pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cmd_info, &slot.cmd) != VK_SUCCESS) {
            fail("vkAllocateCommandBuffers failed");
            return;
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
            fail("vkCreateFence failed");
            return;
        }
    }
}

RsxRenderer::~RsxRenderer() {
    frame_gen_.reset();  // joins the sleeper thread before anything it uses goes away
    const VkDevice device = context_.device();
    vkDeviceWaitIdle(device);
    fg_motion_.reset();
    history_.reset();
    for (std::shared_ptr<FgDepthCopy>& copy : fg_depth_) copy.reset();
    dlaa_.reset();  // frees the DLSS viewport; the device is idle
    for (std::unique_ptr<FrameSlot>& slot : slots_) {
        if (!slot) continue;
        slot->in_use.clear();
        slot->texture_refs.clear();
        slot->keep_alive.clear();
        for (FrameSlot::UploadBlock& block : slot->uploads) {
            vmaDestroyBuffer(context_.allocator(), block.buffer, block.allocation);
        }
        if (slot->readback != VK_NULL_HANDLE)
            vmaDestroyBuffer(context_.allocator(), slot->readback, slot->readback_allocation);
        if (slot->queries != VK_NULL_HANDLE) vkDestroyQueryPool(device, slot->queries, nullptr);
        if (slot->fence != VK_NULL_HANDLE) vkDestroyFence(device, slot->fence, nullptr);
        if (slot->pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, slot->pool, nullptr);
    }
    pipelines_.reset();
    textures_.reset();
    if (placeholder_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device, placeholder_sampler_, nullptr);
    for (const Placeholder& placeholder : placeholders_) {
        if (placeholder.view != VK_NULL_HANDLE) vkDestroyImageView(device, placeholder.view, nullptr);
        if (placeholder.image != VK_NULL_HANDLE)
            vmaDestroyImage(context_.allocator(), placeholder.image, placeholder.allocation);
    }
    last_display_.reset();
    color_targets_.clear();
    depth_targets_.clear();
}

void RsxRenderer::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

void RsxRenderer::noteImageCreated(RenderImageKind kind, std::uint64_t bytes) {
    const auto index = static_cast<std::size_t>(kind);
    stats_.images_created[index]++;
    stats_.image_bytes_created[index] += bytes;
}

bool RsxRenderer::obtainImage(RenderImageKind kind, const VkImageCreateInfo& info, std::uint64_t bytes,
                              PooledImageKey& key, VkImage& image, VmaAllocation& allocation) {
    // The key leaves these out; a caller that varies them would be handed an incompatible image.
    assert(info.imageType == VK_IMAGE_TYPE_2D && info.flags == 0 && info.mipLevels == 1 && info.arrayLayers == 1 &&
           info.extent.depth == 1 && info.tiling == VK_IMAGE_TILING_OPTIMAL);
    key = PooledImageKey{info.format, info.extent.width, info.extent.height, info.samples, info.usage};
    const ImagePoolResult taken = image_pool_->take(key, image, allocation);
    if (taken.hit) {
        stats_.image_pool_hits[static_cast<std::size_t>(kind)]++;
        return true;
    }
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(context_.allocator(), &info, &alloc_info, &image, &allocation, nullptr) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return false;
    }
    stats_.image_pool_misses[static_cast<std::size_t>(taken.miss)]++;
    noteImageCreated(kind, bytes);
    return true;
}

void RsxRenderer::syncTextureStats() {
    if (!textures_) return;
    const auto index = static_cast<std::size_t>(RenderImageKind::Texture);
    stats_.images_created[index] = textures_->imagesCreated();
    stats_.image_bytes_created[index] = textures_->imageBytesCreated();
}

// Image churn shows up in the allocator and driver time of the RSX thread; this tells which kinds cause it.
void RsxRenderer::logImageCreations() {
    syncTextureStats();
    const auto delta = [&](RenderImageKind kind, std::uint64_t& count, double& megabytes) {
        const auto index = static_cast<std::size_t>(kind);
        count = stats_.images_created[index] - logged_stats_.images_created[index];
        megabytes = static_cast<double>(stats_.image_bytes_created[index] - logged_stats_.image_bytes_created[index]) /
                    (1024.0 * 1024.0);
    };
    std::uint64_t counts[kRenderImageKindCount];
    double megabytes[kRenderImageKindCount];
    for (std::size_t i = 0; i < kRenderImageKindCount; i++)
        delta(static_cast<RenderImageKind>(i), counts[i], megabytes[i]);
    const auto hits = [&](RenderImageKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        return stats_.image_pool_hits[index] - logged_stats_.image_pool_hits[index];
    };
    const auto misses = [&](ImagePoolMiss miss) {
        const auto index = static_cast<std::size_t>(miss);
        return stats_.image_pool_misses[index] - logged_stats_.image_pool_misses[index];
    };
    const ImagePoolDiscards& discards = stats_.image_pool_discards;
    const ImagePoolDiscards& logged_discards = logged_stats_.image_pool_discards;
    logDebug(
            "tox2: render: last 600 flips: created target {} ({:.1f} MB), sidecar {} ({:.1f} MB), depth-argb {} "
            "({:.1f} MB), texture {} ({:.1f} MB), upload-block {} ({:.1f} MB); pool hits target {}, sidecar {}, "
            "depth-argb {}; misses no-entry {}, key-cap {}, byte-cap {}, idle {}; discarded key-cap {}, byte-cap {}, "
            "idle {}; pooled {:.1f} MB",
            counts[0], megabytes[0], counts[1], megabytes[1], counts[2], megabytes[2], counts[3], megabytes[3],
            counts[4], megabytes[4], hits(RenderImageKind::Target), hits(RenderImageKind::Sidecar),
            hits(RenderImageKind::DepthArgb), misses(ImagePoolMiss::NoEntry), misses(ImagePoolMiss::KeyCap),
            misses(ImagePoolMiss::ByteCap), misses(ImagePoolMiss::Idle), discards.key_cap - logged_discards.key_cap,
            discards.byte_cap - logged_discards.byte_cap, discards.idle - logged_discards.idle,
            static_cast<double>(stats_.image_pool_bytes) / (1024.0 * 1024.0));
    logged_stats_.images_created = stats_.images_created;
    logged_stats_.image_bytes_created = stats_.image_bytes_created;
    logged_stats_.image_pool_hits = stats_.image_pool_hits;
    logged_stats_.image_pool_misses = stats_.image_pool_misses;
    logged_stats_.image_pool_discards = stats_.image_pool_discards;
}

bool RsxRenderer::waitSlot(FrameSlot& slot) {
    if (!slot.pending) return true;
    const auto wait_begin = std::chrono::steady_clock::now();
    const VkResult result = vkWaitForFences(context_.device(), 1, &slot.fence, VK_TRUE, kFenceTimeoutNs);
    stats_.slot_wait_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - wait_begin)
                    .count());
    if (result != VK_SUCCESS) {
        fail(std::format("waiting for the GPU failed ({})", static_cast<int>(result)));
        return false;
    }
    slot.pending = false;
    slot.in_use.clear();
    slot.texture_refs.clear();
    slot.keep_alive.clear();
    gpuReadSlot(slot);
    return true;
}

void RsxRenderer::drainGpuTiming() {
    if (!gpu_timing_) return;
    for (std::unique_ptr<FrameSlot>& slot : slots_) {
        if (slot) waitSlot(*slot);
    }
}

const char* gpuInstanceKindName(GpuInstanceKind kind) {
    switch (kind) {
        case GpuInstanceKind::Draw: return "draw";
        case GpuInstanceKind::Clear: return "clear";
        case GpuInstanceKind::MaskedClear: return "masked-clear";
        case GpuInstanceKind::Layout: return "layout";
        case GpuInstanceKind::DepthArgb: return "depth-argb";
    }
    return "?";
}

void RsxRenderer::gpuBeginInstance(VkCommandBuffer cmd, GpuInstanceKind kind, std::uint32_t width, std::uint32_t height,
                                   std::uint32_t samples, VkFormat color_format, bool depth) {
    if (!gpu_timing_) return;
    FrameSlot& slot = *slots_[current_slot_];
    gpu_open_ = false;
    if (slot.queries_used + 2 > kGpuQueryCapacity) {
        slot.untimed++;
        return;
    }
    FrameSlot::TimedInstance& instance = slot.timed.emplace_back();
    instance.label = {kind, width, height, samples, color_format, depth, 0};
    instance.begin_query = slot.queries_used;
    slot.queries_used += 2;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, instance.begin_query);
    gpu_open_ = true;
}

void RsxRenderer::gpuEndInstance(VkCommandBuffer cmd) {
    if (!gpu_open_) return;
    gpu_open_ = false;
    const FrameSlot& slot = *slots_[current_slot_];
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, slot.timed.back().begin_query + 1);
}

// Called once the slot's fence has signaled, so every query of the frame is available.
void RsxRenderer::gpuReadSlot(FrameSlot& slot) {
    if (!gpu_timing_ || !slot.timed_submitted) return;
    slot.timed_submitted = false;
    std::vector<std::uint64_t> ticks(slot.queries_used);
    if (vkGetQueryPoolResults(context_.device(), slot.queries, 0, slot.queries_used,
                              ticks.size() * sizeof(std::uint64_t), ticks.data(), sizeof(std::uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    const auto ms = [&](std::uint32_t begin, std::uint32_t end) {
        return static_cast<double>((ticks[end] - ticks[begin]) & timestamp_mask_) * timestamp_period_ns_ / 1e6;
    };
    GpuTimingFrame frame;
    frame.frame_ms = ms(0, 1);
    double instances_ms = 0;
    for (const FrameSlot::TimedInstance& instance : slot.timed) {
        const double instance_ms = ms(instance.begin_query, instance.begin_query + 1);
        frame.instances.push_back({instance.label, instance_ms});
        instances_ms += instance_ms;
    }
    frame.outside_ms = frame.frame_ms - instances_ms;
    frame.untimed_instances = slot.untimed;
    gpu_frames_.push_back(std::move(frame));
}

bool RsxRenderer::beginFrame() {
    if (!valid()) return false;
    if (recording_) return true;
    FrameSlot& slot = *slots_[current_slot_];
    if (!waitSlot(slot)) return false;
    const VkDevice device = context_.device();
    vkResetFences(device, 1, &slot.fence);
    vkResetCommandPool(device, slot.pool, 0);
    for (FrameSlot::UploadBlock& block : slot.uploads) block.used = 0;
    // The cached region lives in an upload block that was just recycled (or belongs to the other slot).
    vp_constant_upload_ = {};
    open_pass_.reset();     // submitFrame closed it; the reset command pool dropped the recording anyway
    invalidateDrawState();  // a new recording starts with no dynamic state set
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS) {
        fail("vkBeginCommandBuffer failed");
        return false;
    }
    if (gpu_timing_) {
        slot.timed.clear();
        slot.untimed = 0;
        slot.queries_used = 2;
        gpu_open_ = false;
        vkCmdResetQueryPool(slot.cmd, slot.queries, 0, kGpuQueryCapacity);
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 0);
    }
    recording_ = true;
    frame_index_ = static_cast<std::uint32_t>(stats_.flips) + 1;
    if (frame_gen_) frame_gen_->beginFrame(frame_index_);
    return true;
}

// The command buffer for anything recorded outside a draw's rendering instance. Every such site asks here at the
// point of recording, so the instance a draw left open for the next one is closed in this one place.
VkCommandBuffer RsxRenderer::outsideCmd() {
    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    if (open_pass_) {
        vkCmdEndRendering(cmd);
        gpuEndInstance(cmd);
        open_pass_.reset();
    }
    return cmd;
}

bool RsxRenderer::submitFrame() {
    FrameSlot& slot = *slots_[current_slot_];
    for (const FrameSlot::UploadBlock& block : slot.uploads) {
        if (block.used != 0) vmaFlushAllocation(context_.allocator(), block.allocation, 0, block.used);
    }
    const VkCommandBuffer cmd = outsideCmd();
    if (gpu_timing_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 1);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fail("vkEndCommandBuffer failed");
        return false;
    }
    VkCommandBufferSubmitInfo cmd_submit{};
    cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_submit.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_submit;
    // Only the blit waits for the swapchain image; the draws recorded before it start right away.
    VkSemaphoreSubmitInfo wait{};
    VkSemaphoreSubmitInfo signal{};
    if (present_pending_) {
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = present_target_.acquire_semaphore;
        wait.stageMask = kPresentWriteStages;
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = present_target_.render_semaphore;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }
    if (vkQueueSubmit2(context_.graphicsQueue(), 1, &submit, slot.fence) != VK_SUCCESS) {
        fail("vkQueueSubmit2 failed");
        return false;
    }
    slot.pending = true;
    slot.timed_submitted = gpu_timing_;
    recording_ = false;
    display_slot_ = current_slot_;
    current_slot_ = (current_slot_ + 1) % kFramesInFlight;
    if (frame_gen_) frame_gen_->submitted();
    if (present_pending_) {
        present_pending_ = false;
        if (frame_gen_) frame_gen_->presentStarted();
        presenter_->present(present_target_);
        if (frame_gen_) frame_gen_->presentFinished();
    }
    // The next frame's token and Reflex sleep, even when nothing was presented: its markers follow at its first
    // command.
    if (frame_gen_) frame_gen_->frameDone(static_cast<std::uint32_t>(stats_.flips) + 1);
    return true;
}

void RsxRenderer::touch(const std::shared_ptr<RenderTarget>& target) {
    std::vector<std::shared_ptr<RenderTarget>>& in_use = slots_[current_slot_]->in_use;
    if (std::find(in_use.begin(), in_use.end(), target) == in_use.end()) in_use.push_back(target);
}

// Conservative full barrier, also recorded before every clear and every draw that begins a rendering instance (even
// when the layout does not change, so successive instances over one attachment or transfer destination are ordered;
// draws that continue an open instance are ordered by rasterization order and record none). An image that stays in a
// read-only layout needs none: a read after a read has no hazard, and every write to the image goes through another
// layout, whose transition records the barrier. Tighter stages are not needed yet.
void RsxRenderer::transitionImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout& current,
                                  VkImageLayout layout) {
    if (current == layout &&
        (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
         layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) {
        stats_.barriers_skipped++;
        return;
    }
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = current;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(outsideCmd(), &dependency);
    stats_.barriers++;
    current = layout;
}

void RsxRenderer::transition(RenderTarget& target, VkImageLayout layout) {
    transitionImage(target.image, target.aspect, target.layout, layout);
}

void RsxRenderer::transitionSidecar(RenderTarget& target, VkImageLayout layout) {
    transitionImage(target.msaa->image, target.aspect, target.msaa->layout, layout);
}

namespace {

// Guest memory layout of a surface: the clip size times the samples per pixel of its anti-aliased layout. The RSX
// stores every sample, so a surface in mode 3 is twice as wide as its clip and one in modes 4 and 5 twice as
// wide and tall. A missing sample count (0) counts as one.
std::uint32_t layoutWidth(const RsxSurface& surface) {
    return surface.width * std::max<std::uint32_t>(surface.samples_x, 1);
}
std::uint32_t layoutHeight(const RsxSurface& surface) {
    return surface.height * std::max<std::uint32_t>(surface.samples_y, 1);
}

// Image pixels per clip pixel of a pass over `surface` drawn into a target of the given scale. A sidecar pass draws
// into an image of the clip size, one pixel per clip pixel.
PixelScale pixelScaleOf(const RsxSurface& surface, std::uint32_t scale, bool sidecar) {
    if (sidecar) return {scale, scale};
    return {scale * std::max<std::uint32_t>(surface.samples_x, 1),
            scale * std::max<std::uint32_t>(surface.samples_y, 1)};
}

}  // namespace

bool RsxRenderer::setResolutionScale(std::uint32_t scale) {
    if (scale < 1 || scale > kMaxResolutionScale) return false;
    resolution_scale_ = scale;
    if (history_) history_->clear();
    return true;
}

bool RsxRenderer::setAaMode(AaMode mode) {
    if (mode == AaMode::Msaa && !msaaAvailable()) return false;
    if (mode == AaMode::Dlaa && !dlaaAvailable()) return false;
    if (mode == AaMode::Dlaa && aa_mode_ != AaMode::Dlaa) dlaa_switched_ = true;
    if (history_ && mode != aa_mode_) history_->clear();
    aa_mode_ = mode;
    return true;
}

std::string RsxRenderer::frameGenUnsupportedReason() const {
    if (presenter_ == nullptr) return "no window";
    if (context_.frameGenerationSupported()) return {};
    return context_.frameGenerationStatus().empty() ? "the Vulkan device was not created for frame generation"
                                                    : context_.frameGenerationStatus();
}

bool RsxRenderer::setFrameGenTarget(FrameGenTarget target) {
    if (target != FrameGenTarget::Off && !frameGenSupported()) return false;
    frame_gen_target_ = target;
    if (frame_gen_) frame_gen_->setTarget(target);
    return true;
}

// The presenter is set after construction, so the initial load cannot happen in the constructor. No swapchain exists
// yet, so the first one is already created with the plugins of the requested state.
void RsxRenderer::createFrameGen() {
    if (frame_gen_ || !frameGenSupported() || context_.streamline() == nullptr) return;
    frame_gen_ = std::make_unique<FrameGenController>(
            context_, *presenter_,
            FrameGenControllerOptions{.present_mode = options_.fg_debug.present_mode,
                                      .reflex_low_latency = options_.fg_debug.reflex_low_latency,
                                      .only_generated = options_.fg_debug.only_generated,
                                      .cycle_seconds = options_.fg_debug.cycle_seconds});
    frame_gen_->setTarget(frame_gen_target_);
}

// The target is read at the first flip, not at construction: the presenter is set in between, and the reason depends
// on it.
void RsxRenderer::noteFrameGenTarget() {
    if (frame_gen_target_ == frame_gen_noted_) return;
    frame_gen_noted_ = frame_gen_target_;
    if (frame_gen_target_ == FrameGenTarget::Off) return;
    if (!frameGenSupported()) {
        logWarn("tox2: render: frame generation unavailable ({}); presenting without it", frameGenUnsupportedReason());
    }
}

bool RsxRenderer::setAnisotropy(std::uint32_t level) {
    if (anisotropyName(level).empty()) return false;
    const float device_max = context_.maxSamplerAnisotropy();
    if (device_max < 2.0f) {
        if (level > 1 && !anisotropy_unavailable_logged_) {
            anisotropy_unavailable_logged_ = true;
            logInfo("tox2: render: anisotropic filtering unavailable on this device");
        }
        anisotropy_ = 1;
    } else {
        anisotropy_ = std::min(level, static_cast<std::uint32_t>(device_max));
    }
    if (textures_) textures_->setAnisotropy(anisotropy_);
    return true;
}

// The scale a target of this layout gets now: the renderer's scale, or the largest one that fits the device's image
// size. Callers have checked the layout against that size.
std::uint32_t RsxRenderer::targetScale(std::uint32_t layout_width, std::uint32_t layout_height) const {
    const std::uint32_t max_dimension = context_.deviceProperties().limits.maxImageDimension2D;
    return std::min(resolution_scale_, max_dimension / std::max(layout_width, layout_height));
}

// A target the cache has not seen holds no guest data (nothing is loaded from guest memory), so it
// starts defined instead of undefined: color is opaque black (0, 0, 0, 1), depth is 1.0 with stencil 0xFF.
// These are RPCS3's values for a surface with nothing inherited and "read color/depth buffers" off
// (VK/VKRenderTargets.cpp, rpcs3/Emu/system_config.h), and what a partial clear leaves in the aspect it does
// not touch (VK/VKGSRender.cpp VKGSRender::clear_surface).
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::createTarget(bool depth, std::uint32_t layout_width,
                                                                     std::uint32_t layout_height, std::uint32_t pitch,
                                                                     VkFormat format) {
    const std::uint32_t max_dimension = context_.deviceProperties().limits.maxImageDimension2D;
    if (layout_width == 0 || layout_height == 0 || layout_width > max_dimension || layout_height > max_dimension) {
        rsxNoteUnimplemented("state", std::format("surface size {}x{}", layout_width, layout_height));
        return nullptr;
    }
    // A target that would exceed the device's image size at the current scale is made at the largest scale that
    // fits; a draw or clear then refuses to mix it with an attachment of another scale.
    const std::uint32_t scale = targetScale(layout_width, layout_height);
    if (scale < resolution_scale_) rsxNoteUnimplemented("state", "surface too large for the resolution scale");
    const std::uint32_t image_width = layout_width * scale;
    const std::uint32_t image_height = layout_height * scale;

    VkFormatProperties format_props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &format_props);
    VkImageUsageFlags usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    const bool sampled = (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    auto target = std::make_shared<RenderTarget>();
    target->context = &context_;
    target->sampled = sampled;
    target->format = format;
    target->aspect = depth ? depthAspect(format) : VK_IMAGE_ASPECT_COLOR_BIT;
    target->layout_width = layout_width;
    target->layout_height = layout_height;
    target->scale = scale;
    target->image_width = image_width;
    target->image_height = image_height;
    target->pitch = pitch;
    target->depth = depth;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {target->image_width, target->image_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    target->pool_bytes = static_cast<std::uint64_t>(image_width) * image_height * targetTexelBytes(format);
    if (!obtainImage(RenderImageKind::Target, image_info, target->pool_bytes, target->pool_key, target->image,
                     target->allocation)) {
        fail("vmaCreateImage failed for a render target");
        return nullptr;
    }
    target->pool = image_pool_;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = target->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {target->aspect, 0, 1, 0, 1};
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &target->view) != VK_SUCCESS) {
        target->view = VK_NULL_HANDLE;
        fail("vkCreateImageView failed for a render target");
        return nullptr;
    }

    transition(*target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkCommandBuffer cmd = outsideCmd();
    const VkImageSubresourceRange range = {target->aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue value{};
        value.depth = 1.0f;
        value.stencil = 0xFF;
        vkCmdClearDepthStencilImage(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    } else {
        VkClearColorValue value{};
        value.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
    return target;
}

// The anti-aliased surface of mode 3 (two samples side by side) is drawn into a 2-sample image in Msaa mode and into
// a 1-sample image of the clip size in Dlaa mode; modes 4 and 5 (2x2 layouts) stay on the single-sample layout image.
VkSampleCountFlags RsxRenderer::sidecarSamples(const RsxSurface& surface) const {
    if (surface.samples_x != 2 || surface.samples_y != 1) return 0;
    switch (aa_mode_) {
        case AaMode::Msaa: return VK_SAMPLE_COUNT_2_BIT;
        case AaMode::Dlaa: return VK_SAMPLE_COUNT_1_BIT;
        default: return 0;
    }
}

// Creates the sidecar of a mode-3 target with the sample count a pass needs, cleared to the values createTarget gives
// a new target. A sidecar of the other count (the mode was switched at run time) is replaced: the layout image takes
// its newer contents first, and the old image stays alive until the commands recorded against it have finished.
// Records outside any rendering instance.
bool RsxRenderer::ensureSidecar(RenderTarget& target, VkSampleCountFlagBits samples) {
    if (target.msaa) {
        if (target.msaa->samples == samples) return true;
        if (target.layout_stale && !syncLayout(target)) return false;
        slots_[current_slot_]->keep_alive.push_back(std::shared_ptr<void>(std::move(target.msaa)));
    }
    auto sidecar = std::make_unique<RenderTarget::MultisampleImage>();
    sidecar->context = &context_;
    sidecar->samples = samples;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = target.format;
    image_info.extent = {target.passWidth(true), target.passHeight(true), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (target.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // The 1-sample image reaches the layout image by a blit; a 2-sample one by a shader pass.
    if (samples == VK_SAMPLE_COUNT_1_BIT) image_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sidecar->usage = image_info.usage;
    sidecar->pool_bytes = static_cast<std::uint64_t>(image_info.extent.width) * image_info.extent.height *
                          static_cast<std::uint64_t>(samples) * targetTexelBytes(target.format);
    if (!obtainImage(RenderImageKind::Sidecar, image_info, sidecar->pool_bytes, sidecar->pool_key, sidecar->image,
                     sidecar->allocation)) {
        fail("vmaCreateImage failed for a sidecar render target");
        return false;
    }
    sidecar->pool = image_pool_;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = sidecar->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = target.format;
    view_info.subresourceRange = {target.aspect, 0, 1, 0, 1};
    if (vkCreateImageView(context_.device(), &view_info, nullptr, &sidecar->view) != VK_SUCCESS) {
        sidecar->view = VK_NULL_HANDLE;
        fail("vkCreateImageView failed for a sidecar render target");
        return false;
    }
    if (target.depth && formatHasStencil(target.format)) {
        const auto aspectView = [&](VkImageAspectFlags aspect, VkImageView& out) {
            view_info.subresourceRange = {aspect, 0, 1, 0, 1};
            if (vkCreateImageView(context_.device(), &view_info, nullptr, &out) != VK_SUCCESS) {
                out = VK_NULL_HANDLE;
                fail("vkCreateImageView failed for a sidecar depth view");
                return false;
            }
            return true;
        };
        if (!aspectView(VK_IMAGE_ASPECT_DEPTH_BIT, sidecar->depth_view) ||
            !aspectView(VK_IMAGE_ASPECT_STENCIL_BIT, sidecar->stencil_view)) {
            return false;
        }
    }
    target.msaa = std::move(sidecar);
    target.dlaa_evaluated = false;
    target.dlaa_depth.reset();
    // The layout image may already hold guest data the sidecar does not; a pass that does not replace all of it is
    // counted (beginSidecarPass).
    target.msaa_stale = target.layout_written;

    transitionSidecar(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkCommandBuffer cmd = outsideCmd();
    const VkImageSubresourceRange range = {target.aspect, 0, 1, 0, 1};
    if (target.depth) {
        VkClearDepthStencilValue value{};
        value.depth = 1.0f;
        value.stencil = 0xFF;
        vkCmdClearDepthStencilImage(cmd, target.msaa->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    } else {
        VkClearColorValue value{};
        value.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, target.msaa->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
    return true;
}

// Writes the layout image of a target from its sidecar when the sidecar holds newer data. A 2-sample sidecar: layout
// texel (x, y) takes sample kLayoutColumnSample[x & 1] of pixel (x >> 1, y) (render_scale.h layoutSource). A 1-sample
// sidecar: both texels of a pixel take its one value, by a blit. Records outside any rendering instance. False only
// on a Vulkan failure.
//
// A 2-sample depth target takes one instance. With VK_EXT_shader_stencil_export one pass writes the depth through
// gl_FragDepth and the stencil through gl_FragStencilRefARB. Without it (Z24S8 only) a depth pass is followed by
// eight passes that rebuild the stencil one bit at a time, because a shader cannot write the stencil otherwise.
bool RsxRenderer::syncLayout(RenderTarget& target) {
    if (!target.msaa || !target.layout_stale) return true;
    if (target.msaa->samples == VK_SAMPLE_COUNT_1_BIT) {
        // NEAREST maps destination texel x to source pixel x >> 1 (the destination is exactly twice as wide), so the
        // two texels of a pixel are equal. A depth blit copies depth and stencil together.
        transitionSidecar(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit region{};
        region.srcSubresource = {target.aspect, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<std::int32_t>(target.passWidth(true)),
                                static_cast<std::int32_t>(target.passHeight(true)), 1};
        region.dstSubresource = {target.aspect, 0, 0, 1};
        region.dstOffsets[1] = {static_cast<std::int32_t>(target.image_width),
                                static_cast<std::int32_t>(target.image_height), 1};
        vkCmdBlitImage(outsideCmd(), target.msaa->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        target.layout_stale = false;
        target.layout_written = true;
        stats_.layout_copies++;
        return true;
    }
    if (!ensurePlaceholder() || !valid()) return false;
    const bool has_stencil = target.depth && formatHasStencil(target.format);
    const bool stencil_export = has_stencil && context_.hasStencilExport();
    const VkPipeline pipeline = target.depth ? pipelines_->layoutDepthPipeline(target.format)
                                             : pipelines_->layoutColorPipeline(target.format);
    if (pipeline == VK_NULL_HANDLE) {
        fail(pipelines_->error());
        return false;
    }

    // The layout pass binds a pipeline with static state and sets its own viewport and scissor.
    invalidateDrawState();
    const VkCommandBuffer cmd = outsideCmd();
    const VkImageLayout read_layout =
            target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageLayout write_layout =
            target.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    transitionSidecar(target, read_layout);
    transition(target, write_layout);

    // Every texel is written; without the export the stencil starts at 0 and gains the bits the stencil passes set.
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = target.view;
    attachment.imageLayout = write_layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo stencil_attachment = attachment;
    stencil_attachment.loadOp = stencil_export ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
    stencil_attachment.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {target.image_width, target.image_height}};
    rendering.layerCount = 1;
    if (target.depth) {
        rendering.pDepthAttachment = &attachment;
        rendering.pStencilAttachment = has_stencil ? &stencil_attachment : nullptr;
    } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
    }

    const VkViewport viewport = {
            0.0f, 0.0f, static_cast<float>(target.image_width), static_cast<float>(target.image_height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {target.image_width, target.image_height}};
    const VkImageView first_view = has_stencil ? target.msaa->depth_view : target.msaa->view;
    const VkDescriptorImageInfo infos[2] = {{placeholder_sampler_, first_view, read_layout},
                                            {placeholder_sampler_, target.msaa->stencil_view, read_layout}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    LayoutPassConstants constants{};
    constants.column_sample[0] = static_cast<std::int32_t>(kLayoutColumnSample[0]);
    constants.column_sample[1] = static_cast<std::int32_t>(kLayoutColumnSample[1]);
    constants.stencil_bit = -1;
    const auto draw = [&] {
        vkCmdPushConstants(cmd, pipelines_->layoutPassLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                           &constants);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    };

    gpuBeginInstance(cmd, GpuInstanceKind::Layout, target.image_width, target.image_height, 1,
                     target.depth ? VK_FORMAT_UNDEFINED : target.format, target.depth);
    vkCmdBeginRendering(cmd, &rendering);
    stats_.rendering_instances++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layoutPassLayout(), 0,
                              has_stencil ? 2 : 1, writes);
    if (target.depth) {
        // The depth pass: depth test ALWAYS with writes on, stencil test off (on for the export pass). Stencil op,
        // masks and reference are set here once for the bit passes, which only toggle the write mask.
        vkCmdSetDepthTestEnable(cmd, VK_TRUE);
        vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
        vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
        // The exported value replaces the reference; REPLACE on pass stores it through the full write mask.
        vkCmdSetStencilTestEnable(cmd, stencil_export ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
                          VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        draw();
        if (has_stencil && !stencil_export) {
            // Bit b: the shader discards the fragments whose stencil sample has the bit clear, the others write
            // reference 0xFF through the one-bit write mask.
            vkCmdSetDepthTestEnable(cmd, VK_FALSE);
            vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
            vkCmdSetStencilTestEnable(cmd, VK_TRUE);
            for (std::int32_t bit = 0; bit < 8; bit++) {
                vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
                constants.stencil_bit = bit;
                draw();
            }
        }
    } else {
        draw();
    }
    vkCmdEndRendering(cmd);
    gpuEndInstance(cmd);

    target.layout_stale = false;
    target.layout_written = true;
    stats_.layout_copies++;
    return true;
}

// The tap rule: the game's resolve is the draw that samples the whole layout of a one-sample A8R8G8B8 scene image from
// a pass that is not a sidecar pass (it writes the plain display buffer). Identified by these properties, never by
// address. A read from inside the scene pass, a feedback read and a sample of a layout that DLAA has not seen all use
// the layout image as before.
bool RsxRenderer::dlaaTap(const RenderTarget& scene, const RenderTarget& drawing, bool sidecar_pass,
                          const RsxTextureDesc& texture) const {
    return options_.dlaa_evaluate && !sidecar_pass && &scene != &drawing && scene.format == VK_FORMAT_B8G8R8A8_UNORM &&
           scene.msaa && scene.msaa->samples == VK_SAMPLE_COUNT_1_BIT && texture.width == scene.layout_width &&
           texture.height == scene.layout_height && (scene.layout_stale || scene.dlaa_evaluated);
}

// Evaluates DLAA on the scene image: colour sidecar and the depth that went with it in, the anti-aliased scene out,
// then widened by a NEAREST blit to the 2W x H the resolve samples. The resolve's texcoords carry a -0.5 / (2W) offset
// that puts a sample at the centre of layout texel 2X, so a W-wide image sampled directly would blend in the
// neighbouring pixel; the widened image reproduces the layout's two equal columns per pixel.
VkImageView RsxRenderer::dlaaView(RenderTarget& scene, std::uint32_t remap) {
    const auto failed = [&](const std::string& reason) -> VkImageView {
        logOnce(LogLevel::Warn, "render-dlaa-failed",
                "tox2: render: DLAA evaluation failed ({}); scene read unfiltered", reason);
        rsxNoteUnimplemented("state", "DLAA evaluation failed");
        stats_.dlaa_failures++;
        return VK_NULL_HANDLE;
    };
    const std::uint32_t width = scene.passWidth(true), height = scene.passHeight(true);

    if (!(scene.dlaa_evaluated && dlaa_output_owner_ == &scene)) {
        // A second scene target would overwrite the shared output; only one is expected.
        if (scene.dlaa_evaluated) return failed("another scene target owns the DLAA output");
        const std::shared_ptr<RenderTarget> depth = scene.dlaa_depth;
        if (!depth || !depth->msaa || depth->msaa->samples != VK_SAMPLE_COUNT_1_BIT ||
            depth->passWidth(true) != width || depth->passHeight(true) != height) {
            return failed("no matching depth image for the scene");
        }
        if (!dlaa_) dlaa_ = std::make_unique<DlaaPass>(context_, *context_.streamline());

        std::string error;
        std::shared_ptr<void> retired;
        bool recreated = false;
        if (!dlaa_->prepare(width, height, retired, recreated, error)) return failed(error);
        // Frames in flight may still read the previous images.
        if (retired) slots_[current_slot_]->keep_alive.push_back(std::move(retired));
        DlaaPass::Resources& res = dlaa_->resources();
        touch(depth);

        // The previous evaluation has to belong to the frame before this one for the history to apply.
        bool reset = recreated || !dlaa_last_flip_ || *dlaa_last_flip_ + 1 != stats_.flips || dlaa_switched_;
        // Without per-object motion vectors the history is reprojected by the camera alone, from the projections of
        // the frame's scene draws; Off and Reset describe no camera motion.
        DlaaCameraTracker::Result camera;
        // Frame generation takes a camera cut only where the scene itself jumps (see kFgCameraCutProbeZ).
        bool fg_reset = reset;
        if (options_.dlaa_debug.camera == DlaaCamera::On) {
            camera = dlaa_camera_.resolve(options_.dlaa_debug.flip_clip_y);
            reset = reset || camera.reset;
            fg_reset = fg_reset || camera.fg_reset;
        } else {
            reset = reset || options_.dlaa_debug.camera == DlaaCamera::Reset;
            fg_reset = reset;
        }

        if (!res.motion_cleared) {
            transitionImage(res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, res.motion.layout,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(outsideCmd(), res.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                                 &range);
            transitionImage(res.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, res.motion.layout,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            res.motion_cleared = true;
        }
        // Streamline samples the depth in a compute pass, so it is tagged as a shader-read image, not as a
        // depth-stencil read-only attachment.
        transitionSidecar(scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionSidecar(*depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionImage(res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, res.output.layout, VK_IMAGE_LAYOUT_GENERAL);

        const auto describe = [&](const RenderTarget& target, VkImageView view, VkImageLayout layout) {
            SlImage image;
            image.image = target.msaa->image;
            VmaAllocationInfo allocated{};
            vmaGetAllocationInfo(context_.allocator(), target.msaa->allocation, &allocated);
            image.memory = allocated.deviceMemory;
            image.view = view;
            image.layout = layout;
            image.width = target.passWidth(true);
            image.height = target.passHeight(true);
            image.format = target.format;
            image.usage = target.msaa->usage;
            return image;
        };
        DlaaPass::Inputs inputs;
        inputs.color_in = describe(scene, scene.msaa->view, scene.msaa->layout);
        inputs.depth = describe(*depth,
                                depth->msaa->depth_view != VK_NULL_HANDLE ? depth->msaa->depth_view : depth->msaa->view,
                                depth->msaa->layout);
        inputs.frame_index = frame_index_;
        const VkCommandBuffer cmd = outsideCmd();
        // The offset the frame's perspective draws were shifted by. A frame without such a draw was not moved, so it
        // reports none. The flips only change what is reported: the geometry always moves toward +x / +y.
        if (dlaa_jitter_frame_ == stats_.flips) {
            const std::array<float, 2> jitter = dlaaJitterPixels(stats_.flips);
            inputs.jitter_px[0] = options_.dlaa_debug.flip_jitter_x ? -jitter[0] : jitter[0];
            inputs.jitter_px[1] = options_.dlaa_debug.flip_jitter_y ? -jitter[1] : jitter[1];
        }
        inputs.matrices = camera.matrices;
        if (options_.dlaa_debug.mvec_invalid_zero) inputs.mvec_invalid_value = 0.0f;
        inputs.reset = reset;
        // The alpha channel travels through DLSS: the resolve copies the scene alpha to the display buffer, and a
        // later draw uses that alpha as a blend factor.
        inputs.alpha = true;
        const bool evaluated = dlaa_->evaluate(cmd, inputs, error);
        // Streamline binds its own pipelines, descriptors and state and does not restore ours (manual hooking).
        // recordDraw re-records everything the shadow covers, and it binds vertex buffers, the index buffer and the
        // push descriptors on every draw.
        invalidateDrawState();
        if (!evaluated) return failed(error);

        // Frame generation reads the same depth and a camera motion vector image. The second evaluation of a frame
        // (the scene written again after the tap) is neither tagged nor checked again.
        const bool fg_inputs =
                frame_gen_ && camera.clip && fg_tapped_.frame != frame_index_ && frame_gen_->wantsInputs();
        const bool mv_check = options_.fg_debug.mv_check && !fg_mv_pending_;
        if (fg_inputs || mv_check) {
            // Failures leave the scene read as it is.
            std::string check_error;
            bool recorded = recordFgMotion(res, *depth, camera, check_error);
            if (!recorded) {
                logOnce(LogLevel::Warn, "render-fg-motion-failed", "tox2: render: motion vectors not recorded ({})",
                        check_error);
            }
            if (fg_inputs) {
                fg_tapped_ = {frame_index_, FrameGenInputs::Failed, width, height, inputs.depth.format};
                std::string tag_error;
                SlImage depth_copy;
                if (recorded && copyFgDepth(*depth, depth_copy, tag_error) &&
                    tagFgInputs(depth_copy, inputs, camera, width, height, fg_reset, outsideCmd(), tag_error)) {
                    fg_tapped_.inputs = FrameGenInputs::Tagged;
                } else if (recorded) {
                    logOnce(LogLevel::Warn, "render-fg-tag-failed",
                            "tox2: render: frame generation inputs not tagged ({})", tag_error);
                }
            }
            if (recorded && mv_check && !recordFgMvCheck(res, fg_reset, check_error)) {
                logOnce(LogLevel::Warn, "render-fg-mv-failed", "tox2: render: motion vector check failed ({})",
                        check_error);
            }
            invalidateDrawState();
        }

        // Streamline returns the output to the layout it was told.
        transitionImage(res.output.image, VK_IMAGE_ASPECT_COLOR_BIT, res.output.layout,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionImage(res.expanded.image, VK_IMAGE_ASPECT_COLOR_BIT, res.expanded.layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[1] = {static_cast<std::int32_t>(res.expanded.width), static_cast<std::int32_t>(height), 1};
        vkCmdBlitImage(outsideCmd(), res.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, res.expanded.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
        transitionImage(res.expanded.image, VK_IMAGE_ASPECT_COLOR_BIT, res.expanded.layout,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        scene.dlaa_evaluated = true;
        scene.dlaa_depth.reset();
        dlaa_output_owner_ = &scene;
        dlaa_last_flip_ = stats_.flips;
        dlaa_switched_ = false;
        stats_.dlaa_evaluations++;
        if (reset) stats_.dlaa_resets++;
        logOnce(LogLevel::Info, std::format("render-dlaa-evaluating-{}x{}", width, height),
                "tox2: render: DLAA evaluating {}x{}", width, height);
    }

    DlaaPass::Image& expanded = dlaa_->resources().expanded;
    VkImageView& view = expanded.sampled_views[remap];
    if (view == VK_NULL_HANDLE) {
        view = textures_->createView(expanded.image, expanded.format, remap);
        if (view == VK_NULL_HANDLE) {
            expanded.sampled_views.erase(remap);
            fail(textures_->error());
        }
    }
    return view;
}

// The depth that went into the DLAA evaluation is still in SHADER_READ_ONLY_OPTIMAL and the DLAA output is in GENERAL,
// which is where frame generation will read them. The pass only reads those images, so the scene is unchanged.
bool RsxRenderer::recordFgMotion(const DlaaPass::Resources& dlaa, const RenderTarget& depth,
                                 const DlaaCameraTracker::Result& camera, std::string& error) {
    if (!fg_motion_) fg_motion_ = std::make_unique<FgMotionPass>(context_);
    std::shared_ptr<void> retired;
    bool recreated = false;
    if (!fg_motion_->prepare(dlaa.output.width, dlaa.output.height, retired, recreated, error)) return false;
    // Frames in flight may still read the previous images.
    if (retired) slots_[current_slot_]->keep_alive.push_back(std::move(retired));

    FgMotionInputs inputs;
    // The scene pass wrote the object image for this frame only when a candidate draw reached an instance.
    fg_motion_object_ = options_.dlaa_debug.motion == DlaaMotion::Object && fg_motion_->hasObjectWrites(frame_index_);
    inputs.object = fg_motion_object_;
    inputs.depth = depth.msaa->depth_view != VK_NULL_HANDLE ? depth.msaa->depth_view : depth.msaa->view;
    // The camera pair gives the same clip-to-clip transform as the DLAA matrices, without their y flip.
    Mat4 inverse{};
    if (camera.clip && camera.prev_clip && mat4Inverse(*camera.clip, inverse)) {
        inputs.clip_to_prev_clip = mat4Multiply(inverse, *camera.prev_clip);
    }
    inputs.flip[0] = options_.fg_debug.mv_flip_x ? -1.0f : 1.0f;
    inputs.flip[1] = options_.fg_debug.mv_flip_y ? -1.0f : 1.0f;

    const VkCommandBuffer cmd = outsideCmd();
    // Streamline's writes to the output are not ordered against fragment reads by anything it records.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);

    return fg_motion_->recordMotion(cmd, inputs, error);
}

bool RsxRenderer::recordFgMvCheck(const DlaaPass::Resources& dlaa, bool reset, std::string& error) {
    if (!fg_motion_->recordWarpCheck(outsideCmd(), dlaa.output.view, VK_IMAGE_LAYOUT_GENERAL, fg_motion_->motionView(),
                                     error, fg_motion_object_ ? fg_motion_->cameraView() : VK_NULL_HANDLE)) {
        return false;
    }
    fg_mv_pending_ = true;
    fg_mv_reset_ = reset;
    return true;
}

// Constants and the depth and motion vectors of the frame, for the frame generation viewport. DLSS-G reads both images
// when the frame is presented, so they are tagged valid until then: the motion vectors are rewritten only by the next
// frame, and the depth is a copy of its own. Streamline blocks the presenting queue until DLSS-G has read its inputs,
// which keeps that frame from starting too early.
bool RsxRenderer::tagFgInputs(const SlImage& depth, const DlaaPass::Inputs& inputs,
                              const DlaaCameraTracker::Result& camera, std::uint32_t width, std::uint32_t height,
                              bool reset, VkCommandBuffer cmd, std::string& error) {
    FrameGenFrame frame;
    frame.depth = depth;
    frame.motion = fg_motion_->motionImage();
    frame.jitter_px[0] = inputs.jitter_px[0];
    frame.jitter_px[1] = inputs.jitter_px[1];
    frame.mvec_scale[0] = 1.0f / static_cast<float>(width);
    frame.mvec_scale[1] = 1.0f / static_cast<float>(height);
    frame.view_to_clip = camera.matrices.view_to_clip;
    frame.clip_to_view = camera.matrices.clip_to_view;
    frame.clip_to_prev_clip = camera.matrices.clip_to_prev_clip;
    frame.prev_clip_to_clip = camera.matrices.prev_clip_to_clip;
    // The fit expects clip y up; the renderer's clip space is y down, which would hand Streamline a mirrored basis.
    Mat4 world_to_clip_y_up = camera.matrices.view_to_clip;
    for (std::size_t row = 0; row < 4; ++row) world_to_clip_y_up[row * 4 + 1] = -world_to_clip_y_up[row * 4 + 1];
    if (const std::optional<FgCamera> fitted = fgCameraFromClip(world_to_clip_y_up)) {
        frame.camera = *fitted;
    } else {
        // An orthonormal basis, so scene change detection finds nothing to complain about.
        frame.camera.aspect = static_cast<float>(width) / static_cast<float>(height);
    }
    const bool size_changed = fg_scene_width_ != width || fg_scene_height_ != height;
    fg_scene_width_ = width;
    fg_scene_height_ = height;
    // takeReset() also consumes the rate event, so it runs for every tagged frame.
    frame.reset = frame_gen_->takeReset() || reset || size_changed;
    return context_.streamline()->tagFrameGen(cmd, frame, error);
}

struct RsxRenderer::FgDepthCopy {
    VkContext* context = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;  // depth aspect only
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    std::uint32_t width = 0, height = 0;

    FgDepthCopy() = default;
    FgDepthCopy(const FgDepthCopy&) = delete;
    FgDepthCopy& operator=(const FgDepthCopy&) = delete;
    ~FgDepthCopy() {
        if (context == nullptr) return;
        if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
        if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
    }
};

// Tagging the depth sidecar itself as valid only now makes Streamline copy it into a clone of its own, and the
// validation layer reports that clone in the wrong layout at every submit (measured). A tag that is valid until present
// needs an image no later pass writes, so the depth gets its own, one per frame slot.
bool RsxRenderer::copyFgDepth(RenderTarget& depth, SlImage& out, std::string& error) {
    const std::uint32_t width = depth.passWidth(true), height = depth.passHeight(true);
    std::shared_ptr<FgDepthCopy>& copy = fg_depth_[current_slot_];
    if (!copy || copy->width != width || copy->height != height || copy->format != depth.format) {
        // Frames in flight may still read the previous image.
        if (copy) slots_[current_slot_]->keep_alive.push_back(std::move(copy));
        auto created = std::make_shared<FgDepthCopy>();
        created->context = &context_;
        constexpr VkImageUsageFlags kUsage =
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = depth.format;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = kUsage;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VmaAllocationInfo allocated{};
        if (vmaCreateImage(context_.allocator(), &image_info, &alloc_info, &created->image, &created->allocation,
                           &allocated) != VK_SUCCESS) {
            created->image = VK_NULL_HANDLE;
            error = "vmaCreateImage failed for the frame generation depth";
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = created->image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(context_.device(), &view_info, nullptr, &created->view) != VK_SUCCESS) {
            created->view = VK_NULL_HANDLE;
            error = "vkCreateImageView failed for the frame generation depth";
            return false;
        }
        created->format = depth.format;
        created->usage = kUsage;
        created->width = width;
        created->height = height;
        copy = std::move(created);
    }

    transitionSidecar(depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionImage(copy->image, depth.aspect, copy->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vkCmdCopyImage(outsideCmd(), depth.msaa->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(copy->image, depth.aspect, copy->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionSidecar(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VmaAllocationInfo allocated{};
    vmaGetAllocationInfo(context_.allocator(), copy->allocation, &allocated);
    out.image = copy->image;
    out.memory = allocated.deviceMemory;
    out.view = copy->view;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.width = width;
    out.height = height;
    out.format = copy->format;
    out.usage = copy->usage;
    return true;
}

void RsxRenderer::collectFgMvCheck() {
    if (!fg_mv_pending_) return;
    fg_mv_pending_ = false;
    // The flip just submitted the frame that recorded the check.
    if (!fg_motion_ || !waitSlot(*slots_[display_slot_])) return;
    FgWarpStats warp;
    if (!fg_motion_->collectWarpStats(warp)) return;  // the first check only seeds the history
    stats_.fg_mv_checks++;
    stats_.fg_mv_last = warp;
    stats_.fg_mv_last_reset = fg_mv_reset_;
}

// A single-sample pass is about to use the layout image of `target`: brings it up to date with the sidecar first
// (unless the pass replaces all of it), and records that the sidecar is behind when the pass writes.
bool RsxRenderer::beginLayoutPass(RenderTarget& target, bool writes, bool overwrites_all) {
    if (target.msaa && target.layout_stale) {
        if (overwrites_all) {
            target.layout_stale = false;
        } else if (!syncLayout(target)) {
            return false;
        }
    }
    if (writes) {
        target.layout_written = true;
        target.dlaa_evaluated = false;
        if (target.msaa) target.msaa_stale = true;
    }
    return true;
}

// A sidecar pass is about to use the sidecar of `target`. The layout image is never copied back into the
// sidecar: when it holds newer data than the sidecar and the pass does not replace all of it, the pass draws over
// the older sidecar contents and is counted.
void RsxRenderer::beginSidecarPass(RenderTarget& target, bool writes, bool overwrites_all) {
    if (overwrites_all) {
        target.msaa_stale = false;
    } else if (target.msaa_stale) {
        rsxNoteUnimplemented("state", "anti-aliased pass over layout data");
        target.msaa_stale = false;
    }
    if (writes) {
        target.layout_stale = true;
        target.dlaa_evaluated = false;
    }
}

namespace {

// Formats whose texels can be copied bit for bit: equal formats, and the two 32-bit colour formats (the
// guest memory holds the same words for both; RPCS3 `VK/VKFormats.cpp` `formats_are_bitcast_compatible`).
bool bitcastCompatible(VkFormat a, VkFormat b) {
    const auto is_32bit_color = [](VkFormat format) {
        return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R32_SFLOAT;
    };
    return a == b || (is_32bit_color(a) && is_32bit_color(b));
}

}  // namespace

// One entry per (location, offset) and kind. Another size or format at the same offset replaces the entry
// and the new target inherits the overlap of the old one (inheritContents), as RPCS3's surface store does
// (Common/surface_store.h, Common/surface_utils.h). The old target stays alive for the commands already
// recorded against it. `overwrites_all` is true for a clear that writes every channel of the whole target:
// the old contents would be lost at once, and RPCS3 skips the copy then too (VK/VKGSRender.cpp clear_surface).
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquire(TargetMap& map, bool depth, const RsxSurface& surface,
                                                                VkFormat format, bool overwrites_all) {
    const TargetKey key{surface.location, surface.offset};
    const auto it = map.find(key);
    std::shared_ptr<RenderTarget> previous;
    if (it != map.end()) {
        std::shared_ptr<RenderTarget> existing = it->second;
        // A scale change reaches a target here, when something binds it again; targets only sampled or flipped
        // keep their image, and every path that reads one uses its own image size.
        if (existing->scale != targetScale(existing->layout_width, existing->layout_height)) {
            std::shared_ptr<RenderTarget> rescaled = rescaleTarget(existing);
            if (!rescaled) return nullptr;
            it->second = rescaled;
            existing = std::move(rescaled);
        }
        // The identity is the layout size: the scale is the renderer's choice, not part of the guest memory. An
        // anti-aliased binding of a W x H clip and a plain binding of 2W x H are the same memory and the same image.
        if (existing->layout_width == layoutWidth(surface) && existing->layout_height == layoutHeight(surface) &&
            existing->format == format) {
            touch(existing);
            return existing;
        }
        previous = existing;
        map.erase(it);
    }
    std::shared_ptr<RenderTarget> target =
            createTarget(depth, layoutWidth(surface), layoutHeight(surface), surface.pitch, format);
    if (!target) return nullptr;
    map[key] = target;
    touch(target);
    if (previous && !overwrites_all) inheritContents(previous, *target);
    return target;
}

// A copy of `previous` at the scale a target of its layout gets now, filled by a whole-image blit. Enlarging
// replicates texels (NEAREST is exact); shrinking a colour target filters LINEAR, which for 2:1 averages the
// 2x2 block. Float and depth data are never filtered. The copy has no depth-as-colour image and no sampled
// views; they are rebuilt on demand. A format that cannot be blitted leaves the copy at its initial contents
// and is counted. The caller replaces the cache entry; the old image stays alive for the commands recorded
// against it.
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::rescaleTarget(const std::shared_ptr<RenderTarget>& previous) {
    std::shared_ptr<RenderTarget> target = createTarget(previous->depth, previous->layout_width,
                                                        previous->layout_height, previous->pitch, previous->format);
    if (!target) return nullptr;
    touch(target);

    VkFormatProperties format_props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), previous->format, &format_props);
    constexpr VkFormatFeatureFlags kBlitFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((format_props.optimalTilingFeatures & kBlitFeatures) != kBlitFeatures) {
        rsxNoteUnimplemented("state", previous->depth ? "rescale of a depth target unavailable"
                                                      : "rescale of a colour target unavailable");
        return target;
    }

    const bool filter = target->scale < previous->scale && previous->format == VK_FORMAT_B8G8R8A8_UNORM &&
                        (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    touch(previous);
    if (!syncLayout(*previous)) return nullptr;
    transition(*previous, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // orders the blit after the initial clear
    VkImageBlit region{};
    region.srcSubresource = {previous->aspect, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<std::int32_t>(previous->image_width),
                            static_cast<std::int32_t>(previous->image_height), 1};
    region.dstSubresource = {target->aspect, 0, 0, 1};
    region.dstOffsets[1] = {static_cast<std::int32_t>(target->image_width),
                            static_cast<std::int32_t>(target->image_height), 1};
    vkCmdBlitImage(outsideCmd(), previous->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    target->layout_written = true;
    return target;
}

// Copies the overlap (origin (0, 0), in layout pixels) of the previous target at the same address into a target that
// was just created, so a resized surface keeps what was drawn. Differences from RPCS3: it copies lazily at
// the first barrier that needs the data, this copy is recorded at once; the parts of a larger old surface
// outside the new extent are dropped; other bytes per pixel, other depth formats and colour/depth pairs are
// not converted; a guest write between the two bindings is not noticed (the renderer never reads guest
// memory). A pair that cannot be copied leaves the new target at its initial contents and is counted. The
// colour/depth check is a guard: the two kinds live in separate maps, so they never meet here today.
void RsxRenderer::inheritContents(const std::shared_ptr<RenderTarget>& previous, RenderTarget& target) {
    if (previous->pitch != target.pitch) {
        rsxNoteUnimplemented("state", "surface resize with another pitch");
        return;
    }
    if (previous->depth != target.depth || !bitcastCompatible(previous->format, target.format)) {
        rsxNoteUnimplemented("state", "surface resize to an incompatible format");
        return;
    }

    // Only the size-limit fallback gives two targets different scales; a copy would need a resampling pass.
    if (previous->scale != target.scale) {
        rsxNoteUnimplemented("state", "surface resize across resolution scales");
        return;
    }

    touch(previous);  // it may not have been used in this frame slot yet, and the copy reads it until the fence
    if (!syncLayout(*previous)) return;
    transition(*previous, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // orders the copy after the initial clear
    VkImageCopy region{};
    region.srcSubresource = {previous->aspect, 0, 0, 1};
    region.dstSubresource = {target.aspect, 0, 0, 1};
    // The layout overlap at the common scale.
    region.extent = {std::min(previous->layout_width, target.layout_width) * target.scale,
                     std::min(previous->layout_height, target.layout_height) * target.scale, 1};
    vkCmdCopyImage(outsideCmd(), previous->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    target.layout_written = true;
    stats_.surface_inherits++;
}

// RPCS3 `VK/VKGSRender.cpp` `get_compatible_surface_format`: F_X32 is R32_SFLOAT. The fragment program writes
// its usual four-component color and the attachment keeps the red channel; no output swizzle is applied
// (RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP is only set for the integer formats in `RSXThread.cpp`).
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquireColor(const RsxSurface& surface, bool overwrites_all) {
    switch (surface.format) {
        case RsxColorFormat::A8R8G8B8:
            return acquire(color_targets_, false, surface, VK_FORMAT_B8G8R8A8_UNORM, overwrites_all);
        case RsxColorFormat::F_X32:
            return acquire(color_targets_, false, surface, VK_FORMAT_R32_SFLOAT, overwrites_all);
        default: return nullptr;
    }
}

std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquireDepth(const RsxSurfaceState& surfaces,
                                                                     bool overwrites_all) {
    switch (surfaces.depth_format) {
        case RsxDepthFormat::Z24S8:
            return acquire(depth_targets_, true, surfaces.depth, context_.depthStencilFormat(), overwrites_all);
        case RsxDepthFormat::Z16:
            return acquire(depth_targets_, true, surfaces.depth, VK_FORMAT_D16_UNORM, overwrites_all);
        default: return nullptr;
    }
}

bool RsxRenderer::allocUpload(std::size_t size, UploadRef& out) {
    FrameSlot& slot = *slots_[current_slot_];
    size = std::max<std::size_t>((size + kUploadAlignment - 1) & ~(kUploadAlignment - 1), kUploadAlignment);
    FrameSlot::UploadBlock* block = nullptr;
    for (FrameSlot::UploadBlock& candidate : slot.uploads) {
        if (candidate.size - candidate.used >= size) {
            block = &candidate;
            break;
        }
    }
    if (block == nullptr) {
        FrameSlot::UploadBlock created;
        created.size = std::max(kUploadBlockSize, size);
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = created.size;
        buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &created.buffer, &created.allocation,
                            &mapped) != VK_SUCCESS ||
            mapped.pMappedData == nullptr) {
            fail("vmaCreateBuffer failed for the draw upload buffer");
            return false;
        }
        created.mapped = static_cast<std::uint8_t*>(mapped.pMappedData);
        if (stats_.upload_memory_flags == 0) {
            vmaGetAllocationMemoryProperties(context_.allocator(), created.allocation, &stats_.upload_memory_flags);
        }
        noteImageCreated(RenderImageKind::UploadBlock, created.size);
        slot.uploads.push_back(created);
        block = &slot.uploads.back();
    }
    out.buffer = block->buffer;
    out.offset = block->used;
    out.data = block->mapped + block->used;
    block->used += size;
    return true;
}

// The placeholders are one magenta texel each (a 2D image, a 3D image and a cube map of six such faces),
// written with a clear so no staging buffer is needed.
bool RsxRenderer::ensurePlaceholder() {
    if (placeholder_ready_) return true;
    const VkDevice device = context_.device();

    constexpr VkImageType kImageTypes[3] = {VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D, VK_IMAGE_TYPE_2D};
    constexpr VkImageViewType kViewTypes[3] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE};
    for (std::uint32_t type = 0; type < 3; type++) {
        const std::uint32_t layers = type == kFpTextureTypeCube ? 6 : 1;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.flags = type == kFpTextureTypeCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        image_info.imageType = kImageTypes[type];
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {1, 1, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = layers;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        Placeholder& placeholder = placeholders_[type];
        if (vmaCreateImage(context_.allocator(), &image_info, &alloc_info, &placeholder.image, &placeholder.allocation,
                           nullptr) != VK_SUCCESS) {
            placeholder.image = VK_NULL_HANDLE;
            fail("vmaCreateImage failed for the placeholder texture");
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = placeholder.image;
        view_info.viewType = kViewTypes[type];
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        if (vkCreateImageView(device, &view_info, nullptr, &placeholder.view) != VK_SUCCESS) {
            placeholder.view = VK_NULL_HANDLE;
            fail("vkCreateImageView failed for the placeholder texture");
            return false;
        }
    }
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &sampler_info, nullptr, &placeholder_sampler_) != VK_SUCCESS) {
        placeholder_sampler_ = VK_NULL_HANDLE;
        fail("vkCreateSampler failed");
        return false;
    }

    const VkCommandBuffer cmd = outsideCmd();
    const auto barrier = [&](const Placeholder& placeholder, std::uint32_t layers, VkImageLayout from, VkImageLayout to,
                             VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = stage;
        b.dstAccessMask = access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = placeholder.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    VkClearColorValue magenta{};
    magenta.float32[0] = 1.0f;
    magenta.float32[2] = 1.0f;
    magenta.float32[3] = 1.0f;
    for (std::uint32_t type = 0; type < 3; type++) {
        const std::uint32_t layers = type == kFpTextureTypeCube ? 6 : 1;
        barrier(placeholders_[type], layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdClearColorImage(cmd, placeholders_[type].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
        barrier(placeholders_[type], layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    placeholder_ready_ = true;
    return true;
}

// Scaled vertex formats are optional for buffers; the others used here are mandatory.
bool RsxRenderer::vertexFormatSupported(VkFormat format) {
    const auto it = vertex_format_support_.find(format);
    if (it != vertex_format_support_.end()) return it->second;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    const bool supported = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    vertex_format_support_[format] = supported;
    return supported;
}

// Blending into a float format is optional in Vulkan (R32_SFLOAT has no mandatory blend support).
bool RsxRenderer::blendSupported(VkFormat format) {
    const auto it = blend_support_.find(format);
    if (it != blend_support_.end()) return it->second;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    const bool supported = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    blend_support_[format] = supported;
    return supported;
}

// The RSX reads a Z24S8 surface through an A8R8G8B8 texture as the raw words of the surface (RPCS3 samples the
// depth and stencil aspects and rebuilds the bytes in the fragment shader, `RSXFragmentTextureDepthConversion.glsl`).
// Here a pass writes those texels into a B8G8R8A8 image the size of the surface, which the draw then samples
// like any A8R8G8B8 texture, so the texture's remap applies to it as usual and the depth attachment of the
// same draw is not read while it is written. The pass runs outside any rendering instance. The image is kept
// with the target and made again only after the depth was cleared or drawn to. A surface without stencil (Z16)
// has no A8R8G8B8 form. Returns null, with a counted reason, when there is no image to sample.
//
// When the 2-sample depth holds writes the layout image lacks, the words are built straight from it (layoutSource
// picks the sample of every layout texel), so reads of an anti-aliased surface never wait for a layout copy of the
// stencil. A 1-sample depth is copied into the layout image first and converted from there like a plain surface.
std::shared_ptr<TextureImage> RsxRenderer::depthAsArgb(RenderTarget& depth) {
    if (!formatHasStencil(depth.format) || !depth.sampled) {
        rsxNoteUnimplemented("texture format", "A8R8G8B8 read of a surface without Z24S8");
        return nullptr;
    }
    if (depth.argb && depth.argb_serial == depth.write_serial) return depth.argb;
    if (depth.msaa && depth.msaa->samples == VK_SAMPLE_COUNT_1_BIT && !syncLayout(depth)) return nullptr;
    const bool from_sidecar = depth.msaa && depth.msaa->samples == VK_SAMPLE_COUNT_2_BIT && depth.layout_stale;

    if (!depth.argb) {
        ImagePoolResult pooled;
        depth.argb = textures_->renderable(depth.image_width, depth.image_height, &pooled);
        if (!textures_->valid()) {
            fail(textures_->error());
            return nullptr;
        }
        if (!depth.argb) return nullptr;
        if (pooled.hit) {
            stats_.image_pool_hits[static_cast<std::size_t>(RenderImageKind::DepthArgb)]++;
        } else {
            stats_.image_pool_misses[static_cast<std::size_t>(pooled.miss)]++;
            noteImageCreated(RenderImageKind::DepthArgb, depth.argb->bytes);
        }
    }
    const auto aspectView = [&](VkImageAspectFlags aspect, VkImageView& view) {
        if (from_sidecar || view != VK_NULL_HANDLE) return true;
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = depth.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth.format;
        view_info.subresourceRange = {aspect, 0, 1, 0, 1};
        if (vkCreateImageView(context_.device(), &view_info, nullptr, &view) != VK_SUCCESS) {
            view = VK_NULL_HANDLE;
            fail("vkCreateImageView failed for a depth view");
            return false;
        }
        return true;
    };
    if (!aspectView(VK_IMAGE_ASPECT_DEPTH_BIT, depth.depth_view) ||
        !aspectView(VK_IMAGE_ASPECT_STENCIL_BIT, depth.stencil_view)) {
        return nullptr;
    }
    const VkImageView target_view = textures_->view(*depth.argb, kIdentityRemap);
    if (!textures_->valid()) {
        fail(textures_->error());
        return nullptr;
    }
    const VkPipeline pipeline = pipelines_->depthConvertPipeline(from_sidecar);
    if (pipeline == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE) {
        if (!pipelines_->valid()) fail(pipelines_->error());
        return nullptr;
    }

    const VkCommandBuffer cmd = outsideCmd();
    invalidateDrawState();  // binds the depth conversion pipeline
    if (from_sidecar) {
        transitionSidecar(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    } else {
        transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    const auto colorBarrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 dst_stage,
                                  VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = depth.argb->image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    // The previous contents are overwritten in full, so the old layout is not preserved.
    colorBarrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = target_view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {depth.image_width, depth.image_height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    const VkViewport viewport = {
            0.0f, 0.0f, static_cast<float>(depth.image_width), static_cast<float>(depth.image_height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {depth.image_width, depth.image_height}};
    const VkImageView depth_view = from_sidecar ? depth.msaa->depth_view : depth.depth_view;
    const VkImageView stencil_view = from_sidecar ? depth.msaa->stencil_view : depth.stencil_view;
    const VkDescriptorImageInfo infos[2] = {
            {placeholder_sampler_, depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {placeholder_sampler_, stencil_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    LayoutPassConstants constants{};
    constants.column_sample[0] = static_cast<std::int32_t>(kLayoutColumnSample[0]);
    constants.column_sample[1] = static_cast<std::int32_t>(kLayoutColumnSample[1]);
    constants.stencil_bit = -1;
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }

    gpuBeginInstance(cmd, GpuInstanceKind::DepthArgb, depth.image_width, depth.image_height, 1,
                     VK_FORMAT_B8G8R8A8_UNORM, false);
    vkCmdBeginRendering(cmd, &rendering);
    stats_.rendering_instances++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layoutPassLayout(), 0, 2, writes);
    vkCmdPushConstants(cmd, pipelines_->layoutPassLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    gpuEndInstance(cmd);
    colorBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    depth.argb->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.argb_serial = depth.write_serial;
    stats_.depth_conversions++;
    return depth.argb;
}

// Fills the image info and coordinate scale of every texture unit the draw's fragment program samples.
// A unit without a usable texture keeps the placeholder, and every such case is counted with
// rsxNoteUnimplemented, so a census without texture kinds means no placeholder was sampled. A texture image is
// uploaded only when the content id of the blob differs from the one it received last: the frame builder gives
// an unchanged guest texture the same blob and id in every draw, and a new id once the guest bytes changed.
//
// A texture at a known render target samples that target's image. When the target is the one the draw
// renders to (a cyclic reference), the draw would read and write the same image: it samples a copy
// taken now instead. The copy is a snapshot, so the result does not depend on rasterization order. RPCS3
// instead keeps the surface in GENERAL or ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT layout and uses texture
// barriers (`VK/VKDraw.cpp` `validate_image_layout_for_read_access`); the extension is not enabled here
// and GENERAL leaves reads of pixels still in flight undefined, so the copy is used.
bool RsxRenderer::prepareTextures(const RsxDraw& draw, const DrawShaders& shaders, RenderTarget& color,
                                  bool sidecar_pass, VkDescriptorImageInfo (&infos)[kFpTextureUnitCount],
                                  float (&scales)[kFpTextureUnitCount][4]) {
    FrameSlot& slot = *slots_[current_slot_];
    const auto reference = [&](const std::shared_ptr<TextureImage>& image) {
        if (std::find(slot.texture_refs.begin(), slot.texture_refs.end(), image) == slot.texture_refs.end()) {
            slot.texture_refs.push_back(image);
        }
    };

    std::vector<const TextureImage*> uploaded;  // uploaded for this draw already (several units, one image)
    std::shared_ptr<TextureImage> feedback_copy;
    for (std::uint32_t unit = 0; unit < kFpTextureUnitCount; unit++) {
        // The placeholder has the view type of the sampler the program declares for the unit.
        const std::uint32_t declared = fpTextureType(shaders.texture_types, unit);
        infos[unit] = {placeholder_sampler_, placeholders_[declared < 3 ? declared : 0].view,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        scales[unit][0] = scales[unit][1] = scales[unit][2] = scales[unit][3] = 1.0f;
        if ((shaders.texture_units & (1u << unit)) == 0) continue;

        const RsxTextureDesc* texture = nullptr;
        for (const RsxTextureDesc& candidate : draw.textures) {
            if (candidate.unit == unit) texture = &candidate;
        }
        if (texture == nullptr) {
            rsxNoteUnimplemented("texture", "sampled unit is disabled");
            continue;
        }
        // Unnormalized coordinates are divided by the size in the shader (fp_translator.h). A cube map has no
        // unnormalized form, and the cube lookup does not read the scale.
        const RsxTextureKind kind = rsxTextureKind(*texture);
        if (!texture->normalized && texture->width != 0 && texture->height != 0) {
            scales[unit][0] = 1.0f / static_cast<float>(texture->width);
            scales[unit][1] = 1.0f / static_cast<float>(texture->height);
            if (kind == RsxTextureKind::Tex3D) scales[unit][2] = 1.0f / static_cast<float>(texture->depth);
        }

        VkImageView view = VK_NULL_HANDLE;
        VkFormat sampled_format = VK_FORMAT_UNDEFINED;  // of the image read, when it limits the sampler
        if (texture->rt_alias >= 0 && texture->rt_depth) {
            if (kind != RsxTextureKind::Tex2D) {
                rsxNoteUnimplemented("texture layout", "render target alias of a 3D or cube texture");
                continue;
            }
            // The only way a depth surface is read here is as the A8R8G8B8 words of Z24S8 (depthAsArgb).
            if ((texture->format & kRsxTextureBaseMask) != kRsxTextureA8R8G8B8) {
                rsxNoteUnimplemented("texture format",
                                     std::format("{} of a depth surface", rsxTextureFormatName(texture->format)));
                continue;
            }
            const auto found = depth_targets_.find(TargetKey{texture->location, texture->offset});
            if (found == depth_targets_.end()) {
                rsxNoteUnimplemented("texture", "alias of unknown render target");
                rsxNoteUnimplemented("texture", "placeholder: render target unavailable");
                continue;
            }
            const std::shared_ptr<RenderTarget> target = found->second;
            touch(target);
            // The layout includes the samples of an anti-aliased surface, so the game's 2W x H read of a W x H
            // clip matches; only a texture that is not an AA layout of the target stays an approximation.
            if (target->layout_width != texture->width || target->layout_height != texture->height) {
                rsxNoteUnimplemented("texture layout", "render target alias of another size");
            }
            const std::shared_ptr<TextureImage> converted = depthAsArgb(*target);
            if (!valid()) return false;
            if (!converted) {
                rsxNoteUnimplemented("texture", "placeholder: depth conversion unavailable");
                continue;
            }
            reference(converted);
            view = textures_->view(*converted, texture->remap);
        } else if (texture->rt_alias >= 0) {
            if (kind != RsxTextureKind::Tex2D) {
                rsxNoteUnimplemented("texture layout", "render target alias of a 3D or cube texture");
                continue;
            }
            // A color target holds A8R8G8B8 or 32-bit float data and is read through the matching texture format
            // only: float bits are not reinterpreted as bytes or the other way round.
            const std::uint32_t base_format = texture->format & kRsxTextureBaseMask;
            const VkFormat wanted = base_format == kRsxTextureA8R8G8B8   ? VK_FORMAT_B8G8R8A8_UNORM
                                    : base_format == kRsxTextureX32Float ? VK_FORMAT_R32_SFLOAT
                                                                         : VK_FORMAT_UNDEFINED;
            if (wanted == VK_FORMAT_UNDEFINED) {
                rsxNoteUnimplemented("texture format", rsxTextureFormatName(texture->format));
                continue;
            }
            std::shared_ptr<RenderTarget> target;
            const auto found = color_targets_.find(TargetKey{texture->location, texture->offset});
            if (found != color_targets_.end()) {
                target = found->second;
                touch(target);
                if (target->format != wanted) {
                    rsxNoteUnimplemented("texture layout", "render target alias of another format");
                    continue;
                }
                if (target->layout_width != texture->width || target->layout_height != texture->height) {
                    rsxNoteUnimplemented("texture layout", "render target alias of another size");
                }
            } else {
                // The target was drawn to before the commands the renderer has seen; it holds no data here.
                rsxNoteUnimplemented("texture", "alias of unknown render target");
                RsxSurface surface;
                surface.location = texture->location;
                surface.offset = texture->offset;
                surface.pitch = texture->pitch;
                surface.width = texture->width;
                surface.height = texture->height;
                surface.format = wanted == VK_FORMAT_R32_SFLOAT ? RsxColorFormat::F_X32 : RsxColorFormat::A8R8G8B8;
                target = acquireColor(surface, false);
                if (!valid()) return false;
                if (!target) {
                    rsxNoteUnimplemented("texture", "placeholder: render target unavailable");
                    continue;
                }
            }
            sampled_format = wanted;

            VkImageView dlaa_view = VK_NULL_HANDLE;
            if (dlaaTap(*target, color, sidecar_pass, *texture)) {
                dlaa_view = dlaaView(*target, texture->remap & 0xFFFF);
                if (!valid()) return false;
            }
            // A draw into the sidecar can sample the layout image of its own target directly once that is up to date.
            if (dlaa_view != VK_NULL_HANDLE) {
                view = dlaa_view;
            } else if (target.get() == &color && !sidecar_pass) {
                // The copy is a B8G8R8A8 image; there is none for a float target.
                if (wanted != VK_FORMAT_B8G8R8A8_UNORM) {
                    rsxNoteUnimplemented("texture", "placeholder: feedback copy unavailable");
                    continue;
                }
                if (!feedback_copy) {
                    feedback_copy = textures_->scratch(target->image_width, target->image_height);
                    if (!textures_->valid()) {
                        fail(textures_->error());
                        return false;
                    }
                    if (!feedback_copy) {
                        rsxNoteUnimplemented("texture", "placeholder: feedback copy unavailable");
                        continue;
                    }
                    reference(feedback_copy);
                    transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    textures_->recordCopy(outsideCmd(), *feedback_copy, target->image);
                    stats_.feedback_copies++;
                }
                view = textures_->view(*feedback_copy, texture->remap);
            } else {
                if (!syncLayout(*target)) return false;
                transition(*target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const std::uint32_t remap = texture->remap & 0xFFFF;
                // A float target is read as (x, x, x, x), so its own view (red only) is not the identity one.
                if (remap == kIdentityRemap && wanted == VK_FORMAT_B8G8R8A8_UNORM) {
                    view = target->view;
                } else {
                    VkImageView& cached = target->sampled_views[remap];
                    if (cached == VK_NULL_HANDLE) {
                        cached = textures_->createView(target->image, target->format, remap);
                    }
                    view = cached;
                    if (view == VK_NULL_HANDLE) target->sampled_views.erase(remap);
                }
            }
        } else if (texture->data != nullptr) {
            // The blob holds every level in tightly packed rows (see RsxTextureDesc::data), which is the
            // layout of a linear texture without padding.
            const std::vector<std::uint8_t>& blob = *texture->data;
            RsxTextureLayout& layout = texture_layout_;
            if (!rsxTextureLayout(texture->format, true, texture->width, texture->height, texture->mips, 0,
                                  kind == RsxTextureKind::Cube ? 6 : 1, layout,
                                  kind == RsxTextureKind::Tex3D ? texture->depth : 1) ||
                blob.size() < layout.packed_bytes) {
                rsxNoteUnimplemented("state", "texture data short");
                continue;
            }
            // Blocks and single-byte texels have no word order to fix.
            const bool compressed = rsxTextureBlockBytes(texture->format) != 0;
            const bool as_stored = compressed || (texture->format & kRsxTextureBaseMask) == kRsxTextureB8;
            const std::shared_ptr<TextureImage> image = textures_->acquire(*texture);
            if (!textures_->valid()) {
                fail(textures_->error());
                return false;
            }
            if (!image) {
                rsxNoteUnimplemented("texture", "placeholder: image unavailable");
                continue;
            }
            reference(image);
            // The image keeps what it was last given: a blob it already holds is not uploaded again. An id of 0
            // names no content, so such a blob is always uploaded. A second unit that names the same image
            // within one draw is left alone, whatever its id says, so the first unit's texels stay.
            const bool current = texture->content_id != 0 && image->content_id == texture->content_id;
            if (!current && std::find(uploaded.begin(), uploaded.end(), image.get()) == uploaded.end()) {
                UploadRef staging;
                if (!allocUpload(layout.packed_bytes, staging)) return false;
                const std::uint8_t* source = blob.data();
                if (as_stored) {
                    // Blocks and bytes are stored the way Vulkan expects them.
                    std::memcpy(staging.data, source, layout.packed_bytes);
                } else {
                    // Guest texels are big-endian words A, R, G, B in memory; byte swapped they are B8G8R8A8 texels.
                    for (std::size_t i = 0; i + 4 <= layout.packed_bytes; i += 4) {
                        std::uint32_t word;
                        std::memcpy(&word, source + i, 4);
                        word = std::byteswap(word);
                        std::memcpy(staging.data + i, &word, 4);
                    }
                }
                // The barrier before the copy waits for every earlier use of the image, so draws recorded
                // before this one still sample the old texels.
                textures_->recordUpload(outsideCmd(), *image, staging.buffer, staging.offset, layout.levels);
                image->content_id = texture->content_id;
                uploaded.push_back(image.get());
                stats_.texture_uploads++;
            }
            view = textures_->view(*image, texture->remap);
        } else {
            // The frame builder copied no bytes. It notes why for most cases, but not for a zero width or height.
            rsxNoteUnimplemented("texture", "placeholder: no texture data");
            continue;
        }

        // A render target has one level whatever the unit's mip count says.
        RsxTextureDesc sampled = *texture;
        if (texture->rt_alias >= 0) sampled.mips = 1;
        const VkSampler sampler = textures_->sampler(sampled, sampled_format);
        if (!textures_->valid()) {
            fail(textures_->error());
            return false;
        }
        if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) {
            rsxNoteUnimplemented("texture", "placeholder: view or sampler unavailable");
            continue;
        }
        infos[unit] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    return true;
}

// Draw path. Everything that records outside a rendering instance (target creation, the placeholder
// texture, uploads) happens before the draw's own commands and ends the instance a previous draw left open
// (outsideCmd). The instance is left open at the end of a draw, and at the end of a clear that has a colour
// attachment: the next draw with the same attachments and render area records into it, and the next command recorded
// outside one ends it. A draw that cannot be rendered is counted
// in draws_skipped and, where a reason exists, with rsxNoteUnimplemented.
//
// Facts used, with their sources:
//  - texture units a draw does not sample get no descriptor; the fragment translator reports the
//    sampled ones (GlslResult::texture_units);
//  - depth test and write are only enabled together with a depth target, and a write without the test
//    stays off (RPCS3 `VK/VKGSRender.cpp` `decode_rsx_state`);
//  - a draw that neither tests depth, writes it nor tests stencil binds no depth attachment (see below);
//  - the viewport covers the surface clip rectangle with a positive height: the vertex program already
//    applies the RSX viewport transform and does not flip y (RPCS3 `VKGSRender::set_viewport` uses
//    x 0, y 0 and the clip size); depth range is 0..1, the vertex program remaps depth with clip_min and
//    clip_max because the device does not enable unrestricted depth ranges (same function);
//  - front face and cull face map one to one (`vk::get_front_face`, `vk::get_cull_face`);
//  - GCM compare words 0x200..0x207 are in VkCompareOp order;
//  - stencil: the test runs with or without the depth test (a disabled depth test passes, so ZPASS applies),
//    the back face uses the BACK registers only with two-sided stencil on (the decoder copies the front face
//    otherwise), and it is only available with a Z24S8 surface (RPCS3 `VK/VKGSRender.cpp` `decode_rsx_state`,
//    `RSXThread.cpp`). The draw writes the depth surface when `rsxZetaWriteEnabled` says so;
//  - user clip planes are applied by the vertex program through gl_ClipDistance (`vp_translator.h`): the
//    control word travels in the draw uniforms and every plane is gated at run time by its control nibble and
//    output-mask bit (RPCS3 `Core/RSXDrawCommands.cpp`, `VK/VKVertexProgram.cpp`). Without shaderClipDistance
//    the draw is not clipped, which RPCS3 does not handle either (`VK/vkutils/device.cpp`).
void RsxRenderer::recordDraw(const RsxDraw& draw) {
    const auto skip = [&](std::string_view category, std::string_view key) {
        if (!category.empty()) rsxNoteUnimplemented(category, key);
        stats_.draws_skipped++;
    };

    // The targets are created even for a draw that is skipped, so a flip of one reads back as a render
    // target rather than as a missing one.
    std::shared_ptr<RenderTarget> color;
    if (draw.surfaces.has_color) color = acquireColor(draw.surfaces.color, false);
    // The game binds a depth surface to most post-processing and UI draws that neither test, write nor use stencil.
    // Such a draw runs without the attachment: acquiring it would resize or copy a depth target the draw never reads.
    // The one-sample (DLAA) scene pass is kept as it was, because its colour target remembers the latest depth it was
    // drawn with for the DLAA inputs.
    const VkSampleCountFlags sidecar_samples = sidecarSamples(draw.surfaces.color);
    const bool sidecar = sidecar_samples != 0;
    const bool depth_used = draw.raster.depth_test || draw.raster.stencil_test ||
                            rsxZetaWriteEnabled(draw.raster, draw.surfaces.depth_format);
    std::shared_ptr<RenderTarget> depth;
    if (depth_used || sidecar_samples == VK_SAMPLE_COUNT_1_BIT) depth = acquireDepth(draw.surfaces, false);
    if (!valid()) return;
    if (!color) return skip("state", "draw without color target");
    // One viewport and scissor serve both attachments, so they must have been made at the same scale.
    if (depth && depth->scale != color->scale) {
        if (depth_used) return skip("state", "attachments at different resolution scales");
        depth.reset();
    }
    if (draw.surfaces.clip_w == 0 || draw.surfaces.clip_h == 0) return skip("state", "draw with empty surface clip");

    VkPrimitiveTopology topology{}, topology_class{};
    if (!topologyFor(draw.primitive, topology, topology_class)) {
        return skip("primitive", rsxPrimitiveName(static_cast<std::uint32_t>(draw.primitive)));
    }

    // A device without usable fans (see VkContext::triangleFans) draws the fan as a triangle list.
    const bool expand_fan = draw.primitive == RsxPrimitive::TriangleFan && !context_.triangleFans();
    if (expand_fan) topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Object motion: a perspective depth-writing draw of the one-sample scene pass keeps its inputs for the next frame
    // and writes a second attachment. The first candidate of a frame starts the frame's history and images; a later
    // one of another scene size is drawn as a plain draw.
    bool object_motion = false;
    if (sidecar_samples == VK_SAMPLE_COUNT_1_BIT && options_.dlaa_evaluate &&
        options_.dlaa_debug.motion != DlaaMotion::Matrices) {
        if (!context_.objectMotionSupported()) {
            logOnce(LogLevel::Info, "render-object-motion-unavailable",
                    "tox2: render: object motion vectors unavailable ({})", context_.objectMotionGap());
        } else if (depth && objectMotionCandidate(draw)) {
            const std::uint32_t scene_width = color->passWidth(true), scene_height = color->passHeight(true);
            if (object_motion_frame_.frame != frame_index_) {
                object_motion_frame_ = {frame_index_, 0, 0};
                if (!fg_motion_) fg_motion_ = std::make_unique<FgMotionPass>(context_);
                std::shared_ptr<void> retired;
                bool recreated = false;
                std::string error;
                if (fg_motion_->prepare(scene_width, scene_height, retired, recreated, error)) {
                    // Frames in flight may still read the previous images.
                    if (retired) slots_[current_slot_]->keep_alive.push_back(std::move(retired));
                    if (!history_) history_ = std::make_unique<ObjectMotionHistory>(context_);
                    history_->beginFrame(frame_index_, scene_width, scene_height, kDefaultMotionMatchRule);
                    object_motion_frame_.width = scene_width;
                    object_motion_frame_.height = scene_height;
                } else {
                    logOnce(LogLevel::Warn, "render-object-motion-failed",
                            "tox2: render: object motion vectors not recorded ({})", error);
                }
            }
            object_motion = object_motion_frame_.width == scene_width && object_motion_frame_.height == scene_height;
        }
    }

    DrawShaders shaders;
    // A program that has no variant is drawn as it is without one.
    if (!(object_motion && pipelines_->shaders(draw, shaders, true))) {
        object_motion = false;
        if (!pipelines_->shaders(draw, shaders)) {
            if (!pipelines_->valid()) fail(pipelines_->error());
            return skip({}, {});  // the reasons were counted by the pipeline store
        }
    }

    for (std::uint32_t unit = 0; unit < kFpTextureUnitCount; unit++) {
        if ((shaders.texture_units & (1u << unit)) == 0) continue;
        for (const RsxTextureDesc& texture : draw.textures) {
            if (texture.unit == unit && rsxTextureKind(texture) == RsxTextureKind::Unsupported) {
                return skip("texture layout", std::format("sampled dimension {}", texture.dimension));
            }
        }
    }

    // The render area is the smaller of the attachments; the scissor is clipped to it, and an empty
    // rectangle draws nothing (counted in `draws`, not skipped).
    const RsxRasterState& raster = draw.raster;
    std::uint32_t area_width = color->passWidth(sidecar), area_height = color->passHeight(sidecar);
    if (depth) {
        area_width = std::min(area_width, depth->passWidth(sidecar));
        area_height = std::min(area_height, depth->passHeight(sidecar));
    }
    // A single-sample anti-aliased surface is drawn at its sample layout: one image pixel per sample, so the clip
    // rectangle, viewport and scissor grow by the samples per pixel as well. A sidecar pass draws at the clip size.
    const PixelScale pixel_scale = pixelScaleOf(draw.surfaces.color, color->scale, sidecar);
    // The coverage controls only act on a multisampled layout and are not modelled; the game writes none of them.
    if (draw.surfaces.color.samples_x > 1 || draw.surfaces.color.samples_y > 1) {
        if (raster.alpha_to_coverage) rsxNoteUnimplemented("state", "alpha to coverage");
        if (raster.alpha_to_one) rsxNoteUnimplemented("state", "alpha to one");
        if (raster.sample_mask != 0xFFFF)
            rsxNoteUnimplemented("state", std::format("sample mask {:#x}", raster.sample_mask));
    }
    const ImageRect scissor_rect = scaleRect(raster.scissor_x, raster.scissor_y, raster.scissor_w, raster.scissor_h,
                                             pixel_scale, area_width, area_height);
    if (scissor_rect.width == 0 || scissor_rect.height == 0) {
        stats_.draws++;
        return;
    }
    if (draw.count == 0) return skip({}, {});

    // The vertex range comes from the frame builder, which leaves the restart index out of it. A restart
    // index is honoured through Vulkan's fixed restart value, for strips and fans only; the builder counts
    // a restart it cannot express.
    const bool is_u16 = draw.index_type == kRsxIndexTypeU16;
    std::uint32_t vertex_count = draw.count;
    std::uint32_t min_index = 0;
    bool restart = false;
    UploadRef indices;
    if (draw.indexed) {
        min_index = draw.min_index;
        const std::uint64_t span = static_cast<std::uint64_t>(draw.max_index) - draw.min_index + 1;
        if (draw.max_index < draw.min_index || span > 0x7FFFFFFFull || min_index > 0x7FFFFFFFu) {
            return skip("state", "index range out of bounds");
        }
        restart = draw.restart_used;
        if (restart && !rsxPrimitiveRestartSupported(draw.primitive, draw.index_type, draw.restart_index)) {
            return skip({}, {});
        }
        // The range check and the byte swap share one pass over the indices. A draw that fails the check leaves
        // its upload unused, which only costs space in this frame's buffer.
        const std::size_t index_bytes = static_cast<std::size_t>(draw.count) * (is_u16 ? 2 : 4);
        if (!allocUpload(index_bytes, indices)) return;
        if (!convertIndicesChecked(is_u16, draw.index_data, draw.count, draw.min_index, draw.max_index, restart,
                                   draw.restart_index, std::span<std::uint8_t>(indices.data, index_bytes))) {
            return skip("state", "index outside the vertex range");
        }
        vertex_count = static_cast<std::uint32_t>(span);
    }

    if (!ensurePlaceholder() || !valid()) return;

    // Vertex inputs: one binding per slot the vertex program reads, bound to the converted array or to
    // the default value register of the slot.
    PipelineKey key;
    key.vp_hash = draw.vp_hash;
    key.fp_hash = draw.fp_hash;
    key.fp_control = draw.fp_control;
    key.texcoord_control = draw.texcoord_control;
    key.fp_texture_types = shaders.texture_types;
    key.color_format = color->format;
    key.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
    key.topology_class = topology_class;
    key.samples = sidecar ? static_cast<VkSampleCountFlagBits>(sidecar_samples) : VK_SAMPLE_COUNT_1_BIT;
    key.object_motion = object_motion;
    VkBuffer vertex_buffers[kPipelineVertexSlots] = {};
    VkDeviceSize vertex_offsets[kPipelineVertexSlots] = {};
    // The converted arrays of this draw, which the history copies for the next frame (size 0: a default slot).
    std::array<ObjectMotionHistory::Range, kPipelineVertexSlots> slot_uploads{};

    UploadRef defaults;
    bool have_defaults = false;
    for (std::uint32_t slot = 0; slot < kPipelineVertexSlots; slot++) {
        if ((shaders.inputs_used & (1u << slot)) == 0) continue;

        const RsxVertexAttrib* attrib = nullptr;
        for (const RsxVertexAttrib& candidate : draw.attribs) {
            if (candidate.slot == slot && candidate.has_data) attrib = &candidate;
        }
        std::optional<VertexAttribFormat> format;
        if (attrib != nullptr) {
            format = vertexAttribFormat(attrib->type, attrib->size);
            // A scaled-integer format the device does not offer for vertex buffers is converted to floats.
            if (format && !vertexFormatSupported(format->format)) format = widenedFormat(*format);
            if (format && !vertexFormatSupported(format->format)) format.reset();
            if (!format) {
                // A type or size without a conversion: the draw cannot be drawn faithfully, so it is not
                // drawn with a default in its place.
                return skip("vertex type", std::format("{} size {}", rsxVertexTypeName(attrib->type), attrib->size));
            }
        }

        if (format) {
            const std::uint32_t stride = attrib->stride;
            const std::size_t source_bytes =
                    attrib->data_offset <= draw.vertex_data.size() ? draw.vertex_data.size() - attrib->data_offset : 0;
            const std::uint32_t needed = stride == 0 ? 1 : vertex_count;
            if (vertexElementsAvailable(*format, stride, source_bytes) < needed) {
                return skip("state", "vertex data short");
            }
            UploadRef ref;
            if (!allocUpload(static_cast<std::size_t>(needed) * format->host_size, ref)) return;
            convertVertexElements(
                    *format, std::span<const std::uint8_t>(draw.vertex_data.data() + attrib->data_offset, source_bytes),
                    stride, needed,
                    std::span<std::uint8_t>(ref.data, static_cast<std::size_t>(needed) * format->host_size));
            key.vertex_formats[slot] = format->format;
            key.vertex_strides[slot] = static_cast<std::uint8_t>(stride == 0 ? 0 : format->host_size);
            vertex_buffers[slot] = ref.buffer;
            vertex_offsets[slot] = ref.offset;
            slot_uploads[slot] = {ref.buffer, ref.offset, static_cast<VkDeviceSize>(needed) * format->host_size};
            continue;
        }

        // No array: the slot reads its default attribute register as written, or (0, 0, 0, 1) when the
        // register was never written.
        if (!have_defaults) {
            if (!allocUpload(kPipelineVertexSlots * 16, defaults)) return;
            std::array<float, kPipelineVertexSlots * 4> values = draw.default_attribs;
            for (std::uint32_t s = 0; s < kPipelineVertexSlots; s++) {
                if ((draw.default_written & (1u << s)) != 0) continue;
                values[s * 4] = values[s * 4 + 1] = values[s * 4 + 2] = 0.0f;
                values[s * 4 + 3] = 1.0f;
            }
            std::memcpy(defaults.data, values.data(), sizeof(values));
            have_defaults = true;
        }
        key.vertex_formats[slot] = VK_FORMAT_R32G32B32A32_SFLOAT;
        key.vertex_strides[slot] = 0;
        vertex_buffers[slot] = defaults.buffer;
        vertex_offsets[slot] = defaults.offset + slot * 16;
    }

    // An expanded fan is drawn from u32 indices, so an array draw becomes an indexed one; a restart index
    // splits the fan, and the list topology takes no restart.
    std::uint32_t draw_count = draw.count;
    bool index_u16 = is_u16;
    if (expand_fan) {
        std::vector<std::uint32_t> list;
        if (!draw.indexed) {
            list = triangleFanToList(0, draw.count);
        } else {
            std::vector<std::uint32_t> fan(draw.count);
            for (std::uint32_t i = 0; i < draw.count; i++) {
                const std::uint8_t* p = draw.index_data.data() + static_cast<std::size_t>(i) * (is_u16 ? 2 : 4);
                fan[i] = is_u16 ? (static_cast<std::uint32_t>(p[0]) << 8 | p[1])
                                : (static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
                                   static_cast<std::uint32_t>(p[2]) << 8 | p[3]);
            }
            std::size_t begin = 0;
            for (std::size_t end = 0; end <= fan.size(); end++) {
                if (end != fan.size() && !(restart && fan[end] == draw.restart_index)) continue;
                const std::vector<std::uint32_t> part =
                        triangleFanIndicesToList(std::span<const std::uint32_t>(fan.data() + begin, end - begin));
                list.insert(list.end(), part.begin(), part.end());
                begin = end + 1;
            }
        }
        if (list.empty()) {
            stats_.draws++;
            return;
        }
        draw_count = static_cast<std::uint32_t>(list.size());
        index_u16 = false;
        const std::size_t index_bytes = list.size() * sizeof(std::uint32_t);
        if (!allocUpload(index_bytes, indices)) return;
        std::memcpy(indices.data, list.data(), index_bytes);
    }

    // A 32-bit float target has no alpha test and no 8-bit rounding (RPCS3 `rsx_methods.h`
    // `alpha_test_enabled`, `RSXThread.cpp` RSX_SHADER_CONTROL_8BIT_FRAMEBUFFER).
    const bool float_target = color->format == VK_FORMAT_R32_SFLOAT;
    const std::uint32_t alpha_func = raster.alpha_test && !float_target ? (raster.alpha_func & 7u) : 7u;
    // Func 7 (always) is the same as no test; "never" (0) is a test. The pipeline without the test has no
    // alpha discard, which keeps hidden surface removal on Apple GPUs.
    key.alpha_test = alpha_func != 7u;

    // Fixed-function state baked into the pipeline.
    key.color_mask = colorWriteMask(raster.color_mask);
    if (raster.blend && !blendSupported(color->format)) {
        rsxNoteUnimplemented("blend", "blending into a format the device cannot blend");
    } else if (raster.blend) {
        key.blend = true;
        key.src_color = blendFactor(raster.blend_sfactor_rgb);
        key.dst_color = blendFactor(raster.blend_dfactor_rgb);
        key.src_alpha = blendFactor(raster.blend_sfactor_a);
        key.dst_alpha = blendFactor(raster.blend_dfactor_a);
        key.color_op = blendOp(raster.blend_eq_rgb);
        key.alpha_op = blendOp(raster.blend_eq_a);
    }
    const VkPipeline pipeline = pipelines_->pipeline(key, shaders);
    if (pipeline == VK_NULL_HANDLE) {
        fail(pipelines_->error());
        return;
    }

    // The writes are what leave the other image of a multisampled target behind. A bound depth surface that is not
    // used (only the one-sample scene pass keeps one) takes no layout pass.
    const bool depth_written = depth && rsxZetaWriteEnabled(raster, draw.surfaces.depth_format);
    const bool color_written = key.color_mask != 0;
    if (sidecar) {
        const auto samples = static_cast<VkSampleCountFlagBits>(sidecar_samples);
        if (!ensureSidecar(*color, samples) || (depth && !ensureSidecar(*depth, samples))) return;
        if (samples == VK_SAMPLE_COUNT_1_BIT && color_written && depth) color->dlaa_depth = depth;
    } else {
        if (!beginLayoutPass(*color, color_written, false) ||
            (depth && depth_used && !beginLayoutPass(*depth, depth_written, false))) {
            return;
        }
    }

    // Textures are prepared before the rendering instance begins: uploads, copies and layout changes
    // cannot be recorded inside it.
    VkDescriptorImageInfo image_infos[kFpTextureUnitCount];
    float texture_scale[kFpTextureUnitCount][4];
    if (!prepareTextures(draw, shaders, *color, sidecar, image_infos, texture_scale)) return;
    // After the textures: a draw that samples its own target copies the sidecar into the layout image first, which
    // must not count as this draw's write.
    if (sidecar) {
        beginSidecarPass(*color, color_written, false);
        if (depth && depth_used) beginSidecarPass(*depth, depth_written, false);
    }

    // Uniform data: vertex constants, fragment constants, the per-draw block shared by both stages.
    constexpr std::size_t kVpConstantBytes = kRsxVpConstantCount * 16;
    UploadRef vp_constants, fp_constants, draw_block;
    const std::size_t fp_entries = std::max<std::size_t>(draw.fp_constants.size() / 4, 1);
    constexpr std::size_t kDrawBlockBytes = kFpDrawBlockOffset + sizeof(FpDrawUniforms);
    // Draws that saw the same constants (equal non-zero id) bind the upload of the first one in this frame.
    const bool reuse_vp_constants = draw.vp_constants_id != 0 && vp_constant_upload_.id == draw.vp_constants_id;
    if (reuse_vp_constants) {
        vp_constants = vp_constant_upload_.ref;
    } else if (!allocUpload(kVpConstantBytes, vp_constants)) {
        return;
    }
    if (!allocUpload(fp_entries * 16, fp_constants) || !allocUpload(kDrawBlockBytes, draw_block)) return;
    if (reuse_vp_constants) {
        stats_.vp_constant_uploads_reused++;
    } else {
        const std::size_t vp_constant_bytes =
                draw.vp_constants ? std::min(draw.vp_constants->size() * 4, kVpConstantBytes) : 0;
        if (vp_constant_bytes != 0) std::memcpy(vp_constants.data, draw.vp_constants->data(), vp_constant_bytes);
        if (vp_constant_bytes < kVpConstantBytes) {
            std::memset(vp_constants.data + vp_constant_bytes, 0, kVpConstantBytes - vp_constant_bytes);
        }
        if (draw.vp_constants_id != 0) vp_constant_upload_ = {draw.vp_constants_id, vp_constants};
    }
    std::memset(fp_constants.data, 0, fp_entries * 16);
    for (std::size_t i = 0; i < draw.fp_constants.size(); i++) {
        const std::uint32_t word = fpFixupWord(draw.fp_constants[i]);
        std::memcpy(fp_constants.data + i * 4, &word, 4);
    }

    VpDrawUniforms vp{};
    std::memcpy(vp.viewport_scale, raster.viewport_scale, sizeof(vp.viewport_scale));
    std::memcpy(vp.viewport_offset, raster.viewport_offset, sizeof(vp.viewport_offset));
    vp.clip_width = static_cast<float>(draw.surfaces.clip_w);
    vp.clip_height = static_cast<float>(draw.surfaces.clip_h);
    vp.clip_min = raster.clip_min;
    vp.clip_max = raster.clip_max;
    vp.output_mask = draw.vp_output_mask;
    vp.branch_bits = draw.vp_branch_bits;
    vp.point_size = raster.point_size > 0.0f ? raster.point_size : 1.0f;
    // Points follow the vertical pixel scale (square points on a non-square sample layout); the vertex shader
    // multiplies gl_PointSize by this lane.
    vp.viewport_scale[3] = static_cast<float>(pixel_scale.y);
    if (draw.primitive == RsxPrimitive::Points && !context_.hasLargePoints() &&
        vp.point_size * vp.viewport_scale[3] > 1.0f) {
        rsxNoteUnimplemented("state", "point size unavailable");
    }
    vp.user_clip = raster.user_clip;
    // The program only writes clip distances on a device with shaderClipDistance. The renderer does not know
    // which slots the program writes, so this can count a draw whose planes have no source.
    if (!context_.hasClipDistance()) {
        for (std::uint32_t plane = 0; plane < 6; plane++) {
            const std::uint32_t op = (raster.user_clip >> (4 * plane)) & 0xFu;
            if ((op == 1 || op == 2) && (draw.vp_output_mask & (1u << (6 + plane))) != 0) {
                rsxNoteUnimplemented("state", "user clip unavailable");
                break;
            }
        }
    }

    FpDrawUniforms fp{};
    fp.alpha_func = alpha_func;
    fp.alpha_ref = static_cast<float>(raster.alpha_ref) / 255.0f;
    fp.flags = (float_target ? 0u : kFpFlagRound8Bit) | (raster.two_sided_lighting ? kFpFlagTwoSided : 0u);
    std::memcpy(fp.texture_scale, texture_scale, sizeof(texture_scale));
    std::memcpy(draw_block.data, &vp, sizeof(vp));
    std::memcpy(draw_block.data + kFpDrawBlockOffset, &fp, sizeof(fp));

    // Previous inputs of an object motion candidate. Matching waits until no skip is left, so a matched draw is one
    // that is drawn. Camera mode matches and records too but binds the current inputs.
    std::optional<ObjectMotionHistory::Previous> previous;
    UploadRef previous_draw_block;
    if (object_motion) {
        const MotionDrawSignature signature = motionDrawSignature(draw);
        const ObjectMotionHistory::Previous* matched = history_->match(signature);
        history_->record(signature, {vp_constants.buffer, vp_constants.offset, kVpConstantBytes}, vp, slot_uploads);
        if (matched != nullptr && options_.dlaa_debug.motion == DlaaMotion::Object) {
            previous = *matched;
            if (!allocUpload(sizeof(VpDrawUniforms), previous_draw_block)) return;
            std::memcpy(previous_draw_block.data, &previous->draw, sizeof(VpDrawUniforms));
        }
    }

    if (depth_written) depth->write_serial++;

    OpenDrawPass pass;
    pass.color_view = sidecar ? color->msaa->view : color->view;
    pass.depth_view = depth ? (sidecar ? depth->msaa->view : depth->view) : VK_NULL_HANDLE;
    pass.object_view = object_motion ? fg_motion_->objectView() : VK_NULL_HANDLE;
    pass.stencil = depth && formatHasStencil(depth->format);
    pass.width = area_width;
    pass.height = area_height;
    // Anything above that recorded outside an instance (texture work, a layout sync, a new target) ended the open one,
    // so a match means nothing touched the attachments since the last draw: their layouts are unchanged and the
    // draws are ordered by rasterization order, with no barrier or new instance between them.
    const bool continues = open_pass_ == pass;
    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    if (!continues) {
        outsideCmd();  // ends the previous instance, whose attachments differ
        if (sidecar) {
            transitionSidecar(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (depth) transitionSidecar(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        } else {
            transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (depth) transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }

        VkRenderingAttachmentInfo color_attachments[2]{};
        VkRenderingAttachmentInfo& color_attachment = color_attachments[0];
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = pass.color_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (object_motion) {
            // Cleared once per frame and kept by every later candidate instance.
            fg_motion_->beginObjectWrites(cmd, frame_index_);
            VkRenderingAttachmentInfo& object_attachment = color_attachments[1];
            object_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            object_attachment.imageView = pass.object_view;
            object_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            object_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            object_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depth_attachment{};
        if (depth) {
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = pass.depth_view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {area_width, area_height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = object_motion ? 2 : 1;
        rendering.pColorAttachments = color_attachments;
        rendering.pDepthAttachment = depth ? &depth_attachment : nullptr;
        rendering.pStencilAttachment = pass.stencil ? &depth_attachment : nullptr;
        gpuBeginInstance(cmd, GpuInstanceKind::Draw, area_width, area_height, sidecar ? sidecar_samples : 1,
                         color->format, depth != nullptr);
        vkCmdBeginRendering(cmd, &rendering);
        stats_.rendering_instances++;
        open_pass_ = pass;
    }
    if (gpu_open_) {
        // A clear left its instance open for this draw. The instance is timed as one and counted as a draw instance,
        // marked with the clear it began with, so `draw` stays the cost of the pass and `clear` the clears no draw
        // followed.
        GpuInstanceLabel& label = slots_[current_slot_]->timed.back().label;
        if (label.kind == GpuInstanceKind::Clear) {
            label.kind = GpuInstanceKind::Draw;
            label.began_with_clear = true;
        }
        label.draws++;
    }

    // Dynamic state is command buffer state and outlives rendering instances, so a value is recorded only when the
    // buffer does not already hold it. Compared bitwise, which also covers floats.
    const bool state_unknown = !draw_state_;
    DrawStateShadow& shadow = state_unknown ? draw_state_.emplace() : *draw_state_;
    const auto update = [&](void* cached, const void* value, std::size_t size) {
        if (!state_unknown && std::memcmp(cached, value, size) == 0) {
            stats_.state_calls_skipped++;
            return false;
        }
        std::memcpy(cached, value, size);
        stats_.state_calls++;
        return true;
    };
    const auto set = [&](auto& cached, const auto& value) {
        static_assert(sizeof(cached) == sizeof(value));
        return update(&cached, &value, sizeof(value));
    };

    if (set(shadow.pipeline, pipeline)) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const VkViewport viewport = {0.0f,
                                 0.0f,
                                 static_cast<float>(draw.surfaces.clip_w * pixel_scale.x),
                                 static_cast<float>(draw.surfaces.clip_h * pixel_scale.y),
                                 0.0f,
                                 1.0f};
    const VkRect2D scissor = {{static_cast<std::int32_t>(scissor_rect.x), static_cast<std::int32_t>(scissor_rect.y)},
                              {scissor_rect.width, scissor_rect.height}};
    if (set(shadow.viewport, viewport)) vkCmdSetViewport(cmd, 0, 1, &viewport);
    if (set(shadow.scissor, scissor)) vkCmdSetScissor(cmd, 0, 1, &scissor);
    constexpr std::uint32_t kCullFront = 0x404, kCullBack = 0x405, kFrontFaceCw = 0x900;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (raster.cull) {
        cull = raster.cull_face == kCullFront  ? VK_CULL_MODE_FRONT_BIT
               : raster.cull_face == kCullBack ? VK_CULL_MODE_BACK_BIT
                                               : VK_CULL_MODE_FRONT_AND_BACK;
    }
    if (set(shadow.cull, cull)) vkCmdSetCullMode(cmd, cull);
    const VkFrontFace front_face =
            raster.front_face == kFrontFaceCw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (set(shadow.front_face, front_face)) vkCmdSetFrontFace(cmd, front_face);
    if (set(shadow.topology, topology)) vkCmdSetPrimitiveTopology(cmd, topology);
    const VkBool32 restart_enable = restart && !expand_fan ? VK_TRUE : VK_FALSE;
    if (set(shadow.restart, restart_enable)) vkCmdSetPrimitiveRestartEnable(cmd, restart_enable);
    const bool depth_test = depth && raster.depth_test;
    const VkBool32 depth_test_enable = depth_test ? VK_TRUE : VK_FALSE;
    const VkBool32 depth_write_enable = depth_test && raster.depth_write ? VK_TRUE : VK_FALSE;
    const VkCompareOp depth_compare = static_cast<VkCompareOp>(raster.depth_func & 7u);
    if (set(shadow.depth_test, depth_test_enable)) vkCmdSetDepthTestEnable(cmd, depth_test_enable);
    if (set(shadow.depth_write, depth_write_enable)) vkCmdSetDepthWriteEnable(cmd, depth_write_enable);
    if (set(shadow.depth_compare, depth_compare)) vkCmdSetDepthCompareOp(cmd, depth_compare);

    // The stencil state is set on every draw, with the test off too, so no draw inherits the previous one's.
    // Vulkan has no stencil without the aspect (Z16 or no depth target): the draw runs untested and is counted.
    const bool stencil_test = raster.stencil_test && depth && formatHasStencil(depth->format);
    if (raster.stencil_test && !stencil_test) rsxNoteUnimplemented("state", "stencil test without a stencil surface");
    const VkBool32 stencil_test_enable = stencil_test ? VK_TRUE : VK_FALSE;
    if (set(shadow.stencil_test, stencil_test_enable)) vkCmdSetStencilTestEnable(cmd, stencil_test_enable);
    const std::pair<VkStencilFaceFlagBits, const RsxStencilFace*> faces[] = {
            {VK_STENCIL_FACE_FRONT_BIT, &raster.stencil_front}, {VK_STENCIL_FACE_BACK_BIT, &raster.stencil_back}};
    for (std::size_t i = 0; i < 2; i++) {
        const auto& [face_bit, face] = faces[i];
        DrawStateShadow::Face& cached = shadow.faces[i];
        DrawStateShadow::Face wanted;
        wanted.fail = stencilOp(face->op_fail);
        wanted.pass = stencilOp(face->op_zpass);
        wanted.depth_fail = stencilOp(face->op_zfail);
        wanted.compare = static_cast<VkCompareOp>(face->func & 7u);
        wanted.compare_mask = face->func_mask;
        wanted.write_mask = face->write_mask;
        wanted.reference = face->ref;
        // The four stencil op fields are one command, so they change together.
        if (update(&cached.fail, &wanted.fail, offsetof(DrawStateShadow::Face, compare_mask))) {
            vkCmdSetStencilOp(cmd, face_bit, wanted.fail, wanted.pass, wanted.depth_fail, wanted.compare);
        }
        if (set(cached.compare_mask, wanted.compare_mask))
            vkCmdSetStencilCompareMask(cmd, face_bit, wanted.compare_mask);
        if (set(cached.write_mask, wanted.write_mask)) vkCmdSetStencilWriteMask(cmd, face_bit, wanted.write_mask);
        if (set(cached.reference, wanted.reference)) vkCmdSetStencilReference(cmd, face_bit, wanted.reference);
    }
    float constants[4];
    blendConstants(raster.blend_color, constants);
    if (set(shadow.blend_constants, constants)) vkCmdSetBlendConstants(cmd, constants);

    // Always a value, never left unset: a stale or missing push constant would shift a draw by a sub-pixel amount.
    // Only perspective draws into the one-sample scene image move: orthographic ones are screen-space and 2D, and the
    // shift is exactly zero everywhere else, so other modes render bit for bit as before. Without an evaluation the
    // shift would only blur the picture, so it needs one too. All shifted draws of a frame share the frame's offset,
    // which keeps passes that depth-test against each other exact.
    float jitter_ndc[2] = {0.0f, 0.0f};
    if (sidecar_samples == VK_SAMPLE_COUNT_1_BIT && options_.dlaa_evaluate && draw.vp_constants &&
        dlaaPerspective(*draw.vp_constants)) {
        // The same draws describe the camera, whether or not the jitter is on.
        DlaaCameraKey camera_key;
        for (std::size_t i = 0; i < camera_key.mvp.size(); i++) {
            camera_key.mvp[i] = std::bit_cast<float>((*draw.vp_constants)[i]);
        }
        std::copy_n(raster.viewport_scale, 3, camera_key.viewport_scale);
        std::copy_n(raster.viewport_offset, 3, camera_key.viewport_offset);
        camera_key.clip_w = static_cast<float>(draw.surfaces.clip_w);
        camera_key.clip_h = static_cast<float>(draw.surfaces.clip_h);
        camera_key.clip_min = raster.clip_min;
        camera_key.clip_max = raster.clip_max;
        dlaa_camera_.record(camera_key);
        if (options_.dlaa_debug.jitter) {
            const std::array<float, 2> jitter = dlaaJitterPixels(stats_.flips);
            jitter_ndc[0] = 2.0f * jitter[0] / static_cast<float>(draw.surfaces.clip_w * pixel_scale.x);
            jitter_ndc[1] = 2.0f * jitter[1] / static_cast<float>(draw.surfaces.clip_h * pixel_scale.y);
            dlaa_jitter_frame_ = stats_.flips;
        }
    }
    // motion_scale turns the variant's NDC difference into pixels: half the viewport the draw covers. Zero tells the
    // variant that the draw has no previous inputs. While the path is active every draw pushes all 16 bytes, so the
    // block a variant draw reads is always fully set; the plain programs declare the first 8.
    const float push[4] = {jitter_ndc[0], jitter_ndc[1], previous ? 0.5f * viewport.width : 0.0f,
                           previous ? 0.5f * viewport.height : 0.0f};
    if (set(shadow.push, push)) {
        vkCmdPushConstants(cmd, pipelines_->drawLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           objectMotionActive() ? sizeof(push) : 2 * sizeof(float), push);
    }

    for (std::uint32_t slot = 0; slot < kPipelineVertexSlots; slot++) {
        if (vertex_buffers[slot] == VK_NULL_HANDLE) continue;
        vkCmdBindVertexBuffers(cmd, slot, 1, &vertex_buffers[slot], &vertex_offsets[slot]);
        if (!object_motion) continue;
        // The variant reads the previous frame's array of the slot, or the current default block of a default slot.
        const ObjectMotionHistory::Range* history = previous ? &previous->slots[slot] : nullptr;
        const VkBuffer buffer = history && history->size != 0 ? history->buffer : vertex_buffers[slot];
        const VkDeviceSize offset = history && history->size != 0 ? history->offset : vertex_offsets[slot];
        vkCmdBindVertexBuffers(cmd, kVpPrevInputLocationBase + slot, 1, &buffer, &offset);
    }

    VkDescriptorBufferInfo buffer_infos[5] = {{vp_constants.buffer, vp_constants.offset, kVpConstantBytes},
                                              {fp_constants.buffer, fp_constants.offset, fp_entries * 16},
                                              {draw_block.buffer, draw_block.offset, kDrawBlockBytes}};
    if (object_motion) {
        // Bindings 19 and 20: the previous frame's constants (in the history buffer) and draw values, else the current
        // ones.
        buffer_infos[3] = previous ? VkDescriptorBufferInfo{previous->constants.buffer, previous->constants.offset,
                                                            kVpConstantBytes}
                                   : buffer_infos[0];
        buffer_infos[4] =
                previous ? VkDescriptorBufferInfo{previous_draw_block.buffer, previous_draw_block.offset,
                                                  sizeof(VpDrawUniforms)}
                         : VkDescriptorBufferInfo{draw_block.buffer, draw_block.offset, sizeof(VpDrawUniforms)};
    }
    VkWriteDescriptorSet writes[5 + kFpTextureUnitCount]{};
    std::uint32_t write_count = 0;
    for (std::uint32_t i = 0; i < (object_motion ? 5u : 3u); i++) {
        VkWriteDescriptorSet& write = writes[write_count++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = i < 3 ? i : kVpPrevConstantsBinding + (i - 3);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer_infos[i];
    }
    for (std::uint32_t unit = 0; unit < kFpTextureUnitCount; unit++) {
        if ((shaders.texture_units & (1u << unit)) == 0) continue;
        VkWriteDescriptorSet& write = writes[write_count++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = kFpTextureBindingBase + unit;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_infos[unit];
    }
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->drawLayout(), 0, write_count, writes);

    // Element 0 of every array is vertex min_index (rsx_frame.h), so indexed draws offset the vertex
    // index by -min_index; array draws start at element 0.
    if (draw.indexed || expand_fan) {
        vkCmdBindIndexBuffer(cmd, indices.buffer, indices.offset,
                             index_u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, draw_count, 1, 0, -static_cast<std::int32_t>(min_index), 0);
    } else {
        vkCmdDraw(cmd, draw.count, 1, 0, 0);
    }
    stats_.draws++;
}

// Facts from RPCS3 `VK/VKGSRender.cpp` `VKGSRender::clear_surface`:
//  - a clear is the clipped scissor rectangle: over the whole render area the attachment load operations clear (CLEAR
//    for each aspect the clear touches, LOAD for the others), otherwise one vkCmdClearAttachments;
//  - color clear values are each 8-bit channel / 255, with alpha in the top byte of the register
//    (`rsx_decode.h` `registers_decoder<NV4097_SET_COLOR_CLEAR_VALUE>`);
//  - the depth value is the register field divided by the format's maximum, 0xFFFF for 16-bit depth
//    and 0xFFFFFF otherwise (`Common/TextureUtils.cpp` `get_max_depth_value`); Z24S8 keeps depth in
//    bits 8..31 and stencil in bits 0..7, Z16 uses bits 0..15 (`rsx_decode.h`
//    `registers_decoder<NV4097_SET_ZSTENCIL_CLEAR_VALUE>`);
//  - the aspects cleared follow the mask bits;
//  - a color mask other than all four channels needs a draw (RPCS3 attachment_clear_pass), done by
//    recordMaskedColorClear; so does a stencil write mask other than 0xFF, which is not modelled (the
//    frame builder counts a partial stencil mask; the clear carries no stencil write mask, so the
//    stencil is cleared in full).
void RsxRenderer::recordClear(const RsxClear& clear) {
    stats_.clears++;
    constexpr std::uint32_t kColorBits = kRsxClearRed | kRsxClearGreen | kRsxClearBlue | kRsxClearAlpha;
    const std::uint32_t color_bits = clear.mask & kColorBits;

    // Whether this clear replaces everything a target held, so a resized target needs no copy of the old one.
    // The rectangle is the scissor clamped to the smaller of the cleared targets, as below; the frame builder
    // has already dropped the stencil bit for a stencil write mask of 0 (RPCS3 does that before its
    // full-aspect check, VK/VKGSRender.cpp clear_surface).
    const bool clears_color = color_bits != 0 && clear.surfaces.has_color;
    const bool clears_depth = (clear.mask & (kRsxClearDepth | kRsxClearStencil)) != 0;
    const bool color_in_area = clears_color && color_bits == kColorBits;
    const RsxSurface& color_surface = clear.surfaces.color;
    const RsxSurface& depth_surface = clear.surfaces.depth;
    const auto covers = [&](const RsxSurface& target, const RsxSurface& other, bool other_in_area) {
        if (clear.scissor[0] != 0 || clear.scissor[1] != 0) return false;
        if (other_in_area && (other.width < target.width || other.height < target.height)) return false;
        return clear.scissor[2] >= target.width && clear.scissor[3] >= target.height;
    };
    const bool color_overwritten = color_in_area && covers(color_surface, depth_surface, clears_depth);
    const bool depth_full_aspect =
            (clear.mask & kRsxClearDepth) != 0 &&
            ((clear.mask & kRsxClearStencil) != 0 || clear.surfaces.depth_format != RsxDepthFormat::Z24S8);
    const bool depth_overwritten = depth_full_aspect && covers(depth_surface, color_surface, color_in_area);

    std::shared_ptr<RenderTarget> color;
    if (clears_color) color = acquireColor(clear.surfaces.color, color_overwritten);
    if (!valid()) return;
    std::shared_ptr<RenderTarget> depth;
    if (clears_depth) depth = acquireDepth(clear.surfaces, depth_overwritten);
    if (!valid()) return;
    // One clear rectangle serves both attachments, so they must have been made at the same scale. Checked
    // before the masked clear so the whole clear is dropped, not half of it.
    if (color && depth && color->scale != depth->scale) {
        rsxNoteUnimplemented("state", "attachments at different resolution scales");
        return;
    }
    // The surface fields of the AA mode are shared by the colour and the depth surface.
    const VkSampleCountFlags sidecar_samples = sidecarSamples(color ? clear.surfaces.color : clear.surfaces.depth);
    const bool sidecar = sidecar_samples != 0;
    if (color && color_bits != kColorBits) {
        recordMaskedColorClear(clear, *color, color_bits, sidecar_samples);
        // After the clear: creating the sidecar inside it drops any earlier depth link.
        if (sidecar_samples == VK_SAMPLE_COUNT_1_BIT && depth) color->dlaa_depth = depth;
        color.reset();
    }
    if (!valid()) return;
    if (!color && !depth) return;
    if (depth) depth->write_serial++;

    if (sidecar) {
        const auto samples = static_cast<VkSampleCountFlagBits>(sidecar_samples);
        if ((color && !ensureSidecar(*color, samples)) || (depth && !ensureSidecar(*depth, samples))) return;
        if (samples == VK_SAMPLE_COUNT_1_BIT && color && depth) color->dlaa_depth = depth;
        if (color) beginSidecarPass(*color, true, color_overwritten);
        if (depth) beginSidecarPass(*depth, true, depth_overwritten);
    } else {
        if ((color && !beginLayoutPass(*color, true, color_overwritten)) ||
            (depth && !beginLayoutPass(*depth, true, depth_overwritten))) {
            return;
        }
    }

    const VkCommandBuffer cmd = outsideCmd();
    std::uint32_t area_width = color ? color->passWidth(sidecar) : depth->passWidth(sidecar);
    std::uint32_t area_height = color ? color->passHeight(sidecar) : depth->passHeight(sidecar);
    if (color && depth) {
        area_width = std::min(color->passWidth(sidecar), depth->passWidth(sidecar));
        area_height = std::min(color->passHeight(sidecar), depth->passHeight(sidecar));
    }
    // The colour and depth targets share a scale here (checked above); a depth-only clear uses the depth one. The
    // clear rectangle is in clip pixels and grows by the samples per pixel of the surface layout.
    const PixelScale pixel_scale = color ? pixelScaleOf(clear.surfaces.color, color->scale, sidecar)
                                         : pixelScaleOf(clear.surfaces.depth, depth->scale, sidecar);

    // The builder clipped the rectangle to the clip size, which is also the target size; the clamp only
    // guards the attachment bounds.
    const ImageRect clear_rect = scaleRect(clear.scissor[0], clear.scissor[1], clear.scissor[2], clear.scissor[3],
                                           pixel_scale, area_width, area_height);
    // A rectangle over the whole render area lets every aspect the clear touches be cleared by the attachment's load
    // operation, which a tile-based GPU does without reading the old contents or loading them into the tile. An aspect
    // the clear leaves alone (the stencil of a depth-only clear) keeps LOAD. A smaller rectangle loads everything and
    // clears it with vkCmdClearAttachments.
    const bool full_area = clear_rect.x == 0 && clear_rect.y == 0 && clear_rect.width == area_width &&
                           clear_rect.height == area_height;

    VkRenderingAttachmentInfo color_attachment{};
    VkRenderingAttachmentInfo depth_attachment{};
    VkRenderingAttachmentInfo stencil_attachment{};
    VkClearAttachment attachments[2]{};
    std::uint32_t attachment_count = 0;
    if (color) {
        if (sidecar) {
            transitionSidecar(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        } else {
            transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = sidecar ? color->msaa->view : color->view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        const std::uint32_t argb = clear.color_argb;
        VkClearValue value{};
        value.color.float32[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        value.color.float32[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        value.color.float32[2] = static_cast<float>(argb & 0xFF) / 255.0f;
        value.color.float32[3] = static_cast<float>(argb >> 24) / 255.0f;
        if (full_area) {
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.clearValue = value;
        } else {
            VkClearAttachment& a = attachments[attachment_count++];
            a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            a.colorAttachment = 0;
            a.clearValue = value;
        }
    }
    if (depth) {
        if (sidecar) {
            transitionSidecar(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        } else {
            transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = sidecar ? depth->msaa->view : depth->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        stencil_attachment = depth_attachment;

        VkImageAspectFlags aspect = 0;
        VkClearDepthStencilValue value{};
        if ((clear.mask & kRsxClearDepth) != 0) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
            const bool z24 = clear.surfaces.depth_format == RsxDepthFormat::Z24S8;
            const std::uint32_t raw = z24 ? (clear.depth_stencil >> 8) : (clear.depth_stencil & 0xFFFF);
            value.depth = static_cast<float>(raw) / (z24 ? 16777215.0f : 65535.0f);
        }
        if ((clear.mask & kRsxClearStencil) != 0 && formatHasStencil(depth->format)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
            value.stencil = clear.depth_stencil & 0xFF;
        }
        if (full_area) {
            if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depth_attachment.clearValue.depthStencil = value;
            }
            if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
                stencil_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                stencil_attachment.clearValue.depthStencil = value;
            }
        } else if (aspect != 0) {
            VkClearAttachment& a = attachments[attachment_count++];
            a.aspectMask = aspect;
            a.clearValue.depthStencil = value;
        }
    }

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {area_width, area_height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = color ? 1 : 0;
    rendering.pColorAttachments = color ? &color_attachment : nullptr;
    rendering.pDepthAttachment = depth ? &depth_attachment : nullptr;
    rendering.pStencilAttachment = depth && formatHasStencil(depth->format) ? &stencil_attachment : nullptr;

    VkClearRect rect{};
    rect.rect.offset = {static_cast<std::int32_t>(clear_rect.x), static_cast<std::int32_t>(clear_rect.y)};
    rect.rect.extent = {clear_rect.width, clear_rect.height};
    rect.layerCount = 1;

    gpuBeginInstance(cmd, GpuInstanceKind::Clear, area_width, area_height, sidecar ? sidecar_samples : 1,
                     color ? color->format : VK_FORMAT_UNDEFINED, depth != nullptr);
    vkCmdBeginRendering(cmd, &rendering);
    stats_.rendering_instances++;
    if (attachment_count != 0 && rect.rect.extent.width != 0 && rect.rect.extent.height != 0) {
        vkCmdClearAttachments(cmd, attachment_count, attachments, 1, &rect);
    }

    // A clear with a colour attachment binds what a draw to the same target binds, so the draws that follow record
    // into this instance (recordDraw) and the target is not stored and loaded in between. Anything else recorded
    // outside an instance ends it first (outsideCmd). A depth-only clear is not left open: a draw always has a colour
    // attachment, so none could continue it.
    if (color) {
        OpenDrawPass pass;
        pass.color_view = color_attachment.imageView;
        pass.depth_view = depth ? depth_attachment.imageView : VK_NULL_HANDLE;
        pass.stencil = depth && formatHasStencil(depth->format);
        pass.width = area_width;
        pass.height = area_height;
        open_pass_ = pass;
        return;
    }
    vkCmdEndRendering(cmd);
    gpuEndInstance(cmd);
}

// A clear that touches only some color channels: a full-target triangle clipped to the clear rectangle,
// writing the clear colour through the channel mask (RPCS3 `VK/VKGSRender.cpp` `VKGSRender::clear_surface`
// falls back to `attachment_clear_pass` for a partial mask).
void RsxRenderer::recordMaskedColorClear(const RsxClear& clear, RenderTarget& color, std::uint32_t color_bits,
                                         VkSampleCountFlags sidecar_samples) {
    const bool sidecar = sidecar_samples != 0;
    VkColorComponentFlags mask = 0;
    if ((color_bits & kRsxClearRed) != 0) mask |= VK_COLOR_COMPONENT_R_BIT;
    if ((color_bits & kRsxClearGreen) != 0) mask |= VK_COLOR_COMPONENT_G_BIT;
    if ((color_bits & kRsxClearBlue) != 0) mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((color_bits & kRsxClearAlpha) != 0) mask |= VK_COLOR_COMPONENT_A_BIT;
    const VkPipeline pipeline = pipelines_->maskedClearPipeline(
            color.format, mask, sidecar ? static_cast<VkSampleCountFlagBits>(sidecar_samples) : VK_SAMPLE_COUNT_1_BIT);
    if (pipeline == VK_NULL_HANDLE) {
        fail(pipelines_->error());
        return;
    }

    const std::uint32_t width = color.passWidth(sidecar), height = color.passHeight(sidecar);
    const ImageRect clear_rect = scaleRect(clear.scissor[0], clear.scissor[1], clear.scissor[2], clear.scissor[3],
                                           pixelScaleOf(clear.surfaces.color, color.scale, sidecar), width, height);
    if (clear_rect.width == 0 || clear_rect.height == 0) return;

    if (sidecar) {
        if (!ensureSidecar(color, static_cast<VkSampleCountFlagBits>(sidecar_samples))) return;
        beginSidecarPass(color, true, false);
    } else if (!beginLayoutPass(color, true, false)) {
        return;
    }

    const VkCommandBuffer cmd = outsideCmd();
    invalidateDrawState();  // binds the masked clear pipeline
    if (sidecar) {
        transitionSidecar(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    } else {
        transition(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = sidecar ? color.msaa->view : color.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width, height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    const std::uint32_t argb = clear.color_argb;
    const float value[4] = {static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((argb >> 8) & 0xFF) / 255.0f, static_cast<float>(argb & 0xFF) / 255.0f,
                            static_cast<float>(argb >> 24) / 255.0f};
    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor = {{static_cast<std::int32_t>(clear_rect.x), static_cast<std::int32_t>(clear_rect.y)},
                              {clear_rect.width, clear_rect.height}};

    gpuBeginInstance(cmd, GpuInstanceKind::MaskedClear, width, height, sidecar ? sidecar_samples : 1, color.format,
                     false);
    vkCmdBeginRendering(cmd, &rendering);
    stats_.rendering_instances++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, pipelines_->clearLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(value), value);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    gpuEndInstance(cmd);
}

bool RsxRenderer::ensureReadback(FrameSlot& slot, VkDeviceSize size) {
    if (slot.readback != VK_NULL_HANDLE && slot.readback_size >= size) return true;
    if (slot.readback != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_.allocator(), slot.readback, slot.readback_allocation);
        slot.readback = VK_NULL_HANDLE;
        slot.readback_size = 0;
    }
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &slot.readback, &slot.readback_allocation,
                        &mapped) != VK_SUCCESS ||
        mapped.pMappedData == nullptr) {
        slot.readback = VK_NULL_HANDLE;
        fail("vmaCreateBuffer failed for the display readback");
        return false;
    }
    slot.readback_mapped = mapped.pMappedData;
    slot.readback_size = size;
    return true;
}

// The display buffer is the color target at (location 0, offset). When the flip is read back (every flip
// with readback_all, otherwise only a dumped or requested one) its pixels are copied into the slot's
// readback buffer here, inside the frame's command buffer, so readDisplay needs no further GPU work. A
// buffer that was never a render target reads back as black.
void RsxRenderer::recordFlip(const RsxFlip& flip) {
    if (frame_gen_) frame_gen_->flipEntered(flip.completed_us);
    stats_.flips++;
    dlaa_camera_.endFrame();
    textures_->trim();
    image_pool_->trim(stats_.flips);
    stats_.image_pool_bytes = image_pool_->bytes();
    stats_.image_pool_discards = image_pool_->discards();
    syncTextureStats();
    if (stats_.flips % kImageLogFlips == 0) logImageCreations();
    FrameSlot& slot = *slots_[current_slot_];
    dump_this_flip_ = dump_requested_;
    dump_requested_ = false;
    noteFrameGenTarget();
    // Asked before the readback decision so a request made for this very flip is honoured.
    std::uint32_t requested_scale = 0;
    std::optional<AaMode> requested_aa_mode;
    std::optional<FrameGenTarget> requested_frame_gen;
    std::uint32_t requested_anisotropy = 0;
    if (presenter_ != nullptr && overlay_ != nullptr) {
        const PresentOverlayRequests requests = overlay_->onFlip(PresentFrameInfo{
                .stats = &stats_,
                .dumps = dumper_->counts(),
                .last_requested_dump = last_requested_dump_,
                .resolution_scale = resolution_scale_,
                .aa_mode = aa_mode_,
                .msaa_available = msaaAvailable(),
                .dlaa_available = dlaaAvailable(),
                .anisotropy = anisotropy_,
                .max_anisotropy = maxAnisotropy(),
                .window = presenter_->windowStatus(),
                .frame_gen_supported = frameGenSupported(),
                .frame_gen_target = frame_gen_target_,
                .frame_gen = frame_gen_ ? frame_gen_->status()
                                        : FrameGenStatus{.inactive_reason = {},
                                                         .max_multiplier = context_.frameGenerationMaxMultiplier()},
        });
        if (requests.dump_display) dump_this_flip_ = true;
        requested_scale = requests.resolution_scale;
        requested_aa_mode = requests.aa_mode;
        requested_anisotropy = requests.anisotropy;
        requested_frame_gen = requests.frame_gen;
        // The window is not part of the frame being recorded, so the request needs no deferral.
        if (requests.window_size) presenter_->requestWindowSize(*requests.window_size);
    }
    const bool read_back = options_.readback_all || dumpDue() || dump_this_flip_;
    display_copied_ = false;
    const auto it = color_targets_.find(TargetKey{0, flip.offset});
    // Only an A8R8G8B8 target can be shown; any other format reads back as black.
    display_black_ = it == color_targets_.end() || it->second->format != VK_FORMAT_B8G8R8A8_UNORM;
    if (display_black_) {
        rsxNoteUnimplemented("state", "flip of non-render-target");
        display_width_ = flip.width * resolution_scale_;
        display_height_ = flip.height * resolution_scale_;
        display_copied_ = read_back;
        last_display_.reset();
    } else {
        const std::shared_ptr<RenderTarget> target = it->second;
        touch(target);
        if (!syncLayout(*target)) return;
        last_display_ = target;
        display_width_ = target->image_width;
        display_height_ = target->image_height;
        if (read_back && ensureReadback(slot, static_cast<VkDeviceSize>(display_width_) * display_height_ * 4)) {
            display_copied_ = true;
            transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {display_width_, display_height_, 1};
            vkCmdCopyImageToBuffer(outsideCmd(), target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.readback, 1,
                                   &region);
            VkMemoryBarrier2 host_barrier{};
            host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &host_barrier;
            vkCmdPipelineBarrier2(outsideCmd(), &dependency);
        }
    }
    if (!valid()) return;
    if (history_) {
        // Copies the frame's candidate inputs for the next frame; the history buffer a larger frame replaces stays
        // alive until this slot's fence. A fresh pointer each flip: a reused one would chain every retired buffer.
        std::shared_ptr<void> retired;
        std::string history_error;
        if (!history_->recordCopy(outsideCmd(), retired, history_error)) {
            logOnce(LogLevel::Warn, "render-object-motion-history-failed",
                    "tox2: render: object motion history not kept ({})", history_error);
        }
        if (retired) slot.keep_alive.push_back(std::move(retired));
        const ObjectMotionHistory::Stats history_stats = history_->stats();
        stats_.object_motion_candidates += history_stats.candidates;
        stats_.object_motion_matched += history_stats.matched;
        stats_.object_motion_history_bytes = history_stats.history_bytes;
    }
    if (presenter_ != nullptr) {
        recordPresentBlit(display_black_ ? nullptr : it->second.get());
        if (frame_gen_ && present_pending_) recordFrameGenPresent();
    }
    if (submitFrame()) {
        has_flip_ = true;
        last_flip_ = Clock::now();
        collectFgMvCheck();
    }
    // Applied only now: this flip, its readback and the present blit were all recorded at the old scale, and
    // targets rescale lazily when the next frame binds them. The mode follows the same rule so a frame is never
    // recorded in both. The anisotropy level follows the same rule; only samplers created afterwards use it.
    if (requested_scale != 0 && valid()) setResolutionScale(requested_scale);
    if (requested_aa_mode && valid()) setAaMode(*requested_aa_mode);
    if (requested_anisotropy != 0 && valid()) setAnisotropy(requested_anisotropy);
    if (requested_frame_gen && valid()) setFrameGenTarget(*requested_frame_gen);
    if (frame_gen_ && valid()) frame_gen_->tick();
}

// A frame that holds only the present blit and the overlays. A draw or clear since the last flip means the game is
// mid-frame: that recording also holds draws the next flip must submit with it, and last_display_ may already be
// overwritten, so only the game's own flip presents then.
void RsxRenderer::presentIdle() {
    if (presenter_ == nullptr || overlay_ == nullptr || !valid() || !has_flip_) return;
    if (!overlay_->wantsIdlePresent()) return;
    const Clock::time_point now = Clock::now();
    if (!idlePresentDue(last_flip_, last_idle_present_, now)) return;
    // Known limit: an idle frame would reuse the frame index of the next real frame, and Streamline's frame token and
    // latency markers expect each index once. With frame generation loaded an overlay is therefore drawn only on
    // real flips, which the game keeps issuing while a system dialog is open.
    if (frame_gen_ && frame_gen_->loaded()) {
        logOnce(LogLevel::Warn, "render-idle-present-frame-gen",
                "tox2: render: idle present skipped, frame generation is loaded; an overlay shows only when the game "
                "flips");
        return;
    }
    if (recording_) {
        logOnce(LogLevel::Warn, "render-idle-present-frame-open",
                "tox2: render: idle present skipped, a frame is open; an overlay shows only when the game flips");
        return;
    }
    if (!beginFrame()) return;
    if (last_display_) touch(last_display_);
    recordPresentBlit(last_display_.get());
    // readDisplay and the dumps read the slot of the last flip, which this frame must not take over.
    const std::size_t flip_slot = display_slot_;
    submitFrame();
    display_slot_ = flip_slot;
    last_idle_present_ = now;
}

void RsxRenderer::recordFrameGenPresent() {
    FrameGenPresent frame;
    frame.dlaa = aa_mode_ == AaMode::Dlaa;
    if (fg_tapped_.frame == frame_index_) {
        frame.inputs = fg_tapped_.inputs;
        frame.scene_width = fg_tapped_.width;
        frame.scene_height = fg_tapped_.height;
        frame.depth_format = fg_tapped_.depth_format;
    }
    frame.display_width = display_width_;
    frame.display_height = display_height_;
    frame.overlay_open = overlay_ != nullptr && overlay_->wantsDraw();
    frame.swapchain_width = present_target_.width;
    frame.swapchain_height = present_target_.height;
    frame.swapchain_format = present_target_.format;
    frame.scene_rect = letterboxRect(present_target_.width, present_target_.height, 16, 9);
    frame_gen_->beforePresent(outsideCmd(), frame);
}

// Acquires a swapchain image and records the copy of the display buffer into it (a black frame when
// `source` is null). Without an image (minimised window, timeout) nothing is recorded and the frame is
// submitted as usual. FIFO presentation can make the acquire block; the presenter measures it.
void RsxRenderer::recordPresentBlit(RenderTarget* source) {
    if (!presenter_->acquire(current_slot_, present_target_)) return;
    present_pending_ = true;
    const Presenter::Target& dst = present_target_;
    const VkCommandBuffer cmd = outsideCmd();

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dst.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    const auto barrierTo = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                               VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    constexpr VkPipelineStageFlags2 kTransfer = kPresentWriteStages;  // also the acquire semaphore wait in submitFrame
    // Measured: handed over in PRESENT_SRC_KHR, the images of a swapchain created through sl.dlss_g (its own buffers)
    // make the validation layer report, at every present, that Streamline's pacer command buffer expects them in
    // TRANSFER_SRC_OPTIMAL. Left in that layout there are no reports, also while DLSS-G generates frames.
    const VkImageLayout final_layout =
            frame_gen_ && frame_gen_->loaded() ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrierTo(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTransfer, 0, kTransfer,
              VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const PixelRect rect = letterboxRect(dst.width, dst.height, 16, 9);
    const bool covers_image = rect.width == dst.width && rect.height == dst.height;
    if (source == nullptr || !covers_image) {
        const VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        if (source != nullptr) {
            barrierTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTransfer,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
    }
    if (source != nullptr && rect.width != 0) {
        transition(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<std::int32_t>(source->image_width),
                                static_cast<std::int32_t>(source->image_height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {rect.x, rect.y, 0};
        region.dstOffsets[1] = {rect.x + static_cast<std::int32_t>(rect.width),
                                rect.y + static_cast<std::int32_t>(rect.height), 1};
        vkCmdBlitImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    }
    if (overlay_ != nullptr && dst.view != VK_NULL_HANDLE && overlay_->wantsDraw()) {
        // The overlay draws over the blit with LOAD, so the transfer writes must be visible to the
        // attachment read and write.
        barrierTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kTransfer,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        overlay_->record(cmd, dst);
        invalidateDrawState();  // the overlay binds its own pipeline and sets state on this command buffer
        barrierTo(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final_layout,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_NONE, 0);
        return;
    }
    barrierTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_NONE, 0);
}

void RsxRenderer::execute(std::span<RsxCommand> commands) {
    for (RsxCommand& command : commands) {
        if (!beginFrame()) break;
        if (const auto* clear = std::get_if<RsxClear>(&command)) {
            recordClear(*clear);
        } else if (const auto* draw = std::get_if<RsxDraw>(&command)) {
            recordDraw(*draw);
        } else {
            recordFlip(std::get<RsxFlip>(command));
            if (valid() && (dumpDue() || dump_this_flip_)) {
                const bool requested = dump_this_flip_;
                // A flip that --dump-frames writes anyway is not written a second time.
                const std::filesystem::path& dir = dumpDue() ? options_.dump_dir : options_.requested_dump_dir;
                const std::filesystem::path path = dir / std::format("frame_{:06}.png", stats_.flips);
                if (dumpDisplay(path) && requested) last_requested_dump_ = path;
            }
        }
        if (!valid()) break;
    }
    syncTextureStats();
}

// The flip number counts from 1; stats_.flips already includes the flip being recorded.
bool RsxRenderer::dumpDue() const {
    return !options_.dump_dir.empty() && stats_.flips % std::max<std::uint32_t>(options_.dump_every, 1) == 0;
}

// Only the readback happens on the calling (RSX) thread; the dump worker converts, encodes and writes. A
// full queue drops the frame instead of making the flip wait. True when the frame was queued.
bool RsxRenderer::dumpDisplay(const std::filesystem::path& path) {
    if (!dumper_->hasRoom()) {
        dumper_->countSkipped();
        logOnce(LogLevel::Warn, "render-dump-skipped", "tox2: render: dump skipped (queue full) at flip {}",
                stats_.flips);
        return false;
    }
    DumpFrame frame;
    frame.bgra = dumper_->takeBuffer();
    if (!readDisplay(frame.bgra, frame.width, frame.height)) {
        dumper_->recycleBuffer(std::move(frame.bgra));
        return false;
    }
    frame.path = path;
    return dumper_->submit(std::move(frame));
}

DumpCounts RsxRenderer::dumpCounts() const { return dumper_->counts(); }

std::uint64_t RsxRenderer::flushDumps(std::chrono::milliseconds timeout) { return dumper_->flush(timeout); }

bool RsxRenderer::savePipelineCache() { return pipelines_ && pipelines_->savePipelineCache(); }

PipelineCacheStats RsxRenderer::pipelineCacheStats() const {
    return pipelines_ ? pipelines_->pipelineCacheStats() : PipelineCacheStats{};
}

bool RsxRenderer::readObjectMotion(std::vector<std::uint32_t>& words, std::uint32_t& width, std::uint32_t& height) {
    if (!valid() || !fg_motion_ || object_motion_frame_.width == 0) return false;
    const VkImage image = fg_motion_->objectImage();
    const VkImageLayout layout = fg_motion_->objectLayout();
    if (image == VK_NULL_HANDLE || layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    width = object_motion_frame_.width;
    height = object_motion_frame_.height;
    const VkDevice device = context_.device();
    vkDeviceWaitIdle(device);

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 8;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo allocated{};
    if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &buffer, &allocation, &allocated) !=
        VK_SUCCESS) {
        return false;
    }
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = context_.graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    bool ok = vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (ok) {
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok = vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
    }
    if (ok) {
        // To the transfer layout and back to the one FgMotionPass tracks for the image.
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = layout;
        vkCmdPipelineBarrier2(cmd, &dependency);
        ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferSubmitInfo cmd_submit{};
        cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_submit;
        ok = vkQueueSubmit2(context_.graphicsQueue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) vkDeviceWaitIdle(device);
    }
    if (ok) {
        vmaInvalidateAllocation(context_.allocator(), allocation, 0, VK_WHOLE_SIZE);
        words.resize(static_cast<std::size_t>(width) * height * 2);
        std::memcpy(words.data(), allocated.pMappedData, words.size() * sizeof(std::uint32_t));
    }
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    vmaDestroyBuffer(context_.allocator(), buffer, allocation);
    return ok;
}

bool RsxRenderer::readDisplay(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
    if (!has_flip_ || !valid() || !display_copied_ || display_width_ == 0 || display_height_ == 0) return false;
    width = display_width_;
    height = display_height_;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    if (display_black_) {
        bgra.assign(bytes, 0);
        for (std::size_t i = 3; i < bytes; i += 4) bgra[i] = 0xFF;
        return true;
    }
    FrameSlot& slot = *slots_[display_slot_];
    if (!waitSlot(slot)) return false;
    vmaInvalidateAllocation(context_.allocator(), slot.readback_allocation, 0, VK_WHOLE_SIZE);
    bgra.resize(bytes);
    std::memcpy(bgra.data(), slot.readback_mapped, bytes);
    return true;
}

}  // namespace tox2

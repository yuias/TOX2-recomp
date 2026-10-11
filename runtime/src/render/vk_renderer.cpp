#include "render/vk_renderer.h"

#include <algorithm>
#include <bit>
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
#include "render/fp_translator.h"
#include "render/frame_dump.h"
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
// The only operations that write a swapchain image: the black clear and the blit.
constexpr VkPipelineStageFlags2 kPresentWriteStages = VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
constexpr std::uint32_t kIdentityRemap = 0xAAE4;  // RSX_TEXTURE_REMAP_IDENTITY (RPCS3 `gcm_enums.h`)

bool formatHasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
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
    std::uint32_t width = 0;
    std::uint32_t height = 0;
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

    RenderTarget() = default;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    ~RenderTarget() {
        for (const auto& entry : sampled_views) vkDestroyImageView(context->device(), entry.second, nullptr);
        if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), depth_view, nullptr);
        if (stencil_view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), stencil_view, nullptr);
        if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
        if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
    }
};

// A region of a slot's upload block, already mapped for writing.
struct RsxRenderer::UploadRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    std::uint8_t* data = nullptr;
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
    // Host-visible copy of the display buffer written at the flip that submitted this slot.
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_allocation = nullptr;
    void* readback_mapped = nullptr;
    VkDeviceSize readback_size = 0;
};

RsxRenderer::RsxRenderer(VkContext& context, RenderOptions options) : context_(context), options_(std::move(options)) {
    if (!options_.shader_cache.empty()) setShaderCacheDir(options_.shader_cache);
    if (!options_.dump_dir.empty()) dumper_ = std::make_unique<FrameDumper>();
    pipelines_ = std::make_unique<DrawPipelines>(context_);
    if (!pipelines_->valid()) {
        fail(pipelines_->error());
        return;
    }
    textures_ = std::make_unique<TextureCache>(context_);
    const VkDevice device = context_.device();
    for (std::unique_ptr<FrameSlot>& slot_ptr : slots_) {
        slot_ptr = std::make_unique<FrameSlot>();
        FrameSlot& slot = *slot_ptr;

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
    const VkDevice device = context_.device();
    vkDeviceWaitIdle(device);
    for (std::unique_ptr<FrameSlot>& slot : slots_) {
        if (!slot) continue;
        slot->in_use.clear();
        slot->texture_refs.clear();
        for (FrameSlot::UploadBlock& block : slot->uploads) {
            vmaDestroyBuffer(context_.allocator(), block.buffer, block.allocation);
        }
        if (slot->readback != VK_NULL_HANDLE)
            vmaDestroyBuffer(context_.allocator(), slot->readback, slot->readback_allocation);
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
    color_targets_.clear();
    depth_targets_.clear();
}

void RsxRenderer::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

bool RsxRenderer::waitSlot(FrameSlot& slot) {
    if (!slot.pending) return true;
    const VkResult result = vkWaitForFences(context_.device(), 1, &slot.fence, VK_TRUE, kFenceTimeoutNs);
    if (result != VK_SUCCESS) {
        fail(std::format("waiting for the GPU failed ({})", static_cast<int>(result)));
        return false;
    }
    slot.pending = false;
    slot.in_use.clear();
    slot.texture_refs.clear();
    return true;
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
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS) {
        fail("vkBeginCommandBuffer failed");
        return false;
    }
    recording_ = true;
    return true;
}

bool RsxRenderer::submitFrame() {
    FrameSlot& slot = *slots_[current_slot_];
    for (const FrameSlot::UploadBlock& block : slot.uploads) {
        if (block.used != 0) vmaFlushAllocation(context_.allocator(), block.allocation, 0, block.used);
    }
    if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
        fail("vkEndCommandBuffer failed");
        return false;
    }
    VkCommandBufferSubmitInfo cmd_submit{};
    cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_submit.commandBuffer = slot.cmd;
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
    recording_ = false;
    display_slot_ = current_slot_;
    current_slot_ = (current_slot_ + 1) % kFramesInFlight;
    if (present_pending_) {
        present_pending_ = false;
        presenter_->present(present_target_);
    }
    return true;
}

void RsxRenderer::touch(const std::shared_ptr<RenderTarget>& target) {
    std::vector<std::shared_ptr<RenderTarget>>& in_use = slots_[current_slot_]->in_use;
    if (std::find(in_use.begin(), in_use.end(), target) == in_use.end()) in_use.push_back(target);
}

// Conservative full barrier, also recorded before every draw and clear that begins a rendering instance
// (even when the layout does not change, so successive passes over one target are ordered). Draws are
// few enough per frame that tighter stages are not needed yet.
void RsxRenderer::transition(RenderTarget& target, VkImageLayout layout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = target.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange = {target.aspect, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(slots_[current_slot_]->cmd, &dependency);
    target.layout = layout;
}

// A target the cache has not seen holds no guest data (nothing is loaded from guest memory), so it
// starts defined instead of undefined: color is opaque black (0, 0, 0, 1), depth is 1.0 with stencil 0xFF.
// These are RPCS3's values for a surface with nothing inherited and "read color/depth buffers" off
// (VK/VKRenderTargets.cpp, rpcs3/Emu/system_config.h), and what a partial clear leaves in the aspect it does
// not touch (VK/VKGSRender.cpp VKGSRender::clear_surface).
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::createTarget(bool depth, const RsxSurface& surface,
                                                                     VkFormat format) {
    const VkPhysicalDeviceProperties& props = context_.deviceProperties();
    if (surface.width == 0 || surface.height == 0 || surface.width > props.limits.maxImageDimension2D ||
        surface.height > props.limits.maxImageDimension2D) {
        rsxNoteUnimplemented("state", std::format("surface size {}x{}", surface.width, surface.height));
        return nullptr;
    }

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
    target->width = surface.width;
    target->height = surface.height;
    target->pitch = surface.pitch;
    target->depth = depth;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {surface.width, surface.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(context_.allocator(), &image_info, &alloc_info, &target->image, &target->allocation, nullptr) !=
        VK_SUCCESS) {
        target->image = VK_NULL_HANDLE;
        fail("vmaCreateImage failed for a render target");
        return nullptr;
    }

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
    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
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
        const std::shared_ptr<RenderTarget>& existing = it->second;
        if (existing->width == surface.width && existing->height == surface.height && existing->format == format) {
            touch(existing);
            return existing;
        }
        previous = existing;
        map.erase(it);
    }
    std::shared_ptr<RenderTarget> target = createTarget(depth, surface, format);
    if (!target) return nullptr;
    map[key] = target;
    touch(target);
    if (previous && !overwrites_all) inheritContents(previous, *target);
    return target;
}

// Copies the overlap (origin (0, 0), unscaled) of the previous target at the same address into a target that
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

    touch(previous);  // it may not have been used in this frame slot yet, and the copy reads it until the fence
    transition(*previous, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);  // orders the copy after the initial clear
    VkImageCopy region{};
    region.srcSubresource = {previous->aspect, 0, 0, 1};
    region.dstSubresource = {target.aspect, 0, 0, 1};
    region.extent = {std::min(previous->width, target.width), std::min(previous->height, target.height), 1};
    vkCmdCopyImage(slots_[current_slot_]->cmd, previous->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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

    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
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
std::shared_ptr<TextureImage> RsxRenderer::depthAsArgb(RenderTarget& depth) {
    if (!formatHasStencil(depth.format) || !depth.sampled) {
        rsxNoteUnimplemented("texture format", "A8R8G8B8 read of a surface without Z24S8");
        return nullptr;
    }
    if (depth.argb && depth.argb_serial == depth.write_serial) return depth.argb;

    if (!depth.argb) {
        depth.argb = textures_->renderable(depth.width, depth.height);
        if (!textures_->valid()) {
            fail(textures_->error());
            return nullptr;
        }
        if (!depth.argb) return nullptr;
    }
    const auto aspectView = [&](VkImageAspectFlags aspect, VkImageView& view) {
        if (view != VK_NULL_HANDLE) return true;
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
    const VkPipeline pipeline = pipelines_->depthConvertPipeline();
    if (pipeline == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE) {
        if (!pipelines_->valid()) fail(pipelines_->error());
        return nullptr;
    }

    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    transition(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
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
    rendering.renderArea = {{0, 0}, {depth.width, depth.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(depth.width), static_cast<float>(depth.height),
                                 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {depth.width, depth.height}};
    const VkDescriptorImageInfo infos[2] = {
            {placeholder_sampler_, depth.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {placeholder_sampler_, depth.stencil_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->depthConvertLayout(), 0, 2, writes);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
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
                                  VkDescriptorImageInfo (&infos)[kFpTextureUnitCount],
                                  float (&scales)[kFpTextureUnitCount][4]) {
    FrameSlot& slot = *slots_[current_slot_];
    const VkCommandBuffer cmd = slot.cmd;
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
            if (target->width != texture->width || target->height != texture->height) {
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
                if (target->width != texture->width || target->height != texture->height) {
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

            if (target.get() == &color) {
                // The copy is a B8G8R8A8 image; there is none for a float target.
                if (wanted != VK_FORMAT_B8G8R8A8_UNORM) {
                    rsxNoteUnimplemented("texture", "placeholder: feedback copy unavailable");
                    continue;
                }
                if (!feedback_copy) {
                    feedback_copy = textures_->scratch(target->width, target->height);
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
                    textures_->recordCopy(cmd, *feedback_copy, target->image);
                    stats_.feedback_copies++;
                }
                view = textures_->view(*feedback_copy, texture->remap);
            } else {
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
            RsxTextureLayout layout;
            if (!rsxTextureLayout(texture->format, true, texture->width, texture->height, texture->mips, 0,
                                  kind == RsxTextureKind::Cube ? 6 : 1, layout,
                                  kind == RsxTextureKind::Tex3D ? texture->depth : 1) ||
                blob.size() < layout.packed_bytes) {
                rsxNoteUnimplemented("state", "texture data short");
                continue;
            }
            const bool compressed = rsxTextureBlockBytes(texture->format) != 0;
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
                if (compressed) {
                    // Blocks are stored the way Vulkan expects them.
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
                textures_->recordUpload(cmd, *image, staging.buffer, staging.offset, layout.levels);
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
// texture, uploads) happens before vkCmdBeginRendering. A draw that cannot be rendered is counted in
// draws_skipped and, where a reason exists, with rsxNoteUnimplemented.
//
// Facts used, with their sources:
//  - texture units a draw does not sample get no descriptor; the fragment translator reports the
//    sampled ones (GlslResult::texture_units);
//  - depth test and write are only enabled together with a depth target, and a write without the test
//    stays off (RPCS3 `VK/VKGSRender.cpp` `decode_rsx_state`);
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
    const std::shared_ptr<RenderTarget> depth = acquireDepth(draw.surfaces, false);
    if (!valid()) return;
    if (!color) return skip("state", "draw without color target");
    if (draw.surfaces.clip_w == 0 || draw.surfaces.clip_h == 0) return skip("state", "draw with empty surface clip");

    VkPrimitiveTopology topology{}, topology_class{};
    if (!topologyFor(draw.primitive, topology, topology_class)) {
        return skip("primitive", rsxPrimitiveName(static_cast<std::uint32_t>(draw.primitive)));
    }

    DrawShaders shaders;
    if (!pipelines_->shaders(draw, shaders)) {
        if (!pipelines_->valid()) fail(pipelines_->error());
        return skip({}, {});  // the reasons were counted by the pipeline store
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
    std::uint32_t area_width = color->width, area_height = color->height;
    if (depth) {
        area_width = std::min(area_width, depth->width);
        area_height = std::min(area_height, depth->height);
    }
    const std::uint32_t scissor_x = std::min(raster.scissor_x, area_width);
    const std::uint32_t scissor_y = std::min(raster.scissor_y, area_height);
    const std::uint32_t scissor_w = std::min(raster.scissor_w, area_width - scissor_x);
    const std::uint32_t scissor_h = std::min(raster.scissor_h, area_height - scissor_y);
    if (scissor_w == 0 || scissor_h == 0) {
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
        if (!indicesInRange(is_u16, draw.index_data, draw.count, draw.min_index, draw.max_index, restart,
                            draw.restart_index)) {
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
    VkBuffer vertex_buffers[kPipelineVertexSlots] = {};
    VkDeviceSize vertex_offsets[kPipelineVertexSlots] = {};

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

    UploadRef indices;
    if (draw.indexed) {
        const std::size_t index_bytes = static_cast<std::size_t>(draw.count) * (is_u16 ? 2 : 4);
        if (!allocUpload(index_bytes, indices)) return;
        convertIndices(is_u16, draw.index_data, draw.count, std::span<std::uint8_t>(indices.data, index_bytes));
    }

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

    // Textures are prepared before the rendering instance begins: uploads, copies and layout changes
    // cannot be recorded inside it.
    VkDescriptorImageInfo image_infos[kFpTextureUnitCount];
    float texture_scale[kFpTextureUnitCount][4];
    if (!prepareTextures(draw, shaders, *color, image_infos, texture_scale)) return;

    // Uniform data: vertex constants, fragment constants, the per-draw block shared by both stages.
    constexpr std::size_t kVpConstantBytes = kRsxVpConstantCount * 16;
    UploadRef vp_constants, fp_constants, draw_block;
    const std::size_t fp_entries = std::max<std::size_t>(draw.fp_constants.size() / 4, 1);
    constexpr std::size_t kDrawBlockBytes = kFpDrawBlockOffset + sizeof(FpDrawUniforms);
    if (!allocUpload(kVpConstantBytes, vp_constants) || !allocUpload(fp_entries * 16, fp_constants) ||
        !allocUpload(kDrawBlockBytes, draw_block)) {
        return;
    }
    std::memset(vp_constants.data, 0, kVpConstantBytes);
    std::memcpy(vp_constants.data, draw.vp_constants.data(), std::min(draw.vp_constants.size() * 4, kVpConstantBytes));
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

    // A 32-bit float target has no alpha test and no 8-bit rounding (RPCS3 `rsx_methods.h`
    // `alpha_test_enabled`, `RSXThread.cpp` RSX_SHADER_CONTROL_8BIT_FRAMEBUFFER).
    const bool float_target = color->format == VK_FORMAT_R32_SFLOAT;
    FpDrawUniforms fp{};
    fp.alpha_func = raster.alpha_test && !float_target ? (raster.alpha_func & 7u) : 7u;
    fp.alpha_ref = static_cast<float>(raster.alpha_ref) / 255.0f;
    fp.flags = (float_target ? 0u : kFpFlagRound8Bit) | (raster.two_sided_lighting ? kFpFlagTwoSided : 0u);
    std::memcpy(fp.texture_scale, texture_scale, sizeof(texture_scale));
    std::memcpy(draw_block.data, &vp, sizeof(vp));
    std::memcpy(draw_block.data + kFpDrawBlockOffset, &fp, sizeof(fp));

    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) {
        transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (rsxZetaWriteEnabled(raster, draw.surfaces.depth_format)) depth->write_serial++;
    }

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = color->view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth_attachment{};
    if (depth) {
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {area_width, area_height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_attachment;
    rendering.pDepthAttachment = depth ? &depth_attachment : nullptr;
    rendering.pStencilAttachment = depth && formatHasStencil(depth->format) ? &depth_attachment : nullptr;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    const VkViewport viewport = {
            0.0f, 0.0f, static_cast<float>(draw.surfaces.clip_w), static_cast<float>(draw.surfaces.clip_h), 0.0f, 1.0f};
    const VkRect2D scissor = {{static_cast<std::int32_t>(scissor_x), static_cast<std::int32_t>(scissor_y)},
                              {scissor_w, scissor_h}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    constexpr std::uint32_t kCullFront = 0x404, kCullBack = 0x405, kFrontFaceCw = 0x900;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    if (raster.cull) {
        cull = raster.cull_face == kCullFront  ? VK_CULL_MODE_FRONT_BIT
               : raster.cull_face == kCullBack ? VK_CULL_MODE_BACK_BIT
                                               : VK_CULL_MODE_FRONT_AND_BACK;
    }
    vkCmdSetCullMode(cmd, cull);
    vkCmdSetFrontFace(cmd,
                      raster.front_face == kFrontFaceCw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, topology);
    vkCmdSetPrimitiveRestartEnable(cmd, restart ? VK_TRUE : VK_FALSE);
    const bool depth_test = depth && raster.depth_test;
    vkCmdSetDepthTestEnable(cmd, depth_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, depth_test && raster.depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, static_cast<VkCompareOp>(raster.depth_func & 7u));

    // The stencil state is set on every draw, with the test off too, so no draw inherits the previous one's.
    // Vulkan has no stencil without the aspect (Z16 or no depth target): the draw runs untested and is counted.
    const bool stencil_test = raster.stencil_test && depth && formatHasStencil(depth->format);
    if (raster.stencil_test && !stencil_test) rsxNoteUnimplemented("state", "stencil test without a stencil surface");
    vkCmdSetStencilTestEnable(cmd, stencil_test ? VK_TRUE : VK_FALSE);
    const std::pair<VkStencilFaceFlagBits, const RsxStencilFace*> faces[] = {
            {VK_STENCIL_FACE_FRONT_BIT, &raster.stencil_front}, {VK_STENCIL_FACE_BACK_BIT, &raster.stencil_back}};
    for (const auto& [face_bit, face] : faces) {
        vkCmdSetStencilOp(cmd, face_bit, stencilOp(face->op_fail), stencilOp(face->op_zpass), stencilOp(face->op_zfail),
                          static_cast<VkCompareOp>(face->func & 7u));
        vkCmdSetStencilCompareMask(cmd, face_bit, face->func_mask);
        vkCmdSetStencilWriteMask(cmd, face_bit, face->write_mask);
        vkCmdSetStencilReference(cmd, face_bit, face->ref);
    }
    float constants[4];
    blendConstants(raster.blend_color, constants);
    vkCmdSetBlendConstants(cmd, constants);

    for (std::uint32_t slot = 0; slot < kPipelineVertexSlots; slot++) {
        if (vertex_buffers[slot] != VK_NULL_HANDLE)
            vkCmdBindVertexBuffers(cmd, slot, 1, &vertex_buffers[slot], &vertex_offsets[slot]);
    }

    VkDescriptorBufferInfo buffer_infos[3] = {{vp_constants.buffer, vp_constants.offset, kVpConstantBytes},
                                              {fp_constants.buffer, fp_constants.offset, fp_entries * 16},
                                              {draw_block.buffer, draw_block.offset, kDrawBlockBytes}};
    VkWriteDescriptorSet writes[3 + kFpTextureUnitCount]{};
    std::uint32_t write_count = 0;
    for (std::uint32_t i = 0; i < 3; i++) {
        VkWriteDescriptorSet& write = writes[write_count++];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = i;
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
    if (draw.indexed) {
        vkCmdBindIndexBuffer(cmd, indices.buffer, indices.offset, is_u16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, draw.count, 1, 0, -static_cast<std::int32_t>(min_index), 0);
    } else {
        vkCmdDraw(cmd, draw.count, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    stats_.draws++;
}

// Facts from RPCS3 `VK/VKGSRender.cpp` `VKGSRender::clear_surface`:
//  - one vkCmdClearAttachments over the clipped scissor rectangle;
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
    if (clears_color) {
        color = acquireColor(clear.surfaces.color, color_overwritten);
        if (color && color_bits != kColorBits) {
            recordMaskedColorClear(clear, *color, color_bits);
            color.reset();
        }
    }
    if (!valid()) return;
    std::shared_ptr<RenderTarget> depth;
    if (clears_depth) depth = acquireDepth(clear.surfaces, depth_overwritten);
    if (!color && !depth) return;
    if (depth) depth->write_serial++;

    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    std::uint32_t area_width = color ? color->width : depth->width;
    std::uint32_t area_height = color ? color->height : depth->height;
    if (color && depth) {
        area_width = std::min(color->width, depth->width);
        area_height = std::min(color->height, depth->height);
    }

    VkRenderingAttachmentInfo color_attachment{};
    VkRenderingAttachmentInfo depth_attachment{};
    VkRenderingAttachmentInfo stencil_attachment{};
    if (color) {
        transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = color->view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (depth) {
        transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        stencil_attachment = depth_attachment;
    }

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {area_width, area_height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = color ? 1 : 0;
    rendering.pColorAttachments = color ? &color_attachment : nullptr;
    rendering.pDepthAttachment = depth ? &depth_attachment : nullptr;
    rendering.pStencilAttachment = depth && formatHasStencil(depth->format) ? &stencil_attachment : nullptr;

    VkClearAttachment attachments[2]{};
    std::uint32_t attachment_count = 0;
    if (color) {
        const std::uint32_t argb = clear.color_argb;
        VkClearAttachment& a = attachments[attachment_count++];
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.colorAttachment = 0;
        a.clearValue.color.float32[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        a.clearValue.color.float32[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        a.clearValue.color.float32[2] = static_cast<float>(argb & 0xFF) / 255.0f;
        a.clearValue.color.float32[3] = static_cast<float>(argb >> 24) / 255.0f;
    }
    if (depth) {
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
        if (aspect != 0) {
            VkClearAttachment& a = attachments[attachment_count++];
            a.aspectMask = aspect;
            a.clearValue.depthStencil = value;
        }
    }

    // The builder clipped the rectangle to the clip size, which is also the target size; the clamp only
    // guards the attachment bounds.
    const std::uint32_t x = std::min(clear.scissor[0], area_width);
    const std::uint32_t y = std::min(clear.scissor[1], area_height);
    VkClearRect rect{};
    rect.rect.offset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    rect.rect.extent = {std::min(clear.scissor[2], area_width - x), std::min(clear.scissor[3], area_height - y)};
    rect.layerCount = 1;

    vkCmdBeginRendering(cmd, &rendering);
    if (attachment_count != 0 && rect.rect.extent.width != 0 && rect.rect.extent.height != 0) {
        vkCmdClearAttachments(cmd, attachment_count, attachments, 1, &rect);
    }
    vkCmdEndRendering(cmd);
}

// A clear that touches only some color channels: a full-target triangle clipped to the clear rectangle,
// writing the clear colour through the channel mask (RPCS3 `VK/VKGSRender.cpp` `VKGSRender::clear_surface`
// falls back to `attachment_clear_pass` for a partial mask).
void RsxRenderer::recordMaskedColorClear(const RsxClear& clear, RenderTarget& color, std::uint32_t color_bits) {
    VkColorComponentFlags mask = 0;
    if ((color_bits & kRsxClearRed) != 0) mask |= VK_COLOR_COMPONENT_R_BIT;
    if ((color_bits & kRsxClearGreen) != 0) mask |= VK_COLOR_COMPONENT_G_BIT;
    if ((color_bits & kRsxClearBlue) != 0) mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((color_bits & kRsxClearAlpha) != 0) mask |= VK_COLOR_COMPONENT_A_BIT;
    const VkPipeline pipeline = pipelines_->maskedClearPipeline(color.format, mask);
    if (pipeline == VK_NULL_HANDLE) {
        fail(pipelines_->error());
        return;
    }

    const std::uint32_t x = std::min(clear.scissor[0], color.width);
    const std::uint32_t y = std::min(clear.scissor[1], color.height);
    const std::uint32_t w = std::min(clear.scissor[2], color.width - x);
    const std::uint32_t h = std::min(clear.scissor[3], color.height - y);
    if (w == 0 || h == 0) return;

    const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
    transition(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = color.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {color.width, color.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    const std::uint32_t argb = clear.color_argb;
    const float value[4] = {static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((argb >> 8) & 0xFF) / 255.0f, static_cast<float>(argb & 0xFF) / 255.0f,
                            static_cast<float>(argb >> 24) / 255.0f};
    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(color.width), static_cast<float>(color.height),
                                 0.0f, 1.0f};
    const VkRect2D scissor = {{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)}, {w, h}};

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, pipelines_->clearLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(value), value);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
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
// with readback_all, otherwise only a dumped one) its pixels are copied into the slot's readback buffer
// here, inside the frame's command buffer, so readDisplay needs no further GPU work. A buffer that was
// never a render target reads back as black.
void RsxRenderer::recordFlip(const RsxFlip& flip) {
    stats_.flips++;
    textures_->trim();
    FrameSlot& slot = *slots_[current_slot_];
    const bool read_back = options_.readback_all || dumpDue();
    display_copied_ = false;
    const auto it = color_targets_.find(TargetKey{0, flip.offset});
    // Only an A8R8G8B8 target can be shown; any other format reads back as black.
    display_black_ = it == color_targets_.end() || it->second->format != VK_FORMAT_B8G8R8A8_UNORM;
    if (display_black_) {
        rsxNoteUnimplemented("state", "flip of non-render-target");
        display_width_ = flip.width;
        display_height_ = flip.height;
        display_copied_ = read_back;
    } else {
        const std::shared_ptr<RenderTarget> target = it->second;
        touch(target);
        display_width_ = target->width;
        display_height_ = target->height;
        if (read_back && ensureReadback(slot, static_cast<VkDeviceSize>(display_width_) * display_height_ * 4)) {
            display_copied_ = true;
            transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {display_width_, display_height_, 1};
            vkCmdCopyImageToBuffer(slot.cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.readback, 1,
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
            vkCmdPipelineBarrier2(slot.cmd, &dependency);
        }
    }
    if (!valid()) return;
    if (presenter_ != nullptr) recordPresentBlit(slot, display_black_ ? nullptr : it->second.get());
    if (submitFrame()) has_flip_ = true;
}

// Acquires a swapchain image and records the copy of the display buffer into it (a black frame when
// `source` is null). Without an image (minimised window, timeout) nothing is recorded and the frame is
// submitted as usual. FIFO presentation can make the acquire block; the presenter measures it.
void RsxRenderer::recordPresentBlit(FrameSlot& slot, RenderTarget* source) {
    if (!presenter_->acquire(current_slot_, present_target_)) return;
    present_pending_ = true;
    const Presenter::Target& dst = present_target_;
    const VkCommandBuffer cmd = slot.cmd;

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
        region.srcOffsets[1] = {static_cast<std::int32_t>(source->width), static_cast<std::int32_t>(source->height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {rect.x, rect.y, 0};
        region.dstOffsets[1] = {rect.x + static_cast<std::int32_t>(rect.width),
                                rect.y + static_cast<std::int32_t>(rect.height), 1};
        vkCmdBlitImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    }
    barrierTo(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, kTransfer,
              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
}

void RsxRenderer::execute(std::span<RsxCommand> commands) {
    for (RsxCommand& command : commands) {
        if (!beginFrame()) return;
        if (const auto* clear = std::get_if<RsxClear>(&command)) {
            recordClear(*clear);
        } else if (const auto* draw = std::get_if<RsxDraw>(&command)) {
            recordDraw(*draw);
        } else {
            recordFlip(std::get<RsxFlip>(command));
            if (valid() && dumpDue()) dumpDisplay();
        }
        if (!valid()) return;
    }
}

// The flip number counts from 1; stats_.flips already includes the flip being recorded.
bool RsxRenderer::dumpDue() const {
    return !options_.dump_dir.empty() && stats_.flips % std::max<std::uint32_t>(options_.dump_every, 1) == 0;
}

// Only the readback happens on the calling (RSX) thread; the dump worker converts, encodes and writes. A
// full queue drops the frame instead of making the flip wait.
void RsxRenderer::dumpDisplay() {
    if (!dumper_->hasRoom()) {
        dumper_->countSkipped();
        logOnce(LogLevel::Warn, "render-dump-skipped", "tox2: render: dump skipped (queue full) at flip {}",
                stats_.flips);
        return;
    }
    DumpFrame frame;
    frame.bgra = dumper_->takeBuffer();
    if (!readDisplay(frame.bgra, frame.width, frame.height)) {
        dumper_->recycleBuffer(std::move(frame.bgra));
        return;
    }
    frame.path = options_.dump_dir / std::format("frame_{:06}.png", stats_.flips);
    dumper_->submit(std::move(frame));
}

DumpCounts RsxRenderer::dumpCounts() const { return dumper_ ? dumper_->counts() : DumpCounts{}; }

std::uint64_t RsxRenderer::flushDumps(std::chrono::milliseconds timeout) {
    return dumper_ ? dumper_->flush(timeout) : 0;
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

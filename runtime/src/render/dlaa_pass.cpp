#include "render/dlaa_pass.h"

#include <utility>

namespace tox2 {
namespace {

SlImage describe(const DlaaPass::Image& image) {
    SlImage out;
    out.image = image.image;
    out.memory = image.memory;
    out.view = image.view;
    out.layout = image.layout;
    out.width = image.width;
    out.height = image.height;
    out.format = image.format;
    out.usage = image.usage;
    return out;
}

bool createImage(VkContext& context, VkFormat format, VkImageUsageFlags usage, std::uint32_t width,
                 std::uint32_t height, DlaaPass::Image& out, std::string& error) {
    out.context = &context;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Streamline is told the device memory of the images, so each one owns a whole allocation.
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateImage(context.allocator(), &image_info, &alloc_info, &out.image, &out.allocation, &allocated) !=
        VK_SUCCESS) {
        out.image = VK_NULL_HANDLE;
        error = "vmaCreateImage failed for a DLAA image";
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(context.device(), &view_info, nullptr, &out.view) != VK_SUCCESS) {
        out.view = VK_NULL_HANDLE;
        error = "vkCreateImageView failed for a DLAA image";
        return false;
    }
    out.memory = allocated.deviceMemory;
    out.width = width;
    out.height = height;
    out.format = format;
    out.usage = usage;
    return true;
}

}  // namespace

DlaaPass::Image::~Image() {
    if (context == nullptr) return;
    for (const auto& entry : sampled_views) vkDestroyImageView(context->device(), entry.second, nullptr);
    if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
}

DlaaPass::DlaaPass(VkContext& context, Streamline& streamline)
    : context_(context), streamline_(streamline), viewport_(streamline.newViewport()) {}

DlaaPass::~DlaaPass() {
    resources_.reset();
    streamline_.freeViewport(viewport_);
}

bool DlaaPass::prepare(std::uint32_t width, std::uint32_t height, std::shared_ptr<void>& retired, bool& recreated,
                       std::string& error) {
    recreated = false;
    if (resources_ && resources_->output.width == width && resources_->output.height == height) return true;
    if (width == 0 || height == 0) {
        error = "the scene image has no size";
        return false;
    }
    auto created = std::make_shared<Resources>();
    // SL clears the output with vkCmdClearColorImage and writes it as a storage image; the widening blit reads it.
    constexpr VkImageUsageFlags kOutputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags kMotionUsage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    constexpr VkImageUsageFlags kExpandedUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!createImage(context_, VK_FORMAT_R8G8B8A8_UNORM, kOutputUsage, width, height, created->output, error) ||
        !createImage(context_, VK_FORMAT_R16G16_SFLOAT, kMotionUsage, width, height, created->motion, error) ||
        !createImage(context_, VK_FORMAT_R8G8B8A8_UNORM, kExpandedUsage, width * 2, height, created->expanded, error)) {
        return false;
    }
    retired = std::move(resources_);
    resources_ = std::move(created);
    recreated = true;
    return true;
}

bool DlaaPass::evaluate(VkCommandBuffer cmd, const Inputs& inputs, std::string& error) {
    DlaaFrame frame;
    frame.viewport = viewport_;
    frame.frame_index = inputs.frame_index;
    frame.color_in = inputs.color_in;
    frame.depth = inputs.depth;
    frame.motion = describe(resources_->motion);
    frame.color_out = describe(resources_->output);
    frame.jitter_px[0] = inputs.jitter_px[0];
    frame.jitter_px[1] = inputs.jitter_px[1];
    frame.mvec_scale[0] = 1.0f / static_cast<float>(inputs.color_in.width);
    frame.mvec_scale[1] = 1.0f / static_cast<float>(inputs.color_in.height);
    frame.view_to_clip = inputs.matrices.view_to_clip;
    frame.clip_to_view = inputs.matrices.clip_to_view;
    frame.clip_to_prev_clip = inputs.matrices.clip_to_prev_clip;
    frame.prev_clip_to_clip = inputs.matrices.prev_clip_to_clip;
    frame.mvec_invalid_value = inputs.mvec_invalid_value;
    frame.reset = inputs.reset;
    frame.alpha = inputs.alpha;
    return streamline_.evaluateDlaa(cmd, frame, error);
}

void DlaaCameraTracker::record(const DlaaCameraKey& key) {
    for (Entry& entry : entries_) {
        if (entry.key == key) {
            entry.draws++;
            return;
        }
    }
    if (entries_.size() >= kMaxKeys) return;
    entries_.push_back({key, 1});
}

// The most drawn key; the first seen wins a tie.
const DlaaCameraTracker::Entry* DlaaCameraTracker::dominant() const {
    const Entry* best = nullptr;
    for (const Entry& entry : entries_) {
        if (best == nullptr || entry.draws > best->draws) best = &entry;
    }
    return best;
}

DlaaCameraTracker::Result DlaaCameraTracker::resolve(bool flip_clip_y) {
    Result result;  // identity matrices and a reset, until both frames have a usable camera
    const Entry* current = dominant();
    resolved_ = true;
    resolved_key_ = current != nullptr ? std::optional<DlaaCameraKey>(current->key) : std::nullopt;
    if (current == nullptr || !previous_) return result;
    const Mat4 clip_current = dlaaClipMatrix(current->key);
    const Mat4 clip_previous = dlaaClipMatrix(*previous_);
    Mat4 inverse_current{}, inverse_previous{};
    if (!mat4Inverse(clip_current, inverse_current) || !mat4Inverse(clip_previous, inverse_previous)) return result;

    DlaaMatrices& matrices = result.matrices;
    matrices.view_to_clip = clip_current;
    matrices.clip_to_view = inverse_current;
    matrices.clip_to_prev_clip = mat4Multiply(inverse_current, clip_previous);
    matrices.prev_clip_to_clip = mat4Multiply(inverse_previous, clip_current);
    result.reset = dlaaCameraCut(matrices.clip_to_prev_clip);
    if (flip_clip_y) {
        matrices.clip_to_prev_clip = dlaaFlipY(matrices.clip_to_prev_clip);
        matrices.prev_clip_to_clip = dlaaFlipY(matrices.prev_clip_to_clip);
    }
    return result;
}

void DlaaCameraTracker::endFrame() {
    // The evaluation saw only the draws before the resolve, and its history was produced with that camera, so that
    // is what the next frame's motion is measured from. Without an evaluation nothing was produced; the whole
    // frame's camera is kept anyway, and the next evaluation resets because it does not follow one.
    if (resolved_) {
        previous_ = resolved_key_;
    } else {
        const Entry* best = dominant();
        previous_ = best != nullptr ? std::optional<DlaaCameraKey>(best->key) : std::nullopt;
    }
    entries_.clear();
    resolved_ = false;
    resolved_key_.reset();
}

}  // namespace tox2

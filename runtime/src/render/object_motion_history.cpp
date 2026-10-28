#include "render/object_motion_history.h"

#include <algorithm>

namespace tox2 {
namespace {

// Source ranges closer than this are copied as one region: the uploads are aligned to 256 bytes, so the arrays of one
// draw sit back to back with only alignment padding between them, and one region per draw is much cheaper to record
// than one per array.
constexpr VkDeviceSize kMergeGap = 256;
constexpr VkDeviceSize kVertexAlignment = 16;
constexpr VkDeviceSize kMinimumCapacity = 64 * 1024;

// Keeps a replaced history buffer alive until the commands that bind it have finished.
struct RetiredBuffer {
    VkContext* context;
    VkBuffer buffer;
    VmaAllocation allocation;
    std::shared_ptr<void> earlier;

    RetiredBuffer(VkContext* context, VkBuffer buffer, VmaAllocation allocation, std::shared_ptr<void> earlier)
        : context(context), buffer(buffer), allocation(allocation), earlier(std::move(earlier)) {}
    RetiredBuffer(const RetiredBuffer&) = delete;
    RetiredBuffer& operator=(const RetiredBuffer&) = delete;
    ~RetiredBuffer() { vmaDestroyBuffer(context->allocator(), buffer, allocation); }
};

constexpr VkDeviceSize roundUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

ObjectMotionHistory::ObjectMotionHistory(VkContext& context) : context_(context) {}

ObjectMotionHistory::~ObjectMotionHistory() {
    for (Buffer& buffer : buffers_) {
        if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(context_.allocator(), buffer.buffer, buffer.allocation);
    }
}

void ObjectMotionHistory::beginFrame(std::uint64_t frame, std::uint32_t width, std::uint32_t height,
                                     MotionMatchRule rule) {
    frame_ = frame;
    width_ = width;
    height_ = height;
    active_ = true;
    stats_.candidates = stats_.matched = 0;
    if (!has_previous_ || previous_frame_ + 1 != frame || previous_width_ != width || previous_height_ != height) {
        previous_.clear();
        has_previous_ = false;
    }
    current_.clear();
    matcher_.beginFrame(previous_.signatures, rule);
}

const ObjectMotionHistory::Previous* ObjectMotionHistory::match(const MotionDrawSignature& signature) {
    stats_.candidates++;
    const std::optional<std::uint32_t> index = matcher_.match(signature);
    if (!index) return nullptr;
    stats_.matched++;
    return &previous_.items[*index];
}

void ObjectMotionHistory::record(const MotionDrawSignature& signature, const Range& constants,
                                 const VpDrawUniforms& draw, const std::array<Range, kPipelineVertexSlots>& uploads) {
    current_.signatures.push_back(signature);
    Previous& item = current_.items.emplace_back();
    item.constants = constants;
    item.draw = draw;
    // Holds the upload ranges until recordCopy() replaces them with the history ranges.
    item.slots = uploads;
}

VkDeviceSize ObjectMotionHistory::place(VkBuffer source, VkDeviceSize offset, VkDeviceSize size,
                                        VkDeviceSize alignment) {
    copied_bytes_ += size;
    std::size_t index = last_source_;
    if (!have_region_ || sources_[index].buffer != source) {
        index = 0;
        while (index < source_count_ && sources_[index].buffer != source) index++;
        if (index == source_count_) {
            if (source_count_ == sources_.size()) sources_.emplace_back();
            sources_[source_count_].buffer = source;
            sources_[source_count_].regions.clear();
            source_count_++;
        }
    }
    std::vector<VkBufferCopy>& regions = sources_[index].regions;
    if (have_region_ && last_source_ == index) {
        VkBufferCopy& last = regions.back();
        const VkDeviceSize last_end = last.srcOffset + last.size;
        if (offset >= last_end && offset - last_end < kMergeGap) {
            const VkDeviceSize destination = last.dstOffset + (offset - last.srcOffset);
            if (destination % alignment == 0) {
                last.size = offset + size - last.srcOffset;
                bump_ = last.dstOffset + last.size;
                return destination;
            }
        }
    }
    const VkDeviceSize destination = roundUp(bump_, alignment);
    regions.push_back({offset, destination, size});
    bump_ = destination + size;
    last_source_ = index;
    have_region_ = true;
    return destination;
}

bool ObjectMotionHistory::ensureCapacity(Buffer& buffer, VkDeviceSize needed, std::shared_ptr<void>& retired,
                                         std::string& error) {
    if (buffer.size >= needed) return true;
    const VkDeviceSize size = std::max(needed + needed / 4, kMinimumCapacity);
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    // TRANSFER_SRC lets the tests read the history back.
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Buffer created;
    if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &created.buffer, &created.allocation,
                        nullptr) != VK_SUCCESS) {
        error = "vmaCreateBuffer failed for the object motion history buffer";
        return false;
    }
    created.size = size;
    if (buffer.buffer != VK_NULL_HANDLE) {
        retired = std::make_shared<RetiredBuffer>(&context_, buffer.buffer, buffer.allocation, std::move(retired));
    }
    buffer = created;
    return true;
}

bool ObjectMotionHistory::recordCopy(VkCommandBuffer cmd, std::shared_ptr<void>& retired, std::string& error) {
    if (!active_) {
        // A flip without candidates must not report the last frame's counts again.
        stats_.candidates = stats_.matched = 0;
        return true;
    }
    active_ = false;
    // The previous records stay as they are; their frame number is stale by the next beginFrame().
    if (current_.items.empty()) return true;

    const VkDeviceSize constants_alignment =
            std::max<VkDeviceSize>(context_.deviceProperties().limits.minUniformBufferOffsetAlignment, 16);
    source_count_ = 0;
    have_region_ = false;
    bump_ = 0;
    copied_bytes_ = 0;
    constants_.clear();

    ConstantsKey last_key{VK_NULL_HANDLE, 0};
    VkDeviceSize last_offset = 0;
    for (Previous& item : current_.items) {
        const ConstantsKey key{item.constants.buffer, item.constants.offset};
        // Draws that share a set are usually consecutive.
        if (key != last_key) {
            auto [it, inserted] = constants_.try_emplace(key, 0);
            if (inserted) it->second = place(key.buffer, key.offset, item.constants.size, constants_alignment);
            last_key = key;
            last_offset = it->second;
        }
        item.constants.offset = last_offset;
        for (Range& slot : item.slots) {
            if (slot.size == 0) continue;
            slot.offset = place(slot.buffer, slot.offset, slot.size, kVertexAlignment);
        }
    }

    Buffer& target = buffers_[frame_ % 2];
    if (!ensureCapacity(target, bump_, retired, error)) {
        current_.clear();
        previous_.clear();
        has_previous_ = false;
        return false;
    }
    for (Previous& item : current_.items) {
        item.constants.buffer = target.buffer;
        for (Range& slot : item.slots) {
            if (slot.size != 0) slot.buffer = target.buffer;
        }
    }

    // The same barrier in both directions: the first orders the reads of the frame that last used this buffer before
    // the copy, the second the copy before the reads of the next frame.
    constexpr VkPipelineStageFlags2 kReadStages =
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    constexpr VkAccessFlags2 kReadAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    barrier.srcStageMask = kReadStages;
    barrier.srcAccessMask = kReadAccess;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &dependency);

    for (std::size_t i = 0; i < source_count_; i++) {
        const SourceCopy& source = sources_[i];
        vkCmdCopyBuffer(cmd, source.buffer, target.buffer, static_cast<std::uint32_t>(source.regions.size()),
                        source.regions.data());
    }

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = kReadStages;
    barrier.dstAccessMask = kReadAccess;
    vkCmdPipelineBarrier2(cmd, &dependency);

    std::swap(previous_, current_);
    has_previous_ = true;
    previous_frame_ = frame_;
    previous_width_ = width_;
    previous_height_ = height_;
    stats_.history_bytes = copied_bytes_;
    return true;
}

void ObjectMotionHistory::clear() {
    previous_.clear();
    current_.clear();
    has_previous_ = false;
    active_ = false;
}

}  // namespace tox2

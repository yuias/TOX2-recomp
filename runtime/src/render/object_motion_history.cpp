#include "render/object_motion_history.h"

#include <algorithm>

namespace tox2 {
namespace {

// Source ranges of one buffer closer than this are copied as one region, gap bytes included. The GPU spends about
// 0.8 us per region and about 0.05 ms on 8 MB, so a few hundred regions cost far more than the gaps they would save;
// the draws' arrays and constants sit within a few uploaded blocks, which this merges into a handful of regions.
constexpr VkDeviceSize kMergeGap = 64 * 1024;
constexpr VkDeviceSize kVertexAlignment = 16;
constexpr VkDeviceSize kRegionAlignment = 256;
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
    stats_.regions = 0;
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
    entries_.clear();
    item_constants_.clear();
    slot_entries_.clear();
    constants_.clear();

    ConstantsKey last_key{VK_NULL_HANDLE, 0};
    std::uint32_t last_entry = 0;
    for (const Previous& item : current_.items) {
        const ConstantsKey key{item.constants.buffer, item.constants.offset};
        // Draws that share a set are usually consecutive.
        if (key != last_key) {
            auto [it, inserted] = constants_.try_emplace(key, 0);
            if (inserted) {
                it->second = static_cast<std::uint32_t>(entries_.size());
                entries_.push_back({key.buffer, key.offset, item.constants.size, constants_alignment, it->second});
            }
            last_key = key;
            last_entry = it->second;
        }
        item_constants_.push_back(last_entry);
        for (const Range& slot : item.slots) {
            if (slot.size == 0) continue;
            const auto id = static_cast<std::uint32_t>(entries_.size());
            entries_.push_back({slot.buffer, slot.offset, slot.size, kVertexAlignment, id});
            slot_entries_.push_back(id);
        }
    }

    // Placing the entries in source order lets neighbours of one source buffer share a region however the draws
    // interleaved them; a region costs the GPU much more than the bytes in a gap.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return std::bit_cast<std::uint64_t>(a.buffer) != std::bit_cast<std::uint64_t>(b.buffer)
                       ? std::bit_cast<std::uint64_t>(a.buffer) < std::bit_cast<std::uint64_t>(b.buffer)
                       : a.offset < b.offset;
    });
    destinations_.resize(entries_.size());
    regions_.clear();
    groups_.clear();
    VkDeviceSize bump = 0;
    VkDeviceSize used_bytes = 0;
    bool open = false;  // the last region belongs to the current group and can still grow
    for (const Entry& entry : entries_) {
        used_bytes += entry.size;
        if (groups_.empty() || groups_.back().buffer != entry.buffer) {
            groups_.push_back({entry.buffer, regions_.size(), 0});
            open = false;
        }
        if (open) {
            VkBufferCopy& region = regions_.back();
            const VkDeviceSize end = region.srcOffset + region.size;
            const VkDeviceSize destination = region.dstOffset + (entry.offset - region.srcOffset);
            if (entry.offset <= end + kMergeGap && destination % entry.alignment == 0) {
                region.size = std::max(end, entry.offset + entry.size) - region.srcOffset;
                destinations_[entry.id] = destination;
                bump = region.dstOffset + region.size;
                continue;
            }
        }
        // Keeping the destination congruent to the source modulo the region alignment lets the entries that join the
        // region later keep the alignment their source has.
        VkDeviceSize destination = roundUp(bump, kRegionAlignment) + entry.offset % kRegionAlignment;
        if (destination % entry.alignment != 0) destination = roundUp(bump, entry.alignment);
        regions_.push_back({entry.offset, destination, entry.size});
        groups_.back().count++;
        destinations_[entry.id] = destination;
        bump = destination + entry.size;
        open = true;
    }

    // The sorted order is gone from here on; entries are looked up by id.
    std::size_t next_slot = 0;
    for (std::size_t i = 0; i < current_.items.size(); i++) {
        Previous& item = current_.items[i];
        item.constants.offset = destinations_[item_constants_[i]];
        for (Range& slot : item.slots) {
            if (slot.size != 0) slot.offset = destinations_[slot_entries_[next_slot++]];
        }
    }

    Buffer& target = buffers_[frame_ % 2];
    if (!ensureCapacity(target, bump, retired, error)) {
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

    for (const Group& group : groups_) {
        vkCmdCopyBuffer(cmd, group.buffer, target.buffer, static_cast<std::uint32_t>(group.count),
                        &regions_[group.first]);
    }
    stats_.regions = regions_.size();

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
    stats_.history_bytes = used_bytes;
    return true;
}

void ObjectMotionHistory::clear() {
    previous_.clear();
    current_.clear();
    has_previous_ = false;
    active_ = false;
}

}  // namespace tox2

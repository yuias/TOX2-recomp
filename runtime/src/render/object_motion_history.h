#pragma once

// The previous frame's inputs of the object motion candidates (object_motion.h), kept on the GPU.
//
// Upload memory is per frame slot and reused two frames later, so frame N cannot bind the uploads of frame N-1. The
// converted vertex arrays and the vertex constant blocks are therefore copied on the GPU, in frame N-1's own command
// buffer, into a device-local history buffer that frame N binds. Nothing is converted or uploaded a second time.

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/object_motion.h"
#include "render/vk_context.h"
#include "render/vk_pipelines.h"
#include "render/vp_translator.h"

namespace tox2 {

// Renderer thread.
class ObjectMotionHistory {
public:
    explicit ObjectMotionHistory(VkContext& context);
    ~ObjectMotionHistory();  // the caller has waited for the device to be idle
    ObjectMotionHistory(const ObjectMotionHistory&) = delete;
    ObjectMotionHistory& operator=(const ObjectMotionHistory&) = delete;

    struct Range {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, size = 0;
    };
    struct Previous {
        // History-buffer range of the vertex constants (kRsxVpConstantCount * 16 bytes, offset aligned to
        // minUniformBufferOffsetAlignment); always set, the current upload is fully written (zero padded).
        Range constants;
        VpDrawUniforms draw{};
        // History-buffer ranges of the slots that had an array; size 0 = the slot read its default (bind the
        // current draw's default block).
        std::array<Range, kPipelineVertexSlots> slots{};
    };
    struct Stats {
        std::uint64_t candidates = 0, matched = 0;  // this frame
        std::uint64_t history_bytes = 0;            // the last copy: vertex bytes plus each distinct constants set once
        std::uint64_t regions = 0;                  // VkBufferCopy regions recorded by the last recordCopy(); 0 if none
    };

    // Frame `frame` (the flip number) of a scene of this size starts. The previous frame's records stay usable only
    // when `frame` is the previous frame + 1 and the size is unchanged; otherwise nothing matches this frame.
    void beginFrame(std::uint64_t frame, std::uint32_t width, std::uint32_t height, MotionMatchRule rule);
    // Once per candidate, in draw order; null when unmatched. The ranges of the result lie in the history buffer and
    // stay valid until recordCopy().
    const Previous* match(const MotionDrawSignature& signature);
    // This candidate's inputs: its vertex arrays' ranges and its vertex constants' range (the upload the draw binds,
    // freshly written or reused) in this frame's upload buffers (valid until this frame's commands finish), and its
    // VpDrawUniforms. A slot without an array has size 0.
    void record(const MotionDrawSignature& signature, const Range& constants, const VpDrawUniforms& draw,
                const std::array<Range, kPipelineVertexSlots>& uploads);
    // Outside rendering, before the frame's submit: copies every recorded vertex range, and every distinct constants
    // range (deduplicated by buffer and offset), into history buffer [frame % 2], one vkCmdCopyBuffer per source
    // buffer. The buffer is grown when too small; the old one goes to `retired`, which the caller keeps alive until
    // this slot's fence. Nothing is recorded when there were no candidates. After it the frame's records are what
    // `match` returns next frame. False with `error` when the buffer cannot be created.
    bool recordCopy(VkCommandBuffer cmd, std::shared_ptr<void>& retired, std::string& error);
    void clear();
    Stats stats() const { return stats_; }

private:
    struct Records {
        std::vector<MotionDrawSignature> signatures;
        std::vector<Previous> items;
        void clear() {
            signatures.clear();
            items.clear();
        }
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceSize size = 0;
    };
    // One source range to copy; `id` names it after the sort that groups the entries by source.
    struct Entry {
        VkBuffer buffer;
        VkDeviceSize offset, size, alignment;
        std::uint32_t id;
    };
    // The regions [first, first + count) read one source buffer.
    struct Group {
        VkBuffer buffer;
        std::size_t first, count;
    };
    struct ConstantsKey {
        VkBuffer buffer;
        VkDeviceSize offset;
        bool operator==(const ConstantsKey&) const = default;
    };
    struct ConstantsKeyHash {
        std::size_t operator()(const ConstantsKey& key) const {
            std::uint64_t state = std::bit_cast<std::uint64_t>(key.buffer);
            detail::motionMix(state, key.offset);
            return static_cast<std::size_t>(state);
        }
    };

    bool ensureCapacity(Buffer& buffer, VkDeviceSize needed, std::shared_ptr<void>& retired, std::string& error);

    VkContext& context_;
    std::array<Buffer, 2> buffers_;
    MotionDrawMatcher matcher_;
    Records previous_, current_;
    bool has_previous_ = false;
    std::uint64_t previous_frame_ = 0, frame_ = 0;
    std::uint32_t previous_width_ = 0, previous_height_ = 0, width_ = 0, height_ = 0;
    bool active_ = false;
    Stats stats_;

    // Scratch of recordCopy, kept for its capacity.
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> item_constants_;  // per item: the id of its constants entry
    std::vector<std::uint32_t> slot_entries_;    // per used slot, in item and slot order: its entry id
    std::vector<VkDeviceSize> destinations_;     // per entry id
    std::vector<VkBufferCopy> regions_;
    std::vector<Group> groups_;
    std::unordered_map<ConstantsKey, std::uint32_t, ConstantsKeyHash> constants_;
};

}  // namespace tox2

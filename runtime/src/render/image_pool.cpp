#include "render/image_pool.h"

#include <iterator>
#include <limits>

namespace tox2 {

ImagePool::ImagePool(VkContext& context, ImagePoolLimits limits) : context_(context), limits_(limits) {}

ImagePool::~ImagePool() {
    for (auto& [key, queue] : entries_) {
        for (const Entry& entry : queue) vmaDestroyImage(context_.allocator(), entry.image, entry.allocation);
    }
}

ImagePoolResult ImagePool::take(const PooledImageKey& key, VkImage& image, VmaAllocation& allocation) {
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        const Entry entry = it->second.front();
        it->second.pop_front();
        if (it->second.empty()) entries_.erase(it);
        bytes_ -= entry.bytes;
        entries_count_--;
        image = entry.image;
        allocation = entry.allocation;
        return {true, ImagePoolMiss::NoEntry};
    }
    const auto pending = pending_.find(key);
    if (pending != pending_.end()) {
        for (const ImagePoolMiss why : {ImagePoolMiss::KeyCap, ImagePoolMiss::ByteCap, ImagePoolMiss::Idle}) {
            std::uint32_t& count = pending->second[static_cast<std::size_t>(why)];
            if (count == 0) continue;
            count--;
            return {false, why};
        }
        // Everything reported: forget the key so the map does not keep one node per size ever seen.
        pending_.erase(pending);
    }
    return {false, ImagePoolMiss::NoEntry};
}

void ImagePool::notePending(const PooledImageKey& key, ImagePoolMiss why) {
    pending_[key][static_cast<std::size_t>(why)]++;
    switch (why) {
        case ImagePoolMiss::KeyCap: discards_.key_cap++; break;
        case ImagePoolMiss::ByteCap: discards_.byte_cap++; break;
        case ImagePoolMiss::Idle: discards_.idle++; break;
        case ImagePoolMiss::NoEntry: break;
    }
}

void ImagePool::discardOldest(std::map<PooledImageKey, std::deque<Entry>>::iterator it, ImagePoolMiss why) {
    const Entry entry = it->second.front();
    vmaDestroyImage(context_.allocator(), entry.image, entry.allocation);
    bytes_ -= entry.bytes;
    entries_count_--;
    notePending(it->first, why);
    it->second.pop_front();
    if (it->second.empty()) entries_.erase(it);
}

void ImagePool::give(const PooledImageKey& key, std::uint64_t bytes, VkImage image, VmaAllocation allocation) {
    if (bytes > limits_.max_bytes) {
        // Keeping it would evict everything else and then itself.
        vmaDestroyImage(context_.allocator(), image, allocation);
        notePending(key, ImagePoolMiss::ByteCap);
        return;
    }
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.size() >= limits_.max_per_key) {
        discardOldest(it, ImagePoolMiss::KeyCap);
        it = entries_.find(key);
    }
    if (it == entries_.end()) it = entries_.try_emplace(key).first;
    it->second.push_back({image, allocation, bytes, last_flip_, next_seq_++});
    bytes_ += bytes;
    entries_count_++;

    while (bytes_ > limits_.max_bytes) {
        auto oldest = entries_.end();
        std::uint64_t oldest_seq = std::numeric_limits<std::uint64_t>::max();
        for (auto scan = entries_.begin(); scan != entries_.end(); ++scan) {
            if (scan->second.front().seq < oldest_seq) {
                oldest_seq = scan->second.front().seq;
                oldest = scan;
            }
        }
        discardOldest(oldest, ImagePoolMiss::ByteCap);
    }
}

void ImagePool::trim(std::uint64_t flip) {
    last_flip_ = flip;
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto& queue = it->second;
        // Entries are stamped in give order, so the stale ones are a prefix.
        while (!queue.empty() && flip - queue.front().flip > limits_.max_idle_flips) {
            const Entry entry = queue.front();
            vmaDestroyImage(context_.allocator(), entry.image, entry.allocation);
            bytes_ -= entry.bytes;
            entries_count_--;
            notePending(it->first, ImagePoolMiss::Idle);
            queue.pop_front();
        }
        it = queue.empty() ? entries_.erase(it) : std::next(it);
    }
}

}  // namespace tox2

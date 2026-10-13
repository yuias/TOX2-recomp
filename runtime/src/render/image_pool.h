#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

#include "render/vk_context.h"

namespace tox2 {

struct PooledImageKey {
    VkFormat format;
    std::uint32_t width, height;
    VkSampleCountFlagBits samples;
    VkImageUsageFlags usage;
    auto operator<=>(const PooledImageKey&) const = default;
};

// Why take() found no entry. NoEntry: nothing of this key was discarded since the key's last take (the key is new,
// or its retired images have not come back yet). The others: an image of this key was given back but destroyed by
// the per-key cap, the byte cap or the idle trim, and this miss is the creation it would have served.
enum class ImagePoolMiss : std::uint8_t { NoEntry, KeyCap, ByteCap, Idle };
inline constexpr std::size_t kImagePoolMissCount = 4;

struct ImagePoolResult {
    bool hit = false;
    ImagePoolMiss miss = ImagePoolMiss::NoEntry;  // meaningful when !hit
};

struct ImagePoolLimits {
    std::size_t max_per_key;
    std::uint64_t max_bytes;
    std::uint64_t max_idle_flips;
};

struct ImagePoolDiscards {
    std::uint64_t key_cap = 0, byte_cap = 0, idle = 0;
};

// Images whose last GPU use has completed (their owner was destroyed after its slot's fence) wait here for a creation
// with the same parameters. Bounded per key and in total; entries not taken within max_idle_flips flips are destroyed
// by trim(). Single-threaded, like the renderer. Must be destroyed before the VkContext.
class ImagePool {
public:
    static constexpr std::size_t kMaxPerKey = 8;
    static constexpr std::uint64_t kMaxBytes = 768ull << 20;
    static constexpr std::uint64_t kMaxIdleFlips = 300;
    static constexpr ImagePoolLimits kDefaultLimits{kMaxPerKey, kMaxBytes, kMaxIdleFlips};

    explicit ImagePool(VkContext& context, ImagePoolLimits limits = kDefaultLimits);
    ~ImagePool();  // destroys what is left
    ImagePool(const ImagePool&) = delete;
    ImagePool& operator=(const ImagePool&) = delete;

    // On a hit, hands out the oldest entry of `key` (it leaves the pool). FIFO so every entry rotates through: with
    // newest-first, a double return would leave old entries that the key cap then destroys every frame while the
    // counters show only hits. On a miss, consumes one pending discard of this key to name the reason (KeyCap, then
    // ByteCap, then Idle), else NoEntry.
    ImagePoolResult take(const PooledImageKey& key, VkImage& image, VmaAllocation& allocation);
    // Keeps the image, stamped with the flip of the last trim(). When the key already holds max_per_key entries,
    // its oldest entry is destroyed (a pending KeyCap discard of the key). Then, while the total exceeds max_bytes,
    // the oldest entry of any key is destroyed (a pending ByteCap discard of that entry's key); an image larger than
    // max_bytes by itself is destroyed at once (ByteCap).
    void give(const PooledImageKey& key, std::uint64_t bytes, VkImage image, VmaAllocation allocation);
    // Called once per flip; destroys entries given more than max_idle_flips flips ago (pending Idle discards).
    void trim(std::uint64_t flip);
    std::uint64_t bytes() const { return bytes_; }
    std::size_t entries() const { return entries_count_; }
    const ImagePoolDiscards& discards() const { return discards_; }

private:
    struct Entry {
        VkImage image;
        VmaAllocation allocation;
        std::uint64_t bytes;
        std::uint64_t flip;  // last_flip_ when given
        std::uint64_t seq;   // give order across keys
    };
    // Discards not yet reported by a miss, indexed by ImagePoolMiss (NoEntry unused).
    using PendingDiscards = std::array<std::uint32_t, kImagePoolMissCount>;

    // Destroys the oldest entry of `key` and records it as a pending discard of kind `why`.
    void discardOldest(std::map<PooledImageKey, std::deque<Entry>>::iterator it, ImagePoolMiss why);
    void notePending(const PooledImageKey& key, ImagePoolMiss why);

    VkContext& context_;
    ImagePoolLimits limits_;
    std::map<PooledImageKey, std::deque<Entry>> entries_;  // non-empty deques only, oldest first
    std::map<PooledImageKey, PendingDiscards> pending_;
    std::uint64_t bytes_ = 0;
    std::size_t entries_count_ = 0;
    std::uint64_t last_flip_ = 0;
    std::uint64_t next_seq_ = 0;
    ImagePoolDiscards discards_;
};

}  // namespace tox2

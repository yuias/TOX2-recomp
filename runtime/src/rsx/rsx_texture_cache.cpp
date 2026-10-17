#include "rsx/rsx_texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include <xxhash.h>

// The wide XXH3 path is chosen from the compile flags (-march=x86-64-v3); a build without AVX2 would still hash
// correctly but several times slower, so it is refused here.
static_assert(XXH_VECTOR == XXH_AVX2, "rsx_texture_cache.cpp must be compiled with AVX2 for the XXH3 wide path");

namespace tox2 {
namespace {

__extension__ typedef unsigned __int128 U128;

constexpr std::uint64_t kSecret0 = 0xA0761D6478BD642Full;
constexpr std::uint64_t kSecret1 = 0xE7037ED1A0B428DBull;
constexpr std::uint64_t kSecret2 = 0x8EBC6AF09C88C6E3ull;
constexpr std::uint64_t kSecret3 = 0x589965CC75374CC3ull;

// Multiplies and folds the two halves of the 128-bit product (the mixing step of wyhash).
std::uint64_t mix(std::uint64_t a, std::uint64_t b) {
    const U128 product = static_cast<U128>(a) * b;
    return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64);
}

}  // namespace

std::uint64_t rsxHashBytes(std::span<const std::uint8_t> bytes) { return XXH3_64bits(bytes.data(), bytes.size()); }

std::size_t RsxTextureCache::KeyHash::operator()(const Key& key) const {
    std::uint64_t h = mix(key.location ^ kSecret0, key.offset ^ kSecret1);
    h = mix(h ^ (static_cast<std::uint64_t>(key.width) << 32 | key.height), key.pitch ^ kSecret2);
    h = mix(h ^ (static_cast<std::uint64_t>(key.depth) << 32 | key.format << 16 | key.mips << 8 | key.layers),
            key.linear ? kSecret3 : kSecret0);
    return static_cast<std::size_t>(h);
}

std::uint64_t RsxTextureCache::nextContentId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

RsxTextureCache::Entry* RsxTextureCache::find(const Key& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return nullptr;
    it->second.last_use = ++use_tick_;
    it->second.last_flip = flip_;
    return &it->second;
}

bool RsxTextureCache::fresh(const Entry& entry) const {
    return entry.epoch == epoch_ && !writtenSince(entry.location, entry.begin, entry.end, entry.serial);
}

bool RsxTextureCache::writtenSince(std::uint32_t location, std::uint64_t begin, std::uint64_t end,
                                   std::uint64_t serial) const {
    for (const WriteRange& write : writes_) {
        if (write.serial > serial && write.location == location && write.begin < end && begin < write.end) {
            return true;
        }
    }
    return false;
}

void RsxTextureCache::markFresh(Entry& entry) {
    entry.epoch = epoch_;
    entry.serial = serial_;
}

RsxTextureCache::Entry& RsxTextureCache::store(const Key& key, const RsxTextureLayout& layout, std::uint64_t hash,
                                               std::shared_ptr<const std::vector<std::uint8_t>> data) {
    generation_++;
    Entry& entry = entries_[key];
    bytes_ -= entryBytes(entry);
    entry.data = std::move(data);
    bytes_ += entryBytes(entry);
    entry.content_id = nextContentId();
    entry.hash = hash;
    entry.guest_bytes = layout.guest_bytes;
    entry.pitch = layout.levels.empty() ? 0 : static_cast<std::uint32_t>(layout.levels[0].row_bytes);
    entry.layout = layout;
    entry.location = key.location;
    entry.begin = key.offset;
    entry.end = static_cast<std::uint64_t>(key.offset) + layout.guest_bytes;
    entry.last_use = ++use_tick_;
    entry.last_flip = flip_;
    markFresh(entry);
    // The new entry is the one in use, so it is never the one dropped to make room.
    if (bytes_ > budget_bytes_) trimBytes(&entry);
    return entry;
}

void RsxTextureCache::erase(const Key& key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return;
    bytes_ -= entryBytes(it->second);
    entries_.erase(it);
    generation_++;
}

void RsxTextureCache::beginEpoch() {
    epoch_++;
    writes_.clear();
}

void RsxTextureCache::noteWrite(std::uint32_t location, std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    serial_++;
    for (WriteRange& write : writes_) {
        if (write.location == location && write.begin == begin && write.end == end) {
            write.serial = serial_;  // a target drawn to again and again stays one record
            return;
        }
    }
    if (writes_.size() >= kMaxWriteRanges) {
        beginEpoch();
        return;
    }
    writes_.push_back({location, begin, end, serial_});
}

void RsxTextureCache::onFlip(std::uint64_t flip_index) {
    flip_ = flip_index;
    beginEpoch();
    // A scan of every entry is cheap next to a frame, but not worth doing for every one.
    if ((flip_index & 31) != 0) return;
    bool dropped = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (flip_ - it->second.last_flip > kIdleFlips) {
            bytes_ -= entryBytes(it->second);
            it = entries_.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    if (trimBytes(nullptr)) dropped = true;
    if (dropped) generation_++;
}

bool RsxTextureCache::trimBytes(const Entry* keep) {
    bool dropped = false;
    while (bytes_ > budget_bytes_) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (&it->second == keep) continue;
            if (oldest == entries_.end() || it->second.last_use < oldest->second.last_use) oldest = it;
        }
        if (oldest == entries_.end()) break;
        bytes_ -= entryBytes(oldest->second);
        entries_.erase(oldest);
        dropped = true;
    }
    return dropped;
}

}  // namespace tox2

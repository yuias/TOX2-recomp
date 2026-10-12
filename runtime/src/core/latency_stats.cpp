#include "core/latency_stats.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace tox2 {
namespace {

constexpr std::size_t kTopBucket = kLatencyBuckets - 1;
constexpr unsigned kOctaves = 27;                              // k = 0..26
constexpr std::uint64_t kTopBucketFloorUs = 1ull << kOctaves;  // 2^27 us and above

}  // namespace

std::size_t latencyBucketIndex(std::uint64_t us) {
    if (us == 0) return 0;
    if (us >= kTopBucketFloorUs) return kTopBucket;
    const unsigned k = static_cast<unsigned>(std::bit_width(us)) - 1;
    const auto sub = static_cast<std::size_t>(((us - (1ull << k)) << 2) >> k);
    return 1 + 4 * static_cast<std::size_t>(k) + sub;
}

std::uint64_t latencyBucketUpperUs(std::size_t index) {
    if (index == 0) return 0;
    if (index >= kTopBucket) return std::numeric_limits<std::uint64_t>::max();
    const unsigned k = static_cast<unsigned>((index - 1) / 4);
    const std::uint64_t quarters = 5 + (index - 1) % 4;  // 2^k * quarters / 4, rounded up
    return ((quarters << k) + 3) >> 2;
}

double LatencySnapshot::meanUs() const {
    return count == 0 ? 0.0 : static_cast<double>(sum_us) / static_cast<double>(count);
}

std::uint64_t LatencySnapshot::percentileUs(double q) const {
    if (count == 0) return 0;
    // The epsilon keeps q * count that is an integer in exact arithmetic (0.99 * 100) from rounding up a rank.
    const double want = std::ceil(q * static_cast<double>(count) - 1e-9);
    const std::uint64_t rank = std::clamp<std::uint64_t>(static_cast<std::uint64_t>(std::max(want, 1.0)), 1, count);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kLatencyBuckets; i++) {
        seen += buckets[i];
        if (seen >= rank) return std::min(latencyBucketUpperUs(i), max_us);
    }
    return max_us;
}

LatencySnapshot LatencySnapshot::since(const LatencySnapshot& earlier) const {
    LatencySnapshot d;
    d.count = count - earlier.count;
    d.sum_us = sum_us - earlier.sum_us;
    d.over = over - earlier.over;
    std::size_t highest = 0;
    bool any = false;
    for (std::size_t i = 0; i < kLatencyBuckets; i++) {
        d.buckets[i] = buckets[i] - earlier.buckets[i];
        if (d.buckets[i] != 0) {
            highest = i;
            any = true;
        }
    }
    if (any) d.max_us = std::min(latencyBucketUpperUs(highest), max_us);
    return d;
}

void LatencySnapshot::merge(const LatencySnapshot& other) {
    count += other.count;
    sum_us += other.sum_us;
    over += other.over;
    max_us = std::max(max_us, other.max_us);
    for (std::size_t i = 0; i < kLatencyBuckets; i++) buckets[i] += other.buckets[i];
}

void LatencyStats::record(std::uint64_t us) {
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    buckets_[latencyBucketIndex(us)].fetch_add(1, std::memory_order_relaxed);
    if (us > over_us_) over_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t seen = max_us_.load(std::memory_order_relaxed);
    while (us > seen && !max_us_.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
}

LatencySnapshot LatencyStats::snapshot() const {
    LatencySnapshot s;
    s.count = count_.load(std::memory_order_relaxed);
    s.sum_us = sum_us_.load(std::memory_order_relaxed);
    s.max_us = max_us_.load(std::memory_order_relaxed);
    s.over = over_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kLatencyBuckets; i++) s.buckets[i] = buckets_[i].load(std::memory_order_relaxed);
    return s;
}

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    if (to <= from) return 0;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
}

std::string formatLatency(const LatencySnapshot& s, std::uint64_t over_us) {
    return std::format("n {} avg {:.2f} p99 {:.2f} max {:.2f} ms >{:.2f}ms {}", s.count, s.meanUs() / 1000.0,
                       static_cast<double>(s.percentileUs(0.99)) / 1000.0, static_cast<double>(s.max_us) / 1000.0,
                       static_cast<double>(over_us) / 1000.0, s.over);
}

}  // namespace tox2

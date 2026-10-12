#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tox2 {

// Durations in microseconds, recorded from any thread without a lock. Buckets: one for 0 us, four per octave
// [2^k, 2^(k+1)) for k = 0..26 (upper bounds 2^k * {1.25, 1.5, 1.75, 2}), one for 2^27 us and above; a quantile is
// reported as the upper bound of its bucket (the top bucket reports max_us), so it reads at most 25 % high.
inline constexpr std::size_t kLatencyBuckets = 110;

struct LatencySnapshot {
    std::uint64_t count = 0;
    std::uint64_t sum_us = 0;
    std::uint64_t max_us = 0;
    std::uint64_t over = 0;  // records above the threshold given to LatencyStats
    std::array<std::uint64_t, kLatencyBuckets> buckets{};

    double meanUs() const;  // 0 when empty
    // q in (0, 1]; 0 when empty. Never above max_us.
    std::uint64_t percentileUs(double q) const;
    // Counts, sums and buckets of `this` minus `earlier`; max_us becomes the upper bound of the highest non-empty
    // bucket of the difference, capped at this->max_us (the exact interval maximum is not kept).
    LatencySnapshot since(const LatencySnapshot& earlier) const;
    void merge(const LatencySnapshot& other);  // sums; max of maxima
};

class LatencyStats {
public:
    explicit LatencyStats(std::uint64_t over_us) : over_us_(over_us) {}
    LatencyStats(const LatencyStats&) = delete;
    LatencyStats& operator=(const LatencyStats&) = delete;

    void record(std::uint64_t us);  // relaxed fetch_add; max by compare-exchange loop
    LatencySnapshot snapshot() const;
    std::uint64_t overUs() const { return over_us_; }

private:
    const std::uint64_t over_us_;
    std::atomic<std::uint64_t> count_{0}, sum_us_{0}, max_us_{0}, over_{0};
    std::array<std::atomic<std::uint64_t>, kLatencyBuckets> buckets_{};
};

// Index of the bucket a duration falls into, and the largest duration the bucket reports (rounded up to a whole
// microsecond; the top bucket reports UINT64_MAX since only max_us bounds it).
std::size_t latencyBucketIndex(std::uint64_t us);
std::uint64_t latencyBucketUpperUs(std::size_t index);

std::uint64_t elapsedUs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to);
// "n 1234 avg 0.21 p99 0.80 max 3.10 ms >1.00ms 12" (milliseconds with two decimals; the threshold from over_us).
std::string formatLatency(const LatencySnapshot& s, std::uint64_t over_us);

}  // namespace tox2

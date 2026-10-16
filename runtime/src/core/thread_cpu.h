#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tox2 {

// Cycles per millisecond of QueryThreadCycleTime, calibrated against steady_clock on first use (function-local static,
// so callers during static initialisation are safe). The unit of the cycle count is not documented as time.
double hostCyclesPerMs();

// Cycles the calling thread has run so far (QueryThreadCycleTime): CPU time at the time-stamp counter rate; time the
// thread is not scheduled does not count.
std::uint64_t currentThreadCycles();

struct CycleStats {
    std::size_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t median = 0;  // element count / 2 of the sorted samples (the upper median for even counts)
    std::uint64_t max = 0;
};
// All fields 0 for an empty span.
CycleStats summarizeCycles(std::span<const std::uint64_t> samples);

// Registers the calling host thread for the perf `cpu` line under `label` (no spaces: "rsx", "audio",
// "ppu.01000006", "spu.06000200"). The thread handle stays open for the life of the process, so a thread that exits
// keeps its final time. A second call from the same thread is ignored. Logs "tox2: thread cpu: <label> host thread
// <tid>" at debug level so a sampler can map host thread ids to labels.
void registerHostThreadCpu(std::string label);

struct HostThreadCpu {
    std::string label;
    std::uint64_t cpu_us = 0;  // CPU time since the thread started
};
// Every registered thread, in registration order (append-only, so an earlier snapshot is a prefix by index).
std::vector<HostThreadCpu> hostThreadCpuSnapshot();
// User + kernel time of the whole process in microseconds, from cycle counts (QueryProcessCycleTime) so that it has the
// resolution of the per-thread figures.
std::uint64_t processCpuUs();

inline constexpr std::uint64_t kCpuLineMinUs = 5000;  // threads below 5 ms in the interval are folded into `other`
inline constexpr std::size_t kCpuLineMaxEntries = 12;
// "cpu total 812.3 ms: rsx=262.1 ppu.01000006=150.2 other=400.0" from two snapshots: entries with a delta of at least
// kCpuLineMinUs, by delta descending (ties in registration order), at most kCpuLineMaxEntries; a thread missing from
// `before` counts from 0; other = process delta - sum of the listed deltas, clamped at 0. One decimal, milliseconds.
// Empty when the process delta is 0.
std::string formatHostThreadCpu(std::span<const HostThreadCpu> before, std::span<const HostThreadCpu> after,
                                std::uint64_t process_before_us, std::uint64_t process_after_us);

}  // namespace tox2

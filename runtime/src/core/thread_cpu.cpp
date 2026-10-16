#include "core/thread_cpu.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tox2 {
namespace {

struct RegisteredThread {
    std::string label;
    HANDLE handle = nullptr;
};

struct Registry {
    std::mutex mutex;
    std::vector<RegisteredThread> threads;
};

// Never destroyed: threads may still register while the process shuts down, and the handles must outlive them.
Registry& registry() {
    static Registry* const instance = new Registry;
    return *instance;
}

thread_local bool t_registered = false;

std::uint64_t currentThreadCycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

// The thread spins for a millisecond a few times and the highest cycles/ms ratio wins, since a spin the host preempted
// counts fewer cycles than wall time.
double calibrateCyclesPerMs() {
    constexpr int kSpins = 5;
    constexpr auto kSpinTime = std::chrono::milliseconds(1);
    double best = 0.0;
    for (int i = 0; i < kSpins; i++) {
        const std::uint64_t cycles_start = currentThreadCycles();
        const auto start = std::chrono::steady_clock::now();
        auto end = start;
        while (end - start < kSpinTime) end = std::chrono::steady_clock::now();
        const std::uint64_t cycles = currentThreadCycles() - cycles_start;
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        best = std::max(best, static_cast<double>(cycles) / ms);
    }
    return best;
}

}  // namespace

double hostCyclesPerMs() {
    static const double cycles_per_ms = calibrateCyclesPerMs();
    return cycles_per_ms;
}

void registerHostThreadCpu(std::string label) {
    if (t_registered) return;
    const DWORD tid = GetCurrentThreadId();
    const HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (handle == nullptr) return;
    t_registered = true;
    {
        Registry& reg = registry();
        std::lock_guard lock(reg.mutex);
        reg.threads.push_back({label, handle});
    }
    logDebug("tox2: thread cpu: {} host thread {}", label, static_cast<unsigned long>(tid));
}

std::vector<HostThreadCpu> hostThreadCpuSnapshot() {
    const double cycles_per_ms = hostCyclesPerMs();
    Registry& reg = registry();
    std::lock_guard lock(reg.mutex);
    std::vector<HostThreadCpu> out;
    out.reserve(reg.threads.size());
    for (const RegisteredThread& thread : reg.threads) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread.handle, &cycles);
        const double cpu_us = cycles_per_ms > 0.0 ? static_cast<double>(cycles) / cycles_per_ms * 1000.0 : 0.0;
        out.push_back({thread.label, static_cast<std::uint64_t>(cpu_us)});
    }
    return out;
}

std::uint64_t processCpuUs() {
    // Cycle counts, like the per-thread figures, so the two are comparable within one second. GetProcessTimes only
    // advances in scheduler ticks (about 15.6 ms), which makes a one-second delta too coarse and too low.
    const double cycles_per_ms = hostCyclesPerMs();
    ULONG64 cycles = 0;
    if (cycles_per_ms <= 0.0 || !QueryProcessCycleTime(GetCurrentProcess(), &cycles)) return 0;
    return static_cast<std::uint64_t>(static_cast<double>(cycles) / cycles_per_ms * 1000.0);
}

std::string formatHostThreadCpu(std::span<const HostThreadCpu> before, std::span<const HostThreadCpu> after,
                                std::uint64_t process_before_us, std::uint64_t process_after_us) {
    if (process_after_us <= process_before_us) return {};
    const std::uint64_t process_delta = process_after_us - process_before_us;

    struct Entry {
        const std::string* label;
        std::uint64_t delta_us;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < after.size(); i++) {
        const std::uint64_t base = i < before.size() ? before[i].cpu_us : 0;
        const std::uint64_t delta = after[i].cpu_us > base ? after[i].cpu_us - base : 0;
        if (delta >= kCpuLineMinUs) entries.push_back({&after[i].label, delta});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.delta_us > b.delta_us; });
    if (entries.size() > kCpuLineMaxEntries) entries.resize(kCpuLineMaxEntries);

    std::uint64_t listed = 0;
    std::string line = std::format("cpu total {:.1f} ms:", static_cast<double>(process_delta) / 1000.0);
    for (const Entry& entry : entries) {
        listed += entry.delta_us;
        line += std::format(" {}={:.1f}", *entry.label, static_cast<double>(entry.delta_us) / 1000.0);
    }
    const std::uint64_t other = process_delta > listed ? process_delta - listed : 0;
    line += std::format(" other={:.1f}", static_cast<double>(other) / 1000.0);
    return line;
}

}  // namespace tox2

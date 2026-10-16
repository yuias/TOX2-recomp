// Guest threads (one host thread each), the guest lock, PPU slots, blocking waits and the supervisor.

#include "core/threads.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/sleeper.h"
#include "core/thread_cpu.h"
#include "tox2/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <process.h>

namespace tox2 {
namespace {

// Guest call depth maps to host frames, so guest threads need far more than the default stack.
// Only reserved: pages are committed as they are touched.
constexpr unsigned kHostStackBytes = 16u << 20;

// Initial r1 offset below the stack top (RPCS3 `ppu_stack_start_offset`).
constexpr std::uint32_t kStackStartOffset = 0x70;

constexpr std::int64_t kStallMs = 2000;
constexpr std::chrono::milliseconds kSupervisorPeriod{100};

thread_local GuestThread* t_current = nullptr;
constexpr std::uint8_t kNoExplicitClass = 0xFF;
thread_local std::uint8_t t_lock_class = kNoExplicitClass;
thread_local std::optional<std::int32_t> t_test_ppu_priority;
// Numbers the host threads that queue for the guest lock or a PPU slot; 0 means "none yet".
thread_local std::uint64_t t_identity = 0;
std::atomic<std::uint64_t> g_next_identity{1};
// Set by GuestWaitLock::unlock() when it gave up the caller's slot, so the matching lock() takes one back.
thread_local bool t_wait_released_slot = false;

// Priority of a PPU-class waiter whose thread has none (an explicit Ppu class without a guest thread): the lowest.
constexpr std::int32_t kLowestPpuPriority = 3071;
// Yield priority of a caller without one (a host thread in tests): below every lv2 priority, so every PPU waiter is
// eligible. Not INT32_MAX, which is GuestLock's "no PPU waiter".
constexpr std::int32_t kHostYieldPriority = 3072;

// Nanoseconds from `from` to `to`; 0 if the clock reads went backwards.
std::uint64_t elapsedNs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0;
}

bool isPpuClass(GuestLockClass cls) { return cls == GuestLockClass::Ppu || cls == GuestLockClass::PpuHigh; }

KernelWaitBucket kernelWaitBucket(const char* label) {
    if (label == nullptr) return KernelWaitBucket::None;
    const std::string_view name(label);
    if (name.starts_with("sys_lwmutex_")) return KernelWaitBucket::Lwmutex;
    if (name == "sys_timer_usleep" || name == "sys_timer_sleep") return KernelWaitBucket::TimerSleep;
    if (name.starts_with("cellGcm") || name.starts_with("_cellGcm")) return KernelWaitBucket::Gcm;
    return KernelWaitBucket::Other;
}

std::atomic<SpuHealthFn> g_spu_health{nullptr};

// CPU cycles the calling thread has run. About 250 ns per call, which is a noticeable share of the many short holds of
// the RSX thread (40k+ acquisitions/s, rsx lock ms/flip +0.3), so only PPU-class holders read it, once per hold plus
// once more when the hold turns out to be long.
std::uint64_t threadCycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

// Calibrated during static initialisation (the calibration itself is shared, see hostCyclesPerMs) because a lazy
// calibration would run inside a hold.
const double g_cycles_per_ms = hostCyclesPerMs();

std::atomic<GuestLockLongHoldObserver> g_long_hold_observer{nullptr};

// Long-hold log lines are limited per wall-clock second, over all locks; holds beyond the limit are still counted.
constexpr int kMaxLongHoldLogsPerSecond = 20;
std::atomic<std::int64_t> g_long_hold_window{-1};
std::atomic<int> g_long_hold_logged{0};

bool allowLongHoldLog(std::chrono::steady_clock::time_point now) {
    const std::int64_t second = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::int64_t window = g_long_hold_window.load(std::memory_order_relaxed);
    if (window != second && g_long_hold_window.compare_exchange_strong(window, second, std::memory_order_relaxed)) {
        g_long_hold_logged.store(0, std::memory_order_relaxed);
    }
    return g_long_hold_logged.fetch_add(1, std::memory_order_relaxed) < kMaxLongHoldLogsPerSecond;
}

// State of the yield backoff: `run` counts consecutive yields that arrived within kYieldSpinGap of the previous one's
// return.
struct YieldSpin {
    unsigned run = 0;
    std::chrono::steady_clock::time_point last_return{};
};
thread_local YieldSpin t_yield_spin;
std::atomic<bool> g_yield_backoff{true};
std::atomic<std::uint64_t> g_yield_calls{0};
std::atomic<std::uint64_t> g_yield_switched{0};
std::atomic<std::uint64_t> g_yield_to_host{0};
std::atomic<std::uint64_t> g_yield_kept{0};
std::atomic<std::uint64_t> g_yield_backoffs{0};

// Counts one yield in the spin run and tells whether it is the one that sleeps (the count restarts after it, so a sleep
// happens once per kYieldSpinThreshold yields and the yields between them still hand over at full speed).
bool yieldSpinSleepDue() {
    YieldSpin& spin = t_yield_spin;
    const auto arrival = std::chrono::steady_clock::now();
    spin.run = arrival - spin.last_return <= kYieldSpinGap ? spin.run + 1 : 1;
    if (spin.run < kYieldSpinThreshold) return false;
    spin.run = 0;
    return true;
}

void sleepYieldBackoff() {
    g_yield_backoffs.fetch_add(1, std::memory_order_relaxed);
    thread_local Sleeper sleeper;
    sleeper.sleep(kYieldBackoffSleep);
}

// Lock order: guest lock, then registry mutex. The census printer runs inside fatal(), possibly
// with the guest lock held, so everything it reads here is behind this mutex or atomic.
std::mutex g_registry_mutex;
std::vector<std::unique_ptr<GuestThread>> g_threads;  // creation order, never freed
constexpr std::uint32_t kFirstThreadId = 0x01000000;

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

std::vector<GuestThread*> snapshotThreads() {
    std::lock_guard lock(g_registry_mutex);
    std::vector<GuestThread*> out;
    out.reserve(g_threads.size());
    for (const auto& t : g_threads) out.push_back(t.get());
    return out;
}

std::optional<std::uint32_t> currentThreadId() {
    if (t_current == nullptr) return std::nullopt;
    return t_current->id();
}

std::vector<ThreadCensusEntry> threadCensus() {
    std::vector<ThreadCensusEntry> out;
    for (GuestThread* t : snapshotThreads()) {
        ThreadCensusEntry entry;
        entry.id = t->id();
        entry.name = t->name();
        switch (t->state()) {
            case ThreadState::Created:
            case ThreadState::Running: entry.state = ThreadCensusEntry::State::Running; break;
            case ThreadState::Blocked: {
                entry.state = ThreadCensusEntry::State::Blocked;
                const char* what = t->blockedIn();
                entry.blocked_in = what != nullptr ? what : "?";
                if (t->service()) entry.blocked_in += " [service]";
                break;
            }
            case ThreadState::Exited: entry.state = ThreadCensusEntry::State::Exited; break;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// One header line and one line per class that took the lock; empty until the lock was used.
std::vector<std::string> guestLockCensus() {
    const GuestLockStats s = guestLock().stats();
    std::uint64_t total = 0;
    for (const GuestLockClassStats& c : s.classes) total += c.wait.count;
    if (total == 0) return {};

    const double seconds = std::max(static_cast<double>(systemTimeUs()) / 1e6, 1e-3);
    const PpuYieldStats yields = ppuYieldStats();
    std::vector<std::string> lines;
    std::string header = std::format(
            "census: guest lock {} acquisitions in {:.1f} s, hand-off {}, wakeups {} ({} spurious), "
            "yield calls {} ok {} host {} kept {} backoff {}",
            total, seconds, formatLatency(s.handoff, kGuestLockHandoffOverUs), s.wakeups, s.spurious_wakeups,
            yields.calls, yields.switched, yields.to_host, yields.kept, yields.backoffs);
    // In the split modes the hold time is kernel sections, not the right to run, so the overlap would be misleading.
    if (!splitPpuMode()) {
        header += std::format(", ppu overlap {:.1f} of {:.1f} s", static_cast<double>(s.ppu_hold_overlap_us) / 1e6,
                              static_cast<double>(s.ppu_hold_us) / 1e6);
    }
    lines.push_back(std::move(header));
    for (std::size_t i = 0; i < kGuestLockClassCount; i++) {
        const GuestLockClassStats& c = s.classes[i];
        if (c.wait.count == 0) continue;
        const double ahead_avg =
                c.contended != 0 ? static_cast<double>(c.ahead_sum) / static_cast<double>(c.contended) : 0.0;
        std::string line =
                std::format("  {}: {:.1f}/s, wait {}, ahead avg {:.1f} max {}, hold {}",
                            guestLockClassName(static_cast<GuestLockClass>(i)),
                            static_cast<double>(c.wait.count) / seconds, formatLatency(c.wait, kGuestLockWaitOverUs),
                            ahead_avg, c.ahead_max, formatLatency(c.hold, kGuestLockHoldOverUs));
        bool first = true;
        for (std::size_t holder = 0; holder < kGuestLockClassCount; holder++) {
            if (c.waited_behind_us[holder] == 0) continue;
            line += std::format("{} {} {:.1f} ms", first ? ", waited behind" : "",
                                guestLockClassName(static_cast<GuestLockClass>(holder)),
                                static_cast<double>(c.waited_behind_us[holder]) / 1000.0);
            first = false;
        }
        lines.push_back(std::move(line));
    }
    if (splitPpuMode()) {
        const PpuSlotStats slots = ppuSlots().stats();
        lines.push_back(std::format(
                "census: ppu slots {}: held {:.1f} s, parallel {:.1f} s, full with waiters {:.1f} s, "
                "wait {} high {}",
                ppuSlots().capacity(), static_cast<double>(slots.held_us) / 1e6,
                static_cast<double>(slots.parallel_us) / 1e6, static_cast<double>(slots.full_waiting_us) / 1e6,
                formatLatency(slots.wait[0], kGuestLockWaitOverUs),
                formatLatency(slots.wait[1], kGuestLockWaitOverUs)));
        const auto bucket_text = [&](KernelWaitBucket bucket) {
            const auto i = static_cast<std::size_t>(bucket);
            return std::format("{:.1f} ms {}", static_cast<double>(s.ppu_wait_by_call.wait_us[i]) / 1000.0,
                               s.ppu_wait_by_call.count[i]);
        };
        lines.push_back(std::format("census: ppu kernel wait by call: lwmutex {}, timer {}, gcm {}, other {}, none {}",
                                    bucket_text(KernelWaitBucket::Lwmutex), bucket_text(KernelWaitBucket::TimerSleep),
                                    bucket_text(KernelWaitBucket::Gcm), bucket_text(KernelWaitBucket::Other),
                                    bucket_text(KernelWaitBucket::None)));
    }
    const PpuReservationStats reservation = ppuReservationStats();
    lines.push_back(std::format("census: ppu reservations lwarx {} stwcx {} failed {}", reservation.lwarx,
                                reservation.stwcx, reservation.stwcx_failed));
    if (lwmutexWatchEnabled()) {
        const LwmutexWatchStats watch = lwmutexWatchStats();
        const auto hit = [&](LwmutexWatchKind kind) { return watch.hits[static_cast<std::size_t>(kind)]; };
        lines.push_back(std::format(
                "census: lwmutex watch live {} max {}, hits lwarx {} stwcx {} dcbz {} getllar {} putllc {} putlluc {} "
                "put {} hle-copy {} owner {} waiter {}",
                watch.live, watch.max_live, hit(LwmutexWatchKind::Lwarx), hit(LwmutexWatchKind::Stwcx),
                hit(LwmutexWatchKind::Dcbz), hit(LwmutexWatchKind::SpuGetllar), hit(LwmutexWatchKind::SpuPutllc),
                hit(LwmutexWatchKind::SpuPutlluc), hit(LwmutexWatchKind::SpuPut), hit(LwmutexWatchKind::HleCopy),
                hit(LwmutexWatchKind::OwnerValue), hit(LwmutexWatchKind::WaiterValue)));
    }
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        // FILETIME counts 100 ns units.
        const auto seconds_of = [](const FILETIME& t) {
            return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7;
        };
        lines.push_back(
                std::format("census: process cpu user {:.1f} s kernel {:.1f} s", seconds_of(user), seconds_of(kernel)));
    }
    return lines;
}

// Cheap and idempotent: repeating it re-registers the census provider after a resetCensus.
void installThreadHooks() {
    setGuestThreadIdProvider(&currentThreadId);
    setThreadCensusProvider(&threadCensus);
    setGuestLockCensusProvider(&guestLockCensus);
}

unsigned __stdcall guestThreadMain(void* arg) {
    auto* thread = static_cast<GuestThread*>(arg);
    reserveFaultStack();
    Tox2PpuContext& ctx = thread->context();
    setFatalContext(&ctx);
    t_current = thread;

    // Split modes: the right to run is a slot; the kernel lock is taken only where a handler needs it.
    if (splitPpuMode()) {
        ppuSlots().acquire();
    } else {
        guestLock().lock();
    }
    thread->setState(ThreadState::Running);

    std::uint8_t* base = process().memory.base();
    const std::uint32_t entry = tox2Read32(base, thread->entryOpd());
    Tox2PpuFunc* function = tox2Lookup(entry);
    if (function == nullptr) {
        fatal(FatalKind::UnknownTarget,
              std::format("thread {:#010x} entry {:#010x} (opd {:#010x}) has no generated function", thread->id(),
                          entry, thread->entryOpd()));
    }
    function(&ctx, base);

    // lr is 0 at entry, so a C return from the entry function ends the thread, like a call to
    // sys_ppu_thread_exit (which also gives the TLS block back).
    logInfo("tox2: thread {:#010x} {} returned from its entry function with {:#x}", thread->id(), thread->name(),
            ctx.gpr[3]);
    // The TLS block allocator is lv2 state: open the kernel section before the free. exitCurrentThread releases it.
    if (splitPpuMode() && !guestLock().heldByCurrentThread()) guestLock().lock();
    freeTls(static_cast<std::uint32_t>(ctx.gpr[13]));
    exitCurrentThread(ctx.gpr[3]);
}

}  // namespace

void setGuestLockClass(GuestLockClass cls) { t_lock_class = static_cast<std::uint8_t>(cls); }

std::uint64_t currentThreadIdentity() {
    if (t_identity == 0) t_identity = g_next_identity.fetch_add(1, std::memory_order_relaxed);
    return t_identity;
}

void setPpuThreadsMode(PpuThreadsMode mode) {
    ppuSlots().setCapacity(mode == PpuThreadsMode::Split2 ? 2 : 1);
    // Alternation emulated the second hardware thread on one lock; with real slots it would only reorder the kernel
    // lock.
    guestLock().setAlternation(mode == PpuThreadsMode::Legacy);
    detail::g_ppu_threads_mode.store(mode, std::memory_order_relaxed);
}

void setGuestLockPpuPriorityForTesting(std::optional<std::int32_t> priority) { t_test_ppu_priority = priority; }

void setGuestLockLongHoldObserverForTesting(GuestLockLongHoldObserver observer) {
    g_long_hold_observer.store(observer, std::memory_order_relaxed);
}

std::optional<std::int32_t> currentPpuPriority() {
    if (t_test_ppu_priority.has_value()) return t_test_ppu_priority;
    if (t_lock_class != kNoExplicitClass || t_current == nullptr) return std::nullopt;
    return t_current->priority();
}

GuestLockClass currentGuestLockClass() {
    if (t_test_ppu_priority.has_value()) {
        return *t_test_ppu_priority < kPpuHighPriorityBelow ? GuestLockClass::PpuHigh : GuestLockClass::Ppu;
    }
    if (t_lock_class != kNoExplicitClass) return static_cast<GuestLockClass>(t_lock_class);
    if (t_current == nullptr) return GuestLockClass::Other;
    return t_current->priority() < kPpuHighPriorityBelow ? GuestLockClass::PpuHigh : GuestLockClass::Ppu;
}

std::string_view guestLockClassName(GuestLockClass cls) {
    switch (cls) {
        case GuestLockClass::Ppu: return "ppu";
        case GuestLockClass::PpuHigh: return "ppu-high";
        case GuestLockClass::Spu: return "spu";
        case GuestLockClass::Audio: return "audio";
        case GuestLockClass::Rsx: return "rsx";
        case GuestLockClass::Other: break;
    }
    return "other";
}

struct GuestLock::Waiter {
    Waiter* next = nullptr;      // list in arrival order
    std::uint64_t seq = 0;       // arrival order across all classes; for PPU waiters a "slot"
    std::uint64_t ppu_tag = 0;   // PPU: lv2 order tag, the tie-break within a priority
    std::uint64_t identity = 0;  // host thread identity
    std::chrono::steady_clock::time_point arrival;
    std::int32_t priority = 0;  // PPU: lv2 priority captured at queue time
    GuestLockClass cls = GuestLockClass::Other;
    bool ppu = false;
    bool resume = false;   // queued by yieldToHostWaiters: its thread held the lock a moment ago and gets it back
    bool granted = false;  // guarded by mutex_
    // Only this waiter waits on it. unlock() notifies it under mutex_: the node is on the waiter's stack, so a waiter
    // that wakes spuriously and sees `granted` must not return, and destroy the node, before the notify.
    std::condition_variable cv;
};

void GuestLock::lock() {
    const GuestLockClass cls = currentGuestLockClass();
    ClassCounters& counters = classes_[static_cast<std::size_t>(cls)];
    // The one clock read of an uncontended acquisition: it is both the entry time and the start of the hold.
    const auto arrival = std::chrono::steady_clock::now();
    const bool ppu = isPpuClass(cls);
    std::unique_lock lock(mutex_);
    if (!held_) {
        held_ = true;
        // Written only under mutex_, so a plain load + store replaces the locked read-modify-write.
        outstanding_.store(outstanding_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        hold_start_ = arrival;
        holder_class_ = cls;
        t_activity.hold_last = nullptr;
        t_activity.hold_prev = nullptr;
        holder_identity_.store(currentThreadIdentity(), std::memory_order_relaxed);
        if (ppu) {
            holder_ppu_tag_ = next_ppu_tag_++;
            last_ppu_identity_ = currentThreadIdentity();
        }
        lock.unlock();
        // After mutex_ is released to keep its critical section short; only the holder reads it.
        if (ppu) hold_start_cycles_ = threadCycles();
        counters.wait.record(0);
        return;
    }

    Waiter node;
    node.identity = currentThreadIdentity();
    node.arrival = arrival;
    node.cls = cls;
    node.ppu = ppu;
    if (ppu) {
        node.priority = currentPpuPriority().value_or(kLowestPpuPriority);
        node.ppu_tag = next_ppu_tag_++;
    }
    enqueueAndWait(lock, node);
}

void GuestLock::enqueueAndWait(std::unique_lock<std::mutex>& lock, Waiter& node) {
    ClassCounters& counters = classes_[static_cast<std::size_t>(node.cls)];
    const std::uint64_t outstanding = outstanding_.load(std::memory_order_relaxed);
    node.seq = next_seq_++;
    if (tail_ != nullptr) {
        tail_->next = &node;
    } else {
        head_ = &node;
    }
    tail_ = &node;
    if (node.ppu) {
        if (node.priority < best_ppu_priority_.load(std::memory_order_relaxed)) {
            best_ppu_priority_.store(node.priority, std::memory_order_relaxed);
        }
    } else {
        host_waiters_.store(host_waiters_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    // Counted only once the node is linked, so hasWaiters() implies a place in the queue.
    outstanding_.store(outstanding + 1, std::memory_order_relaxed);

    const std::uint64_t ahead = outstanding;
    const std::array<std::uint64_t, kGuestLockClassCount> hold_sum_at_queue = hold_sum_us_;
    // Only the chosen waiter is woken, so a wake-up that finds the grant missing is a wasted one.
    std::uint64_t wakeups = 0;
    do {
        node.cv.wait(lock);
        wakeups++;
    } while (!node.granted);
    const auto released_at = released_at_;
    const std::array<std::uint64_t, kGuestLockClassCount> hold_sum_at_turn = hold_sum_us_;
    // The clock is read after the mutex is released. The holder fields need no mutex, since only this thread reads
    // them until its unlock() passes the lock on.
    lock.unlock();
    const auto turn = std::chrono::steady_clock::now();
    hold_start_ = turn;
    holder_identity_.store(node.identity, std::memory_order_relaxed);
    holder_class_ = node.cls;
    holder_ppu_tag_ = node.ppu_tag;
    if (node.ppu) hold_start_cycles_ = threadCycles();
    t_activity.hold_last = nullptr;
    t_activity.hold_prev = nullptr;
    const std::uint64_t handoff_us = elapsedUs(released_at, turn);

    const std::uint64_t wait_us = elapsedUs(node.arrival, turn);
    counters.wait.record(wait_us);
    // The waiter's thread-local label still names the call that queued it, also after a blockingWait re-acquisition.
    if (node.ppu && splitPpuMode()) {
        const auto bucket = static_cast<std::size_t>(kernelWaitBucket(t_activity.current));
        ppu_wait_by_call_us_[bucket].fetch_add(wait_us, std::memory_order_relaxed);
        ppu_wait_by_call_count_[bucket].fetch_add(1, std::memory_order_relaxed);
    }
    counters.contended.fetch_add(1, std::memory_order_relaxed);
    counters.ahead_sum.fetch_add(ahead, std::memory_order_relaxed);
    std::uint64_t seen = counters.ahead_max.load(std::memory_order_relaxed);
    while (ahead > seen && !counters.ahead_max.compare_exchange_weak(seen, ahead, std::memory_order_relaxed)) {
    }
    for (std::size_t i = 0; i < kGuestLockClassCount; i++) {
        const std::uint64_t behind = hold_sum_at_turn[i] - hold_sum_at_queue[i];
        if (behind != 0) counters.waited_behind_us[i].fetch_add(behind, std::memory_order_relaxed);
    }
    handoff_.record(handoff_us);
    wakeups_.fetch_add(wakeups, std::memory_order_relaxed);
    if (wakeups > 1) spurious_wakeups_.fetch_add(wakeups - 1, std::memory_order_relaxed);
}

GuestLock::Waiter* GuestLock::chooseNext() {
    // Best is the PPU waiter with the lowest priority number, then the lowest tag; `second` is the next one.
    const auto ranks_before = [](const Waiter* a, const Waiter* b) {
        return a->priority != b->priority ? a->priority < b->priority : a->ppu_tag < b->ppu_tag;
    };
    Waiter *oldest_host = nullptr, *oldest_ppu = nullptr, *best = nullptr, *second = nullptr;
    for (Waiter* w = head_; w != nullptr; w = w->next) {
        if (!w->ppu) {
            if (oldest_host == nullptr) oldest_host = w;  // host entries are in seq order in the list
            continue;
        }
        if (oldest_ppu == nullptr || w->seq < oldest_ppu->seq) oldest_ppu = w;
        if (best == nullptr || ranks_before(w, best)) {
            second = best;
            best = w;
        } else if (second == nullptr || ranks_before(w, second)) {
            second = w;
        }
    }
    // Host classes keep their arrival position against the PPU slots.
    if (oldest_host != nullptr && (oldest_ppu == nullptr || oldest_host->seq < oldest_ppu->seq)) return oldest_host;
    if (best == nullptr) return nullptr;
    // lv2's two hardware threads are time-multiplexed on the one lock: the PPU thread that ran last lets the second
    // best go first, so the top two ready threads alternate and a third-ranked one waits. A resume node belongs to a
    // cancelled yield and gets its turn back.
    Waiter* pick = best;
    if (alternation_.load(std::memory_order_relaxed) && best->identity == last_ppu_identity_ && !best->resume &&
        second != nullptr) {
        pick = second;
    }
    // The pick consumes the oldest PPU slot; the waiter it overtook takes over the pick's place in arrival order, so
    // host waiters keep their position.
    if (pick != oldest_ppu) oldest_ppu->seq = pick->seq;
    last_ppu_identity_ = pick->identity;
    return pick;
}

void GuestLock::unlink(Waiter* waiter) {
    Waiter* prev = nullptr;
    for (Waiter* w = head_; w != waiter; w = w->next) prev = w;
    (prev != nullptr ? prev->next : head_) = waiter->next;
    if (tail_ == waiter) tail_ = prev;
    waiter->next = nullptr;
}

void GuestLock::refreshMirrors() {
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    std::uint32_t hosts = 0;
    for (const Waiter* w = head_; w != nullptr; w = w->next) {
        if (w->ppu) {
            best = std::min(best, w->priority);
        } else {
            hosts++;
        }
    }
    best_ppu_priority_.store(best, std::memory_order_relaxed);
    host_waiters_.store(hosts, std::memory_order_relaxed);
}

void GuestLock::noteRelease(std::chrono::steady_clock::time_point now, GuestLockClass holder, std::uint64_t hold_us) {
    hold_sum_us_[static_cast<std::size_t>(holder)] += hold_us;
    released_at_ = now;
    if (isPpuClass(holder)) {
        ppu_hold_us_.store(ppu_hold_us_.load(std::memory_order_relaxed) + hold_us, std::memory_order_relaxed);
        const Waiter* earliest = nullptr;
        for (const Waiter* w = head_; w != nullptr; w = w->next) {
            if (w->ppu && (earliest == nullptr || w->arrival < earliest->arrival)) earliest = w;
        }
        if (earliest != nullptr) {
            const std::uint64_t overlap = elapsedUs(std::max(hold_start_, earliest->arrival), now);
            ppu_hold_overlap_us_.store(ppu_hold_overlap_us_.load(std::memory_order_relaxed) + overlap,
                                       std::memory_order_relaxed);
        }
    }
    if (hold_us >= kGuestLockLongHoldUs) {
        long_holds_.store(long_holds_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    releases_.store(releases_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    outstanding_.store(outstanding_.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
}

double GuestLock::cyclesToMs(std::uint64_t cycles) { return static_cast<double>(cycles) / g_cycles_per_ms; }

GuestLock::LongHoldReport GuestLock::captureLongHold(std::uint64_t hold_us, GuestLockClass holder,
                                                     std::uint64_t start_cycles) {
    LongHoldReport report;
    if (hold_us < kGuestLockLongHoldUs) return report;
    report.hold_us = hold_us;
    report.holder = holder;
    report.thread = t_current;
    report.at_release = t_activity.current;
    report.last_in_hold = t_activity.hold_last;
    report.previous_in_hold = t_activity.hold_prev;
    report.has_cpu = isPpuClass(holder);
    if (report.has_cpu) report.cpu_cycles = threadCycles() - start_cycles;
    return report;
}

void GuestLock::emitLongHold(const LongHoldReport& report, std::chrono::steady_clock::time_point now) {
    if (report.hold_us == 0) return;
    if (const GuestLockLongHoldObserver observer = g_long_hold_observer.load(std::memory_order_relaxed)) {
        observer(report);
        return;
    }
    if (!allowLongHoldLog(now)) return;
    const char* at_release = report.at_release != nullptr ? report.at_release : "none";
    const char* last_in_hold = report.last_in_hold != nullptr ? report.last_in_hold : "none";
    const char* previous_in_hold = report.previous_in_hold != nullptr ? report.previous_in_hold : "none";
    const double ms = static_cast<double>(report.hold_us) / 1000.0;
    std::string cpu = "n/a";
    if (report.has_cpu) {
        cpu = std::format("{:.2f} ms ({:.1f} Mcycles)", cyclesToMs(report.cpu_cycles),
                          static_cast<double>(report.cpu_cycles) / 1e6);
    }
    if (report.thread != nullptr) {
        logWarn("tox2: guest lock long hold: {:.2f} ms holder {} thread {:#010x} prio {} ({}) at release: {} last in "
                "hold: {} previous in hold: {} cpu {}",
                ms, guestLockClassName(report.holder), report.thread->id(), report.thread->priority(),
                report.thread->name(), at_release, last_in_hold, previous_in_hold, cpu);
    } else {
        logWarn("tox2: guest lock long hold: {:.2f} ms holder {} at release: {} last in hold: {} previous in hold: {} "
                "cpu {}",
                ms, guestLockClassName(report.holder), at_release, last_in_hold, previous_in_hold, cpu);
    }
}

void GuestLock::grant(Waiter* next) {
    unlink(next);
    next->granted = true;
    refreshMirrors();
    next->cv.notify_one();
}

void GuestLock::unlock() {
    const auto now = std::chrono::steady_clock::now();
    // Only the holder reads these, and the next holder writes them after this call passes the lock on.
    const GuestLockClass holder = holder_class_;
    const std::uint64_t hold_us = elapsedUs(hold_start_, now);
    // Recorded before the release: a lock that lives on a test's stack must not be touched after a waiter
    // may have taken it and let the owner destroy it.
    classes_[static_cast<std::size_t>(holder)].hold.record(hold_us);
    const LongHoldReport report = captureLongHold(hold_us, holder, hold_start_cycles_);
    holder_identity_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex_);
        noteRelease(now, holder, hold_us);
        Waiter* next = chooseNext();
        if (next == nullptr) {
            held_ = false;
        } else {
            grant(next);
        }
    }
    // After the release: formatting and writing the line must not extend the hold of the next thread. Uses no member,
    // because a lock on a test's stack may be gone by now.
    emitLongHold(report, now);
}

bool GuestLock::yieldToHostWaiters() {
    const GuestLockClass cls = currentGuestLockClass();
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    if (host_waiters_.load(std::memory_order_relaxed) == 0) return false;
    const GuestLockClass holder = holder_class_;
    const std::uint64_t hold_us = elapsedUs(hold_start_, now);
    classes_[static_cast<std::size_t>(holder)].hold.record(hold_us);
    const LongHoldReport report = captureLongHold(hold_us, holder, hold_start_cycles_);
    noteRelease(now, holder, hold_us);
    holder_identity_.store(0, std::memory_order_relaxed);
    Waiter* oldest_host = head_;
    while (oldest_host->ppu) oldest_host = oldest_host->next;
    grant(oldest_host);

    Waiter node;
    node.identity = currentThreadIdentity();
    node.arrival = now;
    node.cls = cls;
    node.ppu = isPpuClass(cls);
    node.resume = true;
    if (node.ppu) {
        node.priority = currentPpuPriority().value_or(kLowestPpuPriority);
        node.ppu_tag = holder_ppu_tag_;  // a yield without a switch does not rotate within the priority
    }
    enqueueAndWait(lock, node);
    // The report is written only now: the wait above is where mutex_ is released, and the caller holds the lock again.
    emitLongHold(report, now);
    return true;
}

bool GuestLock::heldByCurrentThread() const {
    return holder_identity_.load(std::memory_order_relaxed) == currentThreadIdentity();
}

GuestLockStats GuestLock::stats() const {
    GuestLockStats out;
    for (std::size_t i = 0; i < kGuestLockClassCount; i++) {
        const ClassCounters& in = classes_[i];
        GuestLockClassStats& c = out.classes[i];
        c.wait = in.wait.snapshot();
        c.hold = in.hold.snapshot();
        c.contended = in.contended.load(std::memory_order_relaxed);
        c.ahead_sum = in.ahead_sum.load(std::memory_order_relaxed);
        c.ahead_max = in.ahead_max.load(std::memory_order_relaxed);
        for (std::size_t h = 0; h < kGuestLockClassCount; h++) {
            c.waited_behind_us[h] = in.waited_behind_us[h].load(std::memory_order_relaxed);
        }
    }
    out.handoff = handoff_.snapshot();
    out.wakeups = wakeups_.load(std::memory_order_relaxed);
    out.spurious_wakeups = spurious_wakeups_.load(std::memory_order_relaxed);
    out.ppu_hold_us = ppu_hold_us_.load(std::memory_order_relaxed);
    out.ppu_hold_overlap_us = ppu_hold_overlap_us_.load(std::memory_order_relaxed);
    out.long_holds = long_holds_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < kKernelWaitBucketCount; i++) {
        out.ppu_wait_by_call.wait_us[i] = ppu_wait_by_call_us_[i].load(std::memory_order_relaxed);
        out.ppu_wait_by_call.count[i] = ppu_wait_by_call_count_[i].load(std::memory_order_relaxed);
    }
    return out;
}

GuestLock& guestLock() {
    // Leaked: host threads may still be queued on it while the process exits.
    static GuestLock* lock = new GuestLock;
    return *lock;
}

struct PpuSlots::Waiter {
    Waiter* next = nullptr;     // list in arrival order
    std::int32_t priority = 0;  // lv2 priority captured at queue time
    std::uint64_t tag = 0;      // lv2 order within a priority
    std::uint64_t identity = 0;
    std::size_t wait_class = 0;  // index into wait_
    std::chrono::steady_clock::time_point arrival;
    std::chrono::steady_clock::time_point granted_at;  // written by the releaser
    bool granted = false;                              // guarded by mutex_
    // Only this waiter waits on it. releaseLocked() notifies it under mutex_: the node is on the waiter's stack, so a
    // waiter that wakes spuriously and sees `granted` must not return, and destroy the node, before the notify.
    std::condition_variable cv;
};

PpuSlots::PpuSlots(unsigned capacity) : capacity_(capacity) {
    if (capacity < 1 || capacity > kMaxPpuSlots) fatal(FatalKind::Internal, "PpuSlots: capacity out of range");
}

void PpuSlots::setCapacity(unsigned capacity) {
    if (capacity < 1 || capacity > kMaxPpuSlots) fatal(FatalKind::Internal, "PpuSlots: capacity out of range");
    std::lock_guard lock(mutex_);
    if (busy_ != 0 || head_ != nullptr) fatal(FatalKind::Internal, "PpuSlots::setCapacity: not idle");
    capacity_.store(capacity, std::memory_order_relaxed);
}

void PpuSlots::acquire() {
    std::unique_lock lock(mutex_);
    takeOrWait(lock, std::chrono::steady_clock::now());
}

void PpuSlots::release() {
    std::lock_guard lock(mutex_);
    releaseLocked(std::chrono::steady_clock::now());
}

bool PpuSlots::yield() {
    // The common case, one atomic load. Entries leave the queue only when a holder releases, so the caller cannot lose
    // a waiter it has seen here to anyone but another holder, which the check below under the mutex covers.
    if (waiters_.load(std::memory_order_relaxed) == 0) return false;
    std::unique_lock lock(mutex_);
    if (head_ == nullptr) return false;
    const auto now = std::chrono::steady_clock::now();
    releaseLocked(now);
    // The slot went to the chosen waiter, so every slot is held again and the caller queues behind it.
    takeOrWait(lock, now);
    return true;
}

bool PpuSlots::heldByCurrentThread() const {
    const std::uint64_t identity = currentThreadIdentity();
    for (const Holder& holder : holders_) {
        if (holder.identity.load(std::memory_order_relaxed) == identity) return true;
    }
    return false;
}

void PpuSlots::takeOrWait(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point now) {
    const std::size_t wait_class = currentGuestLockClass() == GuestLockClass::PpuHigh ? 1 : 0;
    const std::uint64_t identity = currentThreadIdentity();
    advanceTime(now);
    if (busy_ < capacity_.load(std::memory_order_relaxed)) {
        for (Holder& holder : holders_) {
            if (holder.identity.load(std::memory_order_relaxed) != 0) continue;
            holder.identity.store(identity, std::memory_order_relaxed);
            holder.start = now;
            break;
        }
        busy_++;
        acquisitions_.store(acquisitions_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        arrivals_.store(arrivals_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        lock.unlock();
        wait_[wait_class].record(0);
        return;
    }

    Waiter node;
    node.priority = currentPpuPriority().value_or(kLowestPpuPriority);
    node.tag = next_tag_++;
    node.identity = identity;
    node.wait_class = wait_class;
    node.arrival = now;
    if (tail_ != nullptr) {
        tail_->next = &node;
    } else {
        head_ = &node;
    }
    tail_ = &node;
    refreshMirrors();
    // Counted only once the node is linked, so a caller that sees the arrival can rely on the queue entry.
    arrivals_.store(arrivals_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    do {
        node.cv.wait(lock);
    } while (!node.granted);
    lock.unlock();
    wait_[wait_class].record(elapsedUs(node.arrival, node.granted_at));
    contended_.fetch_add(1, std::memory_order_relaxed);
}

void PpuSlots::releaseLocked(std::chrono::steady_clock::time_point now) {
    const std::uint64_t identity = currentThreadIdentity();
    Holder* mine = nullptr;
    for (Holder& holder : holders_) {
        if (holder.identity.load(std::memory_order_relaxed) == identity) mine = &holder;
    }
    if (mine == nullptr) fatal(FatalKind::Internal, "PpuSlots::release: the calling thread holds no slot");
    advanceTime(now);
    held_ns_.store(held_ns_.load(std::memory_order_relaxed) + elapsedNs(mine->start, now), std::memory_order_relaxed);
    releases_.store(releases_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    // The lowest priority number, then the earliest tag.
    Waiter *best = nullptr, *best_prev = nullptr, *prev = nullptr;
    for (Waiter* w = head_; w != nullptr; prev = w, w = w->next) {
        if (best == nullptr || w->priority < best->priority || (w->priority == best->priority && w->tag < best->tag)) {
            best = w;
            best_prev = prev;
        }
    }
    if (best == nullptr) {
        mine->identity.store(0, std::memory_order_relaxed);
        busy_--;
        return;
    }
    (best_prev != nullptr ? best_prev->next : head_) = best->next;
    if (tail_ == best) tail_ = best_prev;
    best->next = nullptr;
    // The slot changes hands without being free in between, so busy_ stays and no one can barge.
    mine->identity.store(best->identity, std::memory_order_relaxed);
    mine->start = now;
    acquisitions_.store(acquisitions_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    best->granted = true;
    best->granted_at = now;
    refreshMirrors();
    best->cv.notify_one();
}

void PpuSlots::advanceTime(std::chrono::steady_clock::time_point now) {
    const std::uint64_t dt = elapsedNs(last_change_, now);
    if (busy_ >= 2) parallel_ns_.store(parallel_ns_.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    if (busy_ == capacity_.load(std::memory_order_relaxed) && head_ != nullptr) {
        full_waiting_ns_.store(full_waiting_ns_.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
    }
    last_change_ = now;
}

void PpuSlots::refreshMirrors() {
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    std::size_t count = 0;
    for (const Waiter* w = head_; w != nullptr; w = w->next) {
        best = std::min(best, w->priority);
        count++;
    }
    best_priority_.store(best, std::memory_order_relaxed);
    waiters_.store(count, std::memory_order_relaxed);
}

PpuSlotStats PpuSlots::stats() const {
    PpuSlotStats out;
    for (std::size_t i = 0; i < wait_.size(); i++) out.wait[i] = wait_[i].snapshot();
    out.acquisitions = acquisitions_.load(std::memory_order_relaxed);
    out.contended = contended_.load(std::memory_order_relaxed);
    // Includes the interval since the last change, so a reader sees the state it is in now.
    std::lock_guard lock(mutex_);
    const std::uint64_t pending = elapsedNs(last_change_, std::chrono::steady_clock::now());
    std::uint64_t parallel_ns = parallel_ns_.load(std::memory_order_relaxed);
    std::uint64_t full_waiting_ns = full_waiting_ns_.load(std::memory_order_relaxed);
    if (busy_ >= 2) parallel_ns += pending;
    if (busy_ == capacity_.load(std::memory_order_relaxed) && head_ != nullptr) full_waiting_ns += pending;
    out.held_us = held_ns_.load(std::memory_order_relaxed) / 1000;
    out.parallel_us = parallel_ns / 1000;
    out.full_waiting_us = full_waiting_ns / 1000;
    return out;
}

PpuSlots& ppuSlots() {
    // Leaked: guest threads may still be queued on it while the process exits.
    static PpuSlots* slots = new PpuSlots(1);
    return *slots;
}

bool KernelSection::takeKernelLock() {
    GuestLock& kernel = guestLock();
    if (kernel.heldByCurrentThread()) return false;
    kernel.lock();
    return true;
}

bool KernelExit::releaseKernelLock() {
    GuestLock& kernel = guestLock();
    if (!kernel.heldByCurrentThread()) return false;
    kernel.unlock();
    return true;
}

void GuestWaitLock::lock() {
    if (!splitPpuMode()) {
        guestLock().lock();
        return;
    }
    // Slot first, then the kernel lock: the lock order, and a woken thread queues for the slot by its priority.
    if (t_wait_released_slot) {
        t_wait_released_slot = false;
        ppuSlots().acquire();
    }
    guestLock().lock();
}

void GuestWaitLock::unlock() {
    if (!splitPpuMode()) {
        guestLock().unlock();
        return;
    }
    // A blocking wait is a place where legacy would let a queued thread in.
    if (t_current != nullptr) t_current->markFirstRelease();
    guestLock().unlock();
    PpuSlots& slots = ppuSlots();
    t_wait_released_slot = slots.heldByCurrentThread();
    if (t_wait_released_slot) slots.release();
}

GuestWaitLock& guestWaitLock() {
    static GuestWaitLock lock;
    return lock;
}

void failBlockingWait(const char* what) {
    fatal(FatalKind::Internal, std::format("{}: blocking wait without the kernel lock", what));
}

GuestThread::GuestThread(std::uint32_t id, const GuestThreadParams& params, std::uint32_t stack_addr,
                         std::uint32_t stack_size)
    : id_(id),
      name_(params.name),
      priority_(params.priority),
      stack_addr_(stack_addr),
      stack_size_(stack_size),
      tls_r13_(params.tls_r13),
      entry_opd_(params.entry_opd),
      joinable_(params.joinable),
      service_(params.service) {
    context_.vscr_nj = 1;
    context_.preempt_budget = kTox2PreemptBudget;
    context_.runtime = this;
}

GuestThread* currentThread() { return t_current; }

GuestThread* findThread(std::uint32_t id) {
    std::lock_guard lock(g_registry_mutex);
    if (id < kFirstThreadId || id - kFirstThreadId >= g_threads.size()) return nullptr;
    return g_threads[id - kFirstThreadId].get();
}

GuestThread* prepareGuestThread(const GuestThreadParams& params) {
    installThreadHooks();
    Process& p = process();
    std::uint8_t* base = p.memory.base();

    // RPCS3 `sys_ppu_thread_create`: a zero size becomes one page.
    constexpr std::uint32_t kPage = 0x1000;
    const std::uint64_t wanted = params.stack_size == 0 ? kPage : params.stack_size;
    const std::uint64_t size64 = (wanted + kPage - 1) & ~std::uint64_t{kPage - 1};
    const std::uint32_t addr = size64 > 0xFFFFFFFFull ? 0 : p.stack.allocate(static_cast<std::uint32_t>(size64), kPage);
    if (addr == 0) {
        logError("no guest stack of {:#x} bytes for thread {}", params.stack_size, params.name);
        return nullptr;
    }
    const auto size = static_cast<std::uint32_t>(size64);
    // The stack is zero, as freshly committed pages are. RPCS3 (`vm.cpp` `block_t::try_alloc`) writes its
    // "STACKGRD"/"UNDERFLO" patterns into the guard pages outside the allocation, which the guest cannot read;
    // writing them inside would hand the initial frames' uninitialised locals non-zero values (a game's
    // CellSaveDataSetList with an unset reserved word then fails the parameter check).

    // Read before taking the registry mutex: a bad descriptor faults, and the fatal path's census
    // needs that mutex.
    const std::uint32_t toc = tox2Read32(base, params.entry_opd + 4);

    std::lock_guard lock(g_registry_mutex);
    const auto id = static_cast<std::uint32_t>(kFirstThreadId + g_threads.size());
    auto thread = std::make_unique<GuestThread>(id, params, addr, size);
    Tox2PpuContext& ctx = thread->context();
    ctx.gpr[1] = std::uint64_t{addr} + size - kStackStartOffset;
    ctx.gpr[2] = toc;
    ctx.gpr[3] = params.arg0;
    ctx.gpr[4] = params.arg1;
    ctx.gpr[13] = params.tls_r13;
    g_threads.push_back(std::move(thread));
    return g_threads.back().get();
}

void startGuestThread(GuestThread* thread) {
    const std::uintptr_t handle = _beginthreadex(nullptr, kHostStackBytes, &guestThreadMain, thread,
                                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0) {
        fatal(FatalKind::Internal, std::format("cannot start the host thread of guest thread {:#010x}", thread->id()));
    }
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

namespace {

bool startGuestThreadFirstSplit(GuestThread* thread, std::chrono::microseconds bound) {
    PpuSlots& slots = ppuSlots();
    const std::uint64_t arrivals_before = slots.arrivals();
    startGuestThread(thread);

    // The count grows when a thread links its slot node. It can also grow because the other slot holder yielded, so it
    // only shows that something arrived; the first-release wait below is what orders the new thread first.
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (slots.arrivals() <= arrivals_before) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    if (t_current != nullptr) t_current->markFirstRelease();
    {
        // Destroyed last: the kernel lock comes back after the slot.
        KernelExit leave_kernel;
        const bool holds_slot = slots.heldByCurrentThread();
        if (holds_slot) slots.release();
        // Not bounded by the poll: with a free second slot the new thread may run without anyone queued, and only its
        // own first release point (at most one budget away) ends the wait.
        thread->waitFirstRelease();
        if (holds_slot) slots.acquire();
    }
    return true;
}

}  // namespace

bool startGuestThreadFirst(GuestThread* thread, std::chrono::microseconds bound) {
    if (splitPpuMode()) return startGuestThreadFirstSplit(thread, bound);
    GuestLock& lock = guestLock();
    const std::size_t waiting_before = lock.waiterCount();
    startGuestThread(thread);

    // The counter grows when the new thread links its node, which needs only the lock's internal
    // mutex, so polling while holding the guest lock cannot deadlock.
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (lock.waiterCount() <= waiting_before) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    yieldGuestLock();
    return true;
}

std::uint32_t createGuestThread(const GuestThreadParams& params) {
    GuestThread* thread = prepareGuestThread(params);
    if (thread == nullptr) return 0;
    startGuestThread(thread);
    return thread->id();
}

namespace {

[[noreturn]] void exitCurrentThreadSplit(GuestThread* thread, std::uint64_t value) {
    GuestLock& kernel = guestLock();
    // The stack free, the state change and the joiner wake-up are lv2 state.
    if (!kernel.heldByCurrentThread()) kernel.lock();
    thread->setExitValue(value);
    process().stack.free(thread->stackAddr());
    thread->setState(ThreadState::Exited);
    thread->joiners().notify_all();
    setFatalContext(nullptr);
    t_current = nullptr;
    // Whoever took the kernel lock: a handler that called sys_ppu_thread_exit holds it through a KernelSection whose
    // destructor never runs, because _endthreadex does not unwind.
    if (kernel.heldByCurrentThread()) kernel.unlock();
    if (ppuSlots().heldByCurrentThread()) ppuSlots().release();
    thread->markFirstRelease();
    _endthreadex(0);
    std::_Exit(static_cast<int>(ExitCode::Fatal));
}

}  // namespace

void exitCurrentThread(std::uint64_t value) {
    GuestThread* thread = t_current;
    if (thread == nullptr) {
        fatal(FatalKind::Internal, "exitCurrentThread called from a host thread");
    }
    if (splitPpuMode()) exitCurrentThreadSplit(thread, value);
    thread->setExitValue(value);
    // The host thread no longer uses the guest stack.
    process().stack.free(thread->stackAddr());
    thread->setState(ThreadState::Exited);
    thread->joiners().notify_all();
    setFatalContext(nullptr);
    t_current = nullptr;
    guestLock().unlock();
    _endthreadex(0);
    // _endthreadex does not return; this keeps the [[noreturn]] contract visible to the compiler.
    std::_Exit(static_cast<int>(ExitCode::Fatal));
}

namespace {

bool yieldSlot() {
    if (t_current != nullptr) t_current->markFirstRelease();
    PpuSlots& slots = ppuSlots();
    if (slots.waiterCount() == 0 || !slots.heldByCurrentThread()) return false;
    // Budget expiry can happen inside a handler's kernel section (the GCM interrupt and sail service bodies): the lock
    // goes first and comes back last, so no thread waits for a slot with the kernel lock.
    KernelExit leave_kernel;
    return slots.yield();
}

}  // namespace

bool yieldGuestLock() {
    if (splitPpuMode()) return yieldSlot();
    GuestLock& lock = guestLock();
    if (!lock.hasWaiters()) return false;
    // unlock() passes the lock to a waiter and this thread queues behind the ones still waiting.
    lock.unlock();
    lock.lock();
    return true;
}

void setYieldBackoffEnabled(bool enabled) { g_yield_backoff.store(enabled, std::memory_order_relaxed); }

PpuYieldStats ppuYieldStats() {
    PpuYieldStats s;
    s.calls = g_yield_calls.load(std::memory_order_relaxed);
    s.switched = g_yield_switched.load(std::memory_order_relaxed);
    s.to_host = g_yield_to_host.load(std::memory_order_relaxed);
    s.kept = g_yield_kept.load(std::memory_order_relaxed);
    s.backoffs = g_yield_backoffs.load(std::memory_order_relaxed);
    return s;
}

std::uint64_t yieldBackoffCount() { return ppuYieldStats().backoffs; }

namespace {

bool yieldPpuThreadSplit() {
    PpuSlots& slots = ppuSlots();
    // Decided at entry, like the legacy rule. At capacity 2 it is a hint: the other holder's release can take the
    // entry.
    const bool eligible = slots.hasWaiterAtOrAbove(currentPpuPriority().value_or(kHostYieldPriority));
    if (t_current != nullptr) t_current->markFirstRelease();
    if (!slots.heldByCurrentThread()) {
        // A guest thread here would be running guest code without a slot; hiding it would also inflate `kept`.
        if (t_current != nullptr) fatal(FatalKind::Internal, "sys_ppu_thread_yield without a PPU slot");
        g_yield_kept.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Normally called without the kernel lock (sys_ppu_thread_yield is kernel-free); a caller inside a section must not
    // wait for a slot with it.
    KernelExit leave_kernel;
    bool slept = false;
    if (g_yield_backoff.load(std::memory_order_relaxed) && yieldSpinSleepDue()) {
        slots.release();
        sleepYieldBackoff();
        slots.acquire();
        slept = true;
    }
    if (!slept) {
        if (eligible) {
            // The eligible waiter is chosen at this release: it outranks or equals the caller, which is not queued yet.
            slots.release();
            slots.acquire();
        } else {
            g_yield_kept.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (eligible) g_yield_switched.fetch_add(1, std::memory_order_relaxed);
    if (g_yield_backoff.load(std::memory_order_relaxed)) t_yield_spin.last_return = std::chrono::steady_clock::now();
    return eligible;
}

}  // namespace

bool yieldPpuThread() {
    g_yield_calls.fetch_add(1, std::memory_order_relaxed);
    if (splitPpuMode()) return yieldPpuThreadSplit();
    GuestLock& lock = guestLock();
    // Decided at entry: a PPU thread that queues during the yield does not change the result.
    const bool eligible = lock.hasPpuWaiterAtOrAbove(currentPpuPriority().value_or(kHostYieldPriority));
    bool slept = false;
    if (g_yield_backoff.load(std::memory_order_relaxed) && yieldSpinSleepDue()) {
        lock.unlock();
        sleepYieldBackoff();
        lock.lock();
        slept = true;
    }
    if (!slept) {
        if (eligible) {
            // The eligible waiter is chosen at this release: it outranks or equals the caller, which is not queued yet.
            lock.unlock();
            lock.lock();
        } else if (lock.yieldToHostWaiters()) {
            g_yield_to_host.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_yield_kept.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (eligible) g_yield_switched.fetch_add(1, std::memory_order_relaxed);
    if (g_yield_backoff.load(std::memory_order_relaxed)) t_yield_spin.last_return = std::chrono::steady_clock::now();
    return eligible;
}

void blockEnter(GuestThread* thread, const char* what, bool indefinite) {
    if (thread != nullptr) thread->setBlocked(what, indefinite, steadyMs());
}

void blockLeave(GuestThread* thread) {
    if (thread != nullptr) thread->setState(ThreadState::Running);
}

void sleepGuestThread(std::chrono::microseconds duration, const char* what) {
    if (!splitPpuMode()) {
        static std::condition_variable_any never_signalled;
        blockingWait(never_signalled, [] { return false; }, what, duration);
        return;
    }
    // The sleep itself needs no lock, and holding the kernel lock for it would stall every other kernel section.
    if (guestLock().heldByCurrentThread()) {
        fatal(FatalKind::Internal, std::format("{}: sleep with the kernel lock held", what));
    }
    GuestThread* thread = t_current;
    blockEnter(thread, what, false);
    if (thread != nullptr) thread->markFirstRelease();
    // Keyed on the slot, not on the guest thread: a caller without a GuestThread can hold a slot too.
    PpuSlots& slots = ppuSlots();
    const bool held = slots.heldByCurrentThread();
    if (held) slots.release();
    std::this_thread::sleep_for(duration);
    if (held) slots.acquire();
    blockLeave(thread);
}

void runSupervisor(std::uint32_t seconds) {
    installThreadHooks();
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(kSupervisorPeriod);

        if (seconds != 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(seconds)) {
            stopProcess(ExitCode::Watchdog, std::format("tox2: stop: watchdog {} s", seconds));
        }

        std::vector<ThreadHealth> health;
        for (GuestThread* t : snapshotThreads()) {
            health.push_back({t->state(), t->indefiniteWait(), t->blockedSinceMs(), t->service()});
        }
        appendSpuHealth(health);
        std::size_t stalled = 0;
        switch (judgeThreads(health, steadyMs(), &stalled)) {
            case ThreadVerdict::NoThreads: stopProcess(ExitCode::NoThreads, "tox2: stop: no guest threads left"); break;
            case ThreadVerdict::Stall:
                stopProcess(ExitCode::Stall, std::format("tox2: stop: stall ({} threads blocked)", stalled));
                break;
            case ThreadVerdict::Ok: break;
        }
    }
}

void setSpuHealthProvider(SpuHealthFn fn) { g_spu_health.store(fn); }

void appendSpuHealth(std::vector<ThreadHealth>& out) {
    if (SpuHealthFn fn = g_spu_health.load()) fn(out);
}

ThreadVerdict judgeThreads(const std::vector<ThreadHealth>& threads, std::int64_t now_ms, std::size_t* stalled_out) {
    std::size_t live = 0, stalled = 0;
    for (const ThreadHealth& t : threads) {
        if (t.state == ThreadState::Exited || t.service) continue;
        live++;
        if (t.state == ThreadState::Blocked && t.indefinite_wait && now_ms - t.blocked_since_ms >= kStallMs) {
            stalled++;
        }
    }
    if (stalled_out != nullptr) *stalled_out = stalled;
    if (live == 0) return ThreadVerdict::NoThreads;
    return stalled == live ? ThreadVerdict::Stall : ThreadVerdict::Ok;
}

}  // namespace tox2

extern "C" {

void tox2Yield(Tox2PpuContext* ctx, uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kTox2PreemptBudget;
    tox2::yieldGuestLock();
}

}  // extern "C"

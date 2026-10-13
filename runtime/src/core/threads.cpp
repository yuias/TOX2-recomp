// Guest threads (one host thread each), the guest lock, blocking waits and the supervisor.

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
#include "core/sleeper.h"
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
std::atomic<SpuHealthFn> g_spu_health{nullptr};

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
std::atomic<std::uint64_t> g_yield_kept{0};
std::atomic<std::uint64_t> g_yield_backoffs{0};

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
    lines.push_back(std::format(
            "census: guest lock {} acquisitions in {:.1f} s, hand-off {}, wakeups {} ({} spurious), "
            "yield calls {} ok {} host {} kept {} backoff {}",
            total, seconds, formatLatency(s.handoff, kGuestLockHandoffOverUs), s.wakeups, s.spurious_wakeups,
            yields.calls, yields.switched, yields.to_host, yields.kept, yields.backoffs));
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

// RPCS3 fills the lowest and the highest page of every stack allocation with these strings (`vm.cpp`
// `block_t::try_alloc`, "Fill stack guards"). The top page holds the initial frame, so guest code
// that reads uninitialised stack sees the pattern.
void fillStackPages(std::uint8_t* base, std::uint32_t addr, std::uint32_t size) {
    constexpr std::uint32_t kPage = 0x1000;
    const auto fill = [&](std::uint32_t at, std::string_view pattern) {
        for (std::uint32_t off = 0; off < kPage; off += 8) {
            std::memcpy(base + at + off, pattern.data(), 8);
        }
    };
    fill(addr, "STACKGRD");
    fill(addr + size - kPage, "UNDERFLO");
}

unsigned __stdcall guestThreadMain(void* arg) {
    auto* thread = static_cast<GuestThread*>(arg);
    reserveFaultStack();
    Tox2PpuContext& ctx = thread->context();
    setFatalContext(&ctx);
    t_current = thread;

    GuestLock& lock = guestLock();
    lock.lock();
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
    freeTls(static_cast<std::uint32_t>(ctx.gpr[13]));
    exitCurrentThread(ctx.gpr[3]);
}

}  // namespace

void setGuestLockClass(GuestLockClass cls) { t_lock_class = static_cast<std::uint8_t>(cls); }

GuestLockClass currentGuestLockClass() {
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

void GuestLock::lock() {
    const GuestLockClass cls = currentGuestLockClass();
    ClassCounters& counters = classes_[static_cast<std::size_t>(cls)];
    // The one clock read of an uncontended acquisition: it is both the entry time and the start of the hold.
    const auto arrival = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    const std::uint64_t ticket = next_ticket_++;
    // Counted only once the ticket is taken, so hasWaiters() implies a place in the queue.
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    if (serving_ == ticket) {
        hold_start_ = arrival;
        holder_class_ = cls;
        lock.unlock();
        counters.wait.record(0);
        return;
    }

    const std::uint64_t ahead = ticket - serving_;
    const std::array<std::uint64_t, kGuestLockClassCount> hold_sum_at_ticket = hold_sum_us_;
    // Every release wakes every waiter, so a wake-up that finds another ticket served is a wasted one.
    std::uint64_t wakeups = 0;
    do {
        turn_.wait(lock);
        wakeups++;
    } while (serving_ != ticket);
    const auto released_at = released_at_;
    const std::array<std::uint64_t, kGuestLockClassCount> hold_sum_at_turn = hold_sum_us_;
    // The clock is read after the mutex is released: every waiter was just woken and wants the mutex. The holder
    // fields need no mutex, since only this thread reads them until its unlock() serves the next ticket.
    lock.unlock();
    const auto turn = std::chrono::steady_clock::now();
    hold_start_ = turn;
    holder_class_ = cls;
    const std::uint64_t handoff_us = elapsedUs(released_at, turn);

    counters.wait.record(elapsedUs(arrival, turn));
    counters.contended.fetch_add(1, std::memory_order_relaxed);
    counters.ahead_sum.fetch_add(ahead, std::memory_order_relaxed);
    std::uint64_t seen = counters.ahead_max.load(std::memory_order_relaxed);
    while (ahead > seen && !counters.ahead_max.compare_exchange_weak(seen, ahead, std::memory_order_relaxed)) {
    }
    for (std::size_t i = 0; i < kGuestLockClassCount; i++) {
        const std::uint64_t behind = hold_sum_at_turn[i] - hold_sum_at_ticket[i];
        if (behind != 0) counters.waited_behind_us[i].fetch_add(behind, std::memory_order_relaxed);
    }
    handoff_.record(handoff_us);
    wakeups_.fetch_add(wakeups, std::memory_order_relaxed);
    if (wakeups > 1) spurious_wakeups_.fetch_add(wakeups - 1, std::memory_order_relaxed);
}

void GuestLock::unlock() {
    const auto now = std::chrono::steady_clock::now();
    // Only the holder reads these, and the next holder writes them after this call serves its ticket.
    const GuestLockClass holder = holder_class_;
    const std::uint64_t hold_us = elapsedUs(hold_start_, now);
    // Recorded before the release: a lock that lives on a test's stack must not be touched after a waiter
    // may have taken it and let the owner destroy it.
    classes_[static_cast<std::size_t>(holder)].hold.record(hold_us);
    {
        std::lock_guard lock(mutex_);
        hold_sum_us_[static_cast<std::size_t>(holder)] += hold_us;
        released_at_ = now;
        serving_++;
        outstanding_.fetch_sub(1, std::memory_order_relaxed);
        releases_.fetch_add(1, std::memory_order_relaxed);
    }
    turn_.notify_all();
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
    return out;
}

GuestLock& guestLock() {
    // Leaked: host threads may still be queued on it while the process exits.
    static GuestLock* lock = new GuestLock;
    return *lock;
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
    fillStackPages(base, addr, size);

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

bool startGuestThreadFirst(GuestThread* thread, std::chrono::microseconds bound) {
    GuestLock& lock = guestLock();
    const std::size_t waiting_before = lock.waiterCount();
    startGuestThread(thread);

    // The counter grows when the new thread takes its ticket, which needs only the lock's internal
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

void exitCurrentThread(std::uint64_t value) {
    GuestThread* thread = t_current;
    if (thread == nullptr) {
        fatal(FatalKind::Internal, "exitCurrentThread called from a host thread");
    }
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

bool yieldGuestLock() {
    GuestLock& lock = guestLock();
    if (!lock.hasWaiters()) return false;
    // The ticket lock hands over in arrival order, so this thread queues behind the waiters.
    lock.unlock();
    lock.lock();
    return true;
}

void setYieldBackoffEnabled(bool enabled) { g_yield_backoff.store(enabled, std::memory_order_relaxed); }

PpuYieldStats ppuYieldStats() {
    PpuYieldStats s;
    s.calls = g_yield_calls.load(std::memory_order_relaxed);
    s.switched = g_yield_switched.load(std::memory_order_relaxed);
    s.kept = g_yield_kept.load(std::memory_order_relaxed);
    s.backoffs = g_yield_backoffs.load(std::memory_order_relaxed);
    return s;
}

std::uint64_t yieldBackoffCount() { return ppuYieldStats().backoffs; }

bool yieldGuestLockBackoff() {
    g_yield_calls.fetch_add(1, std::memory_order_relaxed);
    bool slept = false;
    bool others_ran;
    if (!g_yield_backoff.load(std::memory_order_relaxed)) {
        others_ran = yieldGuestLock();
    } else {
        YieldSpin& spin = t_yield_spin;
        const auto arrival = std::chrono::steady_clock::now();
        spin.run = arrival - spin.last_return <= kYieldSpinGap ? spin.run + 1 : 1;

        if (spin.run >= kYieldSpinThreshold) {
            // Restart the count, so a sleep happens once per kYieldSpinThreshold yields and the yields between them
            // still hand over at full speed.
            spin.run = 0;
            GuestLock& lock = guestLock();
            const std::uint64_t releases_before = lock.releaseCount();
            lock.unlock();
            g_yield_backoffs.fetch_add(1, std::memory_order_relaxed);
            thread_local Sleeper sleeper;
            sleeper.sleep(kYieldBackoffSleep);
            lock.lock();
            slept = true;
            // Our own release is one of the counted ones.
            others_ran = lock.releaseCount() - releases_before > 1;
        } else {
            others_ran = yieldGuestLock();
        }
        spin.last_return = std::chrono::steady_clock::now();
    }
    if (others_ran) {
        g_yield_switched.fetch_add(1, std::memory_order_relaxed);
    } else if (!slept) {
        g_yield_kept.fetch_add(1, std::memory_order_relaxed);
    }
    return others_ran;
}

void blockEnter(GuestThread* thread, const char* what, bool indefinite) {
    if (thread != nullptr) thread->setBlocked(what, indefinite, steadyMs());
}

void blockLeave(GuestThread* thread) {
    if (thread != nullptr) thread->setState(ThreadState::Running);
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

// Guest threads (one host thread each), the guest lock, blocking waits and the supervisor.

#include "core/threads.h"

#include <algorithm>
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
#include "dal/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <process.h>

namespace dal {
namespace {

// Guest call depth maps to host frames, so guest threads need far more than the default stack.
// Only reserved: pages are committed as they are touched.
constexpr unsigned kHostStackBytes = 16u << 20;

// Initial r1 offset below the stack top (RPCS3 `ppu_stack_start_offset`).
constexpr std::uint32_t kStackStartOffset = 0x70;

constexpr std::int64_t kStallMs = 2000;
constexpr std::chrono::milliseconds kSupervisorPeriod{100};

thread_local GuestThread* t_current = nullptr;

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

// Cheap and idempotent: repeating it re-registers the census provider after a resetCensus.
void installThreadHooks() {
    setGuestThreadIdProvider(&currentThreadId);
    setThreadCensusProvider(&threadCensus);
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
    DalPpuContext& ctx = thread->context();
    setFatalContext(&ctx);
    t_current = thread;

    GuestLock& lock = guestLock();
    lock.lock();
    thread->setState(ThreadState::Running);

    std::uint8_t* base = process().memory.base();
    const std::uint32_t entry = dalRead32(base, thread->entryOpd());
    DalPpuFunc* function = dalLookup(entry);
    if (function == nullptr) {
        fatal(FatalKind::UnknownTarget,
              std::format("thread {:#010x} entry {:#010x} (opd {:#010x}) has no generated function", thread->id(),
                          entry, thread->entryOpd()));
    }
    function(&ctx, base);

    // lr is 0 at entry, so a C return from the entry function ends the thread, like a call to
    // sys_ppu_thread_exit (which also gives the TLS block back).
    logInfo("dal: thread {:#010x} {} returned from its entry function with {:#x}", thread->id(), thread->name(),
            ctx.gpr[3]);
    freeTls(static_cast<std::uint32_t>(ctx.gpr[13]));
    exitCurrentThread(ctx.gpr[3]);
}

}  // namespace

void GuestLock::lock() {
    std::unique_lock lock(mutex_);
    const std::uint64_t ticket = next_ticket_++;
    // Counted only once the ticket is taken, so hasWaiters() implies a place in the queue.
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    turn_.wait(lock, [&] { return serving_ == ticket; });
}

void GuestLock::unlock() {
    {
        std::lock_guard lock(mutex_);
        serving_++;
        outstanding_.fetch_sub(1, std::memory_order_relaxed);
    }
    turn_.notify_all();
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
    context_.preempt_budget = kDalPreemptBudget;
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
    const std::uint32_t toc = dalRead32(base, params.entry_opd + 4);

    std::lock_guard lock(g_registry_mutex);
    const auto id = static_cast<std::uint32_t>(kFirstThreadId + g_threads.size());
    auto thread = std::make_unique<GuestThread>(id, params, addr, size);
    DalPpuContext& ctx = thread->context();
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
            stopProcess(ExitCode::Watchdog, std::format("dal: stop: watchdog {} s", seconds));
        }

        std::vector<ThreadHealth> health;
        for (GuestThread* t : snapshotThreads()) {
            health.push_back({t->state(), t->indefiniteWait(), t->blockedSinceMs(), t->service()});
        }
        std::size_t stalled = 0;
        switch (judgeThreads(health, steadyMs(), &stalled)) {
            case ThreadVerdict::NoThreads: stopProcess(ExitCode::NoThreads, "dal: stop: no guest threads left"); break;
            case ThreadVerdict::Stall:
                stopProcess(ExitCode::Stall, std::format("dal: stop: stall ({} threads blocked)", stalled));
                break;
            case ThreadVerdict::Ok: break;
        }
    }
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

}  // namespace dal

extern "C" {

void dalYield(DalPpuContext* ctx, uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kDalPreemptBudget;
    dal::yieldGuestLock();
}

}  // extern "C"

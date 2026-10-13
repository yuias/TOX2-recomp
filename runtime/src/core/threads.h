#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/latency_stats.h"
#include "tox2/ppu_context.h"

namespace tox2 {

// Who is waiting for or holding the guest lock. Guest threads are Ppu or PpuHigh by their lv2 priority at the time
// of the call; host threads name themselves once with setGuestLockClass (default Other).
enum class GuestLockClass : std::uint8_t { Ppu, PpuHigh, Spu, Audio, Rsx, Other };
inline constexpr std::size_t kGuestLockClassCount = 6;
inline constexpr std::int32_t kPpuHighPriorityBelow = 100;  // lv2 priority; the game's sound threads use 0

void setGuestLockClass(GuestLockClass cls);  // thread-local; for host threads
// The explicit class if set, else Ppu/PpuHigh for a guest thread, else Other.
GuestLockClass currentGuestLockClass();
std::string_view guestLockClassName(GuestLockClass cls);  // "ppu", "ppu-high", "spu", "audio", "rsx", "other"

// Thresholds of the `over` counts in GuestLockStats.
inline constexpr std::uint64_t kGuestLockWaitOverUs = 1000;
inline constexpr std::uint64_t kGuestLockHoldOverUs = 1000;
inline constexpr std::uint64_t kGuestLockHandoffOverUs = 100;

struct GuestLockClassStats {
    LatencySnapshot wait;                        // entry -> turn; 0 when uncontended
    LatencySnapshot hold;                        // turn -> unlock()
    std::uint64_t contended = 0;                 // acquisitions that had to queue
    std::uint64_t ahead_sum = 0, ahead_max = 0;  // waiters queued ahead at ticket time (contended acquisitions only)
    // Approximate attribution: per holder class, the hold time that ended while this class waited (from the holder
    // classes' cumulative hold sums at ticket time and at turn). Only contended acquisitions add to it.
    std::array<std::uint64_t, kGuestLockClassCount> waited_behind_us{};
};

struct GuestLockStats {
    std::array<GuestLockClassStats, kGuestLockClassCount> classes;
    LatencySnapshot handoff;                          // unlock() of the previous holder -> turn of a waiting acquirer
    std::uint64_t wakeups = 0, spurious_wakeups = 0;  // waiter wake-ups, and those that found it was not their turn
};

// The one lock all guest code runs under. A thread takes it when it starts and gives it up only
// inside tox2Yield, blockingWait and exitCurrentThread, so guest threads never run in parallel.
// Waiters are served in arrival order. Deferred: lv2 priority scheduling; runnable threads get the
// lock in FIFO order and set/get_priority only store and return the value. The one exception is the
// creation rule of sys_ppu_thread_create (startGuestThreadFirst): a new thread with a numerically
// lower priority than its creator takes the lock before the creator continues.
//
// Waits and holds are timed per GuestLockClass; recording takes no lock beyond the one the operation already needs.
class GuestLock {
public:
    void lock();
    void unlock();

    // Number of threads queued behind the holder. Meant to be asked by the holder; a thread counts
    // once its place in the queue is fixed.
    std::size_t waiterCount() const {
        const std::uint64_t n = outstanding_.load(std::memory_order_relaxed);
        return n > 0 ? static_cast<std::size_t>(n - 1) : 0;
    }
    bool hasWaiters() const { return waiterCount() > 0; }
    // Releases so far; a holder that gave the lock up and got it back compares it to see whether anyone ran meanwhile.
    std::uint64_t releaseCount() const { return releases_.load(std::memory_order_relaxed); }

    GuestLockStats stats() const;  // any thread

private:
    struct ClassCounters {
        LatencyStats wait{kGuestLockWaitOverUs};
        LatencyStats hold{kGuestLockHoldOverUs};
        std::atomic<std::uint64_t> contended{0}, ahead_sum{0}, ahead_max{0};
        std::array<std::atomic<std::uint64_t>, kGuestLockClassCount> waited_behind_us{};
    };

    std::mutex mutex_;
    std::condition_variable turn_;
    std::uint64_t next_ticket_ = 0;              // guarded by mutex_
    std::uint64_t serving_ = 0;                  // guarded by mutex_
    std::atomic<std::uint64_t> outstanding_{0};  // holder plus waiters
    std::atomic<std::uint64_t> releases_{0};

    // The holder's own state: written by a thread when it gets its turn and read only by that thread in
    // unlock(), before the next ticket is served.
    std::chrono::steady_clock::time_point hold_start_{};
    GuestLockClass holder_class_ = GuestLockClass::Other;
    // Written in unlock() under mutex_; waiters diff it between their ticket and their turn.
    std::chrono::steady_clock::time_point released_at_{};            // guarded by mutex_
    std::array<std::uint64_t, kGuestLockClassCount> hold_sum_us_{};  // guarded by mutex_

    std::array<ClassCounters, kGuestLockClassCount> classes_;
    LatencyStats handoff_{kGuestLockHandoffOverUs};
    std::atomic<std::uint64_t> wakeups_{0}, spurious_wakeups_{0};
};

GuestLock& guestLock();

enum class ThreadState { Created, Running, Blocked, Exited };

struct GuestThreadParams {
    std::uint32_t entry_opd = 0;
    std::uint64_t arg0 = 0, arg1 = 0;
    std::int32_t priority = 1001;
    std::uint32_t stack_size = 0;
    std::uint32_t tls_r13 = 0;
    bool joinable = false;
    // A runtime-internal thread (for example the GCM interrupt thread): it does not count toward
    // stall detection or toward "guest threads left".
    bool service = false;
    std::string name;
};

class GuestThread {
public:
    GuestThread(std::uint32_t id, const GuestThreadParams& params, std::uint32_t stack_addr, std::uint32_t stack_size);
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;

    std::uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    // Fully set up by prepareGuestThread; the host thread runs it as it is.
    Tox2PpuContext& context() { return context_; }
    // Atomic because GuestLock::lock() classifies the caller by it before the caller holds the guest lock.
    std::int32_t priority() const { return priority_.load(std::memory_order_relaxed); }
    void setPriority(std::int32_t priority) { priority_.store(priority, std::memory_order_relaxed); }

    std::uint32_t stackAddr() const { return stack_addr_; }
    std::uint32_t stackSize() const { return stack_size_; }
    std::uint32_t tlsR13() const { return tls_r13_; }
    std::uint32_t entryOpd() const { return entry_opd_; }
    bool joinable() const { return joinable_; }
    void detach() { joinable_ = false; }  // under the guest lock
    bool service() const { return service_; }
    std::uint64_t exitValue() const { return exit_value_; }
    void setExitValue(std::uint64_t value) { exit_value_ = value; }

    ThreadState state() const { return state_.load(); }
    void setState(ThreadState state) { state_.store(state); }
    const char* blockedIn() const { return blocked_in_.load(); }
    bool indefiniteWait() const { return indefinite_wait_.load(); }
    // Milliseconds on a steady clock at which the current wait began.
    std::int64_t blockedSinceMs() const { return blocked_since_ms_.load(); }
    void setBlocked(const char* what, bool indefinite, std::int64_t since_ms) {
        blocked_in_.store(what);
        indefinite_wait_.store(indefinite);
        blocked_since_ms_.store(since_ms);
        state_.store(ThreadState::Blocked);
    }
    std::condition_variable_any& joiners() { return joiners_; }

private:
    std::uint32_t id_;
    std::string name_;
    std::atomic<std::int32_t> priority_;
    std::uint32_t stack_addr_, stack_size_, tls_r13_, entry_opd_;
    bool joinable_;
    bool service_;
    std::uint64_t exit_value_ = 0;
    std::atomic<ThreadState> state_{ThreadState::Created};
    std::atomic<const char*> blocked_in_{nullptr};
    std::atomic<bool> indefinite_wait_{false};
    std::atomic<std::int64_t> blocked_since_ms_{0};
    std::condition_variable_any joiners_;
    Tox2PpuContext context_{};
};

// The guest thread running on the calling host thread; nullptr on host threads.
GuestThread* currentThread();
GuestThread* findThread(std::uint32_t id);

// Allocates the stack, sets the initial registers and registers the thread in state Created.
// The caller may adjust the context and then calls startGuestThread. Returns nullptr when no
// stack is available.
GuestThread* prepareGuestThread(const GuestThreadParams& params);

// Starts the host thread of a prepared guest thread.
void startGuestThread(GuestThread* thread);

// Longest wait of startGuestThreadFirst for the new host thread to queue for the guest lock.
inline constexpr std::chrono::microseconds kStartFirstBound{5000};

// Starts `thread` and lets it take the guest lock before the caller continues: snapshots
// guestLock().waiterCount(), starts the host thread, waits (polling, at most `bound`) until the
// waiter count has grown, then yieldGuestLock(). The new host thread queues asynchronously and
// yieldGuestLock() does nothing while nobody waits, so a bare yield would be a timing bet. Returns
// false when the count did not grow in time; the thread is started anyway and the caller keeps the
// lock. Called by a guest thread holding the guest lock.
bool startGuestThreadFirst(GuestThread* thread, std::chrono::microseconds bound = kStartFirstBound);

// prepareGuestThread + startGuestThread. Returns the new thread id, 0 on failure.
std::uint32_t createGuestThread(const GuestThreadParams& params);

// Marks the calling thread Exited, wakes its joiners, releases the guest lock and ends the host
// thread. Must be called with the guest lock held.
[[noreturn]] void exitCurrentThread(std::uint64_t value);

// Hands the guest lock to the threads queued behind the caller and queues behind them. Returns
// false, doing nothing, when nobody waits. Must be called with the guest lock held.
bool yieldGuestLock();

// The game hands frames between its main and render threads by spinning on sys_ppu_thread_yield (and ignores the
// result). On the console the two threads run on separate hardware threads, so the spin costs nothing; here every
// yield is a guest lock hand-off that taxes the RSX thread, and the spinning thread burns a core. A thread whose yields
// follow each other within kYieldSpinGap is treated as spinning: every kYieldSpinThreshold-th such yield also sleeps
// for kYieldBackoffSleep without the lock.
inline constexpr unsigned kYieldSpinThreshold = 64;
inline constexpr std::chrono::microseconds kYieldSpinGap{50};
inline constexpr std::chrono::microseconds kYieldBackoffSleep{100};

// --yield-backoff on|off; on by default.
void setYieldBackoffEnabled(bool enabled);
// Outcomes of sys_ppu_thread_yield since start, any thread. Every call counts in `calls` and, if it returned true,
// in `switched`; a false return without a backoff sleep counts in `kept`. `to_host` is reserved for yields that hand
// the lock to host-class waiters only and stays 0 until the yield follows lv2 priorities.
struct PpuYieldStats {
    std::uint64_t calls = 0;
    std::uint64_t switched = 0;
    std::uint64_t to_host = 0;
    std::uint64_t kept = 0;
    std::uint64_t backoffs = 0;
};
PpuYieldStats ppuYieldStats();
// Number of backoff sleeps so far, any thread.
std::uint64_t yieldBackoffCount();
// sys_ppu_thread_yield: yieldGuestLock() plus the backoff above. Returns what yieldGuestLock() returns, or true when
// the sleep let another thread take the lock. Must be called with the guest lock held.
bool yieldGuestLockBackoff();

// Bookkeeping around a blocking wait, used by blockingWait. A null thread (host thread) is ignored.
void blockEnter(GuestThread* thread, const char* what, bool indefinite);
void blockLeave(GuestThread* thread);

// Waits on `cv` with the guest lock (which the wait releases) until `done()` holds. `what` names
// the wait in the census. Returns false on timeout. Every blocking syscall and HLE function uses
// it. A wait without a timeout counts toward stall detection.
template <class Pred>
bool blockingWait(std::condition_variable_any& cv, Pred done, const char* what,
                  std::optional<std::chrono::microseconds> timeout) {
    GuestThread* thread = currentThread();
    blockEnter(thread, what, !timeout.has_value());
    bool satisfied = true;
    if (timeout.has_value()) {
        satisfied = cv.wait_for(guestLock(), *timeout, done);
    } else {
        cv.wait(guestLock(), done);
    }
    blockLeave(thread);
    return satisfied;
}

// What the supervisor needs to know about one thread.
struct ThreadHealth {
    ThreadState state = ThreadState::Running;
    bool indefinite_wait = false;
    std::int64_t blocked_since_ms = 0;
    bool service = false;
};

// Registered by the SPU layer, so core does not depend on it: appends one ThreadHealth per live SPU thread
// (running, or blocked in a channel wait with the indefinite flag of that wait). SPU threads run outside the
// guest lock, so a run in which every PPU thread waits for SPU work is not a stall while an SPU thread runs.
using SpuHealthFn = void (*)(std::vector<ThreadHealth>& out);
void setSpuHealthProvider(SpuHealthFn fn);
// Appends the SPU provider's entries to `out`; does nothing without a provider. The supervisor's input is
// the PPU threads plus this.
void appendSpuHealth(std::vector<ThreadHealth>& out);

enum class ThreadVerdict { Ok, NoThreads, Stall };

// The supervisor's decision: no live non-service thread, or every live non-service thread in an
// indefinite wait for at least two seconds at `now_ms`. `stalled`, when given, receives the count of
// stalled threads.
ThreadVerdict judgeThreads(const std::vector<ThreadHealth>& threads, std::int64_t now_ms,
                           std::size_t* stalled = nullptr);

// Runs on the host main thread after boot and never returns: every 100 ms it checks the watchdog
// (`seconds`, 0 = off), a stall (every live thread in an indefinite wait for 2 s) and the absence
// of live threads, and ends the process through stopProcess.
[[noreturn]] void runSupervisor(std::uint32_t seconds);

}  // namespace tox2

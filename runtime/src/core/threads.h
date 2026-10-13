#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
// The test override's class if set, else the explicit class if set, else Ppu/PpuHigh for a guest thread, else Other.
GuestLockClass currentGuestLockClass();
// The lv2 priority the calling thread queues with for the guest lock: the test override if set, else the guest
// thread's priority unless the thread named an explicit host class, else nullopt (a host-class thread).
std::optional<std::int32_t> currentPpuPriority();
// For tests: the calling host thread queues as a PPU thread of this lv2 priority (class Ppu, or PpuHigh below
// kPpuHighPriorityBelow); nullopt restores its own identity. Thread-local.
void setGuestLockPpuPriorityForTesting(std::optional<std::int32_t> priority);
std::string_view guestLockClassName(GuestLockClass cls);  // "ppu", "ppu-high", "spu", "audio", "rsx", "other"

// Thresholds of the `over` counts in GuestLockStats.
inline constexpr std::uint64_t kGuestLockWaitOverUs = 1000;
inline constexpr std::uint64_t kGuestLockHoldOverUs = 1000;
inline constexpr std::uint64_t kGuestLockHandoffOverUs = 100;

struct GuestLockClassStats {
    LatencySnapshot wait;                        // entry -> turn; 0 when uncontended
    LatencySnapshot hold;                        // turn -> unlock()
    std::uint64_t contended = 0;                 // acquisitions that had to queue
    std::uint64_t ahead_sum = 0, ahead_max = 0;  // waiters queued ahead when it queued (contended acquisitions only)
    // Approximate attribution: per holder class, the hold time that ended while this class waited (from the holder
    // classes' cumulative hold sums when it queued and at turn). Only contended acquisitions add to it.
    std::array<std::uint64_t, kGuestLockClassCount> waited_behind_us{};
};

struct GuestLockStats {
    std::array<GuestLockClassStats, kGuestLockClassCount> classes;
    LatencySnapshot handoff;                          // unlock() of the previous holder -> turn of a waiting acquirer
    std::uint64_t wakeups = 0, spurious_wakeups = 0;  // waiter wake-ups, and those that found it was not their turn
    // Hold time of PPU-class holders, and the part of it during which another PPU-class thread was queued (from the
    // later of the hold start and that waiter's arrival): what a second PPU hardware thread could have run.
    std::uint64_t ppu_hold_us = 0, ppu_hold_overlap_us = 0;
};

// The one lock all guest code runs under. A thread takes it when it starts and gives it up only
// inside tox2Yield, blockingWait and exitCurrentThread, so guest threads never run in parallel.
// Waiters queue in a list of nodes on their own stacks and unlock() hands the lock directly to one of them, waking
// only that thread. Host-class waiters (spu, audio, rsx, other) keep their arrival order. PPU waiters fill the "PPU
// slots" of that order: when the oldest queued entry is a PPU one, the slot goes to the PPU waiter with the lowest
// lv2 priority number (then the earliest queue order), except that the thread which held the lock last yields the slot
// to the second best, so the top two ready PPU threads alternate like lv2's two hardware threads (a thread that only
// let host waiters in, through yieldToHostWaiters, keeps its turn). There is no
// displacement of the holder and set/get_priority only store and return the value; a waiter uses the priority it had
// when it queued. The creation rule of sys_ppu_thread_create (startGuestThreadFirst) follows from this: a new thread
// with a numerically lower priority than its creator takes the lock before the creator continues.
//
// Waits and holds are timed per GuestLockClass; recording takes no lock beyond the one the operation already needs.
class GuestLock {
public:
    void lock();
    void unlock();
    // sys_ppu_thread_yield without a switch: when host-class threads wait, hands the lock to the oldest of them and
    // queues the caller (fresh arrival order, but its own PPU tag and the resume flag), so that it gets the lock back
    // after the host waiters queued before it and before any PPU waiter it outranks. Returns false and keeps the lock
    // when no host-class thread waits. Holder only.
    bool yieldToHostWaiters();

    // Number of threads queued behind the holder. Meant to be asked by the holder; a thread counts
    // once its place in the queue is fixed.
    std::size_t waiterCount() const {
        const std::uint64_t n = outstanding_.load(std::memory_order_relaxed);
        return n > 0 ? static_cast<std::size_t>(n - 1) : 0;
    }
    bool hasWaiters() const { return waiterCount() > 0; }
    // Holder only. Exact for the holder: entries leave the queue only when the holder releases, and a waiter that
    // arrives concurrently is equivalent to one that arrived just after the call.
    bool hasPpuWaiterAtOrAbove(std::int32_t priority) const {  // priority numerically <=
        return best_ppu_priority_.load(std::memory_order_relaxed) <= priority;
    }
    bool hasHostWaiters() const { return host_waiters_.load(std::memory_order_relaxed) != 0; }
    // Releases so far; a holder that gave the lock up and got it back compares it to see whether anyone ran meanwhile.
    std::uint64_t releaseCount() const { return releases_.load(std::memory_order_relaxed); }

    GuestLockStats stats() const;  // any thread

private:
    struct Waiter;  // lives on the waiting thread's stack

    struct ClassCounters {
        LatencyStats wait{kGuestLockWaitOverUs};
        LatencyStats hold{kGuestLockHoldOverUs};
        std::atomic<std::uint64_t> contended{0}, ahead_sum{0}, ahead_max{0};
        std::array<std::atomic<std::uint64_t>, kGuestLockClassCount> waited_behind_us{};
    };

    // Under mutex_. The waiter to serve next, nullptr when nobody waits.
    Waiter* chooseNext();
    // Under mutex_.
    void unlink(Waiter* waiter);
    // Under mutex_. Takes the release into the statistics.
    void noteRelease(std::chrono::steady_clock::time_point now, GuestLockClass holder, std::uint64_t hold_us);
    // Under mutex_. Unlinks `next`, marks it granted and wakes it; the woken thread sets the holder fields.
    void grant(Waiter* next);
    void refreshMirrors();  // under mutex_; recomputes best_ppu_priority_ and host_waiters_ from the list
    // Under `lock` (a lock on mutex_): links `node`, waits for its grant, releases `lock` and records the wait.
    void enqueueAndWait(std::unique_lock<std::mutex>& lock, Waiter& node);

    std::mutex mutex_;
    Waiter* head_ = nullptr;               // guarded by mutex_; arrival order
    Waiter* tail_ = nullptr;               // guarded by mutex_
    bool held_ = false;                    // guarded by mutex_; false implies an empty list
    std::uint64_t next_seq_ = 0;           // guarded by mutex_
    std::uint64_t next_ppu_tag_ = 0;       // guarded by mutex_; lv2 order within a priority
    std::uint64_t last_ppu_identity_ = 0;  // guarded by mutex_; the PPU thread granted the lock last
    // Mirrors of the list for the holder, written under mutex_ with a plain load + store.
    std::atomic<std::int32_t> best_ppu_priority_{std::numeric_limits<std::int32_t>::max()};  // min over queued PPU
    std::atomic<std::uint32_t> host_waiters_{0};
    std::atomic<std::uint64_t> outstanding_{0};  // holder plus waiters; written under mutex_
    std::atomic<std::uint64_t> releases_{0};

    // The holder's own state: written by a thread when it gets its turn and read only by that thread in
    // unlock(), before the lock is passed on.
    std::chrono::steady_clock::time_point hold_start_{};
    GuestLockClass holder_class_ = GuestLockClass::Other;
    std::uint64_t holder_ppu_tag_ = 0;  // PPU holder: the tag it was granted or drew; reused by yieldToHostWaiters
    // Written in unlock() under mutex_; waiters diff it between queueing and their turn.
    std::chrono::steady_clock::time_point released_at_{};            // guarded by mutex_
    std::array<std::uint64_t, kGuestLockClassCount> hold_sum_us_{};  // guarded by mutex_

    std::array<ClassCounters, kGuestLockClassCount> classes_;
    LatencyStats handoff_{kGuestLockHandoffOverUs};
    std::atomic<std::uint64_t> wakeups_{0}, spurious_wakeups_{0};
    // Written under mutex_ in unlock(), so a plain load + store is enough; atomic for stats().
    std::atomic<std::uint64_t> ppu_hold_us_{0}, ppu_hold_overlap_us_{0};
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

// Releases the guest lock and takes it again, so the next waiter by the GuestLock rules runs first. The caller is not
// queued when that waiter is chosen and queues with a fresh tag afterwards: it cannot win the slot it just gave up, and
// it goes behind equal-priority waiters. Used for budget expiry and thread creation; a lower-priority PPU waiter can
// run here when no better one is queued. Returns false, doing nothing, when nobody waits. Must be called with the guest
// lock held.
bool yieldGuestLock();

// The game hands frames between its main and render threads by spinning on sys_ppu_thread_yield (and ignores the
// result). On the console the two threads run on separate hardware threads, so the spin costs nothing; here every
// yield that hands the lock over taxes the RSX thread, and the spinning thread burns a core. A thread whose yields
// follow each other within kYieldSpinGap is treated as spinning: every kYieldSpinThreshold-th such yield also sleeps
// for kYieldBackoffSleep without the lock, which lets any waiter run, including a lower-priority PPU thread.
inline constexpr unsigned kYieldSpinThreshold = 64;
inline constexpr std::chrono::microseconds kYieldSpinGap{50};
inline constexpr std::chrono::microseconds kYieldBackoffSleep{100};

// --yield-backoff on|off; on by default.
void setYieldBackoffEnabled(bool enabled);
// Outcomes of sys_ppu_thread_yield since start, any thread. Every call counts in `calls`. Exactly one of the other
// outcome fields counts a call that did not sleep: `switched` (returned true: a PPU thread of equal or higher priority
// was queued and ran), `to_host` (returned false after passing the lock to queued host-class waiters and back), `kept`
// (returned false, nobody eligible). A call that slept counts in `backoffs` and, if it returned true, in `switched`.
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
// sys_ppu_thread_yield with lv2's result rule: true only when a PPU thread of the same or a higher priority was queued,
// and then it has run before this returns. A false return still lets queued host-class waiters (RSX, audio, SPU
// callbacks) take the lock: on the console they run in parallel, so serving them is not a guest switch. A caller that
// is not a PPU thread counts every PPU waiter as eligible. Includes the backoff above, which is checked first. Must be
// called with the guest lock held.
bool yieldPpuThread();

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

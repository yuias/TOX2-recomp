// lv2 mutexes and condition variables (syscalls 100-109) and the lightweight mutex of
// sysPrxForUser. Behaviour follows RPCS3 `Emu/Cell/lv2/sys_mutex.cpp`, `sys_cond.cpp` and
// `Modules/sys_lwmutex_.cpp`. Mutex and condition variable handlers run under the kernel lock; a blocking call waits
// through blockingWait, which gives the lock up. An unlock hands the mutex or lwmutex straight to the waiter its
// protocol chooses (FIFO: the longest waiter; otherwise the lowest priority number, then the longest waiter), so the
// unlocking thread cannot take it back first. Deferred: cond and lwcond waiters still wake in no particular order.
//
// The lwmutex is driven by the {owner, waiter} pair in the guest struct (lwmutex_word.h): an uncontended lock is one
// compare-and-swap {Free, 0} -> {tid, 0} and an uncontended unlock one {tid, 0} -> {Free, 0}, neither needs the kernel
// lock. The kernel lock covers only the slow paths: a lock that finds another owner and an unlock that finds a waiter.
// Invariants (aligned lwmutex):
//   - owner == Free implies waiter == 0, so a fast lock never passes a queued or granted waiter.
//   - With the kernel lock free, waiter == the length of the host wait list (granted nodes included). Both change only
//     inside one kernel section, which is why an unlock that sees waiter != 0 can decide Free or Reserved there without
//     losing a wake-up.
//   - owner == Reserved exactly while one list node is granted and its thread has not resumed.
// A lwmutex whose address is not 8-byte aligned cannot use the atomic pair; its calls run under the kernel lock.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <iterator>
#include <list>
#include <memory>
#include <optional>

#include "core/event_history.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/lwmutex_word.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

// Owner value of a thread that is not a guest thread (host-side callers such as tests).
constexpr std::uint32_t kHostCallerId = 0x01ffffff;

// Thread ids (guest threads and kHostCallerId) all have this top byte; the lwmutex watch uses it to tell an owner word
// holding a thread id from garbage without looking the thread up.
constexpr std::uint32_t kThreadIdTopByte = 0x01;

// Priority of a waiter that is not a guest thread (host-side callers such as tests): behind every guest thread.
constexpr std::int32_t kHostWaiterPriority = 3071;

// A thread blocked on a mutex or lwmutex. It lives in the object's list in arrival order; the node address is stable
// while the waiter blocks and its own waiter erases it.
struct SyncWaiter {
    const GuestThread* thread = nullptr;  // nullptr for a host caller; its priority is read when a waiter is chosen
    std::uint32_t tid = 0;
    bool granted = false;  // the unlock gave this waiter the object; it owns it even if its wait timed out meanwhile
};

struct Mutex {
    std::uint32_t protocol = kSyncFifo;
    std::uint32_t recursive = 0;
    std::uint32_t owner = 0;       // thread id, 0 = free
    std::uint32_t lock_count = 0;  // recursive locks beyond the first
    std::uint32_t cond_count = 0;  // condition variables bound to this mutex
    std::list<SyncWaiter> waiters;
    std::condition_variable_any cv;
};

struct Cond {
    std::shared_ptr<Mutex> mutex;
    std::uint32_t waiters = 0;
    std::uint32_t signaled = 0;  // signals not yet consumed by a waiter; never above `waiters`
    std::condition_variable_any cv;
};

struct Lwmutex {
    std::list<SyncWaiter> waiters;
    std::condition_variable_any cv;
};

// Waiters stay queued in arrival order so a signal reaches the longest waiter; the list node address
// is stable while the waiter blocks.
struct Lwcond {
    struct Waiter {
        bool signaled = false;
    };
    std::list<Waiter> waiters;
    std::condition_variable_any cv;
};

IdMap<Mutex> g_mutexes(kMutexIdBase, kSyncIdStep);
IdMap<Cond> g_conds(kCondIdBase, kSyncIdStep);
IdMap<Lwmutex> g_lwmutexes(kLwmutexIdBase, kSyncIdStep);
IdMap<Lwcond> g_lwconds(kLwcondIdBase, kSyncIdStep);
ProcessSyncObjects g_process_objects;

// Read by the perf thread, written under the kernel lock.
std::atomic<std::uint64_t> g_mutex_waits{0}, g_mutex_handoffs{0}, g_lwmutex_waits{0}, g_lwmutex_handoffs{0};
// Slow-path entries and misaligned calls only; the uncontended calls count nothing here.
std::atomic<std::uint64_t> g_lwmutex_slow_locks{0}, g_lwmutex_slow_unlocks{0}, g_lwmutex_misaligned{0};

// The waiter an unlock gives the object to (RPCS3 `lv2_obj::schedule`): the longest waiter for FIFO, otherwise the
// lowest priority number and among equals the longest waiter. Waiters that were already granted are skipped.
SyncWaiter* chooseWaiter(std::list<SyncWaiter>& waiters, std::uint32_t protocol) {
    SyncWaiter* best = nullptr;
    std::int32_t best_priority = 0;
    for (SyncWaiter& waiter : waiters) {
        if (waiter.granted) continue;
        if (protocol == kSyncFifo) return &waiter;
        const std::int32_t priority = waiter.thread != nullptr ? waiter.thread->priority() : kHostWaiterPriority;
        if (best == nullptr || priority < best_priority) {
            best = &waiter;
            best_priority = priority;
        }
    }
    return best;
}

const GuestThread* callerThread(const Tox2PpuContext* ctx) {
    if (ctx != nullptr && ctx->runtime != nullptr) return static_cast<const GuestThread*>(ctx->runtime);
    return currentThread();
}

// Gives up a mutex whose recursion count is already 0: the chosen waiter becomes the owner at once (lv2 `reown`).
void releaseMutex(Mutex& m) {
    SyncWaiter* next = chooseWaiter(m.waiters, m.protocol);
    if (next == nullptr) {
        m.owner = 0;
        return;
    }
    next->granted = true;
    m.owner = next->tid;
    g_mutex_handoffs.fetch_add(1, std::memory_order_relaxed);
    m.cv.notify_all();
}

bool validMutexAttr(std::uint32_t protocol, std::uint32_t recursive) {
    const bool protocol_ok = protocol == kSyncFifo || protocol == kSyncPriority || protocol == kSyncPriorityInherit;
    return protocol_ok && (recursive == kSyncRecursive || recursive == kSyncNotRecursive);
}

// Takes `m` for `tid`, waiting while another thread owns it. The caller has checked that `tid` does
// not own it already.
std::uint32_t acquireMutex(const std::shared_ptr<Mutex>& m, const GuestThread* thread, std::uint32_t tid,
                           std::uint64_t timeout_us, const char* what) {
    if (m->owner == 0) {
        m->owner = tid;
        return kCellOk;
    }
    m->waiters.push_back({thread, tid, false});
    const auto self = std::prev(m->waiters.end());
    g_mutex_waits.fetch_add(1, std::memory_order_relaxed);
    blockingWait(m->cv, [&self] { return self->granted; }, what, waitTimeout(timeout_us));
    // An unlock that granted this waiter already made it the owner, so a timeout that raced with it loses.
    const bool owns = self->granted;
    m->waiters.erase(self);
    return owns ? kCellOk : kCellEtimedout;
}

// The lock-recursion step shared by mutex and lwmutex try/lock when the caller owns the lock.
std::uint32_t recurse(bool recursive, std::uint32_t& count) {
    if (!recursive) return kCellEdeadlk;
    if (count == 0xffffffff) return kCellEkresource;
    count++;
    return kCellOk;
}

// --lwmutex-watch: an owner word that is neither a lwmutex state nor a thread id was written by something other than
// the lwmutex calls.
void watchOwnerValue(std::uint32_t lwmutex, std::uint32_t owner) {
    if (!lwmutexWatchEnabled()) return;
    if (owner == kLwmutexFree || owner == kLwmutexDead || owner == kLwmutexReserved) return;
    if ((owner >> 24) == kThreadIdTopByte) return;
    lwmutexWatchHit(LwmutexWatchKind::OwnerValue, lwmutex + kLwmutexOwner, 4, 0);
}

// --lwmutex-watch: under the kernel lock the waiter word equals the length of the wait list. A guest store to the word
// breaks that without passing through any hook.
void watchWaiterValue(const LwmutexWord& word, std::uint32_t lwmutex, const Lwmutex& object) {
    if (!lwmutexWatchEnabled()) return;
    if (word.load().waiter == object.waiters.size()) return;
    lwmutexWatchHit(LwmutexWatchKind::WaiterValue, lwmutex + kLwmutexWaiter, 4, 0);
}

// Calls on a lwmutex that is not 8-byte aligned cannot use the atomic pair; they run in a kernel section instead,
// which makes the plain accesses of LwmutexWord safe. The section is opened for the whole call.
class MisalignedSection {
public:
    MisalignedSection(const LwmutexWord& word, std::uint32_t lwmutex) {
        if (word.aligned()) return;
        section_.emplace();
        g_lwmutex_misaligned.fetch_add(1, std::memory_order_relaxed);
        logOnce(LogLevel::Warn, std::format("lwmutex-misaligned:{:08x}", lwmutex),
                "tox2: lwmutex: {:#010x} is not 8-byte aligned; its calls use the kernel lock", lwmutex);
    }

private:
    std::optional<KernelSection> section_;
};

}  // namespace

std::uint32_t callerThreadId(const Tox2PpuContext* ctx) {
    if (ctx != nullptr && ctx->runtime != nullptr) {
        return static_cast<const GuestThread*>(ctx->runtime)->id();
    }
    const GuestThread* thread = currentThread();
    return thread != nullptr ? thread->id() : kHostCallerId;
}

std::uint32_t mutexCreate(std::uint32_t protocol, std::uint32_t recursive, std::uint32_t* id_out) {
    if (!validMutexAttr(protocol, recursive)) return kCellEinval;
    auto m = std::make_shared<Mutex>();
    m->protocol = protocol;
    m->recursive = recursive;
    *id_out = g_mutexes.add(std::move(m));
    return kCellOk;
}

std::uint32_t mutexDestroy(std::uint32_t id) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    if (m->owner != 0 || !m->waiters.empty()) return kCellEbusy;
    if (m->cond_count != 0) return kCellEperm;
    g_mutexes.erase(id);
    return kCellOk;
}

std::uint32_t mutexLock(const Tox2PpuContext* ctx, std::uint32_t id, std::uint64_t timeout_us) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    const std::uint32_t tid = callerThreadId(ctx);
    if (m->owner == tid) return recurse(m->recursive == kSyncRecursive, m->lock_count);
    return acquireMutex(m, callerThread(ctx), tid, timeout_us, "sys_mutex_lock");
}

std::uint32_t mutexTryLock(const Tox2PpuContext* ctx, std::uint32_t id) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    const std::uint32_t tid = callerThreadId(ctx);
    if (m->owner == tid) return recurse(m->recursive == kSyncRecursive, m->lock_count);
    if (m->owner != 0) return kCellEbusy;
    m->owner = tid;
    return kCellOk;
}

std::uint32_t mutexUnlock(const Tox2PpuContext* ctx, std::uint32_t id) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    if (m->owner != callerThreadId(ctx)) return kCellEperm;
    if (m->lock_count != 0) {
        m->lock_count--;
        return kCellOk;
    }
    releaseMutex(*m);
    return kCellOk;
}

namespace {

std::uint32_t lwmutexCreateObject(std::uint8_t* base, std::uint32_t lwmutex, std::uint32_t protocol,
                                  std::uint32_t recursive) {
    tox2Write32(base, lwmutex + kLwmutexOwner, kLwmutexFree);
    tox2Write32(base, lwmutex + kLwmutexWaiter, 0);
    tox2Write32(base, lwmutex + kLwmutexAttribute, recursive | protocol);
    tox2Write32(base, lwmutex + kLwmutexRecursiveCount, 0);
    tox2Write32(base, lwmutex + kLwmutexSleepQueue, g_lwmutexes.add(std::make_shared<Lwmutex>()));
    if (lwmutexWatchEnabled()) lwmutexWatchAdd(lwmutex);
    return kCellOk;
}

// Gives up an lwmutex whose recursion count is already 0, under the kernel lock. With a waiter the owner word becomes
// kLwmutexReserved, as RPCS3's user-side unlock leaves it, and the waiter writes its own id when it resumes; nobody
// else can take the lwmutex in between because a fast lock only succeeds on {Free, 0}. Without one the lwmutex is free
// and, by the invariants, the waiter word is 0.
void releaseLwmutexLocked(std::uint8_t* base, std::uint32_t lwmutex) {
    // The wait list and the waiter word are kept in step only under the kernel lock; the lwmutex fast paths are
    // meant to run without it, so a caller that forgot its KernelSection would race them silently.
    if (splitPpuMode() && !guestLock().heldByCurrentThread()) {
        fatal(FatalKind::Internal, "lwmutex: release without the kernel lock");
    }
    LwmutexWord word(base, lwmutex);
    const auto object = g_lwmutexes.find(tox2Read32(base, lwmutex + kLwmutexSleepQueue));
    if (object != nullptr) watchWaiterValue(word, lwmutex, *object);
    // The protocol is the low nibble of the attribute word; the recursive flag sits above it.
    SyncWaiter* next = object != nullptr
                               ? chooseWaiter(object->waiters, tox2Read32(base, lwmutex + kLwmutexAttribute) & 0xf)
                               : nullptr;
    if (next == nullptr) {
        word.update([](LwmutexPair pair) { return LwmutexPair{kLwmutexFree, pair.waiter}; });
        return;
    }
    next->granted = true;
    word.update([](LwmutexPair pair) { return LwmutexPair{kLwmutexReserved, pair.waiter}; });
    g_lwmutex_handoffs.fetch_add(1, std::memory_order_relaxed);
    object->cv.notify_all();
}

// The lock path after the compare-and-swap found another owner: queues behind it and waits for a grant. Everything
// that decides between taking the lwmutex, queueing and giving up happens under the kernel lock, so the waiter word
// and the wait list change together.
std::uint32_t lwmutexAcquireSlow(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex,
                                 std::uint64_t timeout_us) {
    KernelSection section;
    g_lwmutex_slow_locks.fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t tid = callerThreadId(ctx);
    LwmutexWord word(base, lwmutex);
    const auto object = g_lwmutexes.find(tox2Read32(base, lwmutex + kLwmutexSleepQueue));
    if (object == nullptr) return kCellEsrch;
    watchWaiterValue(word, lwmutex, *object);
    object->waiters.push_back({callerThread(ctx), tid, false});
    const auto self = std::prev(object->waiters.end());
    // The owner may have changed since the compare-and-swap: take a free lwmutex now, queue behind any other owner.
    const auto queued = static_cast<std::uint32_t>(object->waiters.size());
    const LwmutexPair seen = word.update([&](LwmutexPair pair) {
        if (pair.owner == kLwmutexFree) return LwmutexPair{tid, 0};
        if (pair.owner == kLwmutexDead) return pair;
        return LwmutexPair{pair.owner, queued};
    });
    if (seen.owner == kLwmutexFree || seen.owner == kLwmutexDead) {
        object->waiters.erase(self);
        return seen.owner == kLwmutexFree ? kCellOk : kCellEinval;
    }
    g_lwmutex_waits.fetch_add(1, std::memory_order_relaxed);
    blockingWait(object->cv, [&self] { return self->granted; }, "sys_lwmutex_lock", waitTimeout(timeout_us));
    // The unlock left the owner word reserved for a granted waiter, so a timeout that raced with it loses.
    const bool owns = self->granted;
    object->waiters.erase(self);
    const auto remaining = static_cast<std::uint32_t>(object->waiters.size());
    word.update([&](LwmutexPair pair) {
        if (!owns) return LwmutexPair{pair.owner, remaining};
        if (pair.owner != kLwmutexReserved) {
            fatal(FatalKind::Internal,
                  std::format("lwmutex {:#010x}: a granted waiter found owner {:#010x}", lwmutex, pair.owner));
        }
        return LwmutexPair{tid, remaining};
    });
    return owns ? kCellOk : kCellEtimedout;
}

// The two lock paths differ only in what happens when another thread owns the lwmutex.
std::uint32_t lwmutexAcquire(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex, bool block,
                             std::uint64_t timeout_us) {
    const std::uint32_t tid = callerThreadId(ctx);
    LwmutexWord word(base, lwmutex);
    const MisalignedSection misaligned(word, lwmutex);
    LwmutexPair seen{kLwmutexFree, 0};
    if (word.compareExchange(seen, {tid, 0})) return kCellOk;
    watchOwnerValue(lwmutex, seen.owner);
    if (seen.owner == tid) {
        std::uint32_t count = tox2Read32(base, lwmutex + kLwmutexRecursiveCount);
        const std::uint32_t result =
                recurse((tox2Read32(base, lwmutex + kLwmutexAttribute) & kSyncRecursive) != 0, count);
        if (result == kCellOk) tox2Write32(base, lwmutex + kLwmutexRecursiveCount, count);
        return result;
    }
    if (seen.owner == kLwmutexDead) return kCellEinval;
    if (!block) return kCellEbusy;
    return lwmutexAcquireSlow(ctx, base, lwmutex, timeout_us);
}

}  // namespace

std::uint32_t lwmutexCreate(std::uint8_t* base, std::uint32_t lwmutex, std::uint32_t attr) {
    if (lwmutex == 0 || attr == 0) return kCellEfault;
    const std::uint32_t protocol = tox2Read32(base, attr);
    const std::uint32_t recursive = tox2Read32(base, attr + 4);
    if (recursive != kSyncRecursive && recursive != kSyncNotRecursive) return kCellEinval;
    if (protocol != kSyncFifo && protocol != kSyncRetry && protocol != kSyncPriority) {
        return kCellEinval;
    }
    return lwmutexCreateObject(base, lwmutex, protocol, recursive);
}

std::uint32_t lwmutexDestroy(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex) {
    LwmutexWord word(base, lwmutex);
    const LwmutexPair pair = word.load();
    watchOwnerValue(lwmutex, pair.owner);
    if (pair.owner == callerThreadId(ctx)) return kCellEbusy;
    if (pair.owner == kLwmutexDead) return kCellEinval;
    if (pair.owner != kLwmutexFree) return kCellEbusy;
    const std::uint32_t id = tox2Read32(base, lwmutex + kLwmutexSleepQueue);
    const auto object = g_lwmutexes.find(id);
    if (object == nullptr) return kCellEsrch;
    if (!object->waiters.empty()) return kCellEbusy;
    // A fast lock that got in since the load makes the lwmutex busy again.
    LwmutexPair expected{kLwmutexFree, 0};
    if (!word.compareExchange(expected, {kLwmutexDead, 0})) return kCellEbusy;
    g_lwmutexes.erase(id);
    if (lwmutexWatchEnabled()) lwmutexWatchRemove(lwmutex);
    return kCellOk;
}

std::uint32_t lwmutexLock(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex,
                          std::uint64_t timeout_us) {
    return lwmutexAcquire(ctx, base, lwmutex, true, timeout_us);
}

std::uint32_t lwmutexTryLock(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex) {
    return lwmutexAcquire(ctx, base, lwmutex, false, 0);
}

std::uint32_t lwmutexUnlock(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex) {
    const std::uint32_t tid = callerThreadId(ctx);
    LwmutexWord word(base, lwmutex);
    const MisalignedSection misaligned(word, lwmutex);
    const std::uint32_t owner = word.load().owner;
    watchOwnerValue(lwmutex, owner);
    if (owner != tid) return kCellEperm;
    const std::uint32_t count = tox2Read32(base, lwmutex + kLwmutexRecursiveCount);
    if (count != 0) {
        tox2Write32(base, lwmutex + kLwmutexRecursiveCount, count - 1);
        return kCellOk;
    }
    LwmutexPair expected{tid, 0};
    if (word.compareExchange(expected, {kLwmutexFree, 0})) return kCellOk;
    // Only the owner changes the owner word, so the compare-and-swap failed because of a waiter. Whether the lwmutex
    // becomes free or reserved for it is decided where the wait list and the waiter word agree.
    KernelSection section;
    g_lwmutex_slow_unlocks.fetch_add(1, std::memory_order_relaxed);
    releaseLwmutexLocked(base, lwmutex);
    return kCellOk;
}

std::uint32_t lwcondCreate(std::uint8_t* base, std::uint32_t lwcond, std::uint32_t lwmutex, std::uint32_t attr) {
    (void)attr;  // only carries a name
    if (lwcond == 0 || lwmutex == 0) return kCellEfault;
    tox2Write32(base, lwcond + kLwcondMutex, lwmutex);
    tox2Write32(base, lwcond + kLwcondQueue, g_lwconds.add(std::make_shared<Lwcond>()));
    return kCellOk;
}

std::uint32_t lwcondDestroy(std::uint8_t* base, std::uint32_t lwcond) {
    if (lwcond == 0) return kCellEfault;
    const std::uint32_t id = tox2Read32(base, lwcond + kLwcondQueue);
    const auto object = g_lwconds.find(id);
    if (object == nullptr) return kCellEsrch;
    if (!object->waiters.empty()) return kCellEbusy;
    g_lwconds.erase(id);
    return kCellOk;
}

std::uint32_t lwcondWait(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwcond,
                         std::uint64_t timeout_us) {
    if (lwcond == 0) return kCellEfault;
    const auto object = g_lwconds.find(tox2Read32(base, lwcond + kLwcondQueue));
    if (object == nullptr) return kCellEsrch;
    const std::uint32_t lwmutex = tox2Read32(base, lwcond + kLwcondMutex);
    const std::uint32_t tid = callerThreadId(ctx);
    if (lwmutex == 0) return kCellEperm;
    const LwmutexWord word(base, lwmutex);
    const std::uint32_t owner = word.load().owner;
    watchOwnerValue(lwmutex, owner);
    if (owner != tid) return kCellEperm;

    // Release the lwmutex completely, as an unlock of the last recursion level does, and queue before
    // anything can run so a signal sent right after cannot be missed.
    const std::uint32_t saved_count = tox2Read32(base, lwmutex + kLwmutexRecursiveCount);
    tox2Write32(base, lwmutex + kLwmutexRecursiveCount, 0);
    releaseLwmutexLocked(base, lwmutex);

    object->waiters.emplace_back();
    const auto self = std::prev(object->waiters.end());
    const bool signaled =
            blockingWait(object->cv, [&self] { return self->signaled; }, "sys_lwcond_wait", waitTimeout(timeout_us));
    object->waiters.erase(self);

    // Only the lwmutex wait can still block; it has no timeout of its own.
    const std::uint32_t relocked = lwmutexAcquire(ctx, base, lwmutex, true, 0);
    if (relocked != kCellOk) return relocked;
    tox2Write32(base, lwmutex + kLwmutexRecursiveCount, saved_count);
    return signaled ? kCellOk : kCellEtimedout;
}

std::uint32_t lwcondSignal(std::uint8_t* base, std::uint32_t lwcond) {
    if (lwcond == 0) return kCellEfault;
    const auto object = g_lwconds.find(tox2Read32(base, lwcond + kLwcondQueue));
    if (object == nullptr) return kCellEsrch;
    for (Lwcond::Waiter& waiter : object->waiters) {
        if (!waiter.signaled) {
            waiter.signaled = true;
            object->cv.notify_all();
            break;
        }
    }
    return kCellOk;
}

SyncHandoffStats syncHandoffStats() {
    return {g_mutex_waits.load(std::memory_order_relaxed),
            g_mutex_handoffs.load(std::memory_order_relaxed),
            g_lwmutex_waits.load(std::memory_order_relaxed),
            g_lwmutex_handoffs.load(std::memory_order_relaxed),
            g_lwmutex_slow_locks.load(std::memory_order_relaxed),
            g_lwmutex_slow_unlocks.load(std::memory_order_relaxed),
            g_lwmutex_misaligned.load(std::memory_order_relaxed)};
}

const ProcessSyncObjects& processSyncObjects() { return g_process_objects; }

void createProcessSyncObjects(std::uint8_t* base) {
    // One block holds both guest structs; the allocator hands out 64 KiB at a time anyway.
    const std::uint32_t block = process().main.allocate(0x100, 0x10000);
    ProcessSyncObjects objects;
    objects.atexit_lwmutex = block;
    objects.prx_lwmutex = block + 0x20;
    lwmutexCreateObject(base, objects.atexit_lwmutex, kSyncPriority, kSyncRecursive);
    mutexCreate(kSyncPriority, kSyncNotRecursive, &objects.once_mutex);
    mutexCreate(kSyncPriority, kSyncRecursive, &objects.exit_mutex);
    lwmutexCreateObject(base, objects.prx_lwmutex, kSyncPriority, kSyncRecursive);
    g_process_objects = objects;
}

void resetSyncObjects() {
    g_mutexes = IdMap<Mutex>(kMutexIdBase, kSyncIdStep);
    g_conds = IdMap<Cond>(kCondIdBase, kSyncIdStep);
    g_lwmutexes = IdMap<Lwmutex>(kLwmutexIdBase, kSyncIdStep);
    g_lwconds = IdMap<Lwcond>(kLwcondIdBase, kSyncIdStep);
    g_process_objects = ProcessSyncObjects{};
    resetSemaphoreObjects();
    resetEventObjects();
}

// 100 sys_mutex_create(u32* mutex_id, sys_mutex_attribute_t* attr)
void sysMutexCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto id_addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto attr = static_cast<std::uint32_t>(ctx->gpr[4]);
    if (id_addr == 0 || attr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    std::uint32_t id = 0;
    const std::uint32_t result = mutexCreate(tox2Read32(base, attr), tox2Read32(base, attr + 4), &id);
    if (result == kCellOk) tox2Write32(base, id_addr, id);
    setResultCode(ctx, result);
}

// 101 sys_mutex_destroy(u32 mutex_id)
void sysMutexDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const std::uint32_t result = mutexDestroy(id);
    recordHistory(HistoryKind::MutexDestroy, id, result, 0);
    setResultCode(ctx, result);
}

// 102 sys_mutex_lock(u32 mutex_id, u64 timeout)
void sysMutexLock(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, mutexLock(ctx, static_cast<std::uint32_t>(ctx->gpr[3]), ctx->gpr[4]));
}

// 103 sys_mutex_trylock(u32 mutex_id)
void sysMutexTryLock(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, mutexTryLock(ctx, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// 104 sys_mutex_unlock(u32 mutex_id)
void sysMutexUnlock(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, mutexUnlock(ctx, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// 105 sys_cond_create(u32* cond_id, u32 mutex_id, sys_cond_attribute_t* attr)
void sysCondCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto id_addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto mutex = g_mutexes.find(static_cast<std::uint32_t>(ctx->gpr[4]));
    const auto attr = static_cast<std::uint32_t>(ctx->gpr[5]);
    if (mutex == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (id_addr == 0 || attr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    auto cond = std::make_shared<Cond>();
    cond->mutex = mutex;
    mutex->cond_count++;
    tox2Write32(base, id_addr, g_conds.add(std::move(cond)));
    setResultCode(ctx, kCellOk);
}

// 106 sys_cond_destroy(u32 cond_id)
void sysCondDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto cond = g_conds.find(id);
    if (cond == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (cond->waiters != 0) {
        setResultCode(ctx, kCellEbusy);
    } else {
        cond->mutex->cond_count--;
        g_conds.erase(id);
        setResultCode(ctx, kCellOk);
    }
}

// 107 sys_cond_wait(u32 cond_id, u64 timeout): the bound mutex must be held by the caller. It is
// released for the wait and taken again, with its recursion count, before the call returns, also
// on a timeout.
void sysCondWait(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto cond = g_conds.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    if (cond == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    const std::uint32_t tid = callerThreadId(ctx);
    const std::shared_ptr<Mutex>& mutex = cond->mutex;
    if (mutex->owner != tid) {
        setResultCode(ctx, kCellEperm);
        return;
    }

    const std::uint32_t saved_count = mutex->lock_count;
    mutex->lock_count = 0;
    releaseMutex(*mutex);

    cond->waiters++;
    const bool signaled =
            blockingWait(cond->cv, [&cond] { return cond->signaled != 0; }, "sys_cond_wait", waitTimeout(ctx->gpr[4]));
    if (signaled) cond->signaled--;
    cond->waiters--;

    // Only the mutex wait can still block; it has no timeout of its own.
    acquireMutex(mutex, callerThread(ctx), tid, 0, "sys_cond_wait");
    mutex->lock_count = saved_count;
    setResultCode(ctx, signaled ? kCellOk : kCellEtimedout);
}

// 108 sys_cond_signal(u32 cond_id): wakes one waiter. Without waiters the signal is dropped.
void sysCondSignal(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto cond = g_conds.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    if (cond == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (cond->signaled < cond->waiters) {
        cond->signaled++;
        cond->cv.notify_all();
    }
    setResultCode(ctx, kCellOk);
}

// 109 sys_cond_signal_all(u32 cond_id)
void sysCondSignalAll(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto cond = g_conds.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    if (cond == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    cond->signaled = cond->waiters;
    cond->cv.notify_all();
    setResultCode(ctx, kCellOk);
}

}  // namespace tox2

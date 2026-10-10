// lv2 mutexes and condition variables (syscalls 100-109) and the lightweight mutex of
// sysPrxForUser. Behaviour follows RPCS3 `Emu/Cell/lv2/sys_mutex.cpp`, `sys_cond.cpp` and
// `Modules/sys_lwmutex_.cpp`. Handlers run under the guest lock; a blocking call waits through
// blockingWait, which gives the lock up. Deferred: waiters wake in no particular order, not by
// priority or FIFO (see GuestLock).

#include <condition_variable>
#include <cstdint>
#include <memory>

#include "core/guest_memory.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

// Owner value of a thread that is not a guest thread (host-side callers such as tests).
constexpr std::uint32_t kHostCallerId = 0x01ffffff;

struct Mutex {
    std::uint32_t recursive = 0;
    std::uint32_t owner = 0;       // thread id, 0 = free
    std::uint32_t lock_count = 0;  // recursive locks beyond the first
    std::uint32_t cond_count = 0;  // condition variables bound to this mutex
    std::uint32_t waiters = 0;
    std::condition_variable_any cv;
};

struct Cond {
    std::shared_ptr<Mutex> mutex;
    std::uint32_t waiters = 0;
    std::uint32_t signaled = 0;  // signals not yet consumed by a waiter; never above `waiters`
    std::condition_variable_any cv;
};

struct Lwmutex {
    std::uint32_t waiters = 0;
    std::condition_variable_any cv;
};

IdMap<Mutex> g_mutexes(kMutexIdBase, kSyncIdStep);
IdMap<Cond> g_conds(kCondIdBase, kSyncIdStep);
IdMap<Lwmutex> g_lwmutexes(kLwmutexIdBase, kSyncIdStep);
ProcessSyncObjects g_process_objects;

bool validMutexAttr(std::uint32_t protocol, std::uint32_t recursive) {
    const bool protocol_ok = protocol == kSyncFifo || protocol == kSyncPriority || protocol == kSyncPriorityInherit;
    return protocol_ok && (recursive == kSyncRecursive || recursive == kSyncNotRecursive);
}

// Takes `m` for `tid`, waiting while another thread owns it. The caller has checked that `tid` does
// not own it already.
std::uint32_t acquireMutex(const std::shared_ptr<Mutex>& m, std::uint32_t tid, std::uint64_t timeout_us,
                           const char* what) {
    if (m->owner == 0) {
        m->owner = tid;
        return kCellOk;
    }
    m->waiters++;
    const bool taken = blockingWait(m->cv, [&m] { return m->owner == 0; }, what, waitTimeout(timeout_us));
    m->waiters--;
    if (!taken) return kCellEtimedout;
    m->owner = tid;
    return kCellOk;
}

// The lock-recursion step shared by mutex and lwmutex try/lock when the caller owns the lock.
std::uint32_t recurse(bool recursive, std::uint32_t& count) {
    if (!recursive) return kCellEdeadlk;
    if (count == 0xffffffff) return kCellEkresource;
    count++;
    return kCellOk;
}

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
    m->recursive = recursive;
    *id_out = g_mutexes.add(std::move(m));
    return kCellOk;
}

std::uint32_t mutexDestroy(std::uint32_t id) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    // A waiter implies an owner, but a woken waiter may still be queued for the guest lock.
    if (m->owner != 0 || m->waiters != 0) return kCellEbusy;
    if (m->cond_count != 0) return kCellEperm;
    g_mutexes.erase(id);
    return kCellOk;
}

std::uint32_t mutexLock(const Tox2PpuContext* ctx, std::uint32_t id, std::uint64_t timeout_us) {
    const auto m = g_mutexes.find(id);
    if (m == nullptr) return kCellEsrch;
    const std::uint32_t tid = callerThreadId(ctx);
    if (m->owner == tid) return recurse(m->recursive == kSyncRecursive, m->lock_count);
    return acquireMutex(m, tid, timeout_us, "sys_mutex_lock");
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
    m->owner = 0;
    m->cv.notify_one();
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
    return kCellOk;
}

// The two lock paths differ only in what happens when another thread owns the lwmutex.
std::uint32_t lwmutexAcquire(const Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex, bool block,
                             std::uint64_t timeout_us) {
    const std::uint32_t tid = callerThreadId(ctx);
    const std::uint32_t owner = tox2Read32(base, lwmutex + kLwmutexOwner);
    if (owner == kLwmutexFree) {
        tox2Write32(base, lwmutex + kLwmutexOwner, tid);
        return kCellOk;
    }
    if (owner == tid) {
        std::uint32_t count = tox2Read32(base, lwmutex + kLwmutexRecursiveCount);
        const std::uint32_t result =
                recurse((tox2Read32(base, lwmutex + kLwmutexAttribute) & kSyncRecursive) != 0, count);
        if (result == kCellOk) tox2Write32(base, lwmutex + kLwmutexRecursiveCount, count);
        return result;
    }
    if (owner == kLwmutexDead) return kCellEinval;
    if (!block) return kCellEbusy;

    const auto object = g_lwmutexes.find(tox2Read32(base, lwmutex + kLwmutexSleepQueue));
    if (object == nullptr) return kCellEsrch;
    object->waiters++;
    tox2Write32(base, lwmutex + kLwmutexWaiter, object->waiters);
    const bool taken = blockingWait(
            object->cv, [&] { return tox2Read32(base, lwmutex + kLwmutexOwner) == kLwmutexFree; }, "sys_lwmutex_lock",
            waitTimeout(timeout_us));
    object->waiters--;
    tox2Write32(base, lwmutex + kLwmutexWaiter, object->waiters);
    if (!taken) return kCellEtimedout;
    tox2Write32(base, lwmutex + kLwmutexOwner, tid);
    return kCellOk;
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
    const std::uint32_t owner = tox2Read32(base, lwmutex + kLwmutexOwner);
    if (owner == callerThreadId(ctx)) return kCellEbusy;
    if (owner == kLwmutexDead) return kCellEinval;
    if (owner != kLwmutexFree) return kCellEbusy;
    const std::uint32_t id = tox2Read32(base, lwmutex + kLwmutexSleepQueue);
    const auto object = g_lwmutexes.find(id);
    if (object == nullptr) return kCellEsrch;
    if (object->waiters != 0) return kCellEbusy;
    g_lwmutexes.erase(id);
    tox2Write32(base, lwmutex + kLwmutexOwner, kLwmutexDead);
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
    if (tox2Read32(base, lwmutex + kLwmutexOwner) != callerThreadId(ctx)) return kCellEperm;
    const std::uint32_t count = tox2Read32(base, lwmutex + kLwmutexRecursiveCount);
    if (count != 0) {
        tox2Write32(base, lwmutex + kLwmutexRecursiveCount, count - 1);
        return kCellOk;
    }
    tox2Write32(base, lwmutex + kLwmutexOwner, kLwmutexFree);
    const auto object = g_lwmutexes.find(tox2Read32(base, lwmutex + kLwmutexSleepQueue));
    if (object != nullptr) object->cv.notify_one();
    return kCellOk;
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
    setResultCode(ctx, mutexDestroy(static_cast<std::uint32_t>(ctx->gpr[3])));
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
    mutex->owner = 0;
    mutex->cv.notify_one();

    cond->waiters++;
    const bool signaled =
            blockingWait(cond->cv, [&cond] { return cond->signaled != 0; }, "sys_cond_wait", waitTimeout(ctx->gpr[4]));
    if (signaled) cond->signaled--;
    cond->waiters--;

    // Only the mutex wait can still block; it has no timeout of its own.
    acquireMutex(mutex, tid, 0, "sys_cond_wait");
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

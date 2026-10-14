// lv2 semaphores (syscalls 90-94 and 114). Behaviour follows RPCS3 `Emu/Cell/lv2/sys_semaphore.cpp`.
// Handlers run under the kernel lock; a blocking wait goes through blockingWait, which gives the lock
// up. Deferred: waiters wake in no particular order, not by priority or FIFO (see GuestLock), and
// process-shared semaphores (an IPC key) are not supported: the key and flags are not read.

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

struct Semaphore {
    std::int32_t value = 0;
    std::int32_t max = 0;
    std::uint32_t waiters = 0;
    std::condition_variable_any cv;
};

IdMap<Semaphore> g_semaphores(kSemaphoreIdBase, kSyncIdStep);

}  // namespace

void resetSemaphoreObjects() { g_semaphores = IdMap<Semaphore>(kSemaphoreIdBase, kSyncIdStep); }

// 90 sys_semaphore_create(u32* sem_id, sys_semaphore_attribute_t* attr, s32 initial_val, s32 max_val)
// The attribute starts with the protocol (u32) and the pshared word (u32).
void sysSemaphoreCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto id_addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto attr = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto initial = static_cast<std::int32_t>(ctx->gpr[5]);
    const auto max = static_cast<std::int32_t>(ctx->gpr[6]);
    if (id_addr == 0 || attr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::uint32_t protocol = tox2Read32(base, attr);
    if (max <= 0 || initial < 0 || initial > max || (protocol != kSyncFifo && protocol != kSyncPriority)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    auto sem = std::make_shared<Semaphore>();
    sem->value = initial;
    sem->max = max;
    tox2Write32(base, id_addr, g_semaphores.add(std::move(sem)));
    setResultCode(ctx, kCellOk);
}

// 91 sys_semaphore_destroy(u32 sem_id)
void sysSemaphoreDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto sem = g_semaphores.find(id);
    if (sem == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (sem->waiters != 0) {
        setResultCode(ctx, kCellEbusy);
    } else {
        g_semaphores.erase(id);
        setResultCode(ctx, kCellOk);
    }
}

// 92 sys_semaphore_wait(u32 sem_id, u64 timeout)
void sysSemaphoreWait(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto sem = g_semaphores.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    if (sem == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (sem->value <= 0) {
        sem->waiters++;
        const bool taken = blockingWait(
                sem->cv, [&sem] { return sem->value > 0; }, "sys_semaphore_wait", waitTimeout(ctx->gpr[4]));
        sem->waiters--;
        if (!taken) {
            setResultCode(ctx, kCellEtimedout);
            return;
        }
    }
    sem->value--;
    setResultCode(ctx, kCellOk);
}

// 93 sys_semaphore_trywait(u32 sem_id)
void sysSemaphoreTryWait(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto sem = g_semaphores.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    if (sem == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (sem->value <= 0) {
        setResultCode(ctx, kCellEbusy);
    } else {
        sem->value--;
        setResultCode(ctx, kCellOk);
    }
}

// 94 sys_semaphore_post(u32 sem_id, s32 count): fails with CELL_EBUSY when the count would pass
// the maximum, leaving the semaphore unchanged.
void sysSemaphorePost(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto sem = g_semaphores.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    const auto count = static_cast<std::int32_t>(ctx->gpr[4]);
    if (sem == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (count < 0) {
        setResultCode(ctx, kCellEinval);
    } else if (static_cast<std::int64_t>(sem->value) + count > sem->max) {
        setResultCode(ctx, kCellEbusy);
    } else {
        sem->value += count;
        sem->cv.notify_all();
        setResultCode(ctx, kCellOk);
    }
}

// 114 sys_semaphore_get_value(u32 sem_id, s32* count)
void sysSemaphoreGetValue(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto sem = g_semaphores.find(static_cast<std::uint32_t>(ctx->gpr[3]));
    const auto count_addr = static_cast<std::uint32_t>(ctx->gpr[4]);
    if (sem == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (count_addr == 0) {
        setResultCode(ctx, kCellEfault);
    } else {
        tox2Write32(base, count_addr, static_cast<std::uint32_t>(sem->value));
        setResultCode(ctx, kCellOk);
    }
}

}  // namespace tox2

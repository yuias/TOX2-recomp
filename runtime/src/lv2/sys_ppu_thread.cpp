// PPU thread syscalls and the thread creation wrapper. Behaviour follows RPCS3
// `Emu/Cell/lv2/sys_ppu_thread.cpp` and `Modules/sys_ppu_thread_.cpp`. Handlers run under the guest
// lock.

#include "lv2/sys_ppu_thread.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "core/boot.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

// RPCS3 `c_max_ppu_name_size` is 28 including the terminator.
constexpr std::uint32_t kMaxThreadNameChars = 27;

constexpr std::int32_t kMinPriority = 0;  // -512 only for debug or root processes
constexpr std::int32_t kMaxPriority = 3071;

constexpr std::uint32_t kCellCancel = 1;

// RPCS3 keeps this in `ppu_thread::joiner`. Claimed: a thread waits to join; Joined: the exit
// value was collected and the thread is gone for lv2.
enum class JoinState { Claimed, Joined };
std::map<std::uint32_t, JoinState> g_join_states;

GuestThread* threadOf(Tox2PpuContext* ctx) { return static_cast<GuestThread*>(ctx->runtime); }

// A thread that has exited and will never be joined is removed from the id manager in RPCS3.
GuestThread* findLiveThread(Tox2PpuContext* ctx, std::uint32_t id) {
    if (GuestThread* self = threadOf(ctx); self != nullptr && self->id() == id) return self;
    GuestThread* thread = findThread(id);
    if (thread == nullptr || thread->state() != ThreadState::Exited) return thread;
    if (!thread->joinable()) return nullptr;
    const auto it = g_join_states.find(id);
    return it != g_join_states.end() && it->second == JoinState::Joined ? nullptr : thread;
}

}  // namespace

std::uint32_t ppuThreadCreate(std::uint8_t* base, std::uint32_t thread_id, std::uint32_t entry, std::uint64_t arg,
                              std::int32_t priority, std::uint32_t stack_size, std::uint64_t flags,
                              std::uint32_t name) {
    // The firmware stub checks the id pointer before anything else.
    if (thread_id == 0) return kCellEnomem;

    const std::uint32_t r13 = allocateTls();
    if (r13 == 0) return kCellEnomem;

    // RPCS3 leaks the TLS block on the error paths; giving it back is harmless.
    std::uint32_t result = kCellOk;
    if (entry == 0) {
        result = kCellEfault;
    } else if (priority < kMinPriority || priority > kMaxPriority) {
        result = kCellEinval;
    } else if ((flags & (kCreateJoinable | kCreateInterrupt)) == (kCreateJoinable | kCreateInterrupt)) {
        result = kCellEperm;
    }

    GuestThread* thread = nullptr;
    if (result == kCellOk) {
        GuestThreadParams params;
        params.entry_opd = entry;
        params.arg0 = arg;
        params.arg1 = 0;
        params.priority = priority;
        params.stack_size = stack_size;
        params.tls_r13 = r13;
        params.joinable = (flags & kCreateJoinable) != 0;
        if (name != 0) {
            for (std::uint32_t i = 0; i < kMaxThreadNameChars && base[name + i] != 0; i++) {
                params.name.push_back(static_cast<char>(base[name + i]));
            }
        }
        thread = prepareGuestThread(params);
        if (thread == nullptr) result = kCellEnomem;
    }
    if (result != kCellOk) {
        freeTls(r13);
        return result;
    }

    tox2Write64(base, thread_id, thread->id());
    if ((flags & kCreateInterrupt) != 0) return kCellOk;

    // lv2 would run a higher-priority thread at once. The game relies on it: its mixer thread (90)
    // must store its notify queue key before the creator (1005) reads it.
    GuestThread* creator = currentThread();
    if (creator == nullptr || priority >= creator->priority()) {
        startGuestThread(thread);
        return kCellOk;
    }
    logDebug("tox2: thread: {:#010x} {} prio {} starts before creator {:#010x} prio {}", thread->id(), thread->name(),
             priority, creator->id(), creator->priority());
    if (!startGuestThreadFirst(thread)) {
        logInfo("tox2: thread: {:#010x} {} not queued within {} ms; creator continues", thread->id(), thread->name(),
                kStartFirstBound.count() / 1000);
    }
    creator->context().preempt_budget = kTox2PreemptBudget;
    return kCellOk;
}

void resetPpuThreadState() { g_join_states.clear(); }

// 41 _sys_ppu_thread_exit(u64 value): the raw syscall; the HLE wrapper frees the TLS block first.
void sysPpuThreadExit(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    exitCurrentThread(ctx->gpr[3]);
}

// 43 sys_ppu_thread_yield: lv2 returns 0 after a switch and CELL_CANCEL otherwise. Here a switch means a queued PPU
// thread of the same or a higher priority ran; a lower-priority thread never gets time from a yield. Queued host-class
// waiters (RSX, audio, SPU callbacks) are still served on CELL_CANCEL, as they run in parallel on the console.
void sysPpuThreadYield(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kTox2PreemptBudget;
    ctx->gpr[3] = yieldPpuThread() ? kCellOk : kCellCancel;
}

// 44 sys_ppu_thread_join(u32 thread_id, u64* vptr)
void sysPpuThreadJoin(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto vptr = static_cast<std::uint32_t>(ctx->gpr[4]);

    if (id == callerThreadId(ctx)) {
        setResultCode(ctx, kCellEdeadlk);
        return;
    }
    GuestThread* thread = findLiveThread(ctx, id);
    if (thread == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    // Detached threads and a thread that already has a joiner cannot be joined.
    if (!thread->joinable() || g_join_states.contains(id)) {
        setResultCode(ctx, kCellEinval);
        return;
    }

    g_join_states[id] = JoinState::Claimed;
    // The registry never frees threads, so the pointer stays valid across the wait.
    blockingWait(
            thread->joiners(), [thread] { return thread->state() == ThreadState::Exited; }, "sys_ppu_thread_join",
            std::nullopt);
    g_join_states[id] = JoinState::Joined;

    if (vptr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write64(base, vptr, thread->exitValue());
    setResultCode(ctx, kCellOk);
}

// 45 sys_ppu_thread_detach(u32 thread_id): a thread that exited without being joined is dropped, as
// in RPCS3 (findLiveThread no longer finds it).
void sysPpuThreadDetach(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    GuestThread* thread = findLiveThread(ctx, id);
    if (thread == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (!thread->joinable()) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (g_join_states.contains(id)) {
        setResultCode(ctx, kCellEbusy);
        return;
    }
    thread->detach();
    setResultCode(ctx, kCellOk);
}

// 47 sys_ppu_thread_set_priority(u32 thread_id, s32 prio): stored only; a thread already queued for the guest lock
// keeps the priority it queued with.
// ppuThreadCreate compares the stored value of the creator with the priority of a new thread.
void sysPpuThreadSetPriority(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto prio = static_cast<std::int32_t>(ctx->gpr[4]);
    if (prio < kMinPriority || prio > kMaxPriority) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    GuestThread* thread = findLiveThread(ctx, static_cast<std::uint32_t>(ctx->gpr[3]));
    if (thread == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    thread->setPriority(prio);
    setResultCode(ctx, kCellOk);
}

// 48 sys_ppu_thread_get_priority(u32 thread_id, s32* priop)
void sysPpuThreadGetPriority(Tox2PpuContext* ctx, std::uint8_t* base) {
    GuestThread* thread = findLiveThread(ctx, static_cast<std::uint32_t>(ctx->gpr[3]));
    if (thread == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    tox2Write32(base, static_cast<std::uint32_t>(ctx->gpr[4]), static_cast<std::uint32_t>(thread->priority()));
    setResultCode(ctx, kCellOk);
}

}  // namespace tox2

#include "rsx/gcm_interrupts.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <optional>

#include "core/boot.h"
#include "core/fatal.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "rsx/rsx_state.h"

namespace dal {
namespace {

// RPCS3 creates the thread with priority 1 and a 16 KiB stack.
constexpr std::int32_t kThreadPriority = 1;
constexpr std::uint32_t kThreadStackSize = 0x4000;
constexpr char kThreadName[] = "_gcm_intr_thread";

struct Item {
    RsxInterrupt kind;
    std::uint32_t opd;
    std::uint32_t arg;
};

// Everything except the counters is guarded by the guest lock.
struct InterruptState {
    std::condition_variable_any wake;
    std::deque<Item> queue;
    bool vblank_queued = false;
    bool stop = false;
    std::uint32_t thread_id = 0;
    std::atomic<std::uint64_t> calls[3] = {};
};

InterruptState& interruptState() {
    // Leaked: the thread may still be waiting on it while the process exits.
    static InterruptState* state = new InterruptState;
    return *state;
}

const char* kindName(RsxInterrupt kind) {
    switch (kind) {
        case RsxInterrupt::VBlank: return "vblank";
        case RsxInterrupt::Flip: return "flip";
        case RsxInterrupt::User: return "user";
    }
    return "?";
}

void interruptHook(RsxInterrupt kind, std::uint32_t arg) { postGcmInterrupt(kind, arg); }

// Entry function of the thread. Returning ends the thread (only stopGcmInterruptThread asks for it).
void interruptThreadBody(DalPpuContext* ctx, std::uint8_t* base) {
    InterruptState& s = interruptState();
    for (;;) {
        blockingWait(s.wake, [&] { return s.stop || !s.queue.empty(); }, "gcm interrupt wait", std::nullopt);
        if (s.stop) {
            ctx->gpr[3] = 0;
            return;
        }
        const Item item = s.queue.front();
        s.queue.pop_front();
        if (item.kind == RsxInterrupt::VBlank) s.vblank_queued = false;

        const auto index = static_cast<std::size_t>(item.kind);
        if (s.calls[index].fetch_add(1, std::memory_order_relaxed) == 0) {
            logInfo("dal: gcm: first {} handler call: opd {:#010x} arg {}", kindName(item.kind), item.opd, item.arg);
        }
        callGuestFunction(ctx, base, item.opd, {item.arg});
        // Give the guest threads that queued meanwhile a turn before the next handler.
        yieldGuestLock();
    }
}

}  // namespace

std::uint32_t startGcmInterruptThread() {
    InterruptState& s = interruptState();
    if (s.thread_id != 0) return s.thread_id;

    const std::uint32_t r13 = allocateTls();
    if (r13 == 0) fatal(FatalKind::Internal, "no TLS block for the GCM interrupt thread");

    GuestThreadParams params;
    params.entry_opd = registerHostFunction("gcmInterruptThread", interruptThreadBody);
    params.priority = kThreadPriority;
    params.stack_size = kThreadStackSize;
    params.tls_r13 = r13;
    params.service = true;
    params.name = kThreadName;
    const std::uint32_t id = createGuestThread(params);
    if (id == 0) fatal(FatalKind::Internal, "cannot create the GCM interrupt thread");

    s.stop = false;
    s.thread_id = id;
    setRsxInterruptHook(&interruptHook);
    return id;
}

void postGcmInterrupt(RsxInterrupt kind, std::uint32_t arg) {
    InterruptState& s = interruptState();
    const RsxState& rsx = rsxState();
    std::uint32_t opd = 0;
    switch (kind) {
        case RsxInterrupt::VBlank: opd = rsx.vblank_handler; break;
        case RsxInterrupt::Flip: opd = rsx.flip_handler; break;
        case RsxInterrupt::User: opd = rsx.user_handler; break;
    }
    if (opd == 0) {
        if (kind == RsxInterrupt::User) {
            logOnce(std::format("gcm-user-command-no-handler-{}", arg),
                    "dal: gcm: user command cause {} dropped: no user handler registered", arg);
        }
        return;
    }
    if (kind == RsxInterrupt::VBlank) {
        if (s.vblank_queued) {
            logOnce("gcm-vblank-coalesced", "dal: gcm: the vblank handler is behind, ticks are being coalesced");
            return;
        }
        s.vblank_queued = true;
    }
    s.queue.push_back({kind, opd, arg});
    s.wake.notify_one();
}

GcmInterruptCounts gcmInterruptCounts() {
    const InterruptState& s = interruptState();
    const auto load = [&](RsxInterrupt kind) {
        return s.calls[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed);
    };
    return {load(RsxInterrupt::VBlank), load(RsxInterrupt::Flip), load(RsxInterrupt::User)};
}

void stopGcmInterruptThread() {
    InterruptState& s = interruptState();
    setRsxInterruptHook(nullptr);
    if (s.thread_id != 0) {
        GuestThread* thread = findThread(s.thread_id);
        s.stop = true;
        s.wake.notify_all();
        if (thread != nullptr) {
            blockingWait(
                    thread->joiners(), [&] { return thread->state() == ThreadState::Exited; }, "gcm interrupt stop",
                    std::chrono::seconds(5));
        }
    }
    s.queue.clear();
    s.vblank_queued = false;
    s.stop = false;
    s.thread_id = 0;
    for (auto& count : s.calls) count.store(0, std::memory_order_relaxed);
}

}  // namespace dal

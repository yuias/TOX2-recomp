#include "hle/sail_service.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>

#include "core/boot.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "media/movie_decoder.h"

namespace tox2 {
namespace {

// Priorities are only stored (see GuestLock). The stack is larger than the GCM interrupt thread's
// because the game's allocation callbacks run here and may call into its heap.
constexpr std::int32_t kThreadPriority = 1001;
constexpr std::uint32_t kThreadStackSize = 0x10000;
constexpr char kThreadName[] = "_libsail-control";

// Everything is guarded by the guest lock.
struct ServiceState {
    std::condition_variable_any wake;
    std::deque<SailCall> queue;
    // Bumped by stopSailService. A thread runs only while it holds the current value, so a thread
    // that outlives a timed-out stop exits when it next wakes instead of consuming next to its
    // replacement.
    std::uint64_t generation = 1;
    std::uint32_t thread_id = 0;
    // Set when the thread could not be created; later calls are dropped instead of retrying.
    bool start_failed = false;
};

ServiceState& serviceState() {
    // Leaked: the thread may still be waiting on it while the process exits.
    static ServiceState* state = new ServiceState;
    return *state;
}

// Entry function of the thread; r3 holds the generation it serves. Returning ends the thread (only
// stopSailService asks for it, by bumping the generation).
void serviceThreadBody(Tox2PpuContext* ctx, std::uint8_t* base) {
    ServiceState& s = serviceState();
    const std::uint64_t generation = ctx->gpr[3];
    for (;;) {
        blockingWait(
                s.wake, [&] { return s.generation != generation || !s.queue.empty(); }, "sail control wait",
                std::nullopt);
        if (s.generation != generation) {
            ctx->gpr[3] = 0;
            return;
        }
        SailCall call = std::move(s.queue.front());
        s.queue.pop_front();
        if (call) call(ctx, base);
        // Give the guest threads that queued meanwhile a turn before the next call.
        yieldGuestLock();
    }
}

// Written once at start-up, before any movie opens. The pointer is published with release so the
// decode threads, which never take the guest lock, can load it.
struct FactoryHolder {
    std::atomic<MediaCodecFactory*> current{nullptr};
    std::unique_ptr<MediaCodecFactory> owned;
};

FactoryHolder& factoryHolder() {
    // Leaked: a decode thread may still use the factory while static destructors run.
    static FactoryHolder* holder = new FactoryHolder;
    return *holder;
}

bool startServiceThread(ServiceState& s) {
    const std::uint32_t r13 = allocateTls();
    if (r13 == 0) return false;

    GuestThreadParams params;
    params.entry_opd = registerHostFunction("sailServiceThread", serviceThreadBody);
    params.priority = kThreadPriority;
    params.stack_size = kThreadStackSize;
    params.arg0 = s.generation;
    params.tls_r13 = r13;
    params.service = true;
    params.name = kThreadName;
    const std::uint32_t id = createGuestThread(params);
    if (id == 0) {
        freeTls(r13);
        return false;
    }

    s.thread_id = id;
    return true;
}

}  // namespace

bool queueSailCall(SailCall call) {
    ServiceState& s = serviceState();
    if (s.thread_id == 0 && !s.start_failed && !startServiceThread(s)) s.start_failed = true;
    if (s.start_failed) {
        // The game would wait for events that never come; say so instead of failing silently.
        logOnce(LogLevel::Error, "sail-service-thread",
                "tox2: sail: cannot create the {} thread; a queued call is dropped", kThreadName);
        return false;
    }
    s.queue.push_back(std::move(call));
    s.wake.notify_one();
    return true;
}

void stopSailService() {
    ServiceState& s = serviceState();
    if (s.thread_id != 0) {
        GuestThread* thread = findThread(s.thread_id);
        s.generation++;
        s.wake.notify_all();
        if (thread != nullptr) {
            const bool exited = blockingWait(
                    thread->joiners(), [&] { return thread->state() == ThreadState::Exited; }, "sail service stop",
                    std::chrono::seconds(5));
            // The thread still exits on its own once it wakes; its generation is no longer current, so
            // a thread started afterwards is the only consumer.
            if (!exited) logWarn("tox2: sail: the {} thread did not stop within 5 s", kThreadName);
        }
    }
    s.queue.clear();
    s.thread_id = 0;
    s.start_failed = false;
}

void setMediaCodecFactory(std::unique_ptr<MediaCodecFactory> factory) {
    FactoryHolder& holder = factoryHolder();
    holder.current.store(factory.get(), std::memory_order_release);
    holder.owned = std::move(factory);
}

MediaCodecFactory* mediaCodecFactory() { return factoryHolder().current.load(std::memory_order_acquire); }

}  // namespace tox2

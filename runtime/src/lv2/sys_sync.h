#pragma once

// lv2 synchronisation objects that live on the host side, addressed by guest ids, plus the native
// entry points shared by the syscall and HLE handlers. Everything here runs under the guest lock,
// so none of it needs atomics.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "dal/ppu_context.h"

namespace dal {

// Maps guest ids to objects. Ids are `base + n * step` for a counter n that only grows, so a
// destroyed id is never handed out again and a stale id cannot alias a new object.
template <class T>
class IdMap {
public:
    IdMap(std::uint32_t base, std::uint32_t step) : base_(base), step_(step) {}

    std::uint32_t add(std::shared_ptr<T> object) {
        const std::uint32_t id = base_ + next_++ * step_;
        objects_.emplace(id, std::move(object));
        return id;
    }
    // Callers keep the returned pointer while they wait, so an object outlives its id.
    std::shared_ptr<T> find(std::uint32_t id) const {
        const auto it = objects_.find(id);
        return it != objects_.end() ? it->second : nullptr;
    }
    bool erase(std::uint32_t id) { return objects_.erase(id) != 0; }
    std::size_t size() const { return objects_.size(); }

private:
    std::uint32_t base_, step_, next_ = 0;
    std::map<std::uint32_t, std::shared_ptr<T>> objects_;
};

inline constexpr std::uint32_t kMutexIdBase = 0x85000000;
inline constexpr std::uint32_t kCondIdBase = 0x86000000;
inline constexpr std::uint32_t kLwmutexIdBase = 0x95000000;
inline constexpr std::uint32_t kSyncIdStep = 0x100;

// Layout of the guest `sys_lwmutex_t` (RPCS3 `lv2/sys_lwmutex.h`), as byte offsets.
inline constexpr std::uint32_t kLwmutexOwner = 0;
inline constexpr std::uint32_t kLwmutexWaiter = 4;
inline constexpr std::uint32_t kLwmutexAttribute = 8;
inline constexpr std::uint32_t kLwmutexRecursiveCount = 12;
inline constexpr std::uint32_t kLwmutexSleepQueue = 16;
inline constexpr std::uint32_t kLwmutexSize = 24;

// Sync attribute values that the shared constants in cell_error.h do not cover.
inline constexpr std::uint32_t kSyncPriorityInherit = 3;
inline constexpr std::uint32_t kSyncNotRecursive = 0x20;

// lv2 timeouts are in microseconds, 0 meaning "wait forever" (nullopt: counts toward stall
// detection). Long timeouts are clamped so the steady-clock deadline cannot overflow.
inline std::optional<std::chrono::microseconds> waitTimeout(std::uint64_t microseconds) {
    constexpr std::uint64_t kMaxWaitUs = 24ull * 3600 * 1000000;
    if (microseconds == 0) return std::nullopt;
    return std::chrono::microseconds(microseconds < kMaxWaitUs ? microseconds : kMaxWaitUs);
}

// lv2 errors reach the guest as sign-extended s32 values.
inline void setResultCode(DalPpuContext* ctx, std::uint32_t code) {
    ctx->gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

// Id of the guest thread that owns `ctx` (the owner value of mutexes and lwmutexes).
std::uint32_t callerThreadId(const DalPpuContext* ctx);

// Native entry points. They return a cell error code (kCellOk on success); a timeout is
// kCellEtimedout and a failed try-lock is kCellEbusy.
std::uint32_t mutexCreate(std::uint32_t protocol, std::uint32_t recursive, std::uint32_t* id_out);
std::uint32_t mutexDestroy(std::uint32_t id);
std::uint32_t mutexLock(const DalPpuContext* ctx, std::uint32_t id, std::uint64_t timeout_us);
std::uint32_t mutexTryLock(const DalPpuContext* ctx, std::uint32_t id);
std::uint32_t mutexUnlock(const DalPpuContext* ctx, std::uint32_t id);

// `lwmutex` is the guest address of a `sys_lwmutex_t`, `attr` that of a `sys_lwmutex_attribute_t`.
std::uint32_t lwmutexCreate(std::uint8_t* base, std::uint32_t lwmutex, std::uint32_t attr);
std::uint32_t lwmutexDestroy(const DalPpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex);
std::uint32_t lwmutexLock(const DalPpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex,
                          std::uint64_t timeout_us);
std::uint32_t lwmutexTryLock(const DalPpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex);
std::uint32_t lwmutexUnlock(const DalPpuContext* ctx, std::uint8_t* base, std::uint32_t lwmutex);

// The objects the process creates for itself when the main thread's TLS is set up (RPCS3
// `Modules/sys_ppu_thread_.cpp` `sys_initialize_tls`): the atexit and `_lv2prx` lwmutexes (guest
// addresses of their `sys_lwmutex_t`) and the `_lv2ppu` and `_lv2tls` mutexes (ids). All zero
// until createProcessSyncObjects ran.
struct ProcessSyncObjects {
    std::uint32_t atexit_lwmutex = 0;
    std::uint32_t prx_lwmutex = 0;
    std::uint32_t once_mutex = 0;
    std::uint32_t exit_mutex = 0;
};
const ProcessSyncObjects& processSyncObjects();
void createProcessSyncObjects(std::uint8_t* base);

// True while the event queue `id` exists (sys_event.cpp).
bool eventQueueExists(std::uint32_t id);

// Id of the live event queue created with `ipc_key`, or 0 (also for key 0, which is local and never
// registered). Under the guest lock.
std::uint32_t eventQueueIdByKey(std::uint64_t ipc_key);

// Sends an event to queue `id` like a connected port: hands it to the oldest receiver or stores it.
// Returns kCellOk, kCellEbusy (queue full) or kCellEsrch (no such queue). Under the guest lock; may
// be called from a host thread that holds it.
std::uint32_t sendEventToQueue(std::uint32_t id, std::uint64_t source, std::uint64_t data1, std::uint64_t data2,
                               std::uint64_t data3);

// Forgets every event flag, event queue and event port (sys_event.cpp).
void resetEventObjects();

// Forgets every mutex, condition variable, lwmutex and event object. Intended for tests.
void resetSyncObjects();

}  // namespace dal

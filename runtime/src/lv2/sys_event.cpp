// lv2 event flags (syscalls 82, 83, 85, 87, 118, 132, 139), event queues (128-131, 133) and local
// event ports (134-138). Behaviour follows RPCS3 `Emu/Cell/lv2/sys_event_flag.cpp` and
// `sys_event.cpp`. Handlers run under the guest lock; a waiting call parks in blockingWait, which
// gives the lock up. Event queues support IPC keys (a non-zero key is unique among live queues);
// the host can look a queue up by key and send to it (eventQueueIdByKey, sendEventToQueue).
// Deferred: waiters are served in arrival order, not by priority (see GuestLock), and IPC ports
// (sys_event_port_connect_ipc) are not supported.

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "core/guest_memory.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/syscalls.h"

namespace tox2 {
namespace {

// sys_event_flag_attribute_t / sys_event_queue_attribute_t field offsets (RPCS3 `sys_event_flag.h`,
// `sys_event.h`).
constexpr std::uint32_t kFlagAttrProtocol = 0;
constexpr std::uint32_t kFlagAttrType = 20;
constexpr std::uint32_t kQueueAttrProtocol = 0;
constexpr std::uint32_t kQueueAttrType = 4;

constexpr std::uint32_t kWaiterSingle = 0x10000;
constexpr std::uint32_t kWaiterMultiple = 0x20000;
constexpr std::uint32_t kPpuQueue = 1;
constexpr std::uint32_t kSpuQueue = 2;
constexpr std::uint32_t kQueueDestroyForce = 1;
constexpr std::uint32_t kPortLocal = 1;
constexpr std::uint32_t kPortIpc = 3;

// Event flags -------------------------------------------------------------------------------------

struct FlagWaiter {
    std::uint64_t bitptn = 0;
    std::uint32_t mode = 0;
    std::uint64_t result = 0;  // the pattern as the waiter saw it
    std::uint32_t code = kCellOk;
    bool done = false;
};

struct EventFlag {
    std::uint32_t protocol = 0;
    std::uint32_t type = 0;
    std::uint64_t pattern = 0;
    std::vector<FlagWaiter*> waiters;  // arrival order
    std::condition_variable_any cv;
};

bool validFlagMode(std::uint32_t mode) {
    const std::uint32_t wait = mode & 0xf;
    if (wait != kWaitAnd && wait != kWaitOr) return false;
    const std::uint32_t clear = mode & ~0xfu;
    return clear == 0 || clear == kWaitClear || clear == kWaitClearAll;
}

// RPCS3 `lv2_event_flag::check_pattern`: `result` gets the pattern before any clearing.
bool checkFlagPattern(std::uint64_t& pattern, std::uint64_t bitptn, std::uint32_t mode, std::uint64_t* result) {
    if (result != nullptr) *result = pattern;
    const std::uint32_t wait = mode & 0xf;
    if ((wait == kWaitAnd && (pattern & bitptn) != bitptn) || (wait == kWaitOr && (pattern & bitptn) == 0)) {
        return false;
    }
    const std::uint32_t clear = mode & ~0xfu;
    if (clear == kWaitClear) {
        pattern &= ~bitptn;
    } else if (clear == kWaitClearAll) {
        pattern = 0;
    }
    return true;
}

// Event queues and ports --------------------------------------------------------------------------

struct Event {
    std::uint64_t source = 0, data1 = 0, data2 = 0, data3 = 0;
};

struct QueueWaiter {
    Event event;
    std::uint32_t code = kCellOk;
    bool done = false;
};

struct EventQueue {
    std::uint32_t protocol = 0;
    std::uint32_t type = 0;
    std::uint32_t size = 0;
    std::uint64_t ipc_key = 0;  // 0: local queue, not findable by key
    bool exists = true;         // false once destroyed; ports that still point here are disconnected
    std::deque<Event> events;
    std::deque<QueueWaiter*> waiters;  // arrival order
    std::condition_variable_any cv;
};

struct EventPort {
    std::uint32_t type = 0;
    std::uint64_t name = 0;
    std::shared_ptr<EventQueue> queue;

    bool connected() const { return queue != nullptr && queue->exists; }
};

constexpr std::uint32_t kEventFlagIdBase = 0x98000000;
constexpr std::uint32_t kEventQueueIdBase = 0x8d000000;
constexpr std::uint32_t kEventPortIdBase = 0x0e000000;

IdMap<EventFlag> g_flags(kEventFlagIdBase, kSyncIdStep);
IdMap<EventQueue> g_queues(kEventQueueIdBase, kSyncIdStep);
IdMap<EventPort> g_ports(kEventPortIdBase, kSyncIdStep);

// Queue id by IPC key, for live queues created with a non-zero key. The key table is per object
// type in lv2, and event queues are the only keyed objects here.
std::map<std::uint64_t, std::uint32_t> g_queue_keys;

// Hands `event` to the oldest receiver or stores it. RPCS3 `lv2_event_queue::send`.
std::uint32_t queueSend(EventQueue& queue, const Event& event) {
    if (!queue.exists) return kCellEnotconn;
    if (queue.waiters.empty()) {
        if (queue.events.size() >= queue.size) return kCellEbusy;
        queue.events.push_back(event);
        return kCellOk;
    }
    QueueWaiter* waiter = queue.waiters.front();
    queue.waiters.pop_front();
    waiter->event = event;
    waiter->code = kCellOk;
    waiter->done = true;
    queue.cv.notify_all();
    return kCellOk;
}

std::uint32_t arg32(const Tox2PpuContext* ctx, int reg) { return static_cast<std::uint32_t>(ctx->gpr[reg]); }

}  // namespace

bool eventQueueExists(std::uint32_t id) { return g_queues.find(id) != nullptr; }

std::uint32_t eventQueueIdByKey(std::uint64_t ipc_key) {
    const auto it = g_queue_keys.find(ipc_key);
    return it != g_queue_keys.end() ? it->second : 0;
}

std::uint32_t sendEventToQueue(std::uint32_t id, std::uint64_t source, std::uint64_t data1, std::uint64_t data2,
                               std::uint64_t data3) {
    const auto queue = g_queues.find(id);
    if (queue == nullptr) return kCellEsrch;
    return queueSend(*queue, Event{source, data1, data2, data3});
}

void resetEventObjects() {
    g_flags = IdMap<EventFlag>(kEventFlagIdBase, kSyncIdStep);
    g_queues = IdMap<EventQueue>(kEventQueueIdBase, kSyncIdStep);
    g_queue_keys.clear();
    g_ports = IdMap<EventPort>(kEventPortIdBase, kSyncIdStep);
}

// 82 sys_event_flag_create(u32* id, sys_event_flag_attribute_t* attr, u64 init)
void sysEventFlagCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t id_addr = arg32(ctx, 3);
    const std::uint32_t attr = arg32(ctx, 4);
    if (id_addr == 0 || attr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::uint32_t protocol = tox2Read32(base, attr + kFlagAttrProtocol);
    const std::uint32_t type = tox2Read32(base, attr + kFlagAttrType);
    if ((protocol != kSyncFifo && protocol != kSyncPriority) || (type != kWaiterSingle && type != kWaiterMultiple)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    auto flag = std::make_shared<EventFlag>();
    flag->protocol = protocol;
    flag->type = type;
    flag->pattern = ctx->gpr[5];
    tox2Write32(base, id_addr, g_flags.add(std::move(flag)));
    setResultCode(ctx, kCellOk);
}

// 83 sys_event_flag_destroy(u32 id)
void sysEventFlagDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    const auto flag = g_flags.find(id);
    if (flag == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (!flag->waiters.empty()) {
        setResultCode(ctx, kCellEbusy);
    } else {
        g_flags.erase(id);
        setResultCode(ctx, kCellOk);
    }
}

// 85 sys_event_flag_wait(u32 id, u64 bitptn, u32 mode, u64* result, u64 timeout). `*result` is
// written on every return, 0 when the call fails before looking at the pattern.
void sysEventFlagWait(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t id = arg32(ctx, 3);
    const std::uint64_t bitptn = ctx->gpr[4];
    const std::uint32_t mode = arg32(ctx, 5);
    const std::uint32_t result_addr = arg32(ctx, 6);
    const std::uint64_t timeout = ctx->gpr[7];

    std::uint64_t result = 0;
    std::uint32_t code = kCellOk;
    [&] {
        if (!validFlagMode(mode)) {
            code = kCellEinval;
            return;
        }
        const auto flag = g_flags.find(id);
        if (flag == nullptr) {
            code = kCellEsrch;
            return;
        }
        if (checkFlagPattern(flag->pattern, bitptn, mode, &result)) return;
        if (flag->type == kWaiterSingle && !flag->waiters.empty()) {
            code = kCellEperm;
            return;
        }

        FlagWaiter waiter;
        waiter.bitptn = bitptn;
        waiter.mode = mode;
        flag->waiters.push_back(&waiter);
        const bool woken =
                blockingWait(flag->cv, [&waiter] { return waiter.done; }, "sys_event_flag_wait", waitTimeout(timeout));
        if (!woken) {
            std::erase(flag->waiters, &waiter);
            waiter.code = kCellEtimedout;
            waiter.result = flag->pattern;
        }
        code = waiter.code;
        result = waiter.result;
    }();
    if (result_addr != 0) tox2Write64(base, result_addr, result);
    setResultCode(ctx, code);
}

std::uint32_t eventFlagSet(std::uint32_t id, std::uint64_t bitptn) {
    const auto flag = g_flags.find(id);
    if (flag == nullptr) return kCellEsrch;
    if ((flag->pattern & bitptn) == bitptn) return kCellOk;

    // Waiters take their turn on the new pattern; each one that matches may clear bits for the next.
    std::uint64_t pattern = flag->pattern | bitptn;
    bool woke = false;
    for (auto it = flag->waiters.begin(); it != flag->waiters.end();) {
        FlagWaiter* waiter = *it;
        if (checkFlagPattern(pattern, waiter->bitptn, waiter->mode, &waiter->result)) {
            waiter->code = kCellOk;
            waiter->done = true;
            woke = true;
            it = flag->waiters.erase(it);
            if (pattern == 0) break;
        } else {
            ++it;
        }
    }
    flag->pattern = pattern;
    if (woke) flag->cv.notify_all();
    return kCellOk;
}

// 87 sys_event_flag_set(u32 id, u64 bitptn)
void sysEventFlagSet(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, eventFlagSet(arg32(ctx, 3), ctx->gpr[4]));
}

// 118 sys_event_flag_clear(u32 id, u64 bitptn): keeps the bits that are set in `bitptn`.
void sysEventFlagClear(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto flag = g_flags.find(arg32(ctx, 3));
    if (flag == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    flag->pattern &= ctx->gpr[4];
    setResultCode(ctx, kCellOk);
}

// 132 sys_event_flag_cancel(u32 id, u32* num): every waiter returns ECANCELED with the current
// pattern; `*num` is the number of waiters.
void sysEventFlagCancel(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t num_addr = arg32(ctx, 4);
    if (num_addr != 0) tox2Write32(base, num_addr, 0);
    const auto flag = g_flags.find(arg32(ctx, 3));
    if (flag == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(flag->waiters.size());
    for (FlagWaiter* waiter : flag->waiters) {
        waiter->code = kCellEcanceled;
        waiter->result = flag->pattern;
        waiter->done = true;
    }
    flag->waiters.clear();
    if (count != 0) flag->cv.notify_all();
    if (num_addr != 0) tox2Write32(base, num_addr, count);
    setResultCode(ctx, kCellOk);
}

// 139 sys_event_flag_get(u32 id, u64* flags)
void sysEventFlagGet(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t flags_addr = arg32(ctx, 4);
    const auto flag = g_flags.find(arg32(ctx, 3));
    if (flag == nullptr) {
        if (flags_addr != 0) tox2Write64(base, flags_addr, 0);
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (flags_addr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write64(base, flags_addr, flag->pattern);
    setResultCode(ctx, kCellOk);
}

// 128 sys_event_queue_create(u32* id, sys_event_queue_attribute_t* attr, u64 ipc_key, s32 size):
// a non-zero key is "create new": a live queue that already holds it makes the call fail with
// EEXIST, without writing `*id`.
void sysEventQueueCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t id_addr = arg32(ctx, 3);
    const std::uint32_t attr = arg32(ctx, 4);
    const auto size = static_cast<std::int32_t>(ctx->gpr[6]);
    if (size <= 0 || size > 127) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (id_addr == 0 || attr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::uint32_t protocol = tox2Read32(base, attr + kQueueAttrProtocol);
    const std::uint32_t type = tox2Read32(base, attr + kQueueAttrType);
    if ((protocol != kSyncFifo && protocol != kSyncPriority) || (type != kPpuQueue && type != kSpuQueue)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    const std::uint64_t ipc_key = ctx->gpr[5];
    if (ipc_key != 0 && g_queue_keys.contains(ipc_key)) {
        setResultCode(ctx, kCellEexist);
        return;
    }
    auto queue = std::make_shared<EventQueue>();
    queue->protocol = protocol;
    queue->type = type;
    queue->size = static_cast<std::uint32_t>(size);
    queue->ipc_key = ipc_key;
    const std::uint32_t id = g_queues.add(std::move(queue));
    if (ipc_key != 0) g_queue_keys.emplace(ipc_key, id);
    tox2Write32(base, id_addr, id);
    setResultCode(ctx, kCellOk);
}

// 129 sys_event_queue_destroy(u32 id, s32 mode): without the force mode a queue with receivers
// is busy; with it the receivers return ECANCELED.
void sysEventQueueDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    const std::uint32_t mode = arg32(ctx, 4);
    if (mode != 0 && mode != kQueueDestroyForce) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    const auto queue = g_queues.find(id);
    if (queue == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (mode == 0 && !queue->waiters.empty()) {
        setResultCode(ctx, kCellEbusy);
        return;
    }
    for (QueueWaiter* waiter : queue->waiters) {
        waiter->code = kCellEcanceled;
        waiter->done = true;
    }
    const bool had_waiters = !queue->waiters.empty();
    queue->waiters.clear();
    queue->exists = false;
    if (queue->ipc_key != 0) g_queue_keys.erase(queue->ipc_key);
    g_queues.erase(id);
    if (had_waiters) queue->cv.notify_all();
    setResultCode(ctx, kCellOk);
}

// 130 sys_event_queue_receive(u32 id, sys_event_t* unused, u64 timeout): the event comes back in
// r4..r7 (source, data1..3); the pointer argument is never written.
void sysEventQueueReceive(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto queue = g_queues.find(arg32(ctx, 3));
    if (queue == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (queue->type != kPpuQueue) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    const auto deliver = [ctx](const Event& event) {
        ctx->gpr[4] = event.source;
        ctx->gpr[5] = event.data1;
        ctx->gpr[6] = event.data2;
        ctx->gpr[7] = event.data3;
    };
    if (!queue->events.empty()) {
        deliver(queue->events.front());
        queue->events.pop_front();
        setResultCode(ctx, kCellOk);
        return;
    }

    QueueWaiter waiter;
    queue->waiters.push_back(&waiter);
    const bool woken = blockingWait(
            queue->cv, [&waiter] { return waiter.done; }, "sys_event_queue_receive", waitTimeout(ctx->gpr[5]));
    if (!woken) {
        std::erase(queue->waiters, &waiter);
        setResultCode(ctx, kCellEtimedout);
        return;
    }
    if (waiter.code == kCellOk) deliver(waiter.event);
    setResultCode(ctx, waiter.code);
}

// 131 sys_event_queue_tryreceive(u32 id, sys_event_t* events, s32 size, u32* number): moves up to
// `size` stored events, oldest first, into the array as {source, data1, data2, data3} (8 bytes each)
// and reports how many. Never waits.
void sysEventQueueTryReceive(Tox2PpuContext* ctx, std::uint8_t* base) {
    constexpr std::uint32_t kEventSize = 32;
    const auto queue = g_queues.find(arg32(ctx, 3));
    const std::uint32_t array = arg32(ctx, 4);
    const std::int32_t size = static_cast<std::int32_t>(ctx->gpr[5]);
    const std::uint32_t number = arg32(ctx, 6);
    if (queue == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(queue->events.size(), size > 0 ? size : 0));
    const Process& p = process();
    if (number == 0 || !p.memory.isCommitted(number, 4) ||
        (count != 0 && (array == 0 || !p.memory.isCommitted(array, count * kEventSize)))) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    for (std::uint32_t i = 0; i < count; i++) {
        const Event& event = queue->events.front();
        const std::uint32_t at = array + i * kEventSize;
        tox2Write64(base, at, event.source);
        tox2Write64(base, at + 8, event.data1);
        tox2Write64(base, at + 16, event.data2);
        tox2Write64(base, at + 24, event.data3);
        queue->events.pop_front();
    }
    tox2Write32(base, number, count);
    setResultCode(ctx, kCellOk);
}

// 133 sys_event_queue_drain(u32 id)
void sysEventQueueDrain(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto queue = g_queues.find(arg32(ctx, 3));
    if (queue == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    queue->events.clear();
    setResultCode(ctx, kCellOk);
}

// 134 sys_event_port_create(u32* id, s32 port_type, u64 name)
void sysEventPortCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t id_addr = arg32(ctx, 3);
    const std::uint32_t type = arg32(ctx, 4);
    if (type != kPortLocal && type != kPortIpc) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (id_addr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    auto port = std::make_shared<EventPort>();
    port->type = type;
    port->name = ctx->gpr[5];
    tox2Write32(base, id_addr, g_ports.add(std::move(port)));
    setResultCode(ctx, kCellOk);
}

// 135 sys_event_port_destroy(u32 id)
void sysEventPortDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    const auto port = g_ports.find(id);
    if (port == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (port->connected()) {
        setResultCode(ctx, kCellEisconn);
    } else {
        g_ports.erase(id);
        setResultCode(ctx, kCellOk);
    }
}

// 136 sys_event_port_connect_local(u32 port_id, u32 queue_id)
void sysEventPortConnectLocal(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto port = g_ports.find(arg32(ctx, 3));
    const auto queue = g_queues.find(arg32(ctx, 4));
    if (port == nullptr || queue == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (port->type != kPortLocal) {
        setResultCode(ctx, kCellEinval);
    } else if (port->connected()) {
        setResultCode(ctx, kCellEisconn);
    } else {
        port->queue = queue;
        setResultCode(ctx, kCellOk);
    }
}

// 137 sys_event_port_disconnect(u32 port_id)
void sysEventPortDisconnect(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto port = g_ports.find(arg32(ctx, 3));
    if (port == nullptr) {
        setResultCode(ctx, kCellEsrch);
    } else if (!port->connected()) {
        setResultCode(ctx, kCellEnotconn);
    } else {
        port->queue.reset();
        setResultCode(ctx, kCellOk);
    }
}

// 138 sys_event_port_send(u32 port_id, u64 data1, u64 data2, u64 data3): never blocks. An unnamed
// port reports (process id << 32) | port id as the event source.
void sysEventPortSend(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    const auto port = g_ports.find(id);
    if (port == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (!port->connected()) {
        setResultCode(ctx, kCellEnotconn);
        return;
    }
    Event event;
    event.source = port->name != 0 ? port->name : (std::uint64_t{kProcessId} << 32) | id;
    event.data1 = ctx->gpr[4];
    event.data2 = ctx->gpr[5];
    event.data3 = ctx->gpr[6];
    setResultCode(ctx, queueSend(*port->queue, event));
}

}  // namespace tox2

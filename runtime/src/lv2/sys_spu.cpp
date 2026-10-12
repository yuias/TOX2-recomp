// lv2 SPU thread groups and images (spurs-description section 4.1 to 4.4): groups, threads, event
// connections and the SPU image records, and the execution of started groups on tox2::SpuThread host
// threads. The SPU image import and close functions (sysPrxForUser) keep their records here too.
//
// Handlers run under the guest lock. An SPU host thread takes that lock only inside the owner callbacks,
// around the group and event state it touches, and never while it waits on a channel. PPU-side waits
// for SPU threads go through blockingWait, which gives the lock up.

#include "lv2/sys_spu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "spu/spu_channels.h"
#include "spu/spu_thread.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kGroupIdBase = 0x04000100;
constexpr std::uint32_t kGroupIdStep = 0x100;
constexpr std::uint32_t kGroupCount = 255;
constexpr std::uint32_t kMaxThreadsPerGroup = 6;
constexpr std::uint32_t kMaxNameSize = 0x80;
constexpr std::uint32_t kMaxPort = 63;
// The SPU number of sys_spu_thread_initialize indexes a per-group table of 256 entries (RPCS3 bounds it the
// same way); it need not be below the group's thread count.
constexpr std::uint32_t kMaxSpuNumber = 256;

// Queue bindings of one thread (sys_spu_thread_bind_queue) and the type of queue that can be bound.
constexpr std::uint32_t kMaxQueueBindings = 32;
constexpr std::uint32_t kSpuQueueType = 2;

// Group types that sys_spu_thread_group_create accepts. Bit 0x08 marks groups that do not use the
// scheduler, so their priority is not range-checked.
constexpr std::uint32_t kGroupTypeNormal = 0;
constexpr std::uint32_t kGroupTypeFromContainer = 4;
constexpr std::uint32_t kGroupTypeNonContext = 0x18;
constexpr std::uint32_t kGroupTypeSystem = 0x20;
constexpr std::uint32_t kGroupTypeNoScheduler = 0x08;
constexpr std::int32_t kMinPriority = 16;
constexpr std::int32_t kMaxPriority = 255;

// Event types of sys_spu_thread_group_connect_event.
constexpr std::uint32_t kEventRun = 1;
constexpr std::uint32_t kEventException = 2;
constexpr std::uint32_t kEventSystemModule = 4;
constexpr std::uint32_t kThreadEventUser = 1;

constexpr std::uint32_t kMaxEntryPoint = 0x3FFFC;
constexpr std::int32_t kMaxSegments = 0x20;
constexpr std::uint64_t kLocalStoreSize = 0x40000;

constexpr std::uint32_t kSpuSegmentSize = 16;
constexpr std::int32_t kSegmentCopy = 1;
constexpr std::int32_t kSegmentFill = 2;
constexpr std::int32_t kSegmentInfo = 4;
// Stop codes of SPU code that lv2 gives a meaning (spurs-description section 4.3).
constexpr std::uint32_t kStopYield = 0x100;
constexpr std::uint32_t kStopGroupExit = 0x101;
constexpr std::uint32_t kStopThreadExit = 0x102;
constexpr std::uint32_t kStopReceiveEvent = 0x110;
constexpr std::uint32_t kStopTryReceiveEvent = 0x111;

// Why a group stopped, as sys_spu_thread_group_join reports it.
constexpr std::uint32_t kCauseGroupExit = 1;
constexpr std::uint32_t kCauseAllThreadsExit = 2;
constexpr std::uint32_t kCauseTerminated = 4;

// Event sources: the user events of an SPU thread (WrOutIntrMbox) and the group's RUN event.
constexpr std::uint64_t kSpuThreadEventUserKey = 0xFFFFFFFF53505501ull;
constexpr std::uint64_t kSpuGroupEventRunKey = 0xFFFFFFFF53505500ull;

// A full event queue is tried again after this long.
constexpr std::chrono::milliseconds kQueueRetry{1};

constexpr std::uint32_t kElfHeaderSize = 0x34;
constexpr std::uint32_t kProgramHeaderSize = 0x20;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint16_t kSpuMachine = 23;

// By the guest address of the segment array.
std::map<std::uint32_t, SpuImageRecord> g_images;

struct SpuSegment {
    std::int32_t type;
    std::uint32_t ls_start;
    std::int32_t size;
    std::uint32_t addr_or_value;
};

// A slot of the thread's queue table: the number the SPU program writes before stop 0x110/0x111 and the
// queue id. Queue ids are never 0, so 0 marks a free slot.
struct QueueBinding {
    std::uint32_t key = 0;
    std::uint32_t queue = 0;
};

struct SpuThreadRecord {
    bool initialized = false;
    std::uint32_t spu_num = 0;  // the slot number the caller chose; records stay dense in initialisation order
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t option = 0;
    std::array<std::uint64_t, 4> args{};
    std::uint32_t entry = 0;
    std::uint32_t source = 0;  // guest address of the imported ELF, 0 when the image is not recorded
    std::vector<SpuSegment> segments;
    std::map<std::uint32_t, std::uint32_t> ports;  // SPU port -> event queue id
    // Like the ports, the bindings outlive each run. A queue that is destroyed stays in its slot, as in lv2.
    std::array<QueueBinding, kMaxQueueBindings> bindings{};

    // The runtime thread of the latest run. It stays after the run ends so that read_ls and the mailbox
    // calls still reach it; the next start replaces it.
    std::unique_ptr<SpuThread> cpu;
    bool mailbox_warned = false;  // the full-mailbox overwrite was logged
};

enum class GroupState { NotInitialized, Initialized, Running };

// How a run ended, as sys_spu_thread_group_join reports it.
struct JoinResult {
    std::uint32_t cause = 0;
    std::uint32_t status = 0;
};

struct SpuGroup {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t num = 0;
    std::int32_t priority = 0;
    std::uint32_t type = 0;
    GroupState state = GroupState::NotInitialized;
    std::vector<SpuThreadRecord> threads;                // one slot per thread index
    std::map<std::uint32_t, std::uint32_t> connections;  // event type -> event queue id
    std::uint64_t cooperative_victims = 0;

    // The run in progress; all of it is guarded by the guest lock.
    std::uint32_t unfinished = 0;        // threads whose host thread has not reported its end
    bool terminated = false;             // sys_spu_thread_group_terminate was called
    bool group_exit = false;             // a thread ended with stop 0x101
    std::uint32_t exit_status = 0;       // the status of the terminate call or of the stop 0x101
    std::uint32_t last_exit_status = 0;  // the status of the latest stop 0x102
    // How the latest run ended: a join that waits gets it through `handoff` (so a restart cannot drop it before
    // that joiner runs); otherwise it stays in `result` until a join takes it or the next start discards it.
    std::uint32_t generation = 0;  // counts starts
    std::optional<JoinResult> handoff;
    std::optional<JoinResult> result;
    bool joining = false;       // a PPU thread waits in join
    std::uint32_t waiters = 0;  // PPU threads inside join or terminate; they hold a pointer to the group
    std::condition_variable_any changed;
};

std::array<std::optional<SpuGroup>, kGroupCount> g_groups;
std::uint32_t g_next_group_slot = 0;
std::uint32_t g_next_thread_number = 1;

SpuGroup* findGroup(std::uint32_t id) {
    if (id < kGroupIdBase || (id - kGroupIdBase) % kGroupIdStep != 0) return nullptr;
    const std::uint32_t slot = (id - kGroupIdBase) / kGroupIdStep;
    return slot < kGroupCount && g_groups[slot] ? &*g_groups[slot] : nullptr;
}

// Thread ids carry the low 24 bits of their group's id.
SpuGroup* findGroupOfThread(std::uint32_t id) { return findGroup((kGroupIdBase & 0xFF000000) | (id & 0x00FFFFFF)); }

SpuThreadRecord* findSpuThread(std::uint32_t id) {
    SpuGroup* group = findGroupOfThread(id);
    if (group == nullptr) return nullptr;
    for (SpuThreadRecord& thread : group->threads) {
        if (thread.initialized && thread.id == id) return &thread;
    }
    return nullptr;
}

std::uint32_t arg32(const Tox2PpuContext* ctx, int reg) { return static_cast<std::uint32_t>(ctx->gpr[reg]); }

// A name of at most `size` bytes, ending at the first NUL. Null when the bytes are not guest memory.
std::optional<std::string> readName(const std::uint8_t* base, std::uint32_t addr, std::uint32_t size) {
    if (size == 0) return std::string();
    if (addr == 0 || !process().memory.isCommitted(addr, size)) return std::nullopt;
    std::string name;
    for (std::uint32_t i = 0; i < size; i++) {
        const std::uint8_t c = tox2Read8(base, addr + i);
        if (c == 0) break;
        name.push_back(static_cast<char>(c));
    }
    return name;
}

const char* segmentKind(std::int32_t type) {
    return type == kSegmentCopy ? "copy" : type == kSegmentFill ? "fill" : type == kSegmentInfo ? "info" : "?";
}

const char* eventKind(std::uint32_t type) {
    return type == kEventRun ? "run" : type == kEventException ? "exception" : "system module";
}

std::string sourceText(std::uint32_t source) {
    return source != 0 ? std::format("{:#010x}", source) : std::string("?");
}

// Ends the run of `group` once its last thread has ended. Under the guest lock.
void stopGroup(SpuGroup& group) {
    JoinResult result;
    if (group.terminated) {
        result = {kCauseTerminated, group.exit_status};
    } else if (group.group_exit) {
        result = {kCauseGroupExit, group.exit_status};
    } else {
        result = {kCauseAllThreadsExit, group.last_exit_status};
    }
    (group.joining ? group.handoff : group.result) = result;
    group.state = GroupState::Initialized;
    logInfo("tox2: spu: group {:#010x} \"{}\" stopped: cause {}, status {:#x}", group.id, group.name, result.cause,
            result.status);
    group.changed.notify_all();
}

// The owner of every lv2 SPU thread. The callbacks run on the SPU's host thread; each one takes the
// guest lock for the lv2 state it reads or changes and never holds it across a wait on a channel.
class GroupOwner final : public SpuOwner {
public:
    void onInterruptMailbox(SpuThread& cpu, std::uint32_t value) override;
    bool onStop(SpuThread& cpu, std::uint32_t site, std::uint32_t code) override;
    void onFinished(SpuThread& cpu, SpuLeave how) override;
};

GroupOwner g_owner;

// WrOutIntrMbox is a system call of the SPU thread (spurs-description section 4.4).
void GroupOwner::onInterruptMailbox(SpuThread& cpu, std::uint32_t value) {
    const std::uint32_t code = value >> 24;
    if (code > 128 && code != 192) {
        spuStop(cpu,
                std::format("tox2: stop: spu: WrOutIntrMbox {:#010x} on thread {:#010x}: code {} is not implemented",
                            value, cpu.id(), code));
    }
    std::uint32_t d1 = 0;
    if (!cpu.channels().takeOutMbox(&d1)) {
        spuStop(cpu, std::format("tox2: stop: spu: WrOutIntrMbox {:#010x} on thread {:#010x} with an empty outbound "
                                 "mailbox",
                                 value, cpu.id()));
    }
    const bool send_event = code < 64;
    const bool throw_event = code >= 64 && code < 128;

    for (;;) {
        {
            std::lock_guard<GuestLock> guard(guestLock());
            const SpuThreadRecord* record = findSpuThread(cpu.id());
            if (record == nullptr) return;  // the group is gone (test teardown)
            std::uint32_t result = kCellOk;
            bool queue_full = false;
            if (send_event || throw_event) {
                const std::uint32_t port = code & kMaxPort;
                const auto bound = record->ports.find(port);
                if (send_event && cpu.channels().inMboxCount() != 0) {
                    // The reply replaces what is queued, so the SPU thread finds it first.
                    cpu.channels().replaceInMbox(kCellEbusy);
                    return;
                } else if (bound == record->ports.end()) {
                    result = kCellEnotconn;
                } else {
                    const std::uint64_t data2 = (std::uint64_t{port} << 32) | (value & 0xFFFFFF);
                    const std::uint32_t sent =
                            sendEventToQueue(bound->second, kSpuThreadEventUserKey, cpu.id(), data2, d1);
                    queue_full = sent == kCellEbusy;
                    result = sent == kCellEsrch ? kCellEnotconn : sent;
                }
            } else {
                const std::uint32_t bit = value & 0xFFFFFF;
                result = bit >= 64 ? kCellEinval : eventFlagSet(d1, std::uint64_t{1} << bit);
            }
            if (!queue_full) {
                // Throw and impatient set-bit calls drop their errors.
                if (send_event || code == 128) cpu.channels().pushInMbox(result);
                return;
            }
        }
        // Not under the guest lock: the wait ends early when the thread is halted.
        if (!cpu.channels().pause(kQueueRetry)) return;
    }
}

// Puts the reply of a completed receive into the inbound mailbox, replacing what it holds: the status and,
// when it is OK, the three data words; otherwise the status alone.
void replyReceive(SpuThread& cpu, const SpuQueueWaiter& waiter) {
    if (waiter.code == kCellOk) {
        const std::uint32_t words[] = {kCellOk, waiter.data[0], waiter.data[1], waiter.data[2]};
        cpu.channels().replaceInMbox(words, 4);
    } else {
        cpu.channels().replaceInMbox(waiter.code);
    }
}

// stop 0x110 (sys_spu_thread_receive_event) and 0x111 (sys_spu_thread_tryreceive_event): the queue is picked
// from the thread's bindings by the key in the outbound mailbox, and the answer comes through the inbound
// mailbox. Only the calling SPU thread waits, where lv2 suspends the whole group; the guest lock is not held
// while it waits, so terminate and halts still reach it.
bool receiveEventBody(SpuThread& cpu, std::uint32_t site, std::uint32_t code) {
    const bool block = code == kStopReceiveEvent;
    std::uint32_t key = 0;
    // The key is always consumed; lv2 leaves it in the mailbox on some paths so that a stopped thread can retry
    // the instruction, which nothing here does.
    if (!cpu.channels().takeOutMbox(&key)) {
        spuStop(cpu, std::format("tox2: stop: spu: stop {:#x} at ls {:#07x} on thread {:#010x}: the outbound mailbox "
                                 "is empty, there is no queue key to receive on",
                                 code, site, cpu.id()));
    }
    if (cpu.channels().inMboxCount() != 0) {
        cpu.channels().replaceInMbox(kCellEbusy);
        return true;
    }

    SpuQueueWaiter waiter;
    waiter.thread = &cpu;
    SpuQueueResult result = SpuQueueResult::Invalid;
    {
        std::lock_guard<GuestLock> guard(guestLock());
        const SpuGroup* group = findGroupOfThread(cpu.id());
        const SpuThreadRecord* record = findSpuThread(cpu.id());
        if (group == nullptr || record == nullptr) return false;  // the group is gone (test teardown)
        // Only the blocking call needs the scheduler context that the non-context group types lack.
        const bool allowed = !block || (group->type & kGroupTypeNoScheduler) == 0;
        // A destroyed queue is skipped like an unbound key.
        const auto bound = std::ranges::find_if(record->bindings, [key](const QueueBinding& b) {
            return b.queue != 0 && b.key == key && eventQueueExists(b.queue);
        });
        if (allowed && bound != record->bindings.end()) result = spuQueueReceive(bound->queue, waiter, block);
        if (firstUse("spu:receive-event")) {
            const char* outcome = result == SpuQueueResult::Received  ? "an event"
                                  : result == SpuQueueResult::Waiting ? "waits"
                                  : result == SpuQueueResult::Empty   ? "no event"
                                                                      : "no usable queue";
            logInfo("tox2: spu: thread {:#010x} first stop {:#x} at ls {:#07x}: key {:#x}: {}", cpu.id(), code, site,
                    key, outcome);
        }
        if (result == SpuQueueResult::Received) replyReceive(cpu, waiter);
    }

    switch (result) {
        case SpuQueueResult::Received: return true;
        case SpuQueueResult::Empty: cpu.channels().replaceInMbox(kCellEbusy); return true;
        case SpuQueueResult::Invalid: cpu.channels().replaceInMbox(kCellEinval); return true;
        case SpuQueueResult::Waiting: break;
    }
    cpu.channels().waitForOwner("stop 0x110 sys_spu_thread_receive_event", waiter.done);
    // `done` changes only under the guest lock, so it is settled once the lock is held.
    std::lock_guard<GuestLock> guard(guestLock());
    if (!waiter.done.load()) {
        spuQueueCancelWait(waiter);  // the queue must not keep a pointer to this frame
        return false;
    }
    replyReceive(cpu, waiter);
    return true;
}

// Times the blocking receive only: a 0x111 poll would break the one-event-per-cycle meaning of the busy time.
bool receiveEvent(SpuThread& cpu, std::uint32_t site, std::uint32_t code) {
    if (code != kStopReceiveEvent) return receiveEventBody(cpu, site, code);
    const auto arrival = std::chrono::steady_clock::now();
    cpu.noteReceiveArrival(arrival);
    const bool replied = receiveEventBody(cpu, site, code);
    if (replied) {
        cpu.noteReceiveReply(arrival, std::chrono::steady_clock::now());
    } else {
        cpu.noteReceiveAbort();
    }
    return replied;
}

bool GroupOwner::onStop(SpuThread& cpu, std::uint32_t site, std::uint32_t code) {
    if (code == kStopYield) return true;
    if (code == kStopReceiveEvent || code == kStopTryReceiveEvent) return receiveEvent(cpu, site, code);
    if (code != kStopGroupExit && code != kStopThreadExit) {
        spuStop(cpu, std::format("tox2: stop: spu: stop {:#x} at ls {:#07x} on thread {:#010x}: not implemented", code,
                                 site, cpu.id()));
    }
    std::uint32_t status = 0;
    if (!cpu.channels().takeOutMbox(&status)) {
        logWarn("tox2: spu: thread {:#010x} stop {:#x} at ls {:#07x} with an empty outbound mailbox, status 0",
                cpu.id(), code, site);
    }
    std::lock_guard<GuestLock> guard(guestLock());
    SpuGroup* group = findGroupOfThread(cpu.id());
    if (group == nullptr || findSpuThread(cpu.id()) == nullptr) return false;
    if (code == kStopThreadExit) {
        group->last_exit_status = status;
    } else if (!group->terminated && !group->group_exit) {
        group->group_exit = true;
        group->exit_status = status;
        for (SpuThreadRecord& other : group->threads) {
            if (other.cpu != nullptr && other.cpu.get() != &cpu) other.cpu->requestHalt();
        }
    }
    return false;
}

void GroupOwner::onFinished(SpuThread& cpu, SpuLeave how) {
    (void)how;  // the cause comes from the group flags, not from how the host thread left
    std::lock_guard<GuestLock> guard(guestLock());
    SpuGroup* group = findGroupOfThread(cpu.id());
    if (group == nullptr || findSpuThread(cpu.id()) == nullptr || group->state != GroupState::Running) return;
    if (--group->unfinished == 0) stopGroup(*group);
}

// Halts every thread of `group` and waits for the host threads, without freeing anything: an SPU thread that is
// still in onStop walks the other threads' records, so those must stay intact until all host threads have
// ended. For teardown only: a thread that is still running may be waiting for the guest lock, so the caller
// must not hold it unless every group has stopped.
void haltAndJoin(SpuGroup& group) {
    for (SpuThreadRecord& thread : group.threads) {
        if (thread.cpu != nullptr) thread.cpu->requestHalt();
    }
    for (SpuThreadRecord& thread : group.threads) {
        if (thread.cpu != nullptr) thread.cpu->join();
    }
}

// The thread record for the calls that reach into a started SPU thread: sets `*error` to ESRCH for an
// unknown id and to ESTAT when the thread was never started.
SpuThreadRecord* startedThread(std::uint32_t id, std::uint32_t* error) {
    SpuThreadRecord* thread = findSpuThread(id);
    if (thread == nullptr) {
        *error = kCellEsrch;
    } else if (thread->cpu == nullptr) {
        *error = kCellEstat;
    }
    return *error == kCellOk ? thread : nullptr;
}

}  // namespace

void resetSpuThreadGroups() {
    // Every host thread ends before any record is freed.
    for (auto& group : g_groups) {
        if (group) haltAndJoin(*group);
    }
    for (auto& group : g_groups) group.reset();
    g_next_group_slot = 0;
    g_next_thread_number = 1;
}

std::uint32_t spuImageImport(std::uint8_t* base, std::uint32_t image, std::uint32_t source, std::uint32_t type) {
    if (type > 1) return kCellEinval;
    if (image == 0 || source == 0) return kCellEfault;
    if (type == 0 && firstUse("spu:image-protect")) {
        logInfo("tox2: spu: sys_spu_image_import with SYS_SPU_IMAGE_PROTECT is handled like DIRECT");
    }
    if (!process().memory.isCommitted(image, 16) || !process().memory.isCommitted(source, kElfHeaderSize)) {
        return kCellEfault;
    }

    // Big-endian ELF32 for the SPU: magic, class 1, data 2, machine 23.
    if (tox2Read32(base, source) != 0x7F454C46 || tox2Read8(base, source + 4) != 1 ||
        tox2Read8(base, source + 5) != 2 || tox2Read16(base, source + 0x12) != kSpuMachine) {
        return kCellEnoexec;
    }
    const std::uint32_t entry = tox2Read32(base, source + 0x18);
    const std::uint32_t phoff = tox2Read32(base, source + 0x1C);
    const std::uint32_t phentsize = tox2Read16(base, source + 0x2A);
    const std::uint32_t phnum = tox2Read16(base, source + 0x2C);
    if (phnum == 0 || phentsize < kProgramHeaderSize ||
        !process().memory.isCommitted(source + phoff, phentsize * phnum)) {
        return kCellEnoexec;
    }

    std::vector<SpuSegment> segments;
    for (std::uint32_t i = 0; i < phnum; i++) {
        const std::uint32_t ph = source + phoff + i * phentsize;
        if (tox2Read32(base, ph) != kPtLoad) continue;
        const std::uint32_t offset = tox2Read32(base, ph + 4);
        const std::uint32_t vaddr = tox2Read32(base, ph + 8);
        const std::uint32_t filesz = tox2Read32(base, ph + 16);
        const std::uint32_t memsz = tox2Read32(base, ph + 20);
        if (filesz > 0) {
            segments.push_back({kSegmentCopy, vaddr, static_cast<std::int32_t>(filesz), source + offset});
        }
        if (memsz > filesz) {
            segments.push_back({kSegmentFill, vaddr + filesz, static_cast<std::int32_t>(memsz - filesz), 0});
        }
    }
    // A PT_LOAD that is empty in file and memory produces no segment; such a file loads nothing.
    if (segments.empty()) return kCellEnoexec;

    const std::uint32_t array =
            process().main.allocate(kSpuSegmentSize * static_cast<std::uint32_t>(segments.size()), 16);
    if (array == 0) return kCellEnomem;
    for (std::size_t i = 0; i < segments.size(); i++) {
        const std::uint32_t at = array + static_cast<std::uint32_t>(i) * kSpuSegmentSize;
        tox2Write32(base, at, static_cast<std::uint32_t>(segments[i].type));
        tox2Write32(base, at + 4, segments[i].ls_start);
        tox2Write32(base, at + 8, static_cast<std::uint32_t>(segments[i].size));
        tox2Write32(base, at + 12, segments[i].addr_or_value);
    }

    const auto count = static_cast<std::int32_t>(segments.size());
    tox2Write32(base, image, 0);
    tox2Write32(base, image + 4, entry);
    tox2Write32(base, image + 8, array);
    tox2Write32(base, image + 12, static_cast<std::uint32_t>(count));
    g_images[array] = SpuImageRecord{source, type, entry, array, count};
    logInfo("tox2: spu: image import {:#010x} type {}: entry {:#x}, {} segments", source, type, entry, count);
    return kCellOk;
}

std::uint32_t spuImageClose(std::uint8_t* base, std::uint32_t image) {
    if (image == 0 || !process().memory.isCommitted(image, 16)) return kCellEfault;
    const std::uint32_t segs = tox2Read32(base, image + 8);
    const auto it = g_images.find(segs);
    if (it == g_images.end()) return kCellEsrch;
    process().main.free(segs);
    g_images.erase(it);
    return kCellOk;
}

const SpuImageRecord* findSpuImage(std::uint32_t segs) {
    const auto it = g_images.find(segs);
    return it == g_images.end() ? nullptr : &it->second;
}

void resetSpuImages() {
    for (const auto& [segs, record] : g_images) process().main.free(segs);
    g_images.clear();
}

SpuThread* spuThreadById(std::uint32_t id) {
    SpuThreadRecord* thread = findSpuThread(id);
    return thread != nullptr ? thread->cpu.get() : nullptr;
}

bool spuGroupHasJoiner(std::uint32_t group_id) {
    const SpuGroup* group = findGroup(group_id);
    return group != nullptr && group->joining;
}

// 169 sys_spu_initialize(u32 max_usable_spu, u32 max_raw_spu): the usable count is pinned to 6 and
// only more than 5 raw SPUs is rejected (the busy check cannot fail without SPU threads).
void sysSpuInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, static_cast<std::uint32_t>(ctx->gpr[4]) > 5 ? kCellEinval : kCellOk);
}

// 170 sys_spu_thread_group_create(u32* id, u32 num, s32 prio, attr*), attr = {u32 nsize, u32 name,
// s32 type, u32 container}. The container word is only read for type 4: callers that ask for another
// type leave it uninitialised.
void sysSpuThreadGroupCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t id_addr = arg32(ctx, 3);
    const std::uint32_t num = arg32(ctx, 4);
    const auto priority = static_cast<std::int32_t>(ctx->gpr[5]);
    const std::uint32_t attr = arg32(ctx, 6);
    const Process& p = process();
    if (id_addr == 0 || attr == 0 || !p.memory.isCommitted(id_addr, 4) || !p.memory.isCommitted(attr, 12)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::uint32_t name_size = tox2Read32(base, attr);
    const std::uint32_t name_addr = tox2Read32(base, attr + 4);
    const std::uint32_t type = tox2Read32(base, attr + 8);
    if (num < 1 || num > kMaxThreadsPerGroup || name_size > kMaxNameSize) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (type != kGroupTypeNormal && type != kGroupTypeFromContainer && type != kGroupTypeNonContext &&
        type != kGroupTypeSystem) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if ((type & kGroupTypeNoScheduler) == 0 && (priority < kMinPriority || priority > kMaxPriority)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (type == kGroupTypeFromContainer) {
        if (!p.memory.isCommitted(attr + 12, 4)) {
            setResultCode(ctx, kCellEfault);
            return;
        }
        if (!memoryContainerExists(tox2Read32(base, attr + 12))) {
            setResultCode(ctx, kCellEsrch);
            return;
        }
    }
    const std::optional<std::string> name = readName(base, name_addr, name_size);
    if (!name) {
        setResultCode(ctx, kCellEfault);
        return;
    }

    // Rotate the start slot so a destroyed id is not reused immediately.
    for (std::uint32_t i = 0; i < kGroupCount; i++) {
        const std::uint32_t slot = (g_next_group_slot + i) % kGroupCount;
        if (g_groups[slot]) continue;
        g_next_group_slot = (slot + 1) % kGroupCount;
        SpuGroup& group = g_groups[slot].emplace();
        group.id = kGroupIdBase + slot * kGroupIdStep;
        group.name = *name;
        group.num = num;
        group.priority = priority;
        group.type = type;
        group.threads.resize(num);
        tox2Write32(base, id_addr, group.id);
        logInfo("tox2: spu: group {:#010x} \"{}\" created: {} threads, priority {}, type {:#x}", group.id, group.name,
                num, priority, type);
        setResultCode(ctx, kCellOk);
        return;
    }
    setResultCode(ctx, kCellEagain);
}

// 171 sys_spu_thread_group_destroy(u32 id): drops the group with its threads and connections. A running
// group, or one that a PPU thread waits to join, is busy.
void sysSpuThreadGroupDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    const SpuGroup* group = findGroup(id);
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->state == GroupState::Running || group->waiters != 0) {
        setResultCode(ctx, kCellEbusy);
        return;
    }
    g_groups[(id - kGroupIdBase) / kGroupIdStep].reset();
    setResultCode(ctx, kCellOk);
}

// Error returns of the thread and group calls are logged: libraries treat most of them as fatal or ignore
// them, so a failed call otherwise shows up only later as a thread that never runs.
static void rejectSpuCall(Tox2PpuContext* ctx, const char* call, std::uint32_t code, const char* why) {
    logWarn("tox2: spu: {} -> {:#x}: {}", call, code, why);
    setResultCode(ctx, code);
}

// 172 sys_spu_thread_initialize(u32* thread, u32 group, u32 index, sys_spu_image* img, attr*, arg*),
// attr = {u32 name, u32 nsize, u32 option}, arg = four u64. The image struct and its segment list are
// read from guest memory; the import record only supplies the ELF's address for the logs.
void sysSpuThreadInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t thread_addr = arg32(ctx, 3);
    const std::uint32_t group_id = arg32(ctx, 4);
    const std::uint32_t index = arg32(ctx, 5);
    const std::uint32_t image = arg32(ctx, 6);
    const std::uint32_t attr = arg32(ctx, 7);
    const std::uint32_t arg_addr = arg32(ctx, 8);
    const Process& p = process();
    if (thread_addr == 0 || image == 0 || attr == 0 || arg_addr == 0 || !p.memory.isCommitted(thread_addr, 4) ||
        !p.memory.isCommitted(image, 16) || !p.memory.isCommitted(attr, 12) || !p.memory.isCommitted(arg_addr, 32)) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEfault, "bad or uncommitted pointer argument");
        return;
    }
    SpuGroup* group = findGroup(group_id);
    if (group == nullptr) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEsrch, "unknown group");
        return;
    }
    const std::uint32_t name_addr = tox2Read32(base, attr);
    const std::uint32_t name_size = tox2Read32(base, attr + 4);
    const std::uint32_t option = tox2Read32(base, attr + 8);
    // The game's sound engine creates a one-thread group and initialises SPU number 1 in it.
    if (index >= kMaxSpuNumber || name_size > kMaxNameSize) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEinval, "SPU number or name size out of range");
        return;
    }
    const auto taken = [index](const SpuThreadRecord& t) { return t.initialized && t.spu_num == index; };
    if (group->state != GroupState::NotInitialized || std::ranges::any_of(group->threads, taken)) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEbusy,
                      "group not NotInitialized or thread already initialized");
        return;
    }

    const std::uint32_t entry = tox2Read32(base, image + 4);
    const std::uint32_t segs = tox2Read32(base, image + 8);
    const auto nsegs = static_cast<std::int32_t>(tox2Read32(base, image + 12));
    if (entry > kMaxEntryPoint || nsegs < 1 || nsegs > kMaxSegments) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEinval, "entry or segment count out of range");
        return;
    }
    if (segs == 0 || !p.memory.isCommitted(segs, kSpuSegmentSize * static_cast<std::uint32_t>(nsegs))) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEfault, "segment list not committed");
        return;
    }
    std::vector<SpuSegment> segments;
    for (std::int32_t i = 0; i < nsegs; i++) {
        const std::uint32_t at = segs + static_cast<std::uint32_t>(i) * kSpuSegmentSize;
        const SpuSegment segment{static_cast<std::int32_t>(tox2Read32(base, at)), tox2Read32(base, at + 4),
                                 static_cast<std::int32_t>(tox2Read32(base, at + 8)), tox2Read32(base, at + 12)};
        const bool known = segment.type == kSegmentCopy || segment.type == kSegmentFill || segment.type == kSegmentInfo;
        if (!known || segment.size < 0 ||
            std::uint64_t{segment.ls_start} + static_cast<std::uint64_t>(segment.size) > kLocalStoreSize) {
            rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEinval, "unknown segment type or segment outside LS");
            return;
        }
        segments.push_back(segment);
    }
    const std::optional<std::string> name = readName(base, name_addr, name_size);
    if (!name) {
        rejectSpuCall(ctx, "sys_spu_thread_initialize", kCellEfault, "name not readable");
        return;
    }

    // Not full: the group is NotInitialized, so fewer than num records are initialised.
    SpuThreadRecord& thread =
            *std::ranges::find_if(group->threads, [](const SpuThreadRecord& t) { return !t.initialized; });
    thread.initialized = true;
    thread.spu_num = index;
    thread.id = (g_next_thread_number++ << 24) | (group->id & 0x00FFFFFF);
    thread.name = *name;
    thread.option = option;
    for (std::uint32_t i = 0; i < 4; i++) thread.args[i] = tox2Read64(base, arg_addr + i * 8);
    thread.entry = entry;
    const SpuImageRecord* record = findSpuImage(segs);
    thread.source = record != nullptr ? record->source : 0;
    thread.segments = std::move(segments);
    tox2Write32(base, thread_addr, thread.id);
    // The arguments are logged, not checked: callers leave some of them uninitialised.
    logInfo("tox2: spu: thread {:#010x} \"{}\" initialized: group {:#010x} index {}, image {} entry {:#x}, args {:#x} "
            "{:#x} {:#x} {:#x}",
            thread.id, thread.name, group->id, index, sourceText(thread.source), entry, thread.args[0], thread.args[1],
            thread.args[2], thread.args[3]);

    const auto ready = std::ranges::count_if(group->threads, [](const SpuThreadRecord& t) { return t.initialized; });
    if (static_cast<std::uint32_t>(ready) == group->num) group->state = GroupState::Initialized;
    setResultCode(ctx, kCellOk);
}

namespace {

// A new SPU thread for `thread` with its segments loaded and the start registers set (SPU ABI of
// sys_spu_thread_initialize: $3..$6 hold the four arguments as 64-bit values in the preferred slot).
std::unique_ptr<SpuThread> makeSpuThread(const SpuThreadRecord& thread, std::uint8_t* base) {
    auto cpu = std::make_unique<SpuThread>(thread.id, thread.name, &g_owner);
    std::memset(cpu->ls(), 0, kTox2SpuLsSize);
    untagLs(*cpu, 0, kTox2SpuLsSize);
    for (const SpuSegment& segment : thread.segments) {
        const auto size = static_cast<std::uint32_t>(segment.size);
        if (segment.type == kSegmentCopy) {
            copyToLs(*cpu, base, segment.addr_or_value, segment.ls_start, size);
        } else if (segment.type == kSegmentFill) {
            // The value is repeated as a big-endian word.
            for (std::uint32_t i = 0; i < size; i++) {
                cpu->ls()[segment.ls_start + i] =
                        static_cast<std::uint8_t>(segment.addr_or_value >> (24 - 8 * (i & 3)));
            }
            untagLs(*cpu, segment.ls_start, size);
        }
    }
    for (std::uint32_t i = 0; i < 4; i++) TOX2_VR_D(cpu->context().gpr[3 + i], 0) = thread.args[i];
    cpu->channels().mfc().setGuestBase(base);
    return cpu;
}

}  // namespace

// 173 sys_spu_thread_group_start(u32 id): loads every thread, then lets the host threads run. The group
// is Running until the last of them ends (see stopGroup).
void sysSpuThreadGroupStart(Tox2PpuContext* ctx, std::uint8_t* base) {
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        rejectSpuCall(ctx, "sys_spu_thread_group_start", kCellEsrch, "unknown group");
        return;
    }
    if (group->state != GroupState::Initialized) {
        rejectSpuCall(ctx, "sys_spu_thread_group_start", kCellEstat, "group not Initialized");
        return;
    }
    // Nothing has changed yet when a segment source is not guest memory.
    for (const SpuThreadRecord& thread : group->threads) {
        for (const SpuSegment& segment : thread.segments) {
            if (segment.type == kSegmentCopy && segment.size > 0 &&
                !process().memory.isCommitted(segment.addr_or_value, static_cast<std::uint32_t>(segment.size))) {
                rejectSpuCall(ctx, "sys_spu_thread_group_start", kCellEfault, "segment source not committed");
                return;
            }
        }
    }
    logInfo("tox2: spu: start group {:#010x} \"{}\" type {:#x} priority {}: {} threads", group->id, group->name,
            group->type, group->priority, group->num);
    for (std::uint32_t i = 0; i < group->num; i++) {
        const SpuThreadRecord& thread = group->threads[i];
        logInfo("tox2: spu:   thread {} {:#010x} \"{}\" image {} entry {:#x} segments {} args {:#x} {:#x} {:#x} {:#x}",
                thread.spu_num, thread.id, thread.name, sourceText(thread.source), thread.entry, thread.segments.size(),
                thread.args[0], thread.args[1], thread.args[2], thread.args[3]);
        for (const SpuSegment& segment : thread.segments) {
            logInfo("tox2: spu:     segment {} ls {:#x} size {:#x} {} {:#x}", segmentKind(segment.type),
                    segment.ls_start, segment.size, segment.type == kSegmentFill ? "value" : "ea",
                    segment.addr_or_value);
        }
    }
    for (const auto& [type, queue] : group->connections) {
        logInfo("tox2: spu:   event queue {} {:#010x}", eventKind(type), queue);
    }
    for (std::uint32_t i = 0; i < group->num; i++) {
        for (const auto& [port, queue] : group->threads[i].ports) {
            logInfo("tox2: spu:   thread {} port {} -> queue {:#010x}", group->threads[i].spu_num, port, queue);
        }
        for (const QueueBinding& binding : group->threads[i].bindings) {
            if (binding.queue != 0) {
                logInfo("tox2: spu:   thread {} key {:#x} -> queue {:#010x}", group->threads[i].spu_num, binding.key,
                        binding.queue);
            }
        }
    }

    // The previous run's threads have all ended (the group was Initialized), so replacing them only
    // joins host threads that are about to return.
    for (SpuThreadRecord& thread : group->threads) thread.cpu = makeSpuThread(thread, base);
    group->unfinished = group->num;
    group->terminated = false;
    group->group_exit = false;
    group->exit_status = 0;
    group->last_exit_status = 0;
    group->result.reset();
    group->generation++;
    group->state = GroupState::Running;

    const auto run = group->connections.find(kEventRun);
    if (run != group->connections.end()) {
        const std::uint32_t sent = sendEventToQueue(run->second, kSpuGroupEventRunKey, group->id, 0, 0);
        if (sent != kCellOk) {
            logWarn("tox2: spu: run event of group {:#010x} not delivered to queue {:#010x}: {:#x}", group->id,
                    run->second, sent);
        }
    }
    for (SpuThreadRecord& thread : group->threads) {
        const std::optional<SpuTracked> tracked = trackedImageAt(*thread.cpu, thread.entry);
        logInfo("tox2: spu: thread {:#010x} \"{}\" started at {} ls {:#07x}", thread.id, thread.name,
                tracked ? tracked->image->name : "untracked", thread.entry);
    }
    // Started only after every line is out: a thread may end the process at once.
    for (SpuThreadRecord& thread : group->threads) thread.cpu->start(thread.entry);
    setResultCode(ctx, kCellOk);
}

// 177 sys_spu_thread_group_terminate(u32 id, s32 value): halts the threads and waits until they have
// ended. The group stops with cause 4 and `value` as the status, unless a thread had already ended it.
void sysSpuThreadGroupTerminate(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->state != GroupState::Running) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    if (!group->terminated && !group->group_exit) {
        group->terminated = true;
        group->exit_status = arg32(ctx, 4);
    }
    for (SpuThreadRecord& thread : group->threads) {
        if (thread.cpu != nullptr) thread.cpu->requestHalt();
    }
    group->waiters++;
    // A run that was restarted meanwhile is not waited for.
    const std::uint32_t generation = group->generation;
    blockingWait(
            group->changed,
            [group, generation] { return group->generation != generation || group->state != GroupState::Running; },
            "sys_spu_thread_group_terminate", std::nullopt);
    group->waiters--;
    setResultCode(ctx, kCellOk);
}

// 178 sys_spu_thread_group_join(u32 id, u32* cause, u32* status): waits for the end of a run, also of one
// that has not started yet, and takes its result. A run that ended earlier is reported at once.
void sysSpuThreadGroupJoin(Tox2PpuContext* ctx, std::uint8_t* base) {
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->state == GroupState::NotInitialized) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    if (group->joining) {
        setResultCode(ctx, kCellEbusy);
        return;
    }
    JoinResult result;
    if (group->result) {
        result = *group->result;
        group->result.reset();
    } else {
        // The end of the run hands its result to the waiting joiner directly, so a start that follows it
        // before this thread runs again cannot discard it.
        group->joining = true;
        group->waiters++;
        blockingWait(
                group->changed, [group] { return group->handoff.has_value(); }, "sys_spu_thread_group_join",
                std::nullopt);
        result = *group->handoff;
        group->handoff.reset();
        group->joining = false;
        group->waiters--;
    }

    // A null pointer is an error after the wait, not before it (SPURS joins with a null cause).
    const std::uint32_t cause_addr = arg32(ctx, 4), status_addr = arg32(ctx, 5);
    const bool cause_ok = cause_addr != 0 && process().memory.isCommitted(cause_addr, 4);
    const bool status_ok = status_addr != 0 && process().memory.isCommitted(status_addr, 4);
    if (cause_ok) tox2Write32(base, cause_addr, result.cause);
    if (status_ok) tox2Write32(base, status_addr, result.status);
    setResultCode(ctx, cause_ok && status_ok ? kCellOk : kCellEfault);
}

namespace {

// sys_spu_thread_write_ls and sys_spu_thread_read_ls: `type` is the size in bytes (1, 2, 4 or 8), the
// access is aligned and big-endian.
bool validLsAccess(std::uint32_t lsa, std::uint32_t type) {
    return (type == 1 || type == 2 || type == 4 || type == 8) && lsa % type == 0 && lsa < kLocalStoreSize &&
           kLocalStoreSize - lsa >= type;
}

}  // namespace

// 181 sys_spu_thread_write_ls(u32 id, u32 lsa, u64 value, u32 type)
void sysSpuThreadWriteLs(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t lsa = arg32(ctx, 4), type = arg32(ctx, 6);
    if (!validLsAccess(lsa, type)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    std::uint32_t error = kCellOk;
    SpuThreadRecord* thread = startedThread(arg32(ctx, 3), &error);
    if (thread == nullptr) {
        setResultCode(ctx, error);
        return;
    }
    const std::uint64_t value = ctx->gpr[5];
    for (std::uint32_t i = 0; i < type; i++) {
        thread->cpu->ls()[lsa + i] = static_cast<std::uint8_t>(value >> (8 * (type - 1 - i)));
    }
    untagLs(*thread->cpu, lsa, type);
    setResultCode(ctx, kCellOk);
}

// 182 sys_spu_thread_read_ls(u32 id, u32 lsa, u64* value, u32 type)
void sysSpuThreadReadLs(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t lsa = arg32(ctx, 4), value_addr = arg32(ctx, 5), type = arg32(ctx, 6);
    if (value_addr == 0 || !process().memory.isCommitted(value_addr, 8)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    if (!validLsAccess(lsa, type)) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    std::uint32_t error = kCellOk;
    SpuThreadRecord* thread = startedThread(arg32(ctx, 3), &error);
    if (thread == nullptr) {
        setResultCode(ctx, error);
        return;
    }
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < type; i++) value = (value << 8) | thread->cpu->ls()[lsa + i];
    tox2Write64(base, value_addr, value);
    setResultCode(ctx, kCellOk);
}

// 190 sys_spu_thread_write_spu_mb(u32 id, u32 value): a write to a full inbound mailbox replaces the
// newest entry, as the hardware does.
void sysSpuThreadWriteSpuMb(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    std::uint32_t error = kCellOk;
    SpuThreadRecord* thread = startedThread(arg32(ctx, 3), &error);
    if (thread == nullptr) {
        setResultCode(ctx, error);
        return;
    }
    if (!thread->cpu->channels().pushInMbox(arg32(ctx, 4), true) && !thread->mailbox_warned) {
        thread->mailbox_warned = true;
        logWarn("tox2: spu: thread {:#010x} inbound mailbox is full, the newest entry was overwritten", thread->id);
    }
    setResultCode(ctx, kCellOk);
}

// 185 sys_spu_thread_group_connect_event(u32 id, u32 eq, u32 et)
void sysSpuThreadGroupConnectEvent(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t type = arg32(ctx, 5);
    if (type != kEventRun && type != kEventException && type != kEventSystemModule) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuGroup* group = findGroup(arg32(ctx, 3));
    const std::uint32_t queue = arg32(ctx, 4);
    if (group == nullptr || !eventQueueExists(queue)) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->connections.contains(type)) {
        setResultCode(ctx, kCellEbusy);
        return;
    }
    group->connections[type] = queue;
    setResultCode(ctx, kCellOk);
}

// 186 sys_spu_thread_group_disconnect_event(u32 id, u32 et)
void sysSpuThreadGroupDisconnectEvent(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t type = arg32(ctx, 4);
    if (type != kEventRun && type != kEventException && type != kEventSystemModule) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    setResultCode(ctx, group->connections.erase(type) != 0 ? kCellOk : kCellEnotconn);
}

// 191 sys_spu_thread_connect_event(u32 thread, u32 eq, u32 et, u8 spup): binds one port of one thread.
void sysSpuThreadConnectEvent(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t type = arg32(ctx, 5);
    const std::uint32_t port = static_cast<std::uint8_t>(ctx->gpr[6]);
    if (type != kThreadEventUser || port > kMaxPort) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuThreadRecord* thread = findSpuThread(arg32(ctx, 3));
    const std::uint32_t queue = arg32(ctx, 4);
    if (thread == nullptr || !eventQueueExists(queue)) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (thread->ports.contains(port)) {
        setResultCode(ctx, kCellEisconn);
        return;
    }
    thread->ports[port] = queue;
    setResultCode(ctx, kCellOk);
}

// 193 sys_spu_thread_bind_queue(u32 thread, u32 queue, u32 spuq_num): makes the SPU queue receivable by the
// thread under the key `spuq_num`. A thread has 32 slots; a key or a queue can be bound once per thread.
void sysSpuThreadBindQueue(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    SpuThreadRecord* thread = findSpuThread(arg32(ctx, 3));
    const std::uint32_t queue = arg32(ctx, 4);
    const std::uint32_t key = arg32(ctx, 5);
    if (thread == nullptr || !eventQueueExists(queue)) {
        rejectSpuCall(ctx, "sys_spu_thread_bind_queue", kCellEsrch, "unknown thread or event queue");
        return;
    }
    if (eventQueueType(queue) != kSpuQueueType) {
        rejectSpuCall(ctx, "sys_spu_thread_bind_queue", kCellEinval, "not an SPU queue");
        return;
    }
    QueueBinding* free_slot = nullptr;
    for (QueueBinding& binding : thread->bindings) {
        if (binding.queue == 0) {
            if (free_slot == nullptr) free_slot = &binding;
        } else if (binding.key == key || binding.queue == queue) {
            rejectSpuCall(ctx, "sys_spu_thread_bind_queue", kCellEbusy, "key or queue already bound on this thread");
            return;
        }
    }
    if (free_slot == nullptr) {
        rejectSpuCall(ctx, "sys_spu_thread_bind_queue", kCellEagain, "all 32 slots are bound");
        return;
    }
    *free_slot = QueueBinding{key, queue};
    setResultCode(ctx, kCellOk);
}

// 194 sys_spu_thread_unbind_queue(u32 thread, u32 spuq_num): frees the slot of the key. A receive that
// waits on the queue is not woken.
void sysSpuThreadUnbindQueue(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    SpuThreadRecord* thread = findSpuThread(arg32(ctx, 3));
    const std::uint32_t key = arg32(ctx, 4);
    if (thread == nullptr) {
        rejectSpuCall(ctx, "sys_spu_thread_unbind_queue", kCellEsrch, "unknown thread");
        return;
    }
    const auto bound = std::ranges::find_if(thread->bindings,
                                            [key](const QueueBinding& b) { return b.queue != 0 && b.key == key; });
    if (bound == thread->bindings.end()) {
        rejectSpuCall(ctx, "sys_spu_thread_unbind_queue", kCellEsrch, "key not bound");
        return;
    }
    *bound = QueueBinding{};
    setResultCode(ctx, kCellOk);
}

// 192 sys_spu_thread_disconnect_event(u32 thread, u32 et, u8 spup)
void sysSpuThreadDisconnectEvent(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t type = arg32(ctx, 4);
    const std::uint32_t port = static_cast<std::uint8_t>(ctx->gpr[5]);
    if (type != kThreadEventUser || port > kMaxPort) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuThreadRecord* thread = findSpuThread(arg32(ctx, 3));
    if (thread == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    setResultCode(ctx, thread->ports.erase(port) != 0 ? kCellOk : kCellEnotconn);
}

// 250 sys_spu_thread_group_set_cooperative_victims(u32 id, u32 mask): kept, never used.
void sysSpuThreadGroupSetCooperativeVictims(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    group->cooperative_victims = arg32(ctx, 4);
    setResultCode(ctx, kCellOk);
}

// 251 sys_spu_thread_group_connect_event_all_threads(u32 id, u32 eq, u64 req, u8* spup): binds the
// lowest port of `req` that is free on every thread to the queue, on all threads.
void sysSpuThreadGroupConnectEventAllThreads(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t queue = arg32(ctx, 4);
    const std::uint64_t req = ctx->gpr[5];
    const std::uint32_t spup = arg32(ctx, 6);
    if (req == 0) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr || !eventQueueExists(queue)) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (spup == 0 || !process().memory.isCommitted(spup, 1)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    // libsre attaches its queues while the SPURS group runs.
    if (group->state == GroupState::NotInitialized) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    for (std::uint64_t left = req; left != 0; left &= left - 1) {
        const auto port = static_cast<std::uint32_t>(std::countr_zero(left));
        const bool free = std::ranges::none_of(group->threads,
                                               [port](const SpuThreadRecord& t) { return t.ports.contains(port); });
        if (!free) continue;
        for (SpuThreadRecord& thread : group->threads) thread.ports[port] = queue;
        tox2Write8(base, spup, static_cast<std::uint8_t>(port));
        setResultCode(ctx, kCellOk);
        return;
    }
    setResultCode(ctx, kCellEisconn);
}

// 252 sys_spu_thread_group_disconnect_event_all_threads(u32 id, u8 spup)
void sysSpuThreadGroupDisconnectEventAllThreads(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t port = static_cast<std::uint8_t>(ctx->gpr[4]);
    if (port > kMaxPort) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    bool bound = false;
    for (SpuThreadRecord& thread : group->threads) bound = thread.ports.erase(port) != 0 || bound;
    setResultCode(ctx, bound ? kCellOk : kCellEnotconn);
}

}  // namespace tox2

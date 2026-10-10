// lv2 SPU thread groups and images, as bookkeeping (spurs-description section 4.1 to 4.4): groups,
// threads, event connections and the SPU image records are kept and checked, nothing runs. Starting a
// group, or joining one that was never started, ends the run with ExitCode::Unsupported. The SPU image
// import and close functions (sysPrxForUser) keep their records here too. Handlers run under the
// guest lock.

#include "lv2/sys_spu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
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

struct SpuThread {
    bool initialized = false;
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t option = 0;
    std::array<std::uint64_t, 4> args{};
    std::uint32_t entry = 0;
    std::uint32_t source = 0;  // guest address of the imported ELF, 0 when the image is not recorded
    std::vector<SpuSegment> segments;
    std::map<std::uint32_t, std::uint32_t> ports;  // SPU port -> event queue id
};

enum class GroupState { NotInitialized, Initialized };

struct SpuGroup {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t num = 0;
    std::int32_t priority = 0;
    std::uint32_t type = 0;
    GroupState state = GroupState::NotInitialized;
    std::vector<SpuThread> threads;                      // one slot per thread index
    std::map<std::uint32_t, std::uint32_t> connections;  // event type -> event queue id
    std::uint64_t cooperative_victims = 0;
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
SpuThread* findThread(std::uint32_t id) {
    SpuGroup* group = findGroup((kGroupIdBase & 0xFF000000) | (id & 0x00FFFFFF));
    if (group == nullptr) return nullptr;
    for (SpuThread& thread : group->threads) {
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

}  // namespace

void resetSpuThreadGroups() {
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

// 171 sys_spu_thread_group_destroy(u32 id): drops the group with its threads and connections.
void sysSpuThreadGroupDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t id = arg32(ctx, 3);
    if (findGroup(id) == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    g_groups[(id - kGroupIdBase) / kGroupIdStep].reset();
    setResultCode(ctx, kCellOk);
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
        setResultCode(ctx, kCellEfault);
        return;
    }
    SpuGroup* group = findGroup(group_id);
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    const std::uint32_t name_addr = tox2Read32(base, attr);
    const std::uint32_t name_size = tox2Read32(base, attr + 4);
    const std::uint32_t option = tox2Read32(base, attr + 8);
    if (index >= group->num || name_size > kMaxNameSize) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (group->state != GroupState::NotInitialized || group->threads[index].initialized) {
        setResultCode(ctx, kCellEbusy);
        return;
    }

    const std::uint32_t entry = tox2Read32(base, image + 4);
    const std::uint32_t segs = tox2Read32(base, image + 8);
    const auto nsegs = static_cast<std::int32_t>(tox2Read32(base, image + 12));
    if (entry > kMaxEntryPoint || nsegs < 1 || nsegs > kMaxSegments) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (segs == 0 || !p.memory.isCommitted(segs, kSpuSegmentSize * static_cast<std::uint32_t>(nsegs))) {
        setResultCode(ctx, kCellEfault);
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
            setResultCode(ctx, kCellEinval);
            return;
        }
        segments.push_back(segment);
    }
    const std::optional<std::string> name = readName(base, name_addr, name_size);
    if (!name) {
        setResultCode(ctx, kCellEfault);
        return;
    }

    SpuThread& thread = group->threads[index];
    thread.initialized = true;
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

    const auto ready = std::ranges::count_if(group->threads, [](const SpuThread& t) { return t.initialized; });
    if (static_cast<std::uint32_t>(ready) == group->num) group->state = GroupState::Initialized;
    setResultCode(ctx, kCellOk);
}

// 173 sys_spu_thread_group_start(u32 id): there is no SPU execution, so the run ends here after the log
// has shown everything that would have started.
void sysSpuThreadGroupStart(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->state != GroupState::Initialized) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    logInfo("tox2: spu: start group {:#010x} \"{}\" type {:#x} priority {}: {} threads", group->id, group->name,
            group->type, group->priority, group->num);
    for (std::uint32_t i = 0; i < group->num; i++) {
        const SpuThread& thread = group->threads[i];
        logInfo("tox2: spu:   thread {} {:#010x} \"{}\" image {} entry {:#x} segments {} args {:#x} {:#x} {:#x} {:#x}",
                i, thread.id, thread.name, sourceText(thread.source), thread.entry, thread.segments.size(),
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
            logInfo("tox2: spu:   thread {} port {} -> queue {:#010x}", i, port, queue);
        }
    }
    const SpuThread& first = group->threads[0];
    stopProcess(ExitCode::Unsupported,
                std::format("tox2: stop: spu: sys_spu_thread_group_start group {:#010x} \"{}\" ({} threads, image {} "
                            "entry {:#x}): SPU execution is not implemented",
                            group->id, group->name, group->num, sourceText(first.source), first.entry));
}

// 177 sys_spu_thread_group_terminate(u32 id, s32 value): no group ever runs, so none can be stopped.
void sysSpuThreadGroupTerminate(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, findGroup(arg32(ctx, 3)) == nullptr ? kCellEsrch : kCellEstat);
}

// 178 sys_spu_thread_group_join(u32 id, u32* cause, u32* status): the wait of an initialized group
// could never end, so the run stops instead of hanging.
void sysSpuThreadGroupJoin(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const SpuGroup* group = findGroup(arg32(ctx, 3));
    if (group == nullptr) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (group->state != GroupState::Initialized) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    stopProcess(ExitCode::Unsupported,
                std::format("tox2: stop: spu: sys_spu_thread_group_join on group {:#010x} that never runs: SPU "
                            "execution is not implemented",
                            group->id));
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
    SpuThread* thread = findThread(arg32(ctx, 3));
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

// 192 sys_spu_thread_disconnect_event(u32 thread, u32 et, u8 spup)
void sysSpuThreadDisconnectEvent(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t type = arg32(ctx, 4);
    const std::uint32_t port = static_cast<std::uint8_t>(ctx->gpr[5]);
    if (type != kThreadEventUser || port > kMaxPort) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    SpuThread* thread = findThread(arg32(ctx, 3));
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
    if (group->state != GroupState::Initialized) {
        setResultCode(ctx, kCellEstat);
        return;
    }
    for (std::uint64_t left = req; left != 0; left &= left - 1) {
        const auto port = static_cast<std::uint32_t>(std::countr_zero(left));
        const bool free =
                std::ranges::none_of(group->threads, [port](const SpuThread& t) { return t.ports.contains(port); });
        if (!free) continue;
        for (SpuThread& thread : group->threads) thread.ports[port] = queue;
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
    for (SpuThread& thread : group->threads) bound = thread.ports.erase(port) != 0 || bound;
    setResultCode(ctx, bound ? kCellOk : kCellEnotconn);
}

}  // namespace tox2

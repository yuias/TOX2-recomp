// lv2 SPU syscalls the game issues before any SPU thread exists: sys_spu_initialize (169) and
// sys_spu_thread_group_connect_event_all_threads (251). Behaviour follows RPCS3
// `Emu/Cell/lv2/sys_spu.cpp`. No SPU thread is ever created, so the "port already in use" scan of
// RPCS3 never finds a user. The SPU image import and close functions (sysPrxForUser) keep their records
// here too. Handlers run under the guest lock.

#include "lv2/sys_spu.h"

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kGroupIdBase = 0x04000100;
constexpr std::uint32_t kGroupIdStep = 0x100;
constexpr std::uint32_t kGroupCount = 255;

std::array<bool, kGroupCount> g_group_used{};
std::uint32_t g_next_group_slot = 0;

constexpr std::uint32_t kSpuSegmentSize = 16;
constexpr std::int32_t kSegmentCopy = 1;
constexpr std::int32_t kSegmentFill = 2;
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

}  // namespace

std::uint32_t createSpuThreadGroup() {
    // Rotate the start slot so a destroyed id is not reused immediately.
    for (std::uint32_t i = 0; i < kGroupCount; i++) {
        const std::uint32_t slot = (g_next_group_slot + i) % kGroupCount;
        if (!g_group_used[slot]) {
            g_group_used[slot] = true;
            g_next_group_slot = (slot + 1) % kGroupCount;
            return kGroupIdBase + slot * kGroupIdStep;
        }
    }
    return 0;
}

void destroySpuThreadGroup(std::uint32_t id) {
    if (spuThreadGroupExists(id)) g_group_used[(id - kGroupIdBase) / kGroupIdStep] = false;
}

bool spuThreadGroupExists(std::uint32_t id) {
    if (id < kGroupIdBase || (id - kGroupIdBase) % kGroupIdStep != 0) return false;
    const std::uint32_t slot = (id - kGroupIdBase) / kGroupIdStep;
    return slot < kGroupCount && g_group_used[slot];
}

void resetSpuThreadGroups() {
    g_group_used.fill(false);
    g_next_group_slot = 0;
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

// 169 sys_spu_initialize(u32 max_usable_spu, u32 max_raw_spu): RPCS3 pins the usable count to 6 and
// only rejects more than 5 raw SPUs (the busy check cannot fail without SPU threads).
void sysSpuInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, static_cast<std::uint32_t>(ctx->gpr[4]) > 5 ? kCellEinval : kCellOk);
}

// 251 sys_spu_thread_group_connect_event_all_threads(u32 id, u32 eq, u64 req, u8* spup): the group
// must be one created by createSpuThreadGroup (always in the initialized state). The port is the
// lowest bit set in `req`.
void sysSpuThreadGroupConnectEventAllThreads(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto eq = static_cast<std::uint32_t>(ctx->gpr[4]);
    const std::uint64_t req = ctx->gpr[5];
    const auto spup = static_cast<std::uint32_t>(ctx->gpr[6]);
    if (req == 0) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (!spuThreadGroupExists(id) || !eventQueueExists(eq)) {
        setResultCode(ctx, kCellEsrch);
        return;
    }
    if (spup == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write8(base, spup, static_cast<std::uint8_t>(std::countr_zero(req)));
    setResultCode(ctx, kCellOk);
}

}  // namespace tox2

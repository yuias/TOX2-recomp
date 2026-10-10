// HLE handlers of the cellSpurs module, reduced to what a game that creates a SPURS instance and
// never schedules a workload can observe. Argument checks, error codes and struct layouts follow
// RPCS3 `Modules/cellSpurs.{h,cpp}` (`_cellSpursAttributeInitialize`,
// `cellSpursInitializeWithAttribute2` / `_spurs::initialize`, `cellSpursGetSpuThreadGroupId`,
// `cellSpursFinalize`). No SPU thread, semaphore, event queue or PPU helper thread is created; the
// SPU thread group is only an id (lv2/sys_spu.h). Handlers run under the guest lock.

#include <cstdint>
#include <cstring>
#include <format>
#include <map>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellSpurs";

constexpr std::uint32_t kSpursErrorInval = 0x80410702;
constexpr std::uint32_t kSpursErrorAlign = 0x80410710;
constexpr std::uint32_t kSpursErrorNullPointer = 0x80410711;

constexpr std::uint32_t kAttributeAlign = 8;
constexpr std::uint32_t kAttributeSize = 512;
constexpr std::uint32_t kSpursAlign = 128;
constexpr std::uint32_t kSpursSize2 = 0x2000;  // the second-generation object is always initialized
constexpr std::uint32_t kNameMaxLength = 15;

// CellSpursAttribute offsets.
constexpr std::uint32_t kAttrRevision = 0x00;
constexpr std::uint32_t kAttrSdkVersion = 0x04;
constexpr std::uint32_t kAttrNSpus = 0x08;
constexpr std::uint32_t kAttrSpuPriority = 0x0c;
constexpr std::uint32_t kAttrPpuPriority = 0x10;
constexpr std::uint32_t kAttrExitIfNoWork = 0x14;
constexpr std::uint32_t kAttrPrefix = 0x15;
constexpr std::uint32_t kAttrPrefixSize = 0x24;
constexpr std::uint32_t kAttrFlags = 0x28;

// CellSpurs offsets of the fields _spurs::initialize sets.
constexpr std::uint32_t kSpursWklFlag = 0x60 + 0x0c;  // CellSpursWorkloadFlag::flag
constexpr std::uint32_t kSpursFlags1 = 0x74;
constexpr std::uint32_t kSpursNSpus = 0x76;
constexpr std::uint32_t kSpursWklFlagReceiver = 0x77;
constexpr std::uint32_t kSpursSysSrvPreemptWklId = 0xc0;
constexpr std::uint32_t kSpursUnk11 = 0x988;
constexpr std::uint32_t kSpursUnk12 = 0x98c;
constexpr std::uint32_t kSpursWklInfoSysSrvSize = 0xd00 + 0x10;
constexpr std::uint32_t kSpursWklInfoSysSrvUniqueId = 0xd00 + 0x14;
constexpr std::uint32_t kSpursPpu0 = 0xd20;
constexpr std::uint32_t kSpursPpu1 = 0xd28;
constexpr std::uint32_t kSpursSpuTg = 0xd30;
constexpr std::uint32_t kSpursFlags = 0xd80;
constexpr std::uint32_t kSpursSpuPriority = 0xd84;
constexpr std::uint32_t kSpursPpuPriority = 0xd88;
constexpr std::uint32_t kSpursPrefix = 0xd8c;
constexpr std::uint32_t kSpursPrefixSize = 0xd9b;
constexpr std::uint32_t kSpursRevision = 0xda0;
constexpr std::uint32_t kSpursSdkVersion = 0xda4;

// SpursAttrFlags / SpursFlags1 values used here.
constexpr std::uint32_t kAttrFlagExitIfNoWork = 0x00000001;
constexpr std::uint32_t kAttrFlagSecondVersion = 0x00000004;
constexpr std::uint8_t kFlags1ThirtyTwoWorkloads = 0x40;
constexpr std::uint8_t kFlags1ExitIfNoWork = 0x80;

// Initialized SPURS objects (guest address) and their fake thread group ids.
std::map<std::uint32_t, std::uint32_t> g_spurs_groups;

// _cellSpursAttributeInitialize(attr*, revision, sdkVersion, nSpus, spuPriority, ppuPriority,
//                               exitIfNoWork)
void spursAttributeInitialize(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t attr = argU32(ctx, 0);
    if (attr == 0) return setResultCode(ctx, kSpursErrorNullPointer);
    if (attr % kAttributeAlign != 0) return setResultCode(ctx, kSpursErrorAlign);
    if (!process().memory.isCommitted(attr, kAttributeSize)) {
        fatal(FatalKind::GuestFault,
              std::format("_cellSpursAttributeInitialize: attr {:#010x} is not committed", attr));
    }

    std::memset(base + attr, 0, kAttributeSize);
    dalWrite32(base, attr + kAttrRevision, argU32(ctx, 1));
    dalWrite32(base, attr + kAttrSdkVersion, argU32(ctx, 2));
    dalWrite32(base, attr + kAttrNSpus, argU32(ctx, 3));
    dalWrite32(base, attr + kAttrSpuPriority, argU32(ctx, 4));
    dalWrite32(base, attr + kAttrPpuPriority, argU32(ctx, 5));
    dalWrite8(base, attr + kAttrExitIfNoWork, (ctx->gpr[9] & 0xff) != 0 ? 1 : 0);
    setResultCode(ctx, kCellOk);
}

// cellSpursInitializeWithAttribute2(spurs*, const attr*)
void spursInitializeWithAttribute2(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t spurs = argU32(ctx, 0);
    const std::uint32_t attr = argU32(ctx, 1);
    if (attr == 0) return setResultCode(ctx, kSpursErrorNullPointer);
    if (attr % kAttributeAlign != 0) return setResultCode(ctx, kSpursErrorAlign);
    if (!process().memory.isCommitted(attr, kAttributeSize)) {
        fatal(FatalKind::GuestFault,
              std::format("cellSpursInitializeWithAttribute2: attr {:#010x} is not committed", attr));
    }

    const std::uint32_t revision = dalRead32(base, attr + kAttrRevision);
    if (revision > 2) return setResultCode(ctx, kSpursErrorInval);

    // _spurs::initialize
    if (spurs == 0) return setResultCode(ctx, kSpursErrorNullPointer);
    if (spurs % kSpursAlign != 0) return setResultCode(ctx, kSpursErrorAlign);
    const std::uint32_t prefix_size = dalRead32(base, attr + kAttrPrefixSize);
    if (prefix_size > kNameMaxLength) return setResultCode(ctx, kSpursErrorInval);
    if (!process().memory.isCommitted(spurs, kSpursSize2)) {
        fatal(FatalKind::GuestFault,
              std::format("cellSpursInitializeWithAttribute2: spurs {:#010x} is not committed", spurs));
    }

    // Re-initializing an object replaces its group.
    if (const auto old = g_spurs_groups.find(spurs); old != g_spurs_groups.end()) {
        destroySpuThreadGroup(old->second);
        g_spurs_groups.erase(old);
    }
    const std::uint32_t group = createSpuThreadGroup();
    if (group == 0) return setResultCode(ctx, kCellEagain);
    g_spurs_groups[spurs] = group;

    const std::uint32_t exit_if_no_work = (dalRead8(base, attr + kAttrExitIfNoWork) != 0 ? kAttrFlagExitIfNoWork : 0);
    const std::uint32_t flags = dalRead32(base, attr + kAttrFlags) | exit_if_no_work | kAttrFlagSecondVersion;
    const std::uint32_t n_spus = dalRead32(base, attr + kAttrNSpus);

    std::memset(base + spurs, 0, kSpursSize2);
    dalWrite32(base, spurs + kSpursRevision, revision);
    dalWrite32(base, spurs + kSpursSdkVersion, dalRead32(base, attr + kAttrSdkVersion));
    dalWrite64(base, spurs + kSpursPpu0, 0xffffffffull);
    dalWrite64(base, spurs + kSpursPpu1, 0xffffffffull);
    dalWrite32(base, spurs + kSpursFlags, flags);
    dalWrite8(base, spurs + kSpursPrefixSize, static_cast<std::uint8_t>(prefix_size));
    std::memcpy(base + spurs + kSpursPrefix, base + attr + kAttrPrefix, prefix_size);
    for (std::uint32_t i = 0; i < 8; i++) dalWrite8(base, spurs + kSpursSysSrvPreemptWklId + i, 0xff);
    dalWrite32(base, spurs + kSpursWklInfoSysSrvSize, 0x2200);
    dalWrite8(base, spurs + kSpursWklInfoSysSrvUniqueId, 0xff);
    dalWrite32(base, spurs + kSpursUnk11, 0xffffffff);
    dalWrite32(base, spurs + kSpursUnk12, 0xffffffff);
    dalWrite8(base, spurs + kSpursNSpus, static_cast<std::uint8_t>(n_spus));
    dalWrite32(base, spurs + kSpursSpuPriority, dalRead32(base, attr + kAttrSpuPriority));
    dalWrite32(base, spurs + kSpursPpuPriority, dalRead32(base, attr + kAttrPpuPriority));
    dalWrite32(base, spurs + kSpursSpuTg, group);
    dalWrite8(base, spurs + kSpursFlags1,
              ((flags & kAttrFlagExitIfNoWork) != 0 ? kFlags1ExitIfNoWork : 0) | kFlags1ThirtyTwoWorkloads);
    dalWrite8(base, spurs + kSpursWklFlagReceiver, 0xff);
    dalWrite32(base, spurs + kSpursWklFlag, 0xffffffff);

    logOnce(std::format("spurs-init-{:08x}", spurs),
            "dal: spurs: initialized {:#010x} nSpus={} group={:#010x} (no SPU threads run)", spurs, n_spus, group);
    setResultCode(ctx, kCellOk);
}

// cellSpursGetSpuThreadGroupId(spurs*, u32* group)
void spursGetSpuThreadGroupId(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t spurs = argU32(ctx, 0);
    const std::uint32_t group = argU32(ctx, 1);
    if (spurs == 0 || group == 0) return setResultCode(ctx, kSpursErrorNullPointer);
    if (spurs % kSpursAlign != 0) return setResultCode(ctx, kSpursErrorAlign);
    dalWrite32(base, group, dalRead32(base, spurs + kSpursSpuTg));
    setResultCode(ctx, kCellOk);
}

// cellSpursFinalize(spurs*): RPCS3 only validates the pointer (the rest is a todo there).
void spursFinalize(DalPpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t spurs = argU32(ctx, 0);
    if (spurs == 0) return setResultCode(ctx, kSpursErrorNullPointer);
    if (spurs % kSpursAlign != 0) return setResultCode(ctx, kSpursErrorAlign);
    if (const auto it = g_spurs_groups.find(spurs); it != g_spurs_groups.end()) {
        destroySpuThreadGroup(it->second);
        g_spurs_groups.erase(it);
    }
    setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x30aa96c4, "cellSpursInitializeWithAttribute2", spursInitializeWithAttribute2},
        {kModule, 0x39c173fb, "cellSpursGetSpuThreadGroupId", spursGetSpuThreadGroupId},
        {kModule, 0x95180230, "_cellSpursAttributeInitialize", spursAttributeInitialize},
        {kModule, 0xca4c4600, "cellSpursFinalize", spursFinalize},
};

}  // namespace

std::span<const HleEntry> cellSpursHandlers() { return kHandlers; }

void resetSpurs() {
    for (const auto& [spurs, group] : g_spurs_groups) destroySpuThreadGroup(group);
    g_spurs_groups.clear();
}

}  // namespace dal

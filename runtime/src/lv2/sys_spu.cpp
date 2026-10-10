// lv2 SPU syscalls the game issues before any SPU thread exists: sys_spu_initialize (169) and
// sys_spu_thread_group_connect_event_all_threads (251). Behaviour follows RPCS3
// `Emu/Cell/lv2/sys_spu.cpp`. No SPU thread is ever created, so the "port already in use" scan of
// RPCS3 never finds a user. Handlers run under the guest lock.

#include "lv2/sys_spu.h"

#include <array>
#include <bit>
#include <cstdint>

#include "core/guest_memory.h"
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

// Startup syscalls: process exit, tty, timers, time base, user memory and memory containers.
// Behaviour follows RPCS3 `Emu/Cell/lv2/sys_process.cpp`, `sys_tty.cpp`, `sys_timer.cpp`,
// `sys_time.cpp` and `sys_memory.cpp`. Handlers run under the guest lock.

#include "lv2/sys_memory.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <memory>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

// RPCS3 returns lv2 error codes as s32 converted to u64, so the high word is sign-extended.
void setResult(Tox2PpuContext* ctx, std::uint32_t code) {
    ctx->gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

constexpr std::uint32_t kPageSize1m = 0x100000;
constexpr std::uint32_t kPageSize64k = 0x10000;
constexpr std::uint64_t kPageSizeFlag1m = 0x400;
constexpr std::uint64_t kPageSizeFlag64k = 0x200;
constexpr std::int32_t kTtyChannelMax = 15;

// A memory container is a slice of the default container's "physical memory" (RPCS3
// `lv2_memory_container`): creating one charges its size to `Process::user_memory_used`, so
// sys_memory_get_user_memory_size sees it as taken, and `used` counts what was allocated from it.
struct MemoryContainer {
    std::uint32_t size = 0;
    std::uint32_t used = 0;
};
constexpr std::uint32_t kContainerIdBase = 0x3F000000;
constexpr std::size_t kMaxContainers = 16;  // RPCS3's id range for containers
IdMap<MemoryContainer> g_containers(kContainerIdBase, 1);
// Allocations made from a container, so that sys_memory_free credits the right one.
std::map<std::uint32_t, std::uint32_t> g_container_blocks;  // address -> container id

// The argument checks that sys_memory_allocate and sys_memory_allocate_from_container share, in
// RPCS3's order. Returns the page size through `align`.
std::uint32_t checkAllocation(std::uint64_t size, std::uint64_t flags, std::uint32_t& align) {
    if (size == 0) return kCellEalign;
    align = flags == kPageSizeFlag1m    ? kPageSize1m
            : flags == kPageSizeFlag64k ? kPageSize64k
            : flags == 0                ? kPageSize1m
                                        : 0;
    if (align == 0) return kCellEinval;
    return size % align != 0 ? kCellEalign : kCellOk;
}

// Sleeps never get signalled; the timeout is the only way out.
void sleepFor(Tox2PpuContext* ctx, std::uint8_t* base, std::uint64_t microseconds) {
    if (microseconds == 0) {
        tox2Yield(ctx, base);
        return;
    }
    // Keeps the deadline arithmetic of the steady clock far from overflow.
    constexpr std::uint64_t kMaxSleepUs = 24ull * 3600 * 1000000;
    static std::condition_variable_any never_signalled;
    blockingWait(
            never_signalled, [] { return false; }, "sys_timer_usleep",
            std::chrono::microseconds(std::min(microseconds, kMaxSleepUs)));
}

}  // namespace

// 22 _sys_process_exit(s32 status, ...)
void sysProcessExit(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto status = static_cast<std::int32_t>(ctx->gpr[3]);
    stopProcess(ExitCode::ProcessExit, std::format("tox2: stop: process exit status {}", status));
}

// 402 sys_tty_read: RPCS3 has no input outside debug-console mode.
void sysTtyRead(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResult(ctx, kCellEio);
}

// 403 sys_tty_write(s32 ch, const char* buf, u32 len, u32* pwritelen)
void sysTtyWrite(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto channel = static_cast<std::int32_t>(ctx->gpr[3]);
    const auto buf = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto len = static_cast<std::uint32_t>(ctx->gpr[5]);
    const auto pwritelen = static_cast<std::uint32_t>(ctx->gpr[6]);

    // Writes to channels outside 0..15 are dropped but still reported as written.
    if (static_cast<std::int32_t>(len) > 0 && channel >= 0 && channel <= kTtyChannelMax) {
        std::fwrite(base + buf, 1, len, stdout);
        std::fflush(stdout);
    }
    if (pwritelen == 0) {
        setResult(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, pwritelen, len);
    setResult(ctx, kCellOk);
}

// 141 sys_timer_usleep(u64 microseconds)
void sysTimerUsleep(Tox2PpuContext* ctx, std::uint8_t* base) {
    sleepFor(ctx, base, ctx->gpr[3]);
    setResult(ctx, kCellOk);
}

// 142 sys_timer_sleep(u32 seconds)
void sysTimerSleep(Tox2PpuContext* ctx, std::uint8_t* base) {
    sleepFor(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3]) * 1000000ull);
    setResult(ctx, kCellOk);
}

// 147 sys_time_get_timebase_frequency
void sysTimeGetTimebaseFrequency(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    ctx->gpr[3] = kTimebaseFrequency;
}

// 348 sys_memory_allocate(u64 size, u64 flags, u32* alloc_addr). The error order follows RPCS3.
void sysMemoryAllocate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint64_t size = ctx->gpr[3];
    const std::uint64_t flags = ctx->gpr[4];
    const auto alloc_addr = static_cast<std::uint32_t>(ctx->gpr[5]);

    std::uint32_t align = 0;
    if (const std::uint32_t error = checkAllocation(size, flags, align); error != kCellOk) {
        setResult(ctx, error);
        return;
    }

    Process& p = process();
    // The "physical memory" of the default container; the size fits 32 bits once this check holds.
    if (size > p.user_memory_total - p.user_memory_used) {
        setResult(ctx, kCellEnomem);
        return;
    }
    const auto bytes = static_cast<std::uint32_t>(size);
    RegionAllocator& region = align == kPageSize64k ? p.user64k : p.user1m;
    const std::uint32_t addr = region.allocate(bytes, align);
    if (addr == 0) {
        setResult(ctx, kCellEnomem);
        return;
    }
    if (alloc_addr == 0) {
        region.free(addr);
        setResult(ctx, kCellEfault);
        return;
    }
    p.user_memory_used += bytes;
    tox2Write32(base, alloc_addr, addr);
    setResult(ctx, kCellOk);
}

// 349 sys_memory_free(u32 addr)
void sysMemoryFree(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    Process& p = process();

    std::uint32_t freed = 0;
    if (addr % kPageSize64k == 0) {
        if (addr >= kUser1mRegion.begin && addr < kUser1mRegion.end) {
            freed = p.user1m.free(addr);
        } else if (addr >= kUser64kRegion.begin && addr < kUser64kRegion.end) {
            freed = p.user64k.free(addr);
        }
    }
    if (freed == 0) {
        setResult(ctx, kCellEinval);
        return;
    }
    // A block from a container gives its memory back to that container; the container stays
    // charged to the default one until it is destroyed.
    if (const auto owner = g_container_blocks.find(addr); owner != g_container_blocks.end()) {
        if (const auto container = g_containers.find(owner->second); container != nullptr) {
            container->used -= std::min(freed, container->used);
        }
        g_container_blocks.erase(owner);
    } else {
        p.user_memory_used -= std::min(freed, p.user_memory_used);
    }
    setResult(ctx, kCellOk);
}

// 352 sys_memory_get_user_memory_size(sys_memory_info_t* info): {total, available}, BE u32 each, of
// the default container; memory taken by memory containers counts as used.
void sysMemoryGetUserMemorySize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto info = static_cast<std::uint32_t>(ctx->gpr[3]);
    const Process& p = process();
    tox2Write32(base, info, p.user_memory_total);
    tox2Write32(base, info + 4, p.user_memory_total - p.user_memory_used);
    setResult(ctx, kCellOk);
}

// 324 and 341 sys_memory_container_create(u32* cid, u64 size): the size is rounded down to 1 MiB.
void sysMemoryContainerCreate(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto cid_addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    const std::uint64_t size = ctx->gpr[4] & ~std::uint64_t{kPageSize1m - 1};

    Process& p = process();
    if (size == 0 || size > p.user_memory_total - p.user_memory_used) {
        setResult(ctx, kCellEnomem);
        return;
    }
    if (g_containers.size() >= kMaxContainers) {
        setResult(ctx, kCellEagain);
        return;
    }
    if (cid_addr == 0) {
        setResult(ctx, kCellEfault);
        return;
    }
    const auto bytes = static_cast<std::uint32_t>(size);
    auto container = std::make_shared<MemoryContainer>();
    container->size = bytes;
    p.user_memory_used += bytes;
    const std::uint32_t id = g_containers.add(std::move(container));
    tox2Write32(base, cid_addr, id);
    logInfo("tox2: memory: container {:#010x} created, {:#x} bytes (default container has {:#x} left)", id, bytes,
            p.user_memory_total - p.user_memory_used);
    setResult(ctx, kCellOk);
}

// 325 and 342 sys_memory_container_destroy(u32 cid): fails while memory allocated from the
// container is still in use.
void sysMemoryContainerDestroy(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const auto id = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto container = g_containers.find(id);
    if (container == nullptr) {
        setResult(ctx, kCellEsrch);
        return;
    }
    if (container->used != 0) {
        setResult(ctx, kCellEbusy);
        return;
    }
    Process& p = process();
    p.user_memory_used -= std::min(container->size, p.user_memory_used);
    g_containers.erase(id);
    setResult(ctx, kCellOk);
}

// 343 sys_memory_container_get_size(sys_memory_info_t* info, u32 cid): {size, size - used}, BE u32.
void sysMemoryContainerGetSize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto info = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto container = g_containers.find(static_cast<std::uint32_t>(ctx->gpr[4]));
    if (container == nullptr) {
        setResult(ctx, kCellEsrch);
        return;
    }
    if (info == 0) {
        setResult(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, info, container->size);
    tox2Write32(base, info + 4, container->size - container->used);
    setResult(ctx, kCellOk);
}

// 350 sys_memory_allocate_from_container(u64 size, u32 cid, u64 flags, u32* alloc_addr)
void sysMemoryAllocateFromContainer(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint64_t size = ctx->gpr[3];
    const auto cid = static_cast<std::uint32_t>(ctx->gpr[4]);
    const std::uint64_t flags = ctx->gpr[5];
    const auto alloc_addr = static_cast<std::uint32_t>(ctx->gpr[6]);

    std::uint32_t align = 0;
    if (const std::uint32_t error = checkAllocation(size, flags, align); error != kCellOk) {
        setResult(ctx, error);
        return;
    }
    const auto container = g_containers.find(cid);
    if (container == nullptr) {
        setResult(ctx, kCellEsrch);
        return;
    }
    if (size > container->size - container->used) {
        setResult(ctx, kCellEnomem);
        return;
    }
    const auto bytes = static_cast<std::uint32_t>(size);
    Process& p = process();
    RegionAllocator& region = align == kPageSize64k ? p.user64k : p.user1m;
    const std::uint32_t addr = region.allocate(bytes, align);
    if (addr == 0) {
        setResult(ctx, kCellEnomem);
        return;
    }
    if (alloc_addr == 0) {
        region.free(addr);
        setResult(ctx, kCellEfault);
        return;
    }
    container->used += bytes;
    g_container_blocks[addr] = cid;
    tox2Write32(base, alloc_addr, addr);
    setResult(ctx, kCellOk);
}

// 988: not in RPCS3's table. The game issues it as `sc 988(kind, 0)` and ignores the result: kind 1
// at the start of its exit functions, which end with `sys_process_exit`, kind 4 at the start of
// `abort()`, ahead of the tty text "abort() is called from ...". It looks like a notification to a
// debugger or monitor, so it is accepted and ignored.
void sysExitNotification(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResult(ctx, kCellOk);
}

void resetMemoryContainers() {
    g_containers = IdMap<MemoryContainer>(kContainerIdBase, 1);
    g_container_blocks.clear();
}

}  // namespace tox2

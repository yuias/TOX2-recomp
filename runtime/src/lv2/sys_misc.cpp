// Startup syscalls: process exit, tty, timers, time base and user memory. Behaviour follows RPCS3
// `Emu/Cell/lv2/sys_process.cpp`, `sys_tty.cpp`, `sys_timer.cpp`, `sys_time.cpp` and
// `sys_memory.cpp`. Handlers run under the guest lock.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <format>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"

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

    if (size == 0) {
        setResult(ctx, kCellEalign);
        return;
    }
    const std::uint32_t align = flags == kPageSizeFlag1m    ? kPageSize1m
                                : flags == kPageSizeFlag64k ? kPageSize64k
                                : flags == 0                ? kPageSize1m
                                                            : 0;
    if (align == 0) {
        setResult(ctx, kCellEinval);
        return;
    }
    if (size % align != 0) {
        setResult(ctx, kCellEalign);
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
    p.user_memory_used -= std::min(freed, p.user_memory_used);
    setResult(ctx, kCellOk);
}

// 352 sys_memory_get_user_memory_size(sys_memory_info_t* info): {total, available}, BE u32 each.
// There are no memory containers, so the total is the whole default container.
void sysMemoryGetUserMemorySize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto info = static_cast<std::uint32_t>(ctx->gpr[3]);
    const Process& p = process();
    tox2Write32(base, info, p.user_memory_total);
    tox2Write32(base, info + 4, p.user_memory_total - p.user_memory_used);
    setResult(ctx, kCellOk);
}

}  // namespace tox2

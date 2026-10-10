// Main-thread setup: primary stack, argument block, TLS and entry registers, following RPCS3
// `Emu/Cell/PPUModule.cpp` `ppu_load_exec` and `Modules/sys_ppu_thread_.cpp` `sys_initialize_tls`.

#include "core/boot.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kTlsSystemArea = 0x30;
constexpr std::uint32_t kTlsAreaBytes = 0x40000;
// The thread pointer sits this far above the start of the thread's TLS image.
constexpr std::uint32_t kTlsPointerBias = 0x7000;

constexpr std::uint32_t kPrimaryStackMin = 0x10000;
constexpr std::uint32_t kPrimaryStackMax = 0x100000;

constexpr std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Updated under the guest lock.
struct TlsState {
    bool ready = false;
    std::uint32_t image = 0, filesz = 0, zero = 0;
    std::uint32_t block_size = 0;  // per-thread block including the system area
    std::uint32_t area = 0;        // first block (already past the area's own system gap)
    std::vector<bool> used;
};
TlsState g_tls;

// Values 0x10..0x70 select 32K..1M; any other value is clamped and page aligned.
std::uint32_t primaryStackSize(std::uint32_t param) {
    switch (param) {
        case 0x10: return 32 * 1024;
        case 0x20: return 64 * 1024;
        case 0x30: return 96 * 1024;
        case 0x40: return 128 * 1024;
        case 0x50: return 256 * 1024;
        case 0x60: return 512 * 1024;
        case 0x70: return 1024 * 1024;
        default: break;
    }
    return alignUp(std::clamp(param, kPrimaryStackMin, kPrimaryStackMax), 4096);
}

}  // namespace

std::uint32_t allocateTls() {
    if (!g_tls.ready) {
        return 0;
    }
    std::uint8_t* base = process().memory.base();
    std::uint32_t block = 0;
    for (std::uint32_t i = 0; i < g_tls.used.size(); i++) {
        if (!g_tls.used[i]) {
            g_tls.used[i] = true;
            block = g_tls.area + i * g_tls.block_size;
            break;
        }
    }
    if (block == 0) {
        // Out of slots in the shared area: take a separate block from the main region.
        block = process().main.allocate(g_tls.block_size, 0x10000);
        if (block == 0) {
            return 0;
        }
    }
    std::memset(base + block, 0, kTlsSystemArea);
    std::memcpy(base + block + kTlsSystemArea, base + g_tls.image, g_tls.filesz);
    std::memset(base + block + kTlsSystemArea + g_tls.filesz, 0, g_tls.zero);
    return block + kTlsSystemArea + kTlsPointerBias;
}

void freeTls(std::uint32_t r13) {
    if (!g_tls.ready || r13 < kTlsSystemArea + kTlsPointerBias) {
        return;
    }
    const std::uint32_t block = r13 - kTlsPointerBias - kTlsSystemArea;
    const std::uint32_t offset = block - g_tls.area;  // wraps for a block below the area
    if (block >= g_tls.area && offset % g_tls.block_size == 0 && offset / g_tls.block_size < g_tls.used.size()) {
        // Same slot test as RPCS3 `ppu_free_tls`; a second free of a slot is reported and ignored.
        const std::uint32_t slot = offset / g_tls.block_size;
        if (!g_tls.used[slot]) {
            logWarn("freeTls({:#x}): block is not allocated", block);
        }
        g_tls.used[slot] = false;
        return;
    }
    // A block that did not fit the shared area came from the main region.
    if (process().main.free(block) == 0) {
        logWarn("freeTls({:#x}): block is not allocated", block);
    }
}

std::uint32_t initializeTls(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t thread_id, std::uint32_t vaddr,
                            std::uint32_t filesz, std::uint32_t memsz) {
    if (ctx->gpr[13] != 0) {
        return static_cast<std::uint32_t>(ctx->gpr[13]);
    }
    if (filesz > memsz) {
        fatal(FatalKind::Internal, std::format("sys_initialize_tls: filesz {:#x} exceeds memsz {:#x}", filesz, memsz));
    }
    g_tls = TlsState{};
    g_tls.image = vaddr;
    g_tls.filesz = filesz;
    g_tls.zero = memsz - filesz;
    g_tls.block_size = memsz + kTlsSystemArea;
    const std::uint32_t area = process().main.allocate(kTlsAreaBytes, 0x10000);
    if (area == 0) {
        fatal(FatalKind::Internal, "sys_initialize_tls: no memory for the TLS area");
    }
    g_tls.area = area + kTlsSystemArea;
    g_tls.used.assign((kTlsAreaBytes - kTlsSystemArea) / g_tls.block_size, false);
    g_tls.ready = true;

    const std::uint32_t r13 = allocateTls();
    if (r13 == 0) {
        fatal(FatalKind::Internal, "sys_initialize_tls: no TLS block for the main thread");
    }
    ctx->gpr[13] = r13;
    logInfo("tox2: tls: thread {:#010x} area {:#010x} block {:#x} slots {} r13 {:#010x}", thread_id, area,
            g_tls.block_size, g_tls.used.size(), r13);

    // The sync objects RPCS3 creates at this point: the atexit and `_lv2prx` lwmutexes and the
    // `_lv2ppu` and `_lv2tls` mutexes.
    createProcessSyncObjects(base);
    return r13;
}

void bootMainThread(const LoadedExecutable& exe, std::string_view argv0, std::uint32_t module_bytes) {
    Process& p = process();
    std::uint8_t* base = p.memory.base();
    // Nothing runs yet, but the allocators and the thread registry expect the guest lock. The new
    // thread queues on it and starts once this function returns.
    std::lock_guard<GuestLock> guard(guestLock());

    GuestThreadParams params;
    params.entry_opd = exe.entry_opd;
    params.priority = exe.primary_prio;
    params.stack_size = primaryStackSize(exe.primary_stacksize);
    params.joinable = false;
    params.name = "main_thread";
    GuestThread* thread = prepareGuestThread(params);
    if (thread == nullptr) {
        fatal(FatalKind::Internal, "cannot allocate the primary stack");
    }

    // RPCS3 charges the primary stack and the loaded segments to the default memory container
    // (`ppu_load_exec`, "take(primary_stacksize + segs_size)"); the image range stands in for the
    // segments, which are 64 KiB rounded. Modules loaded next to the executable are charged the same
    // way; the sum is assigned, so a caller has to pass their bytes here.
    p.user_memory_used = thread->stackSize() + (exe.image_end - kMainRegion.begin) + module_bytes;

    // Argument block at the top of the stack: argv[] (u64 pointers), 0, envp[] (empty), 0, padding
    // to 16 bytes, then the strings. The environment is empty.
    constexpr std::uint32_t kArgc = 1, kEnvc = 0;
    const std::uint32_t pointers_bytes = 8 * alignUp(kArgc + kEnvc + 2, 2);
    const auto string_bytes = static_cast<std::uint32_t>(argv0.size()) + 1;
    const std::uint32_t block_bytes = pointers_bytes + alignUp(string_bytes, 0x10);
    const std::uint32_t stack_top = thread->stackAddr() + thread->stackSize();
    if (thread->stackSize() <= block_bytes) {
        fatal(FatalKind::Internal, "primary stack is too small for the argument block");
    }
    const std::uint32_t argv = stack_top - block_bytes;
    const std::uint32_t envp = argv + 8 * (kArgc + 1);
    const std::uint32_t strings = argv + pointers_bytes;
    tox2Write64(base, argv, strings);
    tox2Write64(base, argv + 8, 0);
    tox2Write64(base, envp, 0);
    std::memcpy(base + strings, argv0.data(), argv0.size());
    base[strings + argv0.size()] = 0;

    Tox2PpuContext& ctx = thread->context();
    ctx.gpr[1] -= block_bytes;
    initializeTls(&ctx, base, thread->id(), exe.tls_vaddr, exe.tls_filesz, exe.tls_memsz);

    // r2 (the entry's TOC) was taken from the descriptor by prepareGuestThread. The rest is RPCS3's
    // `set_args 8` plus r11 = e_entry and r12 = malloc page size.
    ctx.gpr[3] = kArgc;
    ctx.gpr[4] = argv;
    ctx.gpr[5] = envp;
    ctx.gpr[6] = kEnvc;
    ctx.gpr[7] = thread->id();
    ctx.gpr[8] = exe.tls_vaddr;
    ctx.gpr[9] = exe.tls_filesz;
    ctx.gpr[10] = exe.tls_memsz;
    ctx.gpr[11] = exe.entry_opd;
    ctx.gpr[12] = exe.malloc_pagesize;
    ctx.lr = 0;

    logInfo("tox2: boot: stack {:#010x}..{:#010x} entry {:#010x} toc {:#010x} r1 {:#010x} r2 {:#010x} "
            "r3..r12 {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} r13 {:#010x}",
            thread->stackAddr(), stack_top, exe.entry_func, exe.entry_toc, ctx.gpr[1], ctx.gpr[2], ctx.gpr[3],
            ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7], ctx.gpr[8], ctx.gpr[9], ctx.gpr[10], ctx.gpr[11],
            ctx.gpr[12], ctx.gpr[13]);

    startGuestThread(thread);
}

}  // namespace tox2

// HLE handlers of the cellGcmSys module: initialisation, configuration, IO mapping, the command
// buffer wrap callback, display buffers, flip commands, tiles and zcull. Behaviour follows RPCS3
// `Emu/Cell/Modules/cellGcmSys.cpp` (function of the same name) and the memory layout of
// `Emu/Cell/lv2/sys_rsx.cpp`; the state lives in rsx/rsx_state.h. Handlers run under the kernel lock, except the ones
// marked kernel_free, which touch only atomic RsxState fields, guest memory and the commit map.

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "rsx/fifo_parser.h"
#include "rsx/gcm_interrupts.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_thread.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellGcmSys";

// CellGcmConfig, CellGcmContextData, CellGcmControl, RsxDriverInfo field offsets.
constexpr std::uint32_t kConfigLocalAddress = 0x00;
constexpr std::uint32_t kConfigIoAddress = 0x04;
constexpr std::uint32_t kConfigLocalSize = 0x08;
constexpr std::uint32_t kConfigIoSize = 0x0c;
constexpr std::uint32_t kConfigMemoryFrequency = 0x10;
constexpr std::uint32_t kConfigCoreFrequency = 0x14;
constexpr std::uint32_t kConfigSize = 0x18;

constexpr std::uint32_t kContextBegin = 0x00;
constexpr std::uint32_t kContextEnd = 0x04;
constexpr std::uint32_t kContextCurrent = 0x08;
constexpr std::uint32_t kContextCallback = 0x0c;
constexpr std::uint32_t kContextSize = 0x10;

constexpr std::uint32_t kControlPut = 0x00;
constexpr std::uint32_t kControlGet = 0x04;
constexpr std::uint32_t kControlRef = 0x08;

constexpr std::uint32_t kDriverVersionDriver = 0x00;
constexpr std::uint32_t kDriverVersionGpu = 0x04;
constexpr std::uint32_t kDriverMemorySize = 0x08;
constexpr std::uint32_t kDriverHardwareChannel = 0x0c;
constexpr std::uint32_t kDriverNvcoreFrequency = 0x10;
constexpr std::uint32_t kDriverMemoryFrequency = 0x14;
constexpr std::uint32_t kDriverReportsNotifyOffset = 0x2c;
constexpr std::uint32_t kDriverReportsOffset = 0x30;
constexpr std::uint32_t kDriverReportsReportOffset = 0x34;
constexpr std::uint32_t kDriverSystemModeFlags = 0x50;
constexpr std::uint32_t kDriverInfoSize = 0x12f8;

constexpr std::uint32_t kMemoryFrequency = 650000000;
constexpr std::uint32_t kCoreFrequency = 500000000;

// The default command buffer is cut into 32 KiB fragments; the first 4 KiB of the buffer and the
// last word of every fragment are reserved (the word holds the jump to the next fragment).
constexpr std::uint32_t kFragmentSize = 0x8000;
constexpr std::uint32_t kFragmentReserved = 0x1000;

// Initial control register: the first command is written at IO offset 0x1000, and the reference
// value is all ones until the game writes one.
constexpr std::uint32_t kControlStart = 0x1000;
constexpr std::uint32_t kControlRefInitial = 0xffffffff;

// Method headers are `method | (argument count << 18)` (RPCS3 `rsx::make_command`).
constexpr std::uint32_t kMaxDisplayId = 7;
constexpr std::uint64_t kFlipTimeOffsetUs = 1000000;

// Return value of a refused zcull request that the lv1 layer reports (LV1_ILLEGAL_PARAMETER_VALUE).
constexpr std::uint32_t kLv1IllegalParameterValue = 0xffffffef;
// The zcull RAM is 3 MiB.
constexpr std::uint64_t kZcullRamSize = 3u << 20;

// Stack offset of the ninth integer argument (PPC64 ELF ABI parameter save area).
constexpr std::uint32_t kStackArgBase = 0x70;

constexpr std::uint32_t kTiledPitches[] = {
        0x00000000, 0x00000200, 0x00000300, 0x00000400, 0x00000500, 0x00000600, 0x00000700, 0x00000800, 0x00000A00,
        0x00000C00, 0x00000D00, 0x00000E00, 0x00001000, 0x00001400, 0x00001800, 0x00001A00, 0x00001C00, 0x00002000,
        0x00002800, 0x00003000, 0x00003400, 0x00003800, 0x00004000, 0x00005000, 0x00006000, 0x00006800, 0x00007000,
        0x00008000, 0x0000A000, 0x0000C000, 0x0000D000, 0x0000E000, 0x00010000};

void requireCommitted(std::uint32_t addr, std::uint32_t size, const char* what) {
    if (addr == 0 || !process().memory.isCommitted(addr, size)) {
        fatal(FatalKind::GuestFault, std::format("cellGcmSys: {} {:#010x} is not committed guest memory", what, addr));
    }
}

// Integer argument `n` (>= 8) that the caller passed in its stack parameter save area. The slot is
// a doubleword; the argument is its low word.
std::uint32_t stackArgU32(const Tox2PpuContext* ctx, const std::uint8_t* base, std::uint32_t n) {
    return static_cast<std::uint32_t>(
            tox2Read64(base, static_cast<std::uint32_t>(ctx->gpr[1]) + kStackArgBase + 8 * (n - 8)));
}

// Appends a method header and its arguments at the context's `current` and advances it.
void writeCommand(std::uint8_t* base, std::uint32_t context, std::uint32_t method,
                  std::initializer_list<std::uint32_t> args) {
    const std::uint32_t bytes = 4 * (static_cast<std::uint32_t>(args.size()) + 1);
    std::uint32_t current = tox2Read32(base, context + kContextCurrent);
    requireCommitted(current, bytes, "command buffer");
    const std::uint32_t start = current;
    tox2Write32(base, current, method | (static_cast<std::uint32_t>(args.size()) << kFifoCountShift));
    current += 4;
    for (const std::uint32_t arg : args) {
        tox2Write32(base, current, arg);
        current += 4;
    }
    tox2Write32(base, context + kContextCurrent, start + bytes);
}

// Wrap callback of the default context: flush up to `current`, continue in the next fragment.
// RPCS3 `cellGcmCallback`, `getNextCommandBufferBeginEnd`, `isInCommandBufferExcept`.
void gcmCallback(Tox2PpuContext* ctx, std::uint8_t* base) {
    // The game calls this as guest code, outside any handler's kernel section.
    KernelSection kernel;
    RsxState& state = rsxState();
    const std::uint32_t context = argU32(ctx, 0);
    requireCommitted(context, kContextSize, "command buffer context");

    const auto toOffset = [&](std::uint32_t address) {
        std::uint32_t offset = 0;
        if (!rsxEaToOffset(address, offset)) {
            fatal(FatalKind::GuestFault,
                  std::format("cellGcmCallback: {:#010x} is not mapped in the IO space", address));
        }
        return offset;
    };

    const std::uint32_t current = tox2Read32(base, context + kContextCurrent);
    tox2Write32(base, kRsxControlAddr + kControlPut, toOffset(current));

    const std::uint32_t buffer_begin = state.command_begin;
    const std::uint32_t range = (current - buffer_begin) / kFragmentSize;
    std::uint32_t next_begin = buffer_begin + kFragmentReserved;
    std::uint32_t next_end = buffer_begin + kFragmentSize - 4;
    if (range < state.fragment_count - 1) {  // unsigned wrap for a zero fragment count, as RPCS3
        next_begin = buffer_begin + (range + 1) * kFragmentSize;
        next_end = buffer_begin + (range + 2) * kFragmentSize - 4;
    }

    const std::uint32_t jump_target = toOffset(next_begin);
    tox2Write32(base, current, kFifoOldJump | jump_target);
    tox2Write32(base, context + kContextBegin, next_begin);
    tox2Write32(base, context + kContextCurrent, next_begin);
    tox2Write32(base, context + kContextEnd, next_end);

    // The consumer must have left the new fragment before the game writes into it. RPCS3 also has a
    // first test for `get` outside the whole default buffer, but its two comparisons can never hold
    // together, so only the fragment test is effective.
    const std::uint32_t new_begin_offset = jump_target;
    const std::uint32_t new_end_offset = toOffset(next_end);
    blockingWait(
            rsxGetAdvanced(),
            [&] {
                const std::uint32_t get = tox2Read32(base, kRsxControlAddr + kControlGet);
                return get < new_begin_offset || get > new_end_offset;
            },
            "gcm command buffer wait", std::nullopt);
    setResultCode(ctx, kCellOk);
}

// _cellGcmInitBody(context**, cmdSize, ioSize, ioAddress)
void gcmInitBodyHandler(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t result =
            gcmInitBody(base, argU32(ctx, 0), argU32(ctx, 1), argU32(ctx, 2), argU32(ctx, 3), kRsxLocalSize);
    // Only the import starts the interrupt thread and the consumer: tests call gcmInitBody and drive
    // the control register by hand. The hook must exist before the consumer can raise an interrupt.
    if (result == kCellOk) {
        startGcmInterruptThread();
        startRsxThread(base);
    }
    setResultCode(ctx, result);
}

// cellGcmGetConfiguration(CellGcmConfig*)
void gcmGetConfiguration(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t config = argU32(ctx, 0);
    requireCommitted(config, kConfigSize, "CellGcmConfig");
    const RsxState& state = rsxState();
    const bool ready = rsxInitialized();
    tox2Write32(base, config + kConfigLocalAddress, ready ? kRsxLocalBase : 0);
    tox2Write32(base, config + kConfigIoAddress, state.io_address);
    tox2Write32(base, config + kConfigLocalSize, rsxLocalSize());
    tox2Write32(base, config + kConfigIoSize, state.io_size);
    tox2Write32(base, config + kConfigMemoryFrequency, ready ? kMemoryFrequency : 0);
    tox2Write32(base, config + kConfigCoreFrequency, ready ? kCoreFrequency : 0);
}

// cellGcmMapMainMemory(ea, size, u32* offset)
void gcmMapMainMemory(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t offset_out = argU32(ctx, 2);
    std::uint32_t io = 0;
    const std::uint32_t error = rsxMapMainMemory(argU32(ctx, 0), argU32(ctx, 1), io);
    if (error == 0) {
        requireCommitted(offset_out, 4, "cellGcmMapMainMemory offset pointer");
        tox2Write32(base, offset_out, io);
    }
    setResultCode(ctx, error);
}

// cellGcmAddressToOffset(address, u32* offset)
void gcmAddressToOffset(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t offset_out = argU32(ctx, 1);
    std::uint32_t offset = 0;
    if (!rsxEaToOffset(argU32(ctx, 0), offset)) return setResultCode(ctx, kGcmErrorFailure);
    requireCommitted(offset_out, 4, "cellGcmAddressToOffset offset pointer");
    tox2Write32(base, offset_out, offset);
    setResultCode(ctx, kCellOk);
}

// cellGcmUnmapEaIoAddress(ea)
void gcmUnmapEaIoAddress(Tox2PpuContext* ctx, std::uint8_t*) { setResultCode(ctx, rsxUnmapEa(argU32(ctx, 0))); }

// cellGcmGetTiledPitchSize(size): the smallest tiled pitch that holds `size`, 0 when none does.
void gcmGetTiledPitchSize(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t size = argU32(ctx, 0);
    std::uint32_t result = 0;
    for (std::size_t i = 0; i + 1 < std::size(kTiledPitches); i++) {
        if (kTiledPitches[i] < size && size <= kTiledPitches[i + 1]) {
            result = kTiledPitches[i + 1];
            break;
        }
    }
    ctx->gpr[3] = result;
}

void gcmGetControlRegister(Tox2PpuContext* ctx, std::uint8_t*) { ctx->gpr[3] = kRsxControlAddr; }

void gcmGetDefaultCommandWordSize(Tox2PpuContext* ctx, std::uint8_t*) { ctx->gpr[3] = rsxState().command_size; }

void gcmGetDefaultSegmentWordSize(Tox2PpuContext* ctx, std::uint8_t*) { ctx->gpr[3] = rsxState().segment_size; }

// cellGcmSetDefaultFifoSize(bufferSize, segmentSize): RPCS3 only logs.
void gcmSetDefaultFifoSize(Tox2PpuContext* ctx, std::uint8_t*) {
    logOnce("gcm-set-default-fifo-size", "tox2: gcm: cellGcmSetDefaultFifoSize({:#x}, {:#x}) ignored", argU32(ctx, 0),
            argU32(ctx, 1));
    setResultCode(ctx, kCellOk);
}

// _cellGcmFunc15(context*): void in RPCS3 and a todo there.
void gcmFunc15(Tox2PpuContext* ctx, std::uint8_t*) {
    logOnce("gcm-func15", "tox2: gcm: _cellGcmFunc15({:#010x}) ignored", argU32(ctx, 0));
}

// cellGcmGetTimeStamp(index): the timestamp of local report `index`.
void gcmGetTimeStamp(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t index = argU32(ctx, 0);
    if (index >= 2048) {
        logError("tox2: gcm: cellGcmGetTimeStamp: index {} is out of range", index);
        ctx->gpr[3] = 0;
        return;
    }
    ctx->gpr[3] = tox2Read64(base, kRsxLabelAddr + kRsxReportsReportOffset + 0x10 * index);
}

// cellGcmSetDisplayBuffer(id, offset, pitch, width, height)
void gcmSetDisplayBuffer(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t id = argU32(ctx, 0) & 0xff;
    if (id > kMaxDisplayId) return setResultCode(ctx, kGcmErrorFailure);
    RsxState& state = rsxState();
    state.display[id] = {argU32(ctx, 1), argU32(ctx, 2), argU32(ctx, 3), argU32(ctx, 4)};
    const RsxDisplayBuffer& buffer = state.display[id];
    logOnce(std::format("gcm-display-{}-{}-{}-{}-{}", id, buffer.offset, buffer.pitch, buffer.width, buffer.height),
            "tox2: gcm: display buffer {}: offset {:#x} pitch {:#x} {}x{}", id, buffer.offset, buffer.pitch,
            buffer.width, buffer.height);
    state.display_count = std::max(state.display_count, id + 1);
    setResultCode(ctx, kCellOk);
}

// cellGcmSetFlipMode(mode): void. Recorded as RPCS3's `requested_vsync`; in the default flip limit (vsync) a flip in
// VSYNC mode waits for the next vblank tick, in HSYNC mode (or any other flip limit) it does not.
void gcmSetFlipMode(Tox2PpuContext* ctx, std::uint8_t*) {
    // Atomic because the RSX thread reads it without the guest lock while a flip waits for a vblank.
    std::atomic_ref<bool>(rsxState().requested_vsync)
            .store(argU32(ctx, 0) == kRsxDisplayVsync, std::memory_order_relaxed);
}

// cellGcmResetFlipStatus(): void
void gcmResetFlipStatus(Tox2PpuContext*, std::uint8_t*) {
    std::atomic_ref(rsxState().flip_status).store(kRsxFlipStatusWaiting, std::memory_order_relaxed);
}

// cellGcmGetFlipStatus(): s32, the status the last flip or reset left (RPCS3 returns the renderer's flip_status).
// Kernel-free: the RSX thread stores the status without the guest lock, so every access is atomic.
void gcmGetFlipStatus(Tox2PpuContext* ctx, std::uint8_t*) {
    ctx->gpr[3] = std::atomic_ref(rsxState().flip_status).load(std::memory_order_relaxed);
}

// cellGcmGetLabelAddress(u8 index): the 16-byte label slot `index` of the semaphore area. The consumer resolves
// the semaphore offsets of _cellGcmSetFlipCommandWithWaitLabel and the NV406E/NV4097 releases against the same
// base (kRsxLabelAddr + offset), so a label the game polls is the one the RSX writes. Before initialisation there is
// no label area, so the result is 0 (RPCS3 would return the bare offset, which no guest memory backs either).
void gcmGetLabelAddress(Tox2PpuContext* ctx, std::uint8_t*) {
    // Kernel-free: `initialized` is stored atomically by gcmInitBody.
    ctx->gpr[3] = rsxInitialized() ? kRsxLabelAddr + kRsxLabelStride * (argU32(ctx, 0) & 0xff) : 0;
}

// cellGcmGetLastFlipTime(): u64
void gcmGetLastFlipTime(Tox2PpuContext* ctx, std::uint8_t*) { ctx->gpr[3] = rsxState().last_flip_time; }

// cellGcmSetFlipHandler(handler), cellGcmSetVBlankHandler(handler), cellGcmSetUserHandler(handler):
// void; the handler is ignored before initialisation, as RPCS3 does.
template <std::uint32_t RsxState::* Member>
void gcmSetHandler(Tox2PpuContext* ctx, std::uint8_t*) {
    RsxState& state = rsxState();
    if (rsxInitialized()) state.*Member = argU32(ctx, 0);
}

// Shared tail of the old-API flip functions: the flip method with the display buffer id.
// RPCS3 `gcmSetPrepareFlip<true>`: no room check and no update of the put pointer.
void writeFlipCommand(std::uint8_t* base, std::uint32_t context, std::uint32_t id, const char* name) {
    if (id > kMaxDisplayId) {
        logError("tox2: gcm: {}: display buffer id {} is out of range, nothing written", name, id);
        return;
    }
    writeCommand(base, context, kMethodGcmFlipCommand, {id});
}

// _cellGcmSetFlipCommand(context*, id): void
void gcmSetFlipCommand(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    requireCommitted(context, kContextSize, "command buffer context");
    writeFlipCommand(base, context, argU32(ctx, 1), "_cellGcmSetFlipCommand");
}

// _cellGcmSetFlipCommandWithWaitLabel(context*, id, label_index, label_value): void. The label
// wait is written even when the flip itself is refused.
void gcmSetFlipCommandWithWaitLabel(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    requireCommitted(context, kContextSize, "command buffer context");
    writeCommand(base, context, kMethodNv406eSemaphoreOffset, {argU32(ctx, 2) * kRsxLabelStride, argU32(ctx, 3)});
    writeFlipCommand(base, context, argU32(ctx, 1), "_cellGcmSetFlipCommandWithWaitLabel");
}

// cellGcmSetTileInfo(index, location, offset, size, pitch, comp, base, bank)
void gcmSetTileInfo(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t index = argU32(ctx, 0) & 0xff, location = argU32(ctx, 1) & 0xff;
    const std::uint32_t offset = argU32(ctx, 2), size = argU32(ctx, 3), pitch = argU32(ctx, 4);
    const std::uint32_t comp = argU32(ctx, 5) & 0xff, tile_base = argU32(ctx, 6) & 0xffff, bank = argU32(ctx, 7) & 0xff;

    if (index >= kRsxTileCount || tile_base >= 2048 || bank >= 4) return setResultCode(ctx, kGcmErrorInvalidValue);
    if ((offset & 0xffff) != 0 || (size & 0xffff) != 0 || (pitch & 0xff) != 0) {
        return setResultCode(ctx, kGcmErrorInvalidAlignment);
    }
    if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12))) return setResultCode(ctx, kGcmErrorInvalidEnum);

    RsxTile& tile = rsxState().tiles[index];
    tile.location = location;
    tile.offset = offset;
    tile.size = size;
    tile.pitch = pitch;
    tile.comp = comp;
    tile.base = tile_base;
    tile.bank = bank;
    setResultCode(ctx, kCellOk);
}

// cellGcmBindTile(index), cellGcmUnbindTile(index)
template <bool Bound>
void gcmBindTile(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t index = argU32(ctx, 0) & 0xff;
    if (index >= kRsxTileCount) return setResultCode(ctx, kGcmErrorInvalidValue);
    rsxState().tiles[index].bound = Bound;
    setResultCode(ctx, kCellOk);
}

// cellGcmBindZcull(index, offset, width, height, cullStart, zFormat, aaFormat, zCullDir, zCullFormat,
// sFunc, sRef, sMask). RPCS3 packs the region into the register layout of the lv1 call and its
// handler (`sys_rsx_context_attribute`, package 0x301) validates and unpacks it again; the fields
// are kept the way that round trip leaves them.
void gcmBindZcull(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t index = argU32(ctx, 0) & 0xff;
    const std::uint32_t offset = argU32(ctx, 1), width = argU32(ctx, 2), height = argU32(ctx, 3);
    const std::uint32_t cull_start = argU32(ctx, 4), z_format = argU32(ctx, 5), aa_format = argU32(ctx, 6);
    const std::uint32_t zcull_dir = argU32(ctx, 7), zcull_format = stackArgU32(ctx, base, 8);
    const std::uint32_t s_func = stackArgU32(ctx, base, 9), s_ref = stackArgU32(ctx, base, 10);
    const std::uint32_t s_mask = stackArgU32(ctx, base, 11);

    if (!rsxInitialized()) return setResultCode(ctx, kCellEinval);
    if (index >= kRsxZcullCount) return setResultCode(ctx, kLv1IllegalParameterValue);

    const std::uint32_t region = 1 | (z_format << 4) | (aa_format << 8);
    const std::uint32_t size = ((width >> 6) << 22) | ((height >> 6) << 6);
    const std::uint32_t start = cull_start & ~0xfffu;
    const std::uint32_t status0 =
            (zcull_dir << 1) | (zcull_format << 2) | ((s_func & 0xf) << 12) | (s_ref << 16) | (s_mask << 24);

    RsxZcull zcull;
    zcull.z_format = (region >> 4) & 0xf;
    zcull.aa_format = (region >> 8) & 0xf;
    zcull.width = (size >> 22) << 6;
    zcull.height = ((size & 0xffff) >> 6) << 6;
    zcull.cull_start = start;
    zcull.offset = offset & 0x0fffffff;
    zcull.zcull_dir = (status0 >> 1) & 1;
    zcull.zcull_format = (status0 >> 2) & 0x3ff;
    zcull.s_func = (status0 >> 12) & 0xf;
    zcull.s_ref = (status0 >> 16) & 0xff;
    zcull.s_mask = (status0 >> 24) & 0xff;
    zcull.bound = true;

    // The region must be non-empty and inside the zcull RAM; the offset inside local memory.
    const std::uint32_t length = zcull.width * zcull.height;
    if (length == 0 || std::uint64_t{start} + length - 1 >= kZcullRamSize || zcull.offset >= rsxLocalSize()) {
        return setResultCode(ctx, kCellEinval);
    }
    rsxState().zculls[index] = zcull;
    setResultCode(ctx, kCellOk);
}

// cellGcmUnbindZcull(index)
void gcmUnbindZcull(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t index = argU32(ctx, 0) & 0xff;
    if (index >= kRsxZcullCount) return setResultCode(ctx, kGcmErrorInvalidValue);
    rsxState().zculls[index].bound = false;
    setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x055bd74d, "cellGcmGetTiledPitchSize", gcmGetTiledPitchSize, true},
        {kModule, 0x06edea9e, "cellGcmSetUserHandler", gcmSetHandler<&RsxState::user_handler>},
        {kModule, 0x15bae46b, "_cellGcmInitBody", gcmInitBodyHandler},
        {kModule, 0x21397818, "_cellGcmSetFlipCommand", gcmSetFlipCommand, true},
        {kModule, 0x21ac3697, "cellGcmAddressToOffset", gcmAddressToOffset, true},
        {kModule, 0x3a33c1fd, "_cellGcmFunc15", gcmFunc15},
        {kModule, 0x4524cccd, "cellGcmBindTile", gcmBindTile<true>},
        {kModule, 0x4ae8d215, "cellGcmSetFlipMode", gcmSetFlipMode, true},
        {kModule, 0x5a41c10f, "cellGcmGetTimeStamp", gcmGetTimeStamp, true},
        {kModule, 0x5e2ee0f0, "cellGcmGetDefaultCommandWordSize", gcmGetDefaultCommandWordSize},
        {kModule, 0x63387071, "cellGcmGetLastFlipTime", gcmGetLastFlipTime},
        {kModule, 0x72a577ce, "cellGcmGetFlipStatus", gcmGetFlipStatus, true},
        {kModule, 0x8cdf8c70, "cellGcmGetDefaultSegmentWordSize", gcmGetDefaultSegmentWordSize},
        {kModule, 0x9ba451e4, "cellGcmSetDefaultFifoSize", gcmSetDefaultFifoSize},
        {kModule, 0x9dc04436, "cellGcmBindZcull", gcmBindZcull},
        {kModule, 0xa114ec67, "cellGcmMapMainMemory", gcmMapMainMemory},
        {kModule, 0xa41ef7e8, "cellGcmSetFlipHandler", gcmSetHandler<&RsxState::flip_handler>},
        {kModule, 0xa53d12ae, "cellGcmSetDisplayBuffer", gcmSetDisplayBuffer},
        {kModule, 0xa547adde, "cellGcmGetControlRegister", gcmGetControlRegister, true},
        {kModule, 0xa75640e8, "cellGcmUnbindZcull", gcmUnbindZcull},
        {kModule, 0xa91b0402, "cellGcmSetVBlankHandler", gcmSetHandler<&RsxState::vblank_handler>},
        {kModule, 0xb2e761d4, "cellGcmResetFlipStatus", gcmResetFlipStatus},
        {kModule, 0xbd100dbc, "cellGcmSetTileInfo", gcmSetTileInfo},
        {kModule, 0xd8f88e1a, "_cellGcmSetFlipCommandWithWaitLabel", gcmSetFlipCommandWithWaitLabel},
        {kModule, 0xd9b7653e, "cellGcmUnbindTile", gcmBindTile<false>},
        {kModule, 0xe315a0b2, "cellGcmGetConfiguration", gcmGetConfiguration},
        {kModule, 0xefd00f54, "cellGcmUnmapEaIoAddress", gcmUnmapEaIoAddress},
        {kModule, 0xf80196c1, "cellGcmGetLabelAddress", gcmGetLabelAddress, true},
};

}  // namespace

std::span<const HleEntry> cellGcmSysHandlers() { return kHandlers; }

std::uint32_t gcmInitBody(std::uint8_t* base, std::uint32_t context_pp, std::uint32_t cmd_size, std::uint32_t io_size,
                          std::uint32_t io_address, std::uint32_t local_size) {
    RsxState& state = rsxState();
    if (rsxInitialized()) {
        // RPCS3 supports one context only and throws on a second allocation.
        logError("tox2: gcm: _cellGcmInitBody called twice");
        return kGcmErrorFailure;
    }
    requireCommitted(context_pp, 4, "context out-pointer");

    GuestMemory& memory = process().memory;
    if (!memory.commit(kRsxDeviceAddr, kRsxContextCommitSize) || !memory.commit(kRsxLocalBase, local_size)) {
        logError("tox2: gcm: cannot commit the RSX memory areas");
        return kGcmErrorFailure;
    }

    clearRsxState();
    std::atomic_ref(state.local_size).store(local_size, std::memory_order_relaxed);
    if (const std::uint32_t error = rsxMapEaIo(io_address, 0, io_size); error != 0) {
        logError("tox2: gcm: cannot map the IO area ea={:#010x} size={:#x} (error {:#010x})", io_address, io_size,
                 error);
        memory.decommit(kRsxDeviceAddr, kRsxContextCommitSize);
        memory.decommit(kRsxLocalBase, local_size);
        clearRsxState();
        return kGcmErrorFailure;
    }

    // Reports area (RPCS3 sys_rsx_context_allocate): semaphores, notify and report headers.
    std::memset(base + kRsxLabelAddr, 0, kRsxReportsSize);
    for (std::uint32_t i = 0; i < 1024; i += 4) {
        tox2Write32(base, kRsxLabelAddr + 4 * (i + 0), 0x1337C0D3);
        tox2Write32(base, kRsxLabelAddr + 4 * (i + 1), 0x1337BABE);
        tox2Write32(base, kRsxLabelAddr + 4 * (i + 2), 0x1337BEEF);
        tox2Write32(base, kRsxLabelAddr + 4 * (i + 3), 0x1337F001);
    }
    for (std::uint32_t i = 0; i < 64; i++) {
        tox2Write64(base, kRsxLabelAddr + kRsxReportsNotifyOffset + 0x10 * i, ~std::uint64_t{0});
    }
    for (std::uint32_t i = 0; i < 2048; i++) {
        const std::uint32_t report = kRsxLabelAddr + kRsxReportsReportOffset + 0x10 * i;
        tox2Write64(base, report, ~std::uint64_t{0});
        tox2Write32(base, report + 12, 0xffffffff);
    }

    std::memset(base + kRsxDriverInfoAddr, 0, kDriverInfoSize);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverVersionDriver, 0x211);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverVersionGpu, 0x5c);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverMemorySize, local_size);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverHardwareChannel, 1);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverNvcoreFrequency, kCoreFrequency);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverMemoryFrequency, kMemoryFrequency);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverReportsNotifyOffset, kRsxReportsNotifyOffset);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverReportsOffset, 0);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverReportsReportOffset, kRsxReportsReportOffset);
    tox2Write32(base, kRsxDriverInfoAddr + kDriverSystemModeFlags, 0);

    std::memset(base + kRsxDmaAddr, 0, 0x60);
    tox2Write32(base, kRsxControlAddr + kControlPut, kControlStart);
    tox2Write32(base, kRsxControlAddr + kControlGet, kControlStart);
    tox2Write32(base, kRsxControlAddr + kControlRef, kControlRefInitial);

    // RPCS3 `rsx::thread::init` marks the device as ready.
    std::memset(base + kRsxDeviceAddr, 0, 0x100);
    tox2Write32(base, kRsxDeviceAddr + 0x30, 1);

    std::atomic_ref(state.initialized).store(true, std::memory_order_relaxed);
    state.io_address = io_address;
    state.io_size = io_size;
    state.context_pp = context_pp;
    state.command_begin = io_address;
    state.fragment_count = cmd_size / kFragmentSize;
    state.wrap_callback = registerHostFunction("cellGcmCallback", gcmCallback);
    // RPCS3 starts the clock at the RSX thread start, one second in the past.
    const std::uint64_t now = systemTimeUs();
    state.last_flip_time = now > kFlipTimeOffsetUs ? now - kFlipTimeOffsetUs : 0;

    tox2Write32(base, kRsxDeviceAddr + kContextBegin, io_address + kFragmentReserved);
    tox2Write32(base, kRsxDeviceAddr + kContextEnd, io_address + kFragmentSize - 4);
    tox2Write32(base, kRsxDeviceAddr + kContextCurrent, io_address + kFragmentReserved);
    tox2Write32(base, kRsxDeviceAddr + kContextCallback, state.wrap_callback);
    tox2Write32(base, context_pp, kRsxDeviceAddr);

    logInfo("tox2: gcm: init local={:#010x}+{:#x} io={:#010x}+{:#x} commands={:#x} context={:#010x} control={:#010x}",
            kRsxLocalBase, local_size, io_address, io_size, cmd_size, kRsxDeviceAddr, kRsxControlAddr);
    return kCellOk;
}

}  // namespace tox2

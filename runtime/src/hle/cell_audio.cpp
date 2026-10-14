// HLE of the 12 cellAudio imports the game uses: Init, Quit, PortOpen, PortStart, PortStop,
// PortClose, GetPortConfig, GetPortBlockTag, GetPortTimestamp, SetPortLevel, SetNotifyEventQueue and
// RemoveNotifyEventQueue. The library state and the period logic are in cell_audio_ports.cpp; this
// file decodes the arguments and runs the audio engine that ticks the periods.
//
// Facts follow RPCS3 `rpcs3/Emu/Cell/Modules/cellAudio.cpp` (`cellAudioInit` .. `cellAudioRemoveNotifyEventQueue`,
// `AudioSetNotifyEventQueue`, `cell_audio_thread::advance`) and `cellAudio.h` (`CellAudioPortConfig`).
// Not taken over: RPCS3's key-0 workaround in `AudioSetNotifyEventQueue` (the thread creation rule
// makes the game's mixer create its queue first), its short sleeps in PortOpen/PortStart/Set, and
// its period catch-up (the engine skips late periods). cellAudioSetPortLevel differs too: RPCS3 ramps
// the level over several periods and accepts a negative level without changing anything, while here the
// level applies from the next period and a negative or NaN level is CELL_AUDIO_ERROR_PARAM.

#include <cstdint>
#include <mutex>
#include <span>

#include "audio/audio_engine.h"
#include "core/census.h"
#include "core/guest_memory.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellAudio";

// The engine keeps the producer of its first start for good, so this reads the library state
// through the global and checks it under the kernel lock: one call that was already under way can
// still arrive after cellAudioQuit.
void produceBlock(std::span<float> block) {
    setGuestLockClass(GuestLockClass::Audio);
    std::lock_guard<GuestLock> lock(guestLock());
    const std::uint64_t now_us = systemTimeUs();
    cellAudioPorts().producePeriod(process().memory.base(), now_us, block);
}

void audioInit(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t result = cellAudioPorts().init();
    if (result == kCellOk) startAudioEngine(produceBlock);
    setResultCode(ctx, result);
}

void audioQuit(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t result = cellAudioPorts().quit();
    if (result == kCellOk) requestAudioEngineStop();
    setResultCode(ctx, result);
}

// cellAudioPortOpen(CellAudioPortParam* param, u32* port_num)
void audioPortOpen(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, cellAudioPorts().portOpen(base, argU32(ctx, 0), argU32(ctx, 1)));
}

// cellAudioGetPortConfig(u32 port, CellAudioPortConfig* config)
void audioGetPortConfig(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, cellAudioPorts().getPortConfig(base, argU32(ctx, 0), argU32(ctx, 1)));
}

void audioPortStart(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, cellAudioPorts().portStart(base, argU32(ctx, 0)));
}

void audioPortStop(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, cellAudioPorts().portStop(argU32(ctx, 0)));
}

void audioPortClose(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, cellAudioPorts().portClose(argU32(ctx, 0)));
}

// cellAudioGetPortBlockTag(u32 port, u64 block_no, u64* tag)
void audioGetPortBlockTag(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, cellAudioPorts().getPortBlockTag(base, argU32(ctx, 0), ctx->gpr[4], argU32(ctx, 2)));
}

// cellAudioGetPortTimestamp(u32 port, u64 tag, u64* stamp)
void audioGetPortTimestamp(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, cellAudioPorts().getPortTimestamp(base, argU32(ctx, 0), ctx->gpr[4], argU32(ctx, 2)));
}

// cellAudioSetPortLevel(u32 port, float level): the level is the first floating-point argument.
void audioSetPortLevel(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, cellAudioPorts().setPortLevel(argU32(ctx, 0), static_cast<float>(ctx->fpr[1].f64)));
}

// cellAudioSetNotifyEventQueue(u64 key) and cellAudioRemoveNotifyEventQueue(u64 key)
void audioSetNotifyEventQueue(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, cellAudioPorts().setNotifyEventQueue(ctx->gpr[3]));
}

void audioRemoveNotifyEventQueue(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, cellAudioPorts().removeNotifyEventQueue(ctx->gpr[3]));
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x0b168f92, "cellAudioInit", audioInit},
        {kModule, 0xca5ac370, "cellAudioQuit", audioQuit},
        {kModule, 0x377e0cd9, "cellAudioSetNotifyEventQueue", audioSetNotifyEventQueue},
        {kModule, 0x4109d08c, "cellAudioGetPortTimestamp", audioGetPortTimestamp},
        {kModule, 0x4129fe2d, "cellAudioPortClose", audioPortClose},
        {kModule, 0x56dfe179, "cellAudioSetPortLevel", audioSetPortLevel},
        {kModule, 0x5b1e2c73, "cellAudioPortStop", audioPortStop},
        {kModule, 0x74a66af0, "cellAudioGetPortConfig", audioGetPortConfig},
        {kModule, 0x89be28f2, "cellAudioPortStart", audioPortStart},
        {kModule, 0xcd7bc431, "cellAudioPortOpen", audioPortOpen},
        {kModule, 0xe4046afe, "cellAudioGetPortBlockTag", audioGetPortBlockTag},
        {kModule, 0xff3626fd, "cellAudioRemoveNotifyEventQueue", audioRemoveNotifyEventQueue},
};

}  // namespace

std::span<const HleEntry> cellAudioHandlers() { return kHandlers; }

void resetCellAudio() { cellAudioPorts().reset(); }

}  // namespace tox2

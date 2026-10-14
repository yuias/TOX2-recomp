// HLE handlers of the cellSail graphics and sound adapters: Initialize, Finalize, SetPreferredFormat,
// GetFormat, GetFrame and UpdateAvSync. Behaviour follows RPCS3 `Emu/Cell/Modules/cellSail.cpp`
// (functions of the same names): Initialize on an initialised adapter and Finalize on an
// uninitialised or still registered one return CELL_SAIL_ERROR_INVALID_STATE, GetFormat returns
// what SetPreferredFormat stored (or what the player's OpenStream replaced it with). The adapter
// state lives host-side, keyed by the address of the guest object (which the game allocates and the
// HLE never writes), so a freed and reused address starts clean once Finalize ran.
//
// Deviations from RPCS3:
// - GraphicsAdapterInitialize there marks the adapter registered, which would make Finalize fail for
//   every caller; here only the player's SetXAdapter registers an adapter.
// - GetFrame and SoundAdapterGetFrame return a frame-status word: RPCS3 stubs them to CELL_OK, but
//   the game treats CELL_OK as "no frame" and consumes a frame only for a result >= 0x100 (the
//   CELL_SAIL_SYNC_* classes, OR-ed with the sync status).
// The adapters' makeup and cleanup callbacks are stored but not called; the game's are empty stubs.
//
// Everything here runs under the kernel lock.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>

#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/cell_sail_internal.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellSail";

struct GraphicsAdapter {
    std::uint32_t arg = 0;
    std::uint32_t makeup = 0, cleanup = 0, format_changed = 0, alloc = 0, free = 0;  // OPD addresses
    // Guest-layout CellSailVideoFormat bytes. `preferred` is what the game asked for; `format` is what
    // GetFormat reports and starts out as the same bytes.
    std::array<std::uint8_t, sail::kVideoFormatSize> preferred;
    std::array<std::uint8_t, sail::kVideoFormatSize> format;
    std::uint32_t player = 0;  // address of the player the adapter is registered with, 0 if none
    std::uint64_t av_sync_updates = 0;
};

struct SoundAdapter {
    std::uint32_t arg = 0;
    std::uint32_t makeup = 0, cleanup = 0, format_changed = 0;
    std::array<std::uint8_t, sail::kAudioFormatSize> preferred;
    std::array<std::uint8_t, sail::kAudioFormatSize> format;
    std::uint32_t player = 0;
    std::uint64_t av_sync_updates = 0;
};

std::map<std::uint32_t, GraphicsAdapter> g_graphics;
std::map<std::uint32_t, SoundAdapter> g_sound;

using sail_hle::guestMapped;

// The game fills formats it does not care about with 0xff bytes ("unspecified"); an adapter that has
// not been given a preferred format reports the same.
template <std::size_t N>
std::array<std::uint8_t, N> unspecifiedFormat() {
    std::array<std::uint8_t, N> bytes;
    bytes.fill(0xff);
    return bytes;
}

// cellSailGraphicsAdapterInitialize(adapter, CellSailGraphicsAdapterFuncs* callbacks, void* arg)
void graphicsInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t adapter = argU32(ctx, 0);
    const std::uint32_t callbacks = argU32(ctx, 1);
    if (adapter == 0 || !guestMapped(callbacks, sail::kGraphicsAdapterFuncsSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (g_graphics.contains(adapter)) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    GraphicsAdapter state;
    state.arg = argU32(ctx, 2);
    state.makeup = tox2Read32(base, callbacks + sail::kGraphicsAdapterFuncsMakeup);
    state.cleanup = tox2Read32(base, callbacks + sail::kGraphicsAdapterFuncsCleanup);
    state.format_changed = tox2Read32(base, callbacks + sail::kGraphicsAdapterFuncsFormatChanged);
    state.alloc = tox2Read32(base, callbacks + sail::kGraphicsAdapterFuncsAlloc);
    state.free = tox2Read32(base, callbacks + sail::kGraphicsAdapterFuncsFree);
    state.preferred = unspecifiedFormat<sail::kVideoFormatSize>();
    state.format = state.preferred;
    g_graphics.emplace(adapter, state);
    logDebug("tox2: sail: graphics adapter {:#010x} initialised (alloc {:#010x}, free {:#010x})", adapter, state.alloc,
             state.free);
    setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterFinalize(adapter)
void graphicsFinalize(Tox2PpuContext* ctx, std::uint8_t*) {
    const auto it = g_graphics.find(argU32(ctx, 0));
    if (it == g_graphics.end() || it->second.player != 0) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    logDebug("tox2: sail: graphics adapter {:#010x} finalized after {} av-sync updates", it->first,
             it->second.av_sync_updates);
    g_graphics.erase(it);
    setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterSetPreferredFormat(adapter, const CellSailVideoFormat* format)
void graphicsSetPreferredFormat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_graphics.find(argU32(ctx, 0));
    if (it == g_graphics.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t format = argU32(ctx, 1);
    if (!guestMapped(format, sail::kVideoFormatSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    std::memcpy(it->second.preferred.data(), base + format, sail::kVideoFormatSize);
    it->second.format = it->second.preferred;
    setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterGetFormat(adapter, CellSailVideoFormat* format)
void graphicsGetFormat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_graphics.find(argU32(ctx, 0));
    if (it == g_graphics.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!guestMapped(out, sail::kVideoFormatSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    std::memcpy(base + out, it->second.format.data(), sail::kVideoFormatSize);
    setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterInitialize(adapter, CellSailSoundAdapterFuncs* callbacks, void* arg)
void soundInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t adapter = argU32(ctx, 0);
    const std::uint32_t callbacks = argU32(ctx, 1);
    if (adapter == 0 || !guestMapped(callbacks, sail::kSoundAdapterFuncsSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (g_sound.contains(adapter)) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    SoundAdapter state;
    state.arg = argU32(ctx, 2);
    state.makeup = tox2Read32(base, callbacks + sail::kSoundAdapterFuncsMakeup);
    state.cleanup = tox2Read32(base, callbacks + sail::kSoundAdapterFuncsCleanup);
    state.format_changed = tox2Read32(base, callbacks + sail::kSoundAdapterFuncsFormatChanged);
    state.preferred = unspecifiedFormat<sail::kAudioFormatSize>();
    state.format = state.preferred;
    g_sound.emplace(adapter, state);
    logDebug("tox2: sail: sound adapter {:#010x} initialised", adapter);
    setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterFinalize(adapter)
void soundFinalize(Tox2PpuContext* ctx, std::uint8_t*) {
    const auto it = g_sound.find(argU32(ctx, 0));
    if (it == g_sound.end() || it->second.player != 0) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    logDebug("tox2: sail: sound adapter {:#010x} finalized after {} av-sync updates", it->first,
             it->second.av_sync_updates);
    g_sound.erase(it);
    setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterSetPreferredFormat(adapter, const CellSailAudioFormat* format)
void soundSetPreferredFormat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_sound.find(argU32(ctx, 0));
    if (it == g_sound.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t format = argU32(ctx, 1);
    if (!guestMapped(format, sail::kAudioFormatSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    std::memcpy(it->second.preferred.data(), base + format, sail::kAudioFormatSize);
    it->second.format = it->second.preferred;
    setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterGetFormat(adapter, CellSailAudioFormat* format)
void soundGetFormat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_sound.find(argU32(ctx, 0));
    if (it == g_sound.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!guestMapped(out, sail::kAudioFormatSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    std::memcpy(base + out, it->second.format.data(), sail::kAudioFormatSize);
    setResultCode(ctx, kCellOk);
}

// The game does not clear the sound info, so every field is written on every path.
void writeFrameInfo(std::uint8_t* base, std::uint32_t info, std::uint32_t buffer, std::uint32_t session_id,
                    std::uint32_t tag, std::int64_t pts) {
    tox2Write32(base, info + sail::kFrameInfoBuffer, buffer);
    tox2Write32(base, info + sail::kFrameInfoSessionId, session_id);
    tox2Write32(base, info + sail::kFrameInfoTag, tag);
    tox2Write32(base, info + sail::kFrameInfoStatus, 0);
    tox2Write64(base, info + sail::kFrameInfoPts, static_cast<std::uint64_t>(pts));
}

// cellSailGraphicsAdapterGetFrame(adapter, CellSailGraphicsFrameInfo* info)
// The game polls this once per tick from its movie thread and copies the buffer at once, so the
// frame is written into one of the buffers the player allocated and the call returns.
void graphicsGetFrame(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_graphics.find(argU32(ctx, 0));
    if (it == g_graphics.end() || it->second.player == 0) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t info = argU32(ctx, 1);
    if (!guestMapped(info, sail::kFrameInfoSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    const auto frame = sail_hle::takeVideoFrame(base, it->second.player);
    if (!frame) {
        writeFrameInfo(base, info, 0, 0, 0, 0);
        setResultCode(ctx, static_cast<std::uint32_t>(sail::kSyncNoFrame));
        return;
    }
    writeFrameInfo(base, info, frame->buffer, frame->session_id, frame->tag, frame->pts);
    setResultCode(ctx, static_cast<std::uint32_t>(sail::kSyncNextOnTime));
}

// cellSailGraphicsAdapterUpdateAvSync(adapter, u32 tag, u64 time)
// The game reports the flip time of the frame it drew. The player's clock is the host clock, so the
// report is only counted.
void graphicsUpdateAvSync(Tox2PpuContext* ctx, std::uint8_t*) {
    const auto it = g_graphics.find(argU32(ctx, 0));
    if (it == g_graphics.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    it->second.av_sync_updates++;
    setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterGetFrame(adapter, u32 samples, CellSailSoundFrameInfo* info)
void soundGetFrame(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto it = g_sound.find(argU32(ctx, 0));
    if (it == g_sound.end() || it->second.player == 0) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t info = argU32(ctx, 2);
    if (!guestMapped(info, sail::kFrameInfoSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    writeFrameInfo(base, info, 0, 0, 0, 0);
    sail_hle::SoundFrameResult frame;
    const std::uint32_t result = sail_hle::takeSoundFrame(base, it->second.player, argU32(ctx, 1), frame);
    if (result == static_cast<std::uint32_t>(sail::kSyncNextOnTime)) {
        writeFrameInfo(base, info, frame.buffer, frame.session_id, frame.tag, frame.pts);
    }
    setResultCode(ctx, result);
}

// cellSailSoundAdapterUpdateAvSync(adapter, u32 tag, u64 time)
void soundUpdateAvSync(Tox2PpuContext* ctx, std::uint8_t*) {
    const auto it = g_sound.find(argU32(ctx, 0));
    if (it == g_sound.end()) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    it->second.av_sync_updates++;
    setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x1c983864, "cellSailGraphicsAdapterInitialize", graphicsInitialize},
        {kModule, 0x76488bb1, "cellSailGraphicsAdapterFinalize", graphicsFinalize},
        {kModule, 0x2e3ccb5e, "cellSailGraphicsAdapterSetPreferredFormat", graphicsSetPreferredFormat},
        {kModule, 0xffd58aa4, "cellSailGraphicsAdapterGetFormat", graphicsGetFormat},
        {kModule, 0x0247c69e, "cellSailGraphicsAdapterGetFrame", graphicsGetFrame},
        {kModule, 0x44a20e79, "cellSailGraphicsAdapterUpdateAvSync", graphicsUpdateAvSync},
        {kModule, 0x3d0d3b72, "cellSailSoundAdapterInitialize", soundInitialize},
        {kModule, 0xd1462438, "cellSailSoundAdapterFinalize", soundFinalize},
        {kModule, 0x1c9d5e5a, "cellSailSoundAdapterSetPreferredFormat", soundSetPreferredFormat},
        {kModule, 0xf25f197d, "cellSailSoundAdapterGetFormat", soundGetFormat},
        {kModule, 0x7eb8d6b5, "cellSailSoundAdapterGetFrame", soundGetFrame},
        {kModule, 0xeec22809, "cellSailSoundAdapterUpdateAvSync", soundUpdateAvSync},
};

}  // namespace

std::span<const HleEntry> cellSailAdapterHandlers() { return kHandlers; }

namespace sail_hle {

std::uint32_t bindAdapter(AdapterKind kind, std::uint32_t adapter, std::uint32_t player) {
    std::uint32_t* owner = nullptr;
    if (kind == AdapterKind::Graphics) {
        const auto it = g_graphics.find(adapter);
        if (it != g_graphics.end()) owner = &it->second.player;
    } else {
        const auto it = g_sound.find(adapter);
        if (it != g_sound.end()) owner = &it->second.player;
    }
    if (owner == nullptr || (*owner != 0 && *owner != player)) return sail::kErrorInvalidState;
    *owner = player;
    return kCellOk;
}

void unbindAdapters(std::uint32_t player) {
    for (auto& entry : g_graphics) {
        if (entry.second.player == player) entry.second.player = 0;
    }
    for (auto& entry : g_sound) {
        if (entry.second.player == player) entry.second.player = 0;
    }
}

void resetAdapters() {
    g_graphics.clear();
    g_sound.clear();
}

std::optional<GraphicsBinding> graphicsBoundTo(std::uint32_t player) {
    for (const auto& [address, adapter] : g_graphics) {
        if (adapter.player != player) continue;
        GraphicsBinding binding;
        binding.arg = adapter.arg;
        binding.format_changed = adapter.format_changed;
        binding.alloc = adapter.alloc;
        binding.free = adapter.free;
        binding.preferred = adapter.preferred;
        return binding;
    }
    return std::nullopt;
}

std::optional<SoundBinding> soundBoundTo(std::uint32_t player) {
    for (const auto& [address, adapter] : g_sound) {
        if (adapter.player != player) continue;
        SoundBinding binding;
        binding.arg = adapter.arg;
        binding.format_changed = adapter.format_changed;
        binding.preferred = adapter.preferred;
        return binding;
    }
    return std::nullopt;
}

void setGraphicsFormat(std::uint32_t player, std::span<const std::uint8_t> format) {
    for (auto& [address, adapter] : g_graphics) {
        if (adapter.player == player && format.size() == adapter.format.size()) {
            std::memcpy(adapter.format.data(), format.data(), format.size());
        }
    }
}

void setSoundFormat(std::uint32_t player, std::span<const std::uint8_t> format) {
    for (auto& [address, adapter] : g_sound) {
        if (adapter.player == player && format.size() == adapter.format.size()) {
            std::memcpy(adapter.format.data(), format.data(), format.size());
        }
    }
}

}  // namespace sail_hle
}  // namespace tox2

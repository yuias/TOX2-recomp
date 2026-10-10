#pragma once

// Bookkeeping the cellSail handler files share: the adapter table the player binds adapters from, the
// stream the adapters pull frames from, and the PAMF database block that CreateDatabase writes and
// cellPamfReaderGetNumberOfSpecificStreams reads. Everything runs under the guest lock.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "core/guest_memory.h"
#include "hle/cell_sail_types.h"
#include "media/pamf.h"

namespace tox2::sail_hle {

inline bool guestMapped(std::uint32_t addr, std::uint32_t size) {
    return addr != 0 && process().memory.isCommitted(addr, size);
}

enum class AdapterKind { Graphics, Sound };

// Ties an initialised adapter to `player`. kCellOk, or kErrorInvalidState when the adapter is not
// initialised or already belongs to another player.
std::uint32_t bindAdapter(AdapterKind kind, std::uint32_t adapter, std::uint32_t player);

// Releases every adapter bound to `player`, so the adapters can be finalized.
void unbindAdapters(std::uint32_t player);

// Forgets all adapters. Intended for tests.
void resetAdapters();

// What the player needs from the adapter bound to it: the game's callbacks (OPD addresses) and the
// format the game asked for (guest-layout bytes, 0xff = unspecified).
struct GraphicsBinding {
    std::uint32_t arg = 0;
    std::uint32_t format_changed = 0, alloc = 0, free = 0;
    std::array<std::uint8_t, sail::kVideoFormatSize> preferred{};
};
struct SoundBinding {
    std::uint32_t arg = 0;
    std::uint32_t format_changed = 0;
    std::array<std::uint8_t, sail::kAudioFormatSize> preferred{};
};
std::optional<GraphicsBinding> graphicsBoundTo(std::uint32_t player);
std::optional<SoundBinding> soundBoundTo(std::uint32_t player);

// Replaces what GetFormat reports for the adapter bound to `player` (no-op without one).
void setGraphicsFormat(std::uint32_t player, std::span<const std::uint8_t> format);
void setSoundFormat(std::uint32_t player, std::span<const std::uint8_t> format);

// Fills the 128-byte database block at `addr` for a parsed header. False when the block is not
// mapped. Only stream counts are stored, so the block stays valid after the descriptor it came from
// is gone.
bool writePamfDatabase(std::uint8_t* base, std::uint32_t addr, const PamfInfo& info);

// Invalidates a block written by writePamfDatabase. False when `addr` does not hold one.
bool clearPamfDatabase(std::uint8_t* base, std::uint32_t addr);

// What GraphicsAdapterGetFrame reports for a frame that was copied into one of the game's buffers.
struct VideoFrameResult {
    std::uint32_t buffer = 0;
    std::uint32_t session_id = 0, tag = 0;
    std::int64_t pts = 0;  // 90 kHz, as in the file
};

// What SoundAdapterGetFrame reports: `frames` sample frames at `buffer`, valid until the next call.
struct SoundFrameResult {
    std::uint32_t buffer = 0;
    std::uint32_t session_id = 0, tag = 0;
    std::int64_t pts = 0;
};

// Implemented by the player. takeVideoFrame copies the newest due frame of the stream open on
// `player` into the next of the game's frame buffers; nullopt when no frame is due. It also detects
// the end of the stream, which the game cannot learn any other way. takeSoundFrame returns the
// cellSail result code for the call (kSyncNextOnTime with `out` filled, kSyncNoFrame, or an error).
std::optional<VideoFrameResult> takeVideoFrame(std::uint8_t* base, std::uint32_t player);
std::uint32_t takeSoundFrame(std::uint8_t* base, std::uint32_t player, std::uint32_t frames, SoundFrameResult& out);

}  // namespace tox2::sail_hle

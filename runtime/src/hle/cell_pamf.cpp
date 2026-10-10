// HLE of cellPamfReaderGetNumberOfSpecificStreams, the one cellPamf import the game calls. It asks
// a database that cellSailDescriptorCreateDatabase filled, so both ends are ours: the 128-byte
// CellPamfReader-sized block holds a magic and the stream count per codec instead of the reader's
// header pointers.
//
// The stream selectors follow RPCS3 `Emu/Cell/Modules/cellPamf.cpp` `pamfIsSameStreamType`: the
// codec-specific types match one codec, VIDEO (20) matches AVC and MPEG-2 video, AUDIO (21)
// matches ATRAC3plus, AC-3 and LPCM, and UNKNOWN (22) matches LPCM. The PSMF-only types (6..9)
// never match in a PAMF file. The result is the count (a u8 in RPCS3), not an error code.

#include <array>
#include <cstdint>
#include <cstring>

#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/cell_sail_internal.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellPamf";

constexpr std::uint32_t kCodecCount = 6;  // PamfCodec::Avc .. UserData

// Block layout (big-endian words): magic, total stream count, then one count per PamfCodec.
constexpr std::uint32_t kDatabaseMagic = 0x44414c44;  // "DALD"
constexpr std::uint32_t kDatabaseTotal = 4;
constexpr std::uint32_t kDatabaseCounts = 8;
static_assert(kDatabaseCounts + 4 * kCodecCount <= sail::kPamfReaderSize);

std::uint32_t countStreams(const std::array<std::uint32_t, kCodecCount>& counts, std::uint32_t type) {
    using sail::kPamfStreamTypeAudio;
    using sail::kPamfStreamTypeUnknown;
    using sail::kPamfStreamTypeVideo;
    const auto of = [&](PamfCodec codec) { return counts[static_cast<std::size_t>(codec)]; };
    switch (static_cast<std::int32_t>(type)) {
        case kPamfStreamTypeVideo: return of(PamfCodec::Avc) + of(PamfCodec::M2v);
        case kPamfStreamTypeAudio: return of(PamfCodec::Atrac3plus) + of(PamfCodec::Ac3) + of(PamfCodec::Lpcm);
        case kPamfStreamTypeUnknown: return of(PamfCodec::Lpcm);
        default: return type < kCodecCount ? counts[type] : 0;
    }
}

// cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* pSelf, u8 streamType)
void pamfGetNumberOfSpecificStreams(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t reader = argU32(ctx, 0);
    const auto type = static_cast<std::uint32_t>(ctx->gpr[4] & 0xff);
    std::uint32_t found = 0;
    if (sail_hle::guestMapped(reader, sail::kPamfReaderSize) && dalRead32(base, reader) == kDatabaseMagic) {
        std::array<std::uint32_t, kCodecCount> counts{};
        for (std::uint32_t i = 0; i < kCodecCount; i++) {
            counts[i] = dalRead32(base, reader + kDatabaseCounts + 4 * i);
        }
        found = countStreams(counts, type);
    } else {
        logOnce(LogLevel::Warn, "pamf-reader-unknown",
                "dal: pamf: reader {:#010x} is not a database from cellSailDescriptorCreateDatabase", reader);
    }
    ctx->gpr[3] = found > 0xff ? 0xff : found;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0xd0230671, "cellPamfReaderGetNumberOfSpecificStreams", pamfGetNumberOfSpecificStreams},
};

}  // namespace

std::span<const HleEntry> cellPamfHandlers() { return kHandlers; }

namespace sail_hle {

bool writePamfDatabase(std::uint8_t* base, std::uint32_t addr, const PamfInfo& info) {
    if (!guestMapped(addr, sail::kPamfReaderSize)) return false;
    std::memset(base + addr, 0, sail::kPamfReaderSize);
    std::array<std::uint32_t, kCodecCount> counts{};
    for (const PamfStream& stream : info.streams) {
        const auto codec = static_cast<std::size_t>(stream.codec);
        if (codec < kCodecCount) counts[codec]++;
    }
    dalWrite32(base, addr, kDatabaseMagic);
    dalWrite32(base, addr + kDatabaseTotal, static_cast<std::uint32_t>(info.streams.size()));
    for (std::uint32_t i = 0; i < kCodecCount; i++) {
        dalWrite32(base, addr + kDatabaseCounts + 4 * i, counts[i]);
    }
    return true;
}

bool clearPamfDatabase(std::uint8_t* base, std::uint32_t addr) {
    if (!guestMapped(addr, sail::kPamfReaderSize) || dalRead32(base, addr) != kDatabaseMagic) return false;
    std::memset(base + addr, 0, sail::kPamfReaderSize);
    return true;
}

}  // namespace sail_hle
}  // namespace dal

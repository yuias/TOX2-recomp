#include "media/at3_file.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "media/atrac_decoder.h"

namespace dal {

namespace {

constexpr std::uint16_t kFormatExtensible = 0xFFFE;
constexpr std::size_t kRiffHeaderSize = 12;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::size_t kFmtBaseSize = 16;        // WAVEFORMATEX without cbSize
constexpr std::size_t kFmtExtensibleSize = 40;  // WAVEFORMATEXTENSIBLE
constexpr std::size_t kSmplHeaderSize = 36;
constexpr std::size_t kSmplLoopSize = 24;
constexpr std::uint32_t kMaxChannels = 8;

// E923AABF-CB58-4471-A119-FFFA01E4CE62 in its in-file (little-endian field) byte order.
constexpr std::array<std::uint8_t, 16> kAtrac3plusGuid = {0xbf, 0xaa, 0x23, 0xe9, 0x58, 0xcb, 0x71, 0x44,
                                                          0xa1, 0x19, 0xff, 0xfa, 0x01, 0xe4, 0xce, 0x62};

std::uint32_t le16(const std::uint8_t* p) { return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8); }
std::uint32_t le32(const std::uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }

bool isTag(const std::uint8_t* p, const char (&tag)[5]) { return std::memcmp(p, tag, 4) == 0; }

std::expected<void, std::string> parseFmt(std::span<const std::uint8_t> body, At3Info& info) {
    if (body.size() < kFmtBaseSize) return std::unexpected("fmt chunk is too short");
    const std::uint32_t tag = le16(body.data());
    if (tag != kFormatExtensible) {
        return std::unexpected(
                std::format("unsupported format tag 0x{:04x} (only WAVE_FORMAT_EXTENSIBLE ATRAC3plus)", tag));
    }
    if (body.size() < kFmtExtensibleSize) return std::unexpected("fmt chunk is too short for WAVE_FORMAT_EXTENSIBLE");
    if (le16(body.data() + 16) < kFmtExtensibleSize - 18) return std::unexpected("fmt cbSize is too small");
    if (std::memcmp(body.data() + 24, kAtrac3plusGuid.data(), kAtrac3plusGuid.size()) != 0) {
        return std::unexpected("sub-format is not ATRAC3plus");
    }
    info.channels = le16(body.data() + 2);
    info.sample_rate = le32(body.data() + 4);
    info.bytes_per_second = le32(body.data() + 8);
    info.block_align = le16(body.data() + 12);
    if (info.channels == 0 || info.channels > kMaxChannels) {
        return std::unexpected(std::format("unsupported channel count {}", info.channels));
    }
    if (info.sample_rate == 0) return std::unexpected("sample rate is zero");
    if (info.block_align == 0) return std::unexpected("block_align is zero");
    return {};
}

void parseFact(std::span<const std::uint8_t> body, At3Info& info) {
    if (body.size() >= 4) info.total_samples = le32(body.data());
    info.delay_samples = body.size() >= 12 ? le32(body.data() + 8) : 0;
}

std::expected<void, std::string> parseSmpl(std::span<const std::uint8_t> body, At3Info& info) {
    if (body.size() < kSmplHeaderSize) return std::unexpected("smpl chunk is too short");
    const std::uint32_t loops = le32(body.data() + 28);
    if (loops == 0) return {};
    if (body.size() - kSmplHeaderSize < kSmplLoopSize) return std::unexpected("smpl loop list is truncated");
    const std::uint8_t* loop = body.data() + kSmplHeaderSize;
    info.loop = At3Loop{le32(loop + 8), le32(loop + 12)};
    return {};
}

std::unique_ptr<AtracDecoderFactory> g_factory;

}  // namespace

std::expected<At3Info, std::string> parseAt3Header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kRiffHeaderSize) return std::unexpected("truncated RIFF header");
    if (!isTag(bytes.data(), "RIFF") || !isTag(bytes.data() + 8, "WAVE"))
        return std::unexpected("not a RIFF/WAVE file");

    At3Info info;
    bool have_fmt = false;
    std::uint64_t pos = kRiffHeaderSize;
    for (;;) {
        if (pos + kChunkHeaderSize > bytes.size()) return std::unexpected("truncated before the data chunk");
        const std::uint8_t* header = bytes.data() + pos;
        const std::uint32_t size = le32(header + 4);
        const std::uint64_t body = pos + kChunkHeaderSize;

        if (isTag(header, "data")) {
            if (!have_fmt) return std::unexpected("data chunk before fmt chunk");
            if (body > UINT32_MAX) return std::unexpected("data chunk offset exceeds 32 bits");
            info.data_offset = static_cast<std::uint32_t>(body);
            info.data_size = size;
            return info;
        }

        if (body + size > bytes.size()) {
            return std::unexpected(
                    std::format("chunk '{}' is truncated", std::string_view(reinterpret_cast<const char*>(header), 4)));
        }
        const auto chunk = bytes.subspan(static_cast<std::size_t>(body), size);
        if (isTag(header, "fmt ")) {
            if (auto result = parseFmt(chunk, info); !result) return std::unexpected(std::move(result.error()));
            have_fmt = true;
        } else if (isTag(header, "fact")) {
            parseFact(chunk, info);
        } else if (isTag(header, "smpl")) {
            if (auto result = parseSmpl(chunk, info); !result) return std::unexpected(std::move(result.error()));
        }
        // RIFF chunks are padded to an even size; the pad byte is not counted in the chunk size.
        pos = body + size + (size & 1);
    }
}

std::vector<std::pair<std::uint64_t, std::uint32_t>> scanRiffEntries(std::span<const std::uint8_t> bytes) {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> entries;
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i + kRiffHeaderSize <= n) {
        const void* hit = std::memchr(bytes.data() + i, 'R', n - kRiffHeaderSize + 1 - i);
        if (hit == nullptr) break;
        i = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - bytes.data());
        if (!isTag(bytes.data() + i, "RIFF") || !isTag(bytes.data() + i + 8, "WAVE")) {
            ++i;
            continue;
        }
        const std::uint32_t size = le32(bytes.data() + i + 4);
        entries.emplace_back(i, size);
        // Skip the entry's own bytes: a payload may contain the magic by chance.
        const std::uint64_t end = std::uint64_t{i} + kChunkHeaderSize + size;
        i = end <= n ? static_cast<std::size_t>(end) : i + 4;
    }
    return entries;
}

void setAtracDecoderFactory(std::unique_ptr<AtracDecoderFactory> factory) { g_factory = std::move(factory); }

AtracDecoderFactory* atracDecoderFactory() { return g_factory.get(); }

}  // namespace dal

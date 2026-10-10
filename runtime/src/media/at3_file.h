#pragma once

// ATRAC3plus RIFF/WAVE files as stored in the game's archives. Pure host code with no codec
// dependency; the decoder interface is in atrac_decoder.h.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dal {

inline constexpr std::uint32_t kAtrac3plusFrameSamples = 2048;

// `smpl` loop, inclusive. Positions are in decoder-output samples, so they count the encoder delay.
struct At3Loop {
    std::uint32_t start_sample = 0, end_sample = 0;
};

struct At3Info {
    std::uint32_t channels = 0, sample_rate = 0, block_align = 0, bytes_per_second = 0;
    std::uint32_t data_offset = 0, data_size = 0;  // offset in the parsed bytes and size of the `data` payload
    std::uint32_t total_samples = 0;               // `fact`, first dword
    // Encoder delay: the third `fact` dword, 0 when `fact` is shorter. The valid audio is the decoder
    // output samples [delay, delay + total). FFmpeg's RIFF demuxer reads only the first dword, so this
    // convention comes from the game's files: loop positions lie in this output range and the loop end
    // is delay + total - 1 (off by one in 2 of 53 Se.sdat loops; see `dal_audiotool at3scan`).
    std::uint32_t delay_samples = 0;
    std::optional<At3Loop> loop;  // first loop of `smpl`
    std::uint32_t frameCount() const { return block_align != 0 ? data_size / block_align : 0; }
};

// Needs the bytes up to and including the `data` chunk header (the payload itself is not read);
// ignores unknown chunks. Only WAVE_FORMAT_EXTENSIBLE with the ATRAC3plus sub-format GUID is
// accepted; a `smpl` chunk after `data` is not seen. Anything else, or truncated input, is an error.
std::expected<At3Info, std::string> parseAt3Header(std::span<const std::uint8_t> bytes);

// Offsets of "RIFF....WAVE" entries in an archive with their declared RIFF sizes (the 32-bit field
// after the magic). An entry whose declared size fits in the archive is skipped as a whole.
std::vector<std::pair<std::uint64_t, std::uint32_t>> scanRiffEntries(std::span<const std::uint8_t> bytes);

}  // namespace dal

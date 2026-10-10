// WAV files for the audio dump and for tools and tests: a float32 writer whose header stays valid
// while the file grows, a reader for the formats the tools meet, and per-window level statistics.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace dal {

// Frames between header patches. A file cut short (the process ends with std::_Exit, which flushes
// nothing) is valid up to the last multiple of this many frames.
inline constexpr std::uint64_t kWavPatchIntervalFrames = 48000;

// A frame counts as non-silent when any channel exceeds this magnitude (-80 dBFS).
inline constexpr float kWavSilenceThreshold = 1e-4f;

// Writes WAVE_FORMAT_IEEE_FLOAT, 32-bit. The RIFF, `fact` and `data` sizes are patched (and flushed)
// at every kWavPatchIntervalFrames and by finish(). Writes and finish() take an internal mutex, so
// finish() may run on the stopping thread while the engine thread is still writing.
class WavWriter {
public:
    // Creates (truncates) `path` and writes a header describing an empty file.
    static std::expected<std::unique_ptr<WavWriter>, std::string> create(const std::filesystem::path& path,
                                                                         std::uint32_t sample_rate,
                                                                         std::uint16_t channels);

    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Appends interleaved samples (a whole number of frames). False once the file is finished, has
    // failed, or when `samples` is not a whole number of frames; the file is left as it was then.
    bool write(std::span<const float> samples);
    // Patches the header with the final sizes and closes the file. Idempotent. False if the file
    // could not be completed.
    bool finish();
    std::uint64_t framesWritten() const;

private:
    WavWriter(std::ofstream file, std::uint32_t sample_rate, std::uint16_t channels);

    bool patchHeaderLocked();

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::uint32_t sample_rate_;
    std::uint16_t channels_;
    std::uint64_t frames_ = 0;
    bool finished_ = false;
    bool failed_ = false;
};

struct WavData {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::vector<float> samples;  // interleaved, s16 scaled to [-1, 1)

    std::uint64_t frames() const { return channels != 0 ? samples.size() / channels : 0; }
};

// Accepts PCM s16 and IEEE float32, mono or stereo. Unknown chunks are skipped. A `data` size that
// runs past the end of the buffer (a file cut off mid-write) is clamped to the bytes present.
std::expected<WavData, std::string> parseWav(std::span<const std::uint8_t> bytes);
std::expected<WavData, std::string> readWav(const std::filesystem::path& path);

struct WavWindowStats {
    std::uint64_t start_frame = 0;
    std::uint64_t end_frame = 0;  // exclusive
    double rms_db = 0;            // dBFS over all channels; -infinity for digital silence
    double peak_db = 0;
    std::uint64_t non_silent_frames = 0;
};

// One entry per `window_seconds` of audio; the last one covers the remainder. Empty for empty
// input, a zero rate or channel count, or a window shorter than one frame.
std::vector<WavWindowStats> wavStats(std::span<const float> samples, std::uint32_t sample_rate, std::uint32_t channels,
                                     double window_seconds);

}  // namespace dal

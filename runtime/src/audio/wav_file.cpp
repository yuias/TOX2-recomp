#include "audio/wav_file.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>

namespace dal {

namespace {

static_assert(std::endian::native == std::endian::little, "WAV fields are written in host order");

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;

// RIFF/WAVE + 18-byte fmt (cbSize 0) + fact + data header. Non-PCM formats need the longer fmt and
// the fact chunk.
constexpr std::size_t kHeaderBytes = 58;
constexpr std::size_t kRiffSizeOffset = 4;
constexpr std::size_t kFactFramesOffset = 46;
constexpr std::size_t kDataSizeOffset = 54;
constexpr std::uint64_t kMaxDataBytes = std::numeric_limits<std::uint32_t>::max() - (kHeaderBytes - 8);

template <typename T>
void putLe(std::array<std::uint8_t, kHeaderBytes>& header, std::size_t offset, T value) {
    std::memcpy(header.data() + offset, &value, sizeof(T));
}

void putTag(std::array<std::uint8_t, kHeaderBytes>& header, std::size_t offset, std::string_view tag) {
    std::memcpy(header.data() + offset, tag.data(), 4);
}

std::array<std::uint8_t, kHeaderBytes> makeHeader(std::uint32_t sample_rate, std::uint16_t channels,
                                                  std::uint64_t frames) {
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * sizeof(float));
    const std::uint64_t data_bytes = std::min<std::uint64_t>(frames * block_align, kMaxDataBytes);
    std::array<std::uint8_t, kHeaderBytes> header{};
    putTag(header, 0, "RIFF");
    putLe<std::uint32_t>(header, kRiffSizeOffset, static_cast<std::uint32_t>(kHeaderBytes - 8 + data_bytes));
    putTag(header, 8, "WAVE");
    putTag(header, 12, "fmt ");
    putLe<std::uint32_t>(header, 16, 18);
    putLe<std::uint16_t>(header, 20, kFormatFloat);
    putLe<std::uint16_t>(header, 22, channels);
    putLe<std::uint32_t>(header, 24, sample_rate);
    putLe<std::uint32_t>(header, 28, sample_rate * block_align);
    putLe<std::uint16_t>(header, 32, block_align);
    putLe<std::uint16_t>(header, 34, 32);
    putLe<std::uint16_t>(header, 36, 0);
    putTag(header, 38, "fact");
    putLe<std::uint32_t>(header, 42, 4);
    putLe<std::uint32_t>(header, kFactFramesOffset, static_cast<std::uint32_t>(frames));
    putTag(header, 50, "data");
    putLe<std::uint32_t>(header, kDataSizeOffset, static_cast<std::uint32_t>(data_bytes));
    return header;
}

template <typename T>
T getLe(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

bool hasTag(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view tag) {
    return std::memcmp(bytes.data() + offset, tag.data(), 4) == 0;
}

double toDb(double amplitude) {
    return amplitude > 0 ? 20.0 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}

}  // namespace

std::expected<std::unique_ptr<WavWriter>, std::string> WavWriter::create(const std::filesystem::path& path,
                                                                         std::uint32_t sample_rate,
                                                                         std::uint16_t channels) {
    if (channels == 0 || sample_rate == 0) return std::unexpected("invalid WAV format");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected(std::format("cannot create {}", path.string()));
    std::unique_ptr<WavWriter> writer(new WavWriter(std::move(file), sample_rate, channels));
    if (!writer->patchHeaderLocked()) return std::unexpected(std::format("cannot write {}", path.string()));
    return writer;
}

WavWriter::WavWriter(std::ofstream file, std::uint32_t sample_rate, std::uint16_t channels)
    : file_(std::move(file)), sample_rate_(sample_rate), channels_(channels) {}

WavWriter::~WavWriter() { finish(); }

bool WavWriter::patchHeaderLocked() {
    const auto header = makeHeader(sample_rate_, channels_, frames_);
    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    file_.seekp(0, std::ios::end);
    // The flush also pushes the samples written since the last patch, which the sizes now cover.
    file_.flush();
    return static_cast<bool>(file_);
}

bool WavWriter::write(std::span<const float> samples) {
    const std::lock_guard lock(mutex_);
    if (finished_ || failed_ || samples.size() % channels_ != 0) return false;

    std::uint64_t remaining = samples.size() / channels_;
    if ((frames_ + remaining) * channels_ * sizeof(float) > kMaxDataBytes) {
        failed_ = true;
        return false;
    }
    while (remaining != 0) {
        // Split at the patch boundary so a cut file ends exactly on a multiple of the interval.
        const std::uint64_t chunk = std::min(remaining, kWavPatchIntervalFrames - frames_ % kWavPatchIntervalFrames);
        const std::size_t count = static_cast<std::size_t>(chunk * channels_);
        file_.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(count * sizeof(float)));
        samples = samples.subspan(count);
        frames_ += chunk;
        remaining -= chunk;
        if (frames_ % kWavPatchIntervalFrames == 0 && !patchHeaderLocked()) failed_ = true;
        if (!file_) failed_ = true;
        if (failed_) return false;
    }
    return true;
}

bool WavWriter::finish() {
    const std::lock_guard lock(mutex_);
    if (finished_) return !failed_;
    finished_ = true;
    if (!failed_ && !patchHeaderLocked()) failed_ = true;
    file_.close();
    if (!file_) failed_ = true;
    return !failed_;
}

std::uint64_t WavWriter::framesWritten() const {
    const std::lock_guard lock(mutex_);
    return frames_;
}

std::expected<WavData, std::string> parseWav(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || !hasTag(bytes, 0, "RIFF") || !hasTag(bytes, 8, "WAVE")) {
        return std::unexpected("not a RIFF/WAVE file");
    }

    WavData wav;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    bool have_format = false;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;
    bool have_data = false;

    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::size_t payload = pos + 8;
        const std::size_t available = bytes.size() - payload;
        const std::size_t size = std::min<std::size_t>(getLe<std::uint32_t>(bytes, pos + 4), available);
        if (hasTag(bytes, pos, "fmt ")) {
            if (size < 16) return std::unexpected("fmt chunk is too short");
            format = getLe<std::uint16_t>(bytes, payload);
            wav.channels = getLe<std::uint16_t>(bytes, payload + 2);
            wav.sample_rate = getLe<std::uint32_t>(bytes, payload + 4);
            bits = getLe<std::uint16_t>(bytes, payload + 14);
            have_format = true;
        } else if (hasTag(bytes, pos, "data")) {
            data_offset = payload;
            data_size = size;
            have_data = true;
            break;
        }
        pos = payload + size + (size & 1);
    }

    if (!have_format) return std::unexpected("no fmt chunk");
    if (!have_data) return std::unexpected("no data chunk");
    const bool is_float = format == kFormatFloat && bits == 32;
    const bool is_s16 = format == kFormatPcm && bits == 16;
    if (!is_float && !is_s16) {
        return std::unexpected(std::format("unsupported format (tag {}, {} bits)", format, bits));
    }
    if (wav.channels < 1 || wav.channels > 2) {
        return std::unexpected(std::format("unsupported channel count {}", wav.channels));
    }
    if (wav.sample_rate == 0) return std::unexpected("sample rate is zero");

    const std::size_t sample_bytes = bits / 8;
    const std::size_t count = data_size / sample_bytes / wav.channels * wav.channels;
    wav.samples.resize(count);
    const std::uint8_t* src = bytes.data() + data_offset;
    if (is_float) {
        std::memcpy(wav.samples.data(), src, count * sizeof(float));
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            std::int16_t value;
            std::memcpy(&value, src + i * sizeof(value), sizeof(value));
            wav.samples[i] = static_cast<float>(value) / 32768.0f;
        }
    }
    return wav;
}

std::expected<WavData, std::string> readWav(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::unexpected(std::format("cannot open {}", path.string()));
    const std::streamsize size = file.tellg();
    if (size < 0) return std::unexpected(std::format("cannot read {}", path.string()));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(std::format("cannot read {}", path.string()));
    }
    return parseWav(bytes);
}

std::vector<WavWindowStats> wavStats(std::span<const float> samples, std::uint32_t sample_rate, std::uint32_t channels,
                                     double window_seconds) {
    std::vector<WavWindowStats> result;
    if (sample_rate == 0 || channels == 0 || !(window_seconds > 0)) return result;
    const double window_frames_real = std::round(window_seconds * sample_rate);
    if (window_frames_real < 1) return result;
    const std::uint64_t window_frames = window_frames_real >= 1e18 ? std::uint64_t{1'000'000'000'000'000'000}
                                                                   : static_cast<std::uint64_t>(window_frames_real);
    const std::uint64_t total_frames = samples.size() / channels;

    for (std::uint64_t start = 0; start < total_frames; start += window_frames) {
        const std::uint64_t end = std::min(total_frames, start + window_frames);
        double sum_squares = 0;
        float peak = 0;
        std::uint64_t non_silent = 0;
        for (std::uint64_t frame = start; frame < end; ++frame) {
            float frame_peak = 0;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                const float value = samples[frame * channels + ch];
                sum_squares += static_cast<double>(value) * value;
                frame_peak = std::max(frame_peak, std::fabs(value));
            }
            peak = std::max(peak, frame_peak);
            if (frame_peak > kWavSilenceThreshold) ++non_silent;
        }
        WavWindowStats stats;
        stats.start_frame = start;
        stats.end_frame = end;
        stats.rms_db = toDb(std::sqrt(sum_squares / static_cast<double>((end - start) * channels)));
        stats.peak_db = toDb(peak);
        stats.non_silent_frames = non_silent;
        result.push_back(stats);
    }
    return result;
}

}  // namespace dal

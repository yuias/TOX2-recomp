// tox2_audiotool: audio utilities for the runtime. Subcommands:
//   wavstat FILE [--window S]   per-window levels of a WAV file (the audio dump, tool output)
//   devices                     the active render devices and their mix formats
//   tone [--output none|HOST] [--seconds N] [--hz F] [--db L]   a sine through an output
//   play FILE.wav [--output none|HOST]                          a 48 kHz WAV through an output
//   at3scan FILE                the ATRAC3plus RIFF entries of an archive
//   at3 FILE [--offset N | --index N] [--wav OUT] [--hash] [--play]   decode one entry
// HOST is the device output of the platform: wasapi on Windows, coreaudio on macOS (the default).
// Exits 0 on success, 1 when the work fails, 2 on a usage error.

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/game_options.h"
#include "audio/audio_output.h"
#include "audio/stream_output.h"
#if defined(_WIN32)
#include "audio/wasapi_output.h"
#elif defined(__APPLE__)
#include "audio/coreaudio_output.h"
#endif
#include "audio/wav_file.h"
#include "core/sleeper.h"
#include "crypto/host_crypto.h"
#include "media/at3_file.h"
#include "media/atrac_decoder.h"
#include "media/ffmpeg_codecs.h"

namespace {

// How the usage text names the host's audio API; the option value is tox2::kHostAudioOutput.
#if defined(_WIN32)
constexpr std::string_view kHostAudioApi = "WASAPI";
#else
constexpr std::string_view kHostAudioApi = "CoreAudio";
#endif

struct Subcommand {
    std::string_view name;
    int (*run)(std::span<const std::string_view> args);
};

void printUsage() {
    const std::string host(tox2::kHostAudioOutput);
    const std::string api(kHostAudioApi);
    std::fprintf(
            stderr,
            "usage: tox2_audiotool wavstat FILE [--window S]\n"
            "       tox2_audiotool devices\n"
            "       tox2_audiotool tone [--output none|%s] [--seconds N] [--hz F] [--db L]\n"
            "       tox2_audiotool play FILE.wav [--output none|%s]\n"
            "       tox2_audiotool at3scan FILE\n"
            "       tox2_audiotool at3 FILE [--offset N | --index N] [--wav OUT] [--hash] [--play]\n"
            "                     [--expect-frames N] [--expect-samples N] [--expect-pcm SHA256]\n"
            "  wavstat   print RMS, peak and non-silent frame count per window of S seconds (default 1)\n"
            "  devices   list the active render devices with their mix format and device periods\n"
            "  tone      play a sine (default %s, 2 s, 440 Hz, -18 dBFS) and print the output statistics\n"
            "  play      play a 48 kHz mono or stereo WAV file and print the output statistics\n"
            "  at3scan   list the ATRAC3plus RIFF entries of an archive (offset, size, format, samples, delay, loop)\n"
            "  at3       decode one RIFF entry (--index N of the at3scan list, default 0, or the one at byte --offset N)\n"
            "            and cut it to the valid samples [delay, delay + samples); --hash prints the SHA-256 of the\n"
            "            interleaved little-endian float32 result, --wav writes it, --play plays it through %s\n",
            host.c_str(), host.c_str(), host.c_str(), api.c_str());
}

void printLine(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
}

bool parsePositive(std::string_view text, double& value) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end && value > 0 && std::isfinite(value);
}

int runWavstat(std::span<const std::string_view> args) {
    std::string_view file;
    double window = 1.0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--window") {
            if (i + 1 >= args.size() || !parsePositive(args[++i], window)) {
                std::fputs("error: --window needs a positive number of seconds\n", stderr);
                return 2;
            }
        } else if (file.empty() && !args[i].starts_with("--")) {
            file = args[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(args[i].size()), args[i].data());
            return 2;
        }
    }
    if (file.empty()) {
        std::fputs("error: wavstat needs a file\n", stderr);
        return 2;
    }

    const auto wav = tox2::readWav(std::filesystem::path(file));
    if (!wav) {
        std::fprintf(stderr, "error: %s\n", wav.error().c_str());
        return 1;
    }

    const auto windows = tox2::wavStats(wav->samples, wav->sample_rate, wav->channels, window);
    const double rate = wav->sample_rate;
    std::size_t non_silent_windows = 0;
    for (const auto& w : windows) {
        printLine(std::format("t {:.3f}-{:.3f} s: rms {:.1f} dBFS, peak {:.1f} dBFS, non-silent {} frames",
                              static_cast<double>(w.start_frame) / rate, static_cast<double>(w.end_frame) / rate,
                              w.rms_db, w.peak_db, w.non_silent_frames));
        if (w.non_silent_frames != 0) ++non_silent_windows;
    }
    printLine(std::format("wavstat: {:.3f} s, non-silent windows {} of {}", static_cast<double>(wav->frames()) / rate,
                          non_silent_windows, windows.size()));
    return 0;
}

int runDevices(std::span<const std::string_view> args) {
    if (!args.empty()) {
        std::fputs("error: devices takes no arguments\n", stderr);
        return 2;
    }
    const auto devices = tox2::listRenderDevices();
    if (!devices) {
        std::fprintf(stderr, "error: %s\n", devices.error().c_str());
        return 1;
    }
    printLine(std::format("render devices: {}", devices->size()));
    for (const auto& device : *devices) {
        printLine(std::format("{} {}{}", device.is_default ? "*" : "-", device.name,
                              device.is_default ? " (default)" : ""));
        printLine(std::format("    mix {} Hz, {} ch, {} bit {}; period default {:.1f} ms, min {:.1f} ms",
                              device.mix_rate, device.mix_channels, device.mix_bits,
                              device.mix_float ? "float" : "integer", device.default_period_ms, device.min_period_ms));
    }
    return devices->empty() ? 1 : 0;
}

using Clock = std::chrono::steady_clock;

bool parseNumber(std::string_view text, double& value) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end && std::isfinite(value);
}

// Takes the value of an option that must be one of the two output names.
bool parseOutputName(std::string_view text, std::string_view& output) {
    if (text != "none" && text != tox2::kHostAudioOutput) return false;
    output = text;
    return true;
}

std::unique_ptr<tox2::AudioOutput> makeHostOutput() {
#if defined(_WIN32)
    return tox2::makeWasapiOutput();
#else
    return tox2::makeCoreAudioOutput();
#endif
}

std::unique_ptr<tox2::AudioOutput> openOutput(std::string_view kind) {
    std::unique_ptr<tox2::AudioOutput> output =
            kind == tox2::kHostAudioOutput ? makeHostOutput() : tox2::makeNullAudioOutput();
    std::string error;
    if (!output->open(error)) {
        std::fprintf(stderr, "error: %.*s output: %s\n", static_cast<int>(kind.size()), kind.data(), error.c_str());
        return nullptr;
    }
    printLine(std::format("output: {} ({})", output->name(), output->description()));
    return output;
}

// Submits `blocks` blocks at the cellAudio rate, each due at an absolute time so that a late wake-up
// does not shift the schedule, then waits for the device to take them and prints the statistics.
int playBlocks(tox2::AudioOutput& output, std::uint64_t blocks,
               const std::function<void(std::uint64_t block, std::span<float> samples)>& fill) {
    tox2::Sleeper sleeper;
    tox2::AudioBlock samples;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t i = 0; i < blocks; ++i) {
        const Clock::time_point due =
                start + std::chrono::nanoseconds(static_cast<std::int64_t>(i * tox2::kAudioBlockFrames *
                                                                           1'000'000'000ULL / tox2::kAudioSampleRate));
        sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(due - Clock::now()));
        fill(i, samples);
        output.submit(samples);
    }

    // frames_played counts frames handed to the device. The statistics are taken at that point: once
    // the last block is out, the device running dry is the end of the stream, not an underrun.
    const std::uint64_t wanted_frames = blocks * tox2::kAudioBlockFrames;
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(2);
    while (output.stats().frames_played < wanted_frames && Clock::now() < give_up) {
        sleeper.sleep(std::chrono::milliseconds(2));
    }
    const tox2::AudioOutputStats stats = output.stats();
    // Let the device play what it still holds before the output is closed.
    sleeper.sleep(std::chrono::milliseconds(200));

    printLine(std::format(
            "stats: submitted {} blocks, dropped {}, underrun {} frames, played {} frames, device errors {}",
            stats.blocks_submitted, stats.blocks_dropped, stats.underrun_frames, stats.frames_played,
            stats.device_errors));
    return 0;
}

int runTone(std::span<const std::string_view> args) {
    std::string_view output_kind = tox2::kHostAudioOutput;
    double seconds = 2.0;
    double hz = 440.0;
    double db = -18.0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view option = args[i];
        if (option != "--output" && option != "--seconds" && option != "--hz" && option != "--db") {
            std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(option.size()), option.data());
            return 2;
        }
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "error: %.*s needs a value\n", static_cast<int>(option.size()), option.data());
            return 2;
        }
        const std::string_view text = args[++i];
        bool ok = false;
        if (option == "--output") {
            ok = parseOutputName(text, output_kind);
        } else if (option == "--seconds") {
            ok = parsePositive(text, seconds);
        } else if (option == "--hz") {
            ok = parsePositive(text, hz) && hz < tox2::kAudioSampleRate / 2.0;
        } else {
            ok = parseNumber(text, db) && db <= 0.0;
        }
        if (!ok) {
            std::fprintf(stderr, "error: bad value for %.*s: %.*s\n", static_cast<int>(option.size()), option.data(),
                         static_cast<int>(text.size()), text.data());
            return 2;
        }
    }

    auto output = openOutput(output_kind);
    if (!output) return 1;
    const float amplitude = static_cast<float>(std::pow(10.0, db / 20.0));
    printLine(std::format("tone: {} Hz, {} dBFS, {} s", hz, db, seconds));
    const auto blocks =
            static_cast<std::uint64_t>(std::ceil(seconds * tox2::kAudioSampleRate / tox2::kAudioBlockFrames));
    return playBlocks(*output, blocks, [&](std::uint64_t block, std::span<float> samples) {
        for (std::size_t frame = 0; frame < tox2::kAudioBlockFrames; ++frame) {
            const double cycles =
                    hz * static_cast<double>(block * tox2::kAudioBlockFrames + frame) / tox2::kAudioSampleRate;
            const float value =
                    amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * (cycles - std::floor(cycles))));
            samples[2 * frame] = value;
            samples[2 * frame + 1] = value;
        }
    });
}

// Plays interleaved mono or stereo samples at the cellAudio rate through `output`.
int playInterleaved(tox2::AudioOutput& output, std::span<const float> samples, std::uint32_t channels) {
    const std::uint64_t frames = samples.size() / channels;
    printLine(std::format("play: {:.3f} s, {} ch", static_cast<double>(frames) / tox2::kAudioSampleRate, channels));
    const auto blocks = (frames + tox2::kAudioBlockFrames - 1) / tox2::kAudioBlockFrames;
    return playBlocks(output, blocks, [&](std::uint64_t block, std::span<float> out) {
        for (std::size_t frame = 0; frame < tox2::kAudioBlockFrames; ++frame) {
            const std::uint64_t index = block * tox2::kAudioBlockFrames + frame;
            float left = 0.0f;
            float right = 0.0f;
            if (index < frames) {
                left = samples[index * channels];
                right = channels > 1 ? samples[index * channels + 1] : left;
            }
            out[2 * frame] = std::clamp(left, -1.0f, 1.0f);
            out[2 * frame + 1] = std::clamp(right, -1.0f, 1.0f);
        }
    });
}

int runPlay(std::span<const std::string_view> args) {
    std::string_view file;
    std::string_view output_kind = tox2::kHostAudioOutput;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--output") {
            if (i + 1 >= args.size() || !parseOutputName(args[++i], output_kind)) {
                std::fprintf(stderr, "error: --output needs none or %.*s\n",
                             static_cast<int>(tox2::kHostAudioOutput.size()), tox2::kHostAudioOutput.data());
                return 2;
            }
        } else if (file.empty() && !args[i].starts_with("--")) {
            file = args[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(args[i].size()), args[i].data());
            return 2;
        }
    }
    if (file.empty()) {
        std::fputs("error: play needs a file\n", stderr);
        return 2;
    }

    const auto wav = tox2::readWav(std::filesystem::path(file));
    if (!wav) {
        std::fprintf(stderr, "error: %s\n", wav.error().c_str());
        return 1;
    }
    if (wav->sample_rate != tox2::kAudioSampleRate) {
        std::fprintf(stderr, "error: play needs a %u Hz file, this one has %u Hz\n", tox2::kAudioSampleRate,
                     wav->sample_rate);
        return 1;
    }

    auto output = openOutput(output_kind);
    if (!output) return 1;
    return playInterleaved(*output, wav->samples, wav->channels);
}

// Reads a whole file; prints the reason and returns false on failure.
bool readWholeFile(std::string_view path, std::vector<std::uint8_t>& bytes) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!stream) {
        std::fprintf(stderr, "error: cannot open %.*s\n", static_cast<int>(path.size()), path.data());
        return false;
    }
    bytes.resize(static_cast<std::size_t>(stream.tellg()));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        std::fputs("error: read failed\n", stderr);
        return false;
    }
    return true;
}

int runAt3scan(std::span<const std::string_view> args) {
    if (args.size() != 1 || args[0].starts_with("--")) {
        std::fputs("error: at3scan needs exactly one file\n", stderr);
        return 2;
    }
    std::vector<std::uint8_t> bytes;
    if (!readWholeFile(args[0], bytes)) return 1;

    const auto entries = tox2::scanRiffEntries(bytes);
    std::size_t parsed = 0, errors = 0, looped = 0, mono = 0, stereo = 0;
    std::size_t unaligned = 0, overrun = 0, loop_start_valid = 0, loop_end_is_valid_end = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto [offset, size] = entries[index];
        // The entry's bytes, clamped to the archive so that a bad size cannot read past the end.
        const std::uint64_t available = std::min<std::uint64_t>(std::uint64_t{size} + 8, bytes.size() - offset);
        const auto info = tox2::parseAt3Header(std::span<const std::uint8_t>(bytes).subspan(offset, available));
        if (!info) {
            ++errors;
            printLine(std::format("{} offset {} size {}: error: {}", index, offset, size, info.error()));
            continue;
        }
        ++parsed;
        (info->channels == 1 ? mono : stereo) += 1;

        std::string loop = "none";
        if (info->loop) {
            ++looped;
            loop = std::format("{}-{}", info->loop->start_sample, info->loop->end_sample);
            // The valid audio is [delay, delay + total); a loop start is somewhere inside it and the loop
            // end is normally its last sample.
            if (info->loop->start_sample >= info->delay_samples &&
                std::uint64_t{info->loop->start_sample} < std::uint64_t{info->delay_samples} + info->total_samples) {
                ++loop_start_valid;
            }
            if (std::uint64_t{info->loop->end_sample} + 1 == std::uint64_t{info->delay_samples} + info->total_samples) {
                ++loop_end_is_valid_end;
            }
        }
        std::string flags;
        if (info->data_size % info->block_align != 0) {
            ++unaligned;
            flags += " data-not-multiple-of-block_align";
        }
        if (std::uint64_t{info->data_offset} + info->data_size > available) {
            ++overrun;
            flags += " data-past-entry-end";
        }
        printLine(std::format("{} offset {} size {} ch {} rate {} block_align {} samples {} delay {} loop {}{}", index,
                              offset, size, info->channels, info->sample_rate, info->block_align, info->total_samples,
                              info->delay_samples, loop, flags));
    }
    printLine(std::format("at3scan: {} entries, {} parsed, {} errors; {} mono, {} stereo; {} with a loop",
                          entries.size(), parsed, errors, mono, stereo, looped));
    printLine(std::format(
            "at3scan: loop start inside [delay, delay + samples) in {} of {}; loop end == delay + samples - 1 in {} of {}; "
            "data size not a multiple of block_align in {}; data past the entry end in {}",
            loop_start_valid, looped, loop_end_is_valid_end, looped, unaligned, overrun));
    return errors == 0 ? 0 : 1;
}

bool parseUnsigned(std::string_view text, std::uint64_t& value) {
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

// Lower-case hex SHA-256 of `data`; empty when the hash is unavailable.
std::string sha256Hex(std::span<const std::uint8_t> data) {
    tox2::crypto::Sha256Digest digest{};
    if (!tox2::crypto::sha256(data, digest)) return {};
    std::string text;
    for (const std::uint8_t byte : digest) text += std::format("{:02x}", byte);
    return text;
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

struct At3Options {
    std::string_view file;
    std::optional<std::uint64_t> index, offset;
    std::string_view wav;
    bool hash = false, play = false;
    std::optional<std::uint64_t> expect_frames, expect_samples;
    std::string expect_pcm;  // lower-case hex
};

// Returns 0 when the arguments are valid, otherwise the exit code after printing the reason.
int parseAt3Options(std::span<const std::string_view> args, At3Options& options) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view option = args[i];
        if (option == "--hash") {
            options.hash = true;
        } else if (option == "--play") {
            options.play = true;
        } else if (option == "--offset" || option == "--index" || option == "--wav" || option == "--expect-frames" ||
                   option == "--expect-samples" || option == "--expect-pcm") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: %.*s needs a value\n", static_cast<int>(option.size()), option.data());
                return 2;
            }
            const std::string_view text = args[++i];
            std::uint64_t number = 0;
            bool ok = true;
            if (option == "--wav") {
                options.wav = text;
            } else if (option == "--expect-pcm") {
                options.expect_pcm = std::string(text);
                for (char& c : options.expect_pcm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                ok = options.expect_pcm.size() == 64;
            } else {
                ok = parseUnsigned(text, number);
                if (option == "--offset") {
                    options.offset = number;
                } else if (option == "--index") {
                    options.index = number;
                } else if (option == "--expect-frames") {
                    options.expect_frames = number;
                } else {
                    options.expect_samples = number;
                }
            }
            if (!ok) {
                std::fprintf(stderr, "error: bad value for %.*s: %.*s\n", static_cast<int>(option.size()),
                             option.data(), static_cast<int>(text.size()), text.data());
                return 2;
            }
        } else if (options.file.empty() && !option.starts_with("--")) {
            options.file = option;
        } else {
            std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(option.size()), option.data());
            return 2;
        }
    }
    if (options.file.empty()) {
        std::fputs("error: at3 needs a file\n", stderr);
        return 2;
    }
    if (options.index && options.offset) {
        std::fputs("error: --index and --offset exclude each other\n", stderr);
        return 2;
    }
    return 0;
}

int runAt3(std::span<const std::string_view> args) {
    At3Options options;
    if (const int status = parseAt3Options(args, options); status != 0) return status;

    std::vector<std::uint8_t> bytes;
    if (!readWholeFile(options.file, bytes)) return 1;

    // The entry is the declared RIFF size plus its 8-byte header, clamped to the file.
    std::uint64_t offset = 0, size = 0;
    if (options.offset) {
        offset = *options.offset;
        if (offset + 12 > bytes.size() || std::memcmp(&bytes[offset], "RIFF", 4) != 0 ||
            std::memcmp(&bytes[offset + 8], "WAVE", 4) != 0) {
            std::fprintf(stderr, "error: no RIFF/WAVE entry at offset %llu\n", static_cast<unsigned long long>(offset));
            return 1;
        }
        size = readLe32(&bytes[offset + 4]);
    } else {
        const auto entries = tox2::scanRiffEntries(bytes);
        const std::uint64_t index = options.index.value_or(0);
        if (index >= entries.size()) {
            std::fprintf(stderr, "error: entry %llu does not exist, the file has %zu\n",
                         static_cast<unsigned long long>(index), entries.size());
            return 1;
        }
        offset = entries[index].first;
        size = entries[index].second;
    }
    const std::span<const std::uint8_t> entry = std::span<const std::uint8_t>(bytes).subspan(
            offset, std::min<std::uint64_t>(size + 8, bytes.size() - offset));
    const auto info = tox2::parseAt3Header(entry);
    if (!info) {
        std::fprintf(stderr, "error: %s\n", info.error().c_str());
        return 1;
    }
    printLine(std::format("at3: offset {} size {}: ch {} rate {} block_align {} fact samples {} delay {}", offset, size,
                          info->channels, info->sample_rate, info->block_align, info->total_samples,
                          info->delay_samples));

    std::string error;
    auto factory = tox2::createFfmpegAtracDecoderFactory(error);
    if (!factory) {
        std::fprintf(stderr, "error: no decoder: %s\n", error.c_str());
        return 1;
    }
    auto decoder = factory->create(*info, error);
    if (!decoder) {
        std::fprintf(stderr, "error: cannot create the decoder: %s\n", error.c_str());
        return 1;
    }

    const std::size_t channels = info->channels;
    const std::uint64_t data_bytes =
            info->data_offset <= entry.size()
                    ? std::min<std::uint64_t>(info->data_size, entry.size() - info->data_offset)
                    : 0;
    const std::uint64_t frames = data_bytes / info->block_align;
    // The decoder output is a continuous stream of 2048-sample frames; the valid audio is the window
    // [delay, delay + samples) of it.
    const std::uint64_t valid_begin = info->delay_samples;
    const std::uint64_t valid_end = valid_begin + info->total_samples;

    std::vector<float> pcm;
    pcm.reserve(std::size_t{info->total_samples} * channels);
    std::vector<float> decoded(tox2::kAtrac3plusFrameSamples * channels);
    std::uint64_t errors = 0;
    Clock::duration decode_time{};
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        const auto bytes_in = entry.subspan(info->data_offset + frame * info->block_align, info->block_align);
        const Clock::time_point begin = Clock::now();
        const bool ok = decoder->decode(bytes_in, decoded, error);
        decode_time += Clock::now() - begin;
        if (!ok) {
            if (++errors == 1)
                std::fprintf(stderr, "error: frame %llu: %s\n", static_cast<unsigned long long>(frame), error.c_str());
            std::ranges::fill(decoded, 0.0f);
        }
        const std::uint64_t first = frame * tox2::kAtrac3plusFrameSamples;
        const std::uint64_t from = std::max(first, valid_begin);
        const std::uint64_t to = std::min(first + tox2::kAtrac3plusFrameSamples, valid_end);
        if (from < to) {
            pcm.insert(pcm.end(), decoded.begin() + static_cast<std::ptrdiff_t>((from - first) * channels),
                       decoded.begin() + static_cast<std::ptrdiff_t>((to - first) * channels));
        }
    }

    const std::uint64_t samples = pcm.size() / channels;
    const double us_per_frame =
            frames != 0 ? std::chrono::duration<double, std::micro>(decode_time).count() / static_cast<double>(frames)
                        : 0.0;
    std::string line = std::format("at3: {} frames, {} samples", frames, samples);
    std::string hash;
    if (options.hash || !options.expect_pcm.empty()) {
        hash = sha256Hex(std::span(reinterpret_cast<const std::uint8_t*>(pcm.data()), pcm.size() * sizeof(float)));
        if (hash.empty()) {
            std::fputs("error: SHA-256 failed\n", stderr);
            return 1;
        }
        line += std::format(", pcm-sha256 {}", hash);
    }
    printLine(std::format("{}, {:.1f} us/frame", line, us_per_frame));

    int status = 0;
    auto fail = [&status](const std::string& message) {
        std::fprintf(stderr, "error: %s\n", message.c_str());
        status = 1;
    };
    if (errors != 0) fail(std::format("{} of {} frames failed to decode (silence used)", errors, frames));
    if (samples != info->total_samples) {
        fail(std::format("decoded {} samples, the file declares {}", samples, info->total_samples));
    }
    if (options.expect_frames && *options.expect_frames != frames) {
        fail(std::format("{} frames, expected {}", frames, *options.expect_frames));
    }
    if (options.expect_samples && *options.expect_samples != samples) {
        fail(std::format("{} samples, expected {}", samples, *options.expect_samples));
    }
    if (!options.expect_pcm.empty() && options.expect_pcm != hash) {
        fail(std::format("pcm-sha256 {}, expected {}", hash, options.expect_pcm));
    }

    if (!options.wav.empty()) {
        auto writer = tox2::WavWriter::create(std::filesystem::path(options.wav), info->sample_rate,
                                              static_cast<std::uint16_t>(channels));
        if (!writer) {
            fail("cannot write the WAV file: " + writer.error());
        } else if (!(*writer)->write(pcm) || !(*writer)->finish()) {
            fail("cannot write the WAV file");
        }
    }
    if (options.play && status == 0) {
        if (info->sample_rate != tox2::kAudioSampleRate || channels > 2) {
            fail("--play needs 48 kHz mono or stereo");
        } else if (auto output = openOutput(tox2::kHostAudioOutput)) {
            status = playInterleaved(*output, pcm, info->channels);
        } else {
            status = 1;
        }
    }
    if (status == 0) printLine("at3 ok");
    return status;
}

constexpr Subcommand kSubcommands[] = {
        {"wavstat", runWavstat}, {"devices", runDevices}, {"tone", runTone},
        {"play", runPlay},       {"at3scan", runAt3scan}, {"at3", runAt3},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string_view command = argv[1];
    std::vector<std::string_view> args(argv + 2, argv + argc);
    for (const auto& sub : kSubcommands) {
        if (sub.name == command) return sub.run(args);
    }
    std::fprintf(stderr, "error: unknown subcommand %s\n", argv[1]);
    printUsage();
    return 2;
}

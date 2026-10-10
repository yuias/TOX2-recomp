// dal_moviedump: parses a .pam movie, demuxes it and decodes the video (and with --audio the
// ATRAC3plus stream) with the real FFmpeg codec factory. Prints the stream table, then (with --hash)
// one line per decoded frame with the SHA-256 of the decoder's Y, U and V planes and of the converted
// ARGB picture, and a summary line; --audio adds the PCM hash, levels and duration checks. Exits 1 on
// any error, so it doubles as the headless decode check.

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "core/log.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "media/pamf.h"
#include "render/frame_dump.h"

namespace {

using Digest = std::array<std::uint8_t, 32>;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string movie;
    std::uint64_t max_frames = 0;  // 0: whole movie
    std::string png_dir;
    std::uint64_t png_every = 0;  // also write every Nth frame
    bool hash = false;
    bool verbose = false;
    int threads = 0;
    std::optional<bool> bt709;
    bool audio = false;
    bool no_video = false;
    std::string wav;
    std::uint64_t wav_seconds = 0;   // 0: the whole audio stream
    std::uint64_t audio_frames = 0;  // 0: every ATRAC3plus frame
    std::string expect_pcm;
    std::optional<std::uint64_t> expect_audio_samples;
    std::optional<std::uint64_t> expect_frames;
    std::map<std::uint64_t, std::string> expect_yuv;  // frame index -> lower-case hex
    std::map<std::uint64_t, std::string> expect_rgb;
};

void printUsage() {
    std::fputs(
            "usage: dal_moviedump MOVIE.pam [--frames N] [--hash] [--png-dir DIR [--png-every N]]\n"
            "                     [--threads N] [--bt601 | --bt709] [--verbose]\n"
            "                     [--audio [--audio-frames N] [--wav FILE [--wav-seconds N]] [--no-video]]\n"
            "                     [--expect-frames N] [--expect-yuv INDEX:SHA256]... [--expect-rgb INDEX:SHA256]...\n"
            "                     [--expect-pcm SHA256] [--expect-audio-samples N]\n"
            "  --frames N          stop after N decoded video frames (default: the whole movie)\n"
            "  --audio             also decode the ATRAC3plus stream; print sample counts, PCM SHA-256 (big-endian\n"
            "                      float32, as the game receives it), levels and duration checks\n"
            "  --audio-frames N    decode only the first N ATRAC3plus frames (default: all)\n"
            "  --wav FILE          write the audio as 16-bit WAV (first --wav-seconds N seconds, default all)\n"
            "  --no-video          skip video decoding (faster audio checks)\n"
            "  --expect-pcm H      fail unless the PCM SHA-256 is H\n"
            "  --expect-audio-samples N  fail unless N samples per channel were decoded\n"
            "  --hash              print one line per frame with the YUV and ARGB SHA-256\n"
            "  --png-dir DIR       write the first and the last frame (and every --png-every th) as PNG\n"
            "  --threads N         H.264 frame threads (default: min(4, cores))\n"
            "  --bt601 | --bt709   colour matrix of the ARGB conversion (default: from the stream info)\n"
            "  --expect-frames N   fail unless exactly N frames were decoded\n"
            "  --expect-yuv I:H    fail unless frame I has the YUV SHA-256 H\n"
            "  --expect-rgb I:H    fail unless frame I has the ARGB SHA-256 H\n",
            stderr);
}

bool parseUnsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return true;
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string_view& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string_view text;
        std::uint64_t number = 0;
        if (arg == "--hash") {
            options.hash = true;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--bt601") {
            options.bt709 = false;
        } else if (arg == "--bt709") {
            options.bt709 = true;
        } else if (arg == "--audio") {
            options.audio = true;
        } else if (arg == "--no-video") {
            options.no_video = true;
        } else if (arg == "--wav") {
            if (!value(text)) return false;
            options.wav = std::string(text);
        } else if (arg == "--wav-seconds") {
            if (!value(text) || !parseUnsigned(text, options.wav_seconds)) return false;
        } else if (arg == "--audio-frames") {
            if (!value(text) || !parseUnsigned(text, options.audio_frames)) return false;
        } else if (arg == "--expect-audio-samples") {
            if (!value(text) || !parseUnsigned(text, number)) return false;
            options.expect_audio_samples = number;
        } else if (arg == "--expect-pcm") {
            if (!value(text)) return false;
            options.expect_pcm = std::string(text);
            for (char& c : options.expect_pcm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (options.expect_pcm.size() != 64) return false;
        } else if (arg == "--frames") {
            if (!value(text) || !parseUnsigned(text, options.max_frames)) return false;
        } else if (arg == "--png-every") {
            if (!value(text) || !parseUnsigned(text, options.png_every)) return false;
        } else if (arg == "--threads") {
            if (!value(text) || !parseUnsigned(text, number) || number > 64) return false;
            options.threads = static_cast<int>(number);
        } else if (arg == "--png-dir") {
            if (!value(text)) return false;
            options.png_dir = std::string(text);
        } else if (arg == "--expect-frames") {
            if (!value(text) || !parseUnsigned(text, number)) return false;
            options.expect_frames = number;
        } else if (arg == "--expect-yuv" || arg == "--expect-rgb") {
            const bool yuv = arg == "--expect-yuv";
            if (!value(text)) return false;
            const std::size_t colon = text.find(':');
            if (colon == std::string_view::npos || !parseUnsigned(text.substr(0, colon), number)) return false;
            std::string hash(text.substr(colon + 1));
            for (char& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (hash.size() != 64) return false;
            (yuv ? options.expect_yuv : options.expect_rgb)[number] = hash;
        } else if (!arg.empty() && arg[0] == '-') {
            return false;
        } else if (options.movie.empty()) {
            options.movie = std::string(arg);
        } else {
            return false;
        }
    }
    return !options.movie.empty();
}

std::optional<Digest> sha256(std::span<const std::uint8_t> data) {
    Digest digest{};
    const NTSTATUS status =
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()),
                       static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (status < 0) return std::nullopt;
    return digest;
}

std::string hex(const Digest& digest) {
    std::string text;
    for (const std::uint8_t byte : digest) text += std::format("{:02x}", byte);
    return text;
}

// A hash over the per-frame digests, so the summary line fingerprints the whole run.
class RunningDigest {
public:
    void add(const Digest& digest) { bytes_.insert(bytes_.end(), digest.begin(), digest.end()); }
    std::string finish() const {
        const auto digest = sha256(bytes_);
        return digest ? hex(*digest) : std::string("unavailable");
    }

private:
    std::vector<std::uint8_t> bytes_;
};

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

const char* codecName(dal::PamfCodec codec) {
    switch (codec) {
        case dal::PamfCodec::Avc: return "avc";
        case dal::PamfCodec::M2v: return "mpeg2";
        case dal::PamfCodec::Atrac3plus: return "atrac3plus";
        case dal::PamfCodec::Lpcm: return "lpcm";
        case dal::PamfCodec::Ac3: return "ac3";
        case dal::PamfCodec::UserData: return "userdata";
    }
    return "?";
}

bool writeArgbPng(const std::filesystem::path& path, const dal::VideoFrame& frame) {
    dal::RgbImage image;
    image.width = frame.width;
    image.height = frame.height;
    image.rgb.resize(std::size_t{frame.width} * frame.height * 3);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint8_t* src = frame.bytes.data() + std::size_t{frame.pitch} * y;
        std::uint8_t* dst = image.rgb.data() + std::size_t{frame.width} * 3 * y;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 1];
            dst[x * 3 + 1] = src[x * 4 + 2];
            dst[x * 3 + 2] = src[x * 4 + 3];
        }
    }
    std::string error;
    if (!dal::writePngRgb(path, image, error)) {
        std::fprintf(stderr, "error: cannot write %s: %s\n", path.string().c_str(), error.c_str());
        return false;
    }
    return true;
}

// Collects the decoded audio: the PCM bytes as the game receives them (for the hash), level statistics
// and the 16-bit samples for a WAV file. Levels are computed on the decoder's float values.
class AudioAnalysis {
public:
    // `want_wav` keeps the first `wav_seconds` seconds (0: everything) as 16-bit samples.
    AudioAnalysis(bool want_wav, std::uint64_t wav_seconds) : want_wav_(want_wav), wav_seconds_(wav_seconds) {}

    // Chunks hold interleaved big-endian float32. A chunk without a known sample rate is left out; the
    // caller notices the missing samples.
    void add(const dal::AudioChunk& chunk, std::uint32_t sample_rate) {
        if (chunk.frames == 0 || sample_rate == 0) return;
        const std::uint32_t channels = static_cast<std::uint32_t>(chunk.bytes.size() / chunk.frames / 4);
        if (channels_ == 0) {
            channels_ = channels;
            rate_ = sample_rate;
            if (want_wav_) wav_limit_ = wav_seconds_ != 0 ? wav_seconds_ * rate_ : std::uint64_t{1} << 40;
            sum_squares_.assign(channels, 0.0);
            peak_.assign(channels, 0.0f);
            first_pts_ = chunk.pts;
            samples_at_first_pts_ = samples_;
        }
        if (chunk.pts < 0) {
            ++chunks_without_pts_;
        } else {
            if (first_pts_ < 0) {
                first_pts_ = chunk.pts;
                samples_at_first_pts_ = samples_;
            }
            const std::int64_t expected =
                    first_pts_ +
                    static_cast<std::int64_t>(((samples_ - samples_at_first_pts_) * 90000 + rate_ / 2) / rate_);
            max_pts_drift_ = std::max(max_pts_drift_, std::abs(chunk.pts - expected));
            last_pts_ = chunk.pts;
            samples_at_last_pts_ = samples_;
        }
        pcm_.insert(pcm_.end(), chunk.bytes.begin(), chunk.bytes.end());
        ++chunks_;
        for (std::uint32_t i = 0; i < chunk.frames; ++i) {
            float first = 0, second = 0;
            for (std::uint32_t c = 0; c < channels; ++c) {
                const std::uint8_t* p = chunk.bytes.data() + (std::size_t{i} * channels + c) * 4;
                const float value = std::bit_cast<float>(be32(p));
                if (c == 0) first = value;
                if (c == 1) second = value;
                peak_[c] = std::max(peak_[c], std::fabs(value));
                sum_squares_[c] += double{value} * value;
                if (std::fabs(value) > 1.0f) ++clipped_;
                if (wav_.size() < wav_limit_ * channels) {
                    const float scaled = std::isnan(value) ? 0.0f : std::clamp(value * 32767.0f, -32768.0f, 32767.0f);
                    wav_.push_back(static_cast<std::int16_t>(std::lround(scaled)));
                }
            }
            if (channels >= 2) {
                if (first == second) ++same_;
                diff_squares_ += double{first - second} * (first - second);
            }
        }
        samples_ += chunk.frames;
    }

    std::uint32_t channels() const { return channels_; }
    std::uint32_t rate() const { return rate_; }
    std::uint64_t chunks() const { return chunks_; }
    std::uint64_t samples() const { return samples_; }
    std::int64_t firstPts() const { return first_pts_; }
    // The end of the last chunk on the 90 kHz clock, -1 when no chunk carried a PTS.
    std::int64_t endPts() const {
        if (last_pts_ < 0 || rate_ == 0) return -1;
        return last_pts_ + static_cast<std::int64_t>(((samples_ - samples_at_last_pts_) * 90000 + rate_ / 2) / rate_);
    }
    std::int64_t maxPtsDrift() const { return max_pts_drift_; }
    std::uint64_t chunksWithoutPts() const { return chunks_without_pts_; }
    std::uint64_t clipped() const { return clipped_; }
    std::uint64_t identicalPairs() const { return same_; }
    double rms(std::uint32_t channel) const {
        return samples_ != 0 ? std::sqrt(sum_squares_[channel] / static_cast<double>(samples_)) : 0.0;
    }
    float peak(std::uint32_t channel) const { return peak_[channel]; }
    double differenceRms() const {
        return samples_ != 0 ? std::sqrt(diff_squares_ / static_cast<double>(samples_)) : 0.0;
    }
    const std::vector<std::uint8_t>& pcm() const { return pcm_; }
    const std::vector<std::int16_t>& wav() const { return wav_; }

private:
    bool want_wav_;
    std::uint64_t wav_seconds_;
    std::uint64_t wav_limit_ = 0;  // samples per channel
    std::uint32_t channels_ = 0, rate_ = 0;
    std::uint64_t chunks_ = 0, samples_ = 0, clipped_ = 0, same_ = 0, chunks_without_pts_ = 0;
    std::uint64_t samples_at_first_pts_ = 0, samples_at_last_pts_ = 0;
    std::int64_t first_pts_ = -1, last_pts_ = -1, max_pts_drift_ = 0;
    double diff_squares_ = 0;
    std::vector<double> sum_squares_;
    std::vector<float> peak_;
    std::vector<std::uint8_t> pcm_;
    std::vector<std::int16_t> wav_;
};

bool writeWav(const std::string& path, const std::vector<std::int16_t>& samples, std::uint32_t channels,
              std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto put32 = [&](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                           static_cast<char>(v >> 24)};
        out.write(b, 4);
    };
    auto put16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(b, 2);
    };
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    put32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);  // PCM
    put16(static_cast<std::uint16_t>(channels));
    put32(rate);
    put32(rate * channels * 2);
    put16(static_cast<std::uint16_t>(channels * 2));
    put16(16);
    out.write("data", 4);
    put32(data_bytes);
    for (const std::int16_t s : samples) put16(static_cast<std::uint16_t>(s));
    return static_cast<bool>(out);
}

std::string dbfs(double linear) {
    return linear > 0 ? std::format("{:.1f} dBFS", 20.0 * std::log10(linear)) : std::string("-inf dBFS");
}

// Both decoders see the same data; one hands out the decoder's own planes (planar output at the
// decoded size is a plain copy), the other the ARGB conversion.
struct Decoders {
    std::unique_ptr<dal::VideoDecoder> yuv, argb;
    std::deque<dal::VideoFrame> yuv_queue, argb_queue;
    double yuv_seconds = 0, argb_seconds = 0;
    bool send_failed = false;
};

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

void sendUnit(Decoders& d, const dal::PamfAccessUnit& unit) {
    auto start = Clock::now();
    if (!d.yuv->send(unit.bytes, unit.pts, unit.dts)) d.send_failed = true;
    d.yuv_seconds += secondsSince(start);
    start = Clock::now();
    if (!d.argb->send(unit.bytes, unit.pts, unit.dts)) d.send_failed = true;
    d.argb_seconds += secondsSince(start);
}

void receiveReady(Decoders& d) {
    dal::VideoFrame frame;
    auto start = Clock::now();
    while (d.yuv->receive(frame)) d.yuv_queue.push_back(std::move(frame));
    d.yuv_seconds += secondsSince(start);
    start = Clock::now();
    while (d.argb->receive(frame)) d.argb_queue.push_back(std::move(frame));
    d.argb_seconds += secondsSince(start);
}

int run(const Options& options) {
    namespace fs = std::filesystem;
    const auto run_start = Clock::now();
    std::ifstream in(options.movie, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot open %s\n", options.movie.c_str());
        return 1;
    }
    std::vector<std::uint8_t> header(2048);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (static_cast<std::size_t>(in.gcount()) < header.size()) {
        std::fprintf(stderr, "error: %s is too short\n", options.movie.c_str());
        return 1;
    }
    // The header size is in 2048-byte units at offset 8; the parser needs the whole header.
    const std::uint64_t header_size = std::uint64_t{be32(header.data() + 8)} * 2048;
    if (header_size > header.size() && header_size <= (1u << 20)) {
        header.resize(static_cast<std::size_t>(header_size));
        in.read(reinterpret_cast<char*>(header.data() + 2048), static_cast<std::streamsize>(header.size() - 2048));
    }
    const auto info = dal::parsePamfHeader(header);
    if (!info) {
        std::fprintf(stderr, "error: %s: %s\n", options.movie.c_str(), info.error().c_str());
        return 1;
    }

    std::size_t video_index = info->streams.size();
    std::size_t audio_index = info->streams.size();
    for (std::size_t i = 0; i < info->streams.size(); ++i) {
        const dal::PamfStream& s = info->streams[i];
        std::printf("stream %zu: %s id 0x%02x/0x%02x", i, codecName(s.codec), s.stream_id, s.private_stream_id);
        if (s.width != 0) std::printf(" %ux%u", s.width, s.height);
        if (s.codec == dal::PamfCodec::Avc && s.codec_info.size() > 0x17) {
            const std::vector<std::uint8_t>& c = s.codec_info;
            std::printf(", frame-rate code %d", (c[2] & 0x0F) - 1);
            if ((c[2] & 0x40) != 0) {
                std::printf(", signal info: range %s, primaries %u, matrix %u",
                            (c[0x14] & 0x10) != 0 ? "full" : "limited", c[0x15], c[0x17]);
            } else {
                std::printf(", no signal info");
            }
        }
        if (s.sample_rate != 0) std::printf(" %u Hz %u ch", s.sample_rate, s.channels);
        std::printf("\n");
        if (video_index == info->streams.size() && s.codec == dal::PamfCodec::Avc) video_index = i;
        if (audio_index == info->streams.size() && s.codec == dal::PamfCodec::Atrac3plus) audio_index = i;
    }
    std::printf("data: offset %u, %u bytes, pts %llu..%llu\n", info->data_offset, info->data_size,
                static_cast<unsigned long long>(info->start_pts), static_cast<unsigned long long>(info->end_pts));
    const bool has_video = video_index != info->streams.size();
    const bool has_audio = audio_index != info->streams.size();
    const bool decode_video = has_video && !options.no_video;
    if (!has_video && !options.no_video) {
        std::fprintf(stderr, "error: no AVC video stream\n");
        return 1;
    }
    if (options.audio && !has_audio) {
        std::fprintf(stderr, "error: no ATRAC3plus audio stream\n");
        return 1;
    }
    const dal::PamfStream no_stream;
    const dal::PamfStream& video = has_video ? info->streams[video_index] : no_stream;
    const std::vector<std::uint8_t>& block = video.codec_info;

    // Matrix coefficients 1 = BT.709 when the stream info carries video-signal data; without it HD
    // is taken as BT.709 and SD as BT.601, as the PS3 does not state otherwise.
    bool bt709 = video.height > 576;
    if (block.size() > 0x17 && (block[2] & 0x40) != 0) bt709 = block[0x17] == 1;
    if (options.bt709) bt709 = *options.bt709;

    dal::FfmpegCodecOptions codec_options;
    codec_options.video_threads = options.threads;
    std::string error;
    const std::unique_ptr<dal::MediaCodecFactory> factory = dal::createFfmpegCodecFactory(error, codec_options);
    if (!factory) {
        std::fprintf(stderr, "error: codecs unavailable: %s\n", error.c_str());
        return 1;
    }
    Decoders d;
    if (decode_video) {
        dal::VideoOutputFormat format;
        format.start_pts = static_cast<std::int64_t>(info->start_pts);
        format.coding = dal::VideoCoding::Yuv420Planar;
        d.yuv = factory->createVideo(video, format, error);
        if (!d.yuv) {
            std::fprintf(stderr, "error: cannot create the video decoder: %s\n", error.c_str());
            return 1;
        }
        format.coding = dal::VideoCoding::Argb;
        format.bt709 = bt709;
        d.argb = factory->createVideo(video, format, error);
        if (!d.argb) {
            std::fprintf(stderr, "error: cannot create the video decoder: %s\n", error.c_str());
            return 1;
        }
    }

    // Audio is requested in the stream's own format, as the player does; float32 is what the game's
    // sound adapter is expected to ask for.
    std::unique_ptr<dal::AudioDecoder> audio_decoder;
    dal::FfmpegAudioDecoder* ffmpeg_audio = nullptr;  // the same object, for its statistics and rate
    std::optional<AudioAnalysis> analysis;
    if (options.audio) {
        const dal::PamfStream& audio_stream = info->streams[audio_index];
        dal::AudioOutputFormat audio_format;
        audio_format.channels = audio_stream.channels;
        audio_format.sample_rate = audio_stream.sample_rate;
        audio_format.float32 = true;
        audio_decoder = factory->createAudio(audio_stream, audio_format, error);
        if (!audio_decoder) {
            std::fprintf(stderr, "error: cannot create the audio decoder: %s\n", error.c_str());
            return 1;
        }
        ffmpeg_audio = dynamic_cast<dal::FfmpegAudioDecoder*>(audio_decoder.get());
        if (ffmpeg_audio == nullptr) {
            std::fprintf(stderr, "error: the audio decoder is not the FFmpeg one\n");
            return 1;
        }
        analysis.emplace(!options.wav.empty(), options.wav_seconds);
    }

    const fs::path stem = fs::path(options.movie).stem();
    const fs::path png_dir = options.png_dir;
    bool failed = false;
    std::uint64_t frames = 0;
    std::int64_t first_pts = -1, last_pts = -1;
    std::uint32_t width = 0, height = 0;
    RunningDigest yuv_run, rgb_run;
    dal::VideoFrame last_argb;
    bool last_written = false;

    // Pairs the YUV and ARGB outputs, which come out in the same order.
    auto consume = [&]() {
        while (!d.yuv_queue.empty() && !d.argb_queue.empty()) {
            if (options.max_frames != 0 && frames >= options.max_frames) {
                d.yuv_queue.clear();
                d.argb_queue.clear();
                return;
            }
            dal::VideoFrame& y = d.yuv_queue.front();
            dal::VideoFrame& a = d.argb_queue.front();
            if (y.pts != a.pts || y.width != a.width || y.height != a.height) {
                std::fprintf(stderr, "error: frame %llu differs between the two decoders\n",
                             static_cast<unsigned long long>(frames));
                failed = true;
            }
            const auto yuv_hash = sha256(y.bytes);
            const auto rgb_hash = sha256(a.bytes);
            if (!yuv_hash || !rgb_hash) {
                std::fprintf(stderr, "error: SHA-256 failed\n");
                failed = true;
            } else {
                yuv_run.add(*yuv_hash);
                rgb_run.add(*rgb_hash);
                if (options.hash) {
                    std::printf("frame %llu pts %lld %ux%u yuv-sha256 %s rgb-sha256 %s\n",
                                static_cast<unsigned long long>(frames), static_cast<long long>(y.pts), y.width,
                                y.height, hex(*yuv_hash).c_str(), hex(*rgb_hash).c_str());
                }
                const auto expected = options.expect_yuv.find(frames);
                if (expected != options.expect_yuv.end() && expected->second != hex(*yuv_hash)) {
                    std::fprintf(stderr, "error: frame %llu yuv-sha256 %s, expected %s\n",
                                 static_cast<unsigned long long>(frames), hex(*yuv_hash).c_str(),
                                 expected->second.c_str());
                    failed = true;
                }
                const auto expected_rgb = options.expect_rgb.find(frames);
                if (expected_rgb != options.expect_rgb.end() && expected_rgb->second != hex(*rgb_hash)) {
                    std::fprintf(stderr, "error: frame %llu rgb-sha256 %s, expected %s\n",
                                 static_cast<unsigned long long>(frames), hex(*rgb_hash).c_str(),
                                 expected_rgb->second.c_str());
                    failed = true;
                }
            }
            if (frames == 0) {
                first_pts = y.pts;
                width = y.width;
                height = y.height;
            }
            last_pts = y.pts;
            if (!png_dir.empty()) {
                const bool periodic = options.png_every != 0 && frames % options.png_every == 0;
                if (frames == 0 || periodic) {
                    if (!writeArgbPng(png_dir / std::format("{}_{:05}.png", stem.string(), frames), a)) failed = true;
                    last_written = true;
                } else {
                    last_written = false;
                }
                last_argb = std::move(a);
            }
            ++frames;
            d.yuv_queue.pop_front();
            d.argb_queue.pop_front();
        }
    };

    const bool limited = options.max_frames != 0;
    // A decoder that has reached its limit stops getting data, but the file is read on while another
    // one still needs it.
    bool video_active = decode_video;
    bool audio_active = options.audio;
    std::uint64_t audio_units = 0;
    bool audio_send_failed = false;
    dal::PamfDemuxer demux(*info);
    in.clear();
    in.seekg(info->data_offset);
    std::vector<std::uint8_t> chunk(1u << 20);
    std::uint64_t video_units = 0;
    auto takeAudio = [&]() {
        dal::AudioChunk out;
        // The rate comes from the frame headers; the stream info may leave it zero.
        while (audio_decoder->receive(out)) analysis->add(out, ffmpeg_audio->sampleRate());
    };
    auto pull = [&]() {
        while (video_active) {
            auto unit = demux.next(static_cast<std::uint32_t>(video_index));
            if (!unit) break;
            ++video_units;
            sendUnit(d, *unit);
            receiveReady(d);
            consume();
            if (limited && frames >= options.max_frames) video_active = false;
        }
        while (audio_active) {
            auto unit = demux.next(static_cast<std::uint32_t>(audio_index));
            if (!unit) break;
            if (!audio_decoder->send(unit->bytes, unit->pts)) audio_send_failed = true;
            ++audio_units;
            takeAudio();
            if (options.audio_frames != 0 && audio_units >= options.audio_frames) audio_active = false;
        }
        // Streams nobody decodes are dropped so the demuxer does not buffer the whole file.
        for (std::uint32_t i = 0; i < info->streams.size(); ++i) {
            if ((i == video_index && video_active) || (i == audio_index && audio_active)) continue;
            while (demux.next(i)) {
            }
        }
        return video_active || audio_active;
    };

    bool more = true;
    while (more && in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        demux.feed(std::span<const std::uint8_t>(chunk).first(static_cast<std::size_t>(got)));
        more = pull();
    }
    if (more) {
        demux.endOfStream();
        more = pull();
    }
    if (video_active) {
        // Frame threading holds back the last pictures until the decoder is told the stream ended.
        auto start = Clock::now();
        d.yuv->flush();
        d.yuv_seconds += secondsSince(start);
        start = Clock::now();
        d.argb->flush();
        d.argb_seconds += secondsSince(start);
        receiveReady(d);
        consume();
    }
    if (options.audio) {
        audio_decoder->flush();
        takeAudio();
    }

    if (!png_dir.empty() && frames > 0 && !last_written) {
        if (!writeArgbPng(png_dir / std::format("{}_{:05}.png", stem.string(), frames - 1), last_argb)) failed = true;
    }
    if (!d.yuv_queue.empty() || !d.argb_queue.empty()) {
        if (!limited) {
            std::fprintf(stderr, "error: the decoders returned different frame counts\n");
            failed = true;
        }
    }
    if (demux.errors() != 0) {
        std::fprintf(stderr, "error: %llu demux errors\n", static_cast<unsigned long long>(demux.errors()));
        failed = true;
    }
    if (d.send_failed) {
        std::fprintf(stderr, "error: the decoder rejected data\n");
        failed = true;
    }
    if (options.expect_frames && *options.expect_frames != frames) {
        std::fprintf(stderr, "error: %llu frames decoded, expected %llu\n", static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(*options.expect_frames));
        failed = true;
    }
    for (const auto* expected : {&options.expect_yuv, &options.expect_rgb}) {
        for (const auto& [index, hash] : *expected) {
            if (index >= frames) {
                std::fprintf(stderr, "error: frame %llu was not decoded\n", static_cast<unsigned long long>(index));
                failed = true;
            }
        }
    }

    if (decode_video) {
        const double per_frame = frames != 0 ? 1000.0 / static_cast<double>(frames) : 0.0;
        std::printf("moviedump %s: %ux%u, %llu video units, %llu frames, pts %lld..%lld, bt%s\n", stem.string().c_str(),
                    width, height, static_cast<unsigned long long>(video_units),
                    static_cast<unsigned long long>(frames), static_cast<long long>(first_pts),
                    static_cast<long long>(last_pts), bt709 ? "709" : "601");
        std::printf("moviedump yuv-sha256 %s rgb-sha256 %s\n", yuv_run.finish().c_str(), rgb_run.finish().c_str());
        // The decoders run on worker threads, so the time spent inside the calls is the time the caller
        // waited; the wall time covers everything including hashing and PNG output.
        std::printf(
                "moviedump time: yuv decoder calls %.2f ms/frame, argb decoder calls %.2f ms/frame, wall %.2f ms/frame\n",
                d.yuv_seconds * per_frame, d.argb_seconds * per_frame, secondsSince(run_start) * per_frame);
    }

    if (options.audio) {
        const AudioAnalysis& a = *analysis;
        const auto pcm_hash = sha256(a.pcm());
        const std::string pcm_text = pcm_hash ? hex(*pcm_hash) : std::string("unavailable");
        std::printf("moviedump audio %llu frames, %llu samples, pcm-sha256 %s\n",
                    static_cast<unsigned long long>(a.chunks()), static_cast<unsigned long long>(a.samples()),
                    pcm_text.c_str());
        const auto& stats = ffmpeg_audio->stats();
        std::printf(
                "moviedump audio: %u Hz, %u ch, big-endian float32, %llu frames sent; rejected %llu, decode errors %llu, "
                "rate mismatches %llu, channel mismatches %llu\n",
                a.rate(), a.channels(), static_cast<unsigned long long>(audio_units),
                static_cast<unsigned long long>(stats.rejected_frames),
                static_cast<unsigned long long>(stats.decode_errors),
                static_cast<unsigned long long>(stats.rate_mismatches),
                static_cast<unsigned long long>(stats.channel_mismatches));
        for (std::uint32_t c = 0; c < a.channels(); ++c) {
            std::printf("moviedump audio ch%u: peak %.4f (%s), rms %.4f (%s)\n", c, a.peak(c), dbfs(a.peak(c)).c_str(),
                        a.rms(c), dbfs(a.rms(c)).c_str());
        }
        const double total_values = static_cast<double>(a.samples()) * a.channels();
        std::printf("moviedump audio clipped: %llu of %.0f values beyond +-1.0 (%.6f%%)\n",
                    static_cast<unsigned long long>(a.clipped()), total_values,
                    total_values > 0 ? 100.0 * static_cast<double>(a.clipped()) / total_values : 0.0);
        if (a.channels() >= 2) {
            std::printf("moviedump audio L/R: identical in %llu of %llu samples, rms(L-R) %.4f\n",
                        static_cast<unsigned long long>(a.identicalPairs()),
                        static_cast<unsigned long long>(a.samples()), a.differenceRms());
        }
        if (a.rate() != 0) {
            const double audio_seconds = static_cast<double>(a.samples()) / a.rate();
            const double interval_seconds = static_cast<double>(info->end_pts - info->start_pts) / 90000.0;
            const double diff_samples = (audio_seconds - interval_seconds) * a.rate();
            std::printf(
                    "moviedump audio duration: %.4f s; header interval %llu..%llu = %.4f s (audio - interval = %+.1f samples, "
                    "%+.2f frames of 2048)\n",
                    audio_seconds, static_cast<unsigned long long>(info->start_pts),
                    static_cast<unsigned long long>(info->end_pts), interval_seconds, diff_samples,
                    diff_samples / 2048.0);
            if (frames > 1 && !limited && options.audio_frames == 0) {
                const double span = static_cast<double>(last_pts - first_pts) / 90000.0;
                const double video_seconds = span * static_cast<double>(frames) / static_cast<double>(frames - 1);
                std::printf(
                        "moviedump video duration: %.4f s (%llu frames, pts span %.4f s); audio - video = %+.4f s\n",
                        video_seconds, static_cast<unsigned long long>(frames), span, audio_seconds - video_seconds);
            }
            std::printf(
                    "moviedump audio pts: first %lld (header start %llu, %+lld ticks), end %lld (header end %llu, %+lld ticks), "
                    "max drift from the sample count %lld ticks, %llu chunks without pts\n",
                    static_cast<long long>(a.firstPts()), static_cast<unsigned long long>(info->start_pts),
                    static_cast<long long>(a.firstPts() - static_cast<std::int64_t>(info->start_pts)),
                    static_cast<long long>(a.endPts()), static_cast<unsigned long long>(info->end_pts),
                    static_cast<long long>(a.endPts() - static_cast<std::int64_t>(info->end_pts)),
                    static_cast<long long>(a.maxPtsDrift()), static_cast<unsigned long long>(a.chunksWithoutPts()));
        }

        if (a.samples() == 0) {
            std::fprintf(stderr, "error: no audio was decoded\n");
            failed = true;
        }
        if (audio_send_failed || stats.rejected_frames != 0 || stats.decode_errors != 0) {
            std::fprintf(stderr, "error: the audio decoder rejected data or reported errors\n");
            failed = true;
        }
        if (a.chunks() != audio_units) {
            std::fprintf(stderr, "error: %llu audio frames sent, %llu decoded\n",
                         static_cast<unsigned long long>(audio_units), static_cast<unsigned long long>(a.chunks()));
            failed = true;
        }
        if (!options.expect_pcm.empty() && options.expect_pcm != pcm_text) {
            std::fprintf(stderr, "error: pcm-sha256 %s, expected %s\n", pcm_text.c_str(), options.expect_pcm.c_str());
            failed = true;
        }
        if (options.expect_audio_samples && *options.expect_audio_samples != a.samples()) {
            std::fprintf(stderr, "error: %llu audio samples decoded, expected %llu\n",
                         static_cast<unsigned long long>(a.samples()),
                         static_cast<unsigned long long>(*options.expect_audio_samples));
            failed = true;
        }
        if (!options.wav.empty() && a.rate() != 0) {
            if (!writeWav(options.wav, a.wav(), a.channels(), a.rate())) {
                std::fprintf(stderr, "error: cannot write %s\n", options.wav.c_str());
                failed = true;
            } else {
                std::printf("moviedump wav: %s, %zu samples per channel\n", options.wav.c_str(),
                            a.channels() != 0 ? a.wav().size() / a.channels() : std::size_t{0});
            }
        }
    }
    std::printf("moviedump %s\n", failed ? "failed" : "ok");
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    dal::setLogLevel(options.verbose ? dal::LogLevel::Debug : dal::LogLevel::Warn);
    return run(options);
}

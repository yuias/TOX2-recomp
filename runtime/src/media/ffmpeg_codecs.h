#pragma once

// FFmpeg implementation of the movie codec interfaces (see media/movie_decoder.h). Everything goes
// through the run-time loader in media/ffmpeg_api.h, so building this needs the FFmpeg headers only.

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>

#include "media/atrac_decoder.h"
#include "media/ffmpeg_api.h"
#include "media/movie_decoder.h"

namespace tox2 {

struct FfmpegCodecOptions {
    // H.264 frame-thread count; 0 picks min(4, hardware threads). More threads delay the first output
    // frame but do not change the decoded pictures.
    int video_threads = 0;
};

// Converts decoded pictures into the layout the game asked for. Holds a cached swscale context, so
// one instance serves one stream on one thread.
class FfmpegVideoConverter {
public:
    explicit FfmpegVideoConverter(const FfmpegApi& api) : api_(api) {}
    ~FfmpegVideoConverter();
    FfmpegVideoConverter(const FfmpegVideoConverter&) = delete;
    FfmpegVideoConverter& operator=(const FfmpegVideoConverter&) = delete;

    // `format.width`/`height` of 0 keep the decoded size, `pitch` of 0 packs the rows tightly (the
    // luma pitch for Yuv420Planar). `out.pts` is left alone. Planar output is the Y plane
    // (pitch x height) followed by the U and V planes (each (pitch + 1) / 2 x (height + 1) / 2, same
    // order), and the picture's own range is kept. ARGB/RGBA output is full-range RGB converted with
    // BT.601 or BT.709 per `format.bt709`; the source range comes from the picture. False with a
    // reason in `error` for an unsupported picture or a pitch smaller than a row.
    bool convert(const AVFrame& frame, const VideoOutputFormat& format, VideoFrame& out, std::string& error);

private:
    struct ContextKey {
        int src_width = 0, src_height = 0, src_format = -1;
        int dst_width = 0, dst_height = 0, dst_format = -1;
        bool bt709 = false, src_full_range = false;
        bool operator==(const ContextKey&) const = default;
    };

    FfmpegApi api_;
    SwsContext* sws_ = nullptr;
    ContextKey key_;
};

// Converts a decoded planar-float audio frame into interleaved big-endian samples (float32, or s16
// with the float range [-1, 1] scaled by 32767 and rounded). Only AV_SAMPLE_FMT_FLTP with 1 to 8
// channels is accepted. `out.pts` is left alone. False with a reason in `error` otherwise.
bool convertAudioFrame(const AVFrame& frame, bool float32, AudioChunk& out, std::string& error);

struct FfmpegAudioStats {
    std::uint64_t rejected_frames = 0;     // no valid frame header, or fewer bytes than it announces
    std::uint64_t decode_errors = 0;       // FFmpeg errors and frames that could not be converted
    std::uint64_t rate_mismatches = 0;     // chunks emitted at a rate other than the requested one
    std::uint64_t channel_mismatches = 0;  // chunks emitted with a channel count other than requested
};

// ATRAC3plus decoder. Input is one demuxer frame including its 8-byte header: the header's sample
// rate, channel configuration and size configure FFmpeg's decoder (which needs block_align) when the
// first frame arrives, and only the payload after the header goes to FFmpeg. Output keeps the
// stream's own rate and channel count; a difference from the requested format is counted and logged
// (no resampling or mixing).
class FfmpegAudioDecoder final : public AudioDecoder {
public:
    FfmpegAudioDecoder(const FfmpegApi& api, const PamfStream& stream, const AudioOutputFormat& format);
    ~FfmpegAudioDecoder() override;
    FfmpegAudioDecoder(const FfmpegAudioDecoder&) = delete;
    FfmpegAudioDecoder& operator=(const FfmpegAudioDecoder&) = delete;

    bool create(std::string& error);

    bool send(std::span<const std::uint8_t> frame, std::int64_t pts) override;
    bool receive(AudioChunk& out) override;
    void flush() override;

    const FfmpegAudioStats& stats() const { return stats_; }
    // Rate taken from the frame headers; 0 until the first valid frame has configured the decoder.
    std::uint32_t sampleRate() const { return open_rate_; }

private:
    bool openContext(std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t payload, std::string& error);
    void closeContext();
    bool sendPacket(const AVPacket* packet);
    bool receiveDecoded(AudioChunk& out);
    void takeReadyFrames();
    std::int64_t timestampOf(std::int64_t frame_pts, std::uint32_t samples);

    FfmpegApi api_;
    PamfStream stream_;
    AudioOutputFormat format_;
    FfmpegAudioStats stats_;
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::deque<AudioChunk> pending_;
    // Parameters the open context was configured with; 0 while none is open.
    std::uint32_t open_rate_ = 0, open_channels_ = 0, open_payload_ = 0;
    bool draining_ = false;
    bool warned_rate_ = false, warned_channels_ = false;
    std::int64_t anchor_ = -1;  // last known PTS; -1 before the first
    std::int64_t samples_since_anchor_ = 0;
};

class FfmpegCodecFactory final : public MediaCodecFactory {
public:
    explicit FfmpegCodecFactory(const FfmpegApi& api, const FfmpegCodecOptions& options = {})
        : api_(api), options_(options) {}

    std::unique_ptr<VideoDecoder> createVideo(const PamfStream& stream, const VideoOutputFormat& format,
                                              std::string& error) override;
    // ATRAC3plus only; any other codec reports an error so the player falls back to silence.
    std::unique_ptr<AudioDecoder> createAudio(const PamfStream& stream, const AudioOutputFormat& format,
                                              std::string& error) override;

private:
    FfmpegApi api_;
    FfmpegCodecOptions options_;
};

// ATRAC3plus decoder for the raw frames of a RIFF file (media/at3_file.h): each packet is exactly
// block_align bytes with no PAMF header, and each frame yields kAtrac3plusFrameSamples samples per
// channel as interleaved host floats.
class FfmpegAtracDecoder final : public AtracFrameDecoder {
public:
    FfmpegAtracDecoder(const FfmpegApi& api, std::uint32_t channels, std::uint32_t sample_rate,
                       std::uint32_t block_align)
        : api_(api), channels_(channels), sample_rate_(sample_rate), block_align_(block_align) {}
    ~FfmpegAtracDecoder() override;
    FfmpegAtracDecoder(const FfmpegAtracDecoder&) = delete;
    FfmpegAtracDecoder& operator=(const FfmpegAtracDecoder&) = delete;

    bool open(std::string& error);

    bool decode(std::span<const std::uint8_t> frame, std::span<float> out, std::string& error) override;
    // Reopens the codec: the overlap of the previous frame is part of its private state, which
    // avcodec_flush_buffers does not promise to clear.
    void reset() override;

private:
    void close();

    FfmpegApi api_;
    std::uint32_t channels_, sample_rate_, block_align_;
    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
};

class FfmpegAtracDecoderFactory final : public AtracDecoderFactory {
public:
    explicit FfmpegAtracDecoderFactory(const FfmpegApi& api) : api_(api) {}

    // Needs 1..8 channels and a non-zero block_align and sample rate.
    std::unique_ptr<AtracFrameDecoder> create(const At3Info& info, std::string& error) override;

private:
    FfmpegApi api_;
};

// A factory over the FFmpeg DLLs next to the executable; null with the reason in `error` when they
// cannot be loaded.
std::unique_ptr<MediaCodecFactory> createFfmpegCodecFactory(std::string& error, const FfmpegCodecOptions& options = {});
std::unique_ptr<AtracDecoderFactory> createFfmpegAtracDecoderFactory(std::string& error);

}  // namespace tox2

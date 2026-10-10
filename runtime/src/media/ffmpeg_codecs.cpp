#include "media/ffmpeg_codecs.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <thread>
#include <vector>

#include "core/log.h"

namespace tox2 {

namespace {

constexpr std::int64_t kClockHz = 90000;

// Limits for the output format: far above the 1080p the game uses, far below what overflows.
constexpr std::uint32_t kMaxDimension = 8192;
constexpr std::uint32_t kMaxPitch = 4 * kMaxDimension;
constexpr std::uint64_t kMaxFrameBytes = 256ull << 20;

// An ATRAC3plus frame holds 2048 samples per channel; the cap leaves room for other decoders.
constexpr int kMaxAudioChannels = 8;
constexpr int kMaxAudioFrameSamples = 1 << 16;

// Frame-rate codes of the PAMF AVC stream info, as numerator / denominator.
struct FrameRate {
    std::int64_t num, den;
};
constexpr FrameRate kFrameRates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}};

FrameRate frameRateOf(const PamfStream& stream) {
    constexpr FrameRate kDefault{30000, 1001};
    if (stream.codec_info.size() < 3) return kDefault;
    // The stored code is the API value plus one; 0 means "not stated".
    const int code = (stream.codec_info[2] & 0x0F) - 1;
    if (code < 0 || code >= static_cast<int>(std::size(kFrameRates))) return kDefault;
    return kFrameRates[code];
}

std::string errorString(const FfmpegApi& api, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    api.av_strerror(code, text, sizeof(text));
    return std::format("{} ({})", text, code);
}

AVPixelFormat outputPixelFormat(VideoCoding coding) {
    switch (coding) {
        case VideoCoding::Argb: return AV_PIX_FMT_ARGB;
        case VideoCoding::Rgba: return AV_PIX_FMT_RGBA;
        case VideoCoding::Yuv420Planar: return AV_PIX_FMT_YUV420P;
    }
    return AV_PIX_FMT_NONE;
}

// Bilinear because most frames keep their size; the accurate rounding and full chroma
// interpolation keep the colours independent of how swscale splits the work.
constexpr int kScaleFlags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;

class FfmpegVideoDecoder final : public VideoDecoder {
public:
    FfmpegVideoDecoder(const FfmpegApi& api, const VideoOutputFormat& format, FrameRate rate)
        : api_(api), format_(format), rate_(rate), converter_(api), anchor_(format.start_pts) {}

    ~FfmpegVideoDecoder() override {
        if (parser_ != nullptr) api_.av_parser_close(parser_);
        if (frame_ != nullptr) api_.av_frame_free(&frame_);
        if (packet_ != nullptr) api_.av_packet_free(&packet_);
        if (context_ != nullptr) api_.avcodec_free_context(&context_);
    }

    bool open(int threads, std::string& error) {
        const AVCodec* codec = api_.avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) {
            error = "FFmpeg has no H.264 decoder";
            return false;
        }
        context_ = api_.avcodec_alloc_context3(codec);
        frame_ = api_.av_frame_alloc();
        packet_ = api_.av_packet_alloc();
        parser_ = api_.av_parser_init(AV_CODEC_ID_H264);
        if (context_ == nullptr || frame_ == nullptr || packet_ == nullptr || parser_ == nullptr) {
            error = "out of memory creating the H.264 decoder";
            return false;
        }
        if (threads <= 0) {
            threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        }
        context_->thread_count = threads;
        context_->thread_type = FF_THREAD_FRAME;
        context_->pkt_timebase = {1, static_cast<int>(kClockHz)};
        const int result = api_.avcodec_open2(context_, codec, nullptr);
        if (result < 0) {
            error = "cannot open the H.264 decoder: " + errorString(api_, result);
            return false;
        }
        return true;
    }

    bool send(std::span<const std::uint8_t> es, std::int64_t pts, std::int64_t dts) override {
        if (draining_ || parser_ == nullptr) return false;
        // The stamps go with the first byte only: the parser attaches them to the access unit that
        // begins in this payload, and repeating them would stamp later units of the same payload.
        std::int64_t parser_pts = pts < 0 ? AV_NOPTS_VALUE : pts;
        std::int64_t parser_dts = dts < 0 ? AV_NOPTS_VALUE : dts;
        // FFmpeg's bitstream readers look a few bytes past the end of their input. The demuxer's
        // buffers know nothing about that, so the payload is copied into one with zeroed padding.
        padded_.assign(es.begin(), es.end());
        padded_.resize(es.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        const std::uint8_t* data = padded_.data();
        std::size_t remaining = es.size();
        bool ok = true;
        while (remaining > 0) {
            std::uint8_t* unit = nullptr;
            int unit_size = 0;
            const int chunk = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
            const int used =
                    api_.av_parser_parse2(parser_, context_, &unit, &unit_size, data, chunk, parser_pts, parser_dts, 0);
            if (used < 0) return false;
            data += used;
            remaining -= static_cast<std::size_t>(used);
            parser_pts = parser_dts = AV_NOPTS_VALUE;
            if (unit_size > 0 && !submitUnit(unit, unit_size)) ok = false;
            if (used == 0 && unit_size == 0) break;
        }
        return ok;
    }

    bool receive(VideoFrame& out) override {
        if (!pending_.empty()) {
            out = std::move(pending_.front());
            pending_.pop_front();
            return true;
        }
        return receiveDecoded(out);
    }

    void flush() override {
        if (draining_ || parser_ == nullptr) return;
        // The parser holds the last access unit until it sees the next start code.
        std::uint8_t* unit = nullptr;
        int unit_size = 0;
        api_.av_parser_parse2(parser_, context_, &unit, &unit_size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (unit_size > 0) submitUnit(unit, unit_size);
        // Only a decoder that accepted the end-of-stream packet has an end to wait for.
        if (sendPacket(nullptr)) draining_ = true;
    }

private:
    // The parser's output may point into the caller's buffer, so the unit is copied into a packet
    // that owns padded memory.
    bool submitUnit(const std::uint8_t* unit, int size) {
        if (api_.av_new_packet(packet_, size) < 0) return false;
        std::memcpy(packet_->data, unit, static_cast<std::size_t>(size));
        packet_->pts = parser_->pts;
        packet_->dts = parser_->dts;
        const bool ok = sendPacket(packet_);
        api_.av_packet_unref(packet_);
        return ok;
    }

    bool sendPacket(const AVPacket* packet) {
        for (;;) {
            const int result = api_.avcodec_send_packet(context_, packet);
            if (result == 0) return true;
            if (result != AVERROR(EAGAIN)) {
                logDebug("ffmpeg video: send_packet failed: {}", errorString(api_, result));
                return false;
            }
            // The decoder wants its output taken first; keep the frames for receive().
            VideoFrame frame;
            if (!receiveDecoded(frame)) return false;
            pending_.push_back(std::move(frame));
        }
    }

    // Rules for frames without a PTS: last known PTS plus whole frame durations in output order. The
    // product is rounded as a whole so that rounding does not accumulate.
    std::int64_t timestampOf(std::int64_t frame_pts) {
        if (frame_pts != AV_NOPTS_VALUE) {
            anchor_ = frame_pts;
            frames_since_anchor_ = 0;
            return frame_pts;
        }
        ++frames_since_anchor_;
        return anchor_ + (kClockHz * rate_.den * frames_since_anchor_ + rate_.num / 2) / rate_.num;
    }

    bool receiveDecoded(VideoFrame& out) {
        // A decode error belongs to one frame; the next call goes on with the following one. The bound
        // keeps a decoder that fails every time from spinning.
        constexpr int kMaxConsecutiveErrors = 16;
        int errors = 0;
        for (;;) {
            const int result = api_.avcodec_receive_frame(context_, frame_);
            if (result == AVERROR_EOF) {
                // Everything is out; make the decoder and the parser usable for another stream.
                api_.avcodec_flush_buffers(context_);
                api_.av_parser_close(parser_);
                parser_ = api_.av_parser_init(AV_CODEC_ID_H264);
                draining_ = false;
                anchor_ = format_.start_pts;
                frames_since_anchor_ = 0;
                return false;
            }
            if (result == AVERROR(EAGAIN)) return false;  // needs more input
            if (result < 0) {
                ++decode_errors_;
                logDebug("ffmpeg video: receive_frame failed: {}", errorString(api_, result));
                if (++errors >= kMaxConsecutiveErrors) return false;
                continue;
            }
            errors = 0;

            out.pts = timestampOf(frame_->pts);
            std::string error;
            const bool converted = converter_.convert(*frame_, format_, out, error);
            api_.av_frame_unref(frame_);
            if (converted) return true;
            logDebug("ffmpeg video: cannot convert a frame: {}", error);
        }
    }

    FfmpegApi api_;
    VideoOutputFormat format_;
    FrameRate rate_;
    FfmpegVideoConverter converter_;
    AVCodecContext* context_ = nullptr;
    AVCodecParserContext* parser_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::deque<VideoFrame> pending_;
    std::vector<std::uint8_t> padded_;  // reused across send() calls
    std::uint64_t decode_errors_ = 0;
    bool draining_ = false;
    std::int64_t anchor_;
    std::int64_t frames_since_anchor_ = 0;
};

}  // namespace

FfmpegVideoConverter::~FfmpegVideoConverter() {
    if (sws_ != nullptr) api_.sws_freeContext(sws_);
}

bool FfmpegVideoConverter::convert(const AVFrame& frame, const VideoOutputFormat& format, VideoFrame& out,
                                   std::string& error) {
    if (frame.width <= 0 || frame.height <= 0 || frame.data[0] == nullptr || frame.format == AV_PIX_FMT_NONE) {
        error = "empty picture";
        return false;
    }
    const AVPixelFormat dst_format = outputPixelFormat(format.coding);
    const std::uint32_t width = format.width != 0 ? format.width : static_cast<std::uint32_t>(frame.width);
    const std::uint32_t height = format.height != 0 ? format.height : static_cast<std::uint32_t>(frame.height);
    // The format comes from guest memory, so it is bounded before anything is allocated.
    if (width > kMaxDimension || height > kMaxDimension) {
        error = std::format("{}x{} exceeds the {} pixel limit", width, height, kMaxDimension);
        return false;
    }
    const bool planar = format.coding == VideoCoding::Yuv420Planar;
    const std::uint32_t row_bytes = planar ? width : width * 4;
    const std::uint32_t pitch = format.pitch != 0 ? format.pitch : row_bytes;
    if (pitch < row_bytes || pitch > kMaxPitch) {
        error = std::format("pitch {} is outside {}..{} bytes", pitch, row_bytes, kMaxPitch);
        return false;
    }

    const std::uint64_t chroma_pitch = (std::uint64_t{pitch} + 1) / 2;
    const std::uint64_t chroma_height = (std::uint64_t{height} + 1) / 2;
    const std::uint64_t luma_size = std::uint64_t{pitch} * height;
    const std::uint64_t chroma_size = chroma_pitch * chroma_height;
    const std::uint64_t total_bytes = planar ? luma_size + 2 * chroma_size : luma_size;
    if (total_bytes > kMaxFrameBytes) {
        error = std::format("a frame of {} bytes exceeds the {} byte limit", total_bytes, kMaxFrameBytes);
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(total_bytes);
    // Zero-filled so padding bytes are the same on every run.
    out.bytes.assign(total, 0);
    std::uint8_t* planes[4] = {out.bytes.data(), nullptr, nullptr, nullptr};
    int strides[4] = {static_cast<int>(pitch), 0, 0, 0};
    if (planar) {
        planes[1] = planes[0] + static_cast<std::size_t>(luma_size);
        planes[2] = planes[1] + static_cast<std::size_t>(chroma_size);
        strides[1] = strides[2] = static_cast<int>(chroma_pitch);
    }

    ContextKey key;
    key.src_width = frame.width;
    key.src_height = frame.height;
    key.src_format = frame.format;
    key.dst_width = static_cast<int>(width);
    key.dst_height = static_cast<int>(height);
    key.dst_format = dst_format;
    key.bt709 = format.bt709;
    key.src_full_range = frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P;
    if (sws_ == nullptr || !(key == key_)) {
        sws_ = api_.sws_getCachedContext(sws_, key.src_width, key.src_height,
                                         static_cast<AVPixelFormat>(key.src_format), key.dst_width, key.dst_height,
                                         dst_format, kScaleFlags, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) {
            key_ = ContextKey{};
            error = "swscale cannot convert this picture";
            return false;
        }
        if (!planar) {
            const int* matrix = api_.sws_getCoefficients(format.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
            // Output RGB is full range; the destination matrix argument is ignored for RGB.
            if (api_.sws_setColorspaceDetails(sws_, matrix, key.src_full_range ? 1 : 0, matrix, 1, 0, 1 << 16,
                                              1 << 16) < 0) {
                key_ = ContextKey{};
                error = "swscale rejects the colour matrix for this picture";
                return false;
            }
        }
        key_ = key;
    }

    const int rows = api_.sws_scale(sws_, frame.data, frame.linesize, 0, frame.height, planes, strides);
    if (rows != static_cast<int>(height)) {
        error = std::format("swscale produced {} of {} rows", rows, height);
        return false;
    }

    // swscale writes 0xff for pictures without alpha.
    if (!planar && format.alpha != 0xff) {
        const std::size_t offset = format.coding == VideoCoding::Argb ? 0 : 3;
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* row = planes[0] + std::size_t{pitch} * y + offset;
            for (std::uint32_t x = 0; x < width; ++x) row[std::size_t{x} * 4] = format.alpha;
        }
    }
    out.width = width;
    out.height = height;
    out.pitch = pitch;
    return true;
}

// The output is big-endian on every host; the swaps below assume a little-endian one.
static_assert(std::endian::native == std::endian::little);

bool convertAudioFrame(const AVFrame& frame, bool float32, AudioChunk& out, std::string& error) {
    const int channels = frame.ch_layout.nb_channels;
    if (frame.format != AV_SAMPLE_FMT_FLTP) {
        error = std::format("sample format {} is not planar float", frame.format);
        return false;
    }
    if (channels < 1 || channels > kMaxAudioChannels) {
        error = std::format("{} channels are outside 1..{}", channels, kMaxAudioChannels);
        return false;
    }
    if (frame.nb_samples <= 0 || frame.nb_samples > kMaxAudioFrameSamples) {
        error = std::format("{} samples per frame are outside 1..{}", frame.nb_samples, kMaxAudioFrameSamples);
        return false;
    }
    const std::uint8_t* const* planes = frame.extended_data != nullptr ? frame.extended_data : frame.data;
    for (int c = 0; c < channels; ++c) {
        if (planes[c] == nullptr) {
            error = "missing sample plane";
            return false;
        }
    }

    const std::size_t samples = static_cast<std::size_t>(frame.nb_samples);
    const std::size_t width = float32 ? 4 : 2;
    out.frames = static_cast<std::uint32_t>(samples);
    out.bytes.resize(samples * static_cast<std::size_t>(channels) * width);
    std::uint8_t* dst = out.bytes.data();
    for (std::size_t i = 0; i < samples; ++i) {
        for (int c = 0; c < channels; ++c) {
            float value;
            std::memcpy(&value, planes[c] + i * sizeof(float), sizeof(float));
            if (float32) {
                // Not clamped: the game gets the decoder's values as they are.
                const std::uint32_t bits = std::byteswap(std::bit_cast<std::uint32_t>(value));
                std::memcpy(dst, &bits, 4);
            } else {
                // NaN becomes silence; everything else is clamped to the 16-bit range after scaling.
                const float scaled = std::isnan(value) ? 0.0f : std::clamp(value * 32767.0f, -32768.0f, 32767.0f);
                const std::uint16_t bits =
                        std::byteswap(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(scaled))));
                std::memcpy(dst, &bits, 2);
            }
            dst += width;
        }
    }
    return true;
}

FfmpegAudioDecoder::FfmpegAudioDecoder(const FfmpegApi& api, const PamfStream& stream, const AudioOutputFormat& format)
    : api_(api), stream_(stream), format_(format) {
    stream_.codec_info.clear();  // not needed here
}

FfmpegAudioDecoder::~FfmpegAudioDecoder() {
    closeContext();
    if (frame_ != nullptr) api_.av_frame_free(&frame_);
    if (packet_ != nullptr) api_.av_packet_free(&packet_);
}

bool FfmpegAudioDecoder::create(std::string& error) {
    if (api_.avcodec_find_decoder_by_name("atrac3plus") == nullptr) {
        error = "FFmpeg has no ATRAC3plus decoder";
        return false;
    }
    frame_ = api_.av_frame_alloc();
    packet_ = api_.av_packet_alloc();
    if (frame_ == nullptr || packet_ == nullptr) {
        error = "out of memory creating the ATRAC3plus decoder";
        return false;
    }
    return true;
}

void FfmpegAudioDecoder::closeContext() {
    if (context_ != nullptr) api_.avcodec_free_context(&context_);
    open_rate_ = open_channels_ = open_payload_ = 0;
}

bool FfmpegAudioDecoder::openContext(std::uint32_t sample_rate, std::uint32_t channels, std::uint32_t payload,
                                     std::string& error) {
    // Frames after the switch continue from where the old configuration left off, so the sample
    // count since the anchor is never mixed between two rates.
    if (anchor_ >= 0 && open_rate_ != 0) {
        anchor_ += (samples_since_anchor_ * kClockHz + open_rate_ / 2) / open_rate_;
    }
    samples_since_anchor_ = 0;
    closeContext();
    const AVCodec* codec = api_.avcodec_find_decoder_by_name("atrac3plus");
    if (codec == nullptr) {
        error = "FFmpeg has no ATRAC3plus decoder";
        return false;
    }
    context_ = api_.avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
        error = "out of memory creating the ATRAC3plus decoder";
        return false;
    }
    context_->sample_rate = static_cast<int>(sample_rate);
    api_.av_channel_layout_default(&context_->ch_layout, static_cast<int>(channels));
    // The decoder refuses to open without it and cuts packets at this size, so it must be the payload
    // size (the header is not part of the packet).
    context_->block_align = static_cast<int>(payload);
    context_->pkt_timebase = {1, static_cast<int>(kClockHz)};
    const int result = api_.avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
        error = "cannot open the ATRAC3plus decoder: " + errorString(api_, result);
        api_.avcodec_free_context(&context_);
        return false;
    }
    open_rate_ = sample_rate;
    open_channels_ = channels;
    open_payload_ = payload;
    return true;
}

bool FfmpegAudioDecoder::send(std::span<const std::uint8_t> frame, std::int64_t pts) {
    if (draining_ || packet_ == nullptr) return false;
    // Header layout: sync 0x0FD0, then a big-endian word with the rate index (3 bits), the channel
    // configuration (3 bits) and the payload size in 8-byte units minus one (10 bits).
    constexpr std::size_t kHeaderSize = 8;
    if (frame.size() < kHeaderSize || frame[0] != 0x0F || frame[1] != 0xD0) {
        ++stats_.rejected_frames;
        logDebug("ffmpeg audio: frame without an ATRAC3plus header ({} bytes)", frame.size());
        return false;
    }
    const unsigned params = (unsigned{frame[2]} << 8) | frame[3];
    const unsigned rate_index = params >> 13;
    const unsigned channel_index = (params >> 10) & 7;
    const std::uint32_t payload = ((params & 0x3FF) + 1) * 8;
    if ((rate_index != 1 && rate_index != 2) || channel_index == 0) {
        ++stats_.rejected_frames;
        logDebug("ffmpeg audio: unsupported frame header parameters 0x{:04x}", params);
        return false;
    }
    const std::uint32_t sample_rate = rate_index == 1 ? 44100 : 48000;
    const std::uint32_t channels = channel_index <= 4 ? channel_index : channel_index + 1;
    if (frame.size() < kHeaderSize + payload) {
        ++stats_.rejected_frames;
        logDebug("ffmpeg audio: frame of {} bytes is shorter than its header says ({})", frame.size(),
                 kHeaderSize + payload);
        return false;
    }

    // block_align is the largest payload seen: the decoder accepts smaller packets (same output) and
    // splits larger ones, so only a bigger payload, or another rate or channel count, needs a new context.
    if (context_ == nullptr || sample_rate != open_rate_ || channels != open_channels_ || payload > open_payload_) {
        if (context_ != nullptr) takeReadyFrames();  // keep what the old configuration already decoded
        if ((stream_.sample_rate != 0 && stream_.sample_rate != sample_rate) ||
            (stream_.channels != 0 && stream_.channels != channels)) {
            logWarn("ffmpeg audio: the frame header says {} Hz {} ch, the stream info {} Hz {} ch; using the header",
                    sample_rate, channels, stream_.sample_rate, stream_.channels);
        }
        std::string error;
        // Without a rate change the largest payload so far stays the block size.
        const bool same_format = sample_rate == open_rate_ && channels == open_channels_;
        if (!openContext(sample_rate, channels, same_format ? std::max(payload, open_payload_) : payload, error)) {
            ++stats_.decode_errors;
            logDebug("ffmpeg audio: {}", error);
            return false;
        }
    }

    // The packet owns padded memory, as the decoder's bitstream reader looks past the end.
    if (api_.av_new_packet(packet_, static_cast<int>(payload)) < 0) {
        ++stats_.decode_errors;
        return false;
    }
    std::memcpy(packet_->data, frame.data() + kHeaderSize, payload);
    packet_->pts = pts < 0 ? AV_NOPTS_VALUE : pts;
    packet_->dts = packet_->pts;
    const bool ok = sendPacket(packet_);
    api_.av_packet_unref(packet_);
    return ok;
}

bool FfmpegAudioDecoder::receive(AudioChunk& out) {
    if (!pending_.empty()) {
        out = std::move(pending_.front());
        pending_.pop_front();
        return true;
    }
    return receiveDecoded(out);
}

void FfmpegAudioDecoder::flush() {
    if (draining_ || context_ == nullptr) return;
    // Only a decoder that accepted the end-of-stream packet has an end to wait for.
    if (sendPacket(nullptr)) draining_ = true;
}

bool FfmpegAudioDecoder::sendPacket(const AVPacket* packet) {
    for (;;) {
        const int result = api_.avcodec_send_packet(context_, packet);
        if (result == 0) return true;
        if (result != AVERROR(EAGAIN)) {
            ++stats_.decode_errors;
            logDebug("ffmpeg audio: send_packet failed: {}", errorString(api_, result));
            return false;
        }
        // The decoder wants its output taken first; keep the chunks for receive().
        AudioChunk chunk;
        if (!receiveDecoded(chunk)) return false;
        pending_.push_back(std::move(chunk));
    }
}

void FfmpegAudioDecoder::takeReadyFrames() {
    AudioChunk chunk;
    while (receiveDecoded(chunk)) pending_.push_back(std::move(chunk));
}

// A frame without a PTS continues from the last known one by whole sample counts; the product is
// rounded as a whole so that rounding does not accumulate (44.1 kHz frames are not a whole number of
// ticks).
std::int64_t FfmpegAudioDecoder::timestampOf(std::int64_t frame_pts, std::uint32_t samples) {
    std::int64_t pts = -1;
    if (frame_pts != AV_NOPTS_VALUE) {
        pts = anchor_ = frame_pts;
        samples_since_anchor_ = 0;
    } else if (anchor_ >= 0 && open_rate_ != 0) {
        pts = anchor_ + (samples_since_anchor_ * kClockHz + open_rate_ / 2) / open_rate_;
    }
    samples_since_anchor_ += samples;
    return pts;
}

bool FfmpegAudioDecoder::receiveDecoded(AudioChunk& out) {
    constexpr int kMaxConsecutiveErrors = 16;
    int errors = 0;
    for (;;) {
        if (context_ == nullptr) return false;
        const int result = api_.avcodec_receive_frame(context_, frame_);
        if (result == AVERROR_EOF) {
            // Everything is out; the context stays open for another stream.
            api_.avcodec_flush_buffers(context_);
            draining_ = false;
            anchor_ = -1;
            samples_since_anchor_ = 0;
            return false;
        }
        if (result == AVERROR(EAGAIN)) return false;
        if (result < 0) {
            ++stats_.decode_errors;
            logDebug("ffmpeg audio: receive_frame failed: {}", errorString(api_, result));
            if (++errors >= kMaxConsecutiveErrors) return false;
            continue;
        }
        errors = 0;

        std::string error;
        const bool converted = convertAudioFrame(*frame_, format_.float32, out, error);
        const std::int64_t frame_pts = frame_->pts;
        api_.av_frame_unref(frame_);
        if (!converted) {
            ++stats_.decode_errors;
            logDebug("ffmpeg audio: cannot convert a frame: {}", error);
            continue;
        }
        out.pts = timestampOf(frame_pts, out.frames);
        if (format_.sample_rate != 0 && format_.sample_rate != open_rate_) {
            ++stats_.rate_mismatches;
            if (!warned_rate_) {
                warned_rate_ = true;
                logWarn("ffmpeg audio: the stream is {} Hz, {} Hz was requested; no resampling is done", open_rate_,
                        format_.sample_rate);
            }
        }
        if (format_.channels != 0 && format_.channels != open_channels_) {
            ++stats_.channel_mismatches;
            if (!warned_channels_) {
                warned_channels_ = true;
                logWarn("ffmpeg audio: the stream has {} channels, {} were requested; no mixing is done",
                        open_channels_, format_.channels);
            }
        }
        return true;
    }
}

FfmpegAtracDecoder::~FfmpegAtracDecoder() {
    close();
    if (frame_ != nullptr) api_.av_frame_free(&frame_);
    if (packet_ != nullptr) api_.av_packet_free(&packet_);
}

void FfmpegAtracDecoder::close() {
    if (context_ != nullptr) api_.avcodec_free_context(&context_);
}

bool FfmpegAtracDecoder::open(std::string& error) {
    const AVCodec* codec = api_.avcodec_find_decoder_by_name("atrac3plus");
    if (codec == nullptr) {
        error = "FFmpeg has no ATRAC3plus decoder";
        return false;
    }
    if (frame_ == nullptr) frame_ = api_.av_frame_alloc();
    if (packet_ == nullptr) packet_ = api_.av_packet_alloc();
    context_ = api_.avcodec_alloc_context3(codec);
    if (frame_ == nullptr || packet_ == nullptr || context_ == nullptr) {
        error = "out of memory creating the ATRAC3plus decoder";
        close();
        return false;
    }
    context_->sample_rate = static_cast<int>(sample_rate_);
    api_.av_channel_layout_default(&context_->ch_layout, static_cast<int>(channels_));
    // The decoder refuses to open without it.
    context_->block_align = static_cast<int>(block_align_);
    context_->pkt_timebase = {1, static_cast<int>(sample_rate_)};
    const int result = api_.avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
        error = "cannot open the ATRAC3plus decoder: " + errorString(api_, result);
        close();
        return false;
    }
    return true;
}

bool FfmpegAtracDecoder::decode(std::span<const std::uint8_t> frame, std::span<float> out, std::string& error) {
    if (context_ == nullptr) {
        error = "the ATRAC3plus decoder is not open";
        return false;
    }
    const std::size_t out_samples = std::size_t{kAtrac3plusFrameSamples} * channels_;
    if (frame.size() != block_align_ || out.size() != out_samples) {
        error = std::format("frame of {} bytes and output of {} floats, expected {} bytes and {} floats", frame.size(),
                            out.size(), block_align_, out_samples);
        return false;
    }

    // The packet owns padded memory, as the decoder's bitstream reader looks past the end.
    if (api_.av_new_packet(packet_, static_cast<int>(block_align_)) < 0) {
        error = "out of memory";
        return false;
    }
    std::memcpy(packet_->data, frame.data(), frame.size());
    int result = api_.avcodec_send_packet(context_, packet_);
    api_.av_packet_unref(packet_);
    if (result < 0) {
        error = "send_packet failed: " + errorString(api_, result);
        return false;
    }
    result = api_.avcodec_receive_frame(context_, frame_);
    if (result < 0) {
        error = "receive_frame failed: " + errorString(api_, result);
        return false;
    }

    bool ok = true;
    if (frame_->format != AV_SAMPLE_FMT_FLTP || frame_->ch_layout.nb_channels != static_cast<int>(channels_) ||
        frame_->nb_samples != static_cast<int>(kAtrac3plusFrameSamples)) {
        error = std::format("unexpected output: format {}, {} channels, {} samples", frame_->format,
                            frame_->ch_layout.nb_channels, frame_->nb_samples);
        ok = false;
    }
    const std::uint8_t* const* planes = frame_->extended_data != nullptr ? frame_->extended_data : frame_->data;
    for (std::uint32_t c = 0; ok && c < channels_; ++c) {
        if (planes[c] == nullptr) {
            error = "missing sample plane";
            ok = false;
        }
    }
    if (ok) {
        for (std::size_t i = 0; i < kAtrac3plusFrameSamples; ++i) {
            for (std::size_t c = 0; c < channels_; ++c) {
                std::memcpy(&out[i * channels_ + c], planes[c] + i * sizeof(float), sizeof(float));
            }
        }
    }
    api_.av_frame_unref(frame_);
    return ok;
}

void FfmpegAtracDecoder::reset() {
    close();
    std::string error;
    if (!open(error)) {
        // Later decode() calls fail and the caller plays silence.
        logWarn("ffmpeg atrac: cannot reopen the decoder: {}", error);
    }
}

std::unique_ptr<AtracFrameDecoder> FfmpegAtracDecoderFactory::create(const At3Info& info, std::string& error) {
    if (info.channels < 1 || info.channels > static_cast<std::uint32_t>(kMaxAudioChannels)) {
        error = std::format("{} channels are outside 1..{}", info.channels, kMaxAudioChannels);
        return nullptr;
    }
    if (info.block_align == 0 || info.sample_rate == 0) {
        error = std::format("block_align {} and sample rate {} must be non-zero", info.block_align, info.sample_rate);
        return nullptr;
    }
    auto decoder = std::make_unique<FfmpegAtracDecoder>(api_, info.channels, info.sample_rate, info.block_align);
    if (!decoder->open(error)) return nullptr;
    return decoder;
}

std::unique_ptr<VideoDecoder> FfmpegCodecFactory::createVideo(const PamfStream& stream, const VideoOutputFormat& format,
                                                              std::string& error) {
    if (stream.codec != PamfCodec::Avc) {
        error = "only AVC video is supported";
        return nullptr;
    }
    auto decoder = std::make_unique<FfmpegVideoDecoder>(api_, format, frameRateOf(stream));
    if (!decoder->open(options_.video_threads, error)) return nullptr;
    return decoder;
}

std::unique_ptr<AudioDecoder> FfmpegCodecFactory::createAudio(const PamfStream& stream, const AudioOutputFormat& format,
                                                              std::string& error) {
    if (stream.codec != PamfCodec::Atrac3plus) {
        error = "only ATRAC3plus audio is supported";
        return nullptr;
    }
    // The format comes from guest memory; the decoder never emits more than kMaxAudioChannels.
    if (format.channels > kMaxAudioChannels) {
        error = std::format("{} channels exceed the limit of {}", format.channels, kMaxAudioChannels);
        return nullptr;
    }
    auto decoder = std::make_unique<FfmpegAudioDecoder>(api_, stream, format);
    if (!decoder->create(error)) return nullptr;
    return decoder;
}

std::unique_ptr<MediaCodecFactory> createFfmpegCodecFactory(std::string& error, const FfmpegCodecOptions& options) {
    const auto& api = defaultFfmpeg();
    if (!api) {
        error = api.error();
        return nullptr;
    }
    return std::make_unique<FfmpegCodecFactory>(*api, options);
}

std::unique_ptr<AtracDecoderFactory> createFfmpegAtracDecoderFactory(std::string& error) {
    const auto& api = defaultFfmpeg();
    if (!api) {
        error = api.error();
        return nullptr;
    }
    return std::make_unique<FfmpegAtracDecoderFactory>(*api);
}

}  // namespace tox2

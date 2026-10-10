#pragma once

// Codec interfaces of the movie pipeline. They carry no FFmpeg types, so the player and its tests
// build without FFmpeg; the FFmpeg implementation lives in dal_media and is installed at start-up.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "media/pamf.h"

namespace dal {

enum class VideoCoding : std::uint8_t { Argb, Rgba, Yuv420Planar };

// What the game asked for. `pitch` is in bytes; `bt709` selects the colour matrix. A width or height
// of 0 keeps the decoded size and a pitch of 0 packs the rows tightly.
struct VideoOutputFormat {
    VideoCoding coding = VideoCoding::Argb;
    std::uint32_t width = 0, height = 0, pitch = 0;
    std::uint8_t alpha = 0xff;
    bool bt709 = false;
    // The file's start PTS: the anchor for the first decoded frame when no timestamp has been seen yet.
    std::int64_t start_pts = 0;
};

// Interleaved samples, big-endian as the game reads them.
struct AudioOutputFormat {
    std::uint32_t channels = 2, sample_rate = 48000;
    bool float32 = true;
};

struct VideoFrame {
    std::int64_t pts = -1;                           // 90 kHz
    std::uint32_t width = 0, height = 0, pitch = 0;  // of `bytes`; pitch is the luma pitch when planar
    std::vector<std::uint8_t> bytes;                 // guest layout, ready to memcpy
};

struct AudioChunk {
    std::int64_t pts = -1;  // 90 kHz
    std::uint32_t frames = 0;
    std::vector<std::uint8_t> bytes;
};

// All decoders are used from a single decode thread at a time.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // `es` is any split of the elementary stream (one PES payload); `pts` and `dts` belong to the
    // access unit that begins in it, -1 when the PES header carries none. False when data was rejected;
    // the rest of the stream stays decodable.
    virtual bool send(std::span<const std::uint8_t> es, std::int64_t pts, std::int64_t dts) = 0;
    // False when no frame is ready; call again after more input. After flush(), false means the
    // decoder is fully drained.
    virtual bool receive(VideoFrame& out) = 0;
    // End of stream: pending data is decoded and its frames become available through receive(). When
    // they are all taken the decoder is ready for a new stream.
    virtual void flush() = 0;
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    virtual bool send(std::span<const std::uint8_t> frame, std::int64_t pts) = 0;
    virtual bool receive(AudioChunk& out) = 0;
    virtual void flush() = 0;
};

class MediaCodecFactory {
public:
    virtual ~MediaCodecFactory() = default;
    // A null result sets `error`; the caller then plays black or silence.
    virtual std::unique_ptr<VideoDecoder> createVideo(const PamfStream& stream, const VideoOutputFormat& format,
                                                      std::string& error) = 0;
    virtual std::unique_ptr<AudioDecoder> createAudio(const PamfStream& stream, const AudioOutputFormat& format,
                                                      std::string& error) = 0;
};

// Installs the process-wide factory; null means no codecs are available. Call before the first
// movie opens: decode threads read the pointer without synchronisation beyond an atomic load, and
// the previous factory is destroyed here.
void setMediaCodecFactory(std::unique_ptr<MediaCodecFactory> factory);
MediaCodecFactory* mediaCodecFactory();

}  // namespace dal

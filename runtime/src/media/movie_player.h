#pragma once

// Host movie player: one PAMF file read, demuxed and decoded on a decode thread into bounded queues
// that the cellSail handlers poll. Core code: codecs come only through media/movie_decoder.h, so the
// player builds and tests without FFmpeg. It never takes the kernel lock and never calls into the guest;
// the consumer-side calls only take a short internal mutex and never wait for the decode thread.
//
// Time. All times are 90 kHz ticks. The player's clock is a function returning a monotonic tick count
// (default: the steady clock); `start()` captures its value as the origin. The playback time origin is
// `PamfInfo::start_pts`: a video frame is due when `pts - start_pts <= now - origin`, and the movie
// ends when `now - origin >= end_pts - start_pts`. Video frames and audio chunks keep the raw PTS the
// container gave them, so both streams share the one origin and the caller subtracts `startPts()` when
// it needs stream time.
//
// Audio is not gated by the clock: the game's sound thread pulls it, and it comes out in decode order.
// The first ATRAC3plus frame of the game's movies starts one frame (3840 ticks) before `start_pts`; it
// is delivered like the rest, never dropped or clamped. Trimming or aligning that lead-in is the
// caller's decision.
//
// Audio queue. With a video decoder, PCM never stalls the video decode: the default cellAudio may
// never pull audio. Above `audio_queue_frames`, chunks whose end the clock passed more than 0.1 s ago
// are dropped (`audio_stale_frames`; the grace covers a consumer that pulls in step but a little late); audio still in
// the future is kept, up to `audio_queue_cap_frames`, beyond which the oldest PCM is dropped (`audio_overflow_frames`).
// The decode thread reads the clock for this, so the clock function must be callable from any thread without locks of
// the caller's. Without a video decoder the decode waits for the consumer instead.
//
// Video backpressure. A full video queue also stops file reads, so the audio read ahead of the clock
// is set by how the file is multiplexed: in the PAMF movies this player was written against the audio ran
// up to 2.2 s ahead of the clock (`audio_max_lead_*` in the stats). That is why future audio is
// kept up to the hard cap instead of being limited to `audio_queue_frames`.
//
// Threading. `open()` must not run concurrently with any other call except `stop()`: the other calls
// read the stream table and formats it replaces without a lock.
//
// Shutdown. `stop()` is idempotent and the destructor calls it, because destroying a joinable thread
// would terminate the process. The decode thread checks the stop flag once per pass of its loop and
// waits only on a condition variable that `stop()` signals, so the join takes at most one pass (a
// file read, a demux step and one send and receive per stream). The
// supervisor leaves the process with `std::_Exit`, which runs no destructors: a player that is never
// stopped cannot delay process exit, and nothing here is a static.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/movie_decoder.h"
#include "media/pamf.h"

namespace tox2 {

// Returns monotonic 90 kHz ticks. Tests inject their own.
using MovieClock = std::function<std::int64_t()>;

std::int64_t steadyClockTicks();

struct MoviePlayerOptions {
    MovieClock clock;  // empty: steadyClockTicks
    // Converted frames held ahead of the consumer. The decode thread blocks when the queue is full.
    std::size_t video_queue_frames = 4;
    // Decoded PCM held ahead of the consumer, in sample frames (about 0.5 s at 48 kHz).
    std::uint32_t audio_queue_frames = 24000;
    // Memory bound for the PCM queue while a video decoder runs (10 s at 48 kHz); see "Audio queue".
    std::uint32_t audio_queue_cap_frames = 480000;
    std::size_t read_chunk_bytes = 256 * 1024;
};

struct MovieDueVideo {
    VideoFrame frame;
    std::uint32_t dropped = 0;  // due frames skipped because a newer one was due as well
};

struct MovieAudioBlock {
    std::int64_t pts = 0;  // of the first sample frame, 90 kHz, same origin as the video PTS
    std::uint32_t frames = 0;
    std::uint32_t real_frames = 0;  // decoded sample frames at the front; the rest is zero-filled silence
    std::vector<std::uint8_t> bytes;
};

struct MoviePlayerStats {
    bool video_decoder = false, audio_decoder = false;  // a decoder was created for the stream
    std::string codec_error;                            // why a decoder is missing, empty when none is
    bool decode_finished = false;
    std::uint32_t width = 0, height = 0;                   // of the video stream header
    std::uint32_t frame_rate_num = 0, frame_rate_den = 0;  // 0 without a video stream
    std::int64_t duration_ticks = 0;
    std::int64_t played_ticks = 0;  // clock progress seen by takeDueVideo/atEnd, at most the duration
    std::uint64_t video_decoded = 0, video_delivered = 0, video_dropped = 0, video_rejected = 0;
    // Time the decode thread spent inside the video decoder's send() and receive() (decode and colour
    // conversion), summed, and the longest single call. Blocking on the queues is not counted.
    std::uint64_t video_decode_ns = 0, video_decode_max_ns = 0;
    std::uint64_t audio_decoded_frames = 0, audio_delivered_frames = 0, audio_silence_frames = 0;
    std::uint64_t audio_underruns = 0;  // takeAudio calls that had to pad the block with silence
    std::uint64_t audio_rejected = 0, audio_bad_chunks = 0;
    std::uint64_t audio_stale_frames = 0;     // unread PCM dropped because the clock had passed its end
    std::uint64_t audio_overflow_frames = 0;  // unread PCM dropped at the hard cap, still in the future
    // The largest distance by which queued audio (its end PTS) was ahead of the clock when a chunk was
    // queued, when that happened (ticks since the origin) and how many video frames were decoded by then.
    std::int64_t audio_max_lead_ticks = 0, audio_max_lead_at_ticks = 0;
    std::uint64_t audio_max_lead_video_decoded = 0;
    std::uint64_t demux_errors = 0, read_errors = 0;
};

class MoviePlayer {
public:
    explicit MoviePlayer(MoviePlayerOptions options = {});
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    // Reads and parses the header (one read of at most 2048 bytes) and returns the stream table. Stops a
    // running decode first and forgets the previous movie. The first video and the first audio stream of
    // the table are played; `video.start_pts` is overwritten with the file's start PTS.
    std::expected<PamfInfo, std::string> open(const std::filesystem::path& path, const VideoOutputFormat& video,
                                              const AudioOutputFormat& audio);

    // Captures the clock origin and launches the decode thread. False without a successful open() or
    // when already started.
    bool start();
    // Joins the decode thread. Queued frames and PCM stay readable.
    void stop();

    // The player's clock; pass its values to takeDueVideo and atEnd.
    std::int64_t now() const;
    // Moves the origin to the current clock value. For a caller that waited for waitPrerolled() so that
    // the decode start-up latency does not make the first frames late.
    void restartClock();
    // True once the first video frame and the first PCM chunk are queued (a stream without a decoder or
    // with nothing to decode counts as done), or when the timeout expires: returns false then.
    bool waitPrerolled(std::chrono::milliseconds timeout);

    // The newest queued frame that is due at `now`; older due frames are removed and counted as dropped.
    // Nothing is returned when no frame is due. Frames of a stream without a video decoder: one black
    // frame (zero RGB, the format's alpha; Y 16 and U, V 128 when planar) at `start_pts`, nothing after,
    // so the caller keeps showing it.
    std::optional<MovieDueVideo> takeDueVideo(std::int64_t now);
    // Exactly `frames` sample frames in the requested AudioOutputFormat: decoded PCM first, zero-filled
    // silence after it when the queue runs dry or the stream has no decoder. Chunks come out whole or in
    // part in decode order; the PTS of a part is the chunk's PTS plus its offset at the requested sample
    // rate (an AudioChunk carries no rate, so a stream with another rate is not detected here). Silence
    // continues at the end of the previous block, or at `start_pts` before any block.
    MovieAudioBlock takeAudio(std::uint32_t frames);
    // The movie's wall-clock end: `now` is at least the duration past the origin. The decode thread may
    // still be running (a slow decoder drops frames, the clock does not wait) and queues may hold data.
    bool atEnd(std::int64_t now);

    std::int64_t startPts() const { return static_cast<std::int64_t>(info_.start_pts); }
    std::int64_t durationTicks() const { return static_cast<std::int64_t>(info_.end_pts - info_.start_pts); }
    const PamfInfo& info() const { return info_; }
    MoviePlayerStats stats() const;

private:
    struct Counters {
        std::atomic<std::uint64_t> video_decoded{0}, video_delivered{0}, video_dropped{0}, video_rejected{0};
        std::atomic<std::uint64_t> video_decode_ns{0}, video_decode_max_ns{0};
        std::atomic<std::uint64_t> audio_decoded_frames{0}, audio_delivered_frames{0}, audio_silence_frames{0};
        std::atomic<std::uint64_t> audio_underruns{0};
        std::atomic<std::uint64_t> audio_rejected{0}, audio_bad_chunks{0}, audio_overflow_frames{0};
        std::atomic<std::uint64_t> audio_stale_frames{0};
        std::atomic<std::uint64_t> demux_errors{0}, read_errors{0};
    };

    void joinLocked();
    void run();
    void runGuarded();
    bool stopRequested() const { return stop_.load(std::memory_order_acquire); }
    bool tryPushVideo(VideoFrame& frame);
    bool tryPushAudio(AudioChunk& chunk);
    void noteElapsedLocked(std::int64_t elapsed);
    bool prerolledLocked() const;
    std::uint32_t audioFrameBytes() const;

    MoviePlayerOptions options_;
    MovieClock clock_;

    // Set by open(), read-only while the thread runs.
    std::filesystem::path path_;
    PamfInfo info_;
    VideoOutputFormat video_format_;
    AudioOutputFormat audio_format_;
    std::optional<std::uint32_t> video_index_, audio_index_;
    bool opened_ = false;

    // start()/stop()/open() serialise on this; the decode thread never takes it.
    std::mutex lifecycle_mutex_;
    std::thread thread_;
    bool started_ = false;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t generation_ = 0;  // incremented when the consumer frees queue space or stop is requested
    std::int64_t origin_ = 0;
    std::int64_t seen_elapsed_ = 0;
    std::deque<VideoFrame> video_queue_;
    std::deque<AudioChunk> audio_queue_;
    std::size_t audio_offset_ = 0;           // sample frames already taken from audio_queue_.front()
    std::uint64_t audio_queued_frames_ = 0;  // unread sample frames in audio_queue_
    // PTS of the next block is anchor_pts + anchor_frames * 90000 / rate; see takeAudio.
    std::int64_t audio_anchor_pts_ = 0, audio_anchor_frames_ = 0;
    std::int64_t audio_max_lead_ = 0, audio_max_lead_at_ = 0;
    std::uint64_t audio_max_lead_video_decoded_ = 0;
    bool decoders_ready_ = false;  // the thread has tried to create the decoders
    bool video_active_ = false, audio_active_ = false;
    bool video_seen_ = false, audio_seen_ = false;  // a frame / chunk has been queued at least once
    bool finished_ = false;
    std::string codec_error_;
    Counters counters_;
};

}  // namespace tox2

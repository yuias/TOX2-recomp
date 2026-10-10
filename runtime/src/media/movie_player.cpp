#include "media/movie_player.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

#include "core/log.h"

namespace tox2 {

namespace {

constexpr std::size_t kHeaderReadSize = 2048;
constexpr std::int64_t kTicksPerSecond = 90000;
// The format comes from guest memory, so the black frame is bounded like the converter's output.
constexpr std::uint64_t kMaxBlackFrameBytes = 64ull << 20;
constexpr std::uint32_t kMaxTakeFrames = 1u << 18;
// A consumer that pulls in step with the clock is always a little behind it (polling jitter, and the
// first chunk of a movie starts before the start PTS); audio is stale only beyond this lateness.
constexpr std::int64_t kStaleGraceTicks = 9000;  // 0.1 s
constexpr std::uint32_t kMaxAudioChannels = 8;

bool isVideoCodec(PamfCodec codec) { return codec == PamfCodec::Avc || codec == PamfCodec::M2v; }
bool isAudioCodec(PamfCodec codec) {
    return codec == PamfCodec::Atrac3plus || codec == PamfCodec::Lpcm || codec == PamfCodec::Ac3;
}

// Frame-rate codes of the PAMF AVC stream info. The same table is in ffmpeg_codecs.cpp, which core
// code cannot link.
struct FrameRate {
    std::uint32_t num, den;
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

std::optional<VideoFrame> makeBlackFrame(const VideoOutputFormat& format, const PamfStream& stream, std::int64_t pts) {
    const std::uint32_t width = format.width != 0 ? format.width : stream.width;
    const std::uint32_t height = format.height != 0 ? format.height : stream.height;
    if (width == 0 || height == 0 || width > (1u << 16) || height > (1u << 16) || format.pitch > (1u << 20)) {
        return std::nullopt;
    }
    const bool planar = format.coding == VideoCoding::Yuv420Planar;
    const std::uint64_t row = planar ? width : std::uint64_t{width} * 4;
    const std::uint64_t pitch = std::max<std::uint64_t>(format.pitch != 0 ? format.pitch : row, row);
    const std::uint64_t chroma_pitch = (pitch + 1) / 2, chroma_rows = (std::uint64_t{height} + 1) / 2;
    const std::uint64_t luma_bytes = pitch * height;
    const std::uint64_t total = luma_bytes + (planar ? 2 * chroma_pitch * chroma_rows : 0);
    if (total > kMaxBlackFrameBytes) return std::nullopt;

    VideoFrame frame;
    frame.pts = pts;
    frame.width = width;
    frame.height = height;
    frame.pitch = static_cast<std::uint32_t>(pitch);
    frame.bytes.assign(static_cast<std::size_t>(total), 0);
    if (planar) {
        // Black in limited-range YUV.
        std::fill_n(frame.bytes.begin(), static_cast<std::ptrdiff_t>(luma_bytes), std::uint8_t{16});
        std::fill(frame.bytes.begin() + static_cast<std::ptrdiff_t>(luma_bytes), frame.bytes.end(), std::uint8_t{128});
    } else {
        const std::size_t alpha_at = format.coding == VideoCoding::Argb ? 0 : 3;
        for (std::uint32_t y = 0; y < height; y++) {
            std::uint8_t* line = frame.bytes.data() + static_cast<std::size_t>(y * pitch);
            for (std::uint32_t x = 0; x < width; x++) line[std::size_t{x} * 4 + alpha_at] = format.alpha;
        }
    }
    return frame;
}

}  // namespace

std::int64_t steadyClockTicks() {
    const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                    .count();
    // 90000 / 1e9 = 9 / 100000.
    return ns * 9 / 100000;
}

MoviePlayer::MoviePlayer(MoviePlayerOptions options) : options_(std::move(options)) {
    options_.video_queue_frames = std::max<std::size_t>(options_.video_queue_frames, 1);
    options_.audio_queue_frames = std::max<std::uint32_t>(options_.audio_queue_frames, 1);
    options_.audio_queue_cap_frames = std::max(options_.audio_queue_cap_frames, options_.audio_queue_frames);
    options_.read_chunk_bytes = std::max<std::size_t>(options_.read_chunk_bytes, 1);
    clock_ = options_.clock ? options_.clock : MovieClock(steadyClockTicks);
}

MoviePlayer::~MoviePlayer() { stop(); }

std::uint32_t MoviePlayer::audioFrameBytes() const {
    if (audio_format_.channels == 0 || audio_format_.channels > kMaxAudioChannels) return 0;
    return audio_format_.channels * (audio_format_.float32 ? 4u : 2u);
}

std::expected<PamfInfo, std::string> MoviePlayer::open(const std::filesystem::path& path,
                                                       const VideoOutputFormat& video, const AudioOutputFormat& audio) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    joinLocked();
    opened_ = false;
    started_ = false;
    // Forget the previous movie first, so a failed open leaves nothing of it readable.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_queue_.clear();
        audio_queue_.clear();
        audio_offset_ = 0;
        audio_queued_frames_ = 0;
        audio_anchor_pts_ = 0;
        audio_anchor_frames_ = 0;
        audio_max_lead_ = audio_max_lead_at_ = 0;
        audio_max_lead_video_decoded_ = 0;
        origin_ = 0;
        seen_elapsed_ = 0;
        generation_ = 0;
        decoders_ready_ = video_active_ = audio_active_ = false;
        video_seen_ = audio_seen_ = false;
        finished_ = false;
        codec_error_.clear();
    }
    counters_.video_decoded = counters_.video_delivered = counters_.video_dropped = counters_.video_rejected = 0;
    counters_.video_decode_ns = counters_.video_decode_max_ns = 0;
    counters_.audio_decoded_frames = counters_.audio_delivered_frames = counters_.audio_silence_frames = 0;
    counters_.audio_underruns = 0;
    counters_.audio_rejected = counters_.audio_bad_chunks = counters_.audio_overflow_frames = 0;
    counters_.audio_stale_frames = 0;
    counters_.demux_errors = counters_.read_errors = 0;

    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("cannot open " + path.string());
    std::vector<std::uint8_t> header(kHeaderReadSize);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::size_t got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
    auto parsed = parsePamfHeader(std::span<const std::uint8_t>(header).first(got));
    if (!parsed) return std::unexpected(parsed.error());

    path_ = path;
    info_ = std::move(*parsed);
    video_format_ = video;
    // Forgetting this would silently anchor frames without a PTS at zero.
    video_format_.start_pts = static_cast<std::int64_t>(info_.start_pts);
    audio_format_ = audio;
    if (audio_format_.sample_rate == 0) audio_format_.sample_rate = 48000;

    video_index_.reset();
    audio_index_.reset();
    for (std::uint32_t i = 0; i < info_.streams.size(); i++) {
        if (!video_index_ && isVideoCodec(info_.streams[i].codec)) video_index_ = i;
        if (!audio_index_ && isAudioCodec(info_.streams[i].codec)) audio_index_ = i;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_anchor_pts_ = static_cast<std::int64_t>(info_.start_pts);
        audio_anchor_frames_ = 0;
    }
    opened_ = true;
    return info_;
}

bool MoviePlayer::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!opened_ || started_) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        origin_ = clock_();
        stop_.store(false, std::memory_order_release);
    }
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& e) {
        logWarn("tox2: movie: cannot start the decode thread: {}", e.what());
        return false;
    }
    started_ = true;
    return true;
}

void MoviePlayer::joinLocked() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true, std::memory_order_release);
        generation_++;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MoviePlayer::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    joinLocked();
}

std::int64_t MoviePlayer::now() const { return clock_(); }

void MoviePlayer::restartClock() {
    std::lock_guard<std::mutex> lock(mutex_);
    origin_ = clock_();
    seen_elapsed_ = 0;
}

bool MoviePlayer::prerolledLocked() const {
    if (finished_) return true;
    return decoders_ready_ && (!video_active_ || video_seen_) && (!audio_active_ || audio_seen_);
}

bool MoviePlayer::waitPrerolled(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return prerolledLocked() || stopRequested(); });
    return prerolledLocked();
}

void MoviePlayer::noteElapsedLocked(std::int64_t elapsed) { seen_elapsed_ = std::max(seen_elapsed_, elapsed); }

std::optional<MovieDueVideo> MoviePlayer::takeDueVideo(std::int64_t now) {
    std::optional<MovieDueVideo> result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t elapsed = std::max<std::int64_t>(now - origin_, 0);
        noteElapsedLocked(elapsed);
        const std::int64_t media_time = static_cast<std::int64_t>(info_.start_pts) + elapsed;
        std::uint32_t due = 0;
        // A frame without a PTS (-1) is due at once.
        while (!video_queue_.empty() && video_queue_.front().pts <= media_time) {
            if (!result) result.emplace();
            result->frame = std::move(video_queue_.front());
            video_queue_.pop_front();
            due++;
        }
        if (result) {
            result->dropped = due - 1;
            counters_.video_delivered++;
            counters_.video_dropped += due - 1;
            generation_++;
        }
    }
    if (result) cv_.notify_all();
    return result;
}

MovieAudioBlock MoviePlayer::takeAudio(std::uint32_t frames) {
    MovieAudioBlock block;
    const std::uint32_t frame_bytes = audioFrameBytes();
    if (frame_bytes == 0) return block;
    frames = std::min(frames, kMaxTakeFrames);
    const std::int64_t rate = audio_format_.sample_rate;
    block.frames = frames;
    block.bytes.assign(std::size_t{frames} * frame_bytes, 0);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // PTS = anchor + sample frames since the anchor, rounded once, so that the rounding of one block
        // does not carry into the next. A chunk at the front is a new anchor; silence extends the last one.
        if (!audio_queue_.empty()) {
            audio_anchor_pts_ = audio_queue_.front().pts;
            audio_anchor_frames_ = static_cast<std::int64_t>(audio_offset_);
        }
        block.pts = audio_anchor_pts_ + audio_anchor_frames_ * kTicksPerSecond / rate;
        while (block.real_frames < frames && !audio_queue_.empty()) {
            const AudioChunk& chunk = audio_queue_.front();
            const std::size_t available = chunk.frames - audio_offset_;
            const std::size_t take = std::min<std::size_t>(available, frames - block.real_frames);
            std::memcpy(block.bytes.data() + std::size_t{block.real_frames} * frame_bytes,
                        chunk.bytes.data() + audio_offset_ * frame_bytes, take * frame_bytes);
            block.real_frames += static_cast<std::uint32_t>(take);
            audio_offset_ += take;
            audio_queued_frames_ -= take;
            if (audio_offset_ == chunk.frames) {
                audio_queue_.pop_front();
                audio_offset_ = 0;
            }
        }
        audio_anchor_frames_ += frames;
        counters_.audio_delivered_frames += block.real_frames;
        counters_.audio_silence_frames += frames - block.real_frames;
        if (block.real_frames < frames) counters_.audio_underruns++;
        // The queue limit counts sample frames, so any read can make room.
        if (block.real_frames != 0) generation_++;
    }
    if (block.real_frames != 0) cv_.notify_all();
    return block;
}

bool MoviePlayer::atEnd(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t elapsed = std::max<std::int64_t>(now - origin_, 0);
    noteElapsedLocked(elapsed);
    return elapsed >= durationTicks();
}

MoviePlayerStats MoviePlayer::stats() const {
    MoviePlayerStats s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.video_decoder = video_active_;
        s.audio_decoder = audio_active_;
        s.codec_error = codec_error_;
        s.decode_finished = finished_;
        s.played_ticks = std::min(seen_elapsed_, durationTicks());
        s.audio_max_lead_ticks = audio_max_lead_;
        s.audio_max_lead_at_ticks = audio_max_lead_at_;
        s.audio_max_lead_video_decoded = audio_max_lead_video_decoded_;
    }
    s.duration_ticks = durationTicks();
    if (video_index_) {
        s.width = info_.streams[*video_index_].width;
        s.height = info_.streams[*video_index_].height;
        const FrameRate rate = frameRateOf(info_.streams[*video_index_]);
        s.frame_rate_num = rate.num;
        s.frame_rate_den = rate.den;
    }
    s.video_decoded = counters_.video_decoded;
    s.video_delivered = counters_.video_delivered;
    s.video_dropped = counters_.video_dropped;
    s.video_rejected = counters_.video_rejected;
    s.video_decode_ns = counters_.video_decode_ns;
    s.video_decode_max_ns = counters_.video_decode_max_ns;
    s.audio_decoded_frames = counters_.audio_decoded_frames;
    s.audio_delivered_frames = counters_.audio_delivered_frames;
    s.audio_silence_frames = counters_.audio_silence_frames;
    s.audio_underruns = counters_.audio_underruns;
    s.audio_rejected = counters_.audio_rejected;
    s.audio_bad_chunks = counters_.audio_bad_chunks;
    s.audio_overflow_frames = counters_.audio_overflow_frames;
    s.audio_stale_frames = counters_.audio_stale_frames;
    s.demux_errors = counters_.demux_errors;
    s.read_errors = counters_.read_errors;
    return s;
}

bool MoviePlayer::tryPushVideo(VideoFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_queue_.size() >= options_.video_queue_frames) return false;
        video_queue_.push_back(std::move(frame));
        video_seen_ = true;
    }
    cv_.notify_all();
    return true;
}

bool MoviePlayer::tryPushAudio(AudioChunk& chunk) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t rate = audio_format_.sample_rate;
        const std::int64_t elapsed = std::max<std::int64_t>(clock_() - origin_, 0);
        const std::int64_t media_time = static_cast<std::int64_t>(info_.start_pts) + elapsed;
        auto endPts = [rate](const AudioChunk& c) {
            return c.pts + static_cast<std::int64_t>(c.frames) * kTicksPerSecond / rate;
        };
        auto dropFront = [this](std::atomic<std::uint64_t>& counter) {
            const std::uint64_t unread = audio_queue_.front().frames - audio_offset_;
            audio_queued_frames_ -= unread;
            counter += unread;
            audio_queue_.pop_front();
            audio_offset_ = 0;
        };

        if (video_active_) {
            // Nobody may be pulling audio (the default cellAudio never asks for it), and PCM must never stall
            // the video decode. Above the soft limit, audio the clock has already passed is dropped: nobody
            // can play it any more. Audio still in the future is kept up to the hard cap, which only bounds
            // memory (a stalled clock, an unpulled queue); beyond it the oldest goes.
            while (!audio_queue_.empty() && audio_queued_frames_ + chunk.frames > options_.audio_queue_frames &&
                   endPts(audio_queue_.front()) + kStaleGraceTicks < media_time) {
                dropFront(counters_.audio_stale_frames);
            }
            while (!audio_queue_.empty() && audio_queued_frames_ + chunk.frames > options_.audio_queue_cap_frames) {
                dropFront(counters_.audio_overflow_frames);
            }
        } else if (audio_queued_frames_ >= options_.audio_queue_frames) {
            // Without video the decode only feeds audio, so it waits for the consumer instead of reading
            // ahead. One chunk may overshoot the limit; chunks are never split.
            return false;
        }
        if (const std::int64_t lead = endPts(chunk) - media_time; lead > audio_max_lead_) {
            audio_max_lead_ = lead;
            audio_max_lead_at_ = elapsed;
            audio_max_lead_video_decoded_ = counters_.video_decoded;
        }
        audio_queued_frames_ += chunk.frames;
        audio_queue_.push_back(std::move(chunk));
        audio_seen_ = true;
    }
    cv_.notify_all();
    return true;
}

void MoviePlayer::run() {
    try {
        runGuarded();
    } catch (const std::exception& e) {
        logWarn("tox2: movie: decode thread failed: {}", e.what());
    } catch (...) {
        logWarn("tox2: movie: decode thread failed");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
}

void MoviePlayer::runGuarded() {
    MediaCodecFactory* factory = mediaCodecFactory();
    std::unique_ptr<VideoDecoder> video;
    std::unique_ptr<AudioDecoder> audio;
    std::string error;
    auto addError = [&error](const std::string& what) {
        if (!error.empty()) error += "; ";
        error += what;
    };
    if (factory == nullptr) {
        if (video_index_ || audio_index_) addError("no codec factory");
    } else {
        std::string reason;
        if (video_index_) {
            video = factory->createVideo(info_.streams[*video_index_], video_format_, reason);
            if (!video) addError("video: " + reason);
        }
        reason.clear();
        if (audio_index_ && audioFrameBytes() != 0) {
            audio = factory->createAudio(info_.streams[*audio_index_], audio_format_, reason);
            if (!audio) addError("audio: " + reason);
        } else if (audio_index_) {
            addError("audio: unsupported output format");
        }
    }

    // Without a video decoder the movie shows black; built before the lock because it can be large.
    std::optional<VideoFrame> black;
    if (video_index_ && !video) {
        black = makeBlackFrame(video_format_, info_.streams[*video_index_], static_cast<std::int64_t>(info_.start_pts));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video_active_ = video != nullptr;
        audio_active_ = audio != nullptr;
        codec_error_ = error;
        if (black) {
            video_queue_.push_back(std::move(*black));
            video_seen_ = true;
        }
        decoders_ready_ = true;
    }
    cv_.notify_all();
    if (!error.empty()) logDebug("tox2: movie: {}: {}", path_.filename().string(), error);
    if (!video && !audio) return;

    std::ifstream in(path_, std::ios::binary);
    std::uint64_t remaining = info_.data_size;
    bool eof = false;
    if (in) {
        in.seekg(static_cast<std::streamoff>(info_.data_offset));
    }
    if (!in) {
        counters_.read_errors++;
        eof = true;
    }

    PamfDemuxer demux(info_);
    std::vector<std::uint8_t> buffer(options_.read_chunk_bytes);
    std::optional<VideoFrame> held_video;
    std::optional<AudioChunk> held_audio;
    bool ended = false, flushed = false;
    const std::uint32_t frame_bytes = audioFrameBytes();
    const std::int64_t rate = audio_format_.sample_rate;
    std::int64_t audio_expected_pts = static_cast<std::int64_t>(info_.start_pts);
    // Runs one video decoder call and charges its duration to the decode-time counters.
    auto timedVideo = [this](auto&& call) {
        const auto begin = std::chrono::steady_clock::now();
        const bool result = call();
        const auto ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        counters_.video_decode_ns += ns;
        if (ns > counters_.video_decode_max_ns.load(std::memory_order_relaxed)) counters_.video_decode_max_ns = ns;
        return result;
    };

    // Each pass moves at most one access unit per stream, so a stop request is seen within one pass. A
    // decoded frame that does not fit in its queue is held, which stops further input to that decoder;
    // the other stream keeps going on the access units already demuxed, but no file is read while
    // either stream holds output (this bounds the demuxer's backlog).
    for (;;) {
        std::uint64_t seen;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seen = generation_;
        }
        if (stopRequested()) break;
        bool progress = false;

        if (held_video && tryPushVideo(*held_video)) {
            held_video.reset();
            progress = true;
        }
        if (held_audio && tryPushAudio(*held_audio)) {
            held_audio.reset();
            progress = true;
        }

        bool video_idle = false, audio_idle = false;
        if (video && !held_video) {
            VideoFrame frame;
            if (timedVideo([&] { return video->receive(frame); })) {
                counters_.video_decoded++;
                held_video = std::move(frame);
                progress = true;
            } else {
                video_idle = true;
            }
        }
        if (audio && !held_audio) {
            AudioChunk chunk;
            if (audio->receive(chunk)) {
                progress = true;
                counters_.audio_decoded_frames += chunk.frames;
                if (chunk.frames == 0 || chunk.bytes.size() != std::size_t{chunk.frames} * frame_bytes) {
                    // Another channel count or sample format than requested: it cannot be mixed in.
                    counters_.audio_bad_chunks++;
                } else {
                    if (chunk.pts < 0) chunk.pts = audio_expected_pts;
                    audio_expected_pts = chunk.pts + static_cast<std::int64_t>(chunk.frames) * kTicksPerSecond / rate;
                    held_audio = std::move(chunk);
                }
            } else {
                audio_idle = true;
            }
        }

        // Streams that are not decoded (no decoder, or not the chosen one) cost nothing but their demux.
        for (std::uint32_t i = 0; i < info_.streams.size(); i++) {
            const bool decoded = (video && video_index_ == i) || (audio && audio_index_ == i);
            if (!decoded)
                while (demux.next(i)) {
                }
        }

        // Starved: no decoder has output or demuxed input left, so only a read or the end can go on.
        bool starved = true;
        if (video) {
            if (!video_idle) {
                starved = false;
            } else if (!flushed) {
                if (auto au = demux.next(*video_index_)) {
                    if (!timedVideo([&] { return video->send(au->bytes, au->pts, au->dts); }))
                        counters_.video_rejected++;
                    progress = true;
                    starved = false;
                }
            }
        }
        if (audio) {
            if (!audio_idle) {
                starved = false;
            } else if (!flushed) {
                if (auto au = demux.next(*audio_index_)) {
                    if (!audio->send(au->bytes, au->pts)) counters_.audio_rejected++;
                    progress = true;
                    starved = false;
                }
            }
        }
        if (progress) continue;

        if (!starved) {
            // A held frame has no room: wait for the consumer or for stop().
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopRequested() || generation_ != seen; });
            continue;
        }

        if (!eof) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            std::size_t got = 0;
            if (want != 0) {
                in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
                got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
                demux.feed(std::span<const std::uint8_t>(buffer).first(got));
                remaining -= got;
            }
            if (got < want) counters_.read_errors++;  // the file is shorter than its header says
            if (got < want || remaining == 0) eof = true;
            counters_.demux_errors = demux.errors();
            continue;
        }
        if (!ended) {
            demux.endOfStream();
            counters_.demux_errors = demux.errors();
            ended = true;
            continue;
        }
        if (!flushed) {
            // End of stream for the codecs; what they still hold comes out of receive().
            if (video) video->flush();
            if (audio) audio->flush();
            flushed = true;
            continue;
        }
        break;  // drained
    }
    counters_.demux_errors = demux.errors();
}

}  // namespace tox2

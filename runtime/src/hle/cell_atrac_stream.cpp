#include "hle/cell_atrac_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace tox2 {
namespace {

constexpr std::uint32_t kFrameSamples = kAtrac3plusFrameSamples;

bool supportedRate(std::uint32_t rate) { return rate == 32000 || rate == 44100 || rate == 48000; }

std::int32_t clampToInt32(std::uint64_t value) {
    return static_cast<std::int32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::int32_t>::max()));
}

std::string headHex(std::span<const std::uint8_t> bytes) {
    std::string hex;
    for (std::size_t i = 0; i < std::min<std::size_t>(16, bytes.size()); ++i) hex += std::format("{:02x}", bytes[i]);
    return hex;
}

}  // namespace

std::expected<AtracStream, std::uint32_t> AtracStream::open(std::span<const std::uint8_t> head,
                                                            std::uint32_t buffer_addr, std::uint32_t buffer_bytes,
                                                            std::string& error) {
    if (head.size() > buffer_bytes) {
        error = std::format("{} bytes read into a {} byte buffer", head.size(), buffer_bytes);
        return std::unexpected(kAtracErrorReadsizeOverBuffer);
    }
    auto parsed = parseAt3Header(head);
    if (!parsed) {
        error = std::move(parsed.error());
        return std::unexpected(kAtracErrorUnknownFormat);
    }

    AtracStream stream;
    stream.info_ = *parsed;
    const At3Info& info = stream.info_;
    // parseAt3Header accepts up to 8 channels; the game's files and this model have 1 or 2.
    if (info.channels != 1 && info.channels != 2) {
        error = std::format("{} channels", info.channels);
        return std::unexpected(kAtracErrorIllegalData);
    }
    if (!supportedRate(info.sample_rate)) {
        error = std::format("sampling rate {} Hz", info.sample_rate);
        return std::unexpected(kAtracErrorIllegalSamplingRate);
    }

    stream.buffer_addr_ = buffer_addr;
    stream.buffer_bytes_ = buffer_bytes;
    // The buffer is only the whole file when every frame has been read. A read that fills the buffer
    // while the file is longer (the game's first 0x40000 bytes) is a streaming ring that starts full.
    stream.memory_mode_ = head.size() >= std::uint64_t{info.data_offset} + info.data_size;
    const std::uint64_t frames = info.frameCount();
    if (frames == 0 || info.block_align > buffer_bytes) {
        error = std::format("no complete frame (block_align {}, data {} bytes)", info.block_align, info.data_size);
        return std::unexpected(kAtracErrorIllegalData);
    }
    stream.frames_ = static_cast<std::uint32_t>(frames);
    stream.file_end_ = stream.frameOffset(stream.frames_);

    const std::uint64_t decoded_samples = frames * kFrameSamples;
    // Without a `fact` chunk the whole decoder output counts.
    stream.valid_total_ = info.total_samples != 0
                                  ? info.total_samples
                                  : (decoded_samples > info.delay_samples ? decoded_samples - info.delay_samples : 0);
    stream.out_end_ = std::min<std::uint64_t>(std::uint64_t{info.delay_samples} + stream.valid_total_, decoded_samples);

    // The game's first read stops at the pass end too; bytes of a looping file past it are never fed.
    stream.buffered_end_ =
            std::min<std::uint64_t>(head.size(), stream.memory_mode_ ? stream.file_end_ : stream.passFileEnd());
    stream.emit_from_ = info.delay_samples;
    return stream;
}

std::uint32_t AtracStream::createDecoder(std::unique_ptr<AtracFrameDecoder> decoder) {
    if (created_) return kAtracErrorDecoderWasCreated;
    created_ = true;
    decoder_ = std::move(decoder);
    frame_samples_.assign(std::size_t{kFrameSamples} * info_.channels, 0.0f);
    cursor_ = 0;
    emit_from_ = info_.delay_samples;
    return 0;
}

bool AtracStream::loopUsable() const {
    if (!info_.loop) return false;
    // A loop whose region holds no valid audio would make a pass produce nothing forever.
    const std::uint64_t first = std::max<std::uint64_t>(info_.loop->start_sample, info_.delay_samples);
    const std::uint64_t last = std::min<std::uint64_t>(std::uint64_t{info_.loop->end_sample} + 1, out_end_);
    return first < last;
}

bool AtracStream::loopBackPending() const { return (loop_num_ == -1 || loop_num_ > 1) && loopUsable(); }

std::uint64_t AtracStream::passEnd() const {
    if (!loopBackPending()) {
        // A streamed file with a `smpl` loop is fed only up to the end of the frame that holds the loop
        // end, so the last pass cannot play the frames after it.
        if (memory_mode_ || !hasSmplLoop()) return out_end_;
        const std::uint64_t fed = (std::uint64_t{info_.loop->end_sample} / kFrameSamples + 1) * kFrameSamples;
        return std::min(out_end_, fed);
    }
    return std::min<std::uint64_t>(std::uint64_t{info_.loop->end_sample} + 1, out_end_);
}

std::uint32_t AtracStream::restartFrame() const {
    if (!info_.loop) return 0;
    // One frame before the loop start primes the decoder's overlap; its output is discarded.
    const std::uint32_t start_frame = info_.loop->start_sample / kFrameSamples;
    return std::min(start_frame > 0 ? start_frame - 1 : 0, frames_);
}

std::uint64_t AtracStream::loopRestartOffset() const { return frameOffset(restartFrame()); }

std::uint64_t AtracStream::passFileEnd() const {
    if (!hasSmplLoop()) return file_end_;
    return std::min(frameOffset(info_.loop->end_sample / kFrameSamples + 1), file_end_);
}

std::uint64_t AtracStream::loopLength() const {
    const std::uint64_t restart = loopRestartOffset();
    const std::uint64_t end = passFileEnd();
    return end > restart ? end - restart : 0;
}

std::uint64_t AtracStream::streamPos(std::uint32_t pass, std::uint32_t frame) const {
    const std::uint64_t offset = frameOffset(frame);
    if (memory_mode_ || pass == 0) return offset;
    const std::uint64_t restart = loopRestartOffset();
    return passFileEnd() + std::uint64_t{pass - 1} * loopLength() + (offset > restart ? offset - restart : 0);
}

std::uint64_t AtracStream::fileOffsetOfStream(std::uint64_t s) const {
    const std::uint64_t end = passFileEnd();
    if (s < end || !hasSmplLoop()) return s;
    const std::uint64_t length = loopLength();
    return loopRestartOffset() + (length > 0 ? (s - end) % length : 0);
}

std::uint64_t AtracStream::streamLimit() const {
    if (!loopUsable()) return passFileEnd();
    if (loop_num_ == -1) return std::numeric_limits<std::uint64_t>::max();
    // The last pass to play is the current one plus the loop-backs the count still allows.
    const std::uint64_t last_pass = std::uint64_t{pass_} + (loop_num_ > 1 ? loop_num_ : 1) - 1;
    return passFileEnd() + last_pass * loopLength();
}

std::int32_t AtracStream::remain() const {
    if (memory_mode_) return kAtracRemainAllDataOnMemory;
    if (buffered_end_ >= streamLimit()) return kAtracRemainNonLoopStreamOnMemory;
    const std::uint64_t head = streamPos(pass_, cursor_);
    const std::uint64_t ahead = buffered_end_ > head ? (buffered_end_ - head) / info_.block_align : 0;
    return clampToInt32(ahead);
}

AtracSoundInfo AtracStream::soundInfo() const {
    AtracSoundInfo sound;
    sound.end_sample = valid_total_ > 0 ? clampToInt32(valid_total_ - 1) : 0;
    if (info_.loop) {
        const std::uint32_t delay = info_.delay_samples;
        sound.loop_start = clampToInt32(info_.loop->start_sample > delay ? info_.loop->start_sample - delay : 0);
        sound.loop_end = clampToInt32(info_.loop->end_sample > delay ? info_.loop->end_sample - delay : 0);
    }
    return sound;
}

std::uint32_t AtracStream::vacant() const {
    if (memory_mode_) return 0;
    const std::uint64_t head = streamPos(pass_, cursor_);
    const std::uint64_t live = buffered_end_ > head ? buffered_end_ - head : 0;
    if (live >= buffer_bytes_) return 0;
    // The file size is no limit for a looping stream: the game feeds the loop region again after the pass
    // end, and it takes a read only whole.
    const std::uint64_t limit = streamLimit();
    const std::uint64_t to_limit = limit > buffered_end_ ? limit - buffered_end_ : 0;
    return static_cast<std::uint32_t>(std::min(std::uint64_t{buffer_bytes_} - live, to_limit));
}

std::uint32_t AtracStream::writableNow() const {
    const std::uint32_t free_bytes = vacant();
    if (free_bytes == 0) return 0;
    const std::uint64_t to_ring_end = buffer_bytes_ - buffered_end_ % buffer_bytes_;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(free_bytes, to_ring_end));
}

std::expected<AtracStreamInfo, std::uint32_t> AtracStream::streamDataInfo() {
    if (memory_mode_) return std::unexpected(kAtracErrorAllDataIsOnMemory);
    AtracStreamInfo stream;
    stream.write_addr = buffer_addr_ + static_cast<std::uint32_t>(buffered_end_ % buffer_bytes_);
    stream.writable = writableNow();
    stream.read_pos = static_cast<std::uint32_t>(fileOffsetOfStream(buffered_end_));
    return stream;
}

std::uint32_t AtracStream::addStreamData(std::uint32_t n) {
    if (memory_mode_) return kAtracErrorAllDataIsOnMemory;
    if (n > writableNow()) return kAtracErrorAddDataIsTooBig;
    buffered_end_ += n;
    return 0;
}

bool AtracStream::frameBuffered(std::uint32_t index) const {
    return index < frames_ && streamPos(pass_, index) + info_.block_align <= buffered_end_;
}

void AtracStream::gatherFrame(std::span<const std::uint8_t> ring, std::uint32_t index) {
    const std::size_t size = info_.block_align;
    frame_bytes_.resize(size);
    const std::size_t offset = static_cast<std::size_t>(streamPos(pass_, index) % buffer_bytes_);
    const std::size_t first = std::min(size, buffer_bytes_ - offset);
    std::memcpy(frame_bytes_.data(), ring.data() + offset, first);
    if (first < size) std::memcpy(frame_bytes_.data() + first, ring.data(), size - first);
}

void AtracStream::seekToLoopStart() {
    if (loop_num_ > 0) --loop_num_;
    cursor_ = restartFrame();
    emit_from_ = std::max(info_.loop->start_sample, info_.delay_samples);
    if (decoder_) decoder_->reset();
    // The buffered bytes stay: they are the stream's continuation, which starts at the new pass.
    ++pass_;
    trace_pending_ = true;
}

void AtracStream::noteFrame() {
    trace_pending_ = false;
    const std::uint64_t stream_pos = streamPos(pass_, cursor_);
    trace_note_ = std::format("frame {} pass {} file {:#x} stream {:#x} ring {:#x}: {}", cursor_, pass_,
                              frameOffset(cursor_), stream_pos, stream_pos % buffer_bytes_, headHex(frame_bytes_));
}

std::expected<AtracDecodeResult, std::uint32_t> AtracStream::decode(std::span<const std::uint8_t> ring,
                                                                    std::span<float> out) {
    if (!created_) return std::unexpected(kAtracErrorNoDecoder);
    const std::size_t channels = info_.channels;
    if (ring.size() < buffer_bytes_ || out.size() < std::size_t{kAtracMaxSample} * channels) {
        return std::unexpected(kAtracErrorIllegalData);
    }

    AtracDecodeResult result;
    if (finished_) {
        // A repeated call is not an error: the game ends a sound for good on any failure.
        result.finish = true;
        result.remain = remain();
        return result;
    }

    for (;;) {
        const bool loop_back = loopBackPending();
        const std::uint64_t end = passEnd();
        const std::uint64_t pos = std::uint64_t{cursor_} * kFrameSamples;
        if (pos >= end) {
            if (loop_back) {
                seekToLoopStart();
                continue;
            }
            finished_ = true;
            break;
        }
        if (!frameBuffered(cursor_)) break;

        if (decoder_ || trace_pending_) gatherFrame(ring, cursor_);
        if (trace_pending_) noteFrame();

        // Silence when there is no decoder or the frame fails; the bookkeeping is the same.
        bool have_samples = false;
        if (decoder_) {
            std::string error;
            if (decoder_->decode(frame_bytes_, frame_samples_, error)) {
                have_samples = true;
                ++result.counts.decoded;
                if (std::any_of(frame_samples_.begin(), frame_samples_.end(),
                                [](float v) { return std::fabs(v) > kAtracSilenceLevel; })) {
                    ++result.counts.non_silent;
                }
            } else {
                ++result.counts.errors;
                if (!error_seen_) {
                    error_seen_ = true;
                    const std::uint64_t stream_pos = streamPos(pass_, cursor_);
                    first_error_ = std::format(
                            "frame {} pass {} (file {:#x}, stream {:#x}, ring offset {:#x}, buffered to {:#x}, bytes {}): {}",
                            cursor_, pass_, frameOffset(cursor_), stream_pos, stream_pos % buffer_bytes_, buffered_end_,
                            headHex(frame_bytes_), error);
                }
            }
        } else {
            ++result.counts.without_decoder;
        }

        const std::uint64_t frame_end = pos + kFrameSamples;
        const std::uint64_t first = std::max(pos, emit_from_);
        const std::uint64_t last = std::min(frame_end, end);
        if (first < last) {
            const std::size_t count = static_cast<std::size_t>(last - first) * channels;
            const std::span<float> dest = out.first(count);
            if (have_samples) {
                std::copy_n(frame_samples_.begin() + static_cast<std::ptrdiff_t>((first - pos) * channels), count,
                            dest.begin());
            } else {
                std::fill(dest.begin(), dest.end(), 0.0f);
            }
            result.samples = static_cast<std::uint32_t>(last - first);
        }
        ++cursor_;

        if (frame_end >= end) {
            if (loop_back) {
                seekToLoopStart();
            } else {
                finished_ = true;
            }
        }
        if (result.samples > 0 || finished_) break;
    }
    result.finish = finished_;
    // The loop only leaves without samples and without finishing when the next frame is missing.
    result.starved = !finished_ && result.samples == 0;
    result.remain = remain();
    return result;
}

}  // namespace tox2

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
    stream.memory_mode_ = head.size() == buffer_bytes;
    std::uint64_t frames = info.frameCount();
    if (stream.memory_mode_) {
        // A buffer that is shorter than the declared data cannot be read past its end.
        frames = std::min<std::uint64_t>(frames, (head.size() - info.data_offset) / info.block_align);
    }
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

    stream.buffered_end_ = std::min<std::uint64_t>(head.size(), stream.file_end_);
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
    if (!loopBackPending()) return out_end_;
    return std::min<std::uint64_t>(std::uint64_t{info_.loop->end_sample} + 1, out_end_);
}

std::int32_t AtracStream::remain() const {
    if (memory_mode_) return kAtracRemainAllDataOnMemory;
    if (buffered_end_ >= file_end_ && !loopBackPending()) return kAtracRemainNonLoopStreamOnMemory;
    const std::uint64_t head = frameOffset(cursor_);
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

std::uint32_t AtracStream::writableNow() const {
    if (memory_mode_) return 0;
    const std::uint64_t head = frameOffset(cursor_);
    const std::uint64_t live = buffered_end_ > head ? buffered_end_ - head : 0;
    if (live >= buffer_bytes_) return 0;
    const std::uint64_t to_ring_end = buffer_bytes_ - buffered_end_ % buffer_bytes_;
    return static_cast<std::uint32_t>(
            std::min({std::uint64_t{buffer_bytes_} - live, to_ring_end, file_end_ - buffered_end_}));
}

std::expected<AtracStreamInfo, std::uint32_t> AtracStream::streamDataInfo() {
    if (memory_mode_) return std::unexpected(kAtracErrorAllDataIsOnMemory);
    info_epoch_ = stream_epoch_;
    AtracStreamInfo stream;
    stream.write_addr = buffer_addr_ + static_cast<std::uint32_t>(buffered_end_ % buffer_bytes_);
    stream.writable = writableNow();
    stream.read_pos = static_cast<std::uint32_t>(buffered_end_);
    return stream;
}

std::uint32_t AtracStream::addStreamData(std::uint32_t n) {
    if (memory_mode_) return kAtracErrorAllDataIsOnMemory;
    if (info_epoch_ != stream_epoch_) return 0;
    if (n > writableNow()) return kAtracErrorAddDataIsTooBig;
    buffered_end_ += n;
    return 0;
}

bool AtracStream::frameBuffered(std::uint32_t index) const {
    return index < frames_ && frameOffset(index) + info_.block_align <= buffered_end_;
}

void AtracStream::gatherFrame(std::span<const std::uint8_t> ring, std::uint32_t index) {
    const std::size_t size = info_.block_align;
    frame_bytes_.resize(size);
    const std::size_t offset = static_cast<std::size_t>(frameOffset(index) % buffer_bytes_);
    const std::size_t first = std::min(size, buffer_bytes_ - offset);
    std::memcpy(frame_bytes_.data(), ring.data() + offset, first);
    if (first < size) std::memcpy(frame_bytes_.data() + first, ring.data(), size - first);
}

void AtracStream::seekToLoopStart() {
    if (loop_num_ > 0) --loop_num_;
    const std::uint32_t start = info_.loop->start_sample;
    // One frame before the loop start primes the decoder's overlap; its output is discarded.
    const std::uint32_t start_frame = start / kFrameSamples;
    cursor_ = std::min(start_frame > 0 ? start_frame - 1 : 0, frames_);
    emit_from_ = std::max(start, info_.delay_samples);
    if (decoder_) decoder_->reset();
    if (!memory_mode_) {
        buffered_end_ = frameOffset(cursor_);
        ++stream_epoch_;
    }
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

        // Silence when there is no decoder or the frame fails; the bookkeeping is the same.
        bool have_samples = false;
        if (decoder_) {
            gatherFrame(ring, cursor_);
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
                    first_error_ = std::format("frame {}: {}", cursor_, error);
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
    result.remain = remain();
    return result;
}

}  // namespace tox2

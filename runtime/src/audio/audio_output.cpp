#include "audio/audio_output.h"

#include <algorithm>
#include <cstring>

namespace dal {

namespace {

class NullAudioOutput final : public AudioOutput {
public:
    std::string_view name() const override { return "none"; }
    bool open(std::string&) override { return true; }

    void submit(std::span<const float> block) override {
        blocks_submitted_.fetch_add(1, std::memory_order_relaxed);
        frames_played_.fetch_add(block.size() / kAudioOutputChannels, std::memory_order_relaxed);
    }

    std::string description() const override { return "no device"; }

    AudioOutputStats stats() const override {
        AudioOutputStats stats;
        stats.blocks_submitted = blocks_submitted_.load(std::memory_order_relaxed);
        stats.frames_played = frames_played_.load(std::memory_order_relaxed);
        return stats;
    }

private:
    std::atomic<std::uint64_t> blocks_submitted_{0};
    std::atomic<std::uint64_t> frames_played_{0};
};

}  // namespace

std::unique_ptr<AudioOutput> makeNullAudioOutput() { return std::make_unique<NullAudioOutput>(); }

AudioBlockRing::AudioBlockRing(std::size_t capacity_blocks)
    : storage_(std::max<std::size_t>(capacity_blocks, 1) * kAudioBlockSamples),
      capacity_frames_(storage_.size() / kAudioOutputChannels) {}

bool AudioBlockRing::push(std::span<const float> block) {
    if (block.size() != kAudioBlockSamples) return false;
    const std::uint64_t write = write_frames_.load(std::memory_order_relaxed);
    const std::uint64_t read = read_frames_.load(std::memory_order_acquire);
    if (write - read + kAudioBlockFrames > capacity_frames_) return false;
    std::memcpy(storage_.data() + (write % capacity_frames_) * kAudioOutputChannels, block.data(),
                kAudioBlockSamples * sizeof(float));
    write_frames_.store(write + kAudioBlockFrames, std::memory_order_release);
    return true;
}

std::size_t AudioBlockRing::pop(std::span<float> out, std::size_t frames) {
    const std::uint64_t write = write_frames_.load(std::memory_order_acquire);
    std::uint64_t read = read_frames_.load(std::memory_order_relaxed);

    const std::uint32_t drops = pending_drops_.exchange(0, std::memory_order_acq_rel);
    if (drops != 0) {
        read += std::min<std::uint64_t>(std::uint64_t{drops} * kAudioBlockFrames, write - read);
    }

    const std::size_t wanted = std::min(frames, out.size() / kAudioOutputChannels);
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(wanted, write - read));
    if (count != 0) {
        const std::size_t start = static_cast<std::size_t>(read % capacity_frames_);
        const std::size_t first = std::min<std::size_t>(count, capacity_frames_ - start);
        std::memcpy(out.data(), storage_.data() + start * kAudioOutputChannels,
                    first * kAudioOutputChannels * sizeof(float));
        std::memcpy(out.data() + first * kAudioOutputChannels, storage_.data(),
                    (count - first) * kAudioOutputChannels * sizeof(float));
    }
    if (count != 0 || drops != 0) read_frames_.store(read + count, std::memory_order_release);
    return count;
}

std::size_t AudioBlockRing::queuedFrames() const {
    // Read side first: the write counter is monotonic, so the later load cannot be behind it.
    const std::uint64_t read = read_frames_.load(std::memory_order_acquire);
    const std::uint64_t drops = pending_drops_.load(std::memory_order_acquire);
    const std::uint64_t write = write_frames_.load(std::memory_order_acquire);
    const std::uint64_t queued = write - read;
    const std::uint64_t dropped = drops * kAudioBlockFrames;
    return static_cast<std::size_t>(queued > dropped ? queued - dropped : 0);
}

void AudioBlockRing::dropOldestBlock() { pending_drops_.fetch_add(1, std::memory_order_acq_rel); }

}  // namespace dal

// Host audio output: the block format shared by the cellAudio producer and every sink, the sink
// interface (null here, WASAPI later) and the ring that decouples a device thread from the producer.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dal {

inline constexpr std::uint32_t kAudioSampleRate = 48000;
inline constexpr std::uint32_t kAudioBlockFrames = 256;   // one cellAudio period, 5.333 ms
inline constexpr std::uint32_t kAudioOutputChannels = 2;  // interleaved L, R
inline constexpr std::size_t kAudioBlockSamples = kAudioBlockFrames * kAudioOutputChannels;
inline constexpr std::chrono::nanoseconds kAudioBlockPeriod{5'333'333};  // 256 / 48000 s

// Host-order float, nominally [-1, 1]. Samples beyond it are clamped by the producer.
using AudioBlock = std::array<float, kAudioBlockSamples>;

struct AudioOutputStats {
    std::uint64_t blocks_submitted = 0;
    std::uint64_t blocks_dropped = 0;   // ring above its high-water mark: device slower than the clock
    std::uint64_t underrun_frames = 0;  // device wanted frames the ring did not hold (silence played)
    std::uint64_t frames_played = 0;    // frames handed to the device (null output: frames discarded)
    std::uint64_t device_errors = 0;
};

// A host audio sink. submit() is called by the engine thread only and never blocks; a device output
// owns its own thread that pulls from an AudioBlockRing.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual std::string_view name() const = 0;  // "none", "wasapi"
    // Called once on the engine thread before the first submit. False with the reason in `error`.
    virtual bool open(std::string& error) = 0;
    virtual void submit(std::span<const float> block) = 0;  // kAudioBlockSamples values
    virtual std::string description() const = 0;            // for the start line (device, mix format, buffer)
    virtual AudioOutputStats stats() const = 0;             // any thread
    // Frames submitted and not yet played: the output's queue plus what the device holds. The engine
    // steers its clock by it (see BlockRateController). Any thread. nullopt when the output has no
    // device clock to follow, or while it is not playing (priming, device outage).
    virtual std::optional<std::uint64_t> fillFrames() const { return std::nullopt; }
};

// Discards every block (counts them). The engine's clock still runs.
std::unique_ptr<AudioOutput> makeNullAudioOutput();

// Lock-free single-producer single-consumer ring of whole blocks (capacity fixed at construction).
// The consumer may read part of a block and continue with the rest on its next read.
class AudioBlockRing {
public:
    // A capacity of 0 is raised to one block.
    explicit AudioBlockRing(std::size_t capacity_blocks);

    AudioBlockRing(const AudioBlockRing&) = delete;
    AudioBlockRing& operator=(const AudioBlockRing&) = delete;

    // Producer side. False when the ring cannot hold another whole block (or `block` has the wrong
    // size); nothing is stored then.
    bool push(std::span<const float> block);
    // Consumer side. Copies up to `frames` frames into `out` (interleaved stereo, at most
    // out.size() / kAudioOutputChannels); returns frames copied.
    std::size_t pop(std::span<float> out, std::size_t frames);
    // Frames pushed and not yet popped or dropped. Any thread; transiently approximate while the
    // other side runs.
    std::size_t queuedFrames() const;
    // Producer side. Discards the oldest kAudioBlockFrames queued frames. Only the consumer moves the
    // read position, so the drop is applied on its next pop(); queuedFrames() already excludes it.
    // The producer must therefore keep its high-water mark below the capacity, or push() can still
    // find the ring full right after a drop.
    void dropOldestBlock();

private:
    static constexpr std::size_t kCacheLine = 64;

    std::vector<float> storage_;
    std::uint64_t capacity_frames_;
    // Monotonic frame counters; the position in storage_ is the counter modulo the capacity. The
    // capacity is a multiple of the block size and only whole blocks are pushed, so a block never
    // wraps. Separate cache lines keep the two threads from sharing one.
    alignas(kCacheLine) std::atomic<std::uint64_t> write_frames_{0};  // producer-owned
    alignas(kCacheLine) std::atomic<std::uint64_t> read_frames_{0};   // consumer-owned
    alignas(kCacheLine) std::atomic<std::uint32_t> pending_drops_{0};
};

}  // namespace dal

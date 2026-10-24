#include "audio/stream_output.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "core/log.h"
#include "core/sleeper.h"

namespace tox2 {

namespace {

using Clock = std::chrono::steady_clock;

// Between passes of the device thread. Passes normally follow the device's buffer event; the slice
// bounds how long a missed event, a stop request or a ring that needs its drops applied can wait.
constexpr std::chrono::milliseconds kWaitSlice{20};
constexpr std::chrono::milliseconds kOutagePoll{5};
constexpr std::chrono::milliseconds kJoinTimeout{1000};

constexpr RenderStatus kStatusOk = 0;

// State shared by the output (engine thread) and its device thread.
struct Shared {
    Shared(RenderStreamFactory stream_factory, StreamOutputOptions output_options)
        : ring(kRingBlocks), factory(std::move(stream_factory)), options(std::move(output_options)) {}

    AudioBlockRing ring;
    const RenderStreamFactory factory;
    const StreamOutputOptions options;
    std::atomic<bool> stop{false};

    std::atomic<std::uint64_t> blocks_submitted{0};
    std::atomic<std::uint64_t> blocks_dropped{0};
    std::atomic<std::uint64_t> underrun_frames{0};
    std::atomic<std::uint64_t> frames_played{0};
    std::atomic<std::uint64_t> device_errors{0};

    // Frames the device holds as of the device thread's last pass, and whether a stream is playing.
    // The engine thread reads them (the stream stays on the device thread).
    std::atomic<std::uint32_t> device_padding{0};
    std::atomic<std::uint32_t> device_period_frames{0};  // written before `streaming` is set
    std::atomic<bool> streaming{false};

    std::mutex mutex;  // guards the members below
    std::condition_variable changed;
    bool open_done = false;
    bool open_ok = false;
    bool thread_done = false;
    std::string error;
    std::string description;
};

class DevicePump {
public:
    explicit DevicePump(Shared& shared) : shared_(shared) {}

    void run() {
        std::string error;
        std::unique_ptr<RenderStream> stream = shared_.factory ? shared_.factory(error) : nullptr;
        if (!stream) {
            publishOpen(false, error.empty() ? "cannot open the render device" : error, {});
            return;
        }
        publishOpen(true, {}, stream->description());

        bool wait_before_attempt = true;
        for (;;) {
            if (stream) {
                const RenderStatus status = serve(*stream);
                if (stopping()) return;
                shared_.device_errors.fetch_add(1, std::memory_order_relaxed);
                logWarn("tox2: audio: {}: device lost ({}); retrying", shared_.options.name, statusText(status));
                stream.reset();
                // The first attempt follows at once: after a switch of the default device the new one is
                // usually ready already.
                wait_before_attempt = false;
            }

            if (wait_before_attempt) {
                waitOutage(Clock::now() + shared_.options.reopen_interval);
                if (stopping()) return;
            }
            wait_before_attempt = true;
            std::string reopen_error;
            stream = shared_.factory(reopen_error);
            if (stream) {
                const std::string description = stream->description();
                {
                    std::lock_guard<std::mutex> lock(shared_.mutex);
                    shared_.description = description;
                }
                logInfo("tox2: audio: {}: reopened {}", shared_.options.name, description);
            } else {
                logDebug("tox2: audio: {}: reopen failed: {}", shared_.options.name, reopen_error);
            }
        }
    }

private:
    bool stopping() const { return shared_.stop.load(std::memory_order_relaxed); }

    std::string statusText(RenderStatus status) const {
        return shared_.options.status_text ? shared_.options.status_text(status) : std::format("{}", status);
    }

    void publishOpen(bool ok, std::string error, std::string description) {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        shared_.open_done = true;
        shared_.open_ok = ok;
        shared_.error = std::move(error);
        shared_.description = std::move(description);
        shared_.changed.notify_all();
    }

    // Plays until `stream` fails (returns the failing status) or a stop is requested (returns kStatusOk).
    //
    // Only frames the ring holds are written, never padding zeros: the device buffer carries the
    // latency (the primed blocks), and the stream stays contiguous. The device plays silence by itself
    // when its buffer runs dry; that is the only case counted as an underrun. The device buffer is
    // kept near the primed amount: the ring holds only what the engine produced since the last pass.
    RenderStatus serve(RenderStream& stream) {
        // The fill level means nothing before the stream starts and after it is lost.
        struct StreamingFlag {
            Shared& shared;
            ~StreamingFlag() { shared.streaming.store(false, std::memory_order_release); }
        } streaming_flag{shared_};

        const std::uint32_t buffer_frames = stream.bufferFrames();
        const std::uint32_t period_frames = stream.periodFrames();
        scratch_.resize(std::max<std::size_t>(std::size_t{buffer_frames} * kAudioOutputChannels, kDiscardChunkSamples));

        constexpr std::size_t kPrimeFrames = kPrimeBlocks * kAudioBlockFrames;
        while (shared_.ring.queuedFrames() < kPrimeFrames) {
            if (stopping()) return kStatusOk;
            shared_.ring.pop({}, 0);
            sleeper_.sleep(std::chrono::duration_cast<std::chrono::microseconds>(kOutagePoll));
        }

        // The audio engine reads a whole period the moment the stream starts and then once per period.
        // Without a head start the first reads leave the buffer at about one period of audio, and the
        // block-sized arrivals then bring it to empty now and then; one period of silence in front of the
        // primed blocks keeps a margin that costs that much extra latency.
        // Never so much that the primed blocks no longer fit in a small device buffer.
        const std::uint32_t preroll =
                std::min<std::uint32_t>(period_frames, buffer_frames > kPrimeFrames ? buffer_frames - kPrimeFrames : 0);
        if (preroll != 0) {
            std::fill_n(scratch_.begin(), std::size_t{preroll} * kAudioOutputChannels, 0.0f);
            const RenderStatus preroll_status =
                    stream.write(std::span<const float>(scratch_.data(), std::size_t{preroll} * kAudioOutputChannels));
            if (preroll_status < 0) return preroll_status;
        }

        std::uint32_t padding_after = 0;
        RenderStatus status = fill(stream, buffer_frames, preroll, padding_after);
        if (status < 0) return status;
        status = stream.start();
        if (status < 0) return status;
        Clock::time_point last_pass = Clock::now();
        shared_.device_padding.store(padding_after, std::memory_order_relaxed);
        shared_.device_period_frames.store(period_frames, std::memory_order_relaxed);
        shared_.streaming.store(true, std::memory_order_release);

        while (!stopping()) {
            stream.waitForBuffer(kWaitSlice);
            if (stopping()) break;
            shared_.ring.pop({}, 0);  // applies the drops the producer requested

            std::uint32_t pending = 0;
            status = stream.pendingFrames(pending);
            if (status < 0) return status;
            const Clock::time_point now = Clock::now();
            if (pending == 0) {
                // The buffer ran empty: what the device wanted beyond what we had written was silence. The
                // audio engine takes whole periods, so the wanted amount is the elapsed time rounded to
                // periods; this keeps timer jitter (a few frames) from showing up as an underrun.
                const double elapsed_frames = std::chrono::duration<double>(now - last_pass).count() * kAudioSampleRate;
                const double wanted = period_frames != 0 ? std::round(elapsed_frames / period_frames) *
                                                                   static_cast<double>(period_frames)
                                                         : elapsed_frames;
                const double dry = wanted - static_cast<double>(padding_after);
                if (dry >= 1.0)
                    shared_.underrun_frames.fetch_add(static_cast<std::uint64_t>(dry), std::memory_order_relaxed);
            }
            status = fill(stream, buffer_frames, pending, padding_after);
            if (status < 0) return status;
            shared_.device_padding.store(padding_after, std::memory_order_relaxed);
            last_pass = now;
        }
        return kStatusOk;
    }

    // Writes what the ring holds into the free part of the device buffer. `padding_after` receives the
    // frames the device holds afterwards.
    RenderStatus fill(RenderStream& stream, std::uint32_t buffer_frames, std::uint32_t pending,
                      std::uint32_t& padding_after) {
        padding_after = pending;
        const std::size_t free_frames = pending >= buffer_frames ? 0 : buffer_frames - pending;
        const std::size_t wanted = std::min(free_frames, shared_.ring.queuedFrames());
        if (wanted == 0) return kStatusOk;
        const std::size_t got = shared_.ring.pop(scratch_, wanted);
        if (got == 0) return kStatusOk;
        const RenderStatus status = stream.write(std::span<const float>(scratch_.data(), got * kAudioOutputChannels));
        if (status < 0) return status;
        shared_.frames_played.fetch_add(got, std::memory_order_relaxed);
        padding_after = pending + static_cast<std::uint32_t>(got);
        return kStatusOk;
    }

    // No device: empties the ring at the nominal rate so the engine sees no difference, until `until`.
    void waitOutage(Clock::time_point until) {
        scratch_.resize(std::max(scratch_.size(), kDiscardChunkSamples));
        Clock::time_point last = Clock::now();
        double credit = 0.0;
        while (!stopping()) {
            const Clock::time_point now = Clock::now();
            if (now >= until) return;
            credit += std::chrono::duration<double>(now - last).count() * kAudioSampleRate;
            last = now;
            while (credit >= 1.0) {
                const std::size_t chunk = std::min<std::size_t>(static_cast<std::size_t>(credit),
                                                                kDiscardChunkSamples / kAudioOutputChannels);
                const std::size_t got = shared_.ring.pop(scratch_, chunk);
                if (got < chunk) {
                    credit = 0.0;  // the ring is empty; do not bank time for later blocks
                    break;
                }
                credit -= static_cast<double>(got);
            }
            shared_.ring.pop({}, 0);
            sleeper_.sleep(std::chrono::duration_cast<std::chrono::microseconds>(kOutagePoll));
        }
    }

    static constexpr std::size_t kDiscardChunkSamples = 4096 * kAudioOutputChannels;

    Shared& shared_;
    Sleeper sleeper_;
    std::vector<float> scratch_;
};

void deviceThread(std::shared_ptr<Shared> shared) {
    try {
        const std::shared_ptr<void> thread_scope =
                shared->options.enter_device_thread ? shared->options.enter_device_thread() : nullptr;
        DevicePump(*shared).run();
    } catch (const std::exception& error) {
        logWarn("tox2: audio: {}: device thread failed: {}", shared->options.name, error.what());
    } catch (...) {
        logWarn("tox2: audio: {}: device thread failed", shared->options.name);
    }
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (!shared->open_done) {
        shared->open_done = true;
        shared->open_ok = false;
        shared->error = "the device thread failed";
    }
    shared->thread_done = true;
    shared->changed.notify_all();
}

class StreamOutput final : public AudioOutput {
public:
    StreamOutput(RenderStreamFactory factory, StreamOutputOptions options)
        : shared_(std::make_shared<Shared>(std::move(factory), std::move(options))) {}

    ~StreamOutput() override {
        shared_->stop.store(true, std::memory_order_relaxed);
        if (!thread_.joinable()) return;
        bool done = false;
        if (!abandoned_) {
            std::unique_lock<std::mutex> lock(shared_->mutex);
            done = shared_->changed.wait_for(lock, kJoinTimeout, [this] { return shared_->thread_done; });
        }
        // A thread stuck in a driver call is left to finish on its own; it holds the shared state alive.
        if (done) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }

    std::string_view name() const override { return shared_->options.name; }

    bool open(std::string& error) override {
        if (thread_.joinable()) return true;
        try {
            thread_ = std::thread(deviceThread, shared_);
        } catch (const std::system_error& failed) {
            error = std::format("cannot start the device thread: {}", failed.what());
            return false;
        }
        std::unique_lock<std::mutex> lock(shared_->mutex);
        if (!shared_->changed.wait_for(lock, shared_->options.open_timeout, [this] { return shared_->open_done; })) {
            error = "the device did not open in time";
            abandoned_ = true;
            shared_->stop.store(true, std::memory_order_relaxed);
            return false;
        }
        if (!shared_->open_ok) error = shared_->error;
        return shared_->open_ok;
    }

    void submit(std::span<const float> block) override {
        Shared& shared = *shared_;
        shared.blocks_submitted.fetch_add(1, std::memory_order_relaxed);
        if (shared.ring.queuedFrames() >= kHighWaterBlocks * kAudioBlockFrames) {
            shared.ring.dropOldestBlock();
            shared.blocks_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        if (!shared.ring.push(block)) shared.blocks_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    std::string description() const override {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->description;
    }

    // The padding is published right after the device thread's write, when it is at its highest; the
    // device then drains it in steps of one period until the next pass, so on average it is half a
    // period lower. Subtracting that gives the mean fill. The ring and the padding are read
    // separately, so a read that lands while the device thread moves frames from one to the other
    // is off by that amount; it is rare and averages out.
    std::optional<std::uint64_t> fillFrames() const override {
        const Shared& shared = *shared_;
        if (!shared.streaming.load(std::memory_order_acquire)) return std::nullopt;
        const std::uint64_t held = shared.ring.queuedFrames() + shared.device_padding.load(std::memory_order_relaxed);
        const std::uint64_t half_period = shared.device_period_frames.load(std::memory_order_relaxed) / 2;
        return held > half_period ? held - half_period : 0;
    }

    AudioOutputStats stats() const override {
        const Shared& shared = *shared_;
        AudioOutputStats stats;
        stats.blocks_submitted = shared.blocks_submitted.load(std::memory_order_relaxed);
        stats.blocks_dropped = shared.blocks_dropped.load(std::memory_order_relaxed);
        stats.underrun_frames = shared.underrun_frames.load(std::memory_order_relaxed);
        stats.frames_played = shared.frames_played.load(std::memory_order_relaxed);
        stats.device_errors = shared.device_errors.load(std::memory_order_relaxed);
        return stats;
    }

private:
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    bool abandoned_ = false;  // open() gave up on the thread; engine thread only
};

}  // namespace

std::unique_ptr<AudioOutput> makeStreamOutput(RenderStreamFactory factory, StreamOutputOptions options) {
    return std::make_unique<StreamOutput>(std::move(factory), std::move(options));
}

}  // namespace tox2

#include "audio/audio_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/wav_file.h"
#include "core/log.h"
#include "core/sleeper.h"

namespace tox2 {

void BlockClock::setCorrectionPpm(double ppm) {
    ppm = std::clamp(ppm, -kClockMaxPpm, kClockMaxPpm);
    period_ = ppm == 0.0 ? Clock::duration(kAudioBlockPeriod)
                         : Clock::duration(
                                   std::llround(static_cast<double>(kAudioBlockPeriod.count()) / (1.0 + ppm * 1e-6)));
}

BlockClockPlan BlockClock::due(Clock::time_point now) {
    if (now < next_due_) return {};
    const std::uint64_t periods = 1 + static_cast<std::uint64_t>((now - next_due_) / period_);
    const std::uint64_t produce = std::min<std::uint64_t>(periods, 1 + std::uint64_t{kMaxCatchUpBlocks});
    next_due_ += period_ * static_cast<std::int64_t>(periods);

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    return {static_cast<std::uint32_t>(produce), static_cast<std::uint32_t>(std::min(periods - produce, kMax))};
}

bool BlockRateController::observe(Clock::time_point now, std::optional<std::uint64_t> fill_frames) {
    if (!fill_frames) {
        window_open_ = false;
        return false;
    }
    if (!window_open_) {
        window_open_ = true;
        window_start_ = now;
        window_sum_ = 0.0;
        window_count_ = 0;
    }
    window_sum_ += static_cast<double>(*fill_frames);
    ++window_count_;
    if (now - window_start_ < kClockUpdateInterval) return false;

    // The mean over the interval averages out the device's block-sized steps in the fill, so the
    // controller sees the trend and not the sawtooth.
    const double seconds = std::chrono::duration<double>(now - window_start_).count();
    last_mean_fill_ = window_sum_ / static_cast<double>(window_count_);
    const double error = last_mean_fill_ - static_cast<double>(kFillTargetFrames);

    // A fill above the target means the engine is too fast: the correction is the negated sum.
    const double unclamped = kClockKpPpmPerFrame * error + integral_ppm_;
    // Anti-windup: no more integration while the output is at its bound and the error pushes further.
    if (!(std::fabs(unclamped) >= kClockMaxPpm && unclamped * error > 0.0)) {
        integral_ppm_ =
                std::clamp(integral_ppm_ + kClockKiPpmPerFrameSecond * error * seconds, -kClockMaxPpm, kClockMaxPpm);
    }
    ppm_ = -std::clamp(kClockKpPpmPerFrame * error + integral_ppm_, -kClockMaxPpm, kClockMaxPpm);

    window_open_ = false;
    return true;
}

namespace {

using Clock = BlockClock::Clock;

bool isNonSilent(std::span<const float> block) {
    return std::any_of(block.begin(), block.end(), [](float s) { return std::fabs(s) > kWavSilenceThreshold; });
}

constexpr AudioBlock kSilentBlock{};

// One debug line per this many correction updates (seconds).
constexpr unsigned kClockLogEvery = 10;

class Engine : public std::enable_shared_from_this<Engine> {
public:
    Engine(AudioEngineConfig config, AudioBlockProducer producer)
        : config_(std::move(config)), producer_(std::move(producer)) {}

    // Starts the thread, or cancels a pending stop when it is still running.
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        if (running_) return;
        running_ = true;
        try {
            // Detached: the process ends with std::_Exit and nothing joins. The shared_ptr keeps the
            // engine alive until the thread is done with it.
            std::thread([self = shared_from_this()] { self->run(); }).detach();
        } catch (const std::system_error& error) {
            running_ = false;
            logWarn("tox2: audio: cannot start the engine thread: {}; continuing without sound", error.what());
        }
    }

    void requestStop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }

    AudioEngineStats stats() const {
        AudioEngineStats stats;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats.output = output_label_;
            if (output_) stats.output_stats = output_->stats();
        }
        stats.blocks = blocks_.load(std::memory_order_relaxed);
        stats.non_silent_blocks = non_silent_blocks_.load(std::memory_order_relaxed);
        stats.skipped_blocks = skipped_blocks_.load(std::memory_order_relaxed);
        stats.max_producer_time = std::chrono::microseconds(max_producer_us_.load(std::memory_order_relaxed));
        stats.clock_ppm = clock_ppm_.load(std::memory_order_relaxed);
        return stats;
    }

    void finishDump() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Remembered even without a dump yet: the engine thread may still be opening its output, and
        // the dump it creates afterwards must not be left unfinished.
        dump_finish_requested_ = true;
        dump_finished_.store(true, std::memory_order_relaxed);
        if (dump_) dump_->finish();
    }

    bool waitIdle(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return idle_.wait_for(lock, timeout, [this] { return !running_; });
    }

private:
    // First run only: the output and the dump belong to the engine thread from here on (WASAPI needs
    // its COM objects created on the thread that uses them). mutex_ is held only for the stores, never
    // while a device is being opened.
    void openOutputAndDump() {
        std::unique_ptr<AudioOutput> output = config_.make_output ? config_.make_output() : nullptr;
        std::string failed_name;
        std::string error;
        if (output && !output->open(error)) {
            failed_name = std::string(output->name());
            logWarn("tox2: audio: {}: {}; continuing without sound", failed_name,
                    error.empty() ? "cannot open the output" : error);
            output.reset();
        }
        if (!output) output = makeNullAudioOutput();
        std::string label(output->name());
        if (!failed_name.empty()) label += " (" + failed_name + " failed)";
        const std::string description = output->description();
        logInfo("tox2: audio: output {}{}", output->name(), description.empty() ? "" : " (" + description + ")");

        std::shared_ptr<WavWriter> dump;
        bool finish_requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finish_requested = dump_finish_requested_;
        }
        if (!config_.dump.empty() && !finish_requested) {
            auto created = WavWriter::create(config_.dump, kAudioSampleRate, kAudioOutputChannels);
            if (created) {
                dump = std::move(*created);
                logInfo("tox2: audio: dump {}", config_.dump.filename().string());
            } else {
                logWarn("tox2: audio: dump {}: {}; continuing without a dump", config_.dump.filename().string(),
                        created.error());
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        output_ = std::move(output);
        output_label_ = std::move(label);
        // A finish requested while the file was being created leaves a valid, empty, closed file.
        if (dump && dump_finish_requested_) dump->finish();
        dump_ = std::move(dump);
    }

    // The stop request and the end of the thread are decided under one lock, so a start() that arrives
    // in between either cancels the stop or finds the thread gone and starts a new one.
    bool retireIfStopRequested() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_requested_) return false;
        running_ = false;
        idle_.notify_all();
        return true;
    }

    void emit(std::span<const float> block) {
        if (dump_ && !dump_->write(block) && !dump_finished_.load(std::memory_order_relaxed) && !dump_warned_) {
            dump_warned_ = true;
            logWarn("tox2: audio: writing the dump failed; the dump stops here");
        }
        output_->submit(block);
    }

    void run() {
        if (!output_) openOutputAndDump();

        Sleeper sleeper;
        // Started after the output is open, so a slow device open does not show up as skipped periods.
        BlockClock clock(Clock::now());
        // A resumed engine keeps the correction it had learned (same device, same drift); the interval
        // under way when it stopped is not continued.
        rate_.observe(Clock::now(), std::nullopt);
        clock.setCorrectionPpm(rate_.ppm());
        unsigned updates = 0;
        AudioBlock block;
        for (;;) {
            sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(clock.nextDue() - Clock::now()));
            if (retireIfStopRequested()) return;

            const Clock::time_point now = Clock::now();
            const BlockClockPlan plan = clock.due(now);
            if (plan.produce + plan.skipped != 0 && rate_.observe(now, output_->fillFrames())) {
                clock.setCorrectionPpm(rate_.ppm());
                clock_ppm_.store(rate_.ppm(), std::memory_order_relaxed);
                if (++updates % kClockLogEvery == 0) {
                    const AudioOutputStats output_stats = output_->stats();
                    logDebug(
                            "tox2: audio: clock {:+.1f} ppm, mean fill {:.0f} frames (target {}), dropped {}, underrun {} frames",
                            rate_.ppm(), rate_.lastMeanFill(), kFillTargetFrames, output_stats.blocks_dropped,
                            output_stats.underrun_frames);
                }
            }
            // Silence for the periods that were missed comes first; the produced block is the newest.
            for (std::uint32_t i = 0; i < plan.skipped; ++i) {
                emit(kSilentBlock);
                skipped_blocks_.fetch_add(1, std::memory_order_relaxed);
            }
            for (std::uint32_t i = 0; i < plan.produce; ++i) {
                block.fill(0.0f);
                const Clock::time_point before = Clock::now();
                if (producer_) producer_(block);
                const auto elapsed =
                        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count();
                // Only this thread writes it, so a plain load and store cannot lose an update.
                if (elapsed > max_producer_us_.load(std::memory_order_relaxed)) {
                    max_producer_us_.store(elapsed, std::memory_order_relaxed);
                }

                blocks_.fetch_add(1, std::memory_order_relaxed);
                if (isNonSilent(block)) non_silent_blocks_.fetch_add(1, std::memory_order_relaxed);
                emit(block);
            }
        }
    }

    const AudioEngineConfig config_;
    const AudioBlockProducer producer_;

    mutable std::mutex mutex_;  // guards the members below it down to idle_
    std::condition_variable idle_;
    bool running_ = false;
    bool stop_requested_ = false;
    // Set once by the engine thread under mutex_; the engine thread then reads them without the lock,
    // other threads under it.
    std::unique_ptr<AudioOutput> output_;
    std::string output_label_;
    std::shared_ptr<WavWriter> dump_;

    bool dump_finish_requested_ = false;  // guarded by mutex_
    std::atomic<bool> dump_finished_{false};
    bool dump_warned_ = false;  // engine thread only
    BlockRateController rate_;  // engine thread only

    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> non_silent_blocks_{0};
    std::atomic<std::uint64_t> skipped_blocks_{0};
    std::atomic<std::int64_t> max_producer_us_{0};
    std::atomic<double> clock_ppm_{0.0};
};

std::mutex g_mutex;
AudioEngineConfig g_config;
std::shared_ptr<Engine> g_engine;

std::shared_ptr<Engine> currentEngine() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_engine;
}

}  // namespace

void configureAudioEngine(AudioEngineConfig config) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = std::move(config);
}

void startAudioEngine(AudioBlockProducer producer) {
    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_engine) g_engine = std::make_shared<Engine>(g_config, std::move(producer));
        engine = g_engine;
    }
    engine->start();
}

void requestAudioEngineStop() {
    if (const auto engine = currentEngine()) engine->requestStop();
}

AudioEngineStats audioEngineStats() {
    const auto engine = currentEngine();
    return engine ? engine->stats() : AudioEngineStats{};
}

void finishAudioDump() {
    if (const auto engine = currentEngine()) engine->finishDump();
}

bool stopAudioEngineForTest(AudioEngineStats* final_stats) {
    const auto engine = currentEngine();
    bool stopped = true;
    if (engine) {
        engine->requestStop();
        stopped = engine->waitIdle(std::chrono::seconds(10));
        engine->finishDump();
        if (final_stats != nullptr) *final_stats = engine->stats();
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_engine.reset();
    g_config = {};
    return stopped;
}

}  // namespace tox2

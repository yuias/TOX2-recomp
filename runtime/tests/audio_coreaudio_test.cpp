// Tests for the CoreAudio output. The device thread logic (priming, drop accounting, device loss and
// reopen) runs over fake render streams and needs no sound device. The real-device test opens the
// default output device; without one it is skipped and the program exits 77 (CTest "skipped") when
// everything else passed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/audio_output.h"
#include "audio/coreaudio_output.h"
#include "audio/stream_output.h"
#include "core/log.h"
#include "core/sleeper.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

constexpr int kExitSkip = 77;
constexpr tox2::RenderStatus kDeviceLost = -0x21646576;  // -kAudioHardwareBadDeviceError ('!dev')

// Collects the log lines of every thread while it lives.
class LogCapture {
public:
    LogCapture() {
        tox2::setLogSink([this](std::string_view line) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(line);
        });
    }
    ~LogCapture() { tox2::setLogSink({}); }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    int count(std::string_view text) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(),
                                              [&](const std::string& line) { return line.contains(text); }));
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const Clock::time_point give_up = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() >= give_up) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Submits `blocks` blocks on the absolute schedule start + i * spacing. `fill` writes block `first + i`.
// A schedule that fell more than kMaxLagBlocks behind (the machine was starved) restarts from now,
// like the engine, which skips missed periods instead of submitting them in a burst.
void submitPaced(tox2::AudioOutput& output, std::uint64_t first, std::uint64_t blocks, std::chrono::microseconds spacing,
                 const std::function<void(std::uint64_t block, std::span<float> samples)>& fill) {
    constexpr std::int64_t kMaxLagBlocks = 4;
    tox2::Sleeper sleeper;
    tox2::AudioBlock samples;
    Clock::time_point start = Clock::now();
    for (std::uint64_t i = 0; i < blocks; ++i) {
        const Clock::time_point due = start + spacing * static_cast<std::int64_t>(i);
        const Clock::time_point now = Clock::now();
        if (now - due > spacing * kMaxLagBlocks) start = now - spacing * static_cast<std::int64_t>(i);
        sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(
                start + spacing * static_cast<std::int64_t>(i) - Clock::now()));
        fill(first + i, samples);
        output.submit(samples);
    }
}

// Frame n of the ramp has left n + 1 and right -(n + 1): never zero, so inserted silence, lost and
// repeated frames all show.
void fillRamp(std::uint64_t block, std::span<float> samples) {
    for (std::size_t i = 0; i < tox2::kAudioBlockFrames; ++i) {
        const float value = static_cast<float>(block * tox2::kAudioBlockFrames + i + 1);
        samples[2 * i] = value;
        samples[2 * i + 1] = -value;
    }
}

void fillSilence(std::uint64_t, std::span<float> samples) { std::fill(samples.begin(), samples.end(), 0.0f); }

// Left-channel values written to each fake stream, in creation order.
struct Recording {
    std::mutex mutex;
    std::vector<std::vector<float>> streams;
    std::vector<std::int64_t> frames_at_start;  // frames written when start() ran; -1 before that

    std::size_t add() {
        std::lock_guard<std::mutex> lock(mutex);
        streams.emplace_back();
        frames_at_start.push_back(-1);
        return streams.size() - 1;
    }
    std::int64_t framesAtStart(std::size_t stream) {
        std::lock_guard<std::mutex> lock(mutex);
        return stream < frames_at_start.size() ? frames_at_start[stream] : -1;
    }
    std::size_t streamCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return streams.size();
    }
    std::size_t size(std::size_t stream) {
        std::lock_guard<std::mutex> lock(mutex);
        return stream < streams.size() ? streams[stream].size() : 0;
    }
    // Consecutive ramp values from the first one, none missing, repeated or zero.
    bool contiguous(std::size_t stream) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stream >= streams.size()) return false;
        const auto& values = streams[stream];
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != values[0] + static_cast<float>(i)) return false;
        }
        return true;
    }
    float first(std::size_t stream) {
        std::lock_guard<std::mutex> lock(mutex);
        return stream < streams.size() && !streams[stream].empty() ? streams[stream].front() : 0.0f;
    }
    float last(std::size_t stream) {
        std::lock_guard<std::mutex> lock(mutex);
        return stream < streams.size() && !streams[stream].empty() ? streams[stream].back() : 0.0f;
    }
};

struct FakeConfig {
    std::uint32_t buffer_frames = 1920;
    bool consume = true;         // false: the device never plays anything, its buffer stays as filled
    int fail_after_writes = -1;  // pendingFrames() reports a lost device after this many writes
};

class FakeStream final : public tox2::RenderStream {
public:
    FakeStream(FakeConfig config, std::shared_ptr<Recording> recording)
        : config_(config), recording_(std::move(recording)), index_(recording_->add()) {}

    std::string description() const override { return "fake"; }
    std::uint32_t bufferFrames() const override { return config_.buffer_frames; }
    std::uint32_t periodFrames() const override { return 0; }

    bool waitForBuffer(std::chrono::milliseconds timeout) override {
        std::this_thread::sleep_for(std::min(timeout, 5ms));
        return true;
    }

    tox2::RenderStatus pendingFrames(std::uint32_t& frames) override {
        if (config_.fail_after_writes >= 0 && writes_ >= config_.fail_after_writes) return kDeviceLost;
        advance();
        frames = static_cast<std::uint32_t>(std::ceil(queued_));
        return 0;
    }

    tox2::RenderStatus write(std::span<const float> samples) override {
        advance();
        const std::size_t frames = samples.size() / tox2::kAudioOutputChannels;
        CHECK(queued_ + static_cast<double>(frames) <= config_.buffer_frames + 1.0);  // the pump never overfills
        queued_ += static_cast<double>(frames);
        ++writes_;
        std::lock_guard<std::mutex> lock(recording_->mutex);
        for (std::size_t i = 0; i < frames; ++i) recording_->streams[index_].push_back(samples[2 * i]);
        return 0;
    }

    tox2::RenderStatus start() override {
        started_ = true;
        last_ = Clock::now();
        std::lock_guard<std::mutex> lock(recording_->mutex);
        recording_->frames_at_start[index_] = static_cast<std::int64_t>(recording_->streams[index_].size());
        return 0;
    }

private:
    void advance() {
        const Clock::time_point now = Clock::now();
        if (started_ && config_.consume) {
            queued_ =
                    std::max(0.0, queued_ - std::chrono::duration<double>(now - last_).count() * tox2::kAudioSampleRate);
        }
        last_ = now;
    }

    const FakeConfig config_;
    const std::shared_ptr<Recording> recording_;
    const std::size_t index_;
    double queued_ = 0.0;
    int writes_ = 0;
    bool started_ = false;
    Clock::time_point last_ = Clock::now();
};

std::unique_ptr<tox2::AudioOutput> makeFakeOutput(tox2::RenderStreamFactory factory,
                                                 std::chrono::milliseconds reopen_interval = 2000ms) {
    tox2::StreamOutputOptions options;
    options.name = "coreaudio";
    options.reopen_interval = reopen_interval;
    return tox2::makeStreamOutput(std::move(factory), std::move(options));
}

void testOpenFailureIsReported() {
    auto output = makeFakeOutput([](std::string& error) -> std::unique_ptr<tox2::RenderStream> {
        error = "no render device: test";
        return nullptr;
    });
    std::string error;
    CHECK(!output->open(error));
    CHECK(error == "no render device: test");
    CHECK(output->name() == "coreaudio");
}

void testPrimesBeforeStart() {
    const auto recording = std::make_shared<Recording>();
    auto output = makeFakeOutput([recording](std::string&) -> std::unique_ptr<tox2::RenderStream> {
        return std::make_unique<FakeStream>(FakeConfig{}, recording);
    });
    std::string error;
    CHECK(output->open(error));

    // One block short of the priming amount: the stream is open but not started.
    submitPaced(*output, 0, tox2::kPrimeBlocks - 1, 1000us, fillRamp);
    std::this_thread::sleep_for(60ms);
    CHECK(recording->framesAtStart(0) == -1);
    CHECK(recording->size(0) == 0);

    submitPaced(*output, tox2::kPrimeBlocks - 1, 1, 1000us, fillRamp);
    CHECK(waitFor([&] { return recording->framesAtStart(0) >= 0; }, 1000ms));
    // The primed blocks were in the buffer by the time the device started.
    CHECK(recording->framesAtStart(0) >= static_cast<std::int64_t>(tox2::kPrimeBlocks * tox2::kAudioBlockFrames));
    CHECK(recording->contiguous(0));
}

void testHighWaterDropsAreCounted() {
    const auto recording = std::make_shared<Recording>();
    FakeConfig config;
    config.buffer_frames = 512;
    config.consume = false;
    auto output = makeFakeOutput([recording, config](std::string&) -> std::unique_ptr<tox2::RenderStream> {
        return std::make_unique<FakeStream>(config, recording);
    });
    std::string error;
    CHECK(output->open(error));

    constexpr std::uint64_t kBlocks = 40;
    submitPaced(*output, 0, kBlocks, 2000us, fillRamp);
    std::this_thread::sleep_for(100ms);

    const tox2::AudioOutputStats stats = output->stats();
    CHECK(stats.blocks_submitted == kBlocks);
    // The device took 512 frames and the ring holds kHighWaterBlocks: the rest was dropped, each block
    // counted once.
    CHECK(stats.frames_played == 512);
    CHECK(stats.blocks_dropped >= kBlocks - tox2::kHighWaterBlocks - 2);
    CHECK(stats.blocks_dropped <= kBlocks - tox2::kHighWaterBlocks);
}

void testDeviceLossAndReopen() {
    LogCapture log;
    const auto recording = std::make_shared<Recording>();
    const auto calls = std::make_shared<std::atomic<int>>(0);
    auto output = makeFakeOutput(
            [recording, calls](std::string& error) -> std::unique_ptr<tox2::RenderStream> {
                const int call = calls->fetch_add(1) + 1;
                if (call == 1) {
                    FakeConfig config;
                    config.fail_after_writes = 15;
                    return std::make_unique<FakeStream>(config, recording);
                }
                if (call <= 3) {
                    error = "still gone";
                    return nullptr;
                }
                return std::make_unique<FakeStream>(FakeConfig{}, recording);
            },
            60ms);
    std::string error;
    CHECK(output->open(error));

    // The engine keeps submitting at the block rate through the outage and must never see a drop.
    constexpr std::uint64_t kBlocks = 200;
    submitPaced(*output, 0, kBlocks, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillRamp);
    CHECK(waitFor(
            [&] {
                return recording->size(1) > 0 &&
                       output->stats().frames_played == recording->size(0) + recording->size(1);
            },
            1000ms));

    const tox2::AudioOutputStats stats = output->stats();
    CHECK(stats.device_errors == 1);
    CHECK(stats.blocks_dropped == 0);
    CHECK(log.count("tox2: audio: coreaudio: device lost") == 1);
    CHECK(log.count("tox2: audio: coreaudio: reopened fake") == 1);
    CHECK(calls->load() == 4);
    CHECK(recording->streamCount() == 2);
    CHECK(recording->contiguous(0));
    CHECK(recording->contiguous(1));
    CHECK(recording->size(0) > 0);
    CHECK(recording->size(1) > 0);
    // The frames that arrived during the outage were discarded, not queued for the new stream.
    CHECK(recording->first(1) > recording->last(0) + 1.0f);
    CHECK(recording->last(1) <= static_cast<float>(kBlocks * tox2::kAudioBlockFrames));
}

// Returns false when the machine has no output device.
bool haveOutputDevice() {
    const auto devices = tox2::listRenderDevices();
    if (!devices) {
        std::fprintf(stderr, "listRenderDevices failed: %s\n", devices.error().c_str());
        CHECK(false);
        return false;
    }
    for (const tox2::RenderDeviceInfo& device : *devices) {
        std::printf("output device: %s%s, %u Hz, %u ch, period %.1f ms\n", device.name.c_str(),
                    device.is_default ? " (default)" : "", device.mix_rate, device.mix_channels,
                    device.default_period_ms);
    }
    return std::any_of(devices->begin(), devices->end(), [](const auto& device) { return device.is_default; });
}

// Returns false (skipped) when the default device cannot be opened because there is none.
bool testRealDevicePlaysSilence() {
    auto output = tox2::makeCoreAudioOutput();
    CHECK(output->name() == "coreaudio");
    std::string error;
    if (!output->open(error)) {
        if (error.starts_with("no render device:")) return false;
        std::fprintf(stderr, "coreaudio open failed: %s\n", error.c_str());
        CHECK(false);
        return true;
    }
    std::printf("real device: %s\n", output->description().c_str());

    // The unit starts hardware in open(), so the start latency (6-34 ms) is spent before the first
    // block. When it was spent after, the engine kept producing meanwhile and the fill began 288-1632
    // frames above the primed amount. The sampler averages the fill reported in the first 250 ms of
    // streaming. Blocks arrive at the block rate, so the mean stays near the primed blocks plus the
    // preroll (one period) minus half a period, about 1230 frames for a 256-frame period; the
    // limit of kPrimeBlocks blocks plus two periods is about 300 frames above that. A mean rather than a
    // maximum keeps one late device-thread pass (one extra block in the ring for a moment) from
    // failing the test.
    const auto devices = tox2::listRenderDevices();
    double period_ms = 0.0;
    if (devices) {
        for (const tox2::RenderDeviceInfo& device : *devices) {
            if (device.is_default) period_ms = device.default_period_ms;
        }
    }
    const auto period_frames = static_cast<std::uint64_t>(period_ms * tox2::kAudioSampleRate / 1000.0);
    std::atomic<bool> sampling{true};
    std::atomic<std::uint64_t> fill_sum{0};
    std::atomic<std::uint64_t> fill_max{0};
    std::atomic<std::uint64_t> fill_count{0};
    std::thread sampler([&] {
        std::optional<Clock::time_point> first;
        while (sampling.load()) {
            if (const auto fill = output->fillFrames()) {
                if (!first) first = Clock::now();
                if (Clock::now() - *first <= 250ms) {
                    fill_sum += *fill;
                    fill_count++;
                    if (*fill > fill_max.load()) fill_max.store(*fill);
                }
            }
            std::this_thread::sleep_for(2ms);
        }
    });

    // 1 s of silence at the block rate.
    constexpr std::uint64_t kBlocks = 188;
    submitPaced(*output, 0, kBlocks, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillSilence);
    sampling = false;
    sampler.join();
    const std::uint64_t fill_limit = tox2::kPrimeBlocks * tox2::kAudioBlockFrames + 2 * period_frames;
    const std::uint64_t fill_mean = fill_count.load() != 0 ? fill_sum.load() / fill_count.load() : 0;
    std::printf(
            "real device early fill: mean %llu, max %llu frames over %llu samples, limit %llu (period %llu frames)\n",
            static_cast<unsigned long long>(fill_mean), static_cast<unsigned long long>(fill_max.load()),
            static_cast<unsigned long long>(fill_count.load()), static_cast<unsigned long long>(fill_limit),
            static_cast<unsigned long long>(period_frames));
    CHECK(period_frames != 0);
    CHECK(fill_count.load() >= 20);
    CHECK(fill_mean <= fill_limit);
    CHECK(waitFor([&] { return output->stats().frames_played >= 44000; }, 1000ms));

    const tox2::AudioOutputStats stats = output->stats();
    std::printf(
            "real device stats: submitted %llu, dropped %llu, underrun %llu frames, played %llu frames, errors %llu\n",
            static_cast<unsigned long long>(stats.blocks_submitted),
            static_cast<unsigned long long>(stats.blocks_dropped),
            static_cast<unsigned long long>(stats.underrun_frames),
            static_cast<unsigned long long>(stats.frames_played), static_cast<unsigned long long>(stats.device_errors));
    CHECK(stats.frames_played >= 44000);
    CHECK(stats.underrun_frames < 4800);
    CHECK(stats.device_errors == 0);
    return true;
}

}  // namespace

int main() {
    testOpenFailureIsReported();
    testPrimesBeforeStart();
    testHighWaterDropsAreCounted();
    testDeviceLossAndReopen();

    bool skipped = true;
    if (haveOutputDevice()) skipped = !testRealDevicePlaysSilence();
    if (skipped) std::printf("no output device: the real-device test is skipped\n");

    std::printf("audio coreaudio tests: %d passed, %d failed\n", g_passed.load(), g_failed.load());
    if (g_failed != 0) return 1;
    return skipped ? kExitSkip : 0;
}

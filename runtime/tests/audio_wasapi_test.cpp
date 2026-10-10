// Tests for the WASAPI output. The device thread logic (priming, drop accounting, device loss and
// reopen) runs over fake render streams and needs no sound device. Two more tests open the default
// render device; without one they are skipped and the program exits 77 (CTest "skipped") when
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
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/audio_output.h"
#include "audio/wasapi_output.h"
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
constexpr tox2::RenderStatus kDeviceInvalidated =
        static_cast<tox2::RenderStatus>(0x88890004);  // AUDCLNT_E_DEVICE_INVALIDATED

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
void submitPaced(tox2::AudioOutput& output, std::uint64_t first, std::uint64_t blocks,
                 std::chrono::microseconds spacing,
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

    std::size_t add() {
        std::lock_guard<std::mutex> lock(mutex);
        streams.emplace_back();
        return streams.size() - 1;
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
        if (config_.fail_after_writes >= 0 && writes_ >= config_.fail_after_writes) return kDeviceInvalidated;
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
        return 0;
    }

private:
    void advance() {
        const Clock::time_point now = Clock::now();
        if (started_ && config_.consume) {
            queued_ = std::max(0.0,
                               queued_ - std::chrono::duration<double>(now - last_).count() * tox2::kAudioSampleRate);
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

// Wraps a real stream and reports a lost device after a number of writes.
class FlakyStream final : public tox2::RenderStream {
public:
    FlakyStream(std::unique_ptr<tox2::RenderStream> inner, int fail_after_writes)
        : inner_(std::move(inner)), fail_after_writes_(fail_after_writes) {}

    std::string description() const override { return inner_->description(); }
    std::uint32_t bufferFrames() const override { return inner_->bufferFrames(); }
    std::uint32_t periodFrames() const override { return inner_->periodFrames(); }
    bool waitForBuffer(std::chrono::milliseconds timeout) override { return inner_->waitForBuffer(timeout); }
    tox2::RenderStatus pendingFrames(std::uint32_t& frames) override {
        if (writes_ >= fail_after_writes_) return kDeviceInvalidated;
        return inner_->pendingFrames(frames);
    }
    tox2::RenderStatus write(std::span<const float> samples) override {
        ++writes_;
        return inner_->write(samples);
    }
    tox2::RenderStatus start() override { return inner_->start(); }

private:
    std::unique_ptr<tox2::RenderStream> inner_;
    const int fail_after_writes_;
    int writes_ = 0;
};

void testOpenFailureIsReported() {
    auto output = tox2::makeWasapiOutputForTest([](std::string& error) -> std::unique_ptr<tox2::RenderStream> {
        error = "no render device: test";
        return nullptr;
    });
    std::string error;
    CHECK(!output->open(error));
    CHECK(error == "no render device: test");
    CHECK(output->name() == "wasapi");
}

void testOpenTimeoutDoesNotStallTheCaller() {
    tox2::WasapiOutputOptions options;
    options.open_timeout = 50ms;
    const Clock::time_point start = Clock::now();
    {
        auto output = tox2::makeWasapiOutputForTest(
                [](std::string& error) -> std::unique_ptr<tox2::RenderStream> {
                    std::this_thread::sleep_for(500ms);
                    error = "too late";
                    return nullptr;
                },
                options);
        std::string error;
        CHECK(!output->open(error));
        CHECK(!error.empty());
    }
    // The output is destroyed while its device thread is still inside the factory.
    CHECK(Clock::now() - start < 400ms);
}

void testStreamStaysContiguous() {
    const auto recording = std::make_shared<Recording>();
    auto output = tox2::makeWasapiOutputForTest([recording](std::string&) -> std::unique_ptr<tox2::RenderStream> {
        return std::make_unique<FakeStream>(FakeConfig{}, recording);
    });
    std::string error;
    CHECK(output->open(error));
    CHECK(output->description() == "fake");

    constexpr std::uint64_t kBlocks = 100;
    submitPaced(*output, 0, kBlocks, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillRamp);
    const std::uint64_t frames = kBlocks * tox2::kAudioBlockFrames;
    CHECK(waitFor([&] { return output->stats().frames_played == frames; }, 1000ms));

    const tox2::AudioOutputStats stats = output->stats();
    CHECK(stats.blocks_submitted == kBlocks);
    CHECK(stats.blocks_dropped == 0);
    CHECK(stats.device_errors == 0);
    CHECK(recording->streamCount() == 1);
    CHECK(recording->size(0) == frames);
    // No zeros were inserted, nothing was lost or repeated, and the order is the submit order.
    CHECK(recording->contiguous(0));
    CHECK(recording->first(0) == 1.0f);
}

void testStallCountsUnderrun() {
    const auto recording = std::make_shared<Recording>();
    auto output = tox2::makeWasapiOutputForTest([recording](std::string&) -> std::unique_ptr<tox2::RenderStream> {
        return std::make_unique<FakeStream>(FakeConfig{}, recording);
    });
    std::string error;
    CHECK(output->open(error));

    const auto spacing = std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod);
    submitPaced(*output, 0, 30, spacing, fillRamp);
    const std::uint64_t before = output->stats().underrun_frames;
    // The producer stalls for a quarter of a second: the device plays silence for about 230 ms.
    std::this_thread::sleep_for(250ms);
    submitPaced(*output, 30, 30, spacing, fillRamp);
    CHECK(waitFor([&] { return output->stats().frames_played == 60 * tox2::kAudioBlockFrames; }, 1000ms));

    const std::uint64_t underrun = output->stats().underrun_frames - before;
    CHECK(underrun > 5000);
    CHECK(underrun < 13000);
    CHECK(output->stats().blocks_dropped == 0);
    CHECK(recording->contiguous(0));
}

void testHighWaterDropsAreCounted() {
    const auto recording = std::make_shared<Recording>();
    FakeConfig config;
    config.buffer_frames = 512;
    config.consume = false;
    auto output =
            tox2::makeWasapiOutputForTest([recording, config](std::string&) -> std::unique_ptr<tox2::RenderStream> {
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

void testFillFramesCountsQueueAndDevice() {
    const auto recording = std::make_shared<Recording>();
    FakeConfig config;
    config.buffer_frames = 512;
    config.consume = false;
    auto output =
            tox2::makeWasapiOutputForTest([recording, config](std::string&) -> std::unique_ptr<tox2::RenderStream> {
                return std::make_unique<FakeStream>(config, recording);
            });
    CHECK(!output->fillFrames().has_value());
    std::string error;
    CHECK(output->open(error));
    // Priming is not playing: no fill level yet.
    submitPaced(*output, 0, 2, 2000us, fillRamp);
    CHECK(!output->fillFrames().has_value());

    submitPaced(*output, 2, 8, 2000us, fillRamp);
    CHECK(waitFor([&] { return output->stats().frames_played == 512; }, 1000ms));
    // The device holds 512 frames and the ring the other 2048 of the ten blocks.
    CHECK(waitFor([&] { return output->fillFrames().has_value(); }, 1000ms));
    CHECK(output->fillFrames().value_or(0) == 10 * tox2::kAudioBlockFrames);
}

void testFillFramesEndsWithTheDevice() {
    const auto recording = std::make_shared<Recording>();
    FakeConfig config;
    config.fail_after_writes = 2;
    tox2::WasapiOutputOptions options;
    options.reopen_interval = 5000ms;  // the outage lasts for the whole test
    const auto opened = std::make_shared<std::atomic<int>>(0);
    auto output = tox2::makeWasapiOutputForTest(
            [recording, config, opened](std::string& error) -> std::unique_ptr<tox2::RenderStream> {
                if (opened->fetch_add(1) > 0) {
                    error = "still gone";
                    return nullptr;
                }
                return std::make_unique<FakeStream>(config, recording);
            },
            options);
    std::string error;
    CHECK(output->open(error));
    submitPaced(*output, 0, 30, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillRamp);
    CHECK(waitFor([&] { return output->stats().device_errors == 1; }, 1000ms));
    CHECK(!output->fillFrames().has_value());
}

void testDeviceLossAndReopen() {
    LogCapture log;
    const auto recording = std::make_shared<Recording>();
    const auto calls = std::make_shared<std::atomic<int>>(0);
    tox2::WasapiOutputOptions options;
    options.reopen_interval = 60ms;
    auto output = tox2::makeWasapiOutputForTest(
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
            options);
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
    CHECK(log.count("device lost") == 1);
    CHECK(log.count("reopened fake") == 1);
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

// Returns false when the machine has no render device.
bool haveRenderDevice() {
    const auto devices = tox2::listRenderDevices();
    return devices &&
           std::any_of(devices->begin(), devices->end(), [](const auto& device) { return device.is_default; });
}

// Returns false (skipped) when the default device cannot be opened because there is none.
bool testRealDevicePlaysSilence() {
    auto output = tox2::makeWasapiOutput();
    std::string error;
    if (!output->open(error)) {
        if (error.starts_with("no render device:")) return false;
        std::fprintf(stderr, "wasapi open failed: %s\n", error.c_str());
        CHECK(false);
        return true;
    }
    std::printf("real device: %s\n", output->description().c_str());

    // 0.5 s of silence at the block rate.
    constexpr std::uint64_t kBlocks = 94;
    submitPaced(*output, 0, kBlocks, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillSilence);
    CHECK(waitFor([&] { return output->stats().frames_played >= kBlocks * tox2::kAudioBlockFrames; }, 1000ms));

    const tox2::AudioOutputStats stats = output->stats();
    std::printf(
            "real device stats: submitted %llu, dropped %llu, underrun %llu frames, played %llu frames, errors %llu\n",
            static_cast<unsigned long long>(stats.blocks_submitted),
            static_cast<unsigned long long>(stats.blocks_dropped),
            static_cast<unsigned long long>(stats.underrun_frames),
            static_cast<unsigned long long>(stats.frames_played), static_cast<unsigned long long>(stats.device_errors));
    CHECK(stats.frames_played >= 24000 * 80 / 100);
    CHECK(stats.frames_played <= 24000 * 120 / 100);
    CHECK(stats.device_errors == 0);
    return true;
}

// The real stream reports a lost device once; the output reopens the default device and plays on.
void testRealDeviceLossAndReopen() {
    LogCapture log;
    tox2::WasapiOutputOptions options;
    options.reopen_interval = 100ms;
    const tox2::RenderStreamFactory real = tox2::makeWasapiStreamFactory();
    auto output = tox2::makeWasapiOutputForTest(
            [real, calls = 0](std::string& error) mutable -> std::unique_ptr<tox2::RenderStream> {
                std::unique_ptr<tox2::RenderStream> stream = real(error);
                if (stream && calls++ == 0) return std::make_unique<FlakyStream>(std::move(stream), 20);
                return stream;
            },
            options);
    std::string error;
    if (!output->open(error)) {
        std::fprintf(stderr, "wasapi open failed: %s\n", error.c_str());
        CHECK(false);
        return;
    }

    constexpr std::uint64_t kBlocks = 225;  // 1.2 s
    submitPaced(*output, 0, kBlocks, std::chrono::duration_cast<std::chrono::microseconds>(tox2::kAudioBlockPeriod),
                fillSilence);
    CHECK(waitFor([&] { return log.count("reopened") == 1; }, 1000ms));
    std::this_thread::sleep_for(200ms);

    const tox2::AudioOutputStats stats = output->stats();
    std::printf("real device loss stats: dropped %llu, played %llu frames, errors %llu\n",
                static_cast<unsigned long long>(stats.blocks_dropped),
                static_cast<unsigned long long>(stats.frames_played),
                static_cast<unsigned long long>(stats.device_errors));
    CHECK(stats.device_errors == 1);
    CHECK(log.count("device lost") == 1);
    CHECK(stats.blocks_dropped == 0);
    // Frames kept flowing after the reopen: the loss costs about 100 ms of the 1.2 s.
    CHECK(stats.frames_played > kBlocks * tox2::kAudioBlockFrames / 2);
}

}  // namespace

int main() {
    testOpenFailureIsReported();
    testOpenTimeoutDoesNotStallTheCaller();
    testStreamStaysContiguous();
    testStallCountsUnderrun();
    testHighWaterDropsAreCounted();
    testFillFramesCountsQueueAndDevice();
    testFillFramesEndsWithTheDevice();
    testDeviceLossAndReopen();

    bool skipped = true;
    if (haveRenderDevice()) {
        skipped = !testRealDevicePlaysSilence();
        if (!skipped) testRealDeviceLossAndReopen();
    }
    if (skipped) std::printf("no render device: the real-device tests are skipped\n");

    std::printf("audio wasapi tests: %d passed, %d failed\n", g_passed.load(), g_failed.load());
    if (g_failed != 0) return 1;
    return skipped ? kExitSkip : 0;
}

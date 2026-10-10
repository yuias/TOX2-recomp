// WASAPI audio output (shared mode, default render device) and the device-facing seam it is built on.
// The header holds no Windows types, so tools and tests can include it freely.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio/audio_output.h"

namespace tox2 {

// The output owns a ring between the engine thread and the device thread. The device starts playing
// once kPrimeBlocks are queued; above kHighWaterBlocks submit() drops the oldest block (device clock
// slower than the host clock). The high-water mark stays well below the capacity because a drop is
// only applied when the device thread next reads the ring.
inline constexpr std::size_t kRingBlocks = 32;
inline constexpr std::size_t kPrimeBlocks = 4;
inline constexpr std::size_t kHighWaterBlocks = 12;
static_assert(kPrimeBlocks < kHighWaterBlocks && kHighWaterBlocks < kRingBlocks);

// Status codes are HRESULT values: zero or positive is success, negative a failure.
using RenderStatus = std::int32_t;

// One opened, initialised stream on a render endpoint, used from the device thread only. The real
// implementation wraps IAudioClient; tests substitute fakes to exercise device loss.
class RenderStream {
public:
    virtual ~RenderStream() = default;
    virtual std::string description() const = 0;
    virtual std::uint32_t bufferFrames() const = 0;
    // The endpoint's processing period; the audio engine takes the buffer in whole periods. 0 when
    // unknown.
    virtual std::uint32_t periodFrames() const = 0;
    // Waits for the device's "buffer has room" signal. False on timeout.
    virtual bool waitForBuffer(std::chrono::milliseconds timeout) = 0;
    // Frames written and not yet played.
    virtual RenderStatus pendingFrames(std::uint32_t& frames) = 0;
    // Appends interleaved stereo frames (at most bufferFrames() - pendingFrames()).
    virtual RenderStatus write(std::span<const float> samples) = 0;
    virtual RenderStatus start() = 0;
};

// Opens a stream; null with the reason in `error` on failure.
using RenderStreamFactory = std::function<std::unique_ptr<RenderStream>(std::string& error)>;

// Opens the default render device (shared mode, float32 48 kHz stereo, 40 ms buffer). Must run on a
// thread whose COM apartment is initialised (the output's device thread is). An error that starts
// with "no render device:" means the machine has no usable render endpoint.
RenderStreamFactory makeWasapiStreamFactory();

struct WasapiOutputOptions {
    std::chrono::milliseconds open_timeout{2000};     // how long open() waits for the device thread
    std::chrono::milliseconds reopen_interval{2000};  // between reopen attempts after a device loss
};

std::unique_ptr<AudioOutput> makeWasapiOutput();
// Same output over any stream factory; for tests.
std::unique_ptr<AudioOutput> makeWasapiOutputForTest(RenderStreamFactory factory, WasapiOutputOptions options = {});

struct RenderDeviceInfo {
    std::string name;  // friendly name, UTF-8
    bool is_default = false;
    std::uint32_t mix_rate = 0, mix_channels = 0, mix_bits = 0;
    bool mix_float = false;
    double default_period_ms = 0, min_period_ms = 0;
};

// Active render endpoints. An empty list means the machine has none; an error means the device
// enumeration itself failed.
std::expected<std::vector<RenderDeviceInfo>, std::string> listRenderDevices();

}  // namespace tox2

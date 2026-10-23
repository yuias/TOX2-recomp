// The platform-independent half of a device audio output: the ring between the engine thread and the
// device thread, priming, drop accounting, device loss and reopen. The platform libraries (WASAPI,
// CoreAudio) supply a RenderStream; the header holds no platform types.

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

// Zero or positive is success, negative a failure. The platform defines the values (HRESULT on
// Windows; on macOS an OSStatus, with a positive error code negated).
using RenderStatus = std::int32_t;

// One opened, initialised stream on a render endpoint, used from the device thread only. The real
// implementations wrap IAudioClient and an AudioUnit; tests substitute fakes to exercise device loss.
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

struct StreamOutputOptions {
    std::string name;                                 // AudioOutput::name() and the log prefix: "wasapi", "coreaudio"
    std::chrono::milliseconds open_timeout{2000};     // how long open() waits for the device thread
    std::chrono::milliseconds reopen_interval{2000};  // between reopen attempts after a device loss
    // Runs first on the device thread; what it returns lives until that thread ends (COM apartment,
    // scheduler class). May be empty.
    std::function<std::shared_ptr<void>()> enter_device_thread;
    // The log text of a failing status. Empty: the number.
    std::function<std::string(RenderStatus)> status_text;
};

// An output whose device thread pulls from a ring and writes to streams made by `factory`.
std::unique_ptr<AudioOutput> makeStreamOutput(RenderStreamFactory factory, StreamOutputOptions options);

struct RenderDeviceInfo {
    std::string name;  // friendly name, UTF-8
    bool is_default = false;
    std::uint32_t mix_rate = 0, mix_channels = 0, mix_bits = 0;
    bool mix_float = false;
    double default_period_ms = 0, min_period_ms = 0;
};

// Active render endpoints. An empty list means the machine has none; an error means the device
// enumeration itself failed. Implemented by the platform output library.
std::expected<std::vector<RenderDeviceInfo>, std::string> listRenderDevices();

}  // namespace tox2

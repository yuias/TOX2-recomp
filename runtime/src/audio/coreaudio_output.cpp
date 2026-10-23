#include "audio/coreaudio_output.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace tox2 {

namespace {

constexpr std::uint32_t kFifoMs = 40;
constexpr std::uint32_t kFifoFrames = kAudioSampleRate * kFifoMs / 1000;

// Reported once the device is gone or the default device changed; the output reopens the stream.
constexpr RenderStatus kDeviceLost = -static_cast<RenderStatus>(kAudioHardwareBadDeviceError);

RenderStatus toStatus(OSStatus error) { return error == 0 ? 0 : (error < 0 ? error : -error); }

// A four-character code ('!dev') when the four bytes are printable, else the number. A positive
// OSStatus was negated by toStatus(); real OSStatus values below zero are small numbers that are
// never four printable characters.
std::string statusText(RenderStatus status) {
    if (status < 0) {
        const std::uint32_t code = static_cast<std::uint32_t>(-static_cast<std::int64_t>(status));
        char text[4];
        bool printable = true;
        for (int i = 0; i < 4; ++i) {
            text[i] = static_cast<char>((code >> (24 - 8 * i)) & 0xff);
            printable = printable && text[i] >= 0x20 && text[i] < 0x7f;
        }
        if (printable) return std::format("'{}'", std::string_view(text, 4));
    }
    return std::format("{}", status);
}

std::string failure(std::string_view what, OSStatus error) {
    return std::format("{} failed ({})", what, statusText(toStatus(error)));
}

template <typename T>
OSStatus getProperty(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope,
                     T& value) {
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value);
}

AudioObjectID defaultOutputDevice() {
    AudioObjectID device = kAudioObjectUnknown;
    if (getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                    kAudioObjectPropertyScopeGlobal, device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

std::string cfStringToUtf8(CFStringRef text) {
    if (text == nullptr) return {};
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string utf8(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(text, utf8.data(), capacity, kCFStringEncodingUTF8)) return {};
    utf8.resize(std::strlen(utf8.c_str()));
    return utf8;
}

std::string deviceName(AudioObjectID device) {
    CFStringRef name = nullptr;
    if (getProperty(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name) != noErr ||
        name == nullptr) {
        return "(unnamed device)";
    }
    std::string utf8 = cfStringToUtf8(name);
    CFRelease(name);
    return utf8.empty() ? "(unnamed device)" : utf8;
}

// Output channels over all streams of the device; 0 for an input-only device.
std::uint32_t outputChannels(AudioObjectID device) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioBufferList)) {
        return 0;
    }
    std::vector<std::byte> raw(size);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, raw.data()) != noErr) return 0;
    const auto* buffers = reinterpret_cast<const AudioBufferList*>(raw.data());
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) channels += buffers->mBuffers[i].mNumberChannels;
    return channels;
}

// The device's nominal rate, channels and buffer size. The HAL's virtual format is float32.
OSStatus readDeviceFacts(AudioObjectID device, RenderDeviceInfo& info) {
    Float64 rate = 0;
    OSStatus error = getProperty(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
    if (error != noErr) return error;
    UInt32 buffer_frames = 0;
    error = getProperty(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, buffer_frames);
    if (error != noErr) return error;

    info.mix_rate = static_cast<std::uint32_t>(rate);
    info.mix_channels = outputChannels(device);
    info.mix_bits = 32;
    info.mix_float = true;
    info.default_period_ms = rate > 0 ? 1000.0 * buffer_frames / rate : 0.0;
    info.min_period_ms = info.default_period_ms;
    AudioValueRange range{};
    if (rate > 0 && getProperty(device, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal,
                                range) == noErr) {
        info.min_period_ms = 1000.0 * range.mMinimum / rate;
    }
    return noErr;
}

// Single-producer single-consumer FIFO of interleaved stereo frames: the device thread writes, the
// render callback reads. Monotonic frame counters; the position in storage_ is the counter modulo
// the capacity. Nothing here locks or allocates after construction.
class FrameFifo {
public:
    explicit FrameFifo(std::uint32_t capacity_frames)
        : storage_(std::size_t{capacity_frames} * kAudioOutputChannels), capacity_(capacity_frames) {}

    std::uint32_t capacity() const { return capacity_; }

    std::uint32_t queuedFrames() const {
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        return static_cast<std::uint32_t>(write_.load(std::memory_order_acquire) - read);
    }

    // Producer side. Stores what fits and returns the frames stored.
    std::uint32_t push(std::span<const float> samples) {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        const std::uint32_t free_frames = capacity_ - static_cast<std::uint32_t>(write - read);
        const std::uint32_t frames =
                std::min(free_frames, static_cast<std::uint32_t>(samples.size() / kAudioOutputChannels));
        copyIn(write, samples.data(), frames);
        write_.store(write + frames, std::memory_order_release);
        return frames;
    }

    // Consumer side (the render callback). Copies up to `frames` frames and returns the count.
    std::uint32_t pop(float* out, std::uint32_t frames) {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        const std::uint32_t taken = std::min(frames, static_cast<std::uint32_t>(write - read));
        const std::uint32_t start = static_cast<std::uint32_t>(read % capacity_);
        const std::uint32_t first = std::min(taken, capacity_ - start);
        std::memcpy(out, storage_.data() + std::size_t{start} * kAudioOutputChannels,
                    std::size_t{first} * kAudioOutputChannels * sizeof(float));
        std::memcpy(out + std::size_t{first} * kAudioOutputChannels, storage_.data(),
                    std::size_t{taken - first} * kAudioOutputChannels * sizeof(float));
        read_.store(read + taken, std::memory_order_release);
        return taken;
    }

private:
    void copyIn(std::uint64_t write, const float* samples, std::uint32_t frames) {
        const std::uint32_t start = static_cast<std::uint32_t>(write % capacity_);
        const std::uint32_t first = std::min(frames, capacity_ - start);
        std::memcpy(storage_.data() + std::size_t{start} * kAudioOutputChannels, samples,
                    std::size_t{first} * kAudioOutputChannels * sizeof(float));
        std::memcpy(storage_.data(), samples + std::size_t{first} * kAudioOutputChannels,
                    std::size_t{frames - first} * kAudioOutputChannels * sizeof(float));
    }

    std::vector<float> storage_;
    const std::uint32_t capacity_;
    alignas(64) std::atomic<std::uint64_t> write_{0};  // producer-owned
    alignas(64) std::atomic<std::uint64_t> read_{0};   // consumer-owned
};

class CoreAudioStream final : public RenderStream {
public:
    static std::unique_ptr<CoreAudioStream> open(std::string& error) {
        if (defaultOutputDevice() == kAudioObjectUnknown) {
            error = "no render device: there is no default output device";
            return nullptr;
        }

        std::unique_ptr<CoreAudioStream> stream(new CoreAudioStream());
        const AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                                    kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr) {
            error = "finding the default output unit failed";
            return nullptr;
        }
        OSStatus status = AudioComponentInstanceNew(component, &stream->unit_);
        if (status != noErr) {
            error = failure("creating the output unit", status);
            return nullptr;
        }

        // Our own format; the default output unit converts it to the device's.
        AudioStreamBasicDescription format{};
        format.mSampleRate = kAudioSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = kAudioOutputChannels;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = kAudioOutputChannels * sizeof(float);
        format.mBytesPerPacket = format.mBytesPerFrame;
        status = AudioUnitSetProperty(stream->unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                      sizeof(format));
        if (status != noErr) {
            error = failure("setting the stream format", status);
            return nullptr;
        }

        const AURenderCallbackStruct callback{&CoreAudioStream::renderCallback, stream.get()};
        status = AudioUnitSetProperty(stream->unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                      &callback, sizeof(callback));
        if (status != noErr) {
            error = failure("setting the render callback", status);
            return nullptr;
        }
        status = AudioUnitInitialize(stream->unit_);
        if (status != noErr) {
            error = failure("AudioUnitInitialize", status);
            return nullptr;
        }
        stream->initialized_ = true;

        UInt32 size = sizeof(stream->device_);
        status = AudioUnitGetProperty(stream->unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                      &stream->device_, &size);
        if (status != noErr) {
            error = failure("reading the output device", status);
            return nullptr;
        }
        RenderDeviceInfo facts;
        status = readDeviceFacts(stream->device_, facts);
        if (status != noErr) {
            error = failure("reading the device format", status);
            return nullptr;
        }

        status = stream->addListeners();
        if (status != noErr) {
            error = failure("AudioObjectAddPropertyListener", status);
            return nullptr;
        }

        // Starting the hardware takes 6-34 ms before the first render callback. Spent here, it
        // passes before the stream is published as open and the engine's block clock runs; spent in
        // start() it would pile up in the engine's fill, which then sits 1000-1500 frames above its
        // target and keeps the clock controller at its limit for about a minute. The callback
        // plays silence until start() opens the gate.
        status = AudioOutputUnitStart(stream->unit_);
        if (status != noErr) {
            error = failure("AudioOutputUnitStart", status);
            return nullptr;
        }
        stream->started_ = true;

        stream->period_frames_ = static_cast<std::uint32_t>(facts.default_period_ms * kAudioSampleRate / 1000.0);
        stream->description_ = std::format("{}, mix {} Hz {} ch, buffer {:.1f} ms, period {:.1f} ms",
                                           deviceName(stream->device_), facts.mix_rate, facts.mix_channels,
                                           1000.0 * kFifoFrames / kAudioSampleRate, facts.default_period_ms);
        return stream;
    }

    // The listeners go first so that none runs against a destroyed stream; stopping the unit waits
    // for a render callback that is still running. started_ is set once open() has started the unit.
    ~CoreAudioStream() override {
        removeListeners();
        if (started_) AudioOutputUnitStop(unit_);
        if (initialized_) AudioUnitUninitialize(unit_);
        if (unit_ != nullptr) AudioComponentInstanceDispose(unit_);
        if (signal_ != nullptr) dispatch_release(signal_);
    }

    std::string description() const override { return description_; }
    std::uint32_t bufferFrames() const override { return fifo_.capacity(); }
    std::uint32_t periodFrames() const override { return period_frames_; }

    bool waitForBuffer(std::chrono::milliseconds timeout) override {
        const dispatch_time_t due =
                dispatch_time(DISPATCH_TIME_NOW, std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count());
        if (dispatch_semaphore_wait(signal_, due) != 0) return false;
        // One pass serves every period that has passed; leftover signals would only wake the next
        // wait for nothing.
        while (dispatch_semaphore_wait(signal_, DISPATCH_TIME_NOW) == 0) {
        }
        return true;
    }

    RenderStatus pendingFrames(std::uint32_t& frames) override {
        if (lost_.load(std::memory_order_relaxed)) return kDeviceLost;
        frames = fifo_.queuedFrames();
        return 0;
    }

    RenderStatus write(std::span<const float> samples) override {
        if (lost_.load(std::memory_order_relaxed)) return kDeviceLost;
        fifo_.push(samples);
        return 0;
    }

    // The unit already runs (see open()); this lets the callback consume the FIFO.
    RenderStatus start() override {
        if (lost_.load(std::memory_order_relaxed)) return kDeviceLost;
        playing_.store(true, std::memory_order_release);
        return 0;
    }

private:
    CoreAudioStream() : fifo_(kFifoFrames), signal_(dispatch_semaphore_create(0)) {}

    // Runs on the CoreAudio I/O thread with a deadline: only the FIFO and the semaphore, no locks,
    // allocation or logging, and never guest memory.
    static OSStatus renderCallback(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32,
                                   UInt32 frames, AudioBufferList* data) {
        auto& self = *static_cast<CoreAudioStream*>(context);
        if (data == nullptr) return noErr;
        if (!self.playing_.load(std::memory_order_acquire)) {
            // Not started by the device thread yet: the primed frames stay in the FIFO. Signals sent
            // meanwhile only make the first waitForBuffer() return at once; the device thread then
            // reads the real fill from pendingFrames().
            for (UInt32 i = 0; i < data->mNumberBuffers; ++i) {
                if (data->mBuffers[i].mData != nullptr)
                    std::memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
            }
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            dispatch_semaphore_signal(self.signal_);
            return noErr;
        }
        std::uint32_t copied = 0;
        for (UInt32 i = 0; i < data->mNumberBuffers; ++i) {
            AudioBuffer& buffer = data->mBuffers[i];
            if (buffer.mData == nullptr) continue;
            // The interleaved stereo format gives one buffer; any other is silenced.
            const std::uint32_t capacity = buffer.mDataByteSize / (kAudioOutputChannels * sizeof(float));
            const std::uint32_t wanted = i == 0 ? std::min<std::uint32_t>(frames, capacity) : 0;
            auto* out = static_cast<float*>(buffer.mData);
            const std::uint32_t got = wanted != 0 ? self.fifo_.pop(out, wanted) : 0;
            // A shortfall is the device running dry; it plays silence.
            std::memset(out + std::size_t{got} * kAudioOutputChannels, 0,
                        buffer.mDataByteSize - std::size_t{got} * kAudioOutputChannels * sizeof(float));
            if (i == 0) copied = got;
        }
        if (copied == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        dispatch_semaphore_signal(self.signal_);
        return noErr;
    }

    // Notifications come from a CoreAudio thread; they only set the flag.
    static OSStatus propertyChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* context) {
        static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_relaxed);
        return noErr;
    }

    // The default output unit follows a change of the default device by itself, but a reopened stream
    // also re-reads the buffer size and rate, so a change counts as a loss.
    OSStatus addListeners() {
        const AudioObjectPropertyAddress default_changed{kAudioHardwarePropertyDefaultOutputDevice,
                                                         kAudioObjectPropertyScopeGlobal,
                                                         kAudioObjectPropertyElementMain};
        OSStatus status =
                AudioObjectAddPropertyListener(kAudioObjectSystemObject, &default_changed, &propertyChanged, &lost_);
        if (status != noErr) return status;
        default_listener_ = true;
        const AudioObjectPropertyAddress alive{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
        status = AudioObjectAddPropertyListener(device_, &alive, &propertyChanged, &lost_);
        if (status != noErr) return status;
        alive_listener_ = true;
        return noErr;
    }

    void removeListeners() {
        if (default_listener_) {
            const AudioObjectPropertyAddress default_changed{kAudioHardwarePropertyDefaultOutputDevice,
                                                             kAudioObjectPropertyScopeGlobal,
                                                             kAudioObjectPropertyElementMain};
            AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &default_changed, &propertyChanged, &lost_);
        }
        if (alive_listener_) {
            const AudioObjectPropertyAddress alive{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
            AudioObjectRemovePropertyListener(device_, &alive, &propertyChanged, &lost_);
        }
    }

    FrameFifo fifo_;
    dispatch_semaphore_t signal_;
    AudioComponentInstance unit_ = nullptr;
    AudioObjectID device_ = kAudioObjectUnknown;
    std::atomic<bool> lost_{false};
    std::atomic<bool> playing_{false};  // set by start(); until then the callback plays silence
    std::uint32_t period_frames_ = 0;
    std::string description_;
    bool initialized_ = false;
    bool started_ = false;
    bool default_listener_ = false;
    bool alive_listener_ = false;
};

}  // namespace

RenderStreamFactory makeCoreAudioStreamFactory() {
    return [](std::string& error) -> std::unique_ptr<RenderStream> { return CoreAudioStream::open(error); };
}

std::unique_ptr<AudioOutput> makeCoreAudioOutput() {
    StreamOutputOptions options;
    options.name = "coreaudio";
    options.status_text = statusText;
    return makeStreamOutput(makeCoreAudioStreamFactory(), std::move(options));
}

std::expected<std::vector<RenderDeviceInfo>, std::string> listRenderDevices() {
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
    if (status != noErr) return std::unexpected(failure("reading the device list size", status));
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    size = static_cast<UInt32>(ids.size() * sizeof(AudioObjectID));
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data());
    if (status != noErr) return std::unexpected(failure("reading the device list", status));
    ids.resize(size / sizeof(AudioObjectID));

    const AudioObjectID default_device = defaultOutputDevice();
    std::vector<RenderDeviceInfo> devices;
    for (const AudioObjectID id : ids) {
        if (outputChannels(id) == 0) continue;
        RenderDeviceInfo info;
        info.name = deviceName(id);
        info.is_default = id == default_device;
        readDeviceFacts(id, info);
        devices.push_back(std::move(info));
    }
    return devices;
}

}  // namespace tox2

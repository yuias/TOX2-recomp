#include "audio/wasapi_output.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// initguid.h turns the GUID and property key declarations of the headers below into definitions.
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/log.h"
#include "core/sleeper.h"

namespace dal {

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

// Between passes of the device thread. Passes normally follow the device's buffer event; the slice
// bounds how long a missed event, a stop request or a ring that needs its drops applied can wait.
constexpr std::chrono::milliseconds kWaitSlice{20};
constexpr std::chrono::milliseconds kBufferDuration{40};
constexpr std::chrono::milliseconds kOutagePoll{5};
constexpr std::chrono::milliseconds kJoinTimeout{1000};
constexpr double kHundredNsPerMs = 10'000.0;

constexpr DWORD kStereoMask = 0x3;  // front left | front right (KSAUDIO_SPEAKER_STEREO)
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid pulling in ksmedia.h.
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string hresultText(HRESULT hr) { return std::format("{:#010x}", static_cast<std::uint32_t>(hr)); }

std::string failure(std::string_view what, HRESULT hr) { return std::format("{} failed ({})", what, hresultText(hr)); }

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// Joins the thread's COM apartment (multithreaded). A thread that already chose another model keeps
// it and still works with the free-threaded interfaces used here.
class ComApartment {
public:
    ComApartment() : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComApartment() {
        if (initialized_) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_;
};

struct CoTaskMemDeleter {
    void operator()(void* memory) const { CoTaskMemFree(memory); }
};
template <typename T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

struct HandleCloser {
    void operator()(HANDLE handle) const {
        if (handle != nullptr) CloseHandle(handle);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

class PropVariantGuard {
public:
    PropVariantGuard() { PropVariantInit(&value_); }
    ~PropVariantGuard() { PropVariantClear(&value_); }
    PropVariantGuard(const PropVariantGuard&) = delete;
    PropVariantGuard& operator=(const PropVariantGuard&) = delete;
    PROPVARIANT* get() { return &value_; }
    const PROPVARIANT& value() const { return value_; }

private:
    PROPVARIANT value_;
};

// Asks the multimedia scheduler for "Pro Audio" priority for the thread; failure is harmless.
class MmThreadCharacteristics {
public:
    MmThreadCharacteristics() {
        DWORD task_index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    }
    ~MmThreadCharacteristics() {
        if (handle_ != nullptr) AvRevertMmThreadCharacteristics(handle_);
    }
    MmThreadCharacteristics(const MmThreadCharacteristics&) = delete;
    MmThreadCharacteristics& operator=(const MmThreadCharacteristics&) = delete;

private:
    HANDLE handle_ = nullptr;
};

std::string friendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return "(unnamed device)";
    PropVariantGuard value;
    if (FAILED(properties->GetValue(PKEY_Device_FriendlyName, value.get())) || value.value().vt != VT_LPWSTR ||
        value.value().pwszVal == nullptr) {
        return "(unnamed device)";
    }
    return wideToUtf8(value.value().pwszVal);
}

// The engine's shared-mode mix format and the device periods.
HRESULT readClientFacts(IAudioClient* client, RenderDeviceInfo& info) {
    WAVEFORMATEX* raw = nullptr;
    HRESULT hr = client->GetMixFormat(&raw);
    if (FAILED(hr)) return hr;
    const CoTaskMemPtr<WAVEFORMATEX> mix(raw);
    info.mix_rate = mix->nSamplesPerSec;
    info.mix_channels = mix->nChannels;
    info.mix_bits = mix->wBitsPerSample;
    info.mix_float =
            mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 &&
             IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix.get())->SubFormat, kSubtypeIeeeFloat));

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period = 0;
    hr = client->GetDevicePeriod(&default_period, &min_period);
    if (FAILED(hr)) return hr;
    info.default_period_ms = static_cast<double>(default_period) / kHundredNsPerMs;
    info.min_period_ms = static_cast<double>(min_period) / kHundredNsPerMs;
    return S_OK;
}

class WasapiStream final : public RenderStream {
public:
    static std::unique_ptr<WasapiStream> open(std::string& error) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = failure("creating the device enumerator", hr);
            return nullptr;
        }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            error = failure("GetDefaultAudioEndpoint", hr);
            if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) error = "no render device: " + error;
            return nullptr;
        }

        std::unique_ptr<WasapiStream> stream(new WasapiStream());
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(stream->client_.GetAddressOf()));
        if (FAILED(hr)) {
            error = failure("activating the audio client", hr);
            return nullptr;
        }

        RenderDeviceInfo facts;
        hr = readClientFacts(stream->client_.Get(), facts);
        if (FAILED(hr)) {
            error = failure("reading the mix format", hr);
            return nullptr;
        }

        // Our own format; AUTOCONVERTPCM lets the audio engine convert it to the device's mix format.
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = kAudioOutputChannels;
        format.Format.nSamplesPerSec = kAudioSampleRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * format.Format.wBitsPerSample / 8);
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = kStereoMask;
        format.SubFormat = kSubtypeIeeeFloat;

        constexpr DWORD kFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = stream->client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kFlags,
                                         static_cast<REFERENCE_TIME>(kBufferDuration.count() * kHundredNsPerMs), 0,
                                         &format.Format, nullptr);
        if (FAILED(hr)) {
            error = failure("IAudioClient::Initialize", hr);
            return nullptr;
        }

        stream->event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!stream->event_) {
            error = failure("creating the buffer event", HRESULT_FROM_WIN32(GetLastError()));
            return nullptr;
        }
        hr = stream->client_->SetEventHandle(stream->event_.get());
        if (FAILED(hr)) {
            error = failure("SetEventHandle", hr);
            return nullptr;
        }
        hr = stream->client_->GetBufferSize(&stream->buffer_frames_);
        if (FAILED(hr)) {
            error = failure("GetBufferSize", hr);
            return nullptr;
        }
        hr = stream->client_->GetService(IID_PPV_ARGS(&stream->render_));
        if (FAILED(hr)) {
            error = failure("getting the render client", hr);
            return nullptr;
        }

        stream->period_frames_ = static_cast<UINT32>(facts.default_period_ms * kAudioSampleRate / 1000.0);
        stream->description_ = std::format("{}, mix {} Hz {} ch, buffer {:.1f} ms, period {:.1f} ms",
                                           friendlyName(device.Get()), facts.mix_rate, facts.mix_channels,
                                           1000.0 * stream->buffer_frames_ / kAudioSampleRate, facts.default_period_ms);
        return stream;
    }

    ~WasapiStream() override {
        if (started_) client_->Stop();
    }

    std::string description() const override { return description_; }
    std::uint32_t bufferFrames() const override { return buffer_frames_; }
    std::uint32_t periodFrames() const override { return period_frames_; }

    bool waitForBuffer(std::chrono::milliseconds timeout) override {
        return WaitForSingleObject(event_.get(), static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
    }

    RenderStatus pendingFrames(std::uint32_t& frames) override {
        UINT32 padding = 0;
        const HRESULT hr = client_->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr)) frames = padding;
        return hr;
    }

    RenderStatus write(std::span<const float> samples) override {
        const UINT32 frames = static_cast<UINT32>(samples.size() / kAudioOutputChannels);
        if (frames == 0) return S_OK;
        BYTE* data = nullptr;
        HRESULT hr = render_->GetBuffer(frames, &data);
        if (FAILED(hr)) return hr;
        std::memcpy(data, samples.data(), std::size_t{frames} * kAudioOutputChannels * sizeof(float));
        return render_->ReleaseBuffer(frames, 0);
    }

    RenderStatus start() override {
        const HRESULT hr = client_->Start();
        if (SUCCEEDED(hr)) started_ = true;
        return hr;
    }

private:
    WasapiStream() = default;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    UniqueHandle event_;
    UINT32 buffer_frames_ = 0;
    UINT32 period_frames_ = 0;
    std::string description_;
    bool started_ = false;
};

// State shared by the output (engine thread) and its device thread.
struct Shared {
    Shared(RenderStreamFactory stream_factory, WasapiOutputOptions output_options)
        : ring(kRingBlocks), factory(std::move(stream_factory)), options(output_options) {}

    AudioBlockRing ring;
    const RenderStreamFactory factory;
    const WasapiOutputOptions options;
    std::atomic<bool> stop{false};

    std::atomic<std::uint64_t> blocks_submitted{0};
    std::atomic<std::uint64_t> blocks_dropped{0};
    std::atomic<std::uint64_t> underrun_frames{0};
    std::atomic<std::uint64_t> frames_played{0};
    std::atomic<std::uint64_t> device_errors{0};

    // Frames the device holds as of the device thread's last pass, and whether a stream is playing.
    // The engine thread reads them (the IAudioClient stays on the device thread).
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
                logWarn("dal: audio: wasapi: device lost ({}); retrying", hresultText(status));
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
                logInfo("dal: audio: wasapi: reopened {}", description);
            } else {
                logDebug("dal: audio: wasapi: reopen failed: {}", reopen_error);
            }
        }
    }

private:
    bool stopping() const { return shared_.stop.load(std::memory_order_relaxed); }

    void publishOpen(bool ok, std::string error, std::string description) {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        shared_.open_done = true;
        shared_.open_ok = ok;
        shared_.error = std::move(error);
        shared_.description = std::move(description);
        shared_.changed.notify_all();
    }

    // Plays until `stream` fails (returns the failing status) or a stop is requested (returns S_OK).
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
            if (stopping()) return S_OK;
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
        return S_OK;
    }

    // Writes what the ring holds into the free part of the device buffer. `padding_after` receives the
    // frames the device holds afterwards.
    RenderStatus fill(RenderStream& stream, std::uint32_t buffer_frames, std::uint32_t pending,
                      std::uint32_t& padding_after) {
        padding_after = pending;
        const std::size_t free_frames = pending >= buffer_frames ? 0 : buffer_frames - pending;
        const std::size_t wanted = std::min(free_frames, shared_.ring.queuedFrames());
        if (wanted == 0) return S_OK;
        const std::size_t got = shared_.ring.pop(scratch_, wanted);
        if (got == 0) return S_OK;
        const RenderStatus status = stream.write(std::span<const float>(scratch_.data(), got * kAudioOutputChannels));
        if (status < 0) return status;
        shared_.frames_played.fetch_add(got, std::memory_order_relaxed);
        padding_after = pending + static_cast<std::uint32_t>(got);
        return S_OK;
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
        const ComApartment com;
        const MmThreadCharacteristics mmcss;
        DevicePump(*shared).run();
    } catch (const std::exception& error) {
        logWarn("dal: audio: wasapi: device thread failed: {}", error.what());
    } catch (...) {
        logWarn("dal: audio: wasapi: device thread failed");
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

class WasapiOutput final : public AudioOutput {
public:
    WasapiOutput(RenderStreamFactory factory, WasapiOutputOptions options)
        : shared_(std::make_shared<Shared>(std::move(factory), options)) {}

    ~WasapiOutput() override {
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

    std::string_view name() const override { return "wasapi"; }

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

RenderStreamFactory makeWasapiStreamFactory() {
    return [](std::string& error) -> std::unique_ptr<RenderStream> { return WasapiStream::open(error); };
}

std::unique_ptr<AudioOutput> makeWasapiOutput() {
    return std::make_unique<WasapiOutput>(makeWasapiStreamFactory(), WasapiOutputOptions{});
}

std::unique_ptr<AudioOutput> makeWasapiOutputForTest(RenderStreamFactory factory, WasapiOutputOptions options) {
    return std::make_unique<WasapiOutput>(std::move(factory), options);
}

std::expected<std::vector<RenderDeviceInfo>, std::string> listRenderDevices() {
    const ComApartment com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return std::unexpected(failure("creating the device enumerator", hr));

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return std::unexpected(failure("EnumAudioEndpoints", hr));
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) return std::unexpected(failure("IMMDeviceCollection::GetCount", hr));

    std::wstring default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) {
            const CoTaskMemPtr<WCHAR> owned(id);
            default_id = id;
        }
    }

    std::vector<RenderDeviceInfo> devices;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        RenderDeviceInfo info;
        info.name = friendlyName(device.Get());
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            const CoTaskMemPtr<WCHAR> owned(id);
            info.is_default = !default_id.empty() && default_id == id;
        }
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(client.GetAddressOf())))) {
            readClientFacts(client.Get(), info);
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

}  // namespace dal

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

#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tox2 {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::chrono::milliseconds kBufferDuration{40};
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

// What the device thread holds for its lifetime: the COM apartment first, then the scheduler class
// (members are destroyed in reverse order).
struct DeviceThreadScope {
    ComApartment com;
    MmThreadCharacteristics mmcss;
};

std::shared_ptr<void> enterDeviceThread() { return std::make_shared<DeviceThreadScope>(); }

std::unique_ptr<AudioOutput> makeOutput(RenderStreamFactory factory, WasapiOutputOptions options) {
    StreamOutputOptions output_options;
    output_options.name = "wasapi";
    output_options.open_timeout = options.open_timeout;
    output_options.reopen_interval = options.reopen_interval;
    output_options.enter_device_thread = enterDeviceThread;
    output_options.status_text = [](RenderStatus status) { return hresultText(static_cast<HRESULT>(status)); };
    return makeStreamOutput(std::move(factory), std::move(output_options));
}

}  // namespace

RenderStreamFactory makeWasapiStreamFactory() {
    return [](std::string& error) -> std::unique_ptr<RenderStream> { return WasapiStream::open(error); };
}

std::unique_ptr<AudioOutput> makeWasapiOutput() { return makeOutput(makeWasapiStreamFactory(), WasapiOutputOptions{}); }

std::unique_ptr<AudioOutput> makeWasapiOutputForTest(RenderStreamFactory factory, WasapiOutputOptions options) {
    return makeOutput(std::move(factory), options);
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

}  // namespace tox2

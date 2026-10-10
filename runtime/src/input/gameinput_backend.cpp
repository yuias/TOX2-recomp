#include "input/gameinput_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// GameInput.h comes from the pinned package, which the build puts before the Windows SDK's older
// v0 header of the same name.
#include <GameInput.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "input/gamepad_probe.h"

static_assert(GAMEINPUT_API_VERSION == 3, "the pinned v3 GameInput.h must win over the SDK's header");

namespace tox2 {

namespace {

using Microsoft::WRL::ComPtr;
using namespace GameInput::v3;

std::string hresultText(HRESULT hr) { return std::format("{:#010x}", static_cast<std::uint32_t>(hr)); }

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// File name (without directory) of the module that contains `address`; empty if there is none.
std::string moduleFileNameFromAddress(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0) return {};
        if (size < path.size()) {
            path.resize(size);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    return wideToUtf8(slash == std::wstring::npos ? std::wstring_view(path)
                                                  : std::wstring_view(path).substr(slash + 1));
}

std::string failure(std::string_view step, HRESULT hr) {
    std::string text = std::format("{} failed ({})", step, hresultText(hr));
    if (hr == E_NOINTERFACE) {
        // The in-box runtime of older Windows builds only implements the earlier API versions.
        text += ": the GameInput runtime found does not provide API version 3";
        for (const wchar_t* name : {L"GameInputRedist.dll", L"GameInput.dll"}) {
            if (HMODULE module = GetModuleHandleW(name)) {
                text += std::format(" (loaded: {})", moduleFileNameFromAddress(module));
                break;
            }
        }
        text += "; install GameInputRedist.msi from the Microsoft.GameInput package";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) {
        text += ": no GameInput runtime found; install GameInputRedist.msi from the Microsoft.GameInput package";
    }
    return text;
}

struct ButtonMapEntry {
    GameInputGamepadButtons flag;
    GamepadButton button;
};
constexpr ButtonMapEntry kButtonMap[] = {
        {GameInputGamepadMenu, GamepadButton::Menu},
        {GameInputGamepadView, GamepadButton::View},
        {GameInputGamepadA, GamepadButton::A},
        {GameInputGamepadB, GamepadButton::B},
        {GameInputGamepadX, GamepadButton::X},
        {GameInputGamepadY, GamepadButton::Y},
        {GameInputGamepadDPadUp, GamepadButton::DPadUp},
        {GameInputGamepadDPadDown, GamepadButton::DPadDown},
        {GameInputGamepadDPadLeft, GamepadButton::DPadLeft},
        {GameInputGamepadDPadRight, GamepadButton::DPadRight},
        {GameInputGamepadLeftShoulder, GamepadButton::LeftShoulder},
        {GameInputGamepadRightShoulder, GamepadButton::RightShoulder},
        {GameInputGamepadLeftThumbstick, GamepadButton::LeftThumbstick},
        {GameInputGamepadRightThumbstick, GamepadButton::RightThumbstick},
};
static_assert(std::size(kButtonMap) == static_cast<std::size_t>(GamepadButton::Count));

GamepadButtons mapButtons(GameInputGamepadButtons buttons) {
    GamepadButtons mapped = 0;
    for (const ButtonMapEntry& entry : kButtonMap) {
        if ((buttons & entry.flag) != 0) mapped |= gamepadButtonBit(entry.button);
    }
    return mapped;
}

std::uint32_t mapKinds(GameInputKind supported) {
    std::uint32_t kinds = 0;
    if ((supported & GameInputKindGamepad) != 0) kinds |= kGamepadKindGamepad;
    if ((supported & GameInputKindController) != 0) kinds |= kGamepadKindController;
    return kinds;
}

class Backend final : public GameInputBackend {
public:
    Backend(ComPtr<IGameInput> game_input, std::string description)
        : game_input_(std::move(game_input)), description_(std::move(description)) {}

    ~Backend() override {
        if (token_ != 0) {
            // Unregistering waits for a running callback, which locks mutex_: no lock may be held here.
            game_input_->StopCallback(token_);
            game_input_->UnregisterCallback(token_);
        }
    }

    // Registers the device callback; the enumeration of present devices finishes before this returns.
    HRESULT start() {
        return game_input_->RegisterDeviceCallback(nullptr, GameInputKindGamepad | GameInputKindController,
                                                   GameInputDeviceConnected, GameInputBlockingEnumeration, this,
                                                   &Backend::onDevice, &token_);
    }

    std::string description() const override { return description_; }

    std::uint64_t deviceGeneration() const override { return generation_.load(std::memory_order_acquire); }

    std::vector<GamepadDeviceInfo> devices() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<GamepadDeviceInfo> infos;
        infos.reserve(table_.size());
        for (const Entry& entry : table_) infos.push_back(entry.info);
        return infos;
    }

    std::optional<GamepadReading> read(std::uint64_t id) override {
        const ComPtr<IGameInputDevice> device = find(id, kGamepadKindGamepad);
        if (!device) return std::nullopt;
        ComPtr<IGameInputReading> reading;
        // READING_NOT_FOUND is the normal answer for a pad that has produced no reading yet.
        if (FAILED(game_input_->GetCurrentReading(GameInputKindGamepad, device.Get(), reading.GetAddressOf()))) {
            return std::nullopt;
        }
        GameInputGamepadState state{};
        if (!reading->GetGamepadState(&state)) return std::nullopt;
        GamepadReading result;
        result.buttons = mapButtons(state.buttons);
        result.left_trigger = state.leftTrigger;
        result.right_trigger = state.rightTrigger;
        result.left_x = state.leftThumbstickX;
        result.left_y = state.leftThumbstickY;
        result.right_x = state.rightThumbstickX;
        result.right_y = state.rightThumbstickY;
        return result;
    }

    void setRumble(std::uint64_t id, const GamepadRumble& rumble) override {
        const ComPtr<IGameInputDevice> device = find(id, 0);
        if (!device) return;
        // Stopping passes zeroed params: the installed 3.5 runtime crashes on the nullptr the header
        // allows.
        GameInputRumbleParams params{};
        // std::clamp passes NaN through, and the motors must never get it.
        const auto motor = [](float v) { return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f); };
        params.lowFrequency = motor(rumble.low_frequency);
        params.highFrequency = motor(rumble.high_frequency);
        device->SetRumbleState(&params);
    }

    std::optional<GameInputRawReading> readRaw(std::uint64_t id) override {
        const ComPtr<IGameInputDevice> device = find(id, kGamepadKindController);
        if (!device) return std::nullopt;
        ComPtr<IGameInputReading> reading;
        if (FAILED(game_input_->GetCurrentReading(GameInputKindController, device.Get(), reading.GetAddressOf()))) {
            return std::nullopt;
        }
        GameInputRawReading raw;
        const std::uint32_t axis_count = reading->GetControllerAxisCount();
        raw.axes.assign(axis_count, 0.0f);
        if (axis_count != 0) reading->GetControllerAxisState(axis_count, raw.axes.data());

        const std::uint32_t button_count = reading->GetControllerButtonCount();
        if (button_count != 0) {
            // vector<bool> has no contiguous storage.
            const auto pressed = std::make_unique<bool[]>(button_count);
            reading->GetControllerButtonState(button_count, pressed.get());
            raw.buttons.assign(pressed.get(), pressed.get() + button_count);
        }

        const std::uint32_t switch_count = reading->GetControllerSwitchCount();
        if (switch_count != 0) {
            std::vector<GameInputSwitchPosition> positions(switch_count, GameInputSwitchCenter);
            reading->GetControllerSwitchState(switch_count, positions.data());
            for (const GameInputSwitchPosition position : positions) {
                raw.switches.push_back(static_cast<std::uint8_t>(position));
            }
        }
        return raw;
    }

private:
    struct Entry {
        GamepadDeviceInfo info;
        ComPtr<IGameInputDevice> device;
    };

    // Runs on a GameInput thread. `device` is only valid during the call, so the table takes a reference.
    static void CALLBACK onDevice(GameInputCallbackToken, void* context, IGameInputDevice* device, std::uint64_t,
                                  GameInputDeviceStatus current, GameInputDeviceStatus) {
        auto* self = static_cast<Backend*>(context);
        if ((current & GameInputDeviceConnected) != 0) {
            self->addDevice(device);
        } else {
            self->removeDevice(device);
        }
    }

    void addDevice(IGameInputDevice* device) {
        const GameInputDeviceInfo* raw = nullptr;
        if (FAILED(device->GetDeviceInfo(&raw)) || raw == nullptr) return;
        GamepadDeviceInfo info;
        info.vendor_id = raw->vendorId;
        info.product_id = raw->productId;
        info.kinds = mapKinds(raw->supportedInput);
        info.rumble = (raw->supportedRumbleMotors & GameInputRumbleLowFrequency) != 0 &&
                      (raw->supportedRumbleMotors & GameInputRumbleHighFrequency) != 0;
        if (raw->pnpPath != nullptr) info.pnp_path = raw->pnpPath;
        if (raw->displayName != nullptr) info.display_name = raw->displayName;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The status callback can fire again for a device that is already listed.
            const bool known = std::ranges::any_of(table_, [&](const Entry& e) { return e.device.Get() == device; });
            if (known) return;
            info.id = next_id_++;
            table_.push_back({info, ComPtr<IGameInputDevice>(device)});
            generation_.fetch_add(1, std::memory_order_release);
        }
        // Logged outside the lock: the logger may block on its own output.
        logInfo("tox2: pad: device + {} {} rumble {} \"{}\"", formatGamepadVidPid(info), formatGamepadKinds(info.kinds),
                info.rumble ? "yes" : "no", info.display_name);
        logDebug("tox2: pad: device {} path {}", info.id, info.pnp_path);
    }

    void removeDevice(IGameInputDevice* device) {
        ComPtr<IGameInputDevice> released;  // released after the lock
        GamepadDeviceInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = std::ranges::find_if(table_, [&](const Entry& e) { return e.device.Get() == device; });
            if (it == table_.end()) return;
            released = std::move(it->device);
            info = std::move(it->info);
            table_.erase(it);
            generation_.fetch_add(1, std::memory_order_release);
        }
        logInfo("tox2: pad: device - {}", formatGamepadVidPid(info));
    }

    // A reference to the device with `id`, or null when it is gone or lacks any of `required_kinds`.
    ComPtr<IGameInputDevice> find(std::uint64_t id, std::uint32_t required_kinds) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Entry& entry : table_) {
            if (entry.info.id != id) continue;
            if ((entry.info.kinds & required_kinds) != required_kinds) return {};
            return entry.device;
        }
        return {};
    }

    ComPtr<IGameInput> game_input_;
    std::string description_;
    GameInputCallbackToken token_ = 0;
    std::mutex mutex_;
    std::vector<Entry> table_;  // arrival order
    std::uint64_t next_id_ = 1;
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace

std::expected<std::shared_ptr<GameInputBackend>, std::string> makeGameInputBackend(
        const GameInputBackendOptions& options) {
    ComPtr<IGameInput> game_input;
    if (const HRESULT hr = GameInputCreate(game_input.GetAddressOf()); FAILED(hr)) {
        return std::unexpected(failure("GameInputCreate", hr));
    }
    // The static loader's own entry point sits in our executable; the returned object's first method
    // names the module that created it (a hook that patches only some methods does not show here).
    const void* const method = (*reinterpret_cast<void* const* const*>(game_input.Get()))[0];
    std::string module = moduleFileNameFromAddress(method);
    if (module.empty()) module = "unknown module";

    // Set explicitly although the default is foreground-only. ExclusiveForegroundInput is not used: an
    // exclusive claim could take input away from Steam's own handling.
    game_input->SetFocusPolicy(options.background_input ? GameInputEnableBackgroundInput : GameInputDefaultFocusPolicy);

    auto backend =
            std::make_shared<Backend>(std::move(game_input), std::format("v{} from {}", GAMEINPUT_API_VERSION, module));
    if (const HRESULT hr = backend->start(); FAILED(hr)) {
        return std::unexpected(failure("RegisterDeviceCallback", hr));
    }
    return backend;
}

SteamContext detectSteamContext() {
    SteamContext context;
    context.overlay_loaded = GetModuleHandleW(L"GameOverlayRenderer64.dll") != nullptr;
    context.process_id = GetCurrentProcessId();
    for (const std::string_view name : kSteamEnvVars) {
        const std::wstring wide_name(name.begin(), name.end());  // the names are ASCII
        const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        if (size == 0) continue;  // unset
        std::wstring value(size, L'\0');
        const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
        // A set but empty variable reads as unset.
        if (written == 0 || written >= size) continue;
        value.resize(written);
        context.env.emplace(std::string(name), wideToUtf8(value));
    }
    return context;
}

bool processOwnsForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

}  // namespace tox2

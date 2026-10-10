// The seam between gamepad sources and the rest of the runtime. It holds no Windows or GameInput
// types, so the mapping, selection and hub code stays in dal_runtime_core and is tested with a fake
// backend; the GameInput implementation lives in dal_input_gameinput.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dal {

// GameInput's gamepad buttons in our own enum, so core code needs no Windows headers.
enum class GamepadButton : std::uint8_t {
    Menu,
    View,
    A,
    B,
    X,
    Y,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    LeftShoulder,
    RightShoulder,
    LeftThumbstick,
    RightThumbstick,
    Count
};
using GamepadButtons = std::uint16_t;  // bit (1 << GamepadButton)
static_assert(static_cast<unsigned>(GamepadButton::Count) <= 16);

constexpr GamepadButtons gamepadButtonBit(GamepadButton button) {
    return static_cast<GamepadButtons>(1u << static_cast<unsigned>(button));
}

// One reading, normalised like GameInputGamepadState: sticks -1..1 with +y up, triggers 0..1.
struct GamepadReading {
    GamepadButtons buttons = 0;
    float left_trigger = 0.0f, right_trigger = 0.0f;
    float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
    bool operator==(const GamepadReading&) const = default;
};

// Motor strengths 0..1 (the two body motors; trigger motors are not used).
struct GamepadRumble {
    float low_frequency = 0.0f, high_frequency = 0.0f;
    bool operator==(const GamepadRumble&) const = default;
};

inline constexpr std::uint32_t kGamepadKindGamepad = 1u << 0;     // readable as a gamepad
inline constexpr std::uint32_t kGamepadKindController = 1u << 1;  // generic controller

struct GamepadDeviceInfo {
    std::uint64_t id = 0;  // assigned by the backend; never reused within a run
    std::uint16_t vendor_id = 0, product_id = 0;
    std::uint32_t kinds = 0;   // kGamepadKind* bits
    bool rumble = false;       // has the low/high frequency motors
    std::string pnp_path;      // UTF-8; empty if the backend has none
    std::string display_name;  // UTF-8; may be empty
};

// What the process knows about Steam without the Steamworks SDK.
inline constexpr std::array<std::string_view, 7> kSteamEnvVars = {"SteamGameId",
                                                                  "SteamOverlayGameId",
                                                                  "SteamAppId",
                                                                  "EnableConfiguratorSupport",
                                                                  "SteamVirtualGamepadInfo",
                                                                  "SDL_GAMECONTROLLER_IGNORE_DEVICES",
                                                                  "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD"};

struct SteamContext {
    bool overlay_loaded = false;  // GameOverlayRenderer64.dll mapped when the backend was created
    std::uint32_t process_id = 0;
    std::map<std::string, std::string, std::less<>> env;  // the variables of kSteamEnvVars that are set
};

// A source of gamepads. devices/read/setRumble are called from one thread at a time (the gamepad
// thread or a tool); deviceGeneration from any thread. Implementations never call back into the
// caller and never touch guest state.
class GamepadBackend {
public:
    virtual ~GamepadBackend() = default;
    virtual std::string description() const = 0;           // for the start line
    virtual std::uint64_t deviceGeneration() const = 0;    // grows on every arrival or removal
    virtual std::vector<GamepadDeviceInfo> devices() = 0;  // connected devices, arrival order
    // Current state of a device readable as a gamepad; nullopt when gone or not readable.
    virtual std::optional<GamepadReading> read(std::uint64_t id) = 0;
    virtual void setRumble(std::uint64_t id, const GamepadRumble& rumble) = 0;
};

}  // namespace dal

// GameInput v3 as a GamepadBackend. The header holds no Windows or GameInput types so that tools and
// tests can include it freely; GameInput itself is resolved at run time by the package's static
// loader (no GameInput DLL appears in any import table).

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "input/gamepad_backend.h"

namespace tox2 {

struct GameInputBackendOptions {
    // false: foreground-only (the game); true: background input enabled (console tools, whose
    // console window belongs to another process).
    bool background_input = false;
};

// Controller-kind reading of a device in GameInput's generic layout, for devices that do not report
// the gamepad kind.
struct GameInputRawReading {
    std::vector<float> axes;
    std::vector<std::uint8_t> buttons;   // 0 or 1 per button index
    std::vector<std::uint8_t> switches;  // GameInputSwitchPosition per switch: 0 centre, 1 up, clockwise to 8
};

class GameInputBackend : public GamepadBackend {
public:
    // nullopt when the device is gone, has no controller kind or has no reading.
    virtual std::optional<GameInputRawReading> readRaw(std::uint64_t id) = 0;
};

// Initialises GameInput v3, sets the focus policy explicitly, registers a device callback for
// gamepad and controller kinds. The error names the step and the HRESULT.
std::expected<std::shared_ptr<GameInputBackend>, std::string> makeGameInputBackend(
        const GameInputBackendOptions& options);

// GameOverlayRenderer64.dll mapped, the Steam environment variables and the process id.
SteamContext detectSteamContext();

// Whether the foreground window belongs to this process. The hub releases the gamepad's buttons while
// it does not, whatever GameInput reports for a background process.
bool processOwnsForegroundWindow();

}  // namespace tox2

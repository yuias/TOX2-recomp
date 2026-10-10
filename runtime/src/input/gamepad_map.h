// Gamepad readings to PS3 pad input: button mapping, stick deadzone, trigger threshold, merging of
// several pads and the rumble conversion. Pure functions; the hub applies them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "input/gamepad_backend.h"
#include "input/pad_buttons.h"

namespace dal {

// XInput's XINPUT_GAMEPAD_LEFT/RIGHT_THUMB_DEADZONE (7849 and 8689 of 32767) and
// XINPUT_GAMEPAD_TRIGGER_THRESHOLD (30 of 255), the usual values for Xbox-family pads.
inline constexpr float kLeftStickDeadzone = 7849.0f / 32767.0f;
inline constexpr float kRightStickDeadzone = 8689.0f / 32767.0f;
inline constexpr std::uint8_t kTriggerPressThreshold = 30;

struct GamepadMapping {
    std::array<PadButton, static_cast<std::size_t>(GamepadButton::Count)> buttons;
};

// Positional: A = Cross, B = Circle, X = Square, Y = Triangle. The triggers are not in the table;
// they map to L2/R2 through the pressure threshold.
const GamepadMapping& defaultGamepadMapping();

struct MappedPad {
    PadButtons buttons = 0;
    PadAnalog analog;
    bool operator==(const MappedPad&) const = default;
};

// Radial deadzone with rescaling: radius <= deadzone -> (0, 0); otherwise the radius maps linearly
// from (deadzone, 1] to (0, 1], direction kept; radius > 1 is clamped to 1.
std::pair<float, float> applyRadialDeadzone(float x, float y, float deadzone);

// -1..1 -> 0..255 as lround((v + 1) * 127.5) clamped; exactly 0 -> 0x80.
std::uint8_t stickByte(float value);

// Buttons by the mapping; sticks through the deadzone (y inverted: GameInput +y is up, CellPadData 0
// is up); triggers: pressure lround(t * 255), L2/R2 pressed when pressure >= kTriggerPressThreshold
// and the pressure word is 0 below it.
MappedPad mapGamepadReading(const GamepadReading& reading, const GamepadMapping& mapping = defaultGamepadMapping());

// Several used pads on port 0: buttons OR; each stick axis the value farthest from 0x80 (ties: the
// first); triggers the maximum.
MappedPad mergeMappedPads(std::span<const MappedPad> pads);

// cellPad motors -> GameInput: large 0..255 -> low_frequency large / 255; the small motor is on/off
// on a DualShock 3 -> high_frequency 1.0 when non-zero, else 0.
GamepadRumble rumbleFromCellPad(std::uint8_t small_motor, std::uint8_t large_motor);

}  // namespace dal

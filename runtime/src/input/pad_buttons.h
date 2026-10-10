// Pad buttons as host input sees them, and their layout in the CellPadData words.
//
// Bit and word positions follow RPCS3 `rpcs3/Emu/Io/pad_types.h`: `CELL_PAD_CTRL_*` for the digital
// bits of CELL_PAD_BTN_OFFSET_DIGITAL1 / DIGITAL2 and `CELL_PAD_BTN_OFFSET_PRESS_*` for the pressure
// words. Host providers (keyboard, script, gamepad) share this model.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "hle/hle.h"

namespace tox2 {

// The order is the bit order: bit i of a PadButtons mask is bit i of button[2] for i < 8 and bit
// (i - 8) of button[3] for i >= 8, so a mask converts with two shifts.
enum class PadButton : std::uint8_t {
    Select,
    L3,
    R3,
    Start,
    Up,
    Right,
    Down,
    Left,  // button[2] bits 0..7
    L2,
    R2,
    L1,
    R1,
    Triangle,
    Circle,
    Cross,
    Square,  // button[3] bits 0..7
    Count
};

inline constexpr std::size_t kPadButtonTotal = static_cast<std::size_t>(PadButton::Count);

using PadButtons = std::uint16_t;  // bit (1 << PadButton)

constexpr PadButtons padButtonBit(PadButton b) { return static_cast<PadButtons>(1u << static_cast<unsigned>(b)); }

// Index in PadPortState::button of the button's pressure word, 0 for buttons without one (Select,
// L3, R3, Start).
std::size_t padPressureWord(PadButton b);

// neutralPadState() with the digital words set from `buttons` and the pressure word of every
// pressed button at 0xFF (keys are digital; a real pad reports 0xFF fully pressed). Sticks,
// sensors, capability and type stay as neutral, so an empty mask equals neutralPadState() exactly.
PadPortState padStateFromButtons(PadButtons buttons);

// The analog part of a pad in CellPadData units: sticks 0 = left/up, 0xFF = right/down, 0x80 centre;
// trigger pressure 0..0xFF.
struct PadAnalog {
    std::uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
    std::uint8_t l2 = 0, r2 = 0;
    bool operator==(const PadAnalog&) const = default;
};

// padStateFromButtons(buttons) with the stick words 4..7 from `analog`. The pressure word of a
// pressed L2/R2 is analog.l2/r2 when that button is in `analog_pressure`, otherwise 0xFF. An empty
// mask with a default PadAnalog equals neutralPadState() exactly.
PadPortState padStateFromInput(PadButtons buttons, const PadAnalog& analog, PadButtons analog_pressure);

// Lower-case names used by the pad script: select l3 r3 start up right down left l2 r2 l1 r1
// triangle circle cross square. padButtonName returns an empty view for PadButton::Count.
std::string_view padButtonName(PadButton b);
std::optional<PadButton> padButtonFromName(std::string_view name);

}  // namespace tox2

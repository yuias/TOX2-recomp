#include "input/pad_buttons.h"

#include <array>

namespace dal {
namespace {

constexpr std::uint16_t kPressedPressure = 0xFF;

constexpr std::array<std::string_view, kPadButtonTotal> kNames = {
        "select", "l3", "r3", "start", "up",       "right",  "down",  "left",
        "l2",     "r2", "l1", "r1",    "triangle", "circle", "cross", "square",
};

}  // namespace

std::size_t padPressureWord(PadButton b) {
    switch (b) {
        case PadButton::Right: return kPadButtonPressureFirst + 0;
        case PadButton::Left: return kPadButtonPressureFirst + 1;
        case PadButton::Up: return kPadButtonPressureFirst + 2;
        case PadButton::Down: return kPadButtonPressureFirst + 3;
        case PadButton::Triangle: return kPadButtonPressureFirst + 4;
        case PadButton::Circle: return kPadButtonPressureFirst + 5;
        case PadButton::Cross: return kPadButtonPressureFirst + 6;
        case PadButton::Square: return kPadButtonPressureFirst + 7;
        case PadButton::L1: return kPadButtonPressureFirst + 8;
        case PadButton::R1: return kPadButtonPressureFirst + 9;
        case PadButton::L2: return kPadButtonPressureFirst + 10;
        case PadButton::R2: return kPadButtonPressureFirst + 11;
        default: return 0;
    }
}

PadPortState padStateFromButtons(PadButtons buttons) {
    PadPortState state = neutralPadState();
    state.button[kPadButtonDigital1] = static_cast<std::uint16_t>(buttons & 0xFF);
    state.button[kPadButtonDigital2] = static_cast<std::uint16_t>(buttons >> 8);
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        const PadButton b = static_cast<PadButton>(i);
        if ((buttons & padButtonBit(b)) == 0) continue;
        const std::size_t word = padPressureWord(b);
        if (word != 0) state.button[word] = kPressedPressure;
    }
    return state;
}

PadPortState padStateFromInput(PadButtons buttons, const PadAnalog& analog, PadButtons analog_pressure) {
    PadPortState state = padStateFromButtons(buttons);
    state.button[kPadButtonAnalogRightX] = analog.right_x;
    state.button[kPadButtonAnalogRightY] = analog.right_y;
    state.button[kPadButtonAnalogLeftX] = analog.left_x;
    state.button[kPadButtonAnalogLeftY] = analog.left_y;
    // A button that is not pressed keeps pressure 0 whatever the mask says.
    const PadButtons analog_pressed = buttons & analog_pressure;
    if ((analog_pressed & padButtonBit(PadButton::L2)) != 0) {
        state.button[padPressureWord(PadButton::L2)] = analog.l2;
    }
    if ((analog_pressed & padButtonBit(PadButton::R2)) != 0) {
        state.button[padPressureWord(PadButton::R2)] = analog.r2;
    }
    return state;
}

std::string_view padButtonName(PadButton b) {
    const std::size_t index = static_cast<std::size_t>(b);
    return index < kNames.size() ? kNames[index] : std::string_view{};
}

std::optional<PadButton> padButtonFromName(std::string_view name) {
    for (std::size_t i = 0; i < kNames.size(); i++) {
        if (kNames[i] == name) return static_cast<PadButton>(i);
    }
    return std::nullopt;
}

}  // namespace dal

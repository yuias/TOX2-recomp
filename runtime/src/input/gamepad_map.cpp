#include "input/gamepad_map.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tox2 {

namespace {

constexpr GamepadMapping makeDefaultMapping() {
    GamepadMapping mapping{};
    auto set = [&](GamepadButton from, PadButton to) { mapping.buttons[static_cast<std::size_t>(from)] = to; };
    set(GamepadButton::Menu, PadButton::Start);
    set(GamepadButton::View, PadButton::Select);
    set(GamepadButton::A, PadButton::Cross);
    set(GamepadButton::B, PadButton::Circle);
    set(GamepadButton::X, PadButton::Square);
    set(GamepadButton::Y, PadButton::Triangle);
    set(GamepadButton::DPadUp, PadButton::Up);
    set(GamepadButton::DPadDown, PadButton::Down);
    set(GamepadButton::DPadLeft, PadButton::Left);
    set(GamepadButton::DPadRight, PadButton::Right);
    set(GamepadButton::LeftShoulder, PadButton::L1);
    set(GamepadButton::RightShoulder, PadButton::R1);
    set(GamepadButton::LeftThumbstick, PadButton::L3);
    set(GamepadButton::RightThumbstick, PadButton::R3);
    return mapping;
}

constexpr GamepadMapping kDefaultMapping = makeDefaultMapping();

// Deadzone, then the stick bytes; y is inverted because CellPadData 0 is up.
void mapStick(float x, float y, float deadzone, std::uint8_t& out_x, std::uint8_t& out_y) {
    const auto [dx, dy] = applyRadialDeadzone(x, y, deadzone);
    out_x = stickByte(dx);
    out_y = stickByte(-dy);
}

// 0 below the press threshold, so a lightly touched trigger reports neither the button nor pressure.
std::uint8_t triggerPressure(float value) {
    if (!(value > 0.0f)) return 0;  // also rejects NaN
    const long pressure = std::lround(std::min(value, 1.0f) * 255.0f);
    return pressure >= kTriggerPressThreshold ? static_cast<std::uint8_t>(pressure) : 0;
}

std::uint8_t farthestFromCentre(std::uint8_t a, std::uint8_t b) {
    return std::abs(static_cast<int>(b) - 0x80) > std::abs(static_cast<int>(a) - 0x80) ? b : a;
}

}  // namespace

const GamepadMapping& defaultGamepadMapping() { return kDefaultMapping; }

std::pair<float, float> applyRadialDeadzone(float x, float y, float deadzone) {
    const float radius = std::hypot(x, y);
    if (!(radius > deadzone)) return {0.0f, 0.0f};  // also rejects NaN
    const float clamped = std::min(radius, 1.0f);
    const float scale = (clamped - deadzone) / (1.0f - deadzone) / radius;
    return {x * scale, y * scale};
}

std::uint8_t stickByte(float value) {
    if (value == 0.0f) return 0x80;
    if (!(value > -1.0f)) return 0;  // also rejects NaN
    if (value >= 1.0f) return 0xFF;
    return static_cast<std::uint8_t>(std::lround((value + 1.0f) * 127.5f));
}

MappedPad mapGamepadReading(const GamepadReading& reading, const GamepadMapping& mapping) {
    MappedPad pad;
    for (std::size_t i = 0; i < mapping.buttons.size(); ++i) {
        if (reading.buttons & gamepadButtonBit(static_cast<GamepadButton>(i)))
            pad.buttons |= padButtonBit(mapping.buttons[i]);
    }
    mapStick(reading.left_x, reading.left_y, kLeftStickDeadzone, pad.analog.left_x, pad.analog.left_y);
    mapStick(reading.right_x, reading.right_y, kRightStickDeadzone, pad.analog.right_x, pad.analog.right_y);
    pad.analog.l2 = triggerPressure(reading.left_trigger);
    pad.analog.r2 = triggerPressure(reading.right_trigger);
    if (pad.analog.l2 != 0) pad.buttons |= padButtonBit(PadButton::L2);
    if (pad.analog.r2 != 0) pad.buttons |= padButtonBit(PadButton::R2);
    return pad;
}

MappedPad mergeMappedPads(std::span<const MappedPad> pads) {
    if (pads.empty()) return {};
    MappedPad merged = pads.front();
    for (const MappedPad& pad : pads.subspan(1)) {
        merged.buttons |= pad.buttons;
        merged.analog.left_x = farthestFromCentre(merged.analog.left_x, pad.analog.left_x);
        merged.analog.left_y = farthestFromCentre(merged.analog.left_y, pad.analog.left_y);
        merged.analog.right_x = farthestFromCentre(merged.analog.right_x, pad.analog.right_x);
        merged.analog.right_y = farthestFromCentre(merged.analog.right_y, pad.analog.right_y);
        merged.analog.l2 = std::max(merged.analog.l2, pad.analog.l2);
        merged.analog.r2 = std::max(merged.analog.r2, pad.analog.r2);
    }
    return merged;
}

GamepadRumble rumbleFromCellPad(std::uint8_t small_motor, std::uint8_t large_motor) {
    GamepadRumble rumble;
    rumble.low_frequency = static_cast<float>(large_motor) / 255.0f;
    rumble.high_frequency = small_motor != 0 ? 1.0f : 0.0f;
    return rumble;
}

}  // namespace tox2

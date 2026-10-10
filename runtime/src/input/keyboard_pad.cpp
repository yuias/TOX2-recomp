#include "input/keyboard_pad.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace tox2 {
namespace {

// A time of zero is a valid steady-clock reading in tests, so "no press" needs its own value.
constexpr std::int64_t kNoPress = std::numeric_limits<std::int64_t>::min();

constexpr std::size_t kSourceCount = static_cast<std::size_t>(InputSource::Count);

struct Axis {
    PadButton a;
    PadButton b;
};
constexpr Axis kAxes[] = {{PadButton::Up, PadButton::Down}, {PadButton::Left, PadButton::Right}};

std::int64_t toNanoseconds(KeyboardPadState::Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

constexpr PadButtons kTriggers = padButtonBit(PadButton::L2) | padButtonBit(PadButton::R2);

std::uint64_t packAnalog(const PadAnalog& a) {
    return static_cast<std::uint64_t>(a.left_x) | static_cast<std::uint64_t>(a.left_y) << 8 |
           static_cast<std::uint64_t>(a.right_x) << 16 | static_cast<std::uint64_t>(a.right_y) << 24 |
           static_cast<std::uint64_t>(a.l2) << 32 | static_cast<std::uint64_t>(a.r2) << 40;
}

PadAnalog unpackAnalog(std::uint64_t bits) {
    const auto byte = [bits](unsigned shift) { return static_cast<std::uint8_t>(bits >> shift); };
    PadAnalog a;
    a.left_x = byte(0);
    a.left_y = byte(8);
    a.right_x = byte(16);
    a.right_y = byte(24);
    a.l2 = byte(32);
    a.r2 = byte(40);
    return a;
}

class KeyboardPadProvider final : public PadProvider {
public:
    KeyboardPadProvider(std::shared_ptr<KeyboardPadState> state,
                        std::function<KeyboardPadState::Clock::time_point()> clock, PadProviderOptions options)
        : state_(std::move(state)), clock_(std::move(clock)), options_(options) {
        if (!clock_) clock_ = [] { return KeyboardPadState::Clock::now(); };
    }

    PadPortState poll(std::uint32_t port) override {
        if (port != 0) return {};
        if (!options_.always_connected && !state_->gamepadConnected()) return {};
        const PadButtons buttons = state_->reported(clock_());
        // Analog pressure only where the gamepad alone holds the trigger: a key press, and the tail of a
        // released trigger, have no analog value and read as fully pressed.
        const PadButtons analog_pressure =
                buttons & state_->held(InputSource::Gamepad) &
                static_cast<PadButtons>(~(state_->held(InputSource::Keyboard) | state_->held(InputSource::Script))) &
                kTriggers;
        return padStateFromInput(buttons, state_->analog(), analog_pressure);
    }

    void setVibration(std::uint32_t port, std::uint8_t small_motor, std::uint8_t large_motor) override {
        if (port == 0) state_->setVibration(small_motor, large_motor);
    }

private:
    std::shared_ptr<KeyboardPadState> state_;
    std::function<KeyboardPadState::Clock::time_point()> clock_;
    PadProviderOptions options_;
};

}  // namespace

std::optional<PadButton> padButtonForScanCode(std::uint32_t scan_code) {
    switch (scan_code) {
        case 0xE048: return PadButton::Up;
        case 0xE050: return PadButton::Down;
        case 0xE04B: return PadButton::Left;
        case 0xE04D: return PadButton::Right;
        case 0x2C: return PadButton::Cross;     // Z
        case 0x2D: return PadButton::Circle;    // X
        case 0x1F: return PadButton::Square;    // S
        case 0x20: return PadButton::Triangle;  // D
        case 0x11: return PadButton::L1;        // W
        case 0x12: return PadButton::L2;        // E
        case 0x13: return PadButton::R1;        // R
        case 0x16: return PadButton::R2;        // U
        case 0x02: return PadButton::L3;        // 1
        case 0x04: return PadButton::R3;        // 3
        case 0x2F: return PadButton::Start;     // V
        case 0x2E: return PadButton::Select;    // C
        default: return std::nullopt;
    }
}

KeyboardPadState::KeyboardPadState() : analog_(packAnalog(PadAnalog{})) {
    for (auto& times : pressed_at_ns_) {
        for (auto& time : times) time.store(kNoPress, std::memory_order_relaxed);
    }
}

bool KeyboardPadState::keyEvent(std::uint32_t scan_code, bool down, bool repeat, Clock::time_point now) {
    const std::optional<PadButton> button = padButtonForScanCode(scan_code);
    if (!button) return false;
    const PadButtons bit = padButtonBit(*button);
    if (down) {
        // Level-based state: a repeat changes nothing, and must not refresh the press time.
        const bool already_down =
                (held_[static_cast<std::size_t>(InputSource::Keyboard)].load(std::memory_order_relaxed) & bit) != 0;
        if (!repeat && !already_down) setButtons(InputSource::Keyboard, bit, true, now);
    } else {
        setButtons(InputSource::Keyboard, bit, false, now);
    }
    return true;
}

void KeyboardPadState::setButtons(InputSource source, PadButtons buttons, bool down, Clock::time_point now) {
    std::atomic<PadButtons>& mask = held_[static_cast<std::size_t>(source)];
    if (!down) {
        mask.fetch_and(static_cast<PadButtons>(~buttons), std::memory_order_relaxed);
        return;
    }
    const std::int64_t now_ns = toNanoseconds(now);
    auto& times = pressed_at_ns_[static_cast<std::size_t>(source)];
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        if ((buttons & padButtonBit(static_cast<PadButton>(i))) != 0) {
            times[i].store(now_ns, std::memory_order_relaxed);
        }
    }
    mask.fetch_or(buttons, std::memory_order_relaxed);
}

void KeyboardPadState::releaseAll(InputSource source) {
    const PadButtons released = held_[static_cast<std::size_t>(source)].exchange(0, std::memory_order_relaxed);
    auto& times = pressed_at_ns_[static_cast<std::size_t>(source)];
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        if ((released & padButtonBit(static_cast<PadButton>(i))) != 0) {
            times[i].store(kNoPress, std::memory_order_relaxed);
        }
    }
}

void KeyboardPadState::setHeld(InputSource source, PadButtons buttons, Clock::time_point now) {
    std::atomic<PadButtons>& mask = held_[static_cast<std::size_t>(source)];
    // Single writer per source, so the load cannot go stale before the store.
    const PadButtons newly = buttons & static_cast<PadButtons>(~mask.load(std::memory_order_relaxed));
    const std::int64_t now_ns = toNanoseconds(now);
    auto& times = pressed_at_ns_[static_cast<std::size_t>(source)];
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        if ((newly & padButtonBit(static_cast<PadButton>(i))) != 0) {
            times[i].store(now_ns, std::memory_order_relaxed);
        }
    }
    mask.store(buttons, std::memory_order_relaxed);
}

PadButtons KeyboardPadState::held(InputSource source) const {
    return held_[static_cast<std::size_t>(source)].load(std::memory_order_relaxed);
}

void KeyboardPadState::setAnalog(const PadAnalog& analog) {
    analog_.store(packAnalog(analog), std::memory_order_relaxed);
}

PadAnalog KeyboardPadState::analog() const { return unpackAnalog(analog_.load(std::memory_order_relaxed)); }

void KeyboardPadState::setGamepadConnected(bool connected) {
    gamepad_connected_.store(connected, std::memory_order_relaxed);
}

bool KeyboardPadState::gamepadConnected() const { return gamepad_connected_.load(std::memory_order_relaxed); }

void KeyboardPadState::setVibration(std::uint8_t small_motor, std::uint8_t large_motor) {
    vibration_.store(static_cast<std::uint16_t>(small_motor | large_motor << 8), std::memory_order_relaxed);
}

std::pair<std::uint8_t, std::uint8_t> KeyboardPadState::vibration() const {
    const std::uint16_t bits = vibration_.load(std::memory_order_relaxed);
    return {static_cast<std::uint8_t>(bits & 0xFF), static_cast<std::uint8_t>(bits >> 8)};
}

PadButtons KeyboardPadState::held() const {
    PadButtons all = 0;
    for (std::size_t i = 0; i < kSourceCount; i++) all |= held_[i].load(std::memory_order_relaxed);
    return all;
}

PadButtons KeyboardPadState::reported(Clock::time_point now) const {
    const PadButtons held_now = held();
    PadButtons buttons = held_now;
    const std::int64_t now_ns = toNanoseconds(now);
    const std::int64_t visible_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(kMinPressVisible).count();
    std::array<std::int64_t, kPadButtonTotal> latest;
    for (std::size_t i = 0; i < kPadButtonTotal; i++) {
        latest[i] = kNoPress;
        for (const auto& times : pressed_at_ns_) {
            latest[i] = std::max(latest[i], times[i].load(std::memory_order_relaxed));
        }
        // A press stamped slightly after `now` (two threads, two clock reads) counts as recent.
        if (latest[i] != kNoPress && now_ns - latest[i] < visible_ns) {
            buttons |= padButtonBit(static_cast<PadButton>(i));
        }
    }
    // A real d-pad never reports opposite directions together. The tail of a released key must not
    // swallow a newly pressed opposite key (quick Up then Down in a menu).
    for (const Axis& axis : kAxes) {
        const PadButtons bit_a = padButtonBit(axis.a);
        const PadButtons bit_b = padButtonBit(axis.b);
        if ((buttons & (bit_a | bit_b)) != (bit_a | bit_b)) continue;
        const bool held_a = (held_now & bit_a) != 0;
        const bool held_b = (held_now & bit_b) != 0;
        if (held_a && held_b) {
            buttons &= static_cast<PadButtons>(~(bit_a | bit_b));
        } else if (held_a != held_b) {
            buttons &= static_cast<PadButtons>(~(held_a ? bit_b : bit_a));
        } else {
            const bool a_newer = latest[static_cast<std::size_t>(axis.a)] > latest[static_cast<std::size_t>(axis.b)];
            buttons &= static_cast<PadButtons>(~(a_newer ? bit_b : bit_a));
        }
    }
    return buttons;
}

std::unique_ptr<PadProvider> makeKeyboardPadProvider(std::shared_ptr<KeyboardPadState> state,
                                                     std::function<KeyboardPadState::Clock::time_point()> clock,
                                                     PadProviderOptions options) {
    return std::make_unique<KeyboardPadProvider>(std::move(state), std::move(clock), options);
}

}  // namespace tox2

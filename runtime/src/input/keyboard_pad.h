// Host input as the pad on port 0: the keyboard scan-code mapping, the lock-free state that every
// host source (keyboard, script, gamepad) writes and the guest reads, and the PadProvider that
// reads it. The state keeps its keyboard-era name; it is the whole host pad.
//
// Scan codes (physical key positions) rather than virtual keys: the mapping is a positional cluster
// of keys, which must stay in place on AZERTY/QWERTZ layouts and keep working while an IME is open.
// A scan code is the PC/AT set 1 make code, with 0xE000 added for extended keys.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "hle/hle.h"
#include "input/pad_buttons.h"

namespace dal {

// Only the dedicated (extended) arrow keys map; the numpad does not.
std::optional<PadButton> padButtonForScanCode(std::uint32_t scan_code);

enum class InputSource : std::uint8_t { Keyboard, Script, Gamepad, Count };

// Pad state held by host input. Written by the window thread (Keyboard), the script thread (Script)
// and the gamepad thread (Gamepad), read by poll on guest threads under the guest lock, so
// everything is a relaxed atomic: each mask bit, press time and snapshot is independent, and a
// stale read delays a change by one poll (the button mask and the analog snapshot are not updated
// together, so a poll between the two stores sees one stale part).
class KeyboardPadState {
public:
    using Clock = std::chrono::steady_clock;
    // A press stays reported for at least this long, so a tap shorter than the game's polling
    // interval (about 33 ms at 30 fps; cellPadGetData reports only changes) is still seen.
    static constexpr std::chrono::milliseconds kMinPressVisible{60};

    KeyboardPadState();

    // Keyboard source. Unmapped scan codes and key repeats are ignored. Returns whether the scan
    // code is mapped.
    bool keyEvent(std::uint32_t scan_code, bool down, bool repeat, Clock::time_point now);
    void setButtons(InputSource source, PadButtons buttons, bool down, Clock::time_point now);
    // Releases every held button of `source` at once, without the minimum visible time (focus
    // lost). A tap of `source` already released keeps its remaining visible time.
    void releaseAll(InputSource source);
    // Level-based update for a source that reports its whole state (the gamepad): bits newly set get
    // the press time `now`, bits no longer set are released and keep their press time, so a short tap
    // still lasts kMinPressVisible. Call from one thread per source.
    void setHeld(InputSource source, PadButtons buttons, Clock::time_point now);
    PadButtons held(InputSource source) const;

    // Sticks and trigger pressure of the gamepad source; the default PadAnalog when no gamepad is in use.
    void setAnalog(const PadAnalog& analog);
    PadAnalog analog() const;

    // True while at least one gamepad is in use; only a provider made with always_connected = false
    // looks at it.
    void setGamepadConnected(bool connected);
    bool gamepadConnected() const;

    // The latest cellPadSetActDirect values, for the gamepad thread to forward to the motors.
    void setVibration(std::uint8_t small_motor, std::uint8_t large_motor);
    std::pair<std::uint8_t, std::uint8_t> vibration() const;  // (small, large)

    PadButtons held() const;  // OR of all sources
    // held() plus the buttons pressed within kMinPressVisible before `now`. A real d-pad cannot
    // report opposite directions together: both held cancel each other; a held direction beats the
    // tail of a released one; two tails leave the more recent press.
    PadButtons reported(Clock::time_point now) const;

private:
    std::array<std::atomic<PadButtons>, static_cast<std::size_t>(InputSource::Count)> held_{};
    // Press time of each button per source in steady-clock nanoseconds, kept apart so that
    // releaseAll of one source cannot drop the minimum visible time of another source's tap.
    // kNoPress (see the .cpp) when none is remembered.
    std::array<std::array<std::atomic<std::int64_t>, kPadButtonTotal>, static_cast<std::size_t>(InputSource::Count)>
            pressed_at_ns_;
    // PadAnalog packed one byte per field (see the .cpp), so a poll never sees a half-written stick.
    std::atomic<std::uint64_t> analog_;
    std::atomic<bool> gamepad_connected_{false};
    std::atomic<std::uint16_t> vibration_{0};  // small motor in the low byte, large in the high byte
    // poll() runs under the guest lock and must never block.
    static_assert(std::atomic<std::int64_t>::is_always_lock_free && std::atomic<PadButtons>::is_always_lock_free &&
                  std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::uint16_t>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free);
};

struct PadProviderOptions {
    // true: port 0 is always connected (the keyboard or the script is part of the pad). false (`--pad
    // gamepad`): port 0 is connected only while gamepadConnected().
    bool always_connected = true;
};

// Port 0: padStateFromInput of the state's reported buttons and analog snapshot; other ports are
// empty. A trigger held on the gamepad alone reports its analog pressure; keyboard or script input
// (and the minimum-visible tail of a released trigger) report 0xFF, like a digital button.
// `clock` defaults to steady_clock::now and is injectable for tests.
std::unique_ptr<PadProvider> makeKeyboardPadProvider(std::shared_ptr<KeyboardPadState> state,
                                                     std::function<KeyboardPadState::Clock::time_point()> clock = {},
                                                     PadProviderOptions options = {});

}  // namespace dal

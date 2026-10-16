// Scripted pad input for unattended runs: a text file of timed button presses and stick positions,
// played onto the Script source of a KeyboardPadState.
//
// One command per line; `#` starts a comment, blank lines are ignored, tokens are separated by
// spaces or tabs. A time is either decimal seconds from the start of the script (millisecond
// precision) or `f<N>`, the moment the Nth RSX flip has completed (rsxFlipCount() reaches N).
// Buttons are padButtonName() names joined by `+`:
//
//   <time> press   <buttons>
//   <time> release <buttons>
//   <time> tap     <buttons> [every <interval> until <time>]
//   <time> stick   left|right <x> <y>
//
// One script uses one clock, set by its first timed line: mixing seconds and flips is an error.
// `tap` presses at <time> and releases kPadScriptTapHold (kPadScriptTapHoldFlips on the flip clock)
// later. With `every ... until` the tap repeats at <time> + k * <interval> while that is <= the
// `until` time; <interval> must be longer than the hold so that each tap ends before the next one
// starts.
// `stick` puts a stick at (x, y), decimal numbers in -1..1 with +x right and +y up (GameInput's
// convention; digits beyond the third decimal are rounded). It stays there until the next `stick` of
// the same stick; `stick left 0 0` centres it. The values become CellPadData bytes without a deadzone
// and are merged with the gamepad's sticks per axis, the value farther from centre winning.

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "input/keyboard_pad.h"

namespace tox2 {

// Longer than KeyboardPadState::kMinPressVisible, so a tap is held for real rather than only
// kept visible by the minimum press time.
inline constexpr std::chrono::milliseconds kPadScriptTapHold{150};
// The same hold on the flip clock: 150 ms at 60 flips/s.
inline constexpr std::int64_t kPadScriptTapHoldFlips = 9;

// What a script's times count.
enum class PadScriptClock { Seconds, Flips };

enum class PadScriptEventKind : std::uint8_t { Buttons, Stick };

struct PadScriptEvent {
    std::int64_t at;  // milliseconds from the start, or the flip count, as the script's clock says
    PadButtons buttons;
    bool down;
    // Stick events only (`buttons` is 0 and `down` false): the position in thousandths, -1000..1000.
    PadScriptEventKind kind = PadScriptEventKind::Buttons;
    PadStick stick = PadStick::Left;
    int x_milli = 0;
    int y_milli = 0;
};

struct PadScript {
    PadScriptClock clock = PadScriptClock::Seconds;  // a script without timed lines keeps this
    std::vector<PadScriptEvent> events;
};

// Events sorted by time (stable); at equal times button releases come before everything else. An error is one line
// of text starting with the line number ("line 7: unknown button \"crosss\"").
std::expected<PadScript, std::string> parsePadScript(std::string_view text);
std::expected<PadScript, std::string> loadPadScript(const std::filesystem::path& file);

// Applies the events in order to `state` (InputSource::Script), calling wait_until(at) before each
// and logging each event at Info. Synchronous; the core of startPadScript, used by the tests.
void playPadScript(const PadScript& script, KeyboardPadState& state,
                   const std::function<void(std::int64_t)>& wait_until);

// Runs playPadScript on a detached host thread. A seconds script waits on the steady clock from an
// origin taken now; a flip script waits for waitRsxFlipCount(at). Nothing joins the thread: the
// process ends with std::_Exit, and the thread owns what it uses.
void startPadScript(PadScript script, std::shared_ptr<KeyboardPadState> state);

}  // namespace tox2

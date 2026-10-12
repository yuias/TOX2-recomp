// Scripted pad input for unattended runs: a text file of timed button presses, played onto the
// Script source of a KeyboardPadState.
//
// One command per line; `#` starts a comment, blank lines are ignored, tokens are separated by
// spaces or tabs. A time is either decimal seconds from the start of the script (millisecond
// precision) or `f<N>`, the moment the Nth RSX flip has completed (rsxFlipCount() reaches N).
// Buttons are padButtonName() names joined by `+`:
//
//   <time> press   <buttons>
//   <time> release <buttons>
//   <time> tap     <buttons> [every <interval> until <time>]
//
// One script uses one clock, set by its first timed line: mixing seconds and flips is an error.
// `tap` presses at <time> and releases kPadScriptTapHold (kPadScriptTapHoldFlips on the flip clock)
// later. With `every ... until` the tap repeats at <time> + k * <interval> while that is <= the
// `until` time; <interval> must be longer than the hold so that each tap ends before the next one
// starts.

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

struct PadScriptEvent {
    std::int64_t at;  // milliseconds from the start, or the flip count, as the script's clock says
    PadButtons buttons;
    bool down;
};

struct PadScript {
    PadScriptClock clock = PadScriptClock::Seconds;  // a script without timed lines keeps this
    std::vector<PadScriptEvent> events;
};

// Events sorted by time (stable); at equal times releases come before presses. An error is one line
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

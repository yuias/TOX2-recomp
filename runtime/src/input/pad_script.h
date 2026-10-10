// Scripted pad input for unattended runs: a text file of timed button presses, played onto the
// Script source of a KeyboardPadState.
//
// One command per line; `#` starts a comment, blank lines are ignored, tokens are separated by
// spaces or tabs. Times are decimal seconds from the start of the script (millisecond precision).
// Buttons are padButtonName() names joined by `+`:
//
//   <time> press   <buttons>
//   <time> release <buttons>
//   <time> tap     <buttons> [every <interval> until <time>]
//
// `tap` presses at <time> and releases kPadScriptTapHold later. With `every ... until` the tap
// repeats at <time> + k * <interval> while that is <= the `until` time; <interval> must be longer
// than the hold so that each tap ends before the next one starts.

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

namespace dal {

// Longer than KeyboardPadState::kMinPressVisible, so a tap is held for real rather than only
// kept visible by the minimum press time.
inline constexpr std::chrono::milliseconds kPadScriptTapHold{150};

struct PadScriptEvent {
    std::chrono::milliseconds at;
    PadButtons buttons;
    bool down;
};

// Events sorted by time (stable); at equal times releases come before presses. An error is one line
// of text starting with the line number ("line 7: unknown button \"crosss\"").
std::expected<std::vector<PadScriptEvent>, std::string> parsePadScript(std::string_view text);
std::expected<std::vector<PadScriptEvent>, std::string> loadPadScript(const std::filesystem::path& file);

// Applies the events in order to `state` (InputSource::Script), calling wait_until(at) before each
// and logging each event at Info. Synchronous; the core of startPadScript, used by the tests.
void playPadScript(std::span<const PadScriptEvent> events, KeyboardPadState& state,
                   const std::function<void(std::chrono::milliseconds)>& wait_until);

// Runs playPadScript on a detached host thread with a steady-clock origin taken now. Nothing
// joins the thread: the process ends with std::_Exit, and the thread owns what it uses.
void startPadScript(std::vector<PadScriptEvent> events, std::shared_ptr<KeyboardPadState> state);

}  // namespace dal

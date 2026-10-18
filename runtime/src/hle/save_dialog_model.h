// Decision logic of the save data dialogs: pad edges and auto-repeat, selection, scrolling, fades and the result.
// Pure and clock-injected, so it needs no window and no pad provider. The edge and repeat rules follow RPCS3's
// overlay input loop; the enter/back buttons come from dialogButtons().

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "hle/hle.h"
#include "input/pad_buttons.h"

namespace tox2 {

enum class DialogAction : std::uint8_t { Up, Down, PageUp, PageDown, Accept, Back };
enum class DialogKind : std::uint8_t { List, YesNo, Ok };
enum class DialogAnswer : std::uint8_t { Row, Back, Yes, No, Ok };

struct DialogResult {
    DialogAnswer answer = DialogAnswer::Back;
    std::int32_t row = -1;  // the selected row when answer == Row
};

struct PadSample {
    PadButtons buttons = 0;  // PadButton bit order (digital1 | digital2 << 8)
    std::uint8_t left_y = 0x80, right_y = 0x80;
};

PadSample padSampleFromState(const PadPortState& state);

// Turns successive pad samples into actions. Every input counts as held at construction, so a button still down
// from the game's menu acts only after it was released and pressed again.
class DialogInput {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kRepeatDelay{500};
    static constexpr std::chrono::milliseconds kRepeatInterval{200};
    static constexpr std::uint8_t kStickLow = 30;    // a stick counts as pressed below this ...
    static constexpr std::uint8_t kStickHigh = 225;  // ... or above this

    explicit DialogInput(DialogButtons buttons);
    std::vector<DialogAction> update(const PadSample& sample, Clock::time_point now);

private:
    struct Key {
        bool held = true;
        bool repeats = false;
        Clock::time_point next_repeat = Clock::time_point::max();
    };
    enum KeyIndex : std::size_t { Up, Down, PageUp, PageDown, Accept, Back, KeyCount };

    // True when the key acts on this sample.
    bool step(Key& key, bool pressed, Clock::time_point now) const;

    DialogButtons buttons_;
    Key keys_[KeyCount];
};

class SaveDialogModel {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kFade{150};
    static constexpr std::int32_t kVisibleRows = 4;
    static constexpr std::int32_t kPageRows = 10;

    SaveDialogModel(DialogKind kind, std::int32_t row_count, std::int32_t initial_row, Clock::time_point now);
    // Applies `actions` (ignored while a fade runs). Returns the result once the fade-out has finished.
    std::optional<DialogResult> step(std::span<const DialogAction> actions, Clock::time_point now);
    std::int32_t selectedRow() const { return selected_; }
    std::int32_t firstVisibleRow() const { return first_; }
    float opacity(Clock::time_point now) const;  // 0..1 over the fade-in and fade-out

private:
    void apply(DialogAction action, Clock::time_point now);
    void moveTo(std::int32_t row);
    void close(DialogResult result, Clock::time_point now);

    DialogKind kind_;
    std::int32_t row_count_;
    std::int32_t selected_ = 0;
    std::int32_t first_ = 0;
    Clock::time_point opened_;
    std::optional<Clock::time_point> closed_;
    DialogResult result_;
};

}  // namespace tox2

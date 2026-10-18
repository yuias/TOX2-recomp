#include "hle/save_dialog_model.h"

#include <algorithm>

namespace tox2 {
namespace {

bool hasButton(PadButtons buttons, PadButton b) { return (buttons & padButtonBit(b)) != 0; }

bool stickUp(std::uint8_t y) { return y < DialogInput::kStickLow; }
bool stickDown(std::uint8_t y) { return y > DialogInput::kStickHigh; }

}  // namespace

PadSample padSampleFromState(const PadPortState& state) {
    PadSample sample;
    sample.buttons = static_cast<PadButtons>((state.button[kPadButtonDigital1] & 0xFF) |
                                             ((state.button[kPadButtonDigital2] & 0xFF) << 8));
    sample.left_y = static_cast<std::uint8_t>(state.button[kPadButtonAnalogLeftY]);
    sample.right_y = static_cast<std::uint8_t>(state.button[kPadButtonAnalogRightY]);
    return sample;
}

DialogInput::DialogInput(DialogButtons buttons) : buttons_(buttons) {
    keys_[Up].repeats = true;
    keys_[Down].repeats = true;
}

bool DialogInput::step(Key& key, bool pressed, Clock::time_point now) const {
    if (!pressed) {
        key.held = false;
        key.next_repeat = Clock::time_point::max();
        return false;
    }
    if (!key.held) {
        key.held = true;
        if (key.repeats) key.next_repeat = now + kRepeatDelay;
        return true;
    }
    // A key held since construction has no repeat time, so it stays quiet until released.
    if (key.repeats && now >= key.next_repeat) {
        // Re-anchor on `now` when the caller fell behind, so a stall does not release a burst of repeats.
        key.next_repeat += kRepeatInterval;
        if (key.next_repeat <= now) key.next_repeat = now + kRepeatInterval;
        return true;
    }
    return false;
}

std::vector<DialogAction> DialogInput::update(const PadSample& sample, Clock::time_point now) {
    const PadButtons b = sample.buttons;
    const bool up = hasButton(b, PadButton::Up) || stickUp(sample.left_y) || stickUp(sample.right_y);
    const bool down = hasButton(b, PadButton::Down) || stickDown(sample.left_y) || stickDown(sample.right_y);

    std::vector<DialogAction> actions;
    if (step(keys_[Up], up, now)) actions.push_back(DialogAction::Up);
    if (step(keys_[Down], down, now)) actions.push_back(DialogAction::Down);
    if (step(keys_[PageUp], hasButton(b, PadButton::L1), now)) actions.push_back(DialogAction::PageUp);
    if (step(keys_[PageDown], hasButton(b, PadButton::R1), now)) actions.push_back(DialogAction::PageDown);
    if (step(keys_[Accept], hasButton(b, buttons_.enter), now)) actions.push_back(DialogAction::Accept);
    if (step(keys_[Back], hasButton(b, buttons_.back), now)) actions.push_back(DialogAction::Back);
    return actions;
}

SaveDialogModel::SaveDialogModel(DialogKind kind, std::int32_t row_count, std::int32_t initial_row,
                                 Clock::time_point now)
    : kind_(kind), row_count_(std::max<std::int32_t>(row_count, 0)), opened_(now) {
    selected_ = row_count_ > 0 ? std::clamp(initial_row, 0, row_count_ - 1) : 0;
    first_ = std::clamp(selected_ - kVisibleRows / 2, 0, std::max(0, row_count_ - kVisibleRows));
}

void SaveDialogModel::moveTo(std::int32_t row) {
    if (row_count_ <= 0) return;
    selected_ = std::clamp(row, 0, row_count_ - 1);
    // Minimal scroll that keeps the selection inside the visible window.
    if (selected_ < first_) {
        first_ = selected_;
    } else if (selected_ >= first_ + kVisibleRows) {
        first_ = selected_ - kVisibleRows + 1;
    }
}

void SaveDialogModel::close(DialogResult result, Clock::time_point now) {
    result_ = result;
    closed_ = now;
}

void SaveDialogModel::apply(DialogAction action, Clock::time_point now) {
    switch (kind_) {
        case DialogKind::List:
            switch (action) {
                case DialogAction::Up: moveTo(selected_ - 1); break;
                case DialogAction::Down: moveTo(selected_ + 1); break;
                case DialogAction::PageUp: moveTo(selected_ - kPageRows); break;
                case DialogAction::PageDown: moveTo(selected_ + kPageRows); break;
                case DialogAction::Accept:
                    if (row_count_ > 0) close({DialogAnswer::Row, selected_}, now);
                    break;
                case DialogAction::Back: close({DialogAnswer::Back, -1}, now); break;
            }
            break;
        case DialogKind::YesNo:
            if (action == DialogAction::Accept) close({DialogAnswer::Yes, -1}, now);
            if (action == DialogAction::Back) close({DialogAnswer::No, -1}, now);
            break;
        case DialogKind::Ok:
            if (action == DialogAction::Accept) close({DialogAnswer::Ok, -1}, now);
            break;
    }
}

std::optional<DialogResult> SaveDialogModel::step(std::span<const DialogAction> actions, Clock::time_point now) {
    if (closed_) {
        if (now - *closed_ >= kFade) return result_;
        return std::nullopt;
    }
    if (now - opened_ < kFade) return std::nullopt;
    for (const DialogAction action : actions) {
        apply(action, now);
        if (closed_) break;
    }
    return std::nullopt;
}

float SaveDialogModel::opacity(Clock::time_point now) const {
    const auto fraction = [](Clock::duration elapsed) {
        return std::clamp(std::chrono::duration<float>(elapsed) / std::chrono::duration<float>(kFade), 0.0f, 1.0f);
    };
    if (closed_) return 1.0f - fraction(now - *closed_);
    return fraction(now - opened_);
}

}  // namespace tox2

// Self-checking tests for the save data dialog model. Time is injected, so nothing sleeps and no pad provider,
// window or game data is involved.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hle/save_dialog.h"
#include "hle/save_dialog_model.h"
#include "hle/save_dialog_text.h"
#include "input/pad_buttons.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using tox2::DialogAction;
using tox2::DialogAnswer;
using tox2::DialogInput;
using tox2::DialogKind;
using tox2::PadButton;
using tox2::PadSample;
using tox2::SaveDialogModel;
using Clock = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(100);

Clock::time_point at(int ms) { return kT0 + std::chrono::milliseconds(ms); }

PadSample held(PadButton b) {
    PadSample s;
    s.buttons = tox2::padButtonBit(b);
    return s;
}

using Actions = std::vector<DialogAction>;

// A DialogInput that has already seen a neutral sample, so every input counts as released.
DialogInput releasedInput() {
    DialogInput input(tox2::dialogButtons());
    input.update(PadSample{}, at(0));
    return input;
}

void testHeldAtConstruction() {
    DialogInput input(tox2::dialogButtons());
    const PadSample circle = held(PadButton::Circle);
    CHECK(input.update(circle, at(0)).empty());
    CHECK(input.update(circle, at(50)).empty());
    CHECK(input.update(PadSample{}, at(100)).empty());
    CHECK((input.update(circle, at(150)) == Actions{DialogAction::Accept}));

    // A direction held since construction never repeats either.
    DialogInput dir(tox2::dialogButtons());
    const PadSample up = held(PadButton::Up);
    bool acted = false;
    for (int ms = 0; ms <= 3000; ms += 5) acted = acted || !dir.update(up, at(ms)).empty();
    CHECK(!acted);
    dir.update(PadSample{}, at(3005));
    CHECK((dir.update(up, at(3010)) == Actions{DialogAction::Up}));
}

void testDpadRepeat() {
    DialogInput input = releasedInput();
    const PadSample down = held(PadButton::Down);
    CHECK((input.update(down, at(10)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(100)).empty());
    CHECK(input.update(down, at(509)).empty());
    CHECK((input.update(down, at(510)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(709)).empty());
    CHECK((input.update(down, at(710)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(909)).empty());
    CHECK((input.update(down, at(910)) == Actions{DialogAction::Down}));

    // Releasing restarts the delay.
    input.update(PadSample{}, at(950));
    CHECK((input.update(down, at(960)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(1400)).empty());
    CHECK((input.update(down, at(1460)) == Actions{DialogAction::Down}));
}

void testRepeatAfterStall() {
    DialogInput input = releasedInput();
    const PadSample up = held(PadButton::Up);
    CHECK((input.update(up, at(0)) == Actions{DialogAction::Up}));
    // One late sample gives one action, not a burst of the missed repeats.
    CHECK((input.update(up, at(2000)) == Actions{DialogAction::Up}));
    CHECK(input.update(up, at(2005)).empty());
    CHECK((input.update(up, at(2200)) == Actions{DialogAction::Up}));
}

void testStickThresholds() {
    for (const bool left : {true, false}) {
        const auto sample = [left](std::uint8_t y) {
            PadSample s;
            (left ? s.left_y : s.right_y) = y;
            return s;
        };
        DialogInput input = releasedInput();
        CHECK(input.update(sample(30), at(10)).empty());
        CHECK(input.update(sample(225), at(20)).empty());
        CHECK((input.update(sample(29), at(30)) == Actions{DialogAction::Up}));
        input.update(sample(0x80), at(40));
        CHECK((input.update(sample(226), at(50)) == Actions{DialogAction::Down}));
        // Sticks repeat like the d-pad.
        CHECK(input.update(sample(226), at(549)).empty());
        CHECK((input.update(sample(226), at(550)) == Actions{DialogAction::Down}));
        CHECK(input.update(sample(226), at(749)).empty());
        CHECK((input.update(sample(226), at(750)) == Actions{DialogAction::Down}));
    }
}

void testShoulderButtonsNoRepeat() {
    DialogInput input = releasedInput();
    CHECK((input.update(held(PadButton::L1), at(10)) == Actions{DialogAction::PageUp}));
    CHECK((input.update(held(PadButton::R1), at(20)) == Actions{DialogAction::PageDown}));
    bool acted = false;
    for (int ms = 30; ms <= 3000; ms += 5) acted = acted || !input.update(held(PadButton::R1), at(ms)).empty();
    CHECK(!acted);
}

void testOtherButtonsIgnored() {
    DialogInput input = releasedInput();
    for (const PadButton b : {PadButton::Triangle, PadButton::Square, PadButton::Start, PadButton::Select,
                              PadButton::L2, PadButton::R2, PadButton::Left, PadButton::Right}) {
        CHECK(input.update(held(b), at(10)).empty());
        input.update(PadSample{}, at(20));
    }
}

void testEnterButtonAssign() {
    // Circle is enter by default.
    {
        DialogInput input(tox2::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Circle), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Cross), at(30)) == Actions{DialogAction::Back}));
    }
    tox2::setEnterButtonAssign(tox2::EnterButtonAssign::Cross);
    {
        DialogInput input(tox2::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Cross), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Circle), at(30)) == Actions{DialogAction::Back}));
    }
    tox2::setEnterButtonAssign(tox2::EnterButtonAssign::Circle);
}

void testPadSampleFromState() {
    tox2::PadPortState state =
            tox2::padStateFromButtons(tox2::padButtonBit(PadButton::Up) | tox2::padButtonBit(PadButton::Circle) |
                                      tox2::padButtonBit(PadButton::L1));
    state.button[tox2::kPadButtonAnalogLeftY] = 12;
    state.button[tox2::kPadButtonAnalogRightY] = 250;
    const PadSample sample = tox2::padSampleFromState(state);
    CHECK(sample.buttons == (tox2::padButtonBit(PadButton::Up) | tox2::padButtonBit(PadButton::Circle) |
                             tox2::padButtonBit(PadButton::L1)));
    CHECK(sample.left_y == 12);
    CHECK(sample.right_y == 250);
    const PadSample neutral = tox2::padSampleFromState(tox2::neutralPadState());
    CHECK(neutral.buttons == 0 && neutral.left_y == 0x80 && neutral.right_y == 0x80);
}

std::optional<tox2::DialogResult> stepOne(SaveDialogModel& model, DialogAction action, int ms) {
    const DialogAction actions[] = {action};
    return model.step(actions, at(ms));
}

std::optional<tox2::DialogResult> idle(SaveDialogModel& model, int ms) { return model.step({}, at(ms)); }

void testPageJumpsClamped() {
    SaveDialogModel model(DialogKind::List, 25, 12, at(0));
    stepOne(model, DialogAction::PageDown, 200);
    CHECK(model.selectedRow() == 22);
    stepOne(model, DialogAction::PageDown, 210);
    CHECK(model.selectedRow() == 24);
    stepOne(model, DialogAction::PageUp, 220);
    CHECK(model.selectedRow() == 14);
    stepOne(model, DialogAction::PageUp, 230);
    CHECK(model.selectedRow() == 4);
    stepOne(model, DialogAction::PageUp, 240);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Up, 250);
    CHECK(model.selectedRow() == 0);
}

void testNoWrap() {
    SaveDialogModel model(DialogKind::List, 3, 2, at(0));
    stepOne(model, DialogAction::Down, 200);
    CHECK(model.selectedRow() == 2);
    stepOne(model, DialogAction::Up, 210);
    stepOne(model, DialogAction::Up, 220);
    stepOne(model, DialogAction::Up, 230);
    CHECK(model.selectedRow() == 0);
}

void testFadeInIgnoresInput() {
    SaveDialogModel model(DialogKind::List, 5, 0, at(0));
    stepOne(model, DialogAction::Down, 0);
    stepOne(model, DialogAction::Down, 149);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Down, 150);
    CHECK(model.selectedRow() == 1);

    SaveDialogModel accept(DialogKind::List, 5, 3, at(0));
    stepOne(accept, DialogAction::Accept, 149);
    CHECK(!idle(accept, 400));  // never closed: the Accept at 149 was dropped
    CHECK(accept.opacity(at(400)) == 1.0f);
}

void testResultAfterFadeOut() {
    SaveDialogModel model(DialogKind::List, 5, 3, at(0));
    CHECK(!stepOne(model, DialogAction::Accept, 200));
    CHECK(!idle(model, 349));
    const auto result = idle(model, 350);
    CHECK(result.has_value());
    if (result) {
        CHECK(result->answer == DialogAnswer::Row);
        CHECK(result->row == 3);
    }

    // Input after the close does not change the result.
    SaveDialogModel closing(DialogKind::List, 5, 3, at(0));
    stepOne(closing, DialogAction::Back, 200);
    stepOne(closing, DialogAction::Accept, 250);
    const auto back = idle(closing, 400);
    CHECK(back.has_value() && back->answer == DialogAnswer::Back && back->row == -1);
}

void testFirstCloseWinsWithinOneStep() {
    SaveDialogModel model(DialogKind::List, 5, 1, at(0));
    const DialogAction actions[] = {DialogAction::Back, DialogAction::Accept};
    model.step(actions, at(200));
    const auto result = idle(model, 350);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testEmptyListIgnoresAccept() {
    SaveDialogModel model(DialogKind::List, 0, 4, at(0));
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Accept, 200);
    CHECK(!idle(model, 500));
    CHECK(model.opacity(at(500)) == 1.0f);
    stepOne(model, DialogAction::Down, 510);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Back, 520);
    const auto result = idle(model, 700);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testOkIgnoresBack() {
    SaveDialogModel model(DialogKind::Ok, 0, 0, at(0));
    stepOne(model, DialogAction::Back, 200);
    CHECK(!idle(model, 500));
    stepOne(model, DialogAction::Accept, 510);
    CHECK(!idle(model, 600));
    const auto result = idle(model, 660);
    CHECK(result.has_value() && result->answer == DialogAnswer::Ok);
}

void testYesNo() {
    SaveDialogModel yes(DialogKind::YesNo, 0, 0, at(0));
    stepOne(yes, DialogAction::Accept, 200);
    const auto y = idle(yes, 350);
    CHECK(y.has_value() && y->answer == DialogAnswer::Yes);

    SaveDialogModel no(DialogKind::YesNo, 0, 0, at(0));
    stepOne(no, DialogAction::Back, 200);
    const auto n = idle(no, 350);
    CHECK(n.has_value() && n->answer == DialogAnswer::No);
}

void testInitialRowAndScroll() {
    // Centred on open, clamped at both ends.
    CHECK(SaveDialogModel(DialogKind::List, 20, 0, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 2, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 10, at(0)).firstVisibleRow() == 8);
    CHECK(SaveDialogModel(DialogKind::List, 20, 19, at(0)).firstVisibleRow() == 15);
    CHECK(SaveDialogModel(DialogKind::List, 3, 1, at(0)).firstVisibleRow() == 0);
    // initial_row is clamped into the list.
    CHECK(SaveDialogModel(DialogKind::List, 4, 99, at(0)).selectedRow() == 3);
    CHECK(SaveDialogModel(DialogKind::List, 4, -5, at(0)).selectedRow() == 0);
}

void testScrollKeepsSelectionVisible() {
    SaveDialogModel model(DialogKind::List, 12, 0, at(0));
    int ms = 200;
    for (int i = 0; i < 14; i++, ms += 10) {
        stepOne(model, DialogAction::Down, ms);
        CHECK(model.selectedRow() >= model.firstVisibleRow());
        CHECK(model.selectedRow() < model.firstVisibleRow() + SaveDialogModel::kVisibleRows);
    }
    CHECK(model.selectedRow() == 11);
    CHECK(model.firstVisibleRow() == 7);
    // Moving up inside the window does not scroll; leaving it scrolls by the minimum.
    for (int i = 0; i < 4; i++, ms += 10) stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 7);
    CHECK(model.firstVisibleRow() == 7);
    stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 6);
    CHECK(model.firstVisibleRow() == 6);
    ms += 10;
    stepOne(model, DialogAction::PageUp, ms);
    CHECK(model.selectedRow() == 0);
    CHECK(model.firstVisibleRow() == 0);
    ms += 10;
    stepOne(model, DialogAction::PageDown, ms);
    CHECK(model.selectedRow() == 10);
    CHECK(model.firstVisibleRow() == 6);
}

void testOpacity() {
    SaveDialogModel model(DialogKind::List, 3, 0, at(0));
    CHECK(model.opacity(at(0)) == 0.0f);
    CHECK(model.opacity(at(75)) > 0.49f && model.opacity(at(75)) < 0.51f);
    CHECK(model.opacity(at(150)) == 1.0f);
    CHECK(model.opacity(at(1000)) == 1.0f);
    stepOne(model, DialogAction::Back, 1000);
    CHECK(model.opacity(at(1000)) == 1.0f);
    CHECK(model.opacity(at(1075)) > 0.49f && model.opacity(at(1075)) < 0.51f);
    CHECK(model.opacity(at(1150)) == 0.0f);
    CHECK(model.opacity(at(2000)) == 0.0f);
}

void testFormatSaveSize() {
    CHECK(tox2::formatSaveSize(0) == "0 KB");
    CHECK(tox2::formatSaveSize(1) == "1 KB");
    CHECK(tox2::formatSaveSize(1000) == "1 KB");
    CHECK(tox2::formatSaveSize(1001) == "2 KB");
    CHECK(tox2::formatSaveSize(1000000) == "1000 KB");
    CHECK(tox2::formatSaveSize(1000001) == "2 MB");
    CHECK(tox2::formatSaveSize(2000000) == "2 MB");
    CHECK(tox2::formatSaveSize(2000001) == "3 MB");
}

std::tm fixedTime() {
    std::tm t{};
    t.tm_year = 2015 - 1900;
    t.tm_mon = 7;
    t.tm_mday = 21;
    t.tm_hour = 13;
    t.tm_min = 52;
    return t;
}

void testFormatSaveDate() {
    CHECK(tox2::formatSaveDate(fixedTime()) == "2015/08/21  13:52");
    std::tm early{};
    early.tm_year = 2001 - 1900;
    early.tm_mon = 0;
    early.tm_mday = 2;
    early.tm_hour = 3;
    early.tm_min = 4;
    CHECK(tox2::formatSaveDate(early) == "2001/01/02  03:04");
    CHECK(tox2::saveRowInfo(fixedTime(), 1001) == "2015/08/21  13:52   2 KB");
}

void testSaveConfirmText() {
    CHECK(tox2::saveConfirmText("Q?", "T", "S", "D", "Z", "detail") == "Q?\n\nT\nS\nD\nZ\n\ndetail");
    // Empty fields keep their lines.
    CHECK(tox2::saveConfirmText("Q?", "T", "", "D", "Z", "") == "Q?\n\nT\n\nD\nZ\n\n");
    CHECK(tox2::saveConfirmText(tox2::kConfirmOverwrite, "", "", "", "", "") ==
          std::string(tox2::kConfirmOverwrite) + "\n\n\n\n\n\n\n");
}

void testSaveErrorText() {
    const auto no_space = tox2::saveErrorText(-1, 12, "");
    CHECK(no_space.has_value() && *no_space == "空き容量が足りません。\n\n必要な空き容量：12 KB");
    const auto failure = tox2::saveErrorText(-2, 0, "");
    CHECK(failure.has_value() && *failure == "セーブまたはロードに失敗しました。");
    const auto broken = tox2::saveErrorText(-3, 0, "");
    CHECK(broken.has_value() && *broken == "セーブデータが壊れています。");
    const auto no_data = tox2::saveErrorText(-4, 0, "");
    CHECK(no_data.has_value() && *no_data == "セーブデータが見つかりません。");
    const auto invalid = tox2::saveErrorText(-5, 0, "custom message");
    CHECK(invalid.has_value() && *invalid == "custom message");
    const auto invalid_empty = tox2::saveErrorText(-5, 0, "");
    CHECK(invalid_empty.has_value() && invalid_empty->empty());
    CHECK(!tox2::saveErrorText(3, 0, "ignored").has_value());
    CHECK(!tox2::saveErrorText(0, 0, "ignored").has_value());
}

std::shared_ptr<tox2::SaveDialogContent> contentWithRows(int rows, DialogKind kind = DialogKind::List) {
    auto content = std::make_shared<tox2::SaveDialogContent>();
    content->kind = kind;
    content->rows.resize(static_cast<std::size_t>(rows));
    return content;
}

void testAutoDriver() {
    auto driver = tox2::makeAutoSaveDialogDriver(tox2::SaveDialogPolicy::Auto);
    CHECK(!driver->interactive());

    CHECK(driver->showList({contentWithRows(3), 1}) == std::optional<std::int32_t>(1));
    CHECK(driver->showList({contentWithRows(3), 9}) == std::optional<std::int32_t>(2));
    CHECK(driver->showList({contentWithRows(3), -4}) == std::optional<std::int32_t>(0));
    CHECK(!driver->showList({contentWithRows(0), 0}).has_value());

    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == tox2::SaveMessageResult::Yes);
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == tox2::SaveMessageResult::Ok);
}

void testCancelDriver() {
    auto driver = tox2::makeAutoSaveDialogDriver(tox2::SaveDialogPolicy::Cancel);
    CHECK(!driver->interactive());
    CHECK(!driver->showList({contentWithRows(3), 1}).has_value());
    CHECK(!driver->showList({contentWithRows(0), 0}).has_value());
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == tox2::SaveMessageResult::No);
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == tox2::SaveMessageResult::Ok);
}

void testBoard() {
    tox2::SaveDialogBoard board;
    CHECK(board.read().content == nullptr);
    CHECK(board.drawer() == tox2::SaveDialogDrawer::None);

    auto content = contentWithRows(2);
    tox2::SaveDialogSnapshot snapshot;
    snapshot.content = content;
    snapshot.selected_row = 1;
    snapshot.first_visible_row = 1;
    snapshot.opacity = 0.5f;
    board.publish(snapshot);
    const auto first = board.read();
    CHECK(first.content == content);
    CHECK(first.selected_row == 1);
    CHECK(first.first_visible_row == 1);
    CHECK(first.opacity == 0.5f);

    board.publish(snapshot);
    const auto second = board.read();
    CHECK(second.serial > first.serial);

    board.clear();
    const auto cleared = board.read();
    CHECK(cleared.content == nullptr);
    CHECK(cleared.serial > second.serial);

    board.setDrawer(tox2::SaveDialogDrawer::Ready);
    CHECK(board.drawer() == tox2::SaveDialogDrawer::Ready);
    board.setDrawer(tox2::SaveDialogDrawer::Failed);
    CHECK(board.drawer() == tox2::SaveDialogDrawer::Failed);
    board.clear();
    CHECK(board.drawer() == tox2::SaveDialogDrawer::Failed);
}

void testDriverRegistry() {
    tox2::resetSaveDialog();
    CHECK(!tox2::saveDialogDriver().interactive());
    CHECK(tox2::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == tox2::SaveMessageResult::Yes);
    CHECK(!tox2::saveDialogOpen());

    tox2::setSaveDialogDriver(tox2::makeAutoSaveDialogDriver(tox2::SaveDialogPolicy::Cancel));
    CHECK(tox2::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == tox2::SaveMessageResult::No);
    tox2::setSaveDialogDriver(nullptr);
    CHECK(tox2::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == tox2::SaveMessageResult::Yes);

    tox2::saveDialogBoard().setDrawer(tox2::SaveDialogDrawer::Ready);
    tox2::SaveDialogSnapshot snapshot;
    snapshot.content = contentWithRows(1);
    tox2::saveDialogBoard().publish(snapshot);
    tox2::setSaveDialogDriver(tox2::makeInteractiveSaveDialogDriver(tox2::saveDialogBoard()));
    CHECK(tox2::saveDialogDriver().interactive());
    tox2::resetSaveDialog();
    CHECK(!tox2::saveDialogDriver().interactive());
    CHECK(tox2::saveDialogBoard().read().content == nullptr);
    CHECK(tox2::saveDialogBoard().drawer() == tox2::SaveDialogDrawer::None);
}

}  // namespace

int main() {
    testHeldAtConstruction();
    testDpadRepeat();
    testRepeatAfterStall();
    testStickThresholds();
    testShoulderButtonsNoRepeat();
    testOtherButtonsIgnored();
    testEnterButtonAssign();
    testPadSampleFromState();
    testPageJumpsClamped();
    testNoWrap();
    testFadeInIgnoresInput();
    testResultAfterFadeOut();
    testFirstCloseWinsWithinOneStep();
    testEmptyListIgnoresAccept();
    testOkIgnoresBack();
    testYesNo();
    testInitialRowAndScroll();
    testScrollKeepsSelectionVisible();
    testOpacity();
    testFormatSaveSize();
    testFormatSaveDate();
    testSaveConfirmText();
    testSaveErrorText();
    testAutoDriver();
    testCancelDriver();
    testBoard();
    testDriverRegistry();
    std::printf("save dialog tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

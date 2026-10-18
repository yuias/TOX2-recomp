// Self-checking tests for the save data dialog model. Time is injected, so nothing sleeps and no pad provider,
// window or game data is involved.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/threads.h"
#include "hle/hle.h"
#include "hle/save_dialog.h"
#include "hle/save_dialog_model.h"
#include "hle/save_dialog_text.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"
#include "tox2/ppu_context.h"

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
    CHECK(SaveDialogModel(DialogKind::List, 20, 19, at(0)).firstVisibleRow() == 16);
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
    CHECK(model.firstVisibleRow() == 8);
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
    CHECK(model.firstVisibleRow() == 7);
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

// Every wait below has a deadline. When one expires the dialog is told its overlay failed, which makes the driver
// leave its loop, so a broken driver fails checks instead of hanging the run.
constexpr std::chrono::milliseconds kDeadline{5000};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = kDeadline) {
    const auto deadline = Clock::now() + timeout;
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A host thread that holds the kernel lock (and in the split PPU modes a PPU slot) while it runs `body`, like a guest
// thread inside a handler.
class GuestRun {
public:
    GuestRun(tox2::SaveDialogBoard& board, std::function<void()> body, bool use_slot = false) : board_(board) {
        thread_ = std::thread([this, body = std::move(body), use_slot] {
            if (use_slot) tox2::ppuSlots().acquire();
            tox2::guestLock().lock();
            body();
            tox2::guestLock().unlock();
            if (use_slot) tox2::ppuSlots().release();
            finished_at_ = Clock::now();
            done_.store(true);
        });
    }
    ~GuestRun() { join(); }
    GuestRun(const GuestRun&) = delete;
    GuestRun& operator=(const GuestRun&) = delete;

    // False when the body did not end within `timeout`.
    bool join(std::chrono::milliseconds timeout = kDeadline) {
        const bool finished = waitFor([this] { return done_.load(); }, timeout);
        if (!finished) board_.setDrawer(tox2::SaveDialogDrawer::Failed);
        if (thread_.joinable()) thread_.join();
        return finished;
    }

    // When the body ended; valid after join() returned true.
    Clock::time_point finishedAt() const { return finished_at_; }

private:
    tox2::SaveDialogBoard& board_;
    Clock::time_point finished_at_{};
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// Runs `fn` on a thread that holds the kernel lock, as the test's own view into state that handlers guard. In the split
// modes it also needs a PPU slot, so a driver that kept its slot or the lock during the wait leaves this call waiting
// until the join deadline, which then fails the check (and makes the driver give up) instead of passing. A driver that
// ignored the drawer as well would still hang the join.
bool underGuestLock(tox2::SaveDialogBoard& board, std::function<void()> fn) {
    GuestRun run(board, std::move(fn), tox2::splitPpuMode());
    return run.join();
}

struct DialogFixture {
    std::shared_ptr<tox2::KeyboardPadState> pad = std::make_shared<tox2::KeyboardPadState>();
    tox2::SaveDialogBoard board;
    std::unique_ptr<tox2::SaveDialogDriver> driver;

    explicit DialogFixture(bool drawer_ready = true) {
        tox2::resetCellSysutil();
        tox2::resetCellPad();
        tox2::resetSaveDialog();
        tox2::setPadProvider(tox2::makeKeyboardPadProvider(pad));
        if (drawer_ready) board.setDrawer(tox2::SaveDialogDrawer::Ready);
        driver = tox2::makeInteractiveSaveDialogDriver(board);
    }
    ~DialogFixture() {
        tox2::resetCellMsgDialog();
        tox2::setPadProvider(nullptr);
        tox2::resetCellPad();
        tox2::resetSaveDialog();
        tox2::resetCellSysutil();
    }

    void press(PadButton button, bool down) {
        pad->setButtons(tox2::InputSource::Keyboard, tox2::padButtonBit(button), down, Clock::now());
    }
    bool waitForDialog() {
        return waitFor([this] { return board.read().content != nullptr; });
    }
    // Input is dropped during the fade-in, so presses wait for it to end.
    bool waitForFadeIn() {
        return waitFor([this] {
            const auto snapshot = board.read();
            return snapshot.content && snapshot.opacity >= 0.999f;
        });
    }
};

// What the test thread sees of the system state while no dialog code runs.
struct SystemView {
    bool intercepted = false;
    bool dialog_open = false;
    std::int32_t begin = 0;  // queueSysutilEvent(DRAWING_BEGIN)
};

SystemView viewSystem(DialogFixture& f, bool probe_begin) {
    SystemView view;
    CHECK(underGuestLock(f.board, [&] {
        view.intercepted = tox2::padIntercepted();
        view.dialog_open = tox2::saveDialogOpen();
        if (probe_begin) view.begin = tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0);
    }));
    return view;
}

// DRAWING_END for a BEGIN the test sent itself or probed successfully.
std::int32_t sendEnd(DialogFixture& f) {
    std::int32_t result = 0;
    CHECK(underGuestLock(f.board, [&] { result = tox2::queueSysutilEvent(tox2::kSysutilDrawingEnd, 0); }));
    return result;
}

constexpr auto kBusy = static_cast<std::int32_t>(tox2::kSysutilErrorBusy);

// The interactive list: published, pad taken over, pad given back, DRAWING_BEGIN held and released. In the split
// modes the dialog thread also holds a PPU slot, which the wait has to give up as well as the kernel lock.
void testInteractiveList(tox2::PpuThreadsMode mode) {
    tox2::setPpuThreadsMode(mode);
    {
        DialogFixture f;
        std::optional<std::int32_t> row = 99;
        GuestRun run(
                f.board, [&] { row = f.driver->showList({contentWithRows(3), 1}); },
                mode != tox2::PpuThreadsMode::Legacy);

        CHECK(f.waitForDialog());
        const auto first = f.board.read();
        CHECK(first.content && first.content->kind == DialogKind::List && first.content->id != 0);
        CHECK(first.selected_row == 1);

        // The kernel lock is free during the wait: a second BEGIN is refused and the pad is intercepted.
        const SystemView during = viewSystem(f, true);
        CHECK(during.begin == kBusy);
        CHECK(during.intercepted);
        CHECK(during.dialog_open);

        CHECK(f.waitForFadeIn());
        f.press(PadButton::Down, true);
        CHECK(waitFor([&] { return f.board.read().selected_row == 2; }));
        f.press(PadButton::Down, false);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);

        CHECK(row == std::optional<std::int32_t>(2));
        CHECK(f.board.read().content == nullptr);
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted);
        CHECK(!after.dialog_open);
        CHECK(after.begin == 0);  // the dialog's END went out, so BEGIN is accepted again
        CHECK(sendEnd(f) == 0);
    }
    tox2::setPpuThreadsMode(tox2::PpuThreadsMode::Legacy);
}

void testInteractiveMessage() {
    struct Case {
        DialogKind kind;
        PadButton button;
        tox2::SaveMessageResult expected;
    };
    const Case cases[] = {
            {DialogKind::YesNo, PadButton::Circle, tox2::SaveMessageResult::Yes},
            {DialogKind::YesNo, PadButton::Cross, tox2::SaveMessageResult::No},
            {DialogKind::Ok, PadButton::Circle, tox2::SaveMessageResult::Ok},
    };
    for (const Case& c : cases) {
        DialogFixture f;
        tox2::SaveMessageResult result = tox2::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, c.kind)); });
        CHECK(f.waitForFadeIn());
        const auto shown = f.board.read();
        CHECK(shown.content && shown.content->kind == c.kind);
        f.press(c.button, true);
        CHECK(run.join());
        f.press(c.button, false);
        CHECK(result == c.expected);
        CHECK(sendEnd(f) == -1);  // END already went out
    }
}

// Without anything to draw the dialog is answered as Cancel at once and touches neither the pad nor DRAWING_BEGIN.
void testNoDrawerAnswersCancel() {
    for (const tox2::SaveDialogDrawer drawer : {tox2::SaveDialogDrawer::None, tox2::SaveDialogDrawer::Failed}) {
        DialogFixture f(false);
        f.board.setDrawer(drawer);
        std::optional<std::int32_t> row = 99;
        tox2::SaveMessageResult yes_no = tox2::SaveMessageResult::Failed;
        tox2::SaveMessageResult ok = tox2::SaveMessageResult::Failed;
        SystemView after;
        GuestRun run(f.board, [&] {
            row = f.driver->showList({contentWithRows(3), 1});
            yes_no = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo));
            ok = f.driver->showMessage(contentWithRows(0, DialogKind::Ok));
            after.intercepted = tox2::padIntercepted();
            after.dialog_open = tox2::saveDialogOpen();
            after.begin = tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0);
        });
        CHECK(run.join());
        CHECK(!row.has_value());
        CHECK(yes_no == tox2::SaveMessageResult::No);
        CHECK(ok == tox2::SaveMessageResult::Ok);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == 0);
        CHECK(f.board.read().content == nullptr);
    }
}

// The overlay can fail while a dialog waits. The driver must notice within a few ticks, answer as Cancel and still
// release the pad and DRAWING_BEGIN.
void testDrawerFailsDuringWait() {
    for (const DialogKind kind : {DialogKind::List, DialogKind::YesNo}) {
        DialogFixture f;
        std::optional<std::int32_t> row = 99;
        tox2::SaveMessageResult message = tox2::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] {
            if (kind == DialogKind::List) {
                row = f.driver->showList({contentWithRows(2), 0});
            } else {
                message = f.driver->showMessage(contentWithRows(0, kind));
            }
        });
        CHECK(f.waitForDialog());
        CHECK(viewSystem(f, false).intercepted);
        const auto failed_at = Clock::now();
        f.board.setDrawer(tox2::SaveDialogDrawer::Failed);
        const std::uint64_t serial_at_failure = f.board.read().serial;
        CHECK(run.join());
        // The driver notices on its next wake: at most one publish already in flight, then the clear. Counting
        // publishes keeps this independent of machine load; the wall-time bound only guards against a hang.
        CHECK(f.board.read().serial <= serial_at_failure + 2);
        CHECK(run.finishedAt() - failed_at < std::chrono::milliseconds(1000));
        if (kind == DialogKind::List) {
            CHECK(!row.has_value());
        } else {
            CHECK(message == tox2::SaveMessageResult::No);
        }
        CHECK(f.board.read().content == nullptr);
        const SystemView after = viewSystem(f, false);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(sendEnd(f) == -1);  // END was sent
    }
}

// Another system dialog holds DRAWING_BEGIN. A message cannot be shown; a list opens anyway and leaves that BEGIN
// alone (description 2.1).
void testBeginOutstanding() {
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0) == 0); }));
        tox2::SaveMessageResult result = tox2::SaveMessageResult::Ok;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo)); });
        CHECK(run.join());
        CHECK(result == tox2::SaveMessageResult::Failed);
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // still the other dialog's
        CHECK(f.board.read().content == nullptr);
        CHECK(sendEnd(f) == 0);
    }
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(tox2::queueSysutilEvent(tox2::kSysutilDrawingBegin, 0) == 0); }));
        std::optional<std::int32_t> row = 99;
        GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(2), 0}); });
        CHECK(f.waitForFadeIn());
        const SystemView during = viewSystem(f, false);
        CHECK(during.intercepted && during.dialog_open);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);
        CHECK(row == std::optional<std::int32_t>(0));
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // no END was sent for a BEGIN the dialog did not make
        CHECK(sendEnd(f) == 0);
    }
}

// The game's own cellMsgDialogOpen2 is refused while a save dialog is open, and accepted once it closed.
void testMsgDialogBusy() {
    DialogFixture f;
    std::vector<std::uint8_t> memory(0x400);
    std::memcpy(memory.data() + 0x100, "hello", 6);
    const tox2::HleEntry* open2 = nullptr;
    for (const tox2::HleEntry& entry : tox2::cellMsgDialogHandlers()) {
        if (std::string_view(entry.name) == "cellMsgDialogOpen2") open2 = &entry;
    }
    CHECK(open2 != nullptr);
    if (open2 == nullptr) return;
    auto callOpen2 = [&] {
        Tox2PpuContext ctx{};
        ctx.gpr[3] = 2u << 4;  // OK button
        ctx.gpr[4] = 0x100;
        open2->handler(&ctx, memory.data());
        return static_cast<std::uint32_t>(ctx.gpr[3]);
    };

    std::optional<std::int32_t> row = 99;
    GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(1), 0}); });
    CHECK(f.waitForFadeIn());

    // BEGIN is outstanding too, so the result alone cannot tell the two refusals apart; the DRAWING_BEGIN error
    // line only appears when the handler got as far as asking for it.
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) {
        std::lock_guard lock(lines_mutex);
        lines.emplace_back(line);
    });
    std::uint32_t during = 0;
    CHECK(underGuestLock(f.board, [&] { during = callOpen2(); }));
    tox2::setLogSink({});
    CHECK(during == tox2::kSysutilErrorBusy);
    bool asked_for_begin = false;
    for (const std::string& line : lines) asked_for_begin |= line.find("second DRAWING_BEGIN") != std::string::npos;
    CHECK(!asked_for_begin);

    f.press(PadButton::Circle, true);
    CHECK(run.join());
    f.press(PadButton::Circle, false);
    std::uint32_t after = 0xffffffff;
    CHECK(underGuestLock(f.board, [&] { after = callOpen2(); }));
    CHECK(after == 0);
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
    testInteractiveList(tox2::PpuThreadsMode::Legacy);
    testInteractiveList(tox2::PpuThreadsMode::Split1);
    testInteractiveList(tox2::PpuThreadsMode::Split2);
    testInteractiveMessage();
    testNoDrawerAnswersCancel();
    testDrawerFailsDuringWait();
    testBeginOutstanding();
    testMsgDialogBusy();
    std::printf("save dialog tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

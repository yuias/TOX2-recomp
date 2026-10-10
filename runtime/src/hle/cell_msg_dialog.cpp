// HLE handlers of the cellMsgDialog imports (exported by cellSysutil): Open2, Close,
// ProgressBarReset and ProgressBarInc. Behaviour follows RPCS3 `Emu/Cell/Modules/cellMsgDialog.cpp`
// (function of the same name), its path without a display manager. Handlers run under the guest
// lock, so the dialog state needs no mutex.
//
// Headless policy (RPCS3 waits for the user or a GUI; nothing here can):
//  - A dialog with buttons answers itself after a short delay: OK for an OK dialog, and the
//    configured answer for a Yes/No dialog (YES unless setDialogAnswer says otherwise; a game's
//    Yes/No prompts are mostly "continue / retry / overwrite?", and answering No usually leaves the
//    boot path or ends the game).
//  - A dialog without buttons stays open until cellMsgDialogClose.
//  - Every dialog's type and text are logged (the guest text is UTF-8).
//
// Timers need no thread: a closure queued on the sysutil queue polls the deadlines, so completion
// is observed by the guest thread that pumps cellSysutilCheckCallback, as in RPCS3 (the dialog's
// callback runs there).
//
// Deviations from RPCS3: cellMsgDialogOpen2 never blocks, a dialog has no overlay, and a failed
// DRAWING_BEGIN leaves no dialog behind. The progress bars keep no value; only their index is
// validated.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellSysutil";

constexpr std::uint32_t kErrorParam = 0x8002b301;      // CELL_MSGDIALOG_ERROR_PARAM
constexpr std::uint32_t kErrorNotOpened = 0x8002b302;  // CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED
constexpr std::uint32_t kStringSize = 512;             // CELL_MSGDIALOG_STRING_SIZE

// CELL_MSGDIALOG_TYPE_* bit fields.
constexpr std::uint32_t kTypeSeNormal = 1u << 0;
constexpr std::uint32_t kTypeBgInvisible = 1u << 2;
constexpr std::uint32_t kTypeButtonShift = 4;
constexpr std::uint32_t kTypeButtonMask = 7;
constexpr std::uint32_t kTypeDisableCancel = 1u << 7;
constexpr std::uint32_t kTypeCursorShift = 8;
constexpr std::uint32_t kTypeCursorMask = 3;
constexpr std::uint32_t kTypeBarShift = 12;
constexpr std::uint32_t kTypeBarMask = 3;
constexpr std::uint32_t kTypeValidMask = 0x33f7;  // RPCS3 rejects `type & -0x33f8`

constexpr std::uint32_t kButtonNone = 0;
constexpr std::uint32_t kButtonYesNo = 1;
constexpr std::uint32_t kButtonOk = 2;

// Values of the callback's buttonType argument (CELL_MSGDIALOG_BUTTON_*).
constexpr std::int32_t kResultNone = -1;
constexpr std::int32_t kResultYes = 1;  // also OK
constexpr std::int32_t kResultNo = 2;

using Clock = std::chrono::steady_clock;

struct Dialog {
    bool open = false;
    std::uint32_t id = 0;  // distinguishes dialogs for the poll closures
    std::uint32_t type = 0;
    std::uint32_t callback = 0;
    std::uint32_t userdata = 0;
    std::optional<Clock::time_point> answer_at;  // auto-answer of a dialog with buttons
    std::int32_t answer = kResultNone;
    std::optional<Clock::time_point> close_at;  // cellMsgDialogClose
};

Dialog g_dialog;
std::uint32_t g_next_id = 0;
std::int32_t g_yes_no_answer = kResultYes;
std::chrono::milliseconds g_answer_delay{200};

std::uint32_t buttonType(std::uint32_t type) { return (type >> kTypeButtonShift) & kTypeButtonMask; }
std::uint32_t barCount(std::uint32_t type) { return (type >> kTypeBarShift) & kTypeBarMask; }

std::string describeType(std::uint32_t type) {
    const char* buttons = "no buttons";
    switch (buttonType(type)) {
        case kButtonYesNo: buttons = "yes/no"; break;
        case kButtonOk: buttons = "ok"; break;
        default: break;
    }
    return std::format("{}, {}{}{}, {} bar(s)", buttons, (type & kTypeSeNormal) ? "normal" : "error",
                       (type & kTypeBgInvisible) ? ", bg invisible" : "",
                       (type & kTypeDisableCancel) ? ", cancel disabled" : "", barCount(type));
}

// Ends the dialog (RPCS3 `on_close`): DRAWING_END to the system callbacks, then the dialog's own
// callback, both on the next cellSysutilCheckCallback.
void finish(std::int32_t result) {
    logInfo("tox2: msgdialog: closed with button {}", result);
    const std::uint32_t callback = g_dialog.callback;
    const std::uint32_t userdata = g_dialog.userdata;
    g_dialog = {};
    queueSysutilEvent(kSysutilDrawingEnd, 0);
    if (callback != 0) {
        queueSysutilCall([callback, userdata, result](Tox2PpuContext* ctx, std::uint8_t* base) {
            callGuestFunction(ctx, base, callback,
                              {static_cast<std::uint64_t>(static_cast<std::int64_t>(result)), userdata});
        });
    }
}

// Re-queues itself until the dialog it was started for ends, so each cellSysutilCheckCallback
// looks at the deadlines once.
void poll(std::uint32_t id) {
    queueSysutilCall([id](Tox2PpuContext*, std::uint8_t*) {
        if (!g_dialog.open || g_dialog.id != id) return;
        const Clock::time_point now = Clock::now();
        const bool answer_due = g_dialog.answer_at && *g_dialog.answer_at <= now;
        const bool close_due = g_dialog.close_at && *g_dialog.close_at <= now;
        // A button press that precedes the close timer wins (RPCS3: on_close is a one-shot state swap).
        if (answer_due && (!close_due || *g_dialog.answer_at <= *g_dialog.close_at)) {
            finish(g_dialog.answer);
        } else if (close_due) {
            finish(kResultNone);
        } else {
            poll(id);
        }
    });
}

// cellMsgDialogOpen2(u32 type, const char* msgString, CellMsgDialogCallback callback,
//                    void* userData, void* extParam)
void open2(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t type = argU32(ctx, 0);
    const std::uint32_t text_addr = argU32(ctx, 1);
    const std::uint32_t callback = argU32(ctx, 2);
    const std::uint32_t userdata = argU32(ctx, 3);

    if (text_addr == 0 || (type & ~kTypeValidMask) != 0) {
        ctx->gpr[3] = kErrorParam;
        return;
    }
    const std::string text = readGuestString(base, text_addr, kStringSize);
    if (text.size() >= kStringSize) {
        ctx->gpr[3] = kErrorParam;
        return;
    }

    const std::uint32_t cursor = (type >> kTypeCursorShift) & kTypeCursorMask;
    const std::uint32_t bars = barCount(type);
    bool valid = false;
    switch (buttonType(type)) {
        case kButtonNone: valid = cursor == 0 && bars <= 2; break;
        case kButtonYesNo: valid = cursor <= 1 && bars == 0; break;
        case kButtonOk: valid = cursor == 0 && bars == 0; break;
        default: break;
    }
    if (!valid) {
        ctx->gpr[3] = kErrorParam;
        return;
    }

    if (g_dialog.open) {
        ctx->gpr[3] = kSysutilErrorBusy;
        return;
    }
    const std::int32_t drawing = queueSysutilEvent(kSysutilDrawingBegin, 0);
    if (drawing < 0) {
        ctx->gpr[3] = static_cast<std::uint32_t>(drawing);
        return;
    }

    g_dialog = {};
    g_dialog.open = true;
    g_dialog.id = ++g_next_id;
    g_dialog.type = type;
    g_dialog.callback = callback;
    g_dialog.userdata = userdata;
    if (buttonType(type) != kButtonNone) {
        g_dialog.answer = buttonType(type) == kButtonOk ? kResultYes : g_yes_no_answer;
        g_dialog.answer_at = Clock::now() + g_answer_delay;
    }
    logInfo("tox2: msgdialog: open type {:#x} ({}) callback {:#x} userdata {:#x}: {}", type, describeType(type),
            callback, userdata, text);
    if (g_dialog.answer_at) {
        logInfo("tox2: msgdialog: auto-answer {} in {} ms", g_dialog.answer, g_answer_delay.count());
    }
    poll(g_dialog.id);
    ctx->gpr[3] = kCellOk;
}

// cellMsgDialogClose(f32 delay): `delay` in milliseconds.
void closeDialog(Tox2PpuContext* ctx, std::uint8_t*) {
    if (!g_dialog.open) {
        ctx->gpr[3] = kErrorNotOpened;
        return;
    }
    const float delay = std::max(static_cast<float>(ctx->fpr[1].f64), 0.0f);
    g_dialog.close_at =
            Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float, std::milli>(delay));
    logInfo("tox2: msgdialog: close requested in {} ms", delay);
    ctx->gpr[3] = kCellOk;
}

// Shared by the two progress bar handlers: a dialog must be open and the index within its bars.
bool checkBar(Tox2PpuContext* ctx, std::uint32_t index) {
    if (!g_dialog.open) {
        ctx->gpr[3] = kErrorNotOpened;
        return false;
    }
    if (index >= barCount(g_dialog.type)) {
        ctx->gpr[3] = kErrorParam;
        return false;
    }
    return true;
}

// cellMsgDialogProgressBarReset(u32 progressBarIndex)
void progressBarReset(Tox2PpuContext* ctx, std::uint8_t*) {
    if (!checkBar(ctx, argU32(ctx, 0))) return;
    ctx->gpr[3] = kCellOk;
}

// cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta)
void progressBarInc(Tox2PpuContext* ctx, std::uint8_t*) {
    if (!checkBar(ctx, argU32(ctx, 0))) return;
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x20543730, "cellMsgDialogClose", closeDialog},
        {kModule, 0x7603d3db, "cellMsgDialogOpen2", open2},
        {kModule, 0x7bc2c8a8, "cellMsgDialogProgressBarReset", progressBarReset},
        {kModule, 0x94862702, "cellMsgDialogProgressBarInc", progressBarInc},
};

}  // namespace

std::span<const HleEntry> cellMsgDialogHandlers() { return kHandlers; }

bool setDialogAnswer(std::string_view answer) {
    if (answer == "yes" || answer == "ok") {
        g_yes_no_answer = kResultYes;
    } else if (answer == "no") {
        g_yes_no_answer = kResultNo;
    } else {
        return false;
    }
    return true;
}

void setDialogAnswerDelay(std::chrono::milliseconds delay) { g_answer_delay = delay; }

void resetCellMsgDialog() {
    g_dialog = {};
    g_next_id = 0;
    g_yes_no_answer = kResultYes;
    g_answer_delay = std::chrono::milliseconds(200);
}

}  // namespace tox2

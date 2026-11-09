// Wrappers around the game's per-frame field HUD draw tasks. The task dispatcher (fn_0027a35c) calls each through its
// descriptor and ignores the return value, and restores r2 itself, so returning at once skips a task cleanly. A draw
// list cannot tell these draws from the world's: the same shaders draw the HUD banners and world sprites.
//
// A hidden task does not run at all, so whatever else its body advances stops until the HUD is shown again.
// TaskDrawNavigationMap has a time-driven value at +0x738 of its object, which resumes from where it stopped, and
// float stores into another object whose readers are not known. Tasks whose body drains a queue or may hold up the game are not wrapped: TaskRenderInformation (the notice queue),
// TaskRenderItemGetFramework and TaskDrawNextMission (not analysed to the end).

#include "tox2/field_hud.h"
#include "tox2/ppu.h"

#define TOX2_FIELD_HUD_TASK(entry)         \
    TOX2_PPU_FUNC(fn_##entry##_generated); \
    TOX2_PPU_FUNC(fn_##entry) {            \
        if (tox2FieldHudHidden()) return;  \
        TOX2_TAIL(fn_##entry##_generated); \
    }

TOX2_FIELD_HUD_TASK(002bc734)  // TaskDrawNavigationMap
TOX2_FIELD_HUD_TASK(002f6c84)  // TaskDrawChapterInformation
TOX2_FIELD_HUD_TASK(00306724)  // TaskDrawControllerInformation
TOX2_FIELD_HUD_TASK(003cd424)  // TaskDrawSearchMode
TOX2_FIELD_HUD_TASK(003048e4)  // TaskRenderChatNavi

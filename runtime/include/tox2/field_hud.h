#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Whether the field HUD is hidden. The overlay sets it on the renderer's thread and the guest task wrappers
// (runtime/src/ppu_overrides/field_hud.c) read it on the game's thread.
bool tox2FieldHudHidden(void);
void tox2SetFieldHudHidden(bool hidden);

#ifdef __cplusplus
}
#endif

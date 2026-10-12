#pragma once

#include "overlay/overlay_model.h"

namespace tox2 {

// Draws the overlay's single "tox2" window with its four sections (stats, settings, capture, RSX
// unimplemented). Must be called between ImGui::NewFrame and ImGui::Render, on the thread that owns
// `state`. User actions go through the provider callbacks in `sources` (a missing provider disables the
// control) and `state.dump_requested`; the cached values in `state` are refreshed by OverlayState::refresh.
void drawOverlayWindow(OverlayState& state, const OverlaySources& sources);

}  // namespace tox2

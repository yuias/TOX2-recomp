#pragma once

// Entry points into ImGui's macOS backend (externals/imgui/backends/imgui_impl_osx.mm) for the debug
// overlay. The backend keeps its event handler static and feeds itself from an NSEvent monitor and
// application notifications, which would touch the ImGui context from the main thread while the
// renderer's thread builds a frame. The bridge compiles the backend source into its own translation unit,
// takes those self-feeding paths out and lets the overlay pass events in under its mutex. No Objective-C
// types appear here so that C++ code can include it. All functions run on the main thread only, with the
// ImGui context current.

// `ns_view` is the window's content NSView. False when the backend does not start.
bool tox2ImGuiOsxInit(void* ns_view);
void tox2ImGuiOsxShutdown();

// Feeds one NSEvent to ImGui. True when ImGui wants the device the event came from (mouse or keyboard).
// `ns_view` is the NSView whose bounds are the ImGui display area.
bool tox2ImGuiOsxHandleEvent(void* ns_event, void* ns_view);

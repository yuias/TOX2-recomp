#pragma once

// What the debug overlay needs from the window system: the display scale, the platform ImGui backend and
// the translation of window messages. Implemented per platform (overlay_platform_win32.cpp,
// overlay_platform_macos.mm). `native_window` is the HWND on Windows and the content NSView on macOS.
// The ImGui context belongs to DebugOverlay, which serialises these calls with its mutex.

#include <optional>

#include "render/vk_window.h"

namespace tox2::overlay_platform {

// Pixels per layout unit of the window: the DPI scale on Windows, the backing scale on macOS. 1 when
// `native_window` is null. Renderer's thread.
float windowScale(void* native_window);

// False: ImGui coordinates are pixels (Windows). True: they are points and the framebuffer is `scale`
// times larger (macOS).
bool usesPointCoordinates();

// Starts the platform backend for the window. The ImGui context is current. Renderer's thread, with the
// overlay mutex not held.
bool initBackend(void* native_window);
void shutdownBackend();

std::optional<KeyMessage> keyOf(const WindowMessage& message);
bool isFocusLoss(const WindowMessage& message);
// A key press, which is withheld from the pad while ImGui wants the keyboard.
bool isKeyDown(const WindowMessage& message);

// Window thread, overlay mutex held. Feeds the message to the backend and sets `route.handled` and
// `route.result` when the backend consumed it.
void forwardMessage(const WindowMessage& message, MessageRoute& route);

}  // namespace tox2::overlay_platform

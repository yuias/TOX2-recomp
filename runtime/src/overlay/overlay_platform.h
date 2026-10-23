#pragma once

// What the debug overlay needs from the window system: the display scale, the platform ImGui backend and
// the translation of window messages. Implemented per platform (overlay_platform_win32.cpp,
// overlay_platform_macos.mm). `native_window` is the HWND on Windows and the content NSView on macOS.
// The ImGui context belongs to DebugOverlay; ImGui calls happen under imguiMutex() with it current.

#include <optional>

#include "render/vk_window.h"

struct ImGuiContext;

namespace tox2::overlay_platform {

// Pixels per layout unit of the window: the DPI scale on Windows, the backing scale on macOS. 1 when
// `native_window` is null. Renderer's thread.
float windowScale(void* native_window);

// False: ImGui coordinates are pixels (Windows). True: they are points and the framebuffer is `scale`
// times larger (macOS).
bool usesPointCoordinates();

// Starts the platform backend for the window. Renderer's thread, under imguiMutex() with the overlay's
// context current. On macOS the work runs on the main thread while the caller waits; that is safe because
// the window thread takes imguiMutex() only once the overlay is initialised.
bool initBackend(void* native_window);
// Stops it again. Renderer's thread, with imguiMutex() not held (the macOS backend stops on the main
// thread, which may be waiting for the lock); `context` is made current for the call.
void shutdownBackend(ImGuiContext* context);

std::optional<KeyMessage> keyOf(const WindowMessage& message);
bool isFocusLoss(const WindowMessage& message);
// A key press, which is withheld from the pad while ImGui wants the keyboard.
bool isKeyDown(const WindowMessage& message);

// Window thread, under imguiMutex() with the overlay's context current. Feeds the message to the backend and sets
// `route.handled` and `route.result` when the backend consumed it.
void forwardMessage(const WindowMessage& message, MessageRoute& route);

}  // namespace tox2::overlay_platform

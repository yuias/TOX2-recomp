#include "overlay/overlay_platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "overlay/imgui_context.h"

// The header keeps this declaration under `#if 0` so that it does not pull in <windows.h>.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tox2::overlay_platform {

float windowScale(void* native_window) {
    return native_window != nullptr ? ImGui_ImplWin32_GetDpiScaleForHwnd(native_window) : 1.0f;
}

bool usesPointCoordinates() { return false; }

bool initBackend(void* native_window) { return ImGui_ImplWin32_Init(native_window); }

void shutdownBackend(ImGuiContext* context) {
    ImGuiContextScope scope(context);
    ImGui_ImplWin32_Shutdown();
}

std::optional<KeyMessage> keyOf(const WindowMessage& message) {
    return keyMessageFromWin32(message.message, message.wparam, static_cast<std::uint64_t>(message.lparam));
}

bool isFocusLoss(const WindowMessage& message) { return message.message == WM_KILLFOCUS; }

bool isKeyDown(const WindowMessage& message) {
    return message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
}

void forwardMessage(const WindowMessage& message, MessageRoute& route) {
    // The handler calls SetCapture, TrackMouseEvent and GetKeyState, which only work on the thread that
    // owns the window, so the input is fed from here and not queued to the renderer's thread.
    const LRESULT result =
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(message.hwnd), static_cast<UINT>(message.message),
                                           static_cast<WPARAM>(message.wparam), static_cast<LPARAM>(message.lparam));
    route.handled = result != 0;
    route.result = static_cast<std::intptr_t>(result);
}

}  // namespace tox2::overlay_platform

// Compiles ImGui's macOS backend into this translation unit (it needs -fobjc-arc, see cmake/Overlay.cmake)
// and reaches its static handler and backend data. The backend itself is not built as a separate source.

#include "imgui_osx_bridge.h"

#include "imgui_impl_osx.mm"

bool tox2ImGuiOsxInit(void* ns_view) {
    NSView* view = (__bridge NSView*)ns_view;
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    // The backend installs NSPasteboard and NSTextInputContext callbacks that ImGui calls from the thread
    // that builds the frame, which is not the main thread. The overlay needs neither, so ImGui keeps the
    // callbacks it had.
    const auto set_clipboard = platform_io.Platform_SetClipboardTextFn;
    const auto get_clipboard = platform_io.Platform_GetClipboardTextFn;
    const auto set_ime_data = platform_io.Platform_SetImeDataFn;
    // The backend's text input view takes the first responder when it enters the window.
    NSResponder* first_responder = view.window.firstResponder;

    if (!ImGui_ImplOSX_Init(view)) return false;

    platform_io.Platform_SetClipboardTextFn = set_clipboard;
    platform_io.Platform_GetClipboardTextFn = get_clipboard;
    platform_io.Platform_SetImeDataFn = set_ime_data;

    ImGui_ImplOSX_Data* bd = ImGui_ImplOSX_GetBackendData();
    // Application activation changes reach ImGui as focus events through the window instead, under the
    // overlay's mutex. The observer object stays so that the backend's shutdown can remove it again.
    [[NSNotificationCenter defaultCenter] removeObserver:bd->Observer];
    // Events reach ImGui through tox2ImGuiOsxHandleEvent. The local monitor would feed them on the main
    // thread unlocked, and also while the overlay is hidden.
    if (bd->Monitor != nil) {
        [NSEvent removeMonitor:bd->Monitor];
        bd->Monitor = nil;
    }
    // The text input view would feed keys and characters to ImGui on its own and keep the window's key
    // handling from the game window. Text reaches ImGui through tox2ImGuiOsxHandleEvent.
    [bd->KeyEventResponder removeFromSuperview];
    if (first_responder != nil) (void)[view.window makeFirstResponder:first_responder];
    return true;
}

void tox2ImGuiOsxShutdown() {
    if (ImGui_ImplOSX_GetBackendData() == nullptr) return;
    // Init handed a retained reference to the window to the main viewport and the backend never gives it back.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport->PlatformHandle != nullptr) {
        (void)CFBridgingRelease(viewport->PlatformHandle);
        viewport->PlatformHandle = nullptr;
        viewport->PlatformHandleRaw = nullptr;
    }
    ImGui_ImplOSX_Shutdown();
}

bool tox2ImGuiOsxHandleEvent(void* ns_event, void* ns_view) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    NSView* view = (__bridge NSView*)ns_view;
    const bool wanted = ImGui_ImplOSX_HandleEvent(event, view);

    // Typed text. The backend takes it from the text input view, which is not used here, so there is no
    // input method: the characters of the key itself, without the function keys' private-use codes (which
    // ImGui drops) and without shortcuts.
    ImGuiIO& io = ImGui::GetIO();
    if (event.type == NSEventTypeKeyDown && io.WantTextInput &&
        (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0) {
        const char* text = event.characters.UTF8String;
        if (text != nullptr) io.AddInputCharactersUTF8(text);
    }
    return wanted;
}

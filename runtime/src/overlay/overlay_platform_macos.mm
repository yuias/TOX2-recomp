// The overlay's window-system seam on macOS. Mouse and keyboard reach ImGui through the Cocoa window's
// event routing and imgui_impl_osx (see imgui_osx_bridge.h); the overlay itself only reads F1 and Tab
// through the key the window decoded.

#import <Cocoa/Cocoa.h>

#include "overlay/overlay_platform.h"

#include <dispatch/dispatch.h>
#include <pthread.h>

#include "imgui.h"
#include "overlay/imgui_context.h"
#include "overlay/imgui_osx_bridge.h"

namespace tox2::overlay_platform {

namespace {

// True between a successful initBackend and shutdownBackend. Only the main thread touches it, because
// both functions run their work there and the window delivers its events there.
bool g_backend_active = false;

// Runs `work` on the main thread, which serves the window's event loop while the renderer runs on another
// thread. The caller must not hold a lock that the main thread can wait for: the window thread takes
// imguiMutex() inside its event dispatch once the overlay is initialised.
void runOnMain(dispatch_block_t work) {
    if (pthread_main_np() != 0) {
        work();
    } else {
        dispatch_sync(dispatch_get_main_queue(), work);
    }
}

bool isKeyEventType(NSEventType type) {
    return type == NSEventTypeKeyDown || type == NSEventTypeKeyUp || type == NSEventTypeFlagsChanged;
}

}  // namespace

float windowScale(void* native_window) {
    if (native_window == nullptr) return 1.0f;
    __block float scale = 1.0f;
    runOnMain(^{
      NSView* view = (__bridge NSView*)native_window;
      const CGFloat factor = view.window.backingScaleFactor;
      if (factor >= 1.0) scale = static_cast<float>(factor);
    });
    return scale;
}

bool usesPointCoordinates() { return true; }

bool initBackend(void* native_window) {
    __block bool started = false;
    runOnMain(^{
      started = tox2ImGuiOsxInit(native_window);
      g_backend_active = started;
    });
    return started;
}

void shutdownBackend(ImGuiContext* context) {
    runOnMain(^{
      if (!g_backend_active) return;
      g_backend_active = false;
      ImGuiContextScope scope(context);
      tox2ImGuiOsxShutdown();
    });
}

std::optional<KeyMessage> keyOf(const WindowMessage& message) {
    return message.kind == WindowEventKind::Key ? message.key : std::nullopt;
}

bool isFocusLoss(const WindowMessage& message) { return message.kind == WindowEventKind::FocusLost; }

bool isKeyDown(const WindowMessage& message) {
    return message.kind == WindowEventKind::Key && message.key.has_value() && message.key->down;
}

void forwardMessage(const WindowMessage& message, MessageRoute& route) {
    // A message can arrive after the backend went away: the overlay checks its state before it takes the
    // mutex that shutdown also needs.
    if (!g_backend_active) return;
    switch (message.kind) {
        case WindowEventKind::FocusGained:
        case WindowEventKind::FocusLost:
            // Replaces the backend's application activation observers, which are removed at start.
            ImGui::GetIO().AddFocusEvent(message.kind == WindowEventKind::FocusGained);
            return;
        case WindowEventKind::Other:
        case WindowEventKind::Key: break;
    }
    if (message.event == nullptr) return;
    const bool wanted = tox2ImGuiOsxHandleEvent(message.event, message.view);
    // Mouse and scroll events that ImGui wants do not reach the window. Keys always go on: the pad has to
    // see key-ups, and the overlay withholds the key-downs through isKeyDown instead.
    NSEvent* event = (__bridge NSEvent*)message.event;
    if (wanted && !isKeyEventType(event.type)) route.handled = true;
}

}  // namespace tox2::overlay_platform

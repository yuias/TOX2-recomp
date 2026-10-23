// The overlay's window-system seam on macOS. ImGui gets no mouse or keyboard input from the window yet:
// the backend calls are empty and the overlay only reads F1 and Tab through the key the window decoded.

#import <Cocoa/Cocoa.h>

#include "overlay/overlay_platform.h"

#include <dispatch/dispatch.h>
#include <pthread.h>

namespace tox2::overlay_platform {

float windowScale(void* native_window) {
    if (native_window == nullptr) return 1.0f;
    // The view and its window belong to the main thread, which serves the event loop while the renderer
    // runs on another thread.
    __block float scale = 1.0f;
    auto read = ^{
      NSView* view = (__bridge NSView*)native_window;
      const CGFloat factor = view.window.backingScaleFactor;
      if (factor >= 1.0) scale = static_cast<float>(factor);
    };
    if (pthread_main_np() != 0) {
        read();
    } else {
        dispatch_sync(dispatch_get_main_queue(), read);
    }
    return scale;
}

bool usesPointCoordinates() { return true; }

bool initBackend(void*) { return true; }

void shutdownBackend() {}

std::optional<KeyMessage> keyOf(const WindowMessage& message) {
    return message.kind == WindowEventKind::Key ? message.key : std::nullopt;
}

bool isFocusLoss(const WindowMessage& message) { return message.kind == WindowEventKind::FocusLost; }

bool isKeyDown(const WindowMessage& message) {
    return message.kind == WindowEventKind::Key && message.key.has_value() && message.key->down;
}

void forwardMessage(const WindowMessage&, MessageRoute&) {}

}  // namespace tox2::overlay_platform

#pragma once

// OS window plus the swapchain that presents the display buffer to it. The window is a Win32 window on its
// own UI thread, or a Cocoa window served by the process main thread (see runWithHostEventLoop). The
// Vulkan side (surface, swapchain, acquire, present) runs on the one thread that owns the renderer; only
// the window handle and the client size cross threads.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "render/vk_context.h"
#include "render/window_size.h"

namespace tox2 {

struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The largest rectangle with the aspect ratio aspect_width:aspect_height that fits an area, centred
// (rounded down, so the margins differ by at most one pixel). Empty when the area or the ratio is empty.
PixelRect letterboxRect(std::uint32_t area_width, std::uint32_t area_height, std::uint32_t aspect_width,
                        std::uint32_t aspect_height);

// The requested mode if offered, else MAILBOX, else IMMEDIATE, else FIFO. The chain exists because DLSS-G requires
// vsync off; with frame generation unloaded the request is FIFO, which every surface offers, so the off path never
// falls back. A request other than FIFO that ends at FIFO logs one warning per call (swapchain creation is rare); a
// request that ends at another mode than the requested one logs one info line naming both. Requesting FIFO gives FIFO
// without a line (a diagnostic).
VkPresentModeKHR choosePresentMode(VkPresentModeKHR requested, std::span<const VkPresentModeKHR> offered);

struct KeyMessage {
    std::uint32_t scan_code = 0;  // PC/AT set 1 make code, 0xE000 | code for extended keys
    bool down = false;
    bool repeat = false;  // the key was already down (auto-repeat)
};

#if defined(_WIN32)
// WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP -> KeyMessage; nullopt for any other message. The scan
// code is the physical key position, so it does not depend on the keyboard layout or on an IME. A zero
// scan code (some injected input) falls back to the virtual key in `wparam`.
std::optional<KeyMessage> keyMessageFromWin32(std::uint32_t message, std::uint64_t wparam, std::uint64_t lparam);

// A raw Win32 message, so this header stays free of windows.h.
struct WindowMessage {
    void* hwnd = nullptr;
    std::uint32_t message = 0;
    std::uint64_t wparam = 0;
    std::int64_t lparam = 0;
};
#else
enum class WindowEventKind : std::uint8_t { Other, Key, FocusGained, FocusLost };

// An event the Cocoa window is about to dispatch, so this header stays free of Objective-C types.
struct WindowMessage {
    void* view = nullptr;   // the window's content NSView
    void* event = nullptr;  // the NSEvent being dispatched; null for focus changes
    WindowEventKind kind = WindowEventKind::Other;
    std::optional<KeyMessage> key;  // set when kind == Key
};
#endif

// What PresenterOptions::on_message decided for one message.
struct MessageRoute {
    // Windows: the window procedure returns `result` at once. macOS: the event is dropped (`result` is unused).
    bool handled = false;
    std::intptr_t result = 0;
    bool withhold_key = false;  // a key message that on_key does not receive
};

// Runs `body` so that the process main thread serves the OS user interface where the OS requires it.
// Windows: calls body() on the calling thread. macOS: must be called on the main thread; runs body on a
// new thread while the main thread runs the event loop (NSApplication when `user_interface`, else a plain
// CFRunLoop), and returns after body returns. An exception that leaves body is rethrown here.
void runWithHostEventLoop(std::function<void()> body, bool user_interface);

// Processes pending UI events on the calling thread without blocking (tests that own the main thread).
// Windows: no-op (the window has its own thread).
void pumpHostEvents();

struct PresenterOptions {
    // Called on the UI thread when the user closes the window (on macOS also for Cmd+Q). The window is not
    // destroyed by it.
    std::function<void()> on_close;
    // The callbacks below run on the UI thread inside the window procedure (macOS: inside the window's
    // event dispatch): they must be cheap and must not block, or the window stops responding.
    // Called first for every message or event; lets a client (the debug overlay) consume it or keep key
    // messages from on_key.
    std::function<MessageRoute(const WindowMessage&)> on_message;
    // Called for every key message, including repeats and Alt combinations, unless on_message withheld it.
    std::function<void(const KeyMessage&)> on_key;
    // Called when the window loses keyboard focus (key-up messages are not delivered to it afterwards).
    std::function<void()> on_focus_lost;
    // The client size in physical pixels the window opens with, fitted to the monitor's work area.
    WindowSizePreset window_size = WindowSizePreset::Size720p;
    bool fullscreen = false;  // start in borderless fullscreen
    // Called on the UI thread after the user toggled fullscreen (Windows: Alt+Enter, F11; macOS: Ctrl+Cmd+F,
    // the green button); not for the start-up state.
    std::function<void(bool fullscreen)> on_fullscreen_changed;
    // Frame slots of the renderer: one acquire semaphore each.
    std::size_t frame_slots = 2;
};

class Presenter {
public:
    // An acquired swapchain image. The submit that writes it waits on `acquire_semaphore` and signals
    // `render_semaphore`; `present` then waits on the latter.
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        // A 2D view of `image` in `format`; null when the surface does not allow colour-attachment usage.
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
        VkSemaphore render_semaphore = VK_NULL_HANDLE;
        std::uint32_t index = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Opens the window and creates the surface on `context`, which must have been created with
    // VkContextOptions::window. Null with `error` set when a step fails. On macOS the window belongs to the
    // main thread: call this on the main thread or from the body of runWithHostEventLoop (it then waits
    // for the main thread), and destroy the presenter the same way.
    static std::unique_ptr<Presenter> create(VkContext& context, PresenterOptions options, std::string& error);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Acquires the next swapchain image, recreating the swapchain after a resize. False when there is
    // nothing to present to this time (minimised window, no image within the timeout, a failure): the
    // frame is then submitted without a swapchain image. The image is in an undefined layout; the caller
    // leaves it in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, or in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL while the DLSS-G plugin
    // is loaded (sl.dlss_g's pacer copies from its proxy swapchain images and expects that layout; measured, not
    // documented).
    bool acquire(std::size_t frame_slot, Target& target);

    // Number of frame slots acquire accepts (one acquire semaphore each).
    std::size_t frameSlots() const { return acquire_semaphores_.size(); }

    // Queues the presentation of an image acquired by `acquire` whose submit has been queued.
    void present(const Target& target);

    // Present mode for swapchains created from now on (FIFO until changed, so the off path is exactly today's). The
    // mode actually used comes from choosePresentMode. The presenter's thread.
    void setPresentMode(VkPresentModeKHR mode) { requested_present_mode_ = mode; }
    // Of the current swapchain (the last one created while there is none).
    VkPresentModeKHR presentMode() const { return present_mode_; }
    // Of the current swapchain; 0 without one.
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }
    // vkDeviceWaitIdle and destroys the swapchain now; the next acquire creates a new one. The presenter's thread.
    void releaseSwapchain();
    // Called on the presenter's thread just before an existing swapchain is destroyed or recreated, whatever the
    // reason (resize, out of date, releaseSwapchain); not for the presenter's own destruction.
    void setBeforeRecreate(std::function<void()> callback) { before_recreate_ = std::move(callback); }

    // The window's HWND, or the content NSView on macOS. Any thread; it is valid for the presenter's lifetime.
    void* nativeWindowHandle() const;

    // Any thread; read from atomics the UI thread keeps current.
    WindowStatus windowStatus() const;

    // Any thread; hands the request to the UI thread, which resizes the window (immediately when windowed; when
    // leaving fullscreen or when restored from minimised otherwise). Choosing the preset that is already set re-applies
    // it.
    void requestWindowSize(WindowSizePreset preset);

private:
    // The OS window. Its state is defined per platform (vk_window_win32.cpp, vk_window_macos.mm); the
    // contract below is all the presenter relies on.
    class Window {
    public:
        // Opens the window; null with `error` set when that fails.
        static std::unique_ptr<Window> create(const PresenterOptions& options, std::string& error);
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void* nativeHandle() const;                 // HWND / the content NSView*
        bool minimized() const;                     // any thread
        VkExtent2D clientSize() const;              // pixels, any thread
        WindowStatus status() const;                // any thread
        void requestSize(WindowSizePreset preset);  // any thread
        // Any thread. `error` is set when the result is not VK_SUCCESS.
        VkResult createSurface(const VkContext& context, VkSurfaceKHR* surface, std::string& error) const;

    private:
        class Impl;
        explicit Window(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };
    struct Stats {
        std::uint64_t acquires = 0;
        std::uint64_t presents = 0;
        std::uint64_t skipped = 0;
        std::uint64_t recreations = 0;
        std::chrono::nanoseconds acquire_total{0};
        std::chrono::nanoseconds acquire_max{0};
        std::chrono::nanoseconds present_total{0};
        std::chrono::nanoseconds present_max{0};
    };

    explicit Presenter(VkContext& context);
    bool ensureSwapchain();
    bool createSwapchain(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extent);
    void destroySwapchainResources();
    void notifyBeforeRecreate();
    void reportStats();

    VkContext& context_;
    std::unique_ptr<Window> window_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    VkPresentModeKHR requested_present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::function<void()> before_recreate_;
    bool needs_recreate_ = false;
    bool acquire_stalled_ = false;  // the last acquire timed out; acquire without waiting until one succeeds
    int swapchain_failures_ = 0;    // consecutive failed swapchain creations
    bool broken_ = false;           // a failure no retry can fix: acquire reports nothing to present to
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;         // per swapchain image; empty without colour-attachment usage
    std::vector<VkSemaphore> render_semaphores_;   // per swapchain image
    std::vector<VkSemaphore> acquire_semaphores_;  // per frame slot

    Stats stats_;  // since the last report
    std::chrono::steady_clock::time_point report_start_{};
};

}  // namespace tox2

#pragma once

// Win32 window on its own UI thread plus the swapchain that presents the display buffer to it. The
// Vulkan side (surface, swapchain, acquire, present) runs on the one thread that owns the renderer; only
// the window handle and the client size cross threads.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "render/vk_context.h"

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

struct KeyMessage {
    std::uint32_t scan_code = 0;  // PC/AT set 1 make code, 0xE000 | code for extended keys
    bool down = false;
    bool repeat = false;  // the key was already down (auto-repeat)
};

// WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP -> KeyMessage; nullopt for any other message. The scan
// code is the physical key position, so it does not depend on the keyboard layout or on an IME. A zero
// scan code (some injected input) falls back to the virtual key in `wparam`.
std::optional<KeyMessage> keyMessageFromWin32(std::uint32_t message, std::uint64_t wparam, std::uint64_t lparam);

struct PresenterOptions {
    // Called on the UI thread when the user closes the window. The window is not destroyed by it.
    std::function<void()> on_close;
    // Both callbacks run on the UI thread inside the window procedure: they must be cheap and must not
    // block, or the window stops responding.
    // Called for every key message, including repeats and Alt combinations.
    std::function<void(const KeyMessage&)> on_key;
    // Called when the window loses keyboard focus (key-up messages are not delivered to it afterwards).
    std::function<void()> on_focus_lost;
    std::uint32_t client_width = 1280;
    std::uint32_t client_height = 720;
    // Frame slots of the renderer: one acquire semaphore each.
    std::size_t frame_slots = 2;
};

class Presenter {
public:
    // An acquired swapchain image. The submit that writes it waits on `acquire_semaphore` and signals
    // `render_semaphore`; `present` then waits on the latter.
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
        VkSemaphore render_semaphore = VK_NULL_HANDLE;
        std::uint32_t index = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Opens the window and creates the surface on `context`, which must have been created with
    // VkContextOptions::window. Null with `error` set when a step fails.
    static std::unique_ptr<Presenter> create(VkContext& context, PresenterOptions options, std::string& error);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Acquires the next swapchain image, recreating the swapchain after a resize. False when there is
    // nothing to present to this time (minimised window, no image within the timeout, a failure): the
    // frame is then submitted without a swapchain image. The image is in an undefined layout; the caller
    // leaves it in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
    bool acquire(std::size_t frame_slot, Target& target);

    // Number of frame slots acquire accepts (one acquire semaphore each).
    std::size_t frameSlots() const { return acquire_semaphores_.size(); }

    // Queues the presentation of an image acquired by `acquire` whose submit has been queued.
    void present(const Target& target);

private:
    class Window;
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
    void reportStats();

    VkContext& context_;
    std::unique_ptr<Window> window_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    bool needs_recreate_ = false;
    bool acquire_stalled_ = false;  // the last acquire timed out; acquire without waiting until one succeeds
    int swapchain_failures_ = 0;    // consecutive failed swapchain creations
    bool broken_ = false;           // a failure no retry can fix: acquire reports nothing to present to
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> render_semaphores_;   // per swapchain image
    std::vector<VkSemaphore> acquire_semaphores_;  // per frame slot

    Stats stats_;  // since the last report
    std::chrono::steady_clock::time_point report_start_{};
};

}  // namespace tox2

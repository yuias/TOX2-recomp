#pragma once

// The in-window debug overlay: Dear ImGui drawn over the presented swapchain image. ImGui keeps one global
// context, guarded by `mutex_`. The window thread feeds it input from the window procedure; the renderer's
// thread builds the frame and records its draw commands. At most one DebugOverlay may exist at a time.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "overlay/overlay_model.h"
#include "render/present_overlay.h"
#include "render/vk_context.h"
#include "render/vk_window.h"

struct ImGuiContext;

namespace tox2 {

inline constexpr std::uint32_t kOverlayToggleScanCode = 0x3B;  // F1; not in the keyboard pad map

class DebugOverlay final : public PresentOverlay {
public:
    // Touches no Vulkan or ImGui state: the backends start lazily in the first record(), when the
    // presenter's surface format is known.
    DebugOverlay(VkContext& context, OverlaySources sources);
    // Waits for the device to go idle, then shuts the backends down. The window thread must be gone.
    ~DebugOverlay() override;
    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    // Window thread, from PresenterOptions::on_message.
    MessageRoute onWindowMessage(const WindowMessage& message);
    // Renderer's thread, before the first record(). Null means no Win32 input (tests).
    void attachWindow(void* hwnd);

    bool visible() const { return visible_.load(std::memory_order_relaxed); }  // any thread
    void setVisible(bool visible);                                             // any thread
    // Initialisation failed; the overlay stays off for the rest of the run.
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

    PresentOverlayRequests onFlip(const PresentFrameInfo& info) override;
    bool wantsDraw() const override { return visible() && !failed(); }
    void record(VkCommandBuffer cmd, const Presenter::Target& target) override;

private:
    bool initialize(const Presenter::Target& target);
    void shutdown();
    void fail(const char* what);

    VkContext& context_;
    OverlaySources sources_;
    OverlayState state_;  // renderer's thread only

    std::mutex mutex_;  // the ImGui context; see the file comment
    std::atomic<bool> visible_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> initialized_{false};  // set under mutex_ once the backends are up
    std::atomic<void*> hwnd_{nullptr};

    // Renderer's thread only (the backends hold pointers into these).
    ImGuiContext* imgui_ = nullptr;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
    bool win32_backend_ = false;
    bool vulkan_backend_ = false;
    std::chrono::steady_clock::time_point last_frame_time_{};
};

}  // namespace tox2

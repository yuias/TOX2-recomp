#pragma once

// The frame generation controller: loads and unloads the DLSS-G and Reflex plugins with the swapchain recreation that
// goes with it, gives every guest frame one Streamline frame token, sets the latency markers, and runs the thread that
// calls the Reflex sleep. Nothing here tags inputs or turns generation on yet.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <volk.h>

#include "render/frame_gen.h"
#include "render/streamline.h"
#include "render/vk_context.h"
#include "render/vk_window.h"

namespace tox2 {

struct FrameGenControllerOptions {
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;  // requested while the plugins are loaded
    bool reflex_low_latency = true;
    std::uint32_t cycle_seconds = 0;  // 0 = off; else the seconds between unloading and loading again
};

// Renderer thread only, except for the sleeper thread inside. The context and the presenter outlive the controller.
class FrameGenController {
public:
    // The context must support frame generation (it owns a Streamline instance that was asked for it).
    FrameGenController(VkContext& context, Presenter& presenter, const FrameGenControllerOptions& options);
    // Stops and joins the sleeper first: it calls into Streamline.
    ~FrameGenController();
    FrameGenController(const FrameGenController&) = delete;
    FrameGenController& operator=(const FrameGenController&) = delete;

    // The requested target. A change between off and on unloads or loads the plugins at once: it waits for the device
    // and recreates the swapchain, so call it only between frames (after a flip was presented).
    void setTarget(FrameGenTarget target);
    // True while the plugins are loaded.
    bool loaded() const { return loaded_; }

    // Once per flip after it was presented: the --fg-cycle toggle and the periodic debug line.
    void tick();

    // Per guest frame, in this order. All do nothing while the plugins are unloaded.
    void beginFrame(std::uint32_t index);  // the frame's first command; SimulationStart
    void flipEntered();                    // recordFlip entry; SimulationEnd, RenderSubmitStart
    void submitted();                      // after vkQueueSubmit2; RenderSubmitEnd
    void presentStarted();                 // PresentStart
    void presentFinished();                // PresentEnd
    // Token for the next frame, right after the present, and the Reflex sleep for it on the sleeper thread.
    void frameDone(std::uint32_t next_index);

private:
    FrameGenTarget effectiveTarget() const { return cycle_off_ ? FrameGenTarget::Off : requested_; }
    void apply();
    void changeLoadState(bool on, FrameGenTarget target);
    void failLoad(const std::string& reason);
    void logSleepWindow();
    void startSleeper();
    void stopSleeper();
    void sleeperMain();
    void postSleep(const void* handle);

    VkContext& context_;
    Presenter& presenter_;
    Streamline& streamline_;
    FrameGenControllerOptions options_;

    FrameGenTarget requested_ = FrameGenTarget::Off;
    bool loaded_ = false;
    bool failed_ = false;  // loading failed; stays unloaded until the requested target changes
    bool cycle_off_ = false;
    std::optional<std::chrono::steady_clock::time_point> cycle_deadline_;
    std::optional<std::chrono::steady_clock::time_point> log_deadline_;

    // Sleeper thread. A handle is only valid for the next few frames, so only the newest one is kept.
    std::thread sleeper_;
    std::mutex mutex_;
    std::condition_variable wake_;
    const void* pending_ = nullptr;
    bool stop_ = false;
    // Since the last debug line; guarded by mutex_.
    std::uint64_t window_sleeps_ = 0, window_dropped_ = 0;
    std::chrono::nanoseconds window_total_{0}, window_max_{0};
};

}  // namespace tox2

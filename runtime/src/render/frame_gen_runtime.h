#pragma once

// The frame generation controller: loads and unloads the DLSS-G and Reflex plugins with the swapchain recreation that
// goes with it, gives every guest frame one Streamline frame token, sets the latency markers, runs the thread that
// calls the Reflex sleep, and decides for every presented frame whether DLSS-G generates frames around it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <volk.h>

#include "render/frame_gen.h"
#include "render/present_overlay.h"
#include "render/streamline.h"
#include "render/vk_context.h"
#include "render/vk_window.h"

namespace tox2 {

struct FrameGenControllerOptions {
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_MAILBOX_KHR;  // requested while the plugins are loaded
    bool reflex_low_latency = true;
    bool only_generated = false;      // present only the generated frames (diagnostic)
    std::uint32_t cycle_seconds = 0;  // 0 = off; else the seconds between unloading and loading again
};

// What the DLAA tap did for a frame.
enum class FrameGenInputs : std::uint8_t {
    Missing,  // no depth, motion vectors and constants were tagged: the frame has no 3D camera
    Tagged,
    Failed,  // tagging was attempted and Streamline refused it
};

// What the renderer knows about the frame it is about to present.
struct FrameGenPresent {
    bool dlaa = false;  // the effective anti-aliasing mode is DLAA
    FrameGenInputs inputs = FrameGenInputs::Missing;
    std::uint32_t scene_width = 0, scene_height = 0;  // of the tagged depth and motion vectors
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    std::uint32_t display_width = 0, display_height = 0;  // of the display buffer the flip shows
    bool overlay_open = false;
    std::uint32_t swapchain_width = 0, swapchain_height = 0;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    PixelRect scene_rect;  // where the display buffer was blitted to in the swapchain image
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
    // recordFlip entry; SimulationEnd, RenderSubmitStart. `completed_us` is when the guest's flip completed (0 if
    // unknown), which gives the game's frame rate.
    void flipEntered(std::uint64_t completed_us);
    // True when the DLAA tap should tag depth, motion vectors and constants for the frame being recorded: the plugins
    // are loaded, generation is not latched off and the game rate and target call for a multiplier of 2 or more.
    bool wantsInputs() const;
    // The constants' reset flag for the frame being tagged: the first frame after one without generation, and after a
    // change of the game's frame rate. Consumes the rate event.
    bool takeReset();
    // After the display buffer was blitted into the swapchain image, before the frame is submitted: tags the scene
    // rectangle and tells DLSS-G whether to generate around this frame.
    void beforePresent(VkCommandBuffer cmd, const FrameGenPresent& frame);
    void submitted();        // after vkQueueSubmit2; RenderSubmitEnd
    void presentStarted();   // PresentStart
    void presentFinished();  // PresentEnd, then the DLSS-G state
    // Token for the next frame, right after the present, and the Reflex sleep for it on the sleeper thread.
    void frameDone(std::uint32_t next_index);

    FrameGenStatus status() const;
    // The debug query count and the info summary of the run. The process ends without running destructors, so the
    // stop path calls this from its own thread; it prints once.
    void logSummary();

private:
    // The reasons generation is not running; also the key of the summary counts.
    struct Decision {
        bool eligible = false;
        std::string reason;
    };
    // What the last info line said; a change of any member logs a new line.
    struct LoggedState {
        bool active = false;
        std::uint32_t multiplier = 1, vblanks = 0;
        FrameGenTarget target = FrameGenTarget::Off;
        std::string reason;
        bool operator==(const LoggedState&) const = default;
    };
    // Frames presented and the time they took, to measure a rate.
    struct RateMeter {
        std::uint64_t presented = 0;
        std::chrono::microseconds elapsed{0};
        void clear() { *this = RateMeter{}; }
        double fps() const {
            return elapsed.count() > 0 ? static_cast<double>(presented) * 1.0e6 / static_cast<double>(elapsed.count())
                                       : 0.0;
        }
    };

    FrameGenTarget effectiveTarget() const { return cycle_off_ ? FrameGenTarget::Off : requested_; }
    void apply();
    void changeLoadState(bool on, FrameGenTarget target);
    void failLoad(const std::string& reason);
    void logWindow();
    void startSleeper();
    void stopSleeper();
    void sleeperMain();
    void postSleep(const void* handle);

    std::uint32_t multiplier() const;
    std::uint32_t gameFps() const;
    Decision decide(const FrameGenPresent& frame, std::uint32_t multiplier) const;
    void logState(bool active, std::uint32_t multiplier, const std::string& reason);
    void beforeRecreate();
    void pollState();

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

    // The game's frame rate and what DLSS-G is doing about it.
    FlipRateTracker rate_;
    std::uint32_t intervals_ = 0;  // flip intervals seen, to tell "not known yet" from "irregular"
    bool rate_event_ = false;      // the class changed or a stall was seen since the last tagged frame
    std::uint32_t max_multiplier_ = 2;
    std::uint32_t min_size_ = 0;   // DLSSGState::minWidthOrHeight
    std::uint32_t streak_ = 0;     // consecutive eligible frames
    std::uint32_t recreated_ = 0;  // frames left that count as just after a swapchain recreation
    bool changing_ = false;        // inside changeLoadState
    bool latched_ = false;         // DLSS-G reported a status other than ok; off until the target changes
    std::uint32_t latch_status_ = 0;
    bool on_ = false;  // DLSS-G generates around the frame being presented
    std::uint32_t on_multiplier_ = 1;
    std::string reason_;                           // why not, of the last frame
    std::optional<FrameGenOptions> last_options_;  // what DLSS-G was last told
    std::optional<LoggedState> logged_;
    // A state is logged once it has lasted a moment, so a scene change that flips the reason back and forth does not
    // fill the log.
    struct SettlingState {
        LoggedState state;
        std::chrono::steady_clock::time_point since;
    };
    std::optional<SettlingState> settling_;
    bool estimate_pending_ = false, estimate_done_ = false;
    std::uint32_t api_errors_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_present_;

    // Measurements: the 10 s debug window, the last second (for the overlay) and the whole run.
    RateMeter window_, second_;
    std::uint32_t window_multiplier_ = 1, window_vblanks_ = 0;  // what window_ measured
    double presented_fps_ = 0.0;
    // The totals for the summary, read by the stop path from another thread.
    mutable std::mutex totals_mutex_;
    std::uint64_t queries_ = 0, frames_decided_ = 0, frames_generated_ = 0, multiplier_sum_ = 0, presented_total_ = 0;
    std::map<std::string, std::uint64_t> suspended_;
    std::atomic<bool> summary_logged_{false};

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

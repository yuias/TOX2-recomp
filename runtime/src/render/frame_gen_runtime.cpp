#include "render/frame_gen_runtime.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/log.h"

namespace tox2 {

namespace {

constexpr std::chrono::seconds kLogInterval{10};

const char* presentModeLabel(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        default: return "other";
    }
}

double toMs(std::chrono::nanoseconds duration) { return static_cast<double>(duration.count()) / 1.0e6; }

}  // namespace

FrameGenController::FrameGenController(VkContext& context, Presenter& presenter,
                                       const FrameGenControllerOptions& options)
    : context_(context), presenter_(presenter), streamline_(*context.streamline()), options_(options) {}

FrameGenController::~FrameGenController() { stopSleeper(); }

void FrameGenController::setTarget(FrameGenTarget target) {
    if (target != requested_) failed_ = false;
    requested_ = target;
    apply();
}

void FrameGenController::apply() {
    const FrameGenTarget target = effectiveTarget();
    const bool want = target != FrameGenTarget::Off && !failed_;
    if (want != loaded_) changeLoadState(want, target);
}

// The order matters: the sleeper calls into Streamline and waits on the swapchain, slSetFeatureLoaded is
// not thread safe, and the plugins must be (un)loaded while no swapchain exists, because the next acquire creates it
// through the proxies of whichever plugins are loaded then.
void FrameGenController::changeLoadState(bool on, FrameGenTarget target) {
    stopSleeper();
    vkDeviceWaitIdle(context_.device());
    if (!on) streamline_.freeFrameGen();
    presenter_.releaseSwapchain();
    std::string reason;
    if (!streamline_.setFrameGenerationLoaded(on, reason)) {
        failLoad(reason);
        return;
    }
    presenter_.setPresentMode(on ? options_.present_mode : VK_PRESENT_MODE_FIFO_KHR);
    loaded_ = on;
    if (on) {
        const ReflexSettings reflex{.low_latency = options_.reflex_low_latency, .frame_limit_us = 0};
        if (!streamline_.setReflex(reflex, reason)) {
            failLoad(reason);
            return;
        }
        log_deadline_.reset();
        startSleeper();
        logInfo("tox2: render: frame generation loaded (target {}, present mode {})", frameGenTargetName(target),
                presentModeLabel(options_.present_mode));
    } else {
        logInfo("tox2: render: frame generation unloaded");
    }
}

// Back to the state of a run without frame generation. No swapchain exists at this point, so the next one is created
// for whatever the plugins are in after the unload.
void FrameGenController::failLoad(const std::string& reason) {
    failed_ = true;
    stopSleeper();
    streamline_.freeFrameGen();
    presenter_.releaseSwapchain();
    std::string unload_reason;
    const bool unloaded = streamline_.setFrameGenerationLoaded(false, unload_reason);
    // What Streamline reports, not what was asked for: the present mode and the layout the blit leaves the image in
    // (see loaded()) must match the proxy swapchain the next acquire creates.
    loaded_ = streamline_.frameGenerationLoaded();
    presenter_.setPresentMode(loaded_ ? options_.present_mode : VK_PRESENT_MODE_FIFO_KHR);
    if (unloaded) {
        logWarn("tox2: render: frame generation: {}; presenting without it", reason);
    } else {
        logWarn("tox2: render: frame generation: {}; the plugins could not be unloaded ({}), restart the game to "
                "present without them",
                reason, unload_reason);
    }
}

void FrameGenController::tick() {
    const auto now = std::chrono::steady_clock::now();
    if (options_.cycle_seconds != 0) {
        // Started at the first flip, not at construction: the guest takes a while to produce the first one.
        if (!cycle_deadline_) {
            cycle_deadline_ = now + std::chrono::seconds(options_.cycle_seconds);
        } else if (now >= *cycle_deadline_) {
            cycle_off_ = !cycle_off_;
            cycle_deadline_ = now + std::chrono::seconds(options_.cycle_seconds);
            apply();
        }
    }
    if (!loaded_) return;
    if (!log_deadline_) {
        log_deadline_ = now + kLogInterval;
    } else if (now >= *log_deadline_) {
        log_deadline_ = now + kLogInterval;
        logSleepWindow();
    }
}

// Debug, not info: a line every 10 s would bury the other info lines.
void FrameGenController::logSleepWindow() {
    std::uint64_t sleeps = 0, dropped = 0;
    std::chrono::nanoseconds total{0}, max{0};
    {
        const std::lock_guard lock(mutex_);
        sleeps = std::exchange(window_sleeps_, 0);
        dropped = std::exchange(window_dropped_, 0);
        total = std::exchange(window_total_, std::chrono::nanoseconds{0});
        max = std::exchange(window_max_, std::chrono::nanoseconds{0});
    }
    const double avg_ms = sleeps == 0 ? 0.0 : toMs(total) / static_cast<double>(sleeps);
    logDebug("tox2: render: frame generation: sleep avg {:.2f} max {:.2f} ms ({} sleeps, {} dropped)", avg_ms,
             toMs(max), sleeps, dropped);
}

void FrameGenController::beginFrame(std::uint32_t index) {
    if (!loaded_) return;
    // The previous present already asked for this frame's token.
    if (streamline_.frameIndex() != index) streamline_.beginFrame(index);
    streamline_.latencyMarker(LatencyMarker::SimulationStart);
}

void FrameGenController::flipEntered() {
    if (!loaded_) return;
    streamline_.latencyMarker(LatencyMarker::SimulationEnd);
    streamline_.latencyMarker(LatencyMarker::RenderSubmitStart);
}

void FrameGenController::submitted() {
    if (loaded_) streamline_.latencyMarker(LatencyMarker::RenderSubmitEnd);
}

void FrameGenController::presentStarted() {
    if (loaded_) streamline_.latencyMarker(LatencyMarker::PresentStart);
}

void FrameGenController::presentFinished() {
    if (loaded_) streamline_.latencyMarker(LatencyMarker::PresentEnd);
}

void FrameGenController::frameDone(std::uint32_t next_index) {
    if (!loaded_) return;
    streamline_.beginFrame(next_index);
    postSleep(streamline_.frameHandle());
}

void FrameGenController::postSleep(const void* handle) {
    if (handle == nullptr) return;
    {
        const std::lock_guard lock(mutex_);
        if (pending_ != nullptr) window_dropped_++;
        pending_ = handle;
    }
    wake_.notify_one();
}

void FrameGenController::startSleeper() {
    if (sleeper_.joinable()) return;
    {
        const std::lock_guard lock(mutex_);
        stop_ = false;
        pending_ = nullptr;
        window_sleeps_ = window_dropped_ = 0;
        window_total_ = window_max_ = std::chrono::nanoseconds{0};
    }
    sleeper_ = std::thread([this] { sleeperMain(); });
    logDebug("tox2: render: frame generation: sleeper started");
}

// Waits for a sleep that is in flight; the wait on the swapchain inside Streamline is bounded.
void FrameGenController::stopSleeper() {
    if (!sleeper_.joinable()) return;
    const auto begin = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
        pending_ = nullptr;
    }
    wake_.notify_one();
    sleeper_.join();
    const std::chrono::duration<double, std::milli> waited = std::chrono::steady_clock::now() - begin;
    logDebug("tox2: render: frame generation: sleeper stopped (waited {:.1f} ms)", waited.count());
}

void FrameGenController::sleeperMain() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || pending_ != nullptr; });
        if (stop_) return;
        const void* handle = std::exchange(pending_, nullptr);
        lock.unlock();
        const auto begin = std::chrono::steady_clock::now();
        streamline_.reflexSleep(handle);
        const auto took = std::chrono::steady_clock::now() - begin;
        lock.lock();
        window_sleeps_++;
        window_total_ += std::chrono::duration_cast<std::chrono::nanoseconds>(took);
        window_max_ = std::max(window_max_, std::chrono::duration_cast<std::chrono::nanoseconds>(took));
    }
}

}  // namespace tox2

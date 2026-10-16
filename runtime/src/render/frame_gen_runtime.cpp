#include "render/frame_gen_runtime.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/log.h"

namespace tox2 {

namespace {

constexpr std::chrono::seconds kLogInterval{10};
// A window shorter than this says little about the rate; the debug line then reports the sleeps only.
constexpr std::chrono::seconds kMinWindow{1};
// How long a new state must hold before the info line for it is written.
constexpr std::chrono::milliseconds kLogSettle{1000};
// Intervals longer than this are a stall (a swapchain recreation, a hitch), not a rate.
constexpr std::chrono::milliseconds kMaxMeterInterval{250};
// The first frame of a stretch is only a candidate: DLSS-G is switched on at the second consecutive eligible one.
constexpr std::uint32_t kFramesToActivate = 2;
// After a swapchain recreation DLSS-G has no history for the new images.
constexpr std::uint32_t kFramesAfterRecreate = 2;

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

// DLSSGStatus bits (sl_dlss_g.h), named for the latch warning.
std::string statusBits(std::uint32_t status) {
    struct Bit {
        std::uint32_t mask;
        const char* name;
    };
    constexpr Bit kBits[] = {{1u << 0, "resolution too low"},
                             {1u << 1, "Reflex not detected at run time"},
                             {1u << 2, "HDR format not supported"},
                             {1u << 3, "common constants invalid"},
                             {1u << 4, "back buffer index not queried"}};
    std::string names;
    for (const Bit& bit : kBits) {
        if ((status & bit.mask) == 0) continue;
        if (!names.empty()) names += ", ";
        names += bit.name;
    }
    return names.empty() ? "unknown" : names;
}

}  // namespace

FrameGenController::FrameGenController(VkContext& context, Presenter& presenter,
                                       const FrameGenControllerOptions& options)
    : context_(context), presenter_(presenter), streamline_(*context.streamline()), options_(options) {
    // Unknown (0) counts as the smallest generator until the first state query reports the real value.
    max_multiplier_ = std::max<std::uint32_t>(context.frameGenerationMaxMultiplier(), 2);
    presenter_.setBeforeRecreate([this] { beforeRecreate(); });
}

FrameGenController::~FrameGenController() {
    // The presenter outlives the controller and would call into it at its next recreation.
    presenter_.setBeforeRecreate({});
    stopSleeper();
    logSummary();
}

void FrameGenController::setTarget(FrameGenTarget target) {
    if (target != requested_) {
        failed_ = false;
        latched_ = false;
    }
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
    // The swapchain release below runs the recreation callback; generation is turned off here instead, before the
    // resources it works on are freed.
    struct Changing {
        explicit Changing(bool& flag) : flag_(flag) { flag_ = true; }
        ~Changing() { flag_ = false; }
        bool& flag_;
    } changing(changing_);

    stopSleeper();
    vkDeviceWaitIdle(context_.device());
    if (last_options_ && last_options_->on) {
        FrameGenOptions options = *last_options_;
        options.on = false;
        std::string error;
        if (!streamline_.setFrameGenOptions(options, error)) {
            logOnce(LogLevel::Warn, "render-fg-set-options", "tox2: render: frame generation: {}", error);
        }
    }
    if (!on) streamline_.freeFrameGen();
    presenter_.releaseSwapchain();
    // The new swapchain's images have no DLSS-G history, and nothing has been decided for them yet.
    last_options_.reset();
    on_ = false;
    on_multiplier_ = 1;
    streak_ = 0;
    recreated_ = kFramesAfterRecreate;
    reason_.clear();
    logged_.reset();
    settling_.reset();
    window_.clear();
    second_.clear();
    last_present_.reset();
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
        logWindow();
    }
}

// Debug, not info: a line every 10 s would bury the other info lines.
void FrameGenController::logWindow() {
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
    if (window_.elapsed >= kMinWindow && window_vblanks_ != 0) {
        logDebug(
                "tox2: render: frame generation: {}x, game {} fps, presented {:.1f} fps, sleep avg {:.2f} max {:.2f} ms",
                window_multiplier_, 60 / window_vblanks_, window_.fps(), avg_ms, toMs(max));
    } else {
        logDebug("tox2: render: frame generation: sleep avg {:.2f} max {:.2f} ms ({} sleeps, {} dropped)", avg_ms,
                 toMs(max), sleeps, dropped);
    }
    window_.clear();
}

void FrameGenController::beginFrame(std::uint32_t index) {
    if (!loaded_) return;
    // The previous present already asked for this frame's token.
    if (streamline_.frameIndex() != index) streamline_.beginFrame(index);
    streamline_.latencyMarker(LatencyMarker::SimulationStart);
}

void FrameGenController::flipEntered(std::uint64_t completed_us) {
    // Fed while unloaded too, so the rate is known as soon as the plugins load.
    if (completed_us != 0) {
        const FlipRateTracker::Update update = rate_.onFlip(completed_us);
        intervals_ = update.discontinuity ? 0 : intervals_ + 1;
        if (update.changed || update.discontinuity) rate_event_ = true;
    }
    if (!loaded_) return;
    streamline_.latencyMarker(LatencyMarker::SimulationEnd);
    streamline_.latencyMarker(LatencyMarker::RenderSubmitStart);
}

std::uint32_t FrameGenController::multiplier() const {
    // FIFO cannot show more frames than the refresh; under the other modes the excess is dropped, not queued. Capped,
    // FIFO presented 120 fps at 144 Hz for an unlimited target with the presenter blocking at most 1.3 ms.
    const std::uint32_t cap =
            presenter_.presentMode() == VK_PRESENT_MODE_FIFO_KHR ? presenter_.windowStatus().refresh_hz : 0;
    return frameGenMultiplier(effectiveTarget(), rate_.vblanks(), max_multiplier_, cap);
}

std::uint32_t FrameGenController::gameFps() const {
    const std::uint32_t vblanks = rate_.vblanks();
    return vblanks == 0 ? 0 : 60 / vblanks;
}

bool FrameGenController::wantsInputs() const { return loaded_ && !latched_ && multiplier() >= 2; }

bool FrameGenController::takeReset() {
    // The first frame after one without generation has no generated history to continue.
    const bool reset = !on_ || rate_event_;
    rate_event_ = false;
    return reset;
}

// The first reason that applies, in the order a reader would fix them.
FrameGenController::Decision FrameGenController::decide(const FrameGenPresent& frame, std::uint32_t m) const {
    const auto no = [](std::string reason) { return Decision{false, std::move(reason)}; };
    if (latched_) return no(std::format("status {:#x}", latch_status_));
    if (!frame.dlaa) return no("needs DLAA anti-aliasing");
    const std::uint32_t vblanks = rate_.vblanks();
    if (vblanks == 0) {
        // The tracker reports 0 both before the first class is adopted and for irregular intervals.
        return no(intervals_ <= FlipRateTracker::kConfirmFlips ? "measuring the game rate" : "game rate irregular");
    }
    if (vblanks > 2) return no("game below 30 fps");
    if (m < 2) return no("target is less than twice the game rate");
    if (frame.overlay_open) return no(std::string(kFrameGenOverlayOpenReason));
    if (recreated_ > 0) return no("swapchain recreated");
    if (std::min(frame.swapchain_width, frame.swapchain_height) < min_size_) return no("window too small");
    if (frame.inputs == FrameGenInputs::Failed) return no("inputs not tagged");
    if (frame.inputs == FrameGenInputs::Missing) return no("no 3D camera");
    // Scene pixels must map one to one onto the display, which is what the backbuffer rectangle describes.
    if (frame.scene_width != frame.display_width || frame.scene_height != frame.display_height) {
        return no("scene size differs from the display");
    }
    return {true, {}};
}

void FrameGenController::beforePresent(VkCommandBuffer cmd, const FrameGenPresent& frame) {
    if (!loaded_) return;
    std::string error;
    // Every frame while loaded, so the black bars are copied as they are and the scene rectangle matches the inputs.
    const PixelRect& rect = frame.scene_rect;
    if (!streamline_.tagBackbufferRect(cmd, rect.x, rect.y, rect.width, rect.height, error)) {
        logOnce(LogLevel::Warn, "render-fg-backbuffer-tag", "tox2: render: frame generation: {}", error);
    }

    const std::uint32_t m = multiplier();
    const Decision decision = decide(frame, m);
    streak_ = decision.eligible ? streak_ + 1 : 0;
    if (recreated_ > 0) recreated_--;
    bool on = decision.eligible && streak_ >= kFramesToActivate;

    if (on) {
        FrameGenOptions options;
        options.on = true;
        options.generated = m - 1;
        options.only_generated = options_.only_generated;
        options.color_width = frame.swapchain_width;
        options.color_height = frame.swapchain_height;
        options.back_buffers = presenter_.imageCount();
        options.color_format = frame.swapchain_format;
        options.input_width = frame.scene_width;
        options.input_height = frame.scene_height;
        options.depth_format = frame.depth_format;
        options.motion_format = VK_FORMAT_R16G16_SFLOAT;
        if (last_options_ != options) {
            if (streamline_.setFrameGenOptions(options, error)) {
                last_options_ = options;
                if (!estimate_done_) estimate_pending_ = true;
            } else {
                logOnce(LogLevel::Warn, "render-fg-set-options", "tox2: render: frame generation: {}", error);
                on = false;
            }
        }
    } else if (last_options_ && last_options_->on) {
        // Turning off is immediate; the sizes stay as they were so a later activation compares against them.
        FrameGenOptions options = *last_options_;
        options.on = false;
        if (streamline_.setFrameGenOptions(options, error)) {
            last_options_ = options;
        } else {
            logOnce(LogLevel::Warn, "render-fg-set-options", "tox2: render: frame generation: {}", error);
        }
    }

    // A measurement window never mixes two multipliers or game rates.
    const std::uint32_t vblanks = rate_.vblanks();
    if (on && (window_multiplier_ != m || window_vblanks_ != vblanks)) window_.clear();
    if (on) {
        window_multiplier_ = m;
        window_vblanks_ = vblanks;
    }

    {
        const std::lock_guard lock(totals_mutex_);
        frames_decided_++;
        if (on) {
            frames_generated_++;
            multiplier_sum_ += m;
        } else if (!decision.eligible) {
            suspended_[decision.reason]++;
        }
    }
    on_ = on;
    on_multiplier_ = on ? m : 1;
    reason_ = decision.reason;
    // The frame that waits for its second eligible neighbour changes nothing worth a line.
    if (on || !decision.eligible) logState(on, m, decision.reason);
}

void FrameGenController::logState(bool active, std::uint32_t m, const std::string& reason) {
    const LoggedState state{active, active ? m : 1, rate_.vblanks(), effectiveTarget(),
                            active ? std::string() : reason};
    const auto now = std::chrono::steady_clock::now();
    if (logged_ == state) {
        settling_.reset();
        return;
    }
    if (!settling_ || !(settling_->state == state)) settling_ = SettlingState{state, now};
    if (now - settling_->since < kLogSettle) return;
    settling_.reset();
    logged_ = state;
    const std::uint32_t game = gameFps();
    if (active) {
        const std::uint32_t refresh = presenter_.windowStatus().refresh_hz;
        const std::string display = refresh != 0 ? std::format(", display {} Hz", refresh) : std::string();
        logInfo("tox2: render: frame generation active: {}x (game {} fps, expected {} fps{}, target {})", m, game,
                game * m, display, frameGenTargetName(state.target));
    } else if (game != 0) {
        logInfo("tox2: render: frame generation inactive: {} (presenting game {} fps)", reason, game);
    } else {
        logInfo("tox2: render: frame generation inactive: {}", reason);
    }
}

// Runs on the presenter's thread, which is this one, just before the swapchain DLSS-G works on goes away: it must not
// be asked to generate for images that no longer exist.
void FrameGenController::beforeRecreate() {
    recreated_ = kFramesAfterRecreate;
    streak_ = 0;
    if (changing_ || !loaded_ || !last_options_ || !last_options_->on) return;
    FrameGenOptions options = *last_options_;
    options.on = false;
    std::string error;
    if (streamline_.setFrameGenOptions(options, error)) {
        last_options_ = options;
    } else {
        logOnce(LogLevel::Warn, "render-fg-set-options", "tox2: render: frame generation: {}", error);
    }
    on_ = false;
}

void FrameGenController::submitted() {
    if (loaded_) streamline_.latencyMarker(LatencyMarker::RenderSubmitEnd);
}

void FrameGenController::presentStarted() {
    if (loaded_) streamline_.latencyMarker(LatencyMarker::PresentStart);
}

void FrameGenController::presentFinished() {
    if (!loaded_) return;
    streamline_.latencyMarker(LatencyMarker::PresentEnd);
    pollState();
}

// Right after the present, on the thread that presents: sl.dlss_g asks for the query to be synchronised with it.
void FrameGenController::pollState() {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = last_present_ ? std::chrono::duration_cast<std::chrono::microseconds>(now - *last_present_)
                                        : std::chrono::microseconds{0};
    last_present_ = now;

    FrameGenState state;
    std::string error;
    const FrameGenOptions* estimate = estimate_pending_ && last_options_ ? &*last_options_ : nullptr;
    {
        const std::lock_guard lock(totals_mutex_);
        queries_++;
    }
    if (!streamline_.frameGenState(state, estimate, error)) {
        logOnce(LogLevel::Warn, "render-fg-state", "tox2: render: frame generation: {}", error);
        return;
    }
    if (estimate != nullptr) {
        estimate_pending_ = false;
        estimate_done_ = true;
        if (state.vram_bytes != 0) {
            logInfo("tox2: render: frame generation needs about {} MB of video memory",
                    (state.vram_bytes + (1u << 20) - 1) >> 20);
        }
    }
    min_size_ = state.min_size;
    if (state.generated_max != 0) max_multiplier_ = 1 + state.generated_max;

    {
        const std::lock_guard lock(totals_mutex_);
        presented_total_ += state.presented;
    }
    if (interval > std::chrono::microseconds{0} && interval <= kMaxMeterInterval) {
        second_.presented += state.presented;
        second_.elapsed += interval;
        if (on_) {
            window_.presented += state.presented;
            window_.elapsed += interval;
        }
        if (second_.elapsed >= kMinWindow) {
            presented_fps_ = second_.fps();
            second_.clear();
        }
    }

    if (!state.ok && !latched_) {
        latched_ = true;
        latch_status_ = state.status;
        on_ = false;
        logWarn("tox2: render: frame generation: DLSS-G reports status {:#x} ({}); generation stays off until the "
                "target changes",
                state.status, statusBits(state.status));
    }
    const std::uint32_t errors = streamline_.frameGenApiErrors();
    if (errors != api_errors_) {
        api_errors_ = errors;
        logWarn("tox2: render: frame generation: DLSS-G reported {} Vulkan error(s) so far", errors);
    }
}

FrameGenStatus FrameGenController::status() const {
    FrameGenStatus status;
    status.active = on_;
    status.multiplier = on_multiplier_;
    status.game_fps = gameFps();
    status.presented_fps = static_cast<float>(presented_fps_);
    status.inactive_reason = on_ ? std::string() : reason_;
    status.max_multiplier = max_multiplier_;
    return status;
}

void FrameGenController::logSummary() {
    if (summary_logged_.exchange(true)) return;
    const std::lock_guard lock(totals_mutex_);
    if (queries_ != 0) logDebug("tox2: render: frame generation: {} state queries", queries_);
    if (frames_decided_ == 0) return;
    std::string suspended;
    for (const auto& [reason, count] : suspended_) {
        if (!suspended.empty()) suspended += ", ";
        suspended += std::format("{} {}", reason, count);
    }
    if (suspended.empty()) suspended = "none";
    const double average = frames_generated_ == 0
                                   ? 1.0
                                   : static_cast<double>(multiplier_sum_) / static_cast<double>(frames_generated_);
    logInfo("tox2: render: frame generation summary: {} frames presented, {} of {} game frames with generation "
            "(average {:.2f}x), suspended frames: {}",
            presented_total_, frames_generated_, frames_decided_, average, suspended);
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

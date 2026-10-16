#pragma once

// Data behind the debug overlay: frame statistics history and the cached values of the run-time
// providers. No ImGui and no Vulkan, so it is unit-testable and the panels only format what is here.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "render/frame_dump.h"
#include "render/present_overlay.h"
#include "render/vk_renderer.h"
#include "render/window_size.h"
#include "rsx/rsx_thread.h"

namespace tox2 {

// Receive timing of one SPU thread that blocks on events (see SpuThread::receiveBusy), lifetime values.
struct OverlaySpuRow {
    std::string name;
    std::uint64_t receives = 0;
    double busy_mean_ms = 0.0, busy_p99_ms = 0.0, busy_max_ms = 0.0;
    std::uint64_t busy_over = 0;  // busy times above one cellAudio period
};

// Guest lock acquisitions of one caller class, lifetime values. The holder attribution is left out: it sums over
// all waiters, so it is not a time.
struct OverlayLockRow {
    std::string name;
    std::uint64_t acquisitions = 0;
    double wait_p99_ms = 0.0, wait_max_ms = 0.0;
    std::uint64_t wait_over = 0;  // waits above kGuestLockWaitOverUs
};

struct OverlayAudioStats {
    std::string output;  // empty before cellAudioInit
    std::uint64_t blocks = 0;
    std::uint64_t skipped_blocks = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t underrun_frames = 0;
    double max_producer_ms = 0.0;
    double clock_ppm = 0.0;
    double mean_fill_frames = 0.0;
    std::uint64_t periods = 0;  // cellAudio
    std::uint64_t notifies_failed = 0;
    std::uint64_t unwritten_blocks = 0;  // port blocks the game never rewrote
    std::uint64_t unwritten_runs = 0;
    std::vector<OverlaySpuRow> spu;
    std::vector<OverlayLockRow> locks;  // classes with at least one acquisition
};

// Rates of one OverlayLockRow between two audio refreshes.
struct OverlayLockRate {
    double acquisitions_per_s = 0.0;
    double wait_over_per_s = 0.0;
};

// A Settings choice the user made. The value is the option's command-line form.
enum class OverlaySetting : std::uint8_t { FrameLimit, ResolutionScale, AaMode, Anisotropy, WindowSize, FrameGen };

// Every member may be empty: the panel then shows "n/a" or disables the control. The providers are
// called on the renderer's thread and must be safe from any thread.
struct OverlaySources {
    std::function<OverlayAudioStats()> audio;
    std::function<std::vector<std::pair<std::string, std::uint64_t>>()> unimplemented;
    std::function<std::uint64_t()> unimplemented_dropped;
    std::function<RsxFlipLimit()> flip_limit;
    std::function<void(RsxFlipLimit)> set_flip_limit;
    std::function<LogLevel()> log_level;
    std::function<void(LogLevel)> set_log_level;
    std::function<bool()> request_capture;
    std::function<RsxCaptureStatus()> capture_status;
    // Called at the moment the user picks a different value, for the settings that can be saved (not the log
    // level, which is a diagnostic). Runs on the renderer's thread, so it must not block.
    std::function<void(OverlaySetting setting, std::string_view value)> setting_changed;
};

class FrameStatsHistory {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kSamples = 240;

    void onFlip(const RendererStats& stats, Clock::time_point now);

    std::uint64_t flip() const { return flip_; }                     // stats.flips at the last onFlip
    std::uint64_t drawsLastFlip() const { return draws_last_; }      // delta to the previous onFlip (first: all so far)
    std::uint64_t skippedLastFlip() const { return skipped_last_; }  // same rule for draws_skipped
    std::uint64_t skippedTotal() const { return skipped_total_; }

    // (k - 1) / (newest - oldest) over the k flips in (now - 1 s, now]; 0 when k < 2.
    double flipsPerSecond(Clock::time_point now) const;

    // Ring of intervals between consecutive onFlip calls in ms, laid out for
    // ImGui::PlotLines(values_offset): the oldest sample is at frameTimeOffset().
    const std::array<float, kSamples>& frameTimesMs() const { return frame_times_ms_; }
    std::size_t frameTimeOffset() const { return frame_time_count_ < kSamples ? 0 : frame_time_head_; }
    std::size_t frameTimeCount() const { return frame_time_count_; }
    float maxFrameTimeMs() const;

private:
    std::uint64_t flip_ = 0;
    std::uint64_t draws_last_ = 0;
    std::uint64_t skipped_last_ = 0;
    std::uint64_t skipped_total_ = 0;
    std::uint64_t prev_draws_ = 0;
    std::uint64_t prev_skipped_ = 0;

    std::array<float, kSamples> frame_times_ms_{};
    std::size_t frame_time_head_ = 0;  // next slot to write
    std::size_t frame_time_count_ = 0;

    // Flip times of the same ring shape; the one-second window of flipsPerSecond reads these.
    std::array<Clock::time_point, kSamples> flip_times_{};
    std::size_t flip_time_head_ = 0;
    std::size_t flip_time_count_ = 0;
};

// Touched by the renderer's thread only.
struct OverlayState {
    FrameStatsHistory frames;
    OverlayAudioStats audio;
    double skipped_per_s = 0.0;               // audio skipped_blocks rate between two audio refreshes
    double underrun_frames_per_s = 0.0;       // audio underrun_frames rate, same interval
    double unwritten_per_s = 0.0;             // audio unwritten_blocks rate, same interval
    std::vector<OverlayLockRate> lock_rates;  // one per audio.locks entry
    std::vector<std::pair<std::string, std::uint64_t>> unimplemented;
    std::uint64_t unimplemented_dropped = 0;
    RsxCaptureStatus capture;
    RsxFlipLimit flip_limit = RsxFlipLimit::Vsync;
    LogLevel log_level = LogLevel::Info;
    DumpCounts dumps;
    std::filesystem::path last_dump;
    bool dump_requested = false;                 // set by the panel, taken by DebugOverlay::onFlip
    std::uint32_t resolution_scale = 0;          // last value the renderer reported; 0 = unknown
    std::uint32_t resolution_scale_request = 0;  // set by the panel, taken by DebugOverlay::onFlip; 0 = none
    std::optional<AaMode> aa_mode;               // last effective mode the renderer reported; unset = unknown
    bool msaa_available = false;                 // last value the renderer reported; AaMode::Msaa is selectable
    bool dlaa_available = false;                 // last value the renderer reported; AaMode::Dlaa is listed
    std::optional<AaMode> aa_mode_request;       // set by the panel, taken by DebugOverlay::onFlip; unset = none
    std::uint32_t anisotropy = 0;                // last effective level the renderer reported (1 = off); 0 = unknown
    float max_anisotropy = 0.0f;                 // device maximum the renderer reported; 0 = unsupported or unknown
    std::uint32_t anisotropy_request = 0;        // set by the panel, taken by DebugOverlay::onFlip; 0 = none
    std::optional<WindowStatus> window;          // last status the presenter reported; unset = no window
    std::optional<WindowSizePreset> window_size_request;  // set by the panel, taken by DebugOverlay::onFlip

    // Calls the providers: audio at most every 250 ms, unimplemented every 500 ms, the rest every call.
    // The audio and unimplemented snapshots take locks or copy maps, which a per-frame call would not need.
    void refresh(const OverlaySources& sources, FrameStatsHistory::Clock::time_point now);

private:
    std::optional<FrameStatsHistory::Clock::time_point> audio_time_;
    std::optional<FrameStatsHistory::Clock::time_point> unimplemented_time_;
};

// Combo item text for a window size preset: "1080p (1920x1080)". When `available` (the client pixels the work area
// allows) cannot hold the preset, the size it gets is added: "2160p (3840x2160, fits as 2496x1404)". An empty
// `available` means nothing was measured and never reads as a clamp.
std::string windowSizeItemLabel(WindowSizePreset preset, ClientSize available);

// Combo preview for a window status: the item text of `status.preset`; while fullscreen "<name> (when windowed)";
// when the client area differs from the size the preset fits to, "<name> (window WxH)". A minimised window
// (empty client) shows the item text.
std::string windowSizePreviewLabel(const WindowStatus& status);

// The anti-aliasing modes the combo lists, in display order. DLAA is left out when the renderer cannot run it, because
// it depends on the GPU and driver rather than on a device feature a disabled entry could explain.
std::vector<AaMode> aaModeChoices(const OverlayState& state);

// Moves the requests the panels made since the last flip into the reply for the renderer and clears them.
PresentOverlayRequests takePresentRequests(OverlayState& state);

}  // namespace tox2

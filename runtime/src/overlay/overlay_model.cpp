#include "overlay/overlay_model.h"

#include <algorithm>
#include <format>

namespace tox2 {

namespace {

constexpr std::chrono::milliseconds kAudioRefreshInterval{250};
constexpr std::chrono::milliseconds kUnimplementedRefreshInterval{500};
constexpr std::chrono::seconds kFlipRateWindow{1};

// Counters only grow; a smaller value means the source was restarted, which reads as no change.
std::uint64_t deltaOf(std::uint64_t now, std::uint64_t before) { return now >= before ? now - before : 0; }

bool due(const std::optional<FrameStatsHistory::Clock::time_point>& last, FrameStatsHistory::Clock::time_point now,
         FrameStatsHistory::Clock::duration interval) {
    return !last || now - *last >= interval;
}

// The size a preset gets on a screen that holds `available`; the preset's own size when nothing was measured.
ClientSize fittedSize(WindowSizePreset preset, ClientSize available) {
    const ClientSize wanted = presetClientSize(preset);
    if (available.width == 0 || available.height == 0) return wanted;
    return fitClientSize(wanted, available);
}

}  // namespace

void FrameStatsHistory::onFlip(const RendererStats& stats, Clock::time_point now) {
    if (flip_time_count_ > 0) {
        const std::size_t newest = (flip_time_head_ + kSamples - 1) % kSamples;
        const float interval_ms = std::chrono::duration<float, std::milli>(now - flip_times_[newest]).count();
        frame_times_ms_[frame_time_head_] = std::max(interval_ms, 0.0f);
        frame_time_head_ = (frame_time_head_ + 1) % kSamples;
        frame_time_count_ = std::min(frame_time_count_ + 1, kSamples);
    }
    flip_times_[flip_time_head_] = now;
    flip_time_head_ = (flip_time_head_ + 1) % kSamples;
    flip_time_count_ = std::min(flip_time_count_ + 1, kSamples);

    flip_ = stats.flips;
    draws_last_ = deltaOf(stats.draws, prev_draws_);
    skipped_last_ = deltaOf(stats.draws_skipped, prev_skipped_);
    skipped_total_ = stats.draws_skipped;
    prev_draws_ = stats.draws;
    prev_skipped_ = stats.draws_skipped;
}

double FrameStatsHistory::flipsPerSecond(Clock::time_point now) const {
    std::size_t k = 0;
    Clock::time_point oldest = now;
    Clock::time_point newest = now - kFlipRateWindow;
    for (std::size_t i = 0; i < flip_time_count_; ++i) {
        const Clock::time_point t = flip_times_[i];
        if (t > now || t <= now - kFlipRateWindow) {
            continue;
        }
        oldest = std::min(oldest, t);
        newest = std::max(newest, t);
        ++k;
    }
    if (k < 2 || newest <= oldest) {
        return 0.0;
    }
    return static_cast<double>(k - 1) / std::chrono::duration<double>(newest - oldest).count();
}

float FrameStatsHistory::maxFrameTimeMs() const {
    float peak = 0.0f;
    // Slots at or past the count are unwritten; the order inside the ring does not matter for a maximum.
    for (std::size_t i = 0; i < frame_time_count_; ++i) {
        peak = std::max(peak, frame_times_ms_[i]);
    }
    return peak;
}

void OverlayState::setFrameGen(bool supported, FrameGenTarget target, const FrameGenStatus& status) {
    // A remembered rate belongs to the target it ran with.
    if (target != frame_gen_target || target == FrameGenTarget::Off) frame_gen_last_active.reset();
    frame_gen_supported = supported;
    frame_gen_target = target;
    frame_gen = status;
    if (status.active) frame_gen_last_active = status;
}

void OverlayState::refresh(const OverlaySources& sources, FrameStatsHistory::Clock::time_point now) {
    if (sources.audio && due(audio_time_, now, kAudioRefreshInterval)) {
        const OverlayAudioStats next = sources.audio();
        const double seconds = audio_time_ ? std::chrono::duration<double>(now - *audio_time_).count() : 0.0;
        if (seconds > 0.0) {
            skipped_per_s = static_cast<double>(deltaOf(next.skipped_blocks, audio.skipped_blocks)) / seconds;
            underrun_frames_per_s = static_cast<double>(deltaOf(next.underrun_frames, audio.underrun_frames)) / seconds;
            unwritten_per_s = static_cast<double>(deltaOf(next.unwritten_blocks, audio.unwritten_blocks)) / seconds;
        }
        // Rows are matched by class name: a class appears with its first acquisition, so indices can shift.
        lock_rates.assign(next.locks.size(), OverlayLockRate{});
        if (seconds > 0.0) {
            for (std::size_t i = 0; i < next.locks.size(); ++i) {
                const auto before =
                        std::find_if(audio.locks.begin(), audio.locks.end(),
                                     [&](const OverlayLockRow& row) { return row.name == next.locks[i].name; });
                if (before == audio.locks.end()) continue;
                lock_rates[i].acquisitions_per_s =
                        static_cast<double>(deltaOf(next.locks[i].acquisitions, before->acquisitions)) / seconds;
                lock_rates[i].wait_over_per_s =
                        static_cast<double>(deltaOf(next.locks[i].wait_over, before->wait_over)) / seconds;
            }
        }
        audio = next;
        audio_time_ = now;
    }

    if ((sources.unimplemented || sources.unimplemented_dropped) &&
        due(unimplemented_time_, now, kUnimplementedRefreshInterval)) {
        if (sources.unimplemented) {
            unimplemented = sources.unimplemented();
        }
        if (sources.unimplemented_dropped) {
            unimplemented_dropped = sources.unimplemented_dropped();
        }
        unimplemented_time_ = now;
    }

    if (sources.flip_limit) {
        flip_limit = sources.flip_limit();
    }
    if (sources.log_level) {
        log_level = sources.log_level();
    }
    if (sources.capture_status) {
        capture = sources.capture_status();
    }
}

std::string windowSizeItemLabel(WindowSizePreset preset, ClientSize available) {
    const ClientSize wanted = presetClientSize(preset);
    const ClientSize fitted = fittedSize(preset, available);
    if (fitted == wanted) return std::format("{} ({}x{})", windowSizeName(preset), wanted.width, wanted.height);
    return std::format("{} ({}x{}, fits as {}x{})", windowSizeName(preset), wanted.width, wanted.height, fitted.width,
                       fitted.height);
}

std::string windowSizePreviewLabel(const WindowStatus& status) {
    if (status.fullscreen) return std::format("{} (when windowed)", windowSizeName(status.preset));
    const bool measured = status.client.width != 0 && status.client.height != 0;
    if (measured && status.client != fittedSize(status.preset, status.available)) {
        return std::format("{} (window {}x{})", windowSizeName(status.preset), status.client.width,
                           status.client.height);
    }
    return windowSizeItemLabel(status.preset, status.available);
}

std::vector<AaMode> aaModeChoices(const OverlayState& state) {
    std::vector<AaMode> choices{AaMode::Supersample, AaMode::Msaa};
    if (state.dlaa_available) choices.push_back(AaMode::Dlaa);
    return choices;
}

std::vector<FrameGenTarget> frameGenChoices(const OverlayState&) {
    return {FrameGenTarget::Off,    FrameGenTarget::Fps60,  FrameGenTarget::Fps90,
            FrameGenTarget::Fps120, FrameGenTarget::Fps180, FrameGenTarget::Unlimited};
}

std::string frameGenItemLabel(FrameGenTarget target) {
    switch (target) {
        case FrameGenTarget::Off: return "Off";
        case FrameGenTarget::Fps60: return "60 fps";
        case FrameGenTarget::Fps90: return "90 fps";
        case FrameGenTarget::Fps120: return "120 fps";
        // No 180 Hz display was available to look at the result.
        case FrameGenTarget::Fps180: return "180 fps (unverified)";
        case FrameGenTarget::Unlimited: return "Unlimited";
    }
    return "?";
}

bool frameGenSelectable(const OverlayState& state) {
    const std::optional<AaMode> mode = state.aa_mode_request ? state.aa_mode_request : state.aa_mode;
    return mode == AaMode::Dlaa;
}

std::string_view frameGenDisabledNote(const OverlayState& state) {
    return frameGenSelectable(state) ? std::string_view{} : std::string_view{"needs DLAA anti-aliasing"};
}

std::string frameGenStatusLine(const OverlayState& state) {
    if (state.frame_gen_target == FrameGenTarget::Off || !frameGenSelectable(state)) return {};

    const auto rate = [](const FrameGenStatus& status) {
        std::string text = std::format("{}x", status.multiplier);
        if (status.game_fps != 0)
            text += std::format(": {} -> {} fps", status.game_fps, status.game_fps * status.multiplier);
        if (status.presented_fps > 0.0f)
            text += std::format(", measured {:.1f}", static_cast<double>(status.presented_fps));
        return text;
    };

    const FrameGenStatus& status = state.frame_gen;
    if (status.active) return rate(status);
    if (status.inactive_reason == kFrameGenOverlayOpenReason) {
        std::string text = "suspended while this panel is open";
        if (state.frame_gen_last_active) text += "; last: " + rate(*state.frame_gen_last_active);
        return text;
    }
    if (status.inactive_reason.empty()) return "inactive";
    if (status.game_fps != 0) {
        return std::format("inactive: {} (presenting game {} fps)", status.inactive_reason, status.game_fps);
    }
    return "inactive: " + status.inactive_reason;
}

PresentOverlayRequests takePresentRequests(OverlayState& state) {
    PresentOverlayRequests requests;
    requests.dump_display = state.dump_requested;
    requests.resolution_scale = state.resolution_scale_request;
    requests.aa_mode = state.aa_mode_request;
    requests.anisotropy = state.anisotropy_request;
    requests.window_size = state.window_size_request;
    requests.frame_gen = state.frame_gen_request;
    state.dump_requested = false;
    state.resolution_scale_request = 0;
    state.aa_mode_request.reset();
    state.anisotropy_request = 0;
    state.window_size_request.reset();
    state.frame_gen_request.reset();
    return requests;
}

}  // namespace tox2

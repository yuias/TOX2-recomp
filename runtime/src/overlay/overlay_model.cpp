#include "overlay/overlay_model.h"

#include <algorithm>

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

PresentOverlayRequests takePresentRequests(OverlayState& state) {
    PresentOverlayRequests requests;
    requests.dump_display = state.dump_requested;
    requests.resolution_scale = state.resolution_scale_request;
    requests.aa_mode = state.aa_mode_request;
    requests.anisotropy = state.anisotropy_request;
    state.dump_requested = false;
    state.resolution_scale_request = 0;
    state.aa_mode_request.reset();
    state.anisotropy_request = 0;
    return requests;
}

}  // namespace tox2

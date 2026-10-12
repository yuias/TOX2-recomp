#include "overlay/overlay_sources.h"

#include <string>
#include <utility>

#include "audio/audio_engine.h"
#include "core/threads.h"
#include "hle/cell_audio_ports.h"
#include "rsx/rsx_thread.h"
#include "rsx/rsx_unimplemented.h"
#include "spu/spu_census.h"

namespace tox2 {

namespace {

constexpr double kUsPerMs = 1000.0;

OverlayAudioStats collectAudioStats() {
    OverlayAudioStats out;
    const AudioEngineStats engine = audioEngineStats();
    out.output = engine.output;
    // Without an engine the panel shows "not started"; cellAudio counters would only be zeros.
    if (out.output.empty()) return out;

    out.blocks = engine.blocks;
    out.skipped_blocks = engine.skipped_blocks;
    out.blocks_dropped = engine.output_stats.blocks_dropped;
    out.underrun_frames = engine.output_stats.underrun_frames;
    out.max_producer_ms = std::chrono::duration<double, std::milli>(engine.max_producer_time).count();
    out.clock_ppm = engine.clock_ppm;
    out.mean_fill_frames = engine.mean_fill_frames;

    const CellAudioStats ports = cellAudioPorts().stats();
    out.periods = ports.periods;
    out.notifies_failed = ports.notifies_failed;
    out.unwritten_blocks = ports.unwritten_blocks;
    out.unwritten_runs = ports.unwritten_runs;

    for (const SpuReceiveRow& row : spuReceiveSnapshot()) {
        OverlaySpuRow spu;
        spu.name = row.name;
        spu.receives = row.wait.count;
        spu.busy_mean_ms = row.busy.meanUs() / kUsPerMs;
        spu.busy_p99_ms = static_cast<double>(row.busy.percentileUs(0.99)) / kUsPerMs;
        spu.busy_max_ms = static_cast<double>(row.busy.max_us) / kUsPerMs;
        spu.busy_over = row.busy.over;
        out.spu.push_back(std::move(spu));
    }

    const GuestLockStats lock = guestLock().stats();
    for (std::size_t i = 0; i < kGuestLockClassCount; ++i) {
        const LatencySnapshot& wait = lock.classes[i].wait;
        if (wait.count == 0) continue;
        OverlayLockRow row;
        row.name = std::string(guestLockClassName(static_cast<GuestLockClass>(i)));
        row.acquisitions = wait.count;
        row.wait_p99_ms = static_cast<double>(wait.percentileUs(0.99)) / kUsPerMs;
        row.wait_max_ms = static_cast<double>(wait.max_us) / kUsPerMs;
        row.wait_over = wait.over;
        out.locks.push_back(std::move(row));
    }
    return out;
}

// The logger keeps one threshold; the lowest enabled level is that threshold.
LogLevel currentLogLevel() {
    for (const LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn}) {
        if (logEnabled(level)) return level;
    }
    return LogLevel::Error;
}

}  // namespace

OverlaySources makeRuntimeOverlaySources() {
    OverlaySources sources;
    sources.audio = collectAudioStats;
    sources.unimplemented = rsxUnimplementedSnapshot;
    sources.unimplemented_dropped = rsxUnimplementedDropped;
    sources.flip_limit = rsxFlipLimit;
    sources.set_flip_limit = setRsxFlipLimit;
    sources.log_level = currentLogLevel;
    sources.set_log_level = setLogLevel;
    sources.request_capture = requestRsxCapture;
    sources.capture_status = rsxCaptureStatus;
    return sources;
}

}  // namespace tox2

#include "overlay/overlay_sources.h"

#include "audio/audio_engine.h"
#include "hle/cell_audio_ports.h"
#include "rsx/rsx_thread.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {

namespace {

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

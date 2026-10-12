// Self-checking tests for the debug overlay library. The model and panel tests need no GPU or window.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "imgui.h"
#include "imgui_internal.h"
#include "overlay/overlay_model.h"
#include "overlay/overlay_panels.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using Clock = tox2::FrameStatsHistory::Clock;

Clock::time_point at(double seconds) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)) +
                             std::chrono::seconds(100));
}

void testSteadyFlips() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    for (int i = 0; i <= 60; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        history.onFlip(stats, at(i / 60.0));
    }
    const Clock::time_point last = at(1.0);
    CHECK(history.flip() == 61);
    CHECK(std::fabs(history.flipsPerSecond(last) - 60.0) < 0.01);
    CHECK(history.flipsPerSecond(last + std::chrono::seconds(2)) == 0.0);
    CHECK(history.frameTimeCount() == 60);
    CHECK(history.frameTimeOffset() == 0);
    bool steady = true;
    for (std::size_t i = 0; i < history.frameTimeCount(); ++i) {
        steady = steady && std::fabs(history.frameTimesMs()[i] - 16.667f) < 0.01f;
    }
    CHECK(steady);
    CHECK(std::fabs(history.maxFrameTimeMs() - 16.667f) < 0.01f);
}

void testRingWraps() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    for (int i = 0; i < 300; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        // The last interval is the longest, so the maximum has to come from the newest slot.
        history.onFlip(stats, at(i * 0.01 + (i == 299 ? 0.05 : 0.0)));
    }
    CHECK(history.frameTimeCount() == tox2::FrameStatsHistory::kSamples);
    // 299 intervals were written; the next slot to overwrite (the oldest) is 299 % 240.
    CHECK(history.frameTimeOffset() == 299 % tox2::FrameStatsHistory::kSamples);
    CHECK(std::fabs(history.frameTimesMs()[(history.frameTimeOffset() + 239) % 240] - 60.0f) < 0.01f);
    CHECK(std::fabs(history.maxFrameTimeMs() - 60.0f) < 0.01f);
    CHECK(history.flip() == 300);
}

void testDrawDeltas() {
    tox2::FrameStatsHistory history;
    tox2::RendererStats stats;
    stats.flips = 5;
    stats.draws = 40;
    stats.draws_skipped = 3;
    history.onFlip(stats, at(0.0));
    // The first call reports everything so far.
    CHECK(history.drawsLastFlip() == 40);
    CHECK(history.skippedLastFlip() == 3);
    CHECK(history.skippedTotal() == 3);

    stats.flips = 6;
    stats.draws = 52;
    stats.draws_skipped = 4;
    history.onFlip(stats, at(0.016));
    CHECK(history.drawsLastFlip() == 12);
    CHECK(history.skippedLastFlip() == 1);
    CHECK(history.skippedTotal() == 4);
    CHECK(history.flip() == 6);
}

void testRefresh() {
    int audio_calls = 0;
    int unimplemented_calls = 0;
    int limit_calls = 0;
    std::uint64_t skipped = 0;
    std::uint64_t underruns = 0;

    tox2::OverlaySources sources;
    sources.audio = [&] {
        ++audio_calls;
        tox2::OverlayAudioStats stats;
        stats.output = "fake";
        stats.skipped_blocks = skipped;
        stats.underrun_frames = underruns;
        return stats;
    };
    sources.unimplemented = [&] {
        ++unimplemented_calls;
        return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}};
    };
    sources.unimplemented_dropped = [] { return std::uint64_t{7}; };
    sources.flip_limit = [&] {
        ++limit_calls;
        return false;
    };
    sources.log_level = [] { return tox2::LogLevel::Warn; };
    sources.capture_status = [] {
        tox2::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 9;
        return status;
    };

    tox2::OverlayState state;
    CHECK(state.flip_limit);
    CHECK(state.log_level == tox2::LogLevel::Info);

    state.refresh(sources, at(0.0));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(state.audio.output == "fake");
    CHECK(state.skipped_per_s == 0.0);
    CHECK(state.unimplemented.size() == 1 && state.unimplemented[0].second == 2);
    CHECK(state.unimplemented_dropped == 7);
    CHECK(!state.flip_limit);
    CHECK(state.log_level == tox2::LogLevel::Warn);
    CHECK(state.capture.available && state.capture.last_frame == 9);

    // Inside the audio interval: the cheap providers still run, the audio one does not.
    skipped = 10;
    state.refresh(sources, at(0.1));
    CHECK(audio_calls == 1);
    CHECK(unimplemented_calls == 1);
    CHECK(limit_calls == 2);

    skipped = 5;
    underruns = 480;
    state.refresh(sources, at(0.25));
    CHECK(audio_calls == 2);
    CHECK(unimplemented_calls == 1);
    CHECK(std::fabs(state.skipped_per_s - 20.0) < 1e-6);
    CHECK(std::fabs(state.underrun_frames_per_s - 1920.0) < 1e-6);

    state.refresh(sources, at(0.5));
    CHECK(audio_calls == 3);
    CHECK(unimplemented_calls == 2);
    CHECK(state.skipped_per_s == 0.0);

    // Empty sources leave the cached values alone.
    state.refresh(tox2::OverlaySources{}, at(5.0));
    CHECK(state.audio.output == "fake");
    CHECK(state.unimplemented_dropped == 7);
}

tox2::OverlaySources makeFakeSources() {
    tox2::OverlaySources sources;
    sources.audio = [] {
        tox2::OverlayAudioStats stats;
        stats.output = "fake";
        stats.max_producer_ms = 1.5;
        stats.mean_fill_frames = 1500.0;
        return stats;
    };
    sources.unimplemented = [] { return std::vector<std::pair<std::string, std::uint64_t>>{{"state: x", 2}}; };
    sources.unimplemented_dropped = [] { return std::uint64_t{1}; };
    sources.flip_limit = [] { return true; };
    sources.set_flip_limit = [](bool) {};
    sources.log_level = [] { return tox2::LogLevel::Info; };
    sources.set_log_level = [](tox2::LogLevel) {};
    sources.request_capture = [] { return true; };
    sources.capture_status = [] {
        tox2::RsxCaptureStatus status;
        status.available = true;
        status.last_frame = 4;
        status.last_path = "captures/frame_4.tox2cap";
        return status;
    };
    return sources;
}

// Runs three frames of the overlay window in a private ImGui context and returns whether the last one
// produced geometry. The Vulkan backend is not involved.
bool drawFrames(tox2::OverlayState& state, const tox2::OverlaySources& sources) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->Build();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);

    bool drew = false;
    for (int i = 0; i < 3; ++i) {
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        tox2::drawOverlayWindow(state, sources);
        ImGui::Render();
        drew = ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0;
    }
    CHECK(ImGui::FindWindowByName("tox2") != nullptr);
    ImGui::DestroyContext(context);
    return drew;
}

void testPanels() {
    tox2::OverlayState state;
    tox2::RendererStats stats;
    for (int i = 0; i < 5; ++i) {
        stats.flips = static_cast<std::uint64_t>(i + 1);
        stats.draws = static_cast<std::uint64_t>(10 * (i + 1));
        stats.draws_skipped = static_cast<std::uint64_t>(i);
        state.frames.onFlip(stats, at(i / 60.0));
    }
    const tox2::OverlaySources sources = makeFakeSources();
    state.refresh(sources, at(1.0));
    state.dumps.written = 2;
    state.last_dump = "frames/frame_000002.png";
    CHECK(drawFrames(state, sources));

    // The last frame ran through every section with data; clicks are not simulated, so no request is set.
    CHECK(!state.dump_requested);

    // No providers at all: every section falls back to its placeholder.
    tox2::OverlayState empty_state;
    CHECK(drawFrames(empty_state, tox2::OverlaySources{}));
}

}  // namespace

int main() {
    testSteadyFlips();
    testRingWraps();
    testDrawDeltas();
    testRefresh();
    testPanels();
    std::printf("overlay tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

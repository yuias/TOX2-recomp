#include "overlay/overlay_panels.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <string>

#include "audio/audio_engine.h"
#include "audio/audio_output.h"
#include "imgui.h"
#include "render/render_scale.h"

namespace tox2 {

namespace {

constexpr float kBaseFontSize = 13.0f;  // the default font size before DPI scaling
constexpr float kWindowOffset = 10.0f;
constexpr float kMinWindowWidth = 440.0f;  // the window grows with its content; this keeps its width steady
constexpr float kMaxWindowWidth = 720.0f;  // a long path or kind name wraps or clips instead of stretching it
constexpr float kFrameGraphHeight = 60.0f;
constexpr float kMinGraphMaxMs = 33.4f;  // keeps a steady 60 Hz line in the lower half of the graph

constexpr std::array<const char*, 5> kLogLevelNames = {"trace", "debug", "info", "warn", "error"};

unsigned long long asUll(std::uint64_t value) { return static_cast<unsigned long long>(value); }

double toMs(std::chrono::nanoseconds duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

void drawStats(const OverlayState& state) {
    const FrameStatsHistory& frames = state.frames;
    const double fps = frames.flipsPerSecond(FrameStatsHistory::Clock::now());

    ImGui::Text("flip %llu    %.1f flips/s", asUll(frames.flip()), fps);
    ImGui::Text("draws %llu    skipped %llu (total %llu)", asUll(frames.drawsLastFlip()),
                asUll(frames.skippedLastFlip()), asUll(frames.skippedTotal()));

    const float graph_max = std::max(frames.maxFrameTimeMs() * 1.1f, kMinGraphMaxMs);
    char overlay_text[32];
    std::snprintf(overlay_text, sizeof(overlay_text), "max %.1f ms", static_cast<double>(frames.maxFrameTimeMs()));
    const float scale = ImGui::GetFontSize() / kBaseFontSize;
    ImGui::PlotLines("##frame_time", frames.frameTimesMs().data(), static_cast<int>(frames.frameTimeCount()),
                     static_cast<int>(frames.frameTimeOffset()), overlay_text, 0.0f, graph_max,
                     ImVec2(0.0f, kFrameGraphHeight * scale));

    ImGui::SeparatorText("audio");
    const OverlayAudioStats& audio = state.audio;
    if (audio.output.empty()) {
        ImGui::TextUnformatted("n/a (audio not started)");
        return;
    }
    const double period_ms = toMs(kAudioBlockPeriod);
    ImGui::Text("output %s", audio.output.c_str());
    ImGui::Text("producer max %.3f ms of %.3f ms (margin %.3f ms)", audio.max_producer_ms, period_ms,
                period_ms - audio.max_producer_ms);
    ImGui::Text("blocks %llu    periods %llu    notifies failed %llu", asUll(audio.blocks), asUll(audio.periods),
                asUll(audio.notifies_failed));
    ImGui::Text("skipped periods %llu (%.2f/s)", asUll(audio.skipped_blocks), state.skipped_per_s);
    ImGui::Text("underrun frames %llu (%.0f/s)", asUll(audio.underrun_frames), state.underrun_frames_per_s);
    ImGui::Text("dropped blocks %llu", asUll(audio.blocks_dropped));
    ImGui::Text("clock correction %+.1f ppm", audio.clock_ppm);
    ImGui::Text("output fill %.0f frames (target %llu)", audio.mean_fill_frames, asUll(kFillTargetFrames));
}

// The renderer applies the choice after the flip that reports it, so the combo shows the pending request until
// the next flip reports the new scale back.
void drawResolutionScale(OverlayState& state) {
    // 0 means the renderer has not reported a scale (no renderer behind the overlay).
    ImGui::BeginDisabled(state.resolution_scale == 0);
    const std::uint32_t shown =
            state.resolution_scale_request != 0 ? state.resolution_scale_request : state.resolution_scale;
    char preview[16] = "n/a";
    if (shown != 0) std::snprintf(preview, sizeof(preview), "%ux", shown);
    if (ImGui::BeginCombo("Internal resolution scale", preview)) {
        for (std::uint32_t scale = 1; scale <= kMaxResolutionScale; ++scale) {
            char label[16];
            std::snprintf(label, sizeof(label), "%ux", scale);
            if (ImGui::Selectable(label, scale == shown) && scale != shown) state.resolution_scale_request = scale;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void drawSettings(OverlayState& state, const OverlaySources& sources) {
    ImGui::BeginDisabled(!sources.set_flip_limit);
    bool flip_limit = state.flip_limit;
    if (ImGui::Checkbox("Flip limiter", &flip_limit)) {
        state.flip_limit = flip_limit;
        sources.set_flip_limit(flip_limit);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!sources.set_log_level);
    int level = static_cast<int>(state.log_level);
    if (level >= 0 && level < static_cast<int>(kLogLevelNames.size()) &&
        ImGui::BeginCombo("Log level", kLogLevelNames[static_cast<std::size_t>(level)])) {
        for (int i = 0; i < static_cast<int>(kLogLevelNames.size()); ++i) {
            if (ImGui::Selectable(kLogLevelNames[static_cast<std::size_t>(i)], i == level)) {
                state.log_level = static_cast<LogLevel>(i);
                sources.set_log_level(state.log_level);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    drawResolutionScale(state);

    // Not wired to anything yet; shown so the layout of the finished panel is visible.
    ImGui::SeparatorText("(not implemented)");
    ImGui::BeginDisabled();
    int anisotropy = 1;
    float volume = 1.0f;
    ImGui::SliderInt("Anisotropic filtering", &anisotropy, 1, 16, "%dx");
    ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
}

void drawCapture(OverlayState& state, const OverlaySources& sources) {
    const RsxCaptureStatus& capture = state.capture;

    ImGui::BeginDisabled(!sources.request_capture || !capture.available || capture.pending || capture.recording != 0);
    if (ImGui::Button("Capture next flip")) sources.request_capture();
    ImGui::EndDisabled();
    ImGui::SameLine();

    if (!capture.available) {
        ImGui::TextUnformatted("n/a (no frame builder)");
    } else if (capture.pending) {
        ImGui::TextUnformatted("pending");
    } else if (capture.recording != 0) {
        ImGui::Text("recording frame %u", capture.recording);
    } else if (capture.last_frame == 0) {
        ImGui::TextUnformatted("no capture yet");
    } else if (!capture.last_error.empty()) {
        ImGui::TextWrapped("frame %u failed: %s", capture.last_frame, capture.last_error.c_str());
    } else {
        ImGui::TextWrapped("frame %u -> %s", capture.last_frame, capture.last_path.c_str());
    }

    if (ImGui::Button("Dump next flip as PNG")) state.dump_requested = true;
    ImGui::SameLine();
    if (state.dump_requested) {
        ImGui::TextUnformatted("requested");
    } else if (state.last_dump.empty()) {
        ImGui::TextUnformatted("no dump yet");
    } else {
        ImGui::TextWrapped("%s", state.last_dump.generic_string().c_str());
    }
    ImGui::Text("dumps written %llu    failed %llu    skipped %llu", asUll(state.dumps.written),
                asUll(state.dumps.failed), asUll(state.dumps.skipped));
}

void drawUnimplemented(const OverlayState& state, const OverlaySources& sources) {
    // The filter text must outlive a frame and the model stays free of ImGui types, so it lives here.
    // The overlay is single-instance and drawn from one thread.
    static ImGuiTextFilter filter;
    filter.Draw("Filter", ImGui::GetFontSize() * 14.0f);

    if (!sources.unimplemented) {
        ImGui::TextUnformatted("n/a");
        return;
    }
    const float scale = ImGui::GetFontSize() / kBaseFontSize;
    if (ImGui::BeginTable("##unimplemented", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 200.0f * scale))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& [kind, count] : state.unimplemented) {
            if (!filter.PassFilter(kind.c_str())) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", asUll(count));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(kind.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Text("dropped (table full) %llu", asUll(state.unimplemented_dropped));
}

}  // namespace

void drawOverlayWindow(OverlayState& state, const OverlaySources& sources) {
    // The font is sized for the monitor DPI when the overlay starts, so its size doubles as the scale.
    const float scale = ImGui::GetFontSize() / kBaseFontSize;
    ImGui::SetNextWindowPos(ImVec2(kWindowOffset * scale, kWindowOffset * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kMinWindowWidth * scale, 0.0f),
                                        ImVec2(kMaxWindowWidth * scale, FLT_MAX));

    // The sections open and close, so the size follows the content on every frame instead of only on the first;
    // otherwise a section opened later is clipped and wide labels are cut off.
    if (ImGui::Begin("tox2", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader("Stats")) drawStats(state);
        if (ImGui::CollapsingHeader("Settings")) drawSettings(state, sources);
        if (ImGui::CollapsingHeader("Capture")) drawCapture(state, sources);
        if (ImGui::CollapsingHeader("RSX unimplemented")) drawUnimplemented(state, sources);
    }
    ImGui::End();
}

}  // namespace tox2

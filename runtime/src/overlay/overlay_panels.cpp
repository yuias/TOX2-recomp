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
#include "core/threads.h"
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
constexpr double kUsPerMs = 1000.0;

constexpr std::array<const char*, 5> kLogLevelNames = {"trace", "debug", "info", "warn", "error"};

unsigned long long asUll(std::uint64_t value) { return static_cast<unsigned long long>(value); }

double toMs(std::chrono::nanoseconds duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

// Lifetime wait figures per caller class; the rates come from the last two audio refreshes.
void drawGuestLockTable(const OverlayState& state) {
    const OverlayAudioStats& audio = state.audio;
    if (audio.locks.empty()) return;
    ImGui::TextUnformatted("guest lock");
    if (!ImGui::BeginTable("##guest_lock", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    char over_label[24];
    std::snprintf(over_label, sizeof(over_label), ">%.0f ms /s", static_cast<double>(kGuestLockWaitOverUs) / kUsPerMs);
    ImGui::TableSetupColumn("class");
    ImGui::TableSetupColumn("/s");
    ImGui::TableSetupColumn("wait p99 ms");
    ImGui::TableSetupColumn("wait max ms");
    ImGui::TableSetupColumn(over_label);
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < audio.locks.size(); ++i) {
        const OverlayLockRow& row = audio.locks[i];
        const OverlayLockRate rate = i < state.lock_rates.size() ? state.lock_rates[i] : OverlayLockRate{};
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.0f", rate.acquisitions_per_s);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.2f", row.wait_p99_ms);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.2f", row.wait_max_ms);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.2f", rate.wait_over_per_s);
    }
    ImGui::EndTable();
}

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
    ImGui::Text("unwritten blocks %llu (%.2f/s, runs %llu)", asUll(audio.unwritten_blocks), state.unwritten_per_s,
                asUll(audio.unwritten_runs));
    for (const OverlaySpuRow& spu : audio.spu) {
        ImGui::Text("spu %s busy %.2f/%.2f/%.2f ms, >%.2f ms %llu", spu.name.c_str(), spu.busy_mean_ms, spu.busy_p99_ms,
                    spu.busy_max_ms, period_ms, asUll(spu.busy_over));
    }
    drawGuestLockTable(state);
}

// The renderer applies the choice after the flip that reports it, so the combo shows the pending request until
// the next flip reports the new scale back.
void drawResolutionScale(OverlayState& state, const OverlaySources& sources) {
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
            if (ImGui::Selectable(label, scale == shown) && scale != shown) {
                state.resolution_scale_request = scale;
                if (sources.setting_changed)
                    sources.setting_changed(OverlaySetting::ResolutionScale, std::to_string(scale));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

const char* aaModeLabel(AaMode mode) {
    switch (mode) {
        case AaMode::Supersample: return "Supersample";
        case AaMode::Msaa: return "MSAA 2x";
        case AaMode::Dlaa: return "DLAA";
    }
    return "?";
}

// Same pending-request display as the scale combo. The mode is unset when no renderer reports it.
void drawAntiAliasing(OverlayState& state, const OverlaySources& sources) {
    ImGui::BeginDisabled(!state.aa_mode.has_value());
    const std::optional<AaMode> shown = state.aa_mode_request ? state.aa_mode_request : state.aa_mode;
    if (ImGui::BeginCombo("Anti-aliasing", shown ? aaModeLabel(*shown) : "unknown")) {
        for (const AaMode mode : aaModeChoices(state)) {
            const bool selectable = mode != AaMode::Msaa || state.msaa_available;
            ImGui::BeginDisabled(!selectable);
            if (ImGui::Selectable(aaModeLabel(mode), shown == mode) && shown != mode) {
                state.aa_mode_request = mode;
                if (sources.setting_changed) sources.setting_changed(OverlaySetting::AaMode, aaModeName(mode));
            }
            ImGui::EndDisabled();
        }
        if (!state.msaa_available) ImGui::TextDisabled("MSAA is unavailable on this device");
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

// Listed only where the device and the presenter can run frame generation. Same pending-request display as the scale
// combo. Without DLAA it is drawn disabled and the saved target stays as it is, so it applies again when DLAA returns.
void drawFrameGen(OverlayState& state, const OverlaySources& sources) {
    if (!state.frame_gen_supported) return;
    const bool selectable = frameGenSelectable(state);
    const FrameGenTarget shown = state.frame_gen_request ? *state.frame_gen_request : state.frame_gen_target;
    ImGui::BeginDisabled(!selectable);
    if (ImGui::BeginCombo("Frame generation", frameGenItemLabel(shown).c_str())) {
        for (const FrameGenTarget target : frameGenChoices(state)) {
            if (ImGui::Selectable(frameGenItemLabel(target).c_str(), target == shown) && target != shown) {
                state.frame_gen_request = target;
                if (sources.setting_changed) {
                    sources.setting_changed(OverlaySetting::FrameGen, frameGenTargetName(target));
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (!selectable) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", std::string(frameGenDisabledNote(state)).c_str());
    }
    const std::string status = frameGenStatusLine(state);
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}

// Listed only where the device can write object motion. Disabled without DLAA, keeping its value.
void drawObjectMotion(OverlayState& state, const OverlaySources& sources) {
    if (!state.object_motion_supported) return;
    const bool selectable = objectMotionSelectable(state);
    bool shown = state.object_motion_request ? *state.object_motion_request : state.object_motion;
    ImGui::BeginDisabled(!selectable);
    if (ImGui::Checkbox("Object motion vectors", &shown)) {
        state.object_motion_request = shown;
        if (sources.setting_changed) sources.setting_changed(OverlaySetting::ObjectMotion, shown ? "on" : "off");
    }
    ImGui::EndDisabled();
    if (!selectable) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", std::string(frameGenDisabledNote(state)).c_str());
    }
}

const char* anisotropyLabel(std::uint32_t level) {
    switch (level) {
        case 1: return "Off";
        case 2: return "2x";
        case 4: return "4x";
        case 8: return "8x";
        case 16: return "16x";
        default: return "?";
    }
}

// Same pending-request display as the scale combo. A level of 0 means no renderer reports one; a device maximum
// below 2 means the samplerAnisotropy feature is missing.
void drawAnisotropy(OverlayState& state, const OverlaySources& sources) {
    const bool unsupported = state.anisotropy != 0 && state.max_anisotropy < 2.0f;
    ImGui::BeginDisabled(state.anisotropy == 0 || unsupported);
    const std::uint32_t shown = state.anisotropy_request != 0 ? state.anisotropy_request : state.anisotropy;
    const char* preview = unsupported ? "unavailable on this device" : shown != 0 ? anisotropyLabel(shown) : "n/a";
    if (ImGui::BeginCombo("Anisotropic filtering", preview)) {
        for (const std::uint32_t level : kAnisotropyLevels) {
            const bool selectable = level == 1 || static_cast<float>(level) <= state.max_anisotropy;
            ImGui::BeginDisabled(!selectable);
            if (ImGui::Selectable(anisotropyLabel(level), level == shown) && level != shown) {
                state.anisotropy_request = level;
                if (sources.setting_changed) sources.setting_changed(OverlaySetting::Anisotropy, anisotropyName(level));
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

// Same pending-request display as the scale combo. Disabled without a window (nothing reports a status). The
// request is set for every pick, so choosing the current preset snaps a dragged window back; it is only reported
// for saving when it differs from the preset the combo shows.
void drawWindowSize(OverlayState& state, const OverlaySources& sources) {
    ImGui::BeginDisabled(!state.window.has_value());
    WindowStatus shown = state.window.value_or(WindowStatus{});
    if (state.window_size_request) shown.preset = *state.window_size_request;
    const std::string preview = state.window ? windowSizePreviewLabel(shown) : "n/a";
    if (ImGui::BeginCombo("Window size", preview.c_str())) {
        for (const WindowSizePreset preset : kWindowSizePresets) {
            const std::string label = windowSizeItemLabel(preset, shown.available);
            if (ImGui::Selectable(label.c_str(), preset == shown.preset)) {
                state.window_size_request = preset;
                if (preset != shown.preset && sources.setting_changed) {
                    sources.setting_changed(OverlaySetting::WindowSize, windowSizeName(preset));
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

const char* flipLimitLabel(RsxFlipLimit limit) {
    switch (limit) {
        case RsxFlipLimit::Vsync: return "VSync (game flip mode)";
        case RsxFlipLimit::Hz60: return "60 Hz";
        case RsxFlipLimit::Off: return "Off";
    }
    return "?";
}

void drawSettings(OverlayState& state, const OverlaySources& sources) {
    ImGui::BeginDisabled(!sources.set_flip_limit);
    if (ImGui::BeginCombo("Flip limiter", flipLimitLabel(state.flip_limit))) {
        for (const RsxFlipLimit limit : {RsxFlipLimit::Vsync, RsxFlipLimit::Hz60, RsxFlipLimit::Off}) {
            if (ImGui::Selectable(flipLimitLabel(limit), limit == state.flip_limit) && limit != state.flip_limit) {
                state.flip_limit = limit;
                sources.set_flip_limit(limit);
                if (sources.setting_changed)
                    sources.setting_changed(OverlaySetting::FrameLimit, rsxFlipLimitName(limit));
            }
        }
        ImGui::EndCombo();
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

    drawResolutionScale(state, sources);
    drawAntiAliasing(state, sources);
    drawFrameGen(state, sources);
    drawObjectMotion(state, sources);
    drawAnisotropy(state, sources);
    drawWindowSize(state, sources);

    // Not wired to anything yet; shown so the layout of the finished panel is visible.
    ImGui::SeparatorText("(not implemented)");
    ImGui::BeginDisabled();
    float volume = 1.0f;
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

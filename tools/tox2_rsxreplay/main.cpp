// tox2_rsxreplay: feeds a .tox2cap capture (see rsx/rsx_capture.h) through the RSX frame builder without
// the game, prints the commands it produces, renders them headless on Vulkan, writes the displayed
// frame as a PNG and prints the summary line, the unimplemented census section and the validation result.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "core/census.h"
#include "render/frame_dump.h"
#include "render/render_scale.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_unimplemented.h"

namespace {

void printUsage() {
    std::fputs(
            "usage: tox2_rsxreplay CAPTURE [CAPTURE...] [--loop N] [--out DIR]\n"
            "                      [--compare PNG [--compare-rect X,Y,W,H] [--max-diff N] [--max-mean X]\n"
            "                       [--max-differing P]]\n"
            "                      [--resolution-scale N] [--aa-mode supersample|msaa|dlaa] [--no-validation]\n"
            "                      [--dlaa-jitter on|off] [--dlaa-jitter-flip none|x|y|xy]\n"
            "                      [--dlaa-camera on|off|reset] [--dlaa-clip-flip-y on|off]\n"
            "                      [--dlaa-mv-invalid zero|unset]\n"
            "                      [--fg-mv-check [--fg-mv-flip none|x|y|xy]]\n"
            "                      [--shader-cache DIR] [--repeat N] [--quiet]\n"
            "  CAPTURE  a .tox2cap file written by tox2_game --rsx-capture; several captures are replayed in\n"
            "           order through one renderer, each one a frame that must end with a flip\n"
            "  --loop N            play the list of captures N times in order (1..1000, default 1)\n"
            "  --out DIR           write the displayed frame as DIR/<capture name>.png after every frame\n"
            "  --compare PNG       diff the displayed frame of the last frame against PNG (box-downsampled first when\n"
            "                      it is a whole multiple of the PNG's size)\n"
            "  --compare-rect X,Y,W,H  with --compare: diff only this rectangle, in the PNG's pixels\n"
            "  --max-diff N        with --compare: fail when the largest channel difference exceeds N\n"
            "  --max-mean X        with --compare: fail when the mean difference exceeds X\n"
            "  --max-differing P   with --compare: fail when more than P percent of the pixels differ\n"
            "  --resolution-scale N  render every target at N times its guest size (1..4, default 1); the PNG is\n"
            "                      written at that size\n"
            "  --aa-mode M         how anti-aliased surfaces are drawn: msaa (2-sample multisampling, default),\n"
            "                      supersample (one sample per memory-layout pixel) or dlaa (NVIDIA DLAA through\n"
            "                      Streamline); dlaa falls back to msaa when it is unavailable, and msaa falls back\n"
            "                      to supersample on a device without 2-sample support\n"
            "  --dlaa-jitter M     diagnostic, needs --aa-mode dlaa: on (default) or off (no sub-pixel jitter)\n"
            "  --dlaa-jitter-flip F  diagnostic, needs --aa-mode dlaa: negate the jitter offset reported to DLAA on\n"
            "                      the x axis, the y axis or both (none by default); the geometry does not change\n"
            "  --dlaa-camera M     diagnostic, needs --aa-mode dlaa: describe the camera motion between frames from the\n"
            "                      scene draws' projections (on, default), with identity matrices (off), or drop the\n"
            "                      history at every frame (reset)\n"
            "  --dlaa-clip-flip-y M  diagnostic, needs --aa-mode dlaa: on (default) or off, whether the camera's\n"
            "                      clip-to-clip matrices are mirrored in y for Streamline's y-up clip space\n"
            "  --dlaa-mv-invalid V  diagnostic, needs --aa-mode dlaa: motionVectorsInvalidValue 0 (zero, default) or the\n"
            "                      SDK default (unset, which makes Streamline warn once per process)\n"
            "  --fg-mv-check       diagnostic, needs --aa-mode dlaa: draw the camera motion vectors that frame generation\n"
            "                      will use and, from the second frame on, print how well they warp the previous DLAA\n"
            "                      output onto the current one (the displayed frame does not change)\n"
            "  --fg-mv-flip F      diagnostic, needs --fg-mv-check: negate the motion vectors on the x axis, the y axis or\n"
            "                      both (none by default)\n"
            "  --no-validation     do not enable the Vulkan validation layer\n"
            "  --shader-cache DIR  directory of the on-disk SPIR-V cache\n"
            "  --repeat N          replay and render the capture N times (N >= 1) with one memory view, builder and\n"
            "                      renderer, and print the median time per phase (the first iteration is warm-up and\n"
            "                      left out when N >= 2); one capture only; not with --out, --compare or --loop\n"
            "  --quiet             do not print the per-command lines\n",
            stderr);
}

// "X,Y,W,H": four unsigned decimal numbers, W and H above 0. Whether the rectangle fits the image is cropRgb's check.
bool parseRect(std::string_view text, std::uint32_t (&rect)[4]) {
    for (int i = 0; i < 4; i++) {
        const std::size_t end = text.find(',');
        if ((i < 3) != (end != std::string_view::npos)) return false;
        const std::string_view field = text.substr(0, end);
        std::uint32_t value = 0;
        const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (field.empty() || ec != std::errc() || ptr != field.data() + field.size()) return false;
        rect[i] = value;
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    }
    return rect[2] > 0 && rect[3] > 0;
}

const char* primitiveName(tox2::RsxPrimitive primitive) {
    switch (primitive) {
        case tox2::RsxPrimitive::Points: return "POINTS";
        case tox2::RsxPrimitive::Lines: return "LINES";
        case tox2::RsxPrimitive::LineLoop: return "LINE_LOOP";
        case tox2::RsxPrimitive::LineStrip: return "LINE_STRIP";
        case tox2::RsxPrimitive::Triangles: return "TRIANGLES";
        case tox2::RsxPrimitive::TriangleStrip: return "TRIANGLE_STRIP";
        case tox2::RsxPrimitive::TriangleFan: return "TRIANGLE_FAN";
        case tox2::RsxPrimitive::Quads: return "QUADS";
        case tox2::RsxPrimitive::QuadStrip: return "QUAD_STRIP";
        case tox2::RsxPrimitive::Polygon: return "POLYGON";
    }
    return "?";
}

// The unimplemented census provider is registered here because only the game installs the RSX
// consumer's own.
tox2::RsxUnimplementedCensus unimplementedCensus() {
    return {tox2::rsxUnimplementedSnapshot(), tox2::rsxUnimplementedDropped()};
}

// The same text the game prints at stop, cut out of the whole census.
std::string unimplementedSection() {
    const std::string census = tox2::formatCensus();
    constexpr std::string_view kHeader = "census: rsx unimplemented";
    const std::size_t begin = census.find(kHeader);
    if (begin == std::string::npos) return {};
    const std::size_t end = census.find("\ncensus:", begin);
    return census.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void printClear(const tox2::RsxClear& clear) {
    const tox2::RsxSurface& color = clear.surfaces.color;
    std::printf("clear mask=%#x color=0x%08x %ux%u@0x%x", clear.mask, clear.color_argb, color.width, color.height,
                color.offset);
    if (clear.surfaces.aa_mode != 0) std::printf(" aa=%u", clear.surfaces.aa_mode);
    std::printf("\n");
}

const char* surfaceFormatName(tox2::RsxColorFormat format) {
    switch (format) {
        case tox2::RsxColorFormat::A8R8G8B8: return "A8R8G8B8";
        case tox2::RsxColorFormat::F_X32: return "F_X32";
        default: return "?";
    }
}

void printDraw(const tox2::RsxDraw& draw, std::span<const tox2::RsxSurface> surfaces) {
    std::printf("draw %s count=%u vp=%016llx fp=%016llx", primitiveName(draw.primitive), draw.count,
                static_cast<unsigned long long>(draw.vp_hash), static_cast<unsigned long long>(draw.fp_hash));
    const tox2::RsxSurfaceState& target = draw.surfaces;
    if (target.has_color) {
        std::printf(" color=%ux%u@%#x:%s pitch=%#x", target.color.width, target.color.height, target.color.offset,
                    surfaceFormatName(target.color.format), target.color.pitch);
        if (target.aa_mode != 0) std::printf(" aa=%u msaa=%d", target.aa_mode, draw.raster.msaa ? 1 : 0);
    }
    if (target.depth_format != tox2::RsxDepthFormat::None) {
        const char* depth_name = target.depth_format == tox2::RsxDepthFormat::Z24S8 ? "Z24S8"
                                 : target.depth_format == tox2::RsxDepthFormat::Z16 ? "Z16"
                                                                                    : "?";
        std::printf(" depth=%s@%#x dpitch=%#x", depth_name, target.depth.offset, target.depth.pitch);
        std::printf(" zt=%d zw=%d st=%d", draw.raster.depth_test ? 1 : 0, draw.raster.depth_write ? 1 : 0,
                    draw.raster.stencil_test ? 1 : 0);
    }
    for (const tox2::RsxTextureDesc& texture : draw.textures) {
        if (texture.rt_alias >= 0 && static_cast<std::size_t>(texture.rt_alias) < surfaces.size()) {
            const tox2::RsxSurface& surface = surfaces[static_cast<std::size_t>(texture.rt_alias)];
            // The surface size comes from the capture's known-surface table and can be stale, so the address the
            // texture reads, its pitch and its sampling state are printed as well.
            std::printf(" tex%u=alias:%d(%ux%u as %ux%u %s @%u:%#x pitch=%#x norm=%d filter=%u/%u)", texture.unit,
                        texture.rt_alias, surface.width, surface.height, texture.width, texture.height,
                        tox2::rsxTextureFormatName(texture.format).c_str(), texture.location, texture.offset,
                        texture.pitch, texture.normalized ? 1 : 0, tox2::rsxTextureMinFilter(texture.filter),
                        tox2::rsxTextureMagFilter(texture.filter));
        } else {
            const tox2::RsxTextureKind kind = tox2::rsxTextureKind(texture);
            const std::string shape = kind == tox2::RsxTextureKind::Tex3D  ? std::format("x{} 3D", texture.depth)
                                      : kind == tox2::RsxTextureKind::Cube ? " cube"
                                      : kind == tox2::RsxTextureKind::Unsupported
                                              ? std::format(" dim{}", texture.dimension)
                                              : std::string();
            std::printf(" tex%u=bytes(%ux%u%s %s mips=%u)", texture.unit, texture.width, texture.height, shape.c_str(),
                        tox2::rsxTextureFormatName(texture.format).c_str(), texture.mips);
        }
    }
    std::printf("\n");
}

using Clock = std::chrono::steady_clock;

std::uint64_t nanosSince(Clock::time_point begin) {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
}

// What one --repeat iteration measured: times in nanoseconds, counters as deltas over the iteration.
struct IterationSample {
    std::uint64_t build_ns = 0, build_draw_ns = 0, record_draw_ns = 0, record_clear_ns = 0, submit_ns = 0,
                  gpu_wait_ns = 0;
    std::uint64_t draws = 0, fp_hits = 0, fp_misses = 0, vp_hits = 0, vp_misses = 0;
    std::uint64_t bytes_copied = 0, bytes_union = 0;
    std::uint64_t barriers = 0, barriers_skipped = 0, instances = 0, state_calls = 0, state_skipped = 0;
    std::uint64_t uploads_reused = 0;
};

std::uint64_t median(const std::vector<IterationSample>& samples, std::uint64_t IterationSample::* field) {
    std::vector<std::uint64_t> values;
    values.reserve(samples.size());
    for (const IterationSample& sample : samples) values.push_back(sample.*field);
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double toMs(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

unsigned long long toUll(std::uint64_t value) { return static_cast<unsigned long long>(value); }

std::string memoryFlagNames(VkMemoryPropertyFlags flags) {
    std::string text = std::format("{:#x}", static_cast<std::uint32_t>(flags));
    if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) text += " device-local";
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) text += " host-visible";
    if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) text += " host-coherent";
    if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) text += " host-cached";
    return text;
}

// The DLAA counters of a replay, for the line after the summary; empty unless the renderer runs DLAA.
std::string dlaaStatsText(const tox2::RsxRenderer& renderer) {
    if (renderer.aaMode() != tox2::AaMode::Dlaa) return {};
    const tox2::RendererStats& stats = renderer.stats();
    return std::format("dlaa: {} evaluations, {} resets, {} failures", stats.dlaa_evaluations, stats.dlaa_resets,
                       stats.dlaa_failures);
}

// What the closing lines need from a --repeat run (the last iteration's figures).
struct TimedResult {
    std::size_t commands = 0, draws = 0, skipped = 0;
    tox2::AaMode aa_mode = tox2::AaMode::Msaa;
    std::string dlaa_text;  // the "dlaa:" line, empty unless the effective mode is dlaa
};

// Replays and renders the capture `repeat` times with one memory view, builder and renderer, so the texture cache,
// program caches and pipelines are warm as in the game's steady state. Each command is executed on its own, so the
// recording time is split by command type; time spent waiting for a frame slot is reported apart from it. The
// unimplemented census is reset per iteration so the printed section is that of one replay.
bool runTimedReplay(const tox2::RsxCaptureReader& reader, const std::string& capture_name, std::uint32_t repeat,
                    tox2::VkContext& context, const tox2::RenderOptions& render_options, TimedResult& result) {
    tox2::ReplayRsxMemoryView memory;
    tox2::RsxFrameBuilder builder(memory);
    builder.setTimingEnabled(true);
    tox2::RsxRenderer renderer(context, render_options);
    result.aa_mode = renderer.aaMode();

    std::vector<IterationSample> samples;
    for (std::uint32_t iteration = 0; iteration < repeat; iteration++) {
        tox2::rsxUnimplementedReset();
        const tox2::RsxBuilderStats builder_before = builder.stats();
        IterationSample sample;

        Clock::time_point begin = Clock::now();
        tox2::replayCapture(reader, memory, builder);
        std::vector<tox2::RsxCommand> commands = builder.takeCommands();
        sample.build_ns = nanosSince(begin);
        const tox2::RsxBuilderStats& builder_after = builder.stats();
        sample.build_draw_ns = builder_after.build_draw_ns - builder_before.build_draw_ns;
        sample.fp_hits = builder_after.fp_cache_hits - builder_before.fp_cache_hits;
        sample.fp_misses = builder_after.fp_cache_misses - builder_before.fp_cache_misses;
        sample.vp_hits = builder_after.vp_cache_hits - builder_before.vp_cache_hits;
        sample.vp_misses = builder_after.vp_cache_misses - builder_before.vp_cache_misses;
        sample.bytes_copied = builder_after.vertex_bytes_copied - builder_before.vertex_bytes_copied;
        sample.bytes_union = builder_after.vertex_bytes_union - builder_before.vertex_bytes_union;

        const tox2::RendererStats renderer_before = renderer.stats();
        for (tox2::RsxCommand& command : commands) {
            const std::uint64_t wait_before = renderer.stats().slot_wait_ns;
            begin = Clock::now();
            renderer.execute(std::span<tox2::RsxCommand>(&command, 1));
            const std::uint64_t elapsed = nanosSince(begin);
            const std::uint64_t wait = std::min(renderer.stats().slot_wait_ns - wait_before, elapsed);
            sample.gpu_wait_ns += wait;
            const bool is_draw = std::holds_alternative<tox2::RsxDraw>(command);
            std::uint64_t& phase = is_draw                                           ? sample.record_draw_ns
                                   : std::holds_alternative<tox2::RsxClear>(command) ? sample.record_clear_ns
                                                                                     : sample.submit_ns;
            phase += elapsed - wait;
            if (is_draw) sample.draws++;
        }
        if (!renderer.valid()) {
            std::fprintf(stderr, "error: %s\n", renderer.error().c_str());
            return false;
        }
        const tox2::RendererStats& renderer_after = renderer.stats();
        sample.barriers = renderer_after.barriers - renderer_before.barriers;
        sample.barriers_skipped = renderer_after.barriers_skipped - renderer_before.barriers_skipped;
        sample.instances = renderer_after.rendering_instances - renderer_before.rendering_instances;
        sample.state_calls = renderer_after.state_calls - renderer_before.state_calls;
        sample.state_skipped = renderer_after.state_calls_skipped - renderer_before.state_calls_skipped;
        sample.uploads_reused = renderer_after.vp_constant_uploads_reused - renderer_before.vp_constant_uploads_reused;

        result.commands = commands.size();
        result.draws = sample.draws;
        result.skipped = renderer_after.draws_skipped - renderer_before.draws_skipped;
        builder.recycle(std::move(commands));
        // The first iteration creates pipelines and uploads textures.
        if (iteration > 0 || repeat == 1) samples.push_back(sample);
    }

    const double build = toMs(median(samples, &IterationSample::build_ns));
    const double record_draw = toMs(median(samples, &IterationSample::record_draw_ns));
    const double record_clear = toMs(median(samples, &IterationSample::record_clear_ns));
    const double submit = toMs(median(samples, &IterationSample::submit_ns));
    std::printf(
            "timing: %s, %u iteration%s (%s), median ms per iteration: build %.2f (buildDraw %.2f), recordDraw %.2f, "
            "recordClear %.2f, submit %.2f, gpu wait %.2f, total %.2f\n",
            capture_name.c_str(), repeat, repeat == 1 ? "" : "s", repeat == 1 ? "none excluded" : "first excluded",
            build, toMs(median(samples, &IterationSample::build_draw_ns)), record_draw, record_clear, submit,
            toMs(median(samples, &IterationSample::gpu_wait_ns)), build + record_draw + record_clear + submit);
    std::printf(
            "builder: per iteration %llu draws, fp cache %llu hits %llu misses, vp cache %llu hits %llu misses, "
            "vertex bytes %llu copied (union %llu)\n",
            toUll(median(samples, &IterationSample::draws)), toUll(median(samples, &IterationSample::fp_hits)),
            toUll(median(samples, &IterationSample::fp_misses)), toUll(median(samples, &IterationSample::vp_hits)),
            toUll(median(samples, &IterationSample::vp_misses)), toUll(median(samples, &IterationSample::bytes_copied)),
            toUll(median(samples, &IterationSample::bytes_union)));
    std::printf(
            "render: per iteration %llu barriers (%llu skipped), %llu rendering instances, %llu state calls (%llu "
            "skipped), %llu vp constant uploads reused; upload memory %s\n",
            toUll(median(samples, &IterationSample::barriers)),
            toUll(median(samples, &IterationSample::barriers_skipped)),
            toUll(median(samples, &IterationSample::instances)), toUll(median(samples, &IterationSample::state_calls)),
            toUll(median(samples, &IterationSample::state_skipped)),
            toUll(median(samples, &IterationSample::uploads_reused)),
            memoryFlagNames(renderer.stats().upload_memory_flags).c_str());
    result.dlaa_text = dlaaStatsText(renderer);
    return true;
}

// The closing lines of every run: the replay summary, the unimplemented census and the validation result.
int printSummary(std::size_t commands, std::size_t draws, std::size_t skipped, std::size_t frames,
                 std::uint32_t resolution_scale, tox2::AaMode requested_aa_mode, tox2::AaMode effective_aa_mode,
                 const std::string& diff_text, const std::string& dlaa_text, const std::string& png_text,
                 bool validation, bool validation_active, std::uint32_t validation_errors, int exit_code) {
    const std::string frames_text = frames > 1 ? std::format(", frames {}", frames) : "";
    const std::string scale_text = resolution_scale > 1 ? std::format(", resolution scale {}", resolution_scale) : "";
    // The default (msaa) is not mentioned unless it replaced a requested dlaa; any other effective mode is, so a log
    // shows which mode drew the frame.
    std::string aa_text;
    if (requested_aa_mode == tox2::AaMode::Dlaa && effective_aa_mode != tox2::AaMode::Dlaa) {
        aa_text = std::format(", aa {} (dlaa unavailable)", tox2::aaModeName(effective_aa_mode));
    } else if (effective_aa_mode != tox2::AaMode::Msaa) {
        aa_text = std::format(", aa {}", tox2::aaModeName(effective_aa_mode));
    }
    std::printf("replay: %zu commands, %zu draws, %zu skipped%s%s%s%s\n", commands, draws, skipped, frames_text.c_str(),
                scale_text.c_str(), aa_text.c_str(), diff_text.c_str());
    if (!dlaa_text.empty()) std::printf("%s\n", dlaa_text.c_str());
    // The game's census omits an empty section; a replay says so explicitly.
    const std::string section = unimplementedSection();
    std::printf("%s\n", section.empty() ? "census: rsx unimplemented 0 kinds" : section.c_str());
    if (!png_text.empty()) std::printf("%s\n", png_text.c_str());
    const char* validation_state = !validation ? "disabled" : validation_active ? "active" : "inactive";
    std::printf("validation: %s, %u errors\n", validation_state, validation_errors);
    if (validation_errors != 0) exit_code = 1;
    return exit_code;
}

// One capture of a multi-frame replay with its own memory view and builder, so the guest memory of one frame never
// leaks into the next one; only the renderer is shared.
struct ReplayFrame {
    explicit ReplayFrame(std::string capture_path)
        : path(std::move(capture_path)), name(std::filesystem::path(path).stem().string()), builder(memory) {}
    std::string path;
    std::string name;
    tox2::RsxCaptureReader reader;
    tox2::ReplayRsxMemoryView memory;
    tox2::RsxFrameBuilder builder;
};

// Writes DIR/<capture name>.png; `png_paths` keeps each file once, in the order it was first written.
bool writeFramePng(const tox2::RgbImage& image, const ReplayFrame& frame, const std::string& out_dir,
                   std::vector<std::string>& png_paths, std::string& error) {
    const std::filesystem::path png_path = std::filesystem::path(out_dir) / (frame.name + ".png");
    if (!tox2::writePngRgb(png_path, image, error)) return false;
    const std::string text = "png: " + png_path.generic_string();
    if (std::find(png_paths.begin(), png_paths.end(), text) == png_paths.end()) png_paths.push_back(text);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> capture_paths;
    std::string out_dir;
    std::string compare_path;
    std::string shader_cache;
    bool validation = true;
    bool quiet = false;
    std::uint32_t repeat = 0;  // --repeat: 0 means a single replay with the usual output
    std::uint32_t loop = 1;    // --loop: how many times the list of captures is played
    bool has_loop = false;
    std::uint32_t resolution_scale = 1;
    tox2::AaMode aa_mode = tox2::AaMode::Msaa;
    tox2::DlaaDebug dlaa_debug;
    bool has_dlaa_option = false;  // a --dlaa-* option was given, which needs --aa-mode dlaa
    tox2::FgDebug fg_debug;
    bool has_fg_mv_flip = false;  // --fg-mv-flip was given, which needs --fg-mv-check
    std::uint32_t rect[4] = {};   // --compare-rect: x, y, width, height
    bool has_rect = false;
    // Thresholds of --compare; a NaN limit is not checked.
    double max_diff = std::nan(""), max_mean = std::nan(""), max_differing = std::nan("");
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (arg == "--no-validation") {
            validation = false;
            continue;
        }
        if (arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (arg == "--repeat") {
            char* end = nullptr;
            const long value = i + 1 < argc ? std::strtol(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || value < 1 || value > 100000) {
                std::fputs("error: --repeat takes a whole number from 1 to 100000\n", stderr);
                return 2;
            }
            repeat = static_cast<std::uint32_t>(value);
            i++;
            continue;
        }
        if (arg == "--loop") {
            char* end = nullptr;
            const long value = i + 1 < argc ? std::strtol(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || value < 1 || value > 1000) {
                std::fputs("error: --loop takes a whole number from 1 to 1000\n", stderr);
                return 2;
            }
            loop = static_cast<std::uint32_t>(value);
            has_loop = true;
            i++;
            continue;
        }
        if (arg == "--resolution-scale") {
            char* end = nullptr;
            const long value = i + 1 < argc ? std::strtol(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || value < 1 ||
                value > static_cast<long>(tox2::kMaxResolutionScale)) {
                std::fprintf(stderr, "error: --resolution-scale takes a whole number from 1 to %u\n",
                             tox2::kMaxResolutionScale);
                return 2;
            }
            resolution_scale = static_cast<std::uint32_t>(value);
            i++;
            continue;
        }
        if (arg == "--aa-mode") {
            const std::optional<tox2::AaMode> mode = i + 1 < argc ? tox2::parseAaMode(argv[i + 1]) : std::nullopt;
            if (!mode) {
                std::fputs("error: --aa-mode takes supersample|msaa|dlaa\n", stderr);
                return 2;
            }
            aa_mode = *mode;
            i++;
            continue;
        }
        if (arg == "--dlaa-jitter") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "on" && value != "off") {
                std::fputs("error: --dlaa-jitter takes on|off\n", stderr);
                return 2;
            }
            dlaa_debug.jitter = value == "on";
            has_dlaa_option = true;
            i++;
            continue;
        }
        if (arg == "--dlaa-jitter-flip") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "none" && value != "x" && value != "y" && value != "xy") {
                std::fputs("error: --dlaa-jitter-flip takes none|x|y|xy\n", stderr);
                return 2;
            }
            dlaa_debug.flip_jitter_x = value == "x" || value == "xy";
            dlaa_debug.flip_jitter_y = value == "y" || value == "xy";
            has_dlaa_option = true;
            i++;
            continue;
        }
        if (arg == "--dlaa-camera") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "on" && value != "off" && value != "reset") {
                std::fputs("error: --dlaa-camera takes on|off|reset\n", stderr);
                return 2;
            }
            dlaa_debug.camera = value == "on"    ? tox2::DlaaCamera::On
                                : value == "off" ? tox2::DlaaCamera::Off
                                                 : tox2::DlaaCamera::Reset;
            has_dlaa_option = true;
            i++;
            continue;
        }
        if (arg == "--dlaa-clip-flip-y") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "on" && value != "off") {
                std::fputs("error: --dlaa-clip-flip-y takes on|off\n", stderr);
                return 2;
            }
            dlaa_debug.flip_clip_y = value == "on";
            has_dlaa_option = true;
            i++;
            continue;
        }
        if (arg == "--dlaa-mv-invalid") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "zero" && value != "unset") {
                std::fputs("error: --dlaa-mv-invalid takes zero|unset\n", stderr);
                return 2;
            }
            dlaa_debug.mvec_invalid_zero = value == "zero";
            has_dlaa_option = true;
            i++;
            continue;
        }
        if (arg == "--fg-mv-check") {
            fg_debug.mv_check = true;
            continue;
        }
        if (arg == "--fg-mv-flip") {
            const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
            if (value != "none" && value != "x" && value != "y" && value != "xy") {
                std::fputs("error: --fg-mv-flip takes none|x|y|xy\n", stderr);
                return 2;
            }
            fg_debug.mv_flip_x = value == "x" || value == "xy";
            fg_debug.mv_flip_y = value == "y" || value == "xy";
            has_fg_mv_flip = true;
            i++;
            continue;
        }
        if (arg == "--compare-rect") {
            if (i + 1 >= argc || !parseRect(argv[i + 1], rect)) {
                std::fputs("error: --compare-rect takes X,Y,W,H, four whole numbers with W and H above 0\n", stderr);
                return 2;
            }
            has_rect = true;
            i++;
            continue;
        }
        if (arg == "--max-diff" || arg == "--max-mean" || arg == "--max-differing") {
            char* end = nullptr;
            const double value = i + 1 < argc ? std::strtod(argv[i + 1], &end) : 0.0;
            if (i + 1 >= argc || end == argv[i + 1] || *end != '\0') {
                printUsage();
                return 2;
            }
            (arg == "--max-diff" ? max_diff : arg == "--max-mean" ? max_mean : max_differing) = value;
            i++;
            continue;
        }
        if (arg == "--out" || arg == "--compare" || arg == "--shader-cache") {
            if (i + 1 >= argc) {
                printUsage();
                return 2;
            }
            std::string& target = arg == "--out" ? out_dir : arg == "--compare" ? compare_path : shader_cache;
            target = argv[++i];
            continue;
        }
        if (arg.starts_with("--")) {
            printUsage();
            return 2;
        }
        capture_paths.emplace_back(arg);
    }
    const bool has_threshold = !std::isnan(max_diff) || !std::isnan(max_mean) || !std::isnan(max_differing);
    if (capture_paths.empty() || ((has_threshold || has_rect) && compare_path.empty())) {
        printUsage();
        return 2;
    }
    if (has_dlaa_option && aa_mode != tox2::AaMode::Dlaa) {
        std::fputs("error: the --dlaa-* options need --aa-mode dlaa\n", stderr);
        return 2;
    }
    if (has_fg_mv_flip && !fg_debug.mv_check) {
        std::fputs("error: --fg-mv-flip needs --fg-mv-check\n", stderr);
        return 2;
    }
    if (fg_debug.mv_check && aa_mode != tox2::AaMode::Dlaa) {
        std::fputs("error: --fg-mv-check needs --aa-mode dlaa\n", stderr);
        return 2;
    }
    if (repeat != 0 && fg_debug.mv_check) {
        std::fputs("error: --repeat cannot be combined with --fg-mv-check\n", stderr);
        return 2;
    }
    if (repeat != 0 && (!out_dir.empty() || !compare_path.empty())) {
        // Later iterations draw over the previous iteration's targets, so the image is not the capture's frame.
        std::fputs("error: --repeat cannot be combined with --out or --compare\n", stderr);
        return 2;
    }
    if (repeat != 0 && (capture_paths.size() > 1 || has_loop)) {
        std::fputs("error: --repeat takes one capture and cannot be combined with --loop\n", stderr);
        return 2;
    }

    std::string error;
    std::vector<std::unique_ptr<ReplayFrame>> frames;
    for (const std::string& path : capture_paths) {
        frames.push_back(std::make_unique<ReplayFrame>(path));
        if (!frames.back()->reader.load(path, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
    }

    tox2::setRsxUnimplementedProvider(&unimplementedCensus, &tox2::rsxUnimplementedReset);
    tox2::rsxUnimplementedReset();

    if (repeat != 0) {
        tox2::VkContextOptions timed_context_options;
        timed_context_options.validation = validation;
        timed_context_options.streamline = aa_mode == tox2::AaMode::Dlaa;
        std::unique_ptr<tox2::VkContext> timed_context = tox2::createVkContext(timed_context_options, error);
        if (!timed_context) {
            std::fprintf(stderr, "error: --repeat needs a Vulkan device: %s\n", error.c_str());
            return 1;
        }
        tox2::RenderOptions timed_options;
        timed_options.shader_cache = shader_cache;
        timed_options.resolution_scale = resolution_scale;
        timed_options.aa_mode = aa_mode;
        timed_options.dlaa_debug = dlaa_debug;
        TimedResult timed;
        const bool ok = runTimedReplay(frames.front()->reader, frames.front()->name, repeat, *timed_context,
                                       timed_options, timed);
        // The renderer is gone by now, so errors from its teardown are counted.
        return printSummary(timed.commands, timed.draws, timed.skipped, 1, resolution_scale, aa_mode, timed.aa_mode, {},
                            timed.dlaa_text, {}, validation, timed_context->validationEnabled(),
                            timed_context->validationErrorCount(), ok ? 0 : 1);
    }

    // The first pass over the list is built and printed before the device is created; later passes are rebuilt
    // right before they run, as --repeat does.
    const std::size_t frame_count = frames.size() * loop;
    std::vector<std::vector<tox2::RsxCommand>> first_pass;
    std::size_t first_pass_commands = 0, first_pass_draws = 0;
    for (const std::unique_ptr<ReplayFrame>& frame : frames) {
        tox2::replayCapture(frame->reader, frame->memory, frame->builder);
        std::vector<tox2::RsxCommand>& commands = first_pass.emplace_back(frame->builder.takeCommands());
        for (const tox2::RsxCommand& command : commands) {
            if (const auto* clear = std::get_if<tox2::RsxClear>(&command)) {
                if (!quiet) printClear(*clear);
            } else if (const auto* draw = std::get_if<tox2::RsxDraw>(&command)) {
                if (!quiet) printDraw(*draw, frame->builder.knownSurfaces());
                first_pass_draws++;
            } else if (!quiet) {
                const auto& flip = std::get<tox2::RsxFlip>(command);
                std::printf("flip buffer=%u %ux%u@0x%x\n", flip.buffer, flip.width, flip.height, flip.offset);
            }
        }
        first_pass_commands += commands.size();
        // Frame boundaries are flips: a frame that does not end with one would leak its draws into the next frame.
        if (frame_count > 1 && (commands.empty() || !std::holds_alternative<tox2::RsxFlip>(commands.back()))) {
            std::fprintf(stderr,
                         "error: %s does not end with a flip, so it cannot be replayed as one frame of several\n",
                         frame->name.c_str());
            return 2;
        }
    }
    std::size_t commands_total = first_pass_commands * loop;
    std::size_t draws = first_pass_draws * loop;

    // Without a Vulkan device the replay still prints the command list and counts every draw as skipped.
    tox2::VkContextOptions context_options;
    context_options.validation = validation;
    context_options.streamline = aa_mode == tox2::AaMode::Dlaa;
    std::unique_ptr<tox2::VkContext> context = tox2::createVkContext(context_options, error);
    if (!context) std::printf("render: unavailable: %s\n", error.c_str());

    int exit_code = 0;
    std::size_t skipped = draws;
    std::string diff_text;
    std::string png_text;
    std::string dlaa_text;
    std::uint32_t validation_errors = 0;
    bool validation_active = false;
    tox2::AaMode effective_aa_mode = aa_mode;
    if (context) {
        tox2::RenderOptions render_options;
        render_options.shader_cache = shader_cache;
        render_options.resolution_scale = resolution_scale;
        render_options.aa_mode = aa_mode;
        render_options.dlaa_debug = dlaa_debug;
        render_options.fg_debug = fg_debug;
        render_options.readback_all = true;  // the replay reads the display after its last flip
        {
            tox2::RsxRenderer renderer(*context, render_options);
            effective_aa_mode = renderer.aaMode();

            // Every frame but the last is executed here; the last one is read back below with the --compare checks.
            std::vector<std::string> png_paths;
            std::size_t executed_commands = 0, executed_draws = 0;
            std::vector<std::uint8_t> bgra;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint64_t fg_mv_printed = 0;
            for (std::uint32_t pass = 0; pass < loop && renderer.valid(); pass++) {
                for (std::size_t index = 0; index < frames.size() && renderer.valid(); index++) {
                    ReplayFrame& frame = *frames[index];
                    std::vector<tox2::RsxCommand> commands;
                    if (pass == 0) {
                        commands = std::move(first_pass[index]);
                    } else {
                        tox2::replayCapture(frame.reader, frame.memory, frame.builder);
                        commands = frame.builder.takeCommands();
                    }
                    executed_commands += commands.size();
                    for (const tox2::RsxCommand& command : commands) {
                        if (std::holds_alternative<tox2::RsxDraw>(command)) executed_draws++;
                    }
                    renderer.execute(commands);
                    frame.builder.recycle(std::move(commands));
                    const tox2::RendererStats& stats = renderer.stats();
                    if (stats.fg_mv_checks != fg_mv_printed) {
                        fg_mv_printed = stats.fg_mv_checks;
                        std::printf("fg mv: frame %zu, warp mean %.3f, no-motion mean %.3f, %u pixels%s\n",
                                    pass * frames.size() + index + 1, stats.fg_mv_last.warp_mean,
                                    stats.fg_mv_last.no_motion_mean, stats.fg_mv_last.pixels,
                                    stats.fg_mv_last_reset ? ", reset" : "");
                    }
                    const bool last_frame = pass + 1 == loop && index + 1 == frames.size();
                    if (!last_frame && !out_dir.empty() && renderer.valid()) {
                        if (!renderer.readDisplay(bgra, width, height)) {
                            std::fprintf(stderr, "error: no frame to display after %s\n", frame.name.c_str());
                            exit_code = 1;
                        } else if (!writeFramePng(tox2::rgbFromBgra(bgra, width, height), frame, out_dir, png_paths,
                                                  error)) {
                            std::fprintf(stderr, "error: %s\n", error.c_str());
                            exit_code = 1;
                        }
                    }
                }
            }
            commands_total = executed_commands;
            draws = executed_draws;
            skipped = renderer.stats().draws_skipped;
            dlaa_text = dlaaStatsText(renderer);

            if (renderer.valid() && renderer.readDisplay(bgra, width, height)) {
                const tox2::RgbImage image = tox2::rgbFromBgra(bgra, width, height);
                if (!out_dir.empty()) {
                    if (!writeFramePng(image, *frames.back(), out_dir, png_paths, error)) {
                        std::fprintf(stderr, "error: %s\n", error.c_str());
                        exit_code = 1;
                    }
                }
                for (const std::string& text : png_paths) png_text += (png_text.empty() ? "" : "\n") + text;
                if (!compare_path.empty()) {
                    tox2::RgbImage reference;
                    tox2::ImageDiff diff;
                    std::uint32_t factor_x = 1;
                    std::uint32_t factor_y = 1;
                    bool compared = tox2::readPngRgb(compare_path, reference, error);
                    // A frame rendered at a higher resolution scale is brought to the reference's size first;
                    // any other size mismatch is left to diffImages to report.
                    if (compared && reference.width != 0 && reference.height != 0 &&
                        image.width % reference.width == 0 && image.height % reference.height == 0) {
                        factor_x = image.width / reference.width;
                        factor_y = image.height / reference.height;
                    }
                    tox2::RgbImage reduced;
                    if (compared && (factor_x > 1 || factor_y > 1)) {
                        compared = tox2::downsampleBox(image, factor_x, factor_y, reduced, error);
                    }
                    const tox2::RgbImage& shown = factor_x > 1 || factor_y > 1 ? reduced : image;
                    if (compared && has_rect) {
                        // The rectangle is in reference pixels, so it is cut after the downsample.
                        tox2::RgbImage shown_rect;
                        tox2::RgbImage reference_rect;
                        compared =
                                tox2::cropRgb(shown, rect[0], rect[1], rect[2], rect[3], shown_rect, error) &&
                                tox2::cropRgb(reference, rect[0], rect[1], rect[2], rect[3], reference_rect, error) &&
                                tox2::diffImages(shown_rect, reference_rect, diff, error);
                    } else if (compared) {
                        compared = tox2::diffImages(shown, reference, diff, error);
                    }
                    if (compared) {
                        char text[200];
                        std::snprintf(text, sizeof(text), "; diff max %u mean %.3f differing %.3f%%", diff.max,
                                      diff.mean, diff.differing_percent);
                        if (factor_x > 1 || factor_y > 1) {
                            std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text),
                                          "; downsampled %ux%u", factor_x, factor_y);
                        }
                        if (has_rect) {
                            std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text),
                                          "; rect %ux%u+%u+%u", rect[2], rect[3], rect[0], rect[1]);
                        }
                        diff_text = text;
                        const auto exceeded = [](double value, double limit) {
                            return !std::isnan(limit) && value > limit;
                        };
                        if (exceeded(diff.max, max_diff) || exceeded(diff.mean, max_mean) ||
                            exceeded(diff.differing_percent, max_differing)) {
                            std::fprintf(stderr, "error: frame differs from the reference beyond the thresholds (%s)\n",
                                         text + 2);
                            exit_code = 1;
                        }
                    } else {
                        std::fprintf(stderr, "error: %s\n", error.c_str());
                        exit_code = 1;
                    }
                }
            } else {
                std::fprintf(stderr, "error: no frame to display%s%s\n", renderer.valid() ? "" : ": ",
                             renderer.error().c_str());
                exit_code = 1;
            }
        }
        // Read after the renderer is gone so errors from its teardown count too.
        validation_active = context->validationEnabled();
        validation_errors = context->validationErrorCount();
    }

    return printSummary(commands_total, draws, skipped, frame_count, resolution_scale, aa_mode, effective_aa_mode,
                        diff_text, dlaa_text, png_text, validation, validation_active, validation_errors, exit_code);
}

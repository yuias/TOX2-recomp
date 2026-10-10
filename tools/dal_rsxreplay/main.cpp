// dal_rsxreplay: feeds a .dalcap capture (see rsx/rsx_capture.h) through the RSX frame builder without
// the game, prints the commands it produces, renders them headless on Vulkan, writes the displayed
// frame as a PNG and prints the summary line, the unimplemented census section and the validation result.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/census.h"
#include "render/frame_dump.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_unimplemented.h"

namespace {

void printUsage() {
    std::fputs(
            "usage: dal_rsxreplay CAPTURE [--out DIR] [--compare PNG [--max-diff N] [--max-mean X] [--max-differing P]]\n"
            "                      [--no-validation] [--shader-cache DIR]\n"
            "  CAPTURE  a .dalcap file written by dal_game --rsx-capture\n"
            "  --out DIR           write the displayed frame as DIR/<capture name>.png\n"
            "  --compare PNG       diff the displayed frame against PNG\n"
            "  --max-diff N        with --compare: fail when the largest channel difference exceeds N\n"
            "  --max-mean X        with --compare: fail when the mean difference exceeds X\n"
            "  --max-differing P   with --compare: fail when more than P percent of the pixels differ\n"
            "  --no-validation     do not enable the Vulkan validation layer\n"
            "  --shader-cache DIR  directory of the on-disk SPIR-V cache\n",
            stderr);
}

const char* primitiveName(dal::RsxPrimitive primitive) {
    switch (primitive) {
        case dal::RsxPrimitive::Points: return "POINTS";
        case dal::RsxPrimitive::Lines: return "LINES";
        case dal::RsxPrimitive::LineLoop: return "LINE_LOOP";
        case dal::RsxPrimitive::LineStrip: return "LINE_STRIP";
        case dal::RsxPrimitive::Triangles: return "TRIANGLES";
        case dal::RsxPrimitive::TriangleStrip: return "TRIANGLE_STRIP";
        case dal::RsxPrimitive::TriangleFan: return "TRIANGLE_FAN";
        case dal::RsxPrimitive::Quads: return "QUADS";
        case dal::RsxPrimitive::QuadStrip: return "QUAD_STRIP";
        case dal::RsxPrimitive::Polygon: return "POLYGON";
    }
    return "?";
}

// The unimplemented census provider is registered here because only the game installs the RSX
// consumer's own.
dal::RsxUnimplementedCensus unimplementedCensus() {
    return {dal::rsxUnimplementedSnapshot(), dal::rsxUnimplementedDropped()};
}

// The same text the game prints at stop, cut out of the whole census.
std::string unimplementedSection() {
    const std::string census = dal::formatCensus();
    constexpr std::string_view kHeader = "census: rsx unimplemented";
    const std::size_t begin = census.find(kHeader);
    if (begin == std::string::npos) return {};
    const std::size_t end = census.find("\ncensus:", begin);
    return census.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void printClear(const dal::RsxClear& clear) {
    const dal::RsxSurface& color = clear.surfaces.color;
    std::printf("clear mask=%#x color=0x%08x %ux%u@0x%x\n", clear.mask, clear.color_argb, color.width, color.height,
                color.offset);
}

void printDraw(const dal::RsxDraw& draw, std::span<const dal::RsxSurface> surfaces) {
    std::printf("draw %s count=%u vp=%016llx fp=%016llx", primitiveName(draw.primitive), draw.count,
                static_cast<unsigned long long>(draw.vp_hash), static_cast<unsigned long long>(draw.fp_hash));
    for (const dal::RsxTextureDesc& texture : draw.textures) {
        if (texture.rt_alias >= 0 && static_cast<std::size_t>(texture.rt_alias) < surfaces.size()) {
            const dal::RsxSurface& surface = surfaces[static_cast<std::size_t>(texture.rt_alias)];
            std::printf(" tex%u=alias:%d(%ux%u)", texture.unit, texture.rt_alias, surface.width, surface.height);
        } else {
            std::printf(" tex%u=bytes(%ux%u)", texture.unit, texture.width, texture.height);
        }
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture_path;
    std::string out_dir;
    std::string compare_path;
    std::string shader_cache;
    bool validation = true;
    // Thresholds of --compare; a NaN limit is not checked.
    double max_diff = std::nan(""), max_mean = std::nan(""), max_differing = std::nan("");
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (arg == "--no-validation") {
            validation = false;
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
        if (arg.starts_with("--") || !capture_path.empty()) {
            printUsage();
            return 2;
        }
        capture_path = std::string(arg);
    }
    const bool has_threshold = !std::isnan(max_diff) || !std::isnan(max_mean) || !std::isnan(max_differing);
    if (capture_path.empty() || (has_threshold && compare_path.empty())) {
        printUsage();
        return 2;
    }

    dal::RsxCaptureReader reader;
    std::string error;
    if (!reader.load(capture_path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    dal::setRsxUnimplementedProvider(&unimplementedCensus, &dal::rsxUnimplementedReset);
    dal::rsxUnimplementedReset();
    dal::ReplayRsxMemoryView memory;
    dal::RsxFrameBuilder builder(memory);
    dal::replayCapture(reader, memory, builder);
    std::vector<dal::RsxCommand> commands = builder.takeCommands();

    std::size_t draws = 0;
    for (const dal::RsxCommand& command : commands) {
        if (const auto* clear = std::get_if<dal::RsxClear>(&command)) {
            printClear(*clear);
        } else if (const auto* draw = std::get_if<dal::RsxDraw>(&command)) {
            printDraw(*draw, builder.knownSurfaces());
            draws++;
        } else {
            const auto& flip = std::get<dal::RsxFlip>(command);
            std::printf("flip buffer=%u %ux%u@0x%x\n", flip.buffer, flip.width, flip.height, flip.offset);
        }
    }

    // Without a Vulkan device the replay still prints the command list and counts every draw as skipped.
    dal::VkContextOptions context_options;
    context_options.validation = validation;
    std::unique_ptr<dal::VkContext> context = dal::createVkContext(context_options, error);
    if (!context) std::printf("render: unavailable: %s\n", error.c_str());

    int exit_code = 0;
    std::size_t skipped = draws;
    std::string diff_text;
    std::string png_text;
    std::uint32_t validation_errors = 0;
    bool validation_active = false;
    if (context) {
        dal::RenderOptions render_options;
        render_options.shader_cache = shader_cache;
        render_options.readback_all = true;  // the replay reads the display after its last flip
        {
            dal::RsxRenderer renderer(*context, render_options);
            renderer.execute(commands);
            skipped = renderer.stats().draws_skipped;

            std::vector<std::uint8_t> bgra;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (renderer.valid() && renderer.readDisplay(bgra, width, height)) {
                const dal::RgbImage image = dal::rgbFromBgra(bgra, width, height);
                if (!out_dir.empty()) {
                    const std::filesystem::path png_path =
                            std::filesystem::path(out_dir) /
                            (std::filesystem::path(capture_path).stem().string() + ".png");
                    if (dal::writePngRgb(png_path, image, error)) {
                        png_text = "png: " + png_path.generic_string();
                    } else {
                        std::fprintf(stderr, "error: %s\n", error.c_str());
                        exit_code = 1;
                    }
                }
                if (!compare_path.empty()) {
                    dal::RgbImage reference;
                    dal::ImageDiff diff;
                    if (dal::readPngRgb(compare_path, reference, error) &&
                        dal::diffImages(image, reference, diff, error)) {
                        char text[128];
                        std::snprintf(text, sizeof(text), "; diff max %u mean %.3f differing %.3f%%", diff.max,
                                      diff.mean, diff.differing_percent);
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

    std::printf("replay: %zu commands, %zu draws, %zu skipped%s\n", commands.size(), draws, skipped, diff_text.c_str());
    // The game's census omits an empty section; a replay says so explicitly.
    const std::string section = unimplementedSection();
    std::printf("%s\n", section.empty() ? "census: rsx unimplemented 0 kinds" : section.c_str());
    if (!png_text.empty()) std::printf("%s\n", png_text.c_str());
    const char* validation_state = !validation ? "disabled" : validation_active ? "active" : "inactive";
    std::printf("validation: %s, %u errors\n", validation_state, validation_errors);
    if (validation_errors != 0) exit_code = 1;
    return exit_code;
}

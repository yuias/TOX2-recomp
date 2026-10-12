// tox2_rsxreplay: feeds a .tox2cap capture (see rsx/rsx_capture.h) through the RSX frame builder without
// the game, prints the commands it produces, renders them headless on Vulkan, writes the displayed
// frame as a PNG and prints the summary line, the unimplemented census section and the validation result.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
            "usage: tox2_rsxreplay CAPTURE [--out DIR] [--compare PNG [--max-diff N] [--max-mean X] [--max-differing P]]\n"
            "                      [--resolution-scale N] [--no-validation] [--shader-cache DIR]\n"
            "  CAPTURE  a .tox2cap file written by tox2_game --rsx-capture\n"
            "  --out DIR           write the displayed frame as DIR/<capture name>.png\n"
            "  --compare PNG       diff the displayed frame against PNG (box-downsampled first when it is a whole\n"
            "                      multiple of the PNG's size)\n"
            "  --max-diff N        with --compare: fail when the largest channel difference exceeds N\n"
            "  --max-mean X        with --compare: fail when the mean difference exceeds X\n"
            "  --max-differing P   with --compare: fail when more than P percent of the pixels differ\n"
            "  --resolution-scale N  render every target at N times its guest size (1..4, default 1); the PNG is\n"
            "                      written at that size\n"
            "  --no-validation     do not enable the Vulkan validation layer\n"
            "  --shader-cache DIR  directory of the on-disk SPIR-V cache\n",
            stderr);
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
        std::printf(" zt=%d zw=%d", draw.raster.depth_test ? 1 : 0, draw.raster.depth_write ? 1 : 0);
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

}  // namespace

int main(int argc, char** argv) {
    std::string capture_path;
    std::string out_dir;
    std::string compare_path;
    std::string shader_cache;
    bool validation = true;
    std::uint32_t resolution_scale = 1;
    // Thresholds of --compare; a NaN limit is not checked.
    double max_diff = std::nan(""), max_mean = std::nan(""), max_differing = std::nan("");
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (arg == "--no-validation") {
            validation = false;
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

    tox2::RsxCaptureReader reader;
    std::string error;
    if (!reader.load(capture_path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    tox2::setRsxUnimplementedProvider(&unimplementedCensus, &tox2::rsxUnimplementedReset);
    tox2::rsxUnimplementedReset();
    tox2::ReplayRsxMemoryView memory;
    tox2::RsxFrameBuilder builder(memory);
    tox2::replayCapture(reader, memory, builder);
    std::vector<tox2::RsxCommand> commands = builder.takeCommands();

    std::size_t draws = 0;
    for (const tox2::RsxCommand& command : commands) {
        if (const auto* clear = std::get_if<tox2::RsxClear>(&command)) {
            printClear(*clear);
        } else if (const auto* draw = std::get_if<tox2::RsxDraw>(&command)) {
            printDraw(*draw, builder.knownSurfaces());
            draws++;
        } else {
            const auto& flip = std::get<tox2::RsxFlip>(command);
            std::printf("flip buffer=%u %ux%u@0x%x\n", flip.buffer, flip.width, flip.height, flip.offset);
        }
    }

    // Without a Vulkan device the replay still prints the command list and counts every draw as skipped.
    tox2::VkContextOptions context_options;
    context_options.validation = validation;
    std::unique_ptr<tox2::VkContext> context = tox2::createVkContext(context_options, error);
    if (!context) std::printf("render: unavailable: %s\n", error.c_str());

    int exit_code = 0;
    std::size_t skipped = draws;
    std::string diff_text;
    std::string png_text;
    std::uint32_t validation_errors = 0;
    bool validation_active = false;
    if (context) {
        tox2::RenderOptions render_options;
        render_options.shader_cache = shader_cache;
        render_options.resolution_scale = resolution_scale;
        render_options.readback_all = true;  // the replay reads the display after its last flip
        {
            tox2::RsxRenderer renderer(*context, render_options);
            renderer.execute(commands);
            skipped = renderer.stats().draws_skipped;

            std::vector<std::uint8_t> bgra;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (renderer.valid() && renderer.readDisplay(bgra, width, height)) {
                const tox2::RgbImage image = tox2::rgbFromBgra(bgra, width, height);
                if (!out_dir.empty()) {
                    const std::filesystem::path png_path =
                            std::filesystem::path(out_dir) /
                            (std::filesystem::path(capture_path).stem().string() + ".png");
                    if (tox2::writePngRgb(png_path, image, error)) {
                        png_text = "png: " + png_path.generic_string();
                    } else {
                        std::fprintf(stderr, "error: %s\n", error.c_str());
                        exit_code = 1;
                    }
                }
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
                    if (compared && (factor_x > 1 || factor_y > 1)) {
                        tox2::RgbImage reduced;
                        compared = tox2::downsampleBox(image, factor_x, factor_y, reduced, error) &&
                                   tox2::diffImages(reduced, reference, diff, error);
                    } else if (compared) {
                        compared = tox2::diffImages(image, reference, diff, error);
                    }
                    if (compared) {
                        char text[160];
                        std::snprintf(text, sizeof(text), "; diff max %u mean %.3f differing %.3f%%", diff.max,
                                      diff.mean, diff.differing_percent);
                        if (factor_x > 1 || factor_y > 1) {
                            std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text),
                                          "; downsampled %ux%u", factor_x, factor_y);
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

    const std::string scale_text = resolution_scale > 1 ? std::format(", resolution scale {}", resolution_scale) : "";
    std::printf("replay: %zu commands, %zu draws, %zu skipped%s%s\n", commands.size(), draws, skipped,
                scale_text.c_str(), diff_text.c_str());
    // The game's census omits an empty section; a replay says so explicitly.
    const std::string section = unimplementedSection();
    std::printf("%s\n", section.empty() ? "census: rsx unimplemented 0 kinds" : section.c_str());
    if (!png_text.empty()) std::printf("%s\n", png_text.c_str());
    const char* validation_state = !validation ? "disabled" : validation_active ? "active" : "inactive";
    std::printf("validation: %s, %u errors\n", validation_state, validation_errors);
    if (validation_errors != 0) exit_code = 1;
    return exit_code;
}

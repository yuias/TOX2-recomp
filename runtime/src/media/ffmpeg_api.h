#pragma once

// Run-time loader for the FFmpeg shared libraries. Nothing links against FFmpeg: the libraries are
// mapped with LoadLibraryExW (Windows) or dlopen (macOS) and their entry points are resolved by
// name, so missing or mismatched libraries become a reported "unavailable" result instead of a
// loader error.

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

// The FFmpeg headers carry no C++ linkage guards.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

namespace tox2 {

// Every FFmpeg function the runtime calls, grouped by the library that exports it.
// X(library, function)
#define TOX2_FFMPEG_FUNCTIONS(X)             \
    X(avutil, avutil_version)                \
    X(avutil, av_malloc)                     \
    X(avutil, av_free)                       \
    X(avutil, av_strerror)                   \
    X(avutil, av_frame_alloc)                \
    X(avutil, av_frame_free)                 \
    X(avutil, av_frame_unref)                \
    X(avutil, av_log_set_callback)           \
    X(avutil, av_log_set_level)              \
    X(avutil, av_log_format_line2)           \
    X(avutil, av_channel_layout_default)     \
    X(avcodec, avcodec_version)              \
    X(avcodec, avcodec_get_name)             \
    X(avcodec, avcodec_find_decoder)         \
    X(avcodec, avcodec_find_decoder_by_name) \
    X(avcodec, avcodec_alloc_context3)       \
    X(avcodec, avcodec_open2)                \
    X(avcodec, avcodec_send_packet)          \
    X(avcodec, avcodec_receive_frame)        \
    X(avcodec, avcodec_flush_buffers)        \
    X(avcodec, avcodec_free_context)         \
    X(avcodec, av_parser_init)               \
    X(avcodec, av_parser_parse2)             \
    X(avcodec, av_parser_close)              \
    X(avcodec, av_packet_alloc)              \
    X(avcodec, av_packet_free)               \
    X(avcodec, av_packet_unref)              \
    X(avcodec, av_new_packet)                \
    X(swscale, swscale_version)              \
    X(swscale, sws_getCachedContext)         \
    X(swscale, sws_setColorspaceDetails)     \
    X(swscale, sws_scale)                    \
    X(swscale, sws_freeContext)              \
    X(swscale, sws_getCoefficients)

// Member names match FFmpeg's own, so a call site reads like the FFmpeg documentation. The types
// come from the headers, so a signature change breaks the build instead of the run.
struct FfmpegApi {
#define TOX2_FFMPEG_MEMBER(library, name) decltype(&::name) name = nullptr;
    TOX2_FFMPEG_FUNCTIONS(TOX2_FFMPEG_MEMBER)
#undef TOX2_FFMPEG_MEMBER
};

// File name of an FFmpeg library on this platform: "avcodec-62.dll" on Windows,
// "libavcodec.62.dylib" on macOS.
std::string ffmpegLibraryFileName(std::string_view stem, unsigned major);

// Where defaultFfmpeg() looks: the directory of the running executable on Windows, where the build
// copies the DLLs; the lib directory of the FFmpeg the project was built against on macOS (Homebrew's
// install, loaded in place and never copied).
std::filesystem::path defaultFfmpegDirectory();

// Loads avutil, swresample, swscale and avcodec from `dir` (an absolute directory) and checks that
// their major versions equal those of the headers this project is built against. On Windows only
// `dir` and the system directory are searched, so an FFmpeg elsewhere on PATH is never picked up.
// swresample is not used directly but is a static import of avcodec, so it must be present.
// The error text names the missing files or the version mismatch.
// A successful load stays mapped for the life of the process and the FFmpeg log is routed to the
// project logger (debug and trace levels).
std::expected<FfmpegApi, std::string> loadFfmpeg(const std::filesystem::path& dir);

// Loads from defaultFfmpegDirectory() once and returns the same result afterwards.
const std::expected<FfmpegApi, std::string>& defaultFfmpeg();

}  // namespace tox2

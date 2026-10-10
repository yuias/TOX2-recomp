#include "media/ffmpeg_api.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" {
#include <libswresample/version_major.h>
}

#include "core/fatal.h"
#include "core/log.h"

namespace tox2 {

namespace {

// Everything this project loads comes from one directory, so FFmpeg dependencies of the DLLs
// (avutil, swresample) are found there too (DLL_LOAD_DIR) and a copy on PATH or in the current
// directory is never used.
constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

struct Modules {
    HMODULE avutil = nullptr;
    HMODULE avcodec = nullptr;
    HMODULE swscale = nullptr;
};

// Library order of the load; also indexes the file names and handles.
enum Library : std::size_t { kAvutil, kSwresample, kSwscale, kAvcodec, kLibraryCount };

// Frees the libraries of a failed load, last loaded first. Windows resolves an already loaded
// module by its base name, so a leaked handle would make a later load from another directory
// return the old DLLs.
struct LoadedLibraries {
    std::array<HMODULE, kLibraryCount> handles{};
    bool keep = false;

    LoadedLibraries() = default;
    LoadedLibraries(const LoadedLibraries&) = delete;
    LoadedLibraries& operator=(const LoadedLibraries&) = delete;
    ~LoadedLibraries() {
        if (keep) return;
        for (std::size_t i = handles.size(); i-- > 0;) {
            if (handles[i] != nullptr) FreeLibrary(handles[i]);
        }
    }
};

// Per-thread, so it does not change what other threads of the game see.
class ScopedErrorMode {
public:
    ScopedErrorMode() {
        if (!SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode_)) {
            active_ = false;
        }
    }
    ScopedErrorMode(const ScopedErrorMode&) = delete;
    ScopedErrorMode& operator=(const ScopedErrorMode&) = delete;
    ~ScopedErrorMode() {
        if (active_) SetThreadErrorMode(old_mode_, nullptr);
    }

private:
    DWORD old_mode_ = 0;
    bool active_ = true;
};

using FormatLineFn = decltype(&::av_log_format_line2);

// Written under the loader mutex but read by the log callback on any FFmpeg thread.
std::atomic<FormatLineFn> g_format_line{nullptr};

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
    // Warnings and errors from FFmpeg are frequent and mostly harmless for a decoder that skips bad
    // units, so even those stay at debug.
    const LogLevel mapped = level <= AV_LOG_WARNING ? LogLevel::Debug : LogLevel::Trace;
    const FormatLineFn format_line = g_format_line.load(std::memory_order_acquire);
    if (!logEnabled(mapped) || format_line == nullptr) return;

    // Local on purpose: FFmpeg logs from decoder threads and a shared prefix state would race.
    int print_prefix = 1;
    std::array<char, 1024> line;
    format_line(ptr, level, fmt, vl, line.data(), static_cast<int>(line.size()), &print_prefix);
    std::string_view text(line.data());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    if (text.empty()) return;
    logWrite(mapped, std::format("ffmpeg: {}", text));
}

std::string errorText(DWORD error) { return std::system_category().message(static_cast<int>(error)); }

std::string joinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

std::expected<FfmpegApi, std::string> loadFrom(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::path absolute_dir = std::filesystem::absolute(dir, ec);
    if (ec || absolute_dir.empty()) {
        return std::unexpected(std::format("cannot resolve the FFmpeg directory '{}'", dir.string()));
    }

    // Load order puts the dependencies first. swresample is only a static import of avcodec.
    std::array<std::string, kLibraryCount> files;
    files[kAvutil] = std::format("avutil-{}.dll", LIBAVUTIL_VERSION_MAJOR);
    files[kSwresample] = std::format("swresample-{}.dll", LIBSWRESAMPLE_VERSION_MAJOR);
    files[kSwscale] = std::format("swscale-{}.dll", LIBSWSCALE_VERSION_MAJOR);
    files[kAvcodec] = std::format("avcodec-{}.dll", LIBAVCODEC_VERSION_MAJOR);

    std::vector<std::string> missing_files;
    for (const std::string& file : files) {
        if (!std::filesystem::is_regular_file(absolute_dir / file, ec)) missing_files.push_back(file);
    }
    if (!missing_files.empty()) {
        return std::unexpected(
                std::format("missing {} in {}", joinNames(missing_files), absolute_dir.generic_string()));
    }

    LoadedLibraries loaded;
    // A corrupt or truncated DLL would otherwise raise a modal "Bad Image" dialog and block the
    // process; with the error mode set it is reported through GetLastError instead.
    const ScopedErrorMode error_mode;
    for (std::size_t i = 0; i < files.size(); ++i) {
        // The directory must be part of an absolute path for DLL_LOAD_DIR to take effect.
        loaded.handles[i] = LoadLibraryExW((absolute_dir / files[i]).c_str(), nullptr, kLoadFlags);
        if (loaded.handles[i] == nullptr) {
            // Read before anything else can overwrite it (std::format argument order is unspecified).
            const DWORD error = GetLastError();
            return std::unexpected(std::format("cannot load {} from {}: {}", files[i], absolute_dir.generic_string(),
                                               errorText(error)));
        }
    }
    const Modules modules{.avutil = loaded.handles[kAvutil],
                          .avcodec = loaded.handles[kAvcodec],
                          .swscale = loaded.handles[kSwscale]};

    FfmpegApi api;
    std::vector<std::string> missing_symbols;
#define TOX2_FFMPEG_RESOLVE(library, name)                                                                            \
    api.name = reinterpret_cast<decltype(api.name)>(reinterpret_cast<void*>(GetProcAddress(modules.library, #name))); \
    if (api.name == nullptr) missing_symbols.emplace_back(#name);
    TOX2_FFMPEG_FUNCTIONS(TOX2_FFMPEG_RESOLVE)
#undef TOX2_FFMPEG_RESOLVE
    if (!missing_symbols.empty()) {
        return std::unexpected(std::format("missing functions: {}", joinNames(missing_symbols)));
    }

    // The struct layouts in the headers must match the loaded libraries.
    struct VersionCheck {
        const char* name;
        unsigned loaded;
        unsigned expected_major;
    };
    const std::array<VersionCheck, 3> checks = {{
            {"avutil", api.avutil_version(), LIBAVUTIL_VERSION_MAJOR},
            {"avcodec", api.avcodec_version(), LIBAVCODEC_VERSION_MAJOR},
            {"swscale", api.swscale_version(), LIBSWSCALE_VERSION_MAJOR},
    }};
    for (const VersionCheck& check : checks) {
        if ((check.loaded >> 16) != check.expected_major) {
            return std::unexpected(std::format("{} major version {} loaded, {} expected", check.name,
                                               check.loaded >> 16, check.expected_major));
        }
    }

    g_format_line.store(api.av_log_format_line2, std::memory_order_release);
    api.av_log_set_callback(ffmpegLogCallback);
    // With a custom callback av_vlog calls it for every level whatever av_log_set_level says, so
    // logEnabled() in the callback is the actual filter. The level only affects FFmpeg's own
    // av_log_get_level() checks; it is raised so those do not skip messages before they get here.
    api.av_log_set_level(AV_LOG_VERBOSE);
    loaded.keep = true;

    logDebug("ffmpeg: avutil {}.{}.{}, avcodec {}.{}.{}, swscale {}.{}.{} from {}", checks[0].loaded >> 16,
             (checks[0].loaded >> 8) & 0xff, checks[0].loaded & 0xff, checks[1].loaded >> 16,
             (checks[1].loaded >> 8) & 0xff, checks[1].loaded & 0xff, checks[2].loaded >> 16,
             (checks[2].loaded >> 8) & 0xff, checks[2].loaded & 0xff, absolute_dir.generic_string());
    return api;
}

}  // namespace

std::expected<FfmpegApi, std::string> loadFfmpeg(const std::filesystem::path& dir) {
    // Serialized because the log bridge state is process-wide.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    return loadFrom(dir);
}

const std::expected<FfmpegApi, std::string>& defaultFfmpeg() {
    static const std::expected<FfmpegApi, std::string> result = [] {
        const std::filesystem::path dir = executableDirectory();
        if (dir.empty()) {
            return std::expected<FfmpegApi, std::string>(
                    std::unexpected(std::string("cannot determine the executable directory")));
        }
        return loadFfmpeg(dir);
    }();
    return result;
}

}  // namespace tox2

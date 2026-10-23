#include "media/ffmpeg_api.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <libswresample/version_major.h>
}

#include "core/fatal.h"
#include "core/log.h"

namespace tox2 {

namespace {

#if defined(_WIN32)
using LibraryHandle = HMODULE;

// Everything this project loads comes from one directory, so FFmpeg dependencies of the DLLs
// (avutil, swresample) are found there too (DLL_LOAD_DIR) and a copy on PATH or in the current
// directory is never used.
constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

std::string errorText(DWORD error) { return std::system_category().message(static_cast<int>(error)); }

// `path` must be absolute for DLL_LOAD_DIR to take effect. On failure returns null and sets `error`.
LibraryHandle openLibrary(const std::filesystem::path& path, std::string& error) {
    const LibraryHandle handle = LoadLibraryExW(path.c_str(), nullptr, kLoadFlags);
    if (handle == nullptr) error = errorText(GetLastError());
    return handle;
}

void closeLibrary(LibraryHandle handle) { FreeLibrary(handle); }

void* findSymbol(LibraryHandle handle, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

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
#else
using LibraryHandle = void*;

// RTLD_LOCAL keeps FFmpeg's symbols out of the global namespace; everything is resolved by dlsym.
// An absolute path is always passed, so dyld's search path is never consulted for the library itself.
LibraryHandle openLibrary(const std::filesystem::path& path, std::string& error) {
    void* const handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* text = dlerror();
        error = text != nullptr ? text : "unknown error";
    }
    return handle;
}

void closeLibrary(LibraryHandle handle) { dlclose(handle); }

void* findSymbol(LibraryHandle handle, const char* name) { return dlsym(handle, name); }
#endif

struct Modules {
    LibraryHandle avutil = nullptr;
    LibraryHandle avcodec = nullptr;
    LibraryHandle swscale = nullptr;
};

// Library order of the load; also indexes the file names and handles.
enum Library : std::size_t { kAvutil, kSwresample, kSwscale, kAvcodec, kLibraryCount };

// Frees the libraries of a failed load, last loaded first. Both loaders resolve an already loaded
// library by its name, so a leaked handle would make a later load from another directory return the
// old libraries.
struct LoadedLibraries {
    std::array<LibraryHandle, kLibraryCount> handles{};
    bool keep = false;

    LoadedLibraries() = default;
    LoadedLibraries(const LoadedLibraries&) = delete;
    LoadedLibraries& operator=(const LoadedLibraries&) = delete;
    ~LoadedLibraries() {
        if (keep) return;
        for (std::size_t i = handles.size(); i-- > 0;) {
            if (handles[i] != nullptr) closeLibrary(handles[i]);
        }
    }
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
    files[kAvutil] = ffmpegLibraryFileName("avutil", LIBAVUTIL_VERSION_MAJOR);
    files[kSwresample] = ffmpegLibraryFileName("swresample", LIBSWRESAMPLE_VERSION_MAJOR);
    files[kSwscale] = ffmpegLibraryFileName("swscale", LIBSWSCALE_VERSION_MAJOR);
    files[kAvcodec] = ffmpegLibraryFileName("avcodec", LIBAVCODEC_VERSION_MAJOR);

    std::vector<std::string> missing_files;
    for (const std::string& file : files) {
        if (!std::filesystem::is_regular_file(absolute_dir / file, ec)) missing_files.push_back(file);
    }
    if (!missing_files.empty()) {
        return std::unexpected(
                std::format("missing {} in {}", joinNames(missing_files), absolute_dir.generic_string()));
    }

    LoadedLibraries loaded;
#if defined(_WIN32)
    // A corrupt or truncated DLL would otherwise raise a modal "Bad Image" dialog and block the
    // process; with the error mode set it is reported through GetLastError instead.
    const ScopedErrorMode error_mode;
#endif
    for (std::size_t i = 0; i < files.size(); ++i) {
        // The error text is read inside openLibrary, before anything else can overwrite it.
        std::string error;
        loaded.handles[i] = openLibrary(absolute_dir / files[i], error);
        if (loaded.handles[i] == nullptr) {
            return std::unexpected(
                    std::format("cannot load {} from {}: {}", files[i], absolute_dir.generic_string(), error));
        }
    }
    const Modules modules{.avutil = loaded.handles[kAvutil],
                          .avcodec = loaded.handles[kAvcodec],
                          .swscale = loaded.handles[kSwscale]};

    FfmpegApi api;
    std::vector<std::string> missing_symbols;
#define TOX2_FFMPEG_RESOLVE(library, name)                                               \
    api.name = reinterpret_cast<decltype(api.name)>(findSymbol(modules.library, #name)); \
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

std::string ffmpegLibraryFileName(std::string_view stem, unsigned major) {
#if defined(_WIN32)
    return std::format("{}-{}.dll", stem, major);
#else
    return std::format("lib{}.{}.dylib", stem, major);
#endif
}

std::filesystem::path defaultFfmpegDirectory() {
#if defined(_WIN32)
    return executableDirectory();
#elif defined(TOX2_FFMPEG_LIBRARY_DIR)
    return TOX2_FFMPEG_LIBRARY_DIR;
#else
#error "TOX2_FFMPEG_LIBRARY_DIR must name the directory of the FFmpeg libraries"
#endif
}

std::expected<FfmpegApi, std::string> loadFfmpeg(const std::filesystem::path& dir) {
    // Serialized because the log bridge state is process-wide.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    return loadFrom(dir);
}

const std::expected<FfmpegApi, std::string>& defaultFfmpeg() {
    static const std::expected<FfmpegApi, std::string> result = [] {
        const std::filesystem::path dir = defaultFfmpegDirectory();
        if (dir.empty()) {
            return std::expected<FfmpegApi, std::string>(
                    std::unexpected(std::string("cannot determine the FFmpeg directory")));
        }
        return loadFfmpeg(dir);
    }();
    return result;
}

}  // namespace tox2

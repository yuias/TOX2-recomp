#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tox2 {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Returns the id of the guest thread running on the calling host thread, or nullopt on a host
// thread. Registered by the thread layer so the logger does not depend on it.
using GuestThreadIdFn = std::optional<std::uint32_t> (*)();

void setGuestThreadIdProvider(GuestThreadIdFn fn);

// The registered provider's answer for the calling thread; nullopt without a provider. Takes no lock.
std::optional<std::uint32_t> currentGuestThreadId();

// Marks the calling host thread as the host thread of the lv2 SPU thread `id`: its lines start with
// "[spu XXXXXXXX] " instead of "[host] " or "[t XXXXXXXX] ". nullopt clears the mark. Set once where the
// host thread starts, not per log call.
void setLogSpuThread(std::optional<std::uint32_t> id);

// Mirrors every line to `path` (truncated) in addition to stderr. Returns false when the file
// cannot be opened; an empty path closes the current file.
bool setLogFile(const std::string& path);

// Replaces all output with `sink`, which receives each complete line without a trailing newline.
// An empty function restores stderr and the log file. Intended for tests.
void setLogSink(std::function<void(std::string_view)> sink);

// Forgets which keys logOnce has seen. Intended for tests.
void resetLogOnce();

// Messages below `level` are dropped. The default is Info. Safe to call from any thread.
void setLogLevel(LogLevel level);

// Returns true when a message at `level` would be written. Cheap enough for hot paths.
bool logEnabled(LogLevel level);

void logWrite(LogLevel level, std::string_view message);

// Flushes every active output. Lines are already flushed as they are written; this is for paths
// that end the process.
void flushLog();

// Returns true the first time `key` is passed in.
bool firstUse(std::string_view key);

// A suppressed message is not formatted.
template <typename... Args>
void logTrace(std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(LogLevel::Trace)) return;
    logWrite(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(LogLevel::Debug)) return;
    logWrite(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(LogLevel::Info)) return;
    logWrite(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logWarn(std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(LogLevel::Warn)) return;
    logWrite(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) {
    if (!logEnabled(LogLevel::Error)) return;
    logWrite(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

// Logs only the first time `key` is seen, so hot paths can report an unimplemented call without
// flooding the output. The message is not formatted on later calls. A suppressed level does not
// use up the key.
template <typename... Args>
void logOnce(LogLevel level, std::string_view key, std::format_string<Args...> fmt, Args&&... args) {
    if (logEnabled(level) && firstUse(key)) {
        logWrite(level, std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void logOnce(std::string_view key, std::format_string<Args...> fmt, Args&&... args) {
    logOnce(LogLevel::Info, key, fmt, std::forward<Args>(args)...);
}

}  // namespace tox2

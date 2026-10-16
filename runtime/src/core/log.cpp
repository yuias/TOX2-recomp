#include "core/log.h"

#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace tox2 {
namespace {

struct LogState;
LogState& state();

thread_local std::optional<std::uint32_t> t_spu_thread_id;

// Writes the "[host] " / "[t XXXXXXXX] " / "[spu XXXXXXXX] " prefix and the level tag, so the line format does not
// depend on spdlog's own thread ids and level names.
class LinePrefixFormatter : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override;

    std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<LinePrefixFormatter>(); }
};

// Forwards each formatted line, without its trailing newline, to a std::function.
class FunctionSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit FunctionSink(std::function<void(std::string_view)> fn) : fn_(std::move(fn)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        fn_(std::string_view(formatted.data(), formatted.size()));
    }

    void flush_() override {}

private:
    std::function<void(std::string_view)> fn_;
};

std::unique_ptr<spdlog::formatter> makeFormatter(const char* eol) {
    auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::local, std::string(eol));
    formatter->add_flag<LinePrefixFormatter>('*').set_pattern("%*%v");
    return formatter;
}

struct LogState {
    LogState() {
        stderr_sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        stderr_sink->set_formatter(makeFormatter("\n"));
        logger = std::make_shared<spdlog::logger>("tox2", stderr_sink);
        logger->set_level(spdlog::level::info);
        // Each emitted line must reach the stream before a fatal path ends the process. This is
        // independent of the visible level, which drops lines before they get here.
        logger->flush_on(spdlog::level::trace);
    }

    // Replaces the logger's sinks with the ones currently selected. Caller holds `mutex`.
    void rebuildSinks() {
        auto& sinks = logger->sinks();
        sinks.clear();
        if (sink) {
            sinks.push_back(sink);
            return;
        }
        sinks.push_back(stderr_sink);
        if (file_sink) {
            sinks.push_back(file_sink);
        }
    }

    std::mutex mutex;
    // Atomic so currentGuestThreadId() can read it without the log mutex.
    std::atomic<GuestThreadIdFn> thread_id_fn{nullptr};
    std::shared_ptr<spdlog::sinks::stderr_sink_mt> stderr_sink;
    std::shared_ptr<spdlog::sinks::sink> file_sink;
    std::shared_ptr<spdlog::sinks::sink> sink;
    // Not registered with spdlog's global registry, so nothing else can reach or tear it down.
    std::shared_ptr<spdlog::logger> logger;
    std::mutex once_mutex;
    std::unordered_set<std::string> once_keys;
};

// Leaked on purpose: log calls can run during static destruction and from fatal paths that end in
// _Exit.
LogState& state() {
    static LogState* s = new LogState;
    return *s;
}

std::string_view levelTag(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace: return "trace: ";
        case spdlog::level::debug: return "debug: ";
        case spdlog::level::warn: return "warn: ";
        case spdlog::level::err: return "error: ";
        default: break;
    }
    return "";
}

spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Info: break;
    }
    return spdlog::level::info;
}

void LinePrefixFormatter::format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) {
    const std::optional<std::uint32_t> tid = currentGuestThreadId();
    if (t_spu_thread_id) {
        dest.append(std::format("[spu {:08x}] ", *t_spu_thread_id));
    } else if (tid) {
        dest.append(std::format("[t {:08x}] ", *tid));
    } else {
        dest.append("[host] ");
    }
    dest.append(levelTag(msg.level));
}

}  // namespace

void setGuestThreadIdProvider(GuestThreadIdFn fn) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.thread_id_fn.store(fn);
}

std::optional<std::uint32_t> currentGuestThreadId() {
    const GuestThreadIdFn fn = state().thread_id_fn.load();
    return fn ? fn() : std::nullopt;
}

void setLogSpuThread(std::optional<std::uint32_t> id) { t_spu_thread_id = id; }

bool setLogFile(const std::string& path) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.file_sink.reset();
    bool ok = true;
    if (!path.empty()) {
        try {
            s.file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, /*truncate=*/true);
            s.file_sink->set_formatter(makeFormatter("\n"));
        } catch (const std::exception&) {
            ok = false;
        }
    }
    s.rebuildSinks();
    return ok;
}

void setLogSink(std::function<void(std::string_view)> sink) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    if (sink) {
        s.sink = std::make_shared<FunctionSink>(std::move(sink));
        s.sink->set_formatter(makeFormatter(""));
    } else {
        s.sink.reset();
    }
    s.rebuildSinks();
}

void setLogLevel(LogLevel level) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.logger->set_level(toSpdlogLevel(level));
}

bool logEnabled(LogLevel level) {
    // spdlog keeps the level in an atomic, so this needs no lock.
    return state().logger->should_log(toSpdlogLevel(level));
}

void flushLog() {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.logger->flush();
}

void resetLogOnce() {
    LogState& s = state();
    std::lock_guard lock(s.once_mutex);
    s.once_keys.clear();
}

bool firstUse(std::string_view key) {
    LogState& s = state();
    std::lock_guard lock(s.once_mutex);
    return s.once_keys.emplace(key).second;
}

void logWrite(LogLevel level, std::string_view message) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    s.logger->log(toSpdlogLevel(level), spdlog::string_view_t(message.data(), message.size()));
}

}  // namespace tox2

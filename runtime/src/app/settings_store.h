// Saves the settings a player changes in the window to the config file.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/config_file.h"

namespace tox2 {

// set() is cheap and safe from any thread; a writer thread (started by the first set) does the file work: it
// waits until no change has come for `quiet`, or `max_delay` after the first unsaved change, re-reads the file,
// applies every pending change with updateConfigText and writes it with writeFileAtomic. A failed write leaves
// the file untouched and the changes pending; they are retried at the next set and at flush(). Each distinct
// failure is logged once per run.
class SettingsStore {
public:
    struct Timing {
        std::chrono::milliseconds quiet{500};
        std::chrono::milliseconds max_delay{2000};
    };

    // `file` need not exist. `specs` must outlive the store. Two overloads rather than a default argument:
    // Timing's member initializers are not usable as a default argument inside the class.
    SettingsStore(std::filesystem::path file, std::span<const OptionSpec> specs);
    SettingsStore(std::filesystem::path file, std::span<const OptionSpec> specs, Timing timing);
    // Flushes with a 250 ms limit, then stops and joins the writer.
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // `key` is a persisted key and `value` its command-line form; a later set of the same key replaces the
    // pending value. A non-persisted key is a programming error: logged once, ignored.
    void set(std::string_view key, std::string_view value);
    // Writes the pending changes on the calling thread, waiting at most `timeout` for the writer thread to
    // finish a write in progress. True when nothing is pending afterwards.
    bool flush(std::chrono::milliseconds timeout);

    const std::filesystem::path& file() const { return file_; }
    std::uint64_t writes() const;  // successful writes so far

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::string key;
        std::string value;
        std::uint64_t generation = 0;  // bumped by every set; tells a write that the value changed meanwhile
    };

    void writerLoop(std::stop_token stop);
    // The caller holds write_mutex_. True when nothing is pending afterwards. `deadline` bounds the wait for
    // pending_mutex_; without one the wait is unbounded.
    bool writePending(std::optional<Clock::time_point> deadline);
    void reportFailure(const std::string& reason) const;

    const std::filesystem::path file_;
    const std::span<const OptionSpec> specs_;
    const Timing timing_;

    // pending_mutex_ is held only to copy or update the pending state, never across file I/O. A timed mutex so
    // that the stop-time flush can give up.
    mutable std::timed_mutex pending_mutex_;
    std::condition_variable_any wake_;
    std::vector<Entry> pending_;  // in the order the keys were first set
    std::uint64_t last_generation_ = 0;
    Clock::time_point first_unsaved_;
    Clock::time_point last_set_;
    bool blocked_ = false;  // the last write failed and nothing was set since; wait for the next set
    bool started_ = false;

    // Serialises file writes between the writer thread and flush().
    std::timed_mutex write_mutex_;
    std::atomic<std::uint64_t> writes_{0};

    std::jthread writer_;
};

}  // namespace tox2

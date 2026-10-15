#include "app/settings_store.h"

#include <algorithm>
#include <cerrno>
#include <expected>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "core/log.h"

namespace tox2 {

namespace {

namespace stdfs = std::filesystem;

}  // namespace

SettingsStore::SettingsStore(stdfs::path file, std::span<const OptionSpec> specs)
    : SettingsStore(std::move(file), specs, Timing{}) {}

SettingsStore::SettingsStore(stdfs::path file, std::span<const OptionSpec> specs, Timing timing)
    : file_(std::move(file)), specs_(specs), timing_(timing) {}

SettingsStore::~SettingsStore() {
    flush(std::chrono::milliseconds(250));
    writer_.request_stop();
    if (writer_.joinable()) writer_.join();
}

std::uint64_t SettingsStore::writes() const { return writes_.load(); }

void SettingsStore::set(std::string_view key, std::string_view value) {
    const bool persisted = std::ranges::any_of(
            specs_, [&](const OptionSpec& spec) { return spec.persisted && configKeyFor(spec.name) == key; });
    if (!persisted) {
        logOnce(LogLevel::Warn, std::format("settings-store-key:{}", key),
                "tox2: config: \"{}\" is not a setting that is saved; ignored", key);
        return;
    }
    // A value the file cannot hold would fail every later write, so it is rejected here and an earlier valid
    // value of the same key stays pending.
    if (const auto checked = checkConfigChange({std::string(key), std::string(value)}, specs_); !checked) {
        logOnce(LogLevel::Warn, std::format("settings-store-value:{}", key),
                "tox2: config: cannot save {}: {}; ignored", key, checked.error());
        return;
    }

    std::lock_guard lock(pending_mutex_);
    const Clock::time_point now = Clock::now();
    // A retry after a failure starts a fresh debounce instead of writing at once.
    if (pending_.empty() || blocked_) first_unsaved_ = now;
    blocked_ = false;
    last_set_ = now;
    ++last_generation_;
    const auto found = std::ranges::find(pending_, key, &Entry::key);
    if (found != pending_.end()) {
        found->value = value;
        found->generation = last_generation_;
    } else {
        pending_.push_back({std::string(key), std::string(value), last_generation_});
    }
    if (!started_) {
        started_ = true;
        writer_ = std::jthread([this](std::stop_token stop) { writerLoop(stop); });
    }
    wake_.notify_one();
}

bool SettingsStore::flush(std::chrono::milliseconds timeout) {
    const Clock::time_point deadline = Clock::now() + timeout;
    std::unique_lock write_lock(write_mutex_, std::defer_lock);
    if (!write_lock.try_lock_until(deadline)) return false;
    return writePending(deadline);
}

void SettingsStore::writerLoop(std::stop_token stop) {
    std::unique_lock lock(pending_mutex_);
    while (!stop.stop_requested()) {
        // After a failure nothing is retried until the next set.
        if (pending_.empty() || blocked_) {
            wake_.wait(lock, stop, [&] { return !pending_.empty() && !blocked_; });
            continue;
        }
        const Clock::time_point deadline = std::min(last_set_ + timing_.quiet, first_unsaved_ + timing_.max_delay);
        if (Clock::now() < deadline) {
            // A later set moves the quiet deadline, so wake for it and recompute.
            const std::uint64_t seen = last_generation_;
            wake_.wait_until(lock, stop, deadline,
                             [&] { return last_generation_ != seen || pending_.empty() || blocked_; });
            continue;
        }
        lock.unlock();
        {
            std::lock_guard write_lock(write_mutex_);
            writePending(std::nullopt);
        }
        lock.lock();
    }
}

bool SettingsStore::writePending(std::optional<Clock::time_point> deadline) {
    auto lockPending = [&](std::unique_lock<std::timed_mutex>& lock) {
        if (deadline) return lock.try_lock_until(*deadline);
        lock.lock();
        return true;
    };

    std::vector<Entry> snapshot;
    std::uint64_t snapshot_generation = 0;
    {
        std::unique_lock lock(pending_mutex_, std::defer_lock);
        if (!lockPending(lock)) return false;
        snapshot = pending_;
        snapshot_generation = last_generation_;
    }
    if (snapshot.empty()) return true;

    // The file is read just before writing, so edits made by hand while the game ran are kept.
    const auto saved = [&]() -> std::expected<void, std::string> {
        std::string text;
        std::error_code ec;
        const bool exists = stdfs::exists(file_, ec);
        if (ec) return std::unexpected(std::format("cannot check {}: {}", file_.string(), ec.message()));
        if (exists) {
            std::ifstream in(file_, std::ios::binary);
            if (!in) {
                return std::unexpected(
                        std::format("cannot read {}: {}", file_.string(), std::generic_category().message(errno)));
            }
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::vector<ConfigChange> changes;
        changes.reserve(snapshot.size());
        for (const Entry& entry : snapshot) changes.push_back({entry.key, entry.value});
        const auto updated = updateConfigText(text, changes, specs_);
        if (!updated) return std::unexpected(updated.error());
        return writeFileAtomic(file_, *updated);
    }();

    std::unique_lock lock(pending_mutex_, std::defer_lock);
    const bool locked = lockPending(lock);
    if (!saved) {
        reportFailure(saved.error());
        // A set since the snapshot is a retry in itself, so only an unchanged state waits for the next one.
        if (locked) {
            if (last_generation_ == snapshot_generation) blocked_ = true;
            first_unsaved_ = Clock::now();
        }
        return false;
    }

    ++writes_;
    std::string keys;
    for (const Entry& entry : snapshot) keys += (keys.empty() ? "" : ", ") + entry.key;
    logInfo("tox2: config: saved {} to {}", keys, file_.generic_string());
    if (!locked) return false;
    // An entry set again while the file was written stays pending.
    std::erase_if(pending_, [&](const Entry& entry) {
        return std::ranges::any_of(snapshot, [&](const Entry& written) {
            return written.key == entry.key && written.generation == entry.generation;
        });
    });
    first_unsaved_ = Clock::now();
    return pending_.empty();
}

void SettingsStore::reportFailure(const std::string& reason) const {
    logOnce(LogLevel::Warn, std::format("settings-store-failure:{}", reason),
            "tox2: config: cannot save settings to {}: {}", file_.generic_string(), reason);
}

}  // namespace tox2

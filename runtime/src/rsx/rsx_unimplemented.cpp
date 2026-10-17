#include "rsx/rsx_unimplemented.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>

namespace tox2 {
namespace {

constexpr std::size_t kMaxKinds = 1024;  // bounds memory when a key embeds a varying value

struct UnimplementedState {
    std::mutex mutex;
    std::map<std::string, std::uint64_t, std::less<>> counts;
    std::uint64_t dropped = 0;
};

// Leaked on purpose, like the main census: it is read on stop paths that end in _Exit.
UnimplementedState& state() {
    static UnimplementedState* s = new UnimplementedState;
    return *s;
}

// Per thread so that a note made by another thread cannot move a caller's reading; no atomic operation needed.
thread_local std::uint64_t t_thread_notes = 0;

}  // namespace

void rsxNoteUnimplemented(std::string_view category, std::string_view key) {
    t_thread_notes++;
    std::string name;
    name.reserve(category.size() + 1 + key.size());
    name.append(category).push_back(' ');
    name.append(key);

    UnimplementedState& s = state();
    std::lock_guard lock(s.mutex);
    const auto it = s.counts.find(name);
    if (it != s.counts.end()) {
        it->second++;
    } else if (s.counts.size() < kMaxKinds) {
        s.counts.emplace(std::move(name), 1);
    } else {
        s.dropped++;
    }
}

std::vector<std::pair<std::string, std::uint64_t>> rsxUnimplementedSnapshot() {
    UnimplementedState& s = state();
    std::vector<std::pair<std::string, std::uint64_t>> out;
    {
        std::lock_guard lock(s.mutex);
        out.assign(s.counts.begin(), s.counts.end());
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) {
        return std::tie(b.second, a.first) < std::tie(a.second, b.first);
    });
    return out;
}

std::uint64_t rsxUnimplementedDropped() {
    UnimplementedState& s = state();
    std::lock_guard lock(s.mutex);
    return s.dropped;
}

std::uint64_t rsxUnimplementedThreadNotes() { return t_thread_notes; }

void rsxUnimplementedReset() {
    UnimplementedState& s = state();
    std::lock_guard lock(s.mutex);
    s.counts.clear();
    s.dropped = 0;
}

}  // namespace tox2

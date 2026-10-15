#include "core/census.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "core/fatal.h"
#include "core/log.h"

namespace tox2 {

// The count is split into one cache line per stripe and a thread always uses the same stripe: two PPU threads calling
// the same import in parallel would otherwise bounce one line between cores on every call, which is the cost of the
// whole uncontended lwmutex call (measured: a shared counter doubled the time per lock + unlock pair with two holders).
// Threads beyond the stripe count share stripes, which is still exact, only slower.
namespace {

constexpr std::size_t kCensusStripes = 8;

std::size_t censusStripe() {
    static std::atomic<std::uint32_t> next{0};
    thread_local const std::size_t stripe = next.fetch_add(1, std::memory_order_relaxed) % kCensusStripes;
    return stripe;
}

}  // namespace

// The strings are written under the census mutex before the first count; only the stripes are touched without it.
struct CensusCallStat {
    struct alignas(64) Stripe {
        std::atomic<std::uint64_t> count{0};
    };

    void add() { stripes[censusStripe()].count.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t total() const {
        std::uint64_t sum = 0;
        for (const Stripe& stripe : stripes) sum += stripe.count.load(std::memory_order_relaxed);
        return sum;
    }

    std::string name;
    std::array<Stripe, kCensusStripes> stripes;
    bool implemented = false;
    std::string note;
    std::string image;  // set for imports bound to a recompiled module's export
};

namespace {

using CallStat = CensusCallStat;

// Bumped by resetCensus() under the mutex; a slot filled in an earlier generation points at a destroyed counter.
std::atomic<std::uint32_t> g_generation{1};

struct CensusState {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, CallStat> syscalls;
    std::unordered_map<std::string, CallStat> hle;
    // Import stubs pass the same string pointers on every call, so the formatted key is built once per pair.
    // The strings are kept to compare against: a pointer can be reused for another name.
    struct HleSlot {
        std::string module, name;
        CallStat* stat = nullptr;
    };
    std::map<std::pair<const char*, const char*>, HleSlot> hle_slots;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> unknown_targets;
    ThreadCensusFn thread_fn = nullptr;
    RsxCensusFn rsx_fn = nullptr;
    RsxUnimplementedFn unimplemented_fn = nullptr;
    CensusResetFn unimplemented_reset = nullptr;
    CensusWriterFn rsx_stop_writer = nullptr;
    SailCensusFn sail_fn = nullptr;
    AudioCensusFn audio_fn = nullptr;
    GuestLockCensusFn guest_lock_fn = nullptr;
    SpuCensusFn spu_fn = nullptr;
    CensusResetFn spu_reset = nullptr;
};

// Leaked on purpose: the census is printed from stop paths that end in _Exit and may be updated
// during static destruction.
CensusState& state() {
    static CensusState* s = new CensusState;
    return *s;
}

struct Line {
    std::uint64_t count;
    std::string name;  // sort key and display name
    std::uint32_t number;
    bool implemented;
    std::string note = {};
    std::string image = {};
};

void sortLines(std::vector<Line>& lines) {
    std::ranges::sort(lines, [](const Line& a, const Line& b) {
        return std::tie(b.count, a.name, a.number) < std::tie(a.count, b.name, b.number);
    });
}

std::string_view stateText(ThreadCensusEntry::State s) {
    switch (s) {
        case ThreadCensusEntry::State::Running: return "running";
        case ThreadCensusEntry::State::Blocked: return "blocked";
        case ThreadCensusEntry::State::Exited: break;
    }
    return "exited";
}

template <typename Lines>
std::string section(std::string_view title, std::uint64_t calls, const Lines& lines) {
    std::uint64_t defaulted = 0;
    for (const Line& line : lines) {
        defaulted += line.implemented ? 0 : 1;
    }
    return std::format("census: {} {} calls, {} distinct, {} default-handled", title, calls, lines.size(), defaulted);
}

}  // namespace

CensusCallStat* censusCachedStat(const CensusSlot& slot) {
    // The acquire pairs with the release in censusCacheStat, so a matching generation implies the counter is set up.
    if (slot.generation_.load(std::memory_order_acquire) != g_generation.load(std::memory_order_relaxed))
        return nullptr;
    return slot.stat_.load(std::memory_order_relaxed);
}

void censusCacheStat(CensusSlot& slot, CensusCallStat* stat, std::uint32_t generation) {
    slot.stat_.store(stat, std::memory_order_relaxed);
    slot.generation_.store(generation, std::memory_order_release);
}

namespace {

void countSyscall(CensusSlot* slot, std::uint32_t number, const char* name, bool implemented) {
    CensusState& s = state();
    bool first = false;
    std::string shown;
    {
        std::lock_guard lock(s.mutex);
        CallStat& stat = s.syscalls[number];
        if (stat.total() == 0) {
            stat.name = name != nullptr ? name : "?";
            stat.implemented = implemented;
            first = true;
            shown = stat.name;
        }
        stat.add();
        if (slot != nullptr) censusCacheStat(*slot, &stat, g_generation.load(std::memory_order_relaxed));
    }
    if (first) {
        logInfo("tox2: first sc {} {}", number, shown);
    }
}

void countHle(CensusSlot* cache, const char* module, const char* name, bool implemented, const char* image) {
    CensusState& s = state();
    const char* module_text = module != nullptr ? module : "?";
    const char* name_text = name != nullptr ? name : "?";
    std::string first_key;
    {
        std::lock_guard lock(s.mutex);
        CensusState::HleSlot& slot = s.hle_slots[{module, name}];
        if (slot.stat == nullptr || slot.module != module_text || slot.name != name_text) {
            std::string key = std::format("{}::{}", module_text, name_text);
            slot.module = module_text;
            slot.name = name_text;
            slot.stat = &s.hle[key];
            if (slot.stat->total() == 0) {
                slot.stat->name = std::move(key);
                slot.stat->implemented = implemented;
                if (image != nullptr) slot.stat->image = image;
                first_key = slot.stat->name;
            }
        }
        slot.stat->add();
        if (cache != nullptr) censusCacheStat(*cache, slot.stat, g_generation.load(std::memory_order_relaxed));
    }
    if (!first_key.empty()) {
        logInfo("tox2: first hle {}", first_key);
    }
}

}  // namespace

void censusSyscall(std::uint32_t number, const char* name, bool implemented) {
    countSyscall(nullptr, number, name, implemented);
}

void censusSyscall(CensusSlot& slot, std::uint32_t number, const char* name, bool implemented) {
    if (CallStat* stat = censusCachedStat(slot)) {
        stat->add();
        return;
    }
    countSyscall(&slot, number, name, implemented);
}

void censusHle(const char* module, const char* name, bool implemented) {
    countHle(nullptr, module, name, implemented, nullptr);
}

void censusHle(CensusSlot& slot, const char* module, const char* name, bool implemented) {
    if (CallStat* stat = censusCachedStat(slot)) {
        stat->add();
        return;
    }
    countHle(&slot, module, name, implemented, nullptr);
}

void censusHleBound(const char* module, const char* name, const char* image) {
    countHle(nullptr, module, name, true, image != nullptr ? image : "?");
}

void censusHleNote(const char* module, const char* name, std::string note) {
    CensusState& s = state();
    const std::string key = std::format("{}::{}", module != nullptr ? module : "?", name != nullptr ? name : "?");
    std::lock_guard lock(s.mutex);
    const auto it = s.hle.find(key);
    if (it != s.hle.end()) {
        it->second.note = std::move(note);
    }
}

bool censusUnknownTarget(std::uint32_t target, std::uint32_t site) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    return s.unknown_targets[{target, site}]++ == 0;
}

void setThreadCensusProvider(ThreadCensusFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.thread_fn = fn;
}

void setRsxCensusProvider(RsxCensusFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.rsx_fn = fn;
}

RsxCensus rsxCensusSnapshot() {
    CensusState& s = state();
    RsxCensusFn fn = nullptr;
    {
        std::lock_guard lock(s.mutex);
        fn = s.rsx_fn;
    }
    return fn != nullptr ? fn() : RsxCensus{};
}

void setSailCensusProvider(SailCensusFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.sail_fn = fn;
}

void setAudioCensusProvider(AudioCensusFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.audio_fn = fn;
}

void setGuestLockCensusProvider(GuestLockCensusFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.guest_lock_fn = fn;
}

void setSpuCensusProvider(SpuCensusFn fn, CensusResetFn reset) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.spu_fn = fn;
    s.spu_reset = reset;
}

void setRsxUnimplementedProvider(RsxUnimplementedFn fn, CensusResetFn reset) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.unimplemented_fn = fn;
    s.unimplemented_reset = reset;
}

void setRsxStopWriter(CensusWriterFn fn) {
    CensusState& s = state();
    std::lock_guard lock(s.mutex);
    s.rsx_stop_writer = fn;
}

std::string formatCensus() {
    CensusState& s = state();
    std::string out;
    ThreadCensusFn thread_fn = nullptr;
    RsxCensusFn rsx_fn = nullptr;
    RsxUnimplementedFn unimplemented_fn = nullptr;
    SailCensusFn sail_fn = nullptr;
    AudioCensusFn audio_fn = nullptr;
    GuestLockCensusFn guest_lock_fn = nullptr;
    SpuCensusFn spu_fn = nullptr;
    {
        std::lock_guard lock(s.mutex);

        std::vector<Line> lines;
        std::uint64_t calls = 0;
        for (const auto& [number, stat] : s.syscalls) {
            const std::uint64_t count = stat.total();
            lines.push_back({count, stat.name, number, stat.implemented});
            calls += count;
        }
        sortLines(lines);
        out += section("syscalls", calls, lines);
        for (const Line& line : lines) {
            out += std::format("\n  sc {} {}: {} [{}]", line.number, line.name, line.count,
                               line.implemented ? "impl" : "default");
        }

        lines.clear();
        calls = 0;
        for (const auto& [key, stat] : s.hle) {
            const std::uint64_t count = stat.total();
            lines.push_back({count, stat.name, 0, stat.implemented, stat.note, stat.image});
            calls += count;
        }
        sortLines(lines);
        out += "\n" + section("hle", calls, lines);
        for (const Line& line : lines) {
            out += std::format("\n  {}: {} [{}]", line.name, line.count,
                               !line.image.empty() ? line.image : (line.implemented ? "impl" : "default"));
            if (!line.note.empty()) {
                out += " " + line.note;
            }
        }

        std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t>> targets(
                s.unknown_targets.begin(), s.unknown_targets.end());
        std::ranges::stable_sort(targets, [](const auto& a, const auto& b) { return a.second > b.second; });
        out += std::format("\ncensus: unknown targets {} distinct", targets.size());
        for (const auto& [key, count] : targets) {
            out += std::format("\n  {:#010x} from {:#010x}: {}", key.first, key.second, count);
        }
        thread_fn = s.thread_fn;
        rsx_fn = s.rsx_fn;
        unimplemented_fn = s.unimplemented_fn;
        sail_fn = s.sail_fn;
        audio_fn = s.audio_fn;
        guest_lock_fn = s.guest_lock_fn;
        spu_fn = s.spu_fn;
    }

    if (rsx_fn != nullptr) {
        RsxCensus rsx = rsx_fn();
        std::ranges::sort(rsx.methods, [](const RsxMethodCount& a, const RsxMethodCount& b) {
            return std::tie(b.count, a.method) < std::tie(a.count, b.method);
        });
        out += std::format("\ncensus: rsx {} headers, {} distinct methods, {} flips", rsx.headers, rsx.methods.size(),
                           rsx.flips);
        if (rsx.rendering) {
            out += std::format(", rendered {} draws, skipped {}", rsx.draws_rendered, rsx.draws_skipped);
        }
        if (rsx.vblank_handler_runs + rsx.flip_handler_runs + rsx.user_handler_runs != 0) {
            out += std::format("\n  interrupt handler calls: vblank {}, flip {}, user {}", rsx.vblank_handler_runs,
                               rsx.flip_handler_runs, rsx.user_handler_runs);
            if (rsx.vblank_coalesced != 0) out += std::format(", coalesced vblanks {}", rsx.vblank_coalesced);
        }
        for (const RsxMethodCount& m : rsx.methods) {
            out += std::format("\n  method {:#x}{}{}: {}", m.method, m.name.empty() ? "" : " ", m.name, m.count);
        }
    }

    if (unimplemented_fn != nullptr) {
        const RsxUnimplementedCensus unimplemented = unimplemented_fn();
        if (!unimplemented.kinds.empty() || unimplemented.dropped != 0) {
            out += std::format("\ncensus: rsx unimplemented {} kinds", unimplemented.kinds.size());
            for (const auto& [name, count] : unimplemented.kinds) {
                out += std::format("\n  {}: {}", name, count);
            }
            if (unimplemented.dropped != 0) {
                out += std::format("\n  other kinds (limit reached): {}", unimplemented.dropped);
            }
        }
    }

    if (sail_fn != nullptr) {
        const std::vector<std::string> movies = sail_fn();
        if (!movies.empty()) {
            out += std::format("\ncensus: sail {} movies", movies.size());
            for (const std::string& movie : movies) {
                out += "\n  " + movie;
            }
        }
    }

    if (audio_fn != nullptr) {
        const std::vector<std::string> audio = audio_fn();
        if (!audio.empty()) {
            out += "\ncensus: audio";
            for (const std::string& line : audio) {
                out += "\n  " + line;
            }
        }
    }

    // The provider takes its own locks, so it runs outside ours.
    std::vector<ThreadCensusEntry> threads;
    if (thread_fn != nullptr) {
        threads = thread_fn();
    }
    const auto live = std::ranges::count_if(
            threads, [](const ThreadCensusEntry& t) { return t.state != ThreadCensusEntry::State::Exited; });
    out += std::format("\ncensus: threads {} created, {} live", threads.size(), live);
    for (const ThreadCensusEntry& t : threads) {
        out += std::format("\n  {:#010x} {}: {}", t.id, t.name, stateText(t.state));
        if (t.state == ThreadCensusEntry::State::Blocked) {
            out += " in " + t.blocked_in;
        }
    }

    if (guest_lock_fn != nullptr) {
        for (const std::string& line : guest_lock_fn()) {
            out += "\n" + line;
        }
    }

    if (spu_fn != nullptr) {
        for (const std::string& line : spu_fn()) {
            out += "\n" + line;
        }
    }
    return out;
}

void printCensus() {
    const std::string text = formatCensus();
    std::string_view rest = text;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        logInfo("{}", rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    CensusWriterFn writer = nullptr;
    {
        CensusState& s = state();
        std::lock_guard lock(s.mutex);
        writer = s.rsx_stop_writer;
    }
    if (writer != nullptr) writer();
}

void installCensus() { setCensusPrinter(&printCensus); }

void resetCensus() {
    CensusState& s = state();
    CensusResetFn unimplemented_reset = nullptr;
    CensusResetFn spu_reset = nullptr;
    {
        std::lock_guard lock(s.mutex);
        // Callers must not be counting while the census is reset (tests only); the new generation makes their next
        // call look the counters up again.
        g_generation.fetch_add(1, std::memory_order_relaxed);
        s.syscalls.clear();
        s.hle_slots.clear();
        s.hle.clear();
        s.unknown_targets.clear();
        s.thread_fn = nullptr;
        s.rsx_fn = nullptr;
        s.rsx_stop_writer = nullptr;
        s.unimplemented_fn = nullptr;
        s.sail_fn = nullptr;
        s.audio_fn = nullptr;
        s.guest_lock_fn = nullptr;
        s.spu_fn = nullptr;
        unimplemented_reset = s.unimplemented_reset;
        s.unimplemented_reset = nullptr;
        spu_reset = s.spu_reset;
        s.spu_reset = nullptr;
    }
    if (unimplemented_reset != nullptr) unimplemented_reset();
    if (spu_reset != nullptr) spu_reset();
}

}  // namespace tox2

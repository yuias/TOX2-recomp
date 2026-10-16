// tox2_bench: fixed-input benchmarks in thread cycles for code that the game spends CPU in. Every case runs a
// fixed workload several times, checks its output outside the measured span and prints one summary line that the
// A/B comparison script parses:
//
//   bench <case>: N iterations (W warm-up), median C cycles (M ms), min C, max C; check ok
//
// A case may print diagnostic lines before it; they start with "bench <case>:" and do not contain "median".
//
// Usage: tox2_bench --list
//        tox2_bench <case> [case options] [--iterations N] [--warmup N]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/reservation.h"
#include "core/thread_cpu.h"
#include "spu/spu_thread.h"
#include "spu_lzma_fixture.h"
#include "tox2/spu_context.h"

namespace {

struct Options {
    std::uint32_t iterations = 0;
    std::uint32_t warmup = 0;
    std::map<std::string, std::string, std::less<>> values;  // case options by name, without the leading dashes

    std::string_view value(std::string_view name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::string_view() : std::string_view(it->second);
    }
};

bool parseCount(std::string_view option, std::string_view text, std::uint32_t min, std::uint32_t max,
                std::uint32_t* out);

struct Case {
    const char* name;
    const char* summary;
    std::uint32_t default_iterations;
    std::uint32_t default_warmup;
    std::vector<std::string_view> options;  // case options that take a value, without the leading dashes
    int (*run)(const Case& self, const Options& options);
};

// Runs `iteration` for the warm-up and the measured iterations. It stores the cycles of its measured span and returns
// false when the check of its output fails. Prints the summary line; the exit status is 0 only when every check held.
int runMeasured(const Case& self, const Options& options, const std::function<bool(std::uint64_t* cycles)>& iteration) {
    std::vector<std::uint64_t> samples;
    for (std::uint32_t i = 0; i < options.warmup + options.iterations; i++) {
        std::uint64_t cycles = 0;
        if (!iteration(&cycles)) {
            // The iteration number counts the warm-up iterations too.
            std::printf("bench %s: check FAILED iteration %u\n", self.name, i);
            return 1;
        }
        if (i >= options.warmup) samples.push_back(cycles);
    }
    const tox2::CycleStats stats = tox2::summarizeCycles(samples);
    std::printf("bench %s: %u iterations (%u warm-up), median %llu cycles (%.1f ms), min %llu, max %llu; check ok\n",
                self.name, options.iterations, options.warmup, static_cast<unsigned long long>(stats.median),
                static_cast<double>(stats.median) / tox2::hostCyclesPerMs(), static_cast<unsigned long long>(stats.min),
                static_cast<unsigned long long>(stats.max));
    return 0;
}

// The recompiled inflate of the game's LZMA task: the cycles of the runSpu calls of one inflate of the whole item,
// checked against the item's known SHA-256.
int runSpuLzma(const Case& self, const Options& options) {
    const std::string game_dir(options.value("game-dir"));
    const std::string elf(options.value("elf"));
    if (game_dir.empty() || elf.empty()) {
        std::fputs("error: spu-lzma needs --game-dir DIR and --elf EBOOT.elf\n", stderr);
        return 2;
    }
    namespace fixture = tox2::lzma_fixture;
    fixture::Fixture lzma;
    if (!lzma.init(game_dir, elf)) return 1;
    fixture::Inflate run;
    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        if (!lzma.inflate(&run)) return false;
        *cycles = run.cycles;
        return run.output.size() == lzma.itemUsize() &&
               fixture::sha256Hex(run.output.data(), run.output.size()) == fixture::kExpectedSha256;
    });
}

// Layout of the spu-dma-ls case. The synthetic image has a 32 KiB code segment and a 16 KiB data segment behind it;
// the copy sources that belong to no image sit in a separate range of the guest buffer.
constexpr std::uint32_t kDmaGuestSize = 4u << 20;
constexpr std::uint32_t kDmaCodeEa = 0x00100000;
constexpr std::uint32_t kDmaCodeLs = 0x08000;
constexpr std::uint32_t kDmaCodeSize = 0x8000;
constexpr std::uint32_t kDmaDataSegmentSize = 0x4000;
constexpr std::uint32_t kDmaSourceEa = 0x00200000;
constexpr std::uint32_t kDmaSourceSize = 0x40000;
constexpr std::uint32_t kDmaBulkLs = 0x20000;
constexpr std::uint32_t kDmaBulkSlots = 4;
constexpr std::uint32_t kDmaBulkSize = 0x4000;
constexpr std::uint32_t kDmaBulkCopies = 64;
constexpr std::uint32_t kDmaInCodeLs = 0x0a000;
constexpr std::uint32_t kDmaInCodeSize = 0x1000;
constexpr std::uint32_t kDmaInCodeCopies = 16;
constexpr std::uint32_t kDmaCodeRepeats = 16;
constexpr std::uint32_t kDmaCodeUnits = kDmaCodeSize / tox2::kSpuTagUnit;

// Never called: the image only has to be registered so that the copies tag and untag.
TOX2_SPU_FUNC(dmaStubEntry) {
    (void)ctx;
    (void)ls;
}

const Tox2SpuSegment kDmaSegments[] = {
        {kDmaCodeEa, kDmaCodeLs, kDmaCodeSize, 1},
        {kDmaCodeEa + kDmaCodeSize, kDmaCodeLs + kDmaCodeSize, kDmaDataSegmentSize, 0},
};
const Tox2SpuEntry kDmaEntries[] = {{kDmaCodeLs, 1, {0}, dmaStubEntry}};
constexpr char kDmaNoHash[] = "0000000000000000000000000000000000000000000000000000000000000000";
const Tox2SpuImageInfo kDmaImages[] = {
        {"bench_dma", kDmaCodeEa, kDmaCodeSize + kDmaDataSegmentSize, kDmaNoHash, kDmaCodeLs, 2, kDmaSegments, 1,
         kDmaEntries},
};

// Fixed-seed bytes, so the copies change bytes (and untag) the same way in every run. A zero-filled buffer would leave
// the untag path unexercised.
void fillPattern(std::uint8_t* out, std::size_t size, std::uint64_t seed) {
    std::uint64_t x = seed;
    for (std::size_t i = 0; i < size; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<std::uint8_t>(x >> 32);
    }
}

// The LS holds the image bytes and the last bulk data, and exactly the code units are tagged, all at their link
// address.
bool checkDmaLs(const tox2::SpuThread& thread, const std::vector<std::uint8_t>& guest) {
    if (std::memcmp(thread.ls() + kDmaCodeLs, guest.data() + kDmaCodeEa, kDmaCodeSize) != 0) return false;
    // Each bulk slot was written last by one of the final kDmaBulkSlots copies of the bulk loop.
    for (std::uint32_t slot = 0; slot < kDmaBulkSlots; slot++) {
        const std::uint32_t copy = kDmaBulkCopies - kDmaBulkSlots + slot;
        const std::uint32_t ea = kDmaSourceEa + kDmaBulkSize * (copy % (kDmaSourceSize / kDmaBulkSize));
        if (std::memcmp(thread.ls() + kDmaBulkLs + kDmaBulkSize * slot, guest.data() + ea, kDmaBulkSize) != 0) {
            return false;
        }
    }
    std::uint32_t tagged = 0;
    for (std::uint32_t unit = 0; unit < tox2::kSpuTagCount; unit++) {
        const std::uint32_t lsa = unit * tox2::kSpuTagUnit;
        const auto tracked = tox2::trackedImageAt(thread, lsa);
        const bool in_code = lsa >= kDmaCodeLs && lsa < kDmaCodeLs + kDmaCodeSize;
        if (tracked.has_value() != in_code) return false;
        if (!tracked) continue;
        if (tracked->image_index != 0 || tracked->segment != 0 || tracked->delta != 0) return false;
        tagged++;
    }
    return tagged == kDmaCodeUnits;
}

// The cycles of the copyToLs calls that the MFC GET makes: bulk data into untagged LS, the code segment onto itself
// (equal bytes, tags kept), and data into the middle of the tagged code (changed bytes, units untagged) each time
// followed by the code segment again (tags set again).
int runSpuDmaLs(const Case& self, const Options& options) {
    std::vector<std::uint8_t> guest(kDmaGuestSize);
    fillPattern(guest.data() + kDmaCodeEa, kDmaCodeSize + kDmaDataSegmentSize, 0x9e3779b97f4a7c15ull);
    fillPattern(guest.data() + kDmaSourceEa, kDmaSourceSize, 0xd1b54a32d192ed03ull);
    tox2::registerSpuImages(kDmaImages, 1);
    tox2::SpuThread thread(0, "bench-dma", nullptr);
    const std::uint8_t* base = guest.data();
    tox2::copyToLs(thread, base, kDmaCodeEa, kDmaCodeLs, kDmaCodeSize);

    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        const std::uint64_t start = tox2::currentThreadCycles();
        for (std::uint32_t i = 0; i < kDmaBulkCopies; i++) {
            tox2::copyToLs(thread, base, kDmaSourceEa + kDmaBulkSize * (i % (kDmaSourceSize / kDmaBulkSize)),
                           kDmaBulkLs + kDmaBulkSize * (i % kDmaBulkSlots), kDmaBulkSize);
        }
        for (std::uint32_t i = 0; i < kDmaCodeRepeats; i++) {
            tox2::copyToLs(thread, base, kDmaCodeEa, kDmaCodeLs, kDmaCodeSize);
        }
        for (std::uint32_t i = 0; i < kDmaInCodeCopies; i++) {
            tox2::copyToLs(thread, base, kDmaSourceEa + kDmaInCodeSize * i, kDmaInCodeLs, kDmaInCodeSize);
            tox2::copyToLs(thread, base, kDmaCodeEa, kDmaCodeLs, kDmaCodeSize);
        }
        *cycles = tox2::currentThreadCycles() - start;
        return checkDmaLs(thread, guest);
    });
}

// Layout of the rsv-wake case: waiter i watches the line at i * 128, so the five waiters sit on five different stripes.
constexpr std::uint32_t kWakeWaiters = 5;
constexpr std::uint32_t kWakeGuestSize = 64u << 10;
constexpr std::uint64_t kWakePaceCycles = 15000;  // the writer's pause after each acknowledged round
constexpr std::uint32_t kWakeFail = 0xFFFFFFFF;   // waiter 0 reports an unexpected line value through the ack word

// What the writer and the waiter threads of one rsv-wake iteration share.
struct WakeShared {
    tox2::ReservationTable table;
    std::uint8_t* base = nullptr;
    std::uint32_t halt = 0;  // stored atomically, read by waitStoreEpoch()
    std::atomic<std::uint32_t> ack{0};
    std::atomic<std::uint32_t> ready{0};
    std::atomic<std::uint32_t> exited{0};
    std::atomic<std::uint64_t> other_epoch_returns{0};  // StoreWait::Epoch returns of waiters 1..4
    std::array<std::uint64_t, kWakeWaiters> waiter_cycles{};
};

std::uint64_t readBe64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

// The waiter protocol of reservation.h: epoch, then lost(), then the wait. Waiter 0 acknowledges each value that the
// writer puts into its line; the others only count how often a write elsewhere ends their wait.
void wakeWaiter(WakeShared& s, std::uint32_t index) {
    const std::uint64_t start = tox2::currentThreadCycles();
    const std::uint32_t line = index * tox2::kReservationLineSize;
    tox2::Reservation res;
    std::uint8_t snapshot[tox2::kReservationLineSize];
    std::uint32_t acked = 0;
    s.ready.fetch_add(1);
    s.ready.notify_all();
    for (;;) {
        const std::uint64_t seen = s.table.storeEpoch();
        if (!res.valid || s.table.lost(s.base, res)) {
            s.table.getllar(s.base, line, snapshot, res);
            if (index == 0) {
                const std::uint64_t value = readBe64(snapshot);
                if (value != acked) {
                    if (value != acked + 1) {
                        s.ack.store(kWakeFail);
                        s.ack.notify_one();
                        break;
                    }
                    acked = static_cast<std::uint32_t>(value);
                    s.ack.store(acked);
                    s.ack.notify_one();
                }
            }
        }
        if (__atomic_load_n(&s.halt, __ATOMIC_ACQUIRE) != 0) break;
        const tox2::StoreWait reason = s.table.waitStoreEpoch(seen, &s.halt, std::chrono::milliseconds(1), line);
        if (reason == tox2::StoreWait::Halt) break;
        if (reason == tox2::StoreWait::Epoch && index != 0) s.other_epoch_returns.fetch_add(1);
    }
    s.waiter_cycles[index] = tox2::currentThreadCycles() - start;
    s.exited.fetch_add(1);
}

// Reservation wait and wake: the writer puts an increasing value into line 0 and waits for waiter 0 to see it; the
// cycles of the writer and all waiters are summed. The diagnostic line counts how often a put ends the wait of the
// four waiters that watch other lines (about 4 per round when every put wakes every waiter).
int runRsvWake(const Case& self, const Options& options) {
    std::uint32_t rounds = 20000;
    if (!options.value("rounds").empty() && !parseCount("--rounds", options.value("rounds"), 1, 10000000, &rounds)) {
        return 2;
    }
    // The table takes 128-byte lines; the vector is over-allocated to align the guest base.
    std::vector<std::uint8_t> guest(kWakeGuestSize + tox2::kReservationLineSize);
    std::uint8_t* const base = reinterpret_cast<std::uint8_t*>(
            (reinterpret_cast<std::uintptr_t>(guest.data()) + tox2::kReservationLineSize - 1) &
            ~static_cast<std::uintptr_t>(tox2::kReservationLineSize - 1));

    const std::uint32_t total_iterations = options.warmup + options.iterations;
    std::uint32_t iteration_index = 0;
    std::uint64_t epoch_returns = 0;
    std::uint64_t rounds_run = 0;
    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        std::memset(base, 0, kWakeGuestSize);
        WakeShared shared;
        shared.base = base;
        std::vector<std::thread> waiters;
        for (std::uint32_t i = 0; i < kWakeWaiters; i++) waiters.emplace_back(wakeWaiter, std::ref(shared), i);
        for (std::uint32_t seen = shared.ready.load(); seen != kWakeWaiters; seen = shared.ready.load()) {
            shared.ready.wait(seen);
        }

        bool ok = true;
        const std::uint64_t start = tox2::currentThreadCycles();
        for (std::uint32_t r = 1; r <= rounds; r++) {
            std::uint8_t value[8];
            for (int b = 0; b < 8; b++)
                value[b] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(r) >> (56 - 8 * b));
            shared.table.put(base, 0, value, 8);
            for (std::uint32_t cur = shared.ack.load(); cur != r; cur = shared.ack.load()) {
                if (cur == kWakeFail) {
                    ok = false;
                    break;
                }
                shared.ack.wait(cur);
            }
            if (!ok) break;
            // Without a pause the next put often lands before the other waiters, woken by the previous one, have read
            // the epoch again, and they sleep through it (about 2.4 returns per round instead of 4). The pause adds the
            // same cycles to every table implementation.
            for (const std::uint64_t until = tox2::currentThreadCycles() + kWakePaceCycles;
                 tox2::currentThreadCycles() < until;) {
            }
        }
        const std::uint64_t writer_cycles = tox2::currentThreadCycles() - start;

        __atomic_store_n(&shared.halt, 1u, __ATOMIC_RELEASE);
        shared.table.wake();
        for (std::thread& t : waiters) t.join();

        std::uint64_t sum = writer_cycles;
        for (const std::uint64_t c : shared.waiter_cycles) sum += c;
        *cycles = sum;
        epoch_returns += shared.other_epoch_returns.load();
        rounds_run += rounds;
        // The diagnostic goes out before the summary line, so it is printed once, with the last iteration.
        if (++iteration_index == total_iterations) {
            std::printf("bench %s: other waiters returned Epoch per round %.2f\n", self.name,
                        static_cast<double>(epoch_returns) / static_cast<double>(rounds_run));
        }
        return ok && shared.exited.load() == kWakeWaiters && readBe64(base) == rounds && shared.ack.load() == rounds;
    });
}

const std::vector<Case>& cases() {
    static const std::vector<Case> kCases = {
            {"spu-lzma",
             "recompiled LZMA inflate of a TLZC item (--game-dir DIR --elf EBOOT.elf)",
             50,
             3,
             {"game-dir", "elf"},
             runSpuLzma},
            {"spu-dma-ls",
             "DMA into the SPU local store: bulk data, equal code, data over tagged code",
             20,
             2,
             {},
             runSpuDmaLs},
            {"rsv-wake",
             "reservation wait and wake: one writer, five line-watching waiters (--rounds N)",
             5,
             1,
             {"rounds"},
             runRsvWake},
    };
    return kCases;
}

void printUsage(std::FILE* out) {
    std::fputs(
            "usage: tox2_bench --list\n"
            "       tox2_bench <case> [case options] [--iterations N] [--warmup N]\n",
            out);
}

// Parses a whole number in [min, max]; false (with a message) otherwise.
bool parseCount(std::string_view option, std::string_view text, std::uint32_t min, std::uint32_t max,
                std::uint32_t* out) {
    const std::string copy(text);
    char* end = nullptr;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
    if (copy.empty() || *end != '\0' || value < min || value > max) {
        std::fprintf(stderr, "error: %.*s takes a whole number from %u to %u\n", static_cast<int>(option.size()),
                     option.data(), min, max);
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(stderr);
        return 2;
    }
    const std::string_view first = argv[1];
    if (first == "--list") {
        for (const Case& c : cases()) std::printf("%s  %s\n", c.name, c.summary);
        return 0;
    }
    if (first == "--help" || first == "-h") {
        printUsage(stdout);
        return 0;
    }
    const Case* selected = nullptr;
    for (const Case& c : cases()) {
        if (first == c.name) selected = &c;
    }
    if (selected == nullptr) {
        std::fprintf(stderr, "error: unknown case %s (see --list)\n", argv[1]);
        return 2;
    }

    Options options;
    options.iterations = selected->default_iterations;
    options.warmup = selected->default_warmup;
    for (int i = 2; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (!arg.starts_with("--") || i + 1 >= argc) {
            std::fprintf(stderr, "error: option %s is unknown or has no value\n", argv[i]);
            return 2;
        }
        const std::string_view name = arg.substr(2);
        const std::string_view value = argv[++i];
        if (name == "iterations") {
            if (!parseCount(arg, value, 1, 100000, &options.iterations)) return 2;
        } else if (name == "warmup") {
            if (!parseCount(arg, value, 0, 100000, &options.warmup)) return 2;
        } else if (std::find(selected->options.begin(), selected->options.end(), name) != selected->options.end()) {
            options.values[std::string(name)] = std::string(value);
        } else {
            std::fprintf(stderr, "error: case %s has no option %s\n", selected->name, argv[i - 1]);
            return 2;
        }
    }
    return selected->run(*selected, options);
}

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
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/reservation.h"
#include "core/thread_cpu.h"
#include "spu/spu_capture.h"
#include "spu/spu_channels.h"
#include "spu/spu_mfc.h"
#include "spu/spu_thread.h"
#include "spu_lzma_fixture.h"
#include "tox2/spu_context.h"
#include "tox2/spu_lower_stackfree.h"

namespace {

struct Options {
    std::uint32_t iterations = 0;
    std::uint32_t warmup = 0;
    // Case options by name, without the leading dashes; every occurrence is kept, in command-line order.
    std::map<std::string, std::vector<std::string>, std::less<>> values;
    std::set<std::string, std::less<>> flags;  // case flags that were given

    // The last value of the option, or empty.
    std::string_view value(std::string_view name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::string_view() : std::string_view(it->second.back());
    }
    std::vector<std::string> all(std::string_view name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::vector<std::string>() : it->second;
    }
    bool flag(std::string_view name) const { return flags.contains(name); }
};

bool parseCount(std::string_view option, std::string_view text, std::uint32_t min, std::uint32_t max,
                std::uint32_t* out);

struct Case {
    const char* name;
    const char* summary;
    std::uint32_t default_iterations;
    std::uint32_t default_warmup;
    std::vector<std::string_view> options;  // case options that take a value, without the leading dashes
    std::vector<std::string_view> flags;    // case options without a value
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

// The table takes 128-byte lines, so a guest buffer allocated with kReservationLineSize spare bytes is aligned up.
std::uint8_t* alignToReservationLine(std::uint8_t* p) {
    return reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(p) + tox2::kReservationLineSize - 1) &
                                           ~static_cast<std::uintptr_t>(tox2::kReservationLineSize - 1));
}

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
    std::vector<std::uint8_t> guest(kWakeGuestSize + tox2::kReservationLineSize);
    std::uint8_t* const base = alignToReservationLine(guest.data());

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

// Layout of the rsv-put case. Sizes cycle through a small store, one line, and multi-line pieces; the EAs are
// multiples of 4 KiB, so every size is naturally aligned and no piece leaves the buffer.
constexpr std::uint32_t kPutGuestSize = 1u << 20;
constexpr std::uint32_t kPutPatternSize = 4096;
constexpr std::uint32_t kPutCount = 8192;
constexpr std::uint32_t kPutSizes[] = {16, 128, 1024, 4096};

// The cycles of the put calls of a fixed workload. The EAs repeat every 255 calls and the pattern depends on the call
// number modulo 3, so later calls often store bytes that are already there (the skip-equal path) and others change
// them. The guest buffer starts zeroed in every iteration.
int runRsvPut(const Case& self, const Options& options) {
    std::vector<std::uint8_t> guest(kPutGuestSize + tox2::kReservationLineSize);
    std::uint8_t* const base = alignToReservationLine(guest.data());
    std::vector<std::uint8_t> expected(kPutGuestSize);
    std::array<std::array<std::uint8_t, kPutPatternSize>, 2> patterns;
    fillPattern(patterns[0].data(), kPutPatternSize, 0x2545f4914f6cdd1dull);
    fillPattern(patterns[1].data(), kPutPatternSize, 0x9e3779b97f4a7c15ull);

    tox2::ReservationTable table;
    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        std::memset(base, 0, kPutGuestSize);
        const std::uint64_t start = tox2::currentThreadCycles();
        for (std::uint32_t k = 0; k < kPutCount; k++) {
            const std::uint32_t ea = ((k * 4096u) % (kPutGuestSize - 4096u)) & ~15u;
            table.put(base, ea, patterns[k % 3 == 2 ? 1 : 0].data(), kPutSizes[k % 4]);
        }
        *cycles = tox2::currentThreadCycles() - start;

        std::memset(expected.data(), 0, kPutGuestSize);
        for (std::uint32_t k = 0; k < kPutCount; k++) {
            const std::uint32_t ea = ((k * 4096u) % (kPutGuestSize - 4096u)) & ~15u;
            std::memcpy(expected.data() + ea, patterns[k % 3 == 2 ? 1 : 0].data(), kPutSizes[k % 4]);
        }
        return std::memcmp(base, expected.data(), kPutGuestSize) == 0;
    });
}

// Layout of the synthetic span of spu-replay. Two images: the span's code (a function at kReplayStart that works on
// the LS and returns by dispatch to the address in r0) and a second image whose code is tagged away from its link
// address, so the tag rebuild is checked with a non-zero delta and with a name other than the span's image.
constexpr std::uint32_t kReplayStart = 0x6000;
constexpr std::uint32_t kReplayEnd = 0x6010;
constexpr std::uint32_t kReplayCodeEa = 0x00100000;
constexpr std::uint32_t kReplayCodeSize = 0x100;
constexpr std::uint32_t kReplayAuxLs = 0x9000;
constexpr std::uint32_t kReplayAuxEa = 0x00180000;
constexpr std::uint32_t kReplayAuxSize = 0x80;
constexpr std::int32_t kReplayAuxDelta = 0x400;
constexpr std::uint32_t kReplayStartWord = 0xc0de0001;  // the instruction words the dispatcher compares
constexpr std::uint32_t kReplayEndWord = 0xc0de0002;
constexpr std::uint32_t kReplayAuxWord = 0xc0de0003;
constexpr std::uint32_t kReplayIn = 0x10000;
constexpr std::uint32_t kReplayOut = 0x12000;
constexpr std::uint32_t kReplayWords = 1024;

std::uint32_t replayLoadBe32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return std::byteswap(v);
}

void replayStoreBe32(std::uint8_t* p, std::uint32_t value) {
    value = std::byteswap(value);
    std::memcpy(p, &value, 4);
}

// The span: a rotate/xor chain over kReplayWords big-endian words of the LS, written to another LS range, and two
// register results (a word and a quadword), then the return through r0.
TOX2_SPU_FUNC(replaySynthStart) {
    TOX2_SPU_SF_ENTER(kReplayStart);
    std::uint32_t acc = TOX2_VR_W(ctx->gpr[3], 0);
    for (std::uint32_t i = 0; i < kReplayWords; i++) {
        const std::uint32_t w = replayLoadBe32(ls + kReplayIn + 4 * i);
        acc = std::rotl(acc, 5) ^ w;
        replayStoreBe32(ls + kReplayOut + 4 * i, acc + w);
    }
    TOX2_VR_W(ctx->gpr[4], 0) = acc;
    ctx->gpr[5] = tox2SpuLoadQ(ls, kReplayOut + 0x40);
    TOX2_SPU_SF_DISPATCH(kReplayStart + 0xc, TOX2_VR_W(ctx->gpr[0], 0));
}

// Never run: the replay ends at the dispatch to this address.
TOX2_SPU_FUNC(replaySynthStub) {
    (void)ctx;
    (void)ls;
}

// The reference model of the span, on plain vectors and without the runtime.
void replayModel(std::array<Tox2Vr, 128>& gpr, std::vector<std::uint8_t>& ls) {
    std::uint32_t acc = TOX2_VR_W(gpr[3], 0);
    for (std::uint32_t i = 0; i < kReplayWords; i++) {
        const std::uint32_t w = replayLoadBe32(ls.data() + kReplayIn + 4 * i);
        acc = (acc << 5 | acc >> 27) ^ w;
        replayStoreBe32(ls.data() + kReplayOut + 4 * i, acc + w);
    }
    TOX2_VR_W(gpr[4], 0) = acc;
    // Quadword load: the 16 LS bytes in reverse order.
    for (std::uint32_t b = 0; b < 16; b++) gpr[5].u8[b] = ls[kReplayOut + 0x40 + 15 - b];
}

const Tox2SpuSegment kReplaySegments[] = {{kReplayCodeEa, kReplayStart, kReplayCodeSize, 1}};
const Tox2SpuEntry kReplayEntries[] = {{kReplayStart, 1, {kReplayStartWord}, replaySynthStart},
                                       {kReplayEnd, 1, {kReplayEndWord}, replaySynthStub}};
const Tox2SpuSegment kReplayAuxSegments[] = {{kReplayAuxEa, kReplayAuxLs, kReplayAuxSize, 1}};
const Tox2SpuEntry kReplayAuxEntries[] = {{kReplayAuxLs, 1, {kReplayAuxWord}, replaySynthStub}};
const Tox2SpuImageInfo kReplayImages[] = {
        {"bench_replay", kReplayCodeEa, kReplayCodeSize, kDmaNoHash, kReplayStart, 1, kReplaySegments, 2,
         kReplayEntries},
        {"bench_replay_aux", kReplayAuxEa, kReplayAuxSize, kDmaNoHash, kReplayAuxLs, 1, kReplayAuxSegments, 1,
         kReplayAuxEntries},
};
constexpr std::uint32_t kReplayImageCount = 2;

// A capture of the synthetic span whose post state comes from the reference model, not from the runtime.
tox2::SpuCapture makeSyntheticCapture() {
    tox2::SpuCapture c;
    c.image = "bench_replay";
    c.sha256 = kDmaNoHash;
    c.start = kReplayStart;
    c.end = kReplayEnd;
    c.dispatches = 1;
    c.pre_ls.resize(kTox2SpuLsSize);
    fillPattern(c.pre_ls.data(), c.pre_ls.size(), 0x2545f4914f6cdd1dull);
    replayStoreBe32(c.pre_ls.data() + kReplayStart, kReplayStartWord);
    replayStoreBe32(c.pre_ls.data() + kReplayEnd, kReplayEndWord);
    fillPattern(reinterpret_cast<std::uint8_t*>(c.pre_gpr.data()), sizeof(Tox2Vr) * c.pre_gpr.size(),
                0x9e3779b97f4a7c15ull);
    TOX2_VR_W(c.pre_gpr[0], 0) = kReplayEnd;
    fillPattern(reinterpret_cast<std::uint8_t*>(&c.pre_fpscr), sizeof c.pre_fpscr, 0xd1b54a32d192ed03ull);
    c.pre_srr0 = 0x1234;
    c.pre_int_enabled = 1;

    c.post_gpr = c.pre_gpr;
    c.post_fpscr = c.pre_fpscr;
    c.post_ls = c.pre_ls;
    replayModel(c.post_gpr, c.post_ls);
    c.post_jump_target = kReplayEnd;

    c.tag_runs = {{"bench_replay", 0, 0, kReplayStart, kReplayStart + kReplayCodeSize},
                  {"bench_replay_aux", 0, kReplayAuxDelta, kReplayAuxLs + kReplayAuxDelta,
                   kReplayAuxLs + kReplayAuxDelta + kReplayAuxSize}};
    return c;
}

// One captured span with the thread that replays it. Each file has its own thread so that its image tracking is
// exactly what its tag runs say.
struct ReplayFile {
    std::string name;
    tox2::SpuCapture capture;
    tox2::lzma_fixture::Owner owner;
    std::unique_ptr<tox2::SpuThread> thread;
};

// Rebuilds the thread's image tracking from the capture's tag runs. The code bytes are in the captured LS, so they go
// through a scratch image of guest memory (zero-filled, as large as the registered code) and copyToLs, which tags
// exactly those units with that delta. Fails when the tracking does not come out as the runs say.
bool tagReplayLs(ReplayFile& f, std::vector<std::uint8_t>& scratch, std::string& error) {
    tox2::SpuThread& thread = *f.thread;
    const tox2::SpuCapture& c = f.capture;
    std::uint32_t expected = 0;
    for (const tox2::SpuCaptureTagRun& run : c.tag_runs) {
        const Tox2SpuImageInfo* image = nullptr;
        for (std::uint32_t i = 0; i < tox2::spuImageCount(); i++) {
            if (run.image == tox2::spuImage(i).name) image = &tox2::spuImage(i);
        }
        if (image == nullptr) {
            error = "tag run image " + run.image + " is not registered";
            return false;
        }
        const Tox2SpuSegment& seg = image->segments[run.segment];
        const std::uint32_t size = run.hi - run.lo;
        const std::int64_t ea = std::int64_t{seg.ea} + run.lo - seg.ls - run.delta;
        if (ea < seg.ea || ea + size > std::int64_t{seg.ea} + seg.size ||
            ea + size > static_cast<std::int64_t>(scratch.size())) {
            error = "a tag run of " + run.image + " lies outside its segment";
            return false;
        }
        std::memcpy(scratch.data() + ea, c.pre_ls.data() + run.lo, size);
        tox2::copyToLs(thread, scratch.data(), static_cast<std::uint32_t>(ea), run.lo, size);
        expected += size / tox2::kSpuTagUnit;
    }
    std::uint32_t found = 0;
    for (std::uint32_t unit = 0; unit < tox2::kSpuTagCount; unit++) {
        if (thread.recordAt(unit * tox2::kSpuTagUnit) != nullptr) found++;
    }
    bool same = found == expected;
    for (const tox2::SpuCaptureTagRun& run : c.tag_runs) {
        for (std::uint32_t lsa = run.lo; lsa < run.hi && same; lsa += tox2::kSpuTagUnit) {
            const auto tracked = tox2::trackedImageAt(thread, lsa);
            same = tracked && run.image == tracked->image->name && run.segment == tracked->segment &&
                   run.delta == tracked->delta;
        }
    }
    if (!same) error = "the image tracking differs from the tag runs of the capture";
    return same;
}

// The state the span starts from; the tags stay as tagReplayLs left them (the tagged bytes are the same).
void restoreReplayState(ReplayFile& f) {
    const tox2::SpuCapture& c = f.capture;
    Tox2SpuContext& ctx = f.thread->context();
    std::memcpy(f.thread->ls(), c.pre_ls.data(), kTox2SpuLsSize);
    std::memcpy(ctx.gpr, c.pre_gpr.data(), sizeof ctx.gpr);
    ctx.fpscr = c.pre_fpscr;
    ctx.srr0 = c.pre_srr0;
    ctx.int_enabled = static_cast<std::uint8_t>(c.pre_int_enabled);
    __atomic_store_n(&ctx.halt, 0u, __ATOMIC_RELEASE);
    ctx.exit_ls = c.end;
}

// Everything the span could have taken from outside the LS and the registers: channel reads, MFC commands, stops.
std::uint64_t replayOutsideInputs(tox2::SpuThread& thread, const tox2::lzma_fixture::Owner& owner) {
    std::uint64_t n = static_cast<std::uint64_t>(owner.stops.load());
    for (std::uint32_t ch = 0; ch < tox2::chan::kCount; ch++) n += thread.channels().readCount(ch);
    const tox2::SpuMfcCounters& m = thread.channels().mfc().counters();
    for (const std::atomic<std::uint64_t>* field : {&m.get, &m.put, &m.get_list, &m.put_list, &m.getllar, &m.putllc_ok,
                                                    &m.putllc_failed, &m.putlluc, &m.fences}) {
        n += field->load(std::memory_order_relaxed);
    }
    return n;
}

// Null when the replay ended as the game span did, else what differs.
const char* replayMismatch(ReplayFile& f, tox2::SpuLeave how) {
    const tox2::SpuCapture& c = f.capture;
    const Tox2SpuContext& ctx = f.thread->context();
    if (how != tox2::SpuLeave::Exit) return "the run did not end at a dispatch to the end address";
    if ((ctx.jump_target & (kTox2SpuLsSize - 4u)) != c.end || ctx.jump_target != c.post_jump_target) {
        return "the jump target at the end differs";
    }
    if (replayOutsideInputs(*f.thread, f.owner) != 0) return "the span read a channel, ran an MFC command or stopped";
    if (std::memcmp(ctx.gpr, c.post_gpr.data(), sizeof ctx.gpr) != 0) return "the registers differ";
    if (std::memcmp(&ctx.fpscr, &c.post_fpscr, sizeof ctx.fpscr) != 0) return "fpscr differs";
    if (std::memcmp(f.thread->ls(), c.post_ls.data(), kTox2SpuLsSize) != 0) return "the local store differs";
    return nullptr;
}

// Replays captured spans of generated SPU code through the unchanged runtime: the state is restored outside the
// measured span, then runSpu runs from the start entry to the first dispatch to the end entry, and the result is
// compared with the state the game had there. The cycles are the sum of the runSpu calls, K per file per iteration.
// --synthetic replays a span of an image of this file whose post state comes from a reference model instead.
int runSpuReplay(const Case& self, const Options& options) {
    const std::vector<std::string> paths = options.all("capture");
    const bool synthetic = options.flag("synthetic");
    if (synthetic == !paths.empty()) {
        std::fputs("error: spu-replay needs either --synthetic or one or more --capture FILE\n", stderr);
        return 2;
    }
    std::uint32_t repeat = 1;
    if (!options.value("repeat").empty() && !parseCount("--repeat", options.value("repeat"), 1, 1000000, &repeat)) {
        return 2;
    }

    const Tox2SpuImageInfo* images = synthetic ? kReplayImages : kTox2SpuImages;
    const std::uint32_t image_count = synthetic ? kReplayImageCount : kTox2SpuImageCount;
    tox2::registerSpuImages(images, image_count);

    std::vector<std::unique_ptr<ReplayFile>> files;
    std::string error;
    if (synthetic) {
        // Through the file format and back, so the reader and the validation run on the synthetic span as well.
        std::vector<std::uint8_t> bytes;
        std::optional<tox2::SpuCapture> read;
        if (tox2::serializeSpuCapture(makeSyntheticCapture(), bytes, error)) read = tox2::parseSpuCapture(bytes, error);
        if (read && !tox2::validateSpuCapture(*read, images, image_count, error)) read.reset();
        if (!read) {
            std::fprintf(stderr, "error: synthetic capture: %s\n", error.c_str());
            return 1;
        }
        files.push_back(std::make_unique<ReplayFile>());
        files.back()->name = "synthetic";
        files.back()->capture = std::move(*read);
    } else {
        for (const std::string& path : paths) {
            std::optional<tox2::SpuCapture> read = tox2::readSpuCapture(path, images, image_count, error);
            if (!read) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 1;
            }
            files.push_back(std::make_unique<ReplayFile>());
            files.back()->name = std::filesystem::path(path).filename().string();
            files.back()->capture = std::move(*read);
        }
    }

    std::uint64_t scratch_size = 0;
    for (std::uint32_t i = 0; i < image_count; i++) {
        for (std::uint32_t j = 0; j < images[i].segment_count; j++) {
            const Tox2SpuSegment& s = images[i].segments[j];
            if (s.code != 0) scratch_size = std::max<std::uint64_t>(scratch_size, std::uint64_t{s.ea} + s.size);
        }
    }
    std::vector<std::uint8_t> scratch(scratch_size);
    for (std::uint32_t i = 0; i < files.size(); i++) {
        ReplayFile& f = *files[i];
        const tox2::SpuCapture& c = f.capture;
        if (c.start % 4 != 0 || c.end % 4 != 0 || c.start >= kTox2SpuLsSize || c.end >= kTox2SpuLsSize) {
            std::fprintf(stderr, "error: %s: start or end is not a word address in the LS\n", f.name.c_str());
            return 1;
        }
        f.thread = std::make_unique<tox2::SpuThread>(0x01000200 + i, "bench-replay", &f.owner);
        if (!tagReplayLs(f, scratch, error)) {
            std::fprintf(stderr, "error: %s: %s\n", f.name.c_str(), error.c_str());
            return 1;
        }
        std::printf("bench %s: %s: image %s start %#07x end %#07x game span cycles %llu dispatches %u\n", self.name,
                    f.name.c_str(), c.image.c_str(), c.start, c.end, static_cast<unsigned long long>(c.span_cycles),
                    c.dispatches);
    }

    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        std::uint64_t sum = 0;
        for (const auto& file : files) {
            ReplayFile& f = *file;
            for (std::uint32_t k = 0; k < repeat; k++) {
                restoreReplayState(f);
                const std::uint64_t start = tox2::currentThreadCycles();
                const tox2::SpuLeave how = tox2::runSpu(*f.thread, f.capture.start);
                sum += tox2::currentThreadCycles() - start;
                if (const char* what = replayMismatch(f, how)) {
                    std::fprintf(stderr, "bench %s: %s: %s\n", self.name, f.name.c_str(), what);
                    return false;
                }
            }
        }
        *cycles = sum;
        return true;
    });
}

const std::vector<Case>& cases() {
    static const std::vector<Case> kCases = {
            {"spu-lzma",
             "recompiled LZMA inflate of a TLZC item (--game-dir DIR --elf EBOOT.elf)",
             50,
             3,
             {"game-dir", "elf"},
             {},
             runSpuLzma},
            {"spu-dma-ls",
             "DMA into the SPU local store: bulk data, equal code, data over tagged code",
             20,
             2,
             {},
             {},
             runSpuDmaLs},
            {"rsv-wake",
             "reservation wait and wake: one writer, five line-watching waiters (--rounds N)",
             5,
             1,
             {"rounds"},
             {},
             runRsvWake},
            {"rsv-put",
             "PUT into guest memory: pieces of 16 B to 4 KiB, partly equal to the stored bytes",
             20,
             2,
             {},
             {},
             runRsvPut},
            {"spu-replay",
             "replay of captured spans of generated SPU code (--capture FILE ..., --repeat K) or --synthetic",
             20,
             2,
             {"capture", "repeat"},
             {"synthetic"},
             runSpuReplay},
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
        if (arg.starts_with("--") &&
            std::find(selected->flags.begin(), selected->flags.end(), arg.substr(2)) != selected->flags.end()) {
            options.flags.emplace(arg.substr(2));
            continue;
        }
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
            options.values[std::string(name)].emplace_back(value);
        } else {
            std::fprintf(stderr, "error: case %s has no option %s\n", selected->name, argv[i - 1]);
            return 2;
        }
    }
    return selected->run(*selected, options);
}

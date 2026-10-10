// Self-checking unit tests for the SPU runtime pieces. No generated code and no guest address space is
// involved: tests address plain host buffers through the same base + ea convention.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/fatal.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/timer_resolution.h"
#include "spu/spu_channels.h"
#include "spu/spu_mfc.h"
#include "spu/spu_thread.h"
#include "tox2/spu_lower_stackfree.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Guest memory stand-in: 4 lines, 128-byte aligned like a guest line.
struct alignas(128) TestMemory {
    std::uint8_t bytes[512] = {};
    std::uint8_t* base() { return bytes; }
};

std::uint32_t loadBe32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return std::byteswap(v);
}

void storeBe32(std::uint8_t* p, std::uint32_t value) {
    value = std::byteswap(value);
    std::memcpy(p, &value, 4);
}

// A plain store as the PPU does it: no stripe, no epoch.
void plainStore8(std::uint8_t* p, std::uint8_t value) { *static_cast<volatile std::uint8_t*>(p) = value; }

std::uint64_t chunkAt(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

void testGetllarPutllc() {
    tox2::ReservationTable table;
    TestMemory mem;
    for (int i = 0; i < 128; i++) mem.bytes[128 + i] = static_cast<std::uint8_t>(i + 1);

    tox2::Reservation res;
    CHECK(!table.lost(mem.base(), res));  // no reservation, nothing to lose
    std::uint8_t ls[128];
    table.getllar(mem.base(), 128 + 17, ls, res);  // any address inside the line
    CHECK(res.valid && res.line == 128 && (res.version & 1) == 0);
    CHECK(std::memcmp(ls, mem.bytes + 128, 128) == 0);
    CHECK(!table.lost(mem.base(), res));

    storeBe32(ls + 8, 0xdeadbeefu);
    CHECK(table.putllc(mem.base(), 128, ls, res));
    CHECK(loadBe32(mem.bytes + 136) == 0xdeadbeefu);
    CHECK(mem.bytes[128] == 1 && mem.bytes[255] == 128);
    CHECK(!res.valid);

    // The reservation is gone after one PUTLLC, successful or not.
    CHECK(!table.putllc(mem.base(), 128, ls, res));

    // A PUTLLC for another line than the reserved one fails and clears the reservation.
    table.getllar(mem.base(), 128, ls, res);
    CHECK(!table.putllc(mem.base(), 256, ls, res));
    CHECK(!res.valid);
}

void testPlainWriteBeforePutllc() {
    tox2::ReservationTable table;
    TestMemory mem;
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(mem.base(), 0, ls, res);
    plainStore8(mem.bytes + 0x72, 0xff);
    CHECK(table.lost(mem.base(), res));  // content differs from the snapshot
    ls[0] = 1;
    CHECK(!table.putllc(mem.base(), 0, ls, res));
    CHECK(mem.bytes[0] == 0 && mem.bytes[0x72] == 0xff);  // nothing written, the plain store is kept
}

void testPutInvalidatesReservation() {
    tox2::ReservationTable table;
    TestMemory mem;
    tox2::Reservation res;
    std::uint8_t ls[128];
    std::uint8_t zeros[128] = {};

    // The bytes are identical, so only the version bump can fail the PUTLLC.
    table.getllar(mem.base(), 0, ls, res);
    table.put(mem.base(), 0x40, zeros, 16);
    CHECK(table.lost(mem.base(), res));
    CHECK(!table.putllc(mem.base(), 0, ls, res));

    table.getllar(mem.base(), 0, ls, res);
    table.putlluc(mem.base(), 0, zeros);
    CHECK(table.lost(mem.base(), res));
    CHECK(!table.putllc(mem.base(), 0, ls, res));

    // A PUT to another line leaves the reservation alone (the stripes differ).
    table.getllar(mem.base(), 0, ls, res);
    table.put(mem.base(), 128, zeros, 16);
    CHECK(!table.lost(mem.base(), res));
    CHECK(table.putllc(mem.base(), 0, ls, res));
}

void testPutPieces() {
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint8_t src[300];
    for (int i = 0; i < 300; i++) src[i] = static_cast<std::uint8_t>(i * 7 + 3);

    // A range over three lines is written in per-line pieces and bumps each stripe.
    const std::uint64_t epoch_before = table.storeEpoch();
    table.put(mem.base(), 64, src, 288);
    CHECK(std::memcmp(mem.bytes + 64, src, 288) == 0);
    CHECK(mem.bytes[63] == 0 && mem.bytes[352] == 0);
    CHECK(table.storeEpoch() == epoch_before + 3);

    // Small naturally aligned sizes touch exactly their bytes.
    const std::uint8_t one[1] = {0xa5};
    const std::uint8_t two[2] = {0x12, 0x34};
    const std::uint8_t four[4] = {1, 2, 3, 4};
    const std::uint8_t eight[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    table.put(mem.base(), 400, one, 1);
    table.put(mem.base(), 402, two, 2);
    table.put(mem.base(), 404, four, 4);
    table.put(mem.base(), 408, eight, 8);
    CHECK(mem.bytes[400] == 0xa5 && mem.bytes[401] == 0);
    CHECK(mem.bytes[402] == 0x12 && mem.bytes[403] == 0x34);
    CHECK(std::memcmp(mem.bytes + 404, four, 4) == 0 && std::memcmp(mem.bytes + 408, eight, 8) == 0);
    CHECK(mem.bytes[412 + 4] == 0);
}

void testStoreConditional() {
    tox2::ReservationTable table;
    TestMemory mem;
    storeBe32(mem.bytes + 4, 10);
    CHECK(table.storeConditional(mem.base(), 4, 4, 10, 11));
    CHECK(loadBe32(mem.bytes + 4) == 11);
    CHECK(!table.storeConditional(mem.base(), 4, 4, 10, 12));  // value changed since the load
    CHECK(loadBe32(mem.bytes + 4) == 11);

    const std::uint64_t big = 0x0102030405060708ull;
    const std::uint64_t swapped = std::byteswap(big);
    std::memcpy(mem.bytes + 16, &swapped, 8);
    CHECK(mem.bytes[16] == 1 && mem.bytes[23] == 8);
    CHECK(table.storeConditional(mem.base(), 16, 8, big, 0x1122334455667788ull));
    CHECK(mem.bytes[16] == 0x11 && mem.bytes[23] == 0x88);
    CHECK(!table.storeConditional(mem.base(), 16, 8, big, 1));
    CHECK(!table.storeConditional(mem.base(), 16, 2, 0, 0));  // unsupported width

    // A successful store-conditional invalidates an SPU reservation on the line; a failed one does not.
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(mem.base(), 0, ls, res);
    CHECK(!table.storeConditional(mem.base(), 4, 4, 99, 100));
    CHECK(!table.lost(mem.base(), res));
    CHECK(table.storeConditional(mem.base(), 4, 4, 11, 100));
    CHECK(table.lost(mem.base(), res));
}

void testIncrementStress() {
    constexpr int kSpuThreads = 4;
    constexpr std::uint32_t kIncrements = 100000;
    tox2::ReservationTable table;
    TestMemory mem;
    constexpr std::uint32_t kLine = 128;
    constexpr std::uint32_t kCounter = kLine;       // chunk 0 of the line
    constexpr std::uint32_t kPpuWord = kLine + 64;  // another chunk of the same line

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kSpuThreads; t++) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            tox2::Reservation res;
            alignas(16) std::uint8_t ls[128];
            for (std::uint32_t n = 0; n < kIncrements; n++) {
                do {
                    table.getllar(mem.base(), kLine, ls, res);
                    storeBe32(ls, loadBe32(ls) + 1);
                } while (!table.putllc(mem.base(), kLine, ls, res));
            }
        });
    }
    // Stands for a PPU lwarx/stwcx. loop: value-based, retried when the compare fails.
    threads.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        for (std::uint32_t n = 0; n < kIncrements; n++) {
            for (;;) {
                const std::uint32_t seen = std::byteswap(
                        __atomic_load_n(reinterpret_cast<std::uint32_t*>(mem.bytes + kPpuWord), __ATOMIC_RELAXED));
                if (table.storeConditional(mem.base(), kPpuWord, 4, seen, seen + 1)) break;
            }
        }
    });
    go = true;
    for (auto& thread : threads) thread.join();

    CHECK(loadBe32(mem.bytes + kCounter) == kSpuThreads * kIncrements);
    CHECK(loadBe32(mem.bytes + kPpuWord) == kIncrements);
}

// Plain stores that land in the PUTLLC window, driven through the test seam.
struct WindowStore {
    std::uint8_t* target;
    std::uint8_t value;
};

void windowStoreHook(void* arg) {
    auto* store = static_cast<WindowStore*>(arg);
    plainStore8(store->target, store->value);
}

void testCompareWindow() {
    tox2::ReservationTable table;
    TestMemory mem;
    tox2::Reservation res;
    alignas(16) std::uint8_t ls[128];

    // PUTLLC changes chunk 0 (bytes 0..7) only.
    WindowStore store{mem.bytes + 0x48, 0x77};  // chunk 9: unchanged by the PUTLLC
    table.setCompareHook(&windowStoreHook, &store);
    table.getllar(mem.base(), 0, ls, res);
    ls[0] = 0x55;
    CHECK(table.putllc(mem.base(), 0, ls, res));
    CHECK(mem.bytes[0] == 0x55);
    CHECK(mem.bytes[0x48] == 0x77);  // survived: the chunk was not rewritten

    // A plain store to a byte of a chunk the PUTLLC rewrites is lost: the chunk store carries the
    // snapshot value of that byte. This is the exposure the design documents.
    store = WindowStore{mem.bytes + 0x03, 0x99};  // same chunk as byte 0
    table.getllar(mem.base(), 0, ls, res);
    ls[0] = 0x56;
    CHECK(table.putllc(mem.base(), 0, ls, res));
    CHECK(mem.bytes[0] == 0x56);
    CHECK(mem.bytes[0x03] == 0);  // overwritten
    table.setCompareHook(nullptr, nullptr);
}

// Live smoke test only: it can catch a PUTLLC that rewrites an unchanged chunk 5 with a stale snapshot, but
// only when the rollback happens to be visible. The deterministic evidence is testCompareWindow().
void testUnchangedChunkSurvivesRace() {
    tox2::ReservationTable table;
    TestMemory mem;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> plain_last{0};
    std::atomic<bool> went_backwards{false};

    std::thread plain_writer([&] {
        auto* chunk = reinterpret_cast<std::uint64_t*>(mem.bytes + 40);  // chunk 5
        std::uint64_t value = 0;
        while (!done.load()) {
            value++;
            __atomic_store_n(chunk, value, __ATOMIC_RELAXED);
            if (__atomic_load_n(chunk, __ATOMIC_RELAXED) < value) went_backwards = true;
            std::this_thread::yield();
        }
        plain_last = value;
    });

    tox2::Reservation res;
    alignas(16) std::uint8_t ls[128];
    std::uint32_t successes = 0;
    for (std::uint32_t attempt = 0; attempt < 400000 && successes < 3000; attempt++) {
        table.getllar(mem.base(), 0, ls, res);
        storeBe32(ls, loadBe32(ls) + 1);
        if (table.putllc(mem.base(), 0, ls, res)) successes++;
    }
    done = true;
    plain_writer.join();

    CHECK(successes > 0);
    CHECK(loadBe32(mem.bytes) == successes);
    CHECK(!went_backwards.load());
    CHECK(chunkAt(mem.bytes + 40) == plain_last.load());
}

void testWaitForStoreEpoch() {
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint32_t halt = 0;
    tox2::Reservation res;
    std::uint8_t ls[128];
    std::uint8_t zeros[16] = {};

    // A version bump wakes the wait long before its timeout.
    table.getllar(mem.base(), 0, ls, res);
    std::uint64_t seen = table.storeEpoch();
    std::thread bumper([&] {
        std::this_thread::sleep_for(milliseconds(20));
        table.put(mem.base(), 0, zeros, 16);
    });
    auto start = Clock::now();
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(3000)) == tox2::StoreWait::Epoch);
    CHECK(Clock::now() - start < milliseconds(2000));
    bumper.join();
    CHECK(table.lost(mem.base(), res));

    // A plain write moves no epoch: the wait runs to its timeout, and the content compare finds it.
    table.getllar(mem.base(), 0, ls, res);
    seen = table.storeEpoch();
    std::thread plain([&] {
        std::this_thread::sleep_for(milliseconds(5));
        plainStore8(mem.bytes + 0x72, 0xff);
    });
    start = Clock::now();
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(60)) == tox2::StoreWait::Timeout);
    CHECK(Clock::now() - start >= milliseconds(40));
    plain.join();
    CHECK(table.lost(mem.base(), res));

    // A halt request ends the wait once the setter wakes the table.
    seen = table.storeEpoch();
    std::thread halter([&] {
        std::this_thread::sleep_for(milliseconds(20));
        __atomic_store_n(&halt, 1u, __ATOMIC_RELEASE);
        table.wake();
    });
    start = Clock::now();
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(3000)) == tox2::StoreWait::Halt);
    CHECK(Clock::now() - start < milliseconds(2000));
    halter.join();

    // An epoch that already moved, or a flag that is already set, returns without waiting.
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(3000)) == tox2::StoreWait::Halt);
    __atomic_store_n(&halt, 0u, __ATOMIC_RELEASE);
    CHECK(table.waitStoreEpoch(seen - 1, &halt, milliseconds(3000)) == tox2::StoreWait::Epoch);
}

// Ping-pong: each round the writer does exactly one put() once the waiter has read its epoch, so a wakeup
// lost anywhere between that read and the wait shows up as a Timeout.
void testWaitersAreNotLost() {
    constexpr int kRounds = 10000;
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint8_t zeros[16] = {};
    std::atomic<bool> ready{false};
    std::atomic<bool> abort{false};
    std::thread writer([&] {
        for (int i = 0; i < kRounds; i++) {
            while (!ready.load(std::memory_order_acquire)) {
                if (abort.load(std::memory_order_relaxed)) return;
            }
            ready.store(false, std::memory_order_relaxed);
            table.put(mem.base(), 0, zeros, 16);
        }
    });
    bool timed_out = false;
    for (int i = 0; i < kRounds; i++) {
        const std::uint64_t seen = table.storeEpoch();
        ready.store(true, std::memory_order_release);
        if (table.waitStoreEpoch(seen, nullptr, milliseconds(1000)) == tox2::StoreWait::Timeout) {
            timed_out = true;
            break;
        }
    }
    abort = true;
    writer.join();
    CHECK(!timed_out);
}

// A failed store-conditional locks the stripe and restores it unchanged; lost() must not report that as a
// lost reservation, on the same line or on a line that shares the stripe.
void testFailedWritersDoNotLoseReservation() {
    // Line 0 and the line kReservationStripeCount lines further up map to the same stripe.
    constexpr std::uint32_t kCollidingLine = tox2::kReservationStripeCount * tox2::kReservationLineSize;
    struct alignas(128) Block {
        std::uint8_t bytes[kCollidingLine + tox2::kReservationLineSize] = {};
    };
    auto block = std::make_unique<Block>();
    std::uint8_t* base = block->bytes;

    tox2::ReservationTable table;
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(base, 0, ls, res);

    // Deterministic part: the stripe is back to its old version after each failure.
    CHECK(!table.storeConditional(base, 4, 4, 1, 2));
    CHECK(!table.lost(base, res));
    CHECK(!table.storeConditional(base, kCollidingLine + 4, 4, 1, 2));
    CHECK(!table.lost(base, res));

    // Concurrent part: lost() may see the stripe while a failing writer holds it.
    for (const std::uint32_t target : {4u, kCollidingLine + 4}) {
        std::atomic<bool> done{false};
        std::thread failer([&] {
            for (int i = 0; i < 200000; i++) table.storeConditional(base, target, 4, 1, 2);
            done = true;
        });
        bool wrongly_lost = false;
        while (!done.load()) {
            if (table.lost(base, res)) wrongly_lost = true;
        }
        failer.join();
        CHECK(!wrongly_lost);
    }
}

// GETLLAR must never return a line that is half one PUTLLUC and half another.
void testGetllarIsNotTorn() {
    tox2::ReservationTable table;
    TestMemory mem;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        alignas(16) std::uint8_t src[128];
        for (std::uint64_t counter = 1; !done.load(std::memory_order_relaxed); counter++) {
            for (int i = 0; i < 16; i++) std::memcpy(src + 8 * i, &counter, 8);
            table.putlluc(mem.base(), 128, src);
        }
    });

    bool torn = false;
    std::uint64_t reads = 0;
    std::uint64_t last_counter = 0;
    tox2::Reservation res;
    alignas(16) std::uint8_t ls[128];
    const auto deadline = Clock::now() + milliseconds(1000);
    while (Clock::now() < deadline && !torn) {
        for (int n = 0; n < 1000; n++) {
            table.getllar(mem.base(), 128, ls, res);
            const std::uint64_t first = chunkAt(ls);
            for (int i = 1; i < 16; i++) {
                if (chunkAt(ls + 8 * i) != first) torn = true;
            }
            last_counter = first;
            reads++;
        }
    }
    done = true;
    writer.join();
    CHECK(!torn);
    CHECK(reads > 0 && last_counter > 0);  // the writer really ran during the reads
}

void testSharedTable() {
    CHECK(&tox2::reservations() == &tox2::reservations());
    TestMemory mem;
    tox2::Reservation res;
    std::uint8_t ls[128];
    tox2::reservations().getllar(mem.base(), 0, ls, res);
    CHECK(tox2::reservations().putllc(mem.base(), 0, ls, res));
}

// ---------------------------------------------------------------------------------------------
// SPU thread core: a synthetic image of hand-written functions that use the stack-free macros.
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kLsChainA = 0x3000;
constexpr std::uint32_t kLsChainB = 0x3010;
constexpr std::uint32_t kLsGoto = 0x3020;  // dispatches to word 0 of $4
constexpr std::uint32_t kLsSpin = 0x3030;  // loops until a halt is requested
constexpr std::uint32_t kLsEnd = 0x3040;   // exit_ls in the chain tests; not an entry
constexpr std::uint32_t kLsImageB = 0x4000;
constexpr std::uint32_t kHops = 10000;

constexpr std::uint32_t kEaA = 0x4000;  // code segment of image "synth" (0x100 bytes), then 0x40 bytes of data
constexpr std::uint32_t kEaB = 0x5000;  // code segment of image "synth_b" (0x20 bytes)

std::uintptr_t g_sp_first;
std::uintptr_t g_sp_last;
std::atomic<int> g_image_b_runs{0};
std::atomic<bool> g_spinning{false};

TOX2_SPU_FUNC(synthChainA) {
    TOX2_SPU_SF_ENTER(kLsChainA);
    // The address of a local shows whether the host stack grows from hop to hop.
    volatile char marker = 0;
    const std::uint32_t n = TOX2_VR_W(ctx->gpr[3], 0);
    if (n == 0) g_sp_first = reinterpret_cast<std::uintptr_t>(&marker);
    if (n == kHops - 2) g_sp_last = reinterpret_cast<std::uintptr_t>(&marker);
    TOX2_VR_W(ctx->gpr[3], 0) = n + 1;
    if (n + 1 >= kHops) {
        TOX2_SPU_SF_DISPATCH(kLsChainA + 8, kLsEnd);
    }
    TOX2_SPU_SF_DISPATCH(kLsChainA + 8, kLsChainB);
}

TOX2_SPU_FUNC(synthChainB) {
    TOX2_SPU_SF_ENTER(kLsChainB);
    TOX2_VR_W(ctx->gpr[3], 0) += 1;
    TOX2_SPU_SF_DISPATCH(kLsChainB + 8, kLsChainA);
}

TOX2_SPU_FUNC(synthGoto) {
    TOX2_SPU_SF_ENTER(kLsGoto);
    TOX2_SPU_SF_DISPATCH(kLsGoto + 4, TOX2_VR_W(ctx->gpr[4], 0));
}

TOX2_SPU_FUNC(synthSpin) {
    (void)ls;
    TOX2_SPU_SF_ENTER(kLsSpin);
    g_spinning = true;
    for (;;) {
        TOX2_SPU_SF_LOOP(kLsSpin + 4);
    }
}

TOX2_SPU_FUNC(synthImageB) {
    TOX2_SPU_SF_ENTER(kLsImageB);
    g_image_b_runs++;
    TOX2_SPU_SF_DISPATCH(kLsImageB + 4, kLsEnd);
}

const Tox2SpuSegment kSegmentsA[] = {{kEaA, 0x3000, 0x100, 1}, {kEaA + 0x100, 0x3100, 0x40, 0}};
const Tox2SpuEntry kEntriesA[] = {
        {kLsChainA, 1, {0x11111111}, synthChainA},
        {kLsChainB, 2, {0x22222222, 0x33333333}, synthChainB},
        {kLsGoto, 1, {0x55555555}, synthGoto},
        {kLsSpin, 1, {0x66666666}, synthSpin},
};
const Tox2SpuSegment kSegmentsB[] = {{kEaB, kLsImageB, 0x20, 1}};
const Tox2SpuEntry kEntriesB[] = {{kLsImageB, 1, {0x77777777}, synthImageB}};

constexpr char kNoHash[] = "0000000000000000000000000000000000000000000000000000000000000000";
const Tox2SpuImageInfo kSynthImages[] = {
        {"synth", kEaA, 0x140, kNoHash, kLsChainA, 2, kSegmentsA, 4, kEntriesA},
        {"synth_b", kEaB, 0x20, kNoHash, kLsImageB, 1, kSegmentsB, 1, kEntriesB},
};

void putBe32(std::vector<std::uint8_t>& guest, std::uint32_t ea, std::uint32_t word) {
    storeBe32(guest.data() + ea, word);
}

// Guest memory with the bytes of both synthetic images at their addresses.
std::vector<std::uint8_t> makeGuest() {
    std::vector<std::uint8_t> guest(0x10000, 0xee);
    putBe32(guest, kEaA + 0x00, 0x11111111);
    putBe32(guest, kEaA + 0x10, 0x22222222);
    putBe32(guest, kEaA + 0x14, 0x33333333);
    putBe32(guest, kEaA + 0x20, 0x55555555);
    putBe32(guest, kEaA + 0x30, 0x66666666);
    std::memset(guest.data() + kEaA + 0x100, 0xdd, 0x40);
    putBe32(guest, kEaB, 0x77777777);
    return guest;
}

void registerSynthImages() { tox2::registerSpuImages(kSynthImages, 2); }

// Loads both synthetic images the way the lv2 segment load would.
void loadSynthImages(tox2::SpuThread& thread, const std::vector<std::uint8_t>& guest) {
    tox2::copyToLs(thread, guest.data(), kEaA, 0x3000, 0x140);
    tox2::copyToLs(thread, guest.data(), kEaB, kLsImageB, 0x20);
}

bool trackedAs(const tox2::SpuThread& thread, std::uint32_t lsa, std::string_view image) {
    const auto t = tox2::trackedImageAt(thread, lsa);
    return t.has_value() && image == t->image->name;
}

bool untracked(const tox2::SpuThread& thread, std::uint32_t lsa) { return !tox2::trackedImageAt(thread, lsa); }

// A call chain of 10,000 transfers through the dispatcher ends at exit_ls with a flat host stack.
// Not inlined into main: the frame of this function is the "run loop" that the leaf's depth is measured against.
TOX2_NOINLINE void testDispatchChain() {
    registerSynthImages();
    const auto guest = makeGuest();
    auto thread = std::make_unique<tox2::SpuThread>(0x01000100, "chain", nullptr);
    loadSynthImages(*thread, guest);
    thread->context().exit_ls = kLsEnd;

    volatile char run_loop_marker = 0;
    const auto run_loop_sp = reinterpret_cast<std::uintptr_t>(&run_loop_marker);
    g_sp_first = g_sp_last = 0;
    const tox2::SpuLeave how = tox2::runSpu(*thread, kLsChainA);
    CHECK(how == tox2::SpuLeave::Exit);
    CHECK(TOX2_VR_W(thread->context().gpr[3], 0) >= kHops);
    CHECK(thread->dispatchCount() >= kHops);
    CHECK(thread->imageDispatchCount(0) == thread->dispatchCount());
    CHECK(thread->imageDispatchCount(1) == 0);
    CHECK(g_sp_first != 0 && g_sp_first == g_sp_last);
    const std::uintptr_t distance = run_loop_sp > g_sp_last ? run_loop_sp - g_sp_last : g_sp_last - run_loop_sp;
    CHECK(distance < 16 * 1024);
    CHECK(thread->context().trace_pos >= kHops);
}

// A transfer into another image's range runs that image's function, whichever image branched.
void testDispatchAcrossImages() {
    registerSynthImages();
    const auto guest = makeGuest();
    auto thread = std::make_unique<tox2::SpuThread>(0x01000101, "cross", nullptr);
    loadSynthImages(*thread, guest);
    thread->context().exit_ls = kLsEnd;
    TOX2_VR_W(thread->context().gpr[4], 0) = kLsImageB;
    g_image_b_runs = 0;
    CHECK(tox2::runSpu(*thread, kLsGoto) == tox2::SpuLeave::Exit);
    CHECK(g_image_b_runs == 1);
    CHECK(thread->imageDispatchCount(0) == 1 && thread->imageDispatchCount(1) == 1);

    // Tracking persists: a copy elsewhere changes nothing about the loaded ranges.
    tox2::copyToLs(*thread, guest.data(), 0x8000, 0x9000, 0x100);
    CHECK(trackedAs(*thread, kLsGoto, "synth") && trackedAs(*thread, kLsImageB, "synth_b"));
    CHECK(untracked(*thread, 0x9000));
    g_image_b_runs = 0;
    CHECK(tox2::runSpu(*thread, kLsGoto) == tox2::SpuLeave::Exit);
    CHECK(g_image_b_runs == 1);
}

void testTrackingIntersection() {
    registerSynthImages();
    const auto guest = makeGuest();
    tox2::SpuThread thread(7, "tags", nullptr);

    // A copy that starts before the code segment and runs through its end into the data segment: only the
    // code segment's bytes are tagged, at a non-link delta since the copy sits at LS 0x2000.
    tox2::copyToLs(thread, guest.data(), kEaA - 0x40, 0x2000, 0x180);
    CHECK(untracked(thread, 0x2000) && untracked(thread, 0x2030));
    CHECK(trackedAs(thread, 0x2040, "synth") && trackedAs(thread, 0x2130, "synth"));
    CHECK(untracked(thread, 0x2140) && untracked(thread, 0x2170));
    const auto t = tox2::trackedImageAt(thread, 0x2080);
    CHECK(t && t->segment == 0 && t->delta == 0x2040 - 0x3000);
    CHECK(std::memcmp(thread.ls() + 0x2040, guest.data() + kEaA, 0x100) == 0);

    // A copy that covers part of two units of the segment tags only the unit that lies wholly inside.
    tox2::SpuThread partial(8, "partial", nullptr);
    tox2::copyToLs(partial, guest.data(), kEaA + 8, 0x3008, 0x20);
    CHECK(untracked(partial, 0x3000));
    CHECK(trackedAs(partial, 0x3010, "synth") && tox2::trackedImageAt(partial, 0x3010)->delta == 0);
    CHECK(untracked(partial, 0x3020));

    // Pieces of one load, as the policy module issues them, give one consistent record.
    tox2::SpuThread pieces(9, "pieces", nullptr);
    tox2::copyToLs(pieces, guest.data(), kEaA, 0x3000, 0x80);
    tox2::copyToLs(pieces, guest.data(), kEaA + 0x80, 0x3080, 0xc0);
    CHECK(trackedAs(pieces, 0x3000, "synth") && trackedAs(pieces, 0x30f0, "synth"));
    CHECK(untracked(pieces, 0x3100));
    CHECK(tox2::trackedImageAt(pieces, 0x3000)->delta == 0 && tox2::trackedImageAt(pieces, 0x30f0)->delta == 0);

    // Other bytes over a tracked range untag it, rounded outward to whole units.
    tox2::copyToLs(pieces, guest.data(), 0x8000, 0x3014, 4);
    CHECK(trackedAs(pieces, 0x3000, "synth") && untracked(pieces, 0x3010) && trackedAs(pieces, 0x3020, "synth"));
    tox2::untagLs(pieces, 0x3020, 0x11);
    CHECK(untracked(pieces, 0x3020) && untracked(pieces, 0x3030) && trackedAs(pieces, 0x3040, "synth"));
    tox2::untagLs(pieces, 0x3000, 0);  // nothing
    CHECK(trackedAs(pieces, 0x3000, "synth"));
}

void testCopyWrapsAtLsEnd() {
    registerSynthImages();
    auto guest = makeGuest();
    for (int i = 0; i < 0x40; i++) guest[0x9000 + i] = static_cast<std::uint8_t>(i + 1);
    tox2::SpuThread thread(10, "wrap", nullptr);
    tox2::copyToLs(thread, guest.data(), 0x9000, kTox2SpuLsSize - 0x20, 0x40);
    CHECK(std::memcmp(thread.ls() + kTox2SpuLsSize - 0x20, guest.data() + 0x9000, 0x20) == 0);
    CHECK(std::memcmp(thread.ls(), guest.data() + 0x9020, 0x20) == 0);
}

void testFirstImageLog() {
    registerSynthImages();
    const auto guest = makeGuest();
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::SpuThread first(0x01000102, "first", nullptr);
    tox2::copyToLs(first, guest.data(), kEaA, 0x3000, 0x140);
    tox2::copyToLs(first, guest.data(), kEaA, 0x3000, 0x140);
    tox2::SpuThread second(0x01000103, "second", nullptr);
    tox2::copyToLs(second, guest.data(), kEaA, 0x3000, 0x140);
    tox2::copyToLs(second, guest.data(), kEaB, kLsImageB, 0x20);
    tox2::setLogSink({});
    CHECK(lines.size() == 2);
    CHECK(lines.size() > 0 &&
          lines[0].contains("tox2: spu: first image synth at ls 0x03000..0x03100 on thread 0x01000102"));
    CHECK(lines.size() > 1 &&
          lines[1].contains("tox2: spu: first image synth_b at ls 0x04000..0x04020 on thread 0x01000103"));
}

void testCheckImages() {
    // SHA-256 of "abc" (FIPS 180-4 appendix B.1).
    const Tox2SpuImageInfo good = {
            "abc",   0x100, 3,      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 0, 0,
            nullptr, 0,     nullptr};
    const Tox2SpuImageInfo bad = {
            "abd",   0x100, 3,      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ae", 0, 0,
            nullptr, 0,     nullptr};
    std::vector<std::uint8_t> guest(0x200, 0);
    std::memcpy(guest.data() + 0x100, "abc", 3);

    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    tox2::registerSpuImages(&good, 1);
    CHECK(tox2::checkSpuImages(guest.data()));
    CHECK(lines.size() == 1 && lines[0].contains("tox2: spu: 1 images ok"));
    lines.clear();
    tox2::registerSpuImages(&bad, 1);
    CHECK(!tox2::checkSpuImages(guest.data()));
    CHECK(lines.size() == 1 && lines[0].contains("image abd") && lines[0].contains("does not match"));
    tox2::setLogSink({});
    registerSynthImages();
}

struct RecordingOwner : tox2::SpuOwner {
    std::atomic<int> finished{0};
    std::atomic<tox2::SpuLeave> how{tox2::SpuLeave::Returned};
    void onInterruptMailbox(tox2::SpuThread&, std::uint32_t) override {}
    bool onStop(tox2::SpuThread&, std::uint32_t, std::uint32_t) override { return false; }
    void onFinished(tox2::SpuThread&, tox2::SpuLeave h) override {
        how = h;
        finished++;
    }
};

void testHostThread() {
    registerSynthImages();
    const auto guest = makeGuest();
    RecordingOwner owner;

    // To exit_ls on its own host thread.
    {
        tox2::SpuThread thread(0x01000104, "host", &owner);
        loadSynthImages(thread, guest);
        thread.context().exit_ls = kLsEnd;
        CHECK(thread.state() == tox2::SpuState::Created);
        CHECK(thread.start(kLsChainA));
        CHECK(!thread.start(kLsChainA));
        thread.join();
        CHECK(thread.state() == tox2::SpuState::Finished);
        CHECK(owner.finished == 1 && owner.how == tox2::SpuLeave::Exit);
        CHECK(tox2::currentSpuThread() == nullptr);
    }

    // A thread stuck in a loop notices a halt request from another thread.
    {
        tox2::SpuThread thread(0x01000105, "spin", &owner);
        loadSynthImages(thread, guest);
        g_spinning = false;
        CHECK(thread.start(kLsSpin));
        const auto deadline = Clock::now() + milliseconds(5000);
        while (!g_spinning && Clock::now() < deadline) std::this_thread::yield();
        CHECK(g_spinning);
        CHECK(thread.state() == tox2::SpuState::Running);
        thread.requestHalt();
        thread.join();
        CHECK(owner.finished == 2 && owner.how == tox2::SpuLeave::Halt);
        CHECK(thread.state() == tox2::SpuState::Finished);
    }

    // A halt that is already pending stops the thread before it runs anything.
    {
        tox2::SpuThread thread(0x01000106, "halted", nullptr);
        loadSynthImages(thread, guest);
        thread.requestHalt();
        CHECK(tox2::runSpu(thread, kLsChainA) == tox2::SpuLeave::Halt);
        CHECK(thread.context().trace_pos == 0);
    }

    // The destructor halts and joins a running thread.
    {
        auto thread = std::make_unique<tox2::SpuThread>(0x01000107, "dtor", nullptr);
        loadSynthImages(*thread, guest);
        g_spinning = false;
        thread->start(kLsSpin);
        const auto deadline = Clock::now() + milliseconds(5000);
        while (!g_spinning && Clock::now() < deadline) std::this_thread::yield();
        thread.reset();
        CHECK(true);
    }
}

// Child process modes: each one ends in a stop that the parent checks.
int runMfcChild(std::string_view mode);
int runChanChild(std::string_view mode);

int runStopChild(std::string_view mode) {
    if (mode.starts_with("--mfc-")) return runMfcChild(mode);
    if (mode.starts_with("--chan-")) return runChanChild(mode);
    tox2::installProcessSettings();
    registerSynthImages();
    const auto guest = makeGuest();
    tox2::SpuThread thread(0x01000108, "child", nullptr);
    loadSynthImages(thread, guest);
    thread.context().exit_ls = kLsEnd;
    std::uint32_t target = 0;
    if (mode == "--spu-untracked-child") {
        target = 0x6000;
    } else if (mode == "--spu-non-entry-child") {
        target = kLsChainA + 4;
    } else if (mode == "--spu-non-link-child") {
        tox2::copyToLs(thread, guest.data(), kEaA, 0x5000, 0x100);
        target = 0x5000;
    } else if (mode == "--spu-mismatch-child") {
        storeBe32(thread.ls() + kLsChainB + 4, 0x33333334);
        target = kLsChainB;
    } else if (mode == "--spu-halt-child") {
        tox2SpuHalt(&thread.context(), thread.ls(), 0x3004);
    } else if (mode == "--spu-unhandled-child") {
        tox2SpuUnhandled(&thread.context(), thread.ls(), 0x3008, 0xdeadbeef);
    } else {
        return 99;
    }
    TOX2_VR_W(thread.context().gpr[4], 0) = target;
    tox2::runSpu(thread, kLsGoto);
    return 0;  // not reached: the stop ends the process
}

struct ChildResult {
    DWORD exit_code = 0xFFFFFFFF;
    std::string output;
};

// Runs this executable again with `arg`, capturing stdout and stderr together.
ChildResult runChild(const wchar_t* arg) {
    ChildResult result;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = std::wstring(L"\"") + exe + L"\" " + arg;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CreatePipe(&read_end, &write_end, &sa, 0);
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return result;
    }
    CloseHandle(write_end);

    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got != 0) {
        result.output.append(buffer, got);
    }
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_end);
    return result;
}

void testStopChildren() {
    struct Case {
        const wchar_t* arg;
        const char* line;
    };
    const Case cases[] = {
            {L"--spu-untracked-child",
             "tox2: stop: spu: unknown target ls 0x06000 from ls 0x03024 on thread 0x01000108 (untracked)"},
            {L"--spu-non-entry-child",
             "tox2: stop: spu: unknown target ls 0x03004 from ls 0x03024 on thread 0x01000108 (synth)"},
            {L"--spu-non-link-child", "loaded at a non-link address"},
            {L"--spu-mismatch-child", "tox2: stop: spu: code mismatch at ls 0x03010 (synth) on thread 0x01000108"},
            {L"--spu-halt-child", "tox2: stop: spu: halt at ls 0x03004 on thread 0x01000108"},
            {L"--spu-unhandled-child", "tox2: stop: spu: unhandled word 0xdeadbeef at ls 0x03008 on thread 0x01000108"},
    };
    for (const Case& c : cases) {
        const ChildResult r = runChild(c.arg);
        const bool found = r.output.find(c.line) != std::string::npos;
        // The dump follows the stop line.
        const bool dump = r.output.find("trace (oldest first)") != std::string::npos &&
                          r.output.find("ls map:") != std::string::npos;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(found);
        CHECK(dump);
        if (!found || !dump) std::fprintf(stderr, "child output:\n%s\n", r.output.c_str());
    }
    // The tracked ranges appear in the dump of the non-link case.
    const ChildResult r = runChild(L"--spu-non-link-child");
    CHECK(r.output.find("ls 0x05000..0x05100 synth segment 0 delta 8192") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// MFC: DMA, atomics, tag status. Guest memory is a plain host buffer.
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kMfcSite = 0x3000;
constexpr std::uint32_t kPatternEa = 0x8000;  // 0x4000 bytes of pattern
constexpr std::uint32_t kScratchEa = 0xc000;  // 0x4000 bytes, zero

struct MfcFixture {
    std::vector<std::uint8_t> guest = makeGuest();
    tox2::SpuThread thread{0x01000200, "mfc", nullptr};
    tox2::SpuMfc mfc{thread, guest.data()};

    MfcFixture() {
        registerSynthImages();
        for (std::uint32_t i = 0; i < 0x4000; i++) {
            guest[kPatternEa + i] = static_cast<std::uint8_t>(i * 7 + (i >> 8) + 3);
            guest[kScratchEa + i] = 0;
        }
    }

    void run(std::uint32_t cmd, std::uint32_t lsa, std::uint32_t ea, std::uint32_t size, std::uint32_t tag = 0) {
        mfc.writeLsa(lsa);
        mfc.writeEal(ea);
        mfc.writeSize(size);
        mfc.writeTagId(tag);
        mfc.command(kMfcSite, cmd);
    }
};

bool same(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t n) { return std::memcmp(a, b, n) == 0; }

void testMfcGetPut() {
    for (const std::uint32_t size : {1u, 2u, 4u, 8u, 16u, 0x4000u}) {
        MfcFixture f;
        // Small transfers keep the low four bits of both addresses equal.
        const std::uint32_t low = size < 16 ? size : 0;
        const std::uint32_t ea = kPatternEa + low;
        const std::uint32_t lsa = 0x2000 + low;
        std::memset(f.thread.ls() + 0x2000, 0x55, 0x4010);
        f.run(tox2::mfc::kGet, lsa, ea, size);
        CHECK(same(f.thread.ls() + lsa, f.guest.data() + ea, size));
        if (size < 16) {
            CHECK(f.thread.ls()[lsa - 1] == 0x55 && f.thread.ls()[lsa + size] == 0x55);  // neighbours untouched
        }

        f.run(tox2::mfc::kPut, lsa, kScratchEa + low, size, 3);
        CHECK(same(f.guest.data() + kScratchEa + low, f.guest.data() + ea, size));
        if (size < 16) {
            CHECK(f.guest[kScratchEa + low - 1] == 0 && f.guest[kScratchEa + low + size] == 0);
        }
    }

    // GETB behaves as GET, and the fence and barrier forms run in order.
    MfcFixture f;
    f.run(tox2::mfc::kGetB, 0x2000, kPatternEa, 0x80);
    CHECK(same(f.thread.ls() + 0x2000, f.guest.data() + kPatternEa, 0x80));
    f.run(tox2::mfc::kGetF, 0x2100, kPatternEa + 0x80, 0x10);
    CHECK(same(f.thread.ls() + 0x2100, f.guest.data() + kPatternEa + 0x80, 0x10));
    f.run(tox2::mfc::kPutB, 0x2000, kScratchEa, 0x80);
    f.run(tox2::mfc::kPutF, 0x2100, kScratchEa + 0x80, 0x10);
    CHECK(same(f.guest.data() + kScratchEa, f.guest.data() + kPatternEa, 0x90));
    f.mfc.command(kMfcSite, tox2::mfc::kBarrier);
    f.mfc.command(kMfcSite, tox2::mfc::kMfcSync);
    f.mfc.command(kMfcSite, tox2::mfc::kMfcEieio);
    CHECK(f.mfc.counters().fences == 3);
    CHECK(f.mfc.counters().get == 2 && f.mfc.counters().put == 2);
    CHECK(f.mfc.commandSlots() == 16);
}

void testMfcLsWrap() {
    MfcFixture f;
    // A GET that runs off the end of the LS continues at LS 0.
    f.run(tox2::mfc::kGet, kTox2SpuLsSize - 0x20, kPatternEa, 0x50);
    CHECK(same(f.thread.ls() + kTox2SpuLsSize - 0x20, f.guest.data() + kPatternEa, 0x20));
    CHECK(same(f.thread.ls(), f.guest.data() + kPatternEa + 0x20, 0x30));
    // The same for a PUT, which splits into two reservation-table writes.
    f.run(tox2::mfc::kPut, kTox2SpuLsSize - 0x20, kScratchEa, 0x50);
    CHECK(same(f.guest.data() + kScratchEa, f.guest.data() + kPatternEa, 0x50));
    // The LS address of the command itself wraps as well.
    f.run(tox2::mfc::kGet, kTox2SpuLsSize + 0x40, kPatternEa + 0x100, 0x10);
    CHECK(same(f.thread.ls() + 0x40, f.guest.data() + kPatternEa + 0x100, 0x10));
}

void testMfcImageTracking() {
    MfcFixture f;
    // Image bytes tag exactly the code segment; the data segment behind it stays untracked.
    f.run(tox2::mfc::kGet, 0x3000, kEaA, 0x140);
    CHECK(trackedAs(f.thread, 0x3000, "synth") && trackedAs(f.thread, 0x30f0, "synth"));
    CHECK(untracked(f.thread, 0x3100) && untracked(f.thread, 0x3130));
    // Other bytes over part of it untag that part, rounded outward to units.
    f.run(tox2::mfc::kGet, 0x3020, kPatternEa, 0x20);
    CHECK(trackedAs(f.thread, 0x3010, "synth") && untracked(f.thread, 0x3020) && untracked(f.thread, 0x3030) &&
          trackedAs(f.thread, 0x3040, "synth"));
    // A GET of image bytes at another LS address records the delta, which dispatch would refuse.
    f.run(tox2::mfc::kGet, 0x5000, kEaA, 0x100);
    CHECK(trackedAs(f.thread, 0x5000, "synth") && tox2::trackedImageAt(f.thread, 0x5000)->delta == 0x2000);
    // GETLLAR over tracked bytes untags them; a PUT leaves the tags alone.
    f.run(tox2::mfc::kGet, 0x3000, kEaA, 0x100);
    f.run(tox2::mfc::kGetLLAR, 0x3080, kPatternEa, 128);
    CHECK(trackedAs(f.thread, 0x3070, "synth") && untracked(f.thread, 0x3080) && untracked(f.thread, 0x30f0));
    f.run(tox2::mfc::kPut, 0x3000, kScratchEa, 0x70);
    CHECK(trackedAs(f.thread, 0x3000, "synth") && trackedAs(f.thread, 0x3060, "synth"));
}

void testMfcAtomics() {
    MfcFixture f;
    std::uint32_t status = 99;
    CHECK(!f.mfc.atomicStatusAvailable() && !f.mfc.takeAtomicStatus(&status));

    // The EA and LS address are rounded down to the line.
    f.run(tox2::mfc::kGetLLAR, 0x1010, kPatternEa + 0x180 + 0x23, 128);
    CHECK(f.mfc.hasReservation());
    CHECK(same(f.thread.ls() + 0x1000, f.guest.data() + kPatternEa + 0x180, 128));
    CHECK(f.mfc.atomicStatusAvailable() && f.mfc.takeAtomicStatus(&status) && status == 4);
    CHECK(!f.mfc.atomicStatusAvailable() && !f.mfc.takeAtomicStatus(&status));
    CHECK(f.mfc.reservation().line == kPatternEa + 0x180 && !f.mfc.reservationLost());

    // PUTLLC with an intact reservation succeeds (status 0) and clears it.
    f.thread.ls()[0x1005] ^= 0xff;
    f.run(tox2::mfc::kPutLLC, 0x1000, kPatternEa + 0x180, 128);
    CHECK(f.mfc.takeAtomicStatus(&status) && status == 0);
    CHECK(f.guest[kPatternEa + 0x185] == f.thread.ls()[0x1005] && !f.mfc.hasReservation());

    // A second PUTLLC has no reservation: status 1, nothing written.
    const std::uint8_t before = f.guest[kPatternEa + 0x185];
    f.thread.ls()[0x1005] ^= 0xff;
    f.run(tox2::mfc::kPutLLC, 0x1000, kPatternEa + 0x180, 128);
    CHECK(f.mfc.takeAtomicStatus(&status) && status == 1);
    CHECK(f.guest[kPatternEa + 0x185] == before);

    // Another writer between GETLLAR and PUTLLC: the loss is visible and the PUTLLC fails.
    f.run(tox2::mfc::kGetLLAR, 0x1000, kPatternEa + 0x180, 128);
    f.mfc.takeAtomicStatus(&status);
    const std::uint8_t other[16] = {1, 2, 3};
    tox2::reservations().put(f.guest.data(), kPatternEa + 0x1c0, other, 16);
    CHECK(f.mfc.reservationLost());
    f.run(tox2::mfc::kPutLLC, 0x1000, kPatternEa + 0x180, 128);
    CHECK(f.mfc.takeAtomicStatus(&status) && status == 1);
    CHECK(!f.mfc.hasReservation() && !f.mfc.reservationLost());
    CHECK(f.guest[kPatternEa + 0x1c0] == 1);  // the other writer's bytes survive

    // A plain store (no stripe bump) is seen through the content compare.
    f.run(tox2::mfc::kGetLLAR, 0x1000, kPatternEa + 0x180, 128);
    f.mfc.takeAtomicStatus(&status);
    plainStore8(f.guest.data() + kPatternEa + 0x1a0, 0x77);
    CHECK(f.mfc.reservationLost());
    f.run(tox2::mfc::kPutLLC, 0x1000, kPatternEa + 0x180, 128);
    CHECK(f.mfc.takeAtomicStatus(&status) && status == 1);

    // A PUT from this very SPU to the reserved line also takes the reservation.
    f.run(tox2::mfc::kGetLLAR, 0x1000, kPatternEa + 0x180, 128);
    f.mfc.takeAtomicStatus(&status);
    f.run(tox2::mfc::kPut, 0x1000, kPatternEa + 0x190, 16);
    CHECK(f.mfc.reservationLost());

    // PUTLLUC writes unconditionally, status 2; the queued form reports no status.
    std::memset(f.thread.ls() + 0x1000, 0x42, 128);
    f.run(tox2::mfc::kPutLLUC, 0x1000, kScratchEa + 0x100, 128);
    CHECK(f.mfc.takeAtomicStatus(&status) && status == 2);
    CHECK(same(f.thread.ls() + 0x1000, f.guest.data() + kScratchEa + 0x100, 128));
    std::memset(f.thread.ls() + 0x1000, 0x43, 128);
    f.run(tox2::mfc::kPutQLLUC, 0x1000, kScratchEa + 0x180, 128);
    CHECK(!f.mfc.atomicStatusAvailable());
    CHECK(same(f.thread.ls() + 0x1000, f.guest.data() + kScratchEa + 0x180, 128));

    CHECK(f.mfc.counters().getllar == 4 && f.mfc.counters().putllc_ok == 1 && f.mfc.counters().putllc_failed == 3);
    CHECK(f.mfc.counters().putlluc == 2);
}

void testMfcTagStatus() {
    MfcFixture f;
    std::uint32_t status = 99;
    CHECK(!f.mfc.tagStatusAvailable() && !f.mfc.takeTagStatus(&status));
    CHECK(f.mfc.tagMask() == 0);
    for (const std::uint32_t mode : {0u, 1u, 2u}) {
        f.mfc.writeTagMask(0x80000005u);
        CHECK(f.mfc.tagMask() == 0x80000005u);
        f.run(tox2::mfc::kGet, 0x2000, kPatternEa, 16, 2);
        f.mfc.writeTagUpdate(kMfcSite, mode);
        CHECK(f.mfc.tagStatusAvailable());
        // The status is the mask at the time of the request.
        f.mfc.writeTagMask(0);
        CHECK(f.mfc.takeTagStatus(&status) && status == 0x80000005u);
        CHECK(!f.mfc.tagStatusAvailable() && !f.mfc.takeTagStatus(&status));
    }
    f.mfc.writeTagMask(0);
    f.mfc.writeTagUpdate(kMfcSite, 2);
    CHECK(f.mfc.takeTagStatus(&status) && status == 0);
}

void putBe32Ls(tox2::SpuThread& thread, std::uint32_t addr, std::uint32_t word) { storeBe32(thread.ls() + addr, word); }

void testMfcLists() {
    MfcFixture f;
    // Three elements: 0x20 bytes, a 4-byte element whose effective address ends in 4, 0x30 bytes.
    putBe32Ls(f.thread, 0x100, 0x20);
    putBe32Ls(f.thread, 0x104, kPatternEa);
    putBe32Ls(f.thread, 0x108, 4);
    putBe32Ls(f.thread, 0x10c, kPatternEa + 0x104);
    putBe32Ls(f.thread, 0x110, 0x30);
    putBe32Ls(f.thread, 0x114, kPatternEa + 0x200);
    f.run(tox2::mfc::kGetL, 0x2000, 0x100, 24);
    CHECK(same(f.thread.ls() + 0x2000, f.guest.data() + kPatternEa, 0x20));
    CHECK(same(f.thread.ls() + 0x2024, f.guest.data() + kPatternEa + 0x104, 4));
    CHECK(same(f.thread.ls() + 0x2030, f.guest.data() + kPatternEa + 0x200, 0x30));
    CHECK(f.mfc.counters().get_list == 1);

    // A list PUT with an empty element in the middle.
    putBe32Ls(f.thread, 0x120, 0x20);
    putBe32Ls(f.thread, 0x124, kScratchEa);
    putBe32Ls(f.thread, 0x128, 0);
    putBe32Ls(f.thread, 0x12c, kScratchEa + 0x300);
    putBe32Ls(f.thread, 0x130, 0x30);
    putBe32Ls(f.thread, 0x134, kScratchEa + 0x200);
    f.run(tox2::mfc::kPutL, 0x2000, 0x120, 24);
    CHECK(same(f.guest.data() + kScratchEa, f.thread.ls() + 0x2000, 0x20));
    CHECK(same(f.guest.data() + kScratchEa + 0x200, f.thread.ls() + 0x2020, 0x30));
    CHECK(f.guest[kScratchEa + 0x300] == 0);
    CHECK(f.mfc.counters().put_list == 1);

    // A list whose elements are image bytes tags like a plain GET.
    putBe32Ls(f.thread, 0x140, 0x100);
    putBe32Ls(f.thread, 0x144, kEaA);
    f.run(tox2::mfc::kGetL, 0x3000, 0x140, 8);
    CHECK(trackedAs(f.thread, 0x3000, "synth") && trackedAs(f.thread, 0x30f0, "synth"));
}

// Child processes for the fatal conditions.
constexpr std::uint32_t kTagUpdateMode3 = 0xffffffffu;

struct MfcStopCase {
    const char* arg;
    std::uint32_t cmd, lsa, ea, size, tag;
    void (*setup)(MfcFixture&);
    const char* line;  // after "tox2: stop: spu: mfc "
};

void setupEahSet(MfcFixture& f) { f.mfc.writeEah(1); }
void setupListStall(MfcFixture& f) {
    putBe32Ls(f.thread, 0x100, 0x80000010u);
    putBe32Ls(f.thread, 0x104, kPatternEa);
}
void setupListBad(MfcFixture& f) {
    putBe32Ls(f.thread, 0x100, 0x20);
    putBe32Ls(f.thread, 0x104, kPatternEa + 8);  // 32 bytes need a 16-byte aligned address
}

#define MFC_AT " at ls 0x03000 on thread 0x01000200"
const MfcStopCase kMfcStops[] = {
        {"--mfc-size3", 0x40, 0x2000, kPatternEa, 3, 0, nullptr,
         "get (command 0x40): size is not 1, 2, 4, 8 or a multiple of 16" MFC_AT},
        {"--mfc-size20", 0x40, 0x2000, kPatternEa, 20, 0, nullptr,
         "get (command 0x40): size is not a multiple of 16" MFC_AT},
        {"--mfc-size0", 0x20, 0x2000, kPatternEa, 0, 0, nullptr,
         "put (command 0x20): size is 0 or above 0x4000" MFC_AT},
        {"--mfc-size-big", 0x40, 0x2000, kPatternEa, 0x4010, 0, nullptr,
         "get (command 0x40): size is 0 or above 0x4000" MFC_AT},
        {"--mfc-ea16", 0x40, 0x2000, kPatternEa + 8, 16, 0, nullptr,
         "get (command 0x40): address is not 16-byte aligned" MFC_AT},
        {"--mfc-ls16", 0x20, 0x2008, kPatternEa, 32, 0, nullptr,
         "put (command 0x20): address is not 16-byte aligned" MFC_AT},
        {"--mfc-natural", 0x40, 0x2000, kPatternEa + 2, 4, 0, nullptr,
         "get (command 0x40): address is not aligned" MFC_AT},
        {"--mfc-put-natural", 0x20, 0x2004, kPatternEa + 4, 8, 0, nullptr,
         "put (command 0x20): address is not aligned" MFC_AT},
        {"--mfc-nibble", 0x40, 0x2004, kPatternEa + 8, 4, 0, nullptr,
         "get (command 0x40): ls and ea differ in the low 4 bits" MFC_AT},
        {"--mfc-tag32", 0x40, 0x2000, kPatternEa, 16, 32, nullptr, "get (command 0x40): tag id above 31" MFC_AT},
        {"--mfc-4gib", 0x40, 0x2000, 0xffffffe0u, 64, 0, nullptr, "get (command 0x40): range passes 4 GiB" MFC_AT},
        {"--mfc-eah", 0x40, 0x2000, kPatternEa, 16, 0, setupEahSet,
         "get (command 0x40): effective address above 4 GiB" MFC_AT},
        {"--mfc-sndsig", 0xa0, 0, 0, 4, 0, nullptr, "sndsig (command 0xa0): command is not implemented" MFC_AT},
        {"--mfc-unknown", 0x48, 0x2000, kPatternEa, 16, 0, nullptr,
         "unknown (command 0x48): command is not implemented" MFC_AT},
        {"--mfc-atomic-size", 0xd0, 0x1000, kPatternEa, 64, 0, nullptr,
         "getllar (command 0xd0): size is not 128" MFC_AT},
        {"--mfc-list-stall", 0x44, 0x2000, 0x100, 8, 0, setupListStall,
         "getl (command 0x44): list element with stall-and-notify is not implemented" MFC_AT},
        {"--mfc-list-align", 0x44, 0x2000, 0x104, 8, 0, nullptr,
         "getl (command 0x44): list address is not 8-byte aligned" MFC_AT},
        {"--mfc-list-size", 0x24, 0x2000, 0x100, 12, 0, nullptr,
         "putl (command 0x24): list size is not a multiple of 8 up to 0x4000" MFC_AT},
        {"--mfc-list-element", 0x44, 0x2000, 0x100, 8, 0, setupListBad,
         "getl (command 0x44): address is not 16-byte aligned" MFC_AT},
        {"--mfc-tag-update", kTagUpdateMode3, 0, 0, 0, 0, nullptr, "tag update mode 3 is invalid" MFC_AT},
};
#undef MFC_AT

int runMfcChild(std::string_view mode) {
    tox2::installProcessSettings();
    for (const MfcStopCase& c : kMfcStops) {
        if (mode != c.arg) continue;
        MfcFixture f;
        if (c.setup != nullptr) c.setup(f);
        if (c.cmd == kTagUpdateMode3) {
            f.mfc.writeTagUpdate(kMfcSite, 3);
        } else {
            f.run(c.cmd, c.lsa, c.ea, c.size, c.tag);
        }
        return 0;  // not reached: the stop ends the process
    }
    return 99;
}

void testMfcStopChildren() {
    for (const MfcStopCase& c : kMfcStops) {
        const std::string narrow = c.arg;
        const ChildResult r = runChild(std::wstring(narrow.begin(), narrow.end()).c_str());
        const std::string expected = std::string("tox2: stop: spu: mfc ") + c.line;
        const bool found = r.output.find(expected) != std::string::npos;
        const bool dump = r.output.find("trace (oldest first)") != std::string::npos;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(found);
        CHECK(dump);
        if (!found || !dump) std::fprintf(stderr, "child %s output:\n%s\n", c.arg, r.output.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// Channels: mailboxes, events, decrementer, machine status. The hooks are called directly on a thread
// that has no host thread of its own; blocking calls run on helper threads.
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kChanSite = 0x3000;
constexpr std::uint32_t kLineEa = 0x2000;  // a reservation line in the fixture's guest memory

struct ChanOwner : tox2::SpuOwner {
    std::atomic<int> intr_count{0};
    std::atomic<std::uint32_t> intr_value{0};
    std::atomic<std::uint32_t> intr_out_value{0};
    std::atomic<bool> intr_took_out{false};
    std::atomic<int> stop_count{0};
    std::atomic<std::uint32_t> stop_site{0};
    std::atomic<std::uint32_t> stop_code{0};
    std::atomic<bool> stop_result{true};

    void onInterruptMailbox(tox2::SpuThread& thread, std::uint32_t value) override {
        // The channel mutex must not be held here: taking the outbound mailbox needs it.
        std::uint32_t out = 0;
        intr_took_out = thread.channels().takeOutMbox(&out);
        intr_out_value = out;
        intr_value = value;
        intr_count++;
    }
    bool onStop(tox2::SpuThread&, std::uint32_t site, std::uint32_t code) override {
        stop_site = site;
        stop_code = code;
        stop_count++;
        return stop_result;
    }
    void onFinished(tox2::SpuThread&, tox2::SpuLeave) override {}
};

struct ChanFixture {
    std::vector<std::uint8_t> guest = std::vector<std::uint8_t>(0x10000, 0);
    ChanOwner owner;
    tox2::SpuThread thread;
    tox2::SpuChannels& ch;

    explicit ChanFixture(bool with_owner = true)
        : thread(0x01000300, "chan", with_owner ? &owner : nullptr), ch(thread.channels()) {
        ch.mfc().setGuestBase(guest.data());
        for (std::uint32_t i = 0; i < 128; i++) guest[kLineEa + i] = static_cast<std::uint8_t>(i + 1);
    }

    bool rd(std::uint32_t channel, std::uint32_t* value) {
        return tox2SpuReadChannel(&thread.context(), thread.ls(), kChanSite, channel, value);
    }
    bool wr(std::uint32_t channel, std::uint32_t value) {
        return tox2SpuWriteChannel(&thread.context(), thread.ls(), kChanSite, channel, value);
    }
    std::uint32_t count(std::uint32_t channel) { return tox2SpuChannelCount(&thread.context(), thread.ls(), channel); }

    // GETLLAR of the fixture's line through the MFC channels, then reads the atomic status.
    std::uint32_t getllar() {
        wr(tox2::chan::kMfcLsa, 0x1000);
        wr(tox2::chan::kMfcEah, 0);
        wr(tox2::chan::kMfcEal, kLineEa);
        wr(tox2::chan::kMfcSize, 128);
        wr(tox2::chan::kMfcTagId, 0);
        wr(tox2::chan::kMfcCmd, tox2::mfc::kGetLLAR);
        std::uint32_t status = 99;
        rd(tox2::chan::kRdAtomicStat, &status);
        return status;
    }
    std::uint32_t putllc() {
        wr(tox2::chan::kMfcCmd, tox2::mfc::kPutLLC);
        std::uint32_t status = 99;
        rd(tox2::chan::kRdAtomicStat, &status);
        return status;
    }

    // Another agent's PUTLLC on the line.
    void otherPutllc() {
        tox2::Reservation res;
        std::uint8_t line[128];
        tox2::reservations().getllar(guest.data(), kLineEa, line, res);
        line[0] ^= 0x5a;
        tox2::reservations().putllc(guest.data(), kLineEa, line, res);
    }
    void otherPlainStore() { plainStore8(guest.data() + kLineEa + 0x72, guest[kLineEa + 0x72] ^ 0x5a); }

    bool blockedIn(std::string_view what) const {
        const char* in = thread.blockedIn();
        return thread.state() == tox2::SpuState::Blocked && in != nullptr && std::string_view(in).contains(what);
    }
};

template <class Pred>
bool waitUntil(Pred pred, int timeout_ms = 3000) {
    const auto deadline = Clock::now() + milliseconds(timeout_ms);
    while (!pred()) {
        if (Clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// Runs a channel read on a helper thread so the test can watch it block and wake.
struct AsyncRead {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::atomic<std::uint32_t> value{0};
    std::atomic<Clock::rep> finished{0};
    std::thread worker;

    AsyncRead(ChanFixture& f, std::uint32_t channel) {
        worker = std::thread([this, &f, channel] {
            std::uint32_t v = 0;
            ok = f.rd(channel, &v);
            value = v;
            finished = Clock::now().time_since_epoch().count();
            done = true;
        });
    }
    ~AsyncRead() {
        if (worker.joinable()) worker.join();
    }
    double latencyMsSince(Clock::time_point t) const {
        const auto end = Clock::time_point(Clock::duration(finished.load()));
        return std::chrono::duration<double, std::milli>(end - t).count();
    }
};

void testChanInMbox() {
    ChanFixture f;
    std::uint32_t v = 0;
    CHECK(f.count(tox2::chan::kRdInMbox) == 0);
    for (std::uint32_t i = 1; i <= 4; i++) CHECK(f.ch.pushInMbox(i * 0x11));
    CHECK(f.count(tox2::chan::kRdInMbox) == 4);
    // A fifth value is dropped, or replaces the newest entry on request.
    CHECK(!f.ch.pushInMbox(0x99));
    CHECK(f.count(tox2::chan::kRdInMbox) == 4);
    CHECK(!f.ch.pushInMbox(0x55, true));
    CHECK(f.count(tox2::chan::kRdInMbox) == 4);
    for (const std::uint32_t expected : {0x11u, 0x22u, 0x33u, 0x55u}) {
        CHECK(f.rd(tox2::chan::kRdInMbox, &v) && v == expected);
    }
    CHECK(f.count(tox2::chan::kRdInMbox) == 0);

    // A blocking read is woken by a push from another thread and shows as blocked meanwhile.
    {
        AsyncRead reader(f, tox2::chan::kRdInMbox);
        CHECK(waitUntil([&] { return f.blockedIn("RdInMbox") && f.thread.indefiniteWait(); }));
        CHECK(!reader.done);
        std::this_thread::sleep_for(milliseconds(10));
        CHECK(!reader.done);
        f.ch.pushInMbox(0xabcd);
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(reader.ok && reader.value == 0xabcd);
        CHECK(f.thread.state() == tox2::SpuState::Running);
    }

    // A halt ends the wait with false.
    {
        AsyncRead reader(f, tox2::chan::kRdInMbox);
        CHECK(waitUntil([&] { return f.blockedIn("RdInMbox"); }));
        f.thread.requestHalt();
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(!reader.ok);
    }
}

void testChanOutMbox() {
    ChanFixture f;
    std::uint32_t v = 0;
    CHECK(f.count(tox2::chan::kWrOutMbox) == 1 && !f.ch.outMboxFull());
    CHECK(f.wr(tox2::chan::kWrOutMbox, 0x1234));
    CHECK(f.count(tox2::chan::kWrOutMbox) == 0 && f.ch.outMboxFull());

    // A second write blocks until the owner takes the first value.
    {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};
        std::thread writer([&] {
            ok = f.wr(tox2::chan::kWrOutMbox, 0x5678);
            done = true;
        });
        CHECK(waitUntil([&] { return f.blockedIn("WrOutMbox"); }));
        std::this_thread::sleep_for(milliseconds(10));
        CHECK(!done);
        CHECK(f.ch.takeOutMbox(&v) && v == 0x1234);
        CHECK(waitUntil([&] { return done.load(); }));
        writer.join();
        CHECK(ok);
        CHECK(f.ch.takeOutMbox(&v) && v == 0x5678);
        CHECK(!f.ch.takeOutMbox(&v));
    }

    // A halt ends a blocked write with false.
    f.wr(tox2::chan::kWrOutMbox, 1);
    {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{true};
        std::thread writer([&] {
            ok = f.wr(tox2::chan::kWrOutMbox, 2);
            done = true;
        });
        CHECK(waitUntil([&] { return f.blockedIn("WrOutMbox"); }));
        f.thread.requestHalt();
        CHECK(waitUntil([&] { return done.load(); }));
        writer.join();
        CHECK(!ok);
        CHECK(f.ch.takeOutMbox(&v) && v == 1);
    }
}

void testChanOutIntrMbox() {
    ChanFixture f;
    CHECK(f.count(tox2::chan::kWrOutIntrMbox) == 1);
    f.wr(tox2::chan::kWrOutMbox, 0x77);
    CHECK(f.wr(tox2::chan::kWrOutIntrMbox, 0x40000000 | (16u << 24) | 5));
    // The owner ran without the channel lock (it took the outbound mailbox) and saw both values.
    CHECK(f.owner.intr_count == 1 && f.owner.intr_took_out);
    CHECK(f.owner.intr_out_value == 0x77 && f.owner.intr_value == (0x40000000u | (16u << 24) | 5));
    CHECK(f.count(tox2::chan::kWrOutIntrMbox) == 1 && !f.ch.outMboxFull());
}

void testChanStop() {
    ChanFixture f;
    CHECK(tox2SpuStop(&f.thread.context(), f.thread.ls(), 0x3010, 0x102) == true);
    CHECK(f.owner.stop_count == 1 && f.owner.stop_site == 0x3010 && f.owner.stop_code == 0x102);
    f.owner.stop_result = false;
    CHECK(tox2SpuStop(&f.thread.context(), f.thread.ls(), 0x3014, 0x101) == false);
    CHECK(f.owner.stop_count == 2 && f.owner.stop_code == 0x101);
}

void testChanMiscChannels() {
    ChanFixture f;
    std::uint32_t v = 99;
    // RdMachStat follows int_enabled; bit 1 stays clear.
    f.thread.context().int_enabled = 0;
    CHECK(f.rd(tox2::chan::kRdMachStat, &v) && v == 0);
    f.thread.context().int_enabled = 1;
    CHECK(f.rd(tox2::chan::kRdMachStat, &v) && v == 1);
    f.thread.context().int_enabled = 0;
    CHECK(f.rd(tox2::chan::kRdMachStat, &v) && v == 0);

    CHECK(f.wr(tox2::chan::kWrSrr0, 0x1234));
    CHECK(f.rd(tox2::chan::kRdSrr0, &v) && v == 0x1234 && f.thread.context().srr0 == 0x1234);

    // Event mask and tag mask read back what was written.
    CHECK(f.wr(tox2::chan::kWrEventMask, 0x410));
    CHECK(f.rd(tox2::chan::kRdEventMask, &v) && v == 0x410 && f.ch.eventMask() == 0x410);
    CHECK(f.wr(tox2::chan::kWrTagMask, 0x80000001u));
    CHECK(f.rd(tox2::chan::kRdTagMask, &v) && v == 0x80000001u);

    // Counts: constants, the MFC queue, and the status channels.
    for (const std::uint32_t c : {1u, 2u, 7u, 8u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 22u, 23u}) {
        CHECK(f.count(c) == 1);
    }
    CHECK(f.count(tox2::chan::kMfcCmd) == tox2::mfc::kQueueSlots);
    CHECK(f.count(tox2::chan::kRdTagStat) == 0 && f.count(tox2::chan::kRdAtomicStat) == 0);
    f.wr(tox2::chan::kWrTagUpdate, 0);
    CHECK(f.count(tox2::chan::kRdTagStat) == 1);
    CHECK(f.rd(tox2::chan::kRdTagStat, &v) && v == 0x80000001u);
    CHECK(f.count(tox2::chan::kRdTagStat) == 0);
    CHECK(f.getllar() == 4);
    CHECK(f.count(tox2::chan::kRdAtomicStat) == 0);
    CHECK(f.ch.readCount(tox2::chan::kRdTagStat) == 1 && f.ch.readCount(tox2::chan::kRdAtomicStat) == 1);
}

void testChanDecrementer() {
    ChanFixture f;
    std::uint32_t v = 99;
    CHECK(f.rd(tox2::chan::kRdDec, &v) && v == 0);
    CHECK(f.wr(tox2::chan::kWrDec, 0x7fffffffu));
    std::uint32_t first = 0, second = 0;
    CHECK(f.rd(tox2::chan::kRdDec, &first));
    std::this_thread::sleep_for(milliseconds(20));
    CHECK(f.rd(tox2::chan::kRdDec, &second));
    CHECK(first <= 0x7fffffffu && second < first);
    // 20 ms at 79.8 MHz are about 1.6 million ticks; allow for scheduling in both directions.
    const std::uint32_t ticks = first - second;
    CHECK(ticks >= 1000000u && ticks < 80000000u);

    // The decrementer event comes when the count passes zero: 79,800 ticks are 1 ms.
    CHECK(f.wr(tox2::chan::kWrEventAck, 0xffffffffu));
    CHECK(f.wr(tox2::chan::kWrDec, 79800 * 5));
    CHECK(f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kDecrementer));
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
    const auto start = Clock::now();
    CHECK(f.rd(tox2::chan::kRdEventStat, &v) && v == tox2::spu_event::kDecrementer);
    CHECK(Clock::now() - start >= milliseconds(4));
    CHECK(Clock::now() - start < milliseconds(2000));
    CHECK(f.rd(tox2::chan::kRdDec, &v) && static_cast<std::int32_t>(v) < 0);
    // Sticky until acknowledged.
    CHECK(f.count(tox2::chan::kRdEventStat) == 1);
    CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kDecrementer));
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
}

void testChanLevelEvents() {
    ChanFixture f;
    std::uint32_t v = 99;
    // Queue vacancy is always there.
    f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kQueueVacancy);
    CHECK(f.count(tox2::chan::kRdEventStat) == 1 && f.rd(tox2::chan::kRdEventStat, &v) && v == 0x8);

    // Inbound mailbox available follows the FIFO.
    f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kInMboxAvailable);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
    f.ch.pushInMbox(7);
    CHECK(f.count(tox2::chan::kRdEventStat) == 1 && f.rd(tox2::chan::kRdEventStat, &v) && v == 0x10);
    f.rd(tox2::chan::kRdInMbox, &v);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);

    // A blocked wait on it is woken by a push from another thread.
    {
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        const auto t0 = Clock::now();
        f.ch.pushInMbox(8);
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(reader.ok && reader.value == 0x10);
        CHECK(reader.latencyMsSince(t0) < 100.0);
    }
    f.rd(tox2::chan::kRdInMbox, &v);

    // Tag-group status update.
    f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kTagStatusUpdate);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
    f.wr(tox2::chan::kWrTagMask, 1);
    f.wr(tox2::chan::kWrTagUpdate, 2);
    CHECK(f.count(tox2::chan::kRdEventStat) == 1 && f.rd(tox2::chan::kRdEventStat, &v) && v == 0x1);
    f.rd(tox2::chan::kRdTagStat, &v);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
}

void testChanLockLineLost() {
    ChanFixture f;
    std::uint32_t v = 99;
    CHECK(f.getllar() == 4);
    CHECK(f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kLockLineLost));
    CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kLockLineLost));
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);

    // Woken at once by a PUTLLC from another agent.
    {
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        std::this_thread::sleep_for(milliseconds(10));
        CHECK(!reader.done);
        const auto t0 = Clock::now();
        f.otherPutllc();
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(reader.ok && reader.value == tox2::spu_event::kLockLineLost);
        CHECK(reader.latencyMsSince(t0) < 5.0);
    }
    // The failing PUTLLC clears the MFC's loss; the event stays until the acknowledgement.
    CHECK(f.putllc() == 1);
    CHECK(!f.ch.mfc().hasReservation());
    CHECK(f.count(tox2::chan::kRdEventStat) == 1);
    CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kLockLineLost));
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);

    // The idle loop of the system service: acknowledge, then PUTLLC. The acknowledged loss does not come
    // back when the PUTLLC looks again.
    CHECK(f.getllar() == 4);
    f.otherPutllc();
    CHECK(f.rd(tox2::chan::kRdEventStat, &v) && v == 0x400);
    CHECK(f.wr(tox2::chan::kWrEventAck, 0x400));
    CHECK(f.putllc() == 1);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);
    // A loss that nobody polled for before the failing PUTLLC is still recorded.
    CHECK(f.getllar() == 4);
    f.otherPutllc();
    CHECK(f.putllc() == 1);
    CHECK(f.count(tox2::chan::kRdEventStat) == 1);
    f.wr(tox2::chan::kWrEventAck, 0x400);
    // An untouched reservation lets PUTLLC through and raises nothing.
    CHECK(f.getllar() == 4);
    CHECK(f.putllc() == 0);
    CHECK(f.count(tox2::chan::kRdEventStat) == 0);

    // A plain write is noticed within 5 ms (best of a few tries: the wait polls every millisecond).
    double best = 1e9;
    for (int attempt = 0; attempt < 8 && best >= 5.0; attempt++) {
        CHECK(f.getllar() == 4);
        f.wr(tox2::chan::kWrEventAck, 0x400);
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        std::this_thread::sleep_for(milliseconds(3));
        const auto t0 = Clock::now();
        f.otherPlainStore();
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(reader.ok && reader.value == 0x400);
        best = std::min(best, reader.latencyMsSince(t0));
    }
    CHECK(best < 5.0);
    f.wr(tox2::chan::kWrEventAck, 0x400);

    // A mixed mask is also woken by a mailbox push.
    CHECK(f.getllar() == 4);
    f.wr(tox2::chan::kWrEventMask, 0x410);
    {
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        std::this_thread::sleep_for(milliseconds(5));
        const auto t0 = Clock::now();
        f.ch.pushInMbox(1);
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(reader.ok && reader.value == 0x10);
        CHECK(reader.latencyMsSince(t0) < 50.0);
    }

    // A halt ends the wait on the reservation table.
    f.rd(tox2::chan::kRdInMbox, &v);
    {
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        f.thread.requestHalt();
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(!reader.ok);
    }
}

// A halt ends the wait on the thread's own condition variable.
void testChanHaltOnCondvar() {
    ChanFixture f;
    f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kInMboxAvailable);
    AsyncRead reader(f, tox2::chan::kRdEventStat);
    CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
    f.thread.requestHalt();
    CHECK(waitUntil([&] { return reader.done.load(); }));
    CHECK(!reader.ok);
}

// Children for the fatal conditions.
struct ChanStopCase {
    const char* arg;
    const char* line;  // after "tox2: stop: spu: "
};

const ChanStopCase kChanStops[] = {
        {"--chan-mask0",
         "rdch 0 (SPU_RdEventStat) at ls 0x03000 on thread 0x01000300: event mask 0x0 has no implemented"},
        {"--chan-mask-unimplemented",
         "rdch 0 (SPU_RdEventStat) at ls 0x03000 on thread 0x01000300: event mask 0x1000 has no implemented source"},
        {"--chan-tagstat", "rdch 24 (MFC_RdTagStat) at ls 0x03000 on thread 0x01000300: no tag update was requested"},
        {"--chan-atomicstat", "rdch 27 (MFC_RdAtomicStat) at ls 0x03000 on thread 0x01000300: no atomic command"},
        {"--chan-rd-signal", "rdch 3 (SPU_RdSigNotify1) at ls 0x03000 on thread 0x01000300: channel is not readable"},
        {"--chan-wr-readonly", "wrch 13 (SPU_RdMachStat) at ls 0x03000 on thread 0x01000300: channel is not writable"},
        {"--chan-wr-liststall",
         "wrch 26 (MFC_WrListStallAck) at ls 0x03000 on thread 0x01000300: channel is not writable"},
        {"--chan-count-signal", "rchcnt 4 (SPU_RdSigNotify2) at ls 0x00000 on thread 0x01000300: channel is not impl"},
        {"--chan-intr-noowner",
         "wrch 30 (SPU_WrOutIntrMbox) at ls 0x03000 on thread 0x01000300: the thread has no owner"},
        {"--chan-stop-noowner", "stop 0x120 at ls 0x03000 on thread 0x01000300: not implemented"},
};

int runChanChild(std::string_view mode) {
    tox2::installProcessSettings();
    ChanFixture f(mode != "--chan-intr-noowner" && mode != "--chan-stop-noowner");
    std::uint32_t v = 0;
    if (mode == "--chan-mask0") {
        f.rd(tox2::chan::kRdEventStat, &v);
    } else if (mode == "--chan-mask-unimplemented") {
        f.wr(tox2::chan::kWrEventMask, 0x1000);
        f.rd(tox2::chan::kRdEventStat, &v);
    } else if (mode == "--chan-tagstat") {
        f.rd(tox2::chan::kRdTagStat, &v);
    } else if (mode == "--chan-atomicstat") {
        f.rd(tox2::chan::kRdAtomicStat, &v);
    } else if (mode == "--chan-rd-signal") {
        f.rd(tox2::chan::kRdSigNotify1, &v);
    } else if (mode == "--chan-wr-readonly") {
        f.wr(tox2::chan::kRdMachStat, 1);
    } else if (mode == "--chan-wr-liststall") {
        f.wr(tox2::chan::kWrListStallAck, 1);
    } else if (mode == "--chan-count-signal") {
        f.count(tox2::chan::kRdSigNotify2);
    } else if (mode == "--chan-intr-noowner") {
        f.wr(tox2::chan::kWrOutIntrMbox, 1);
    } else if (mode == "--chan-stop-noowner") {
        tox2SpuStop(&f.thread.context(), f.thread.ls(), kChanSite, 0x120);
    } else {
        return 99;
    }
    return 0;  // not reached: the stop ends the process
}

void testChanStopChildren() {
    for (const ChanStopCase& c : kChanStops) {
        const std::string narrow = c.arg;
        const ChildResult r = runChild(std::wstring(narrow.begin(), narrow.end()).c_str());
        const std::string expected = std::string("tox2: stop: spu: ") + c.line;
        const bool found = r.output.find(expected) != std::string::npos;
        const bool dump = r.output.find("trace (oldest first)") != std::string::npos;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(found);
        CHECK(dump);
        if (!found || !dump) std::fprintf(stderr, "child %s output:\n%s\n", c.arg, r.output.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return runStopChild(argv[1]);
    // Timed waits round up to the system tick otherwise, which the latency checks would measure.
    const tox2::TimerResolution timer_resolution;

    testGetllarPutllc();
    testPlainWriteBeforePutllc();
    testPutInvalidatesReservation();
    testPutPieces();
    testStoreConditional();
    testIncrementStress();
    testCompareWindow();
    testUnchangedChunkSurvivesRace();
    testWaitForStoreEpoch();
    testWaitersAreNotLost();
    testFailedWritersDoNotLoseReservation();
    testGetllarIsNotTorn();
    testSharedTable();
    testDispatchChain();
    testDispatchAcrossImages();
    testTrackingIntersection();
    testCopyWrapsAtLsEnd();
    testFirstImageLog();
    testCheckImages();
    testHostThread();
    testStopChildren();
    testMfcGetPut();
    testMfcLsWrap();
    testMfcImageTracking();
    testMfcAtomics();
    testMfcTagStatus();
    testMfcLists();
    testMfcStopChildren();
    testChanInMbox();
    testChanOutMbox();
    testChanOutIntrMbox();
    testChanStop();
    testChanMiscChannels();
    testChanDecrementer();
    testChanLevelEvents();
    testChanLockLineLost();
    testChanHaltOnCondvar();
    testChanStopChildren();

    std::printf("spu runtime tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

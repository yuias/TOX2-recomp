// Self-checking unit tests for the SPU runtime pieces. Most tests address plain host buffers through the
// same base + ea convention; the lv2 group tests use the process's guest memory and the syscall table.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "lv2/cell_error.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "spu/spu_channels.h"
#include "spu/spu_mfc.h"
#include "spu/spu_thread.h"
#include "tox2/ppu_context.h"
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
int runLv2Child(std::string_view mode);
int runHostUnknownChild();

int runStopChild(std::string_view mode) {
    if (mode.starts_with("--mfc-")) return runMfcChild(mode);
    if (mode.starts_with("--chan-")) return runChanChild(mode);
    if (mode.starts_with("--lv2-")) return runLv2Child(mode);
    if (mode == "--spu-host-unknown-child") return runHostUnknownChild();
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

// The inbound mailbox can be replaced by one value, whatever it holds.
void testChanReplaceInMbox() {
    ChanFixture f;
    std::uint32_t v = 0;
    for (std::uint32_t i = 1; i <= 4; i++) CHECK(f.ch.pushInMbox(i));
    f.ch.replaceInMbox(0x99);
    CHECK(f.count(tox2::chan::kRdInMbox) == 1);
    CHECK(f.rd(tox2::chan::kRdInMbox, &v) && v == 0x99);
    CHECK(f.count(tox2::chan::kRdInMbox) == 0);
    f.ch.replaceInMbox(0x55);  // also into an empty mailbox
    CHECK(f.count(tox2::chan::kRdInMbox) == 1 && f.rd(tox2::chan::kRdInMbox, &v) && v == 0x55);
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

// ---------------------------------------------------------------------------------------------
// lv2 SPU thread groups: synthetic SPU programs run through the group syscalls on real host threads.
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kLvMain = 0x3000;         // GET a word, add one, PUT it, throw an event, exit 0x102
constexpr std::uint32_t kLvExitGroup = 0x3100;    // status = low word of $3, stop 0x101
constexpr std::uint32_t kLvWaitMbox = 0x3200;     // blocks in RdInMbox
constexpr std::uint32_t kLvCollect = 0x3300;      // see lvCollect
constexpr std::uint32_t kLvThrowTwo = 0x3400;     // two events, the second waits for room in the queue
constexpr std::uint32_t kLvSendEvent = 0x3500;    // send_event on a bound and an unbound port
constexpr std::uint32_t kLvFlag = 0x3600;         // event-flag set bit, valid and invalid bit
constexpr std::uint32_t kLvBadStop = 0x3700;      // stop 0x1234
constexpr std::uint32_t kLvEmptyExit = 0x3800;    // stop 0x100, then stop 0x102 with an empty OutMbox
constexpr std::uint32_t kLvIntrEmpty = 0x3900;    // WrOutIntrMbox without an OutMbox value
constexpr std::uint32_t kLvIntrBadCode = 0x3a00;  // WrOutIntrMbox with an unsupported code
constexpr std::uint32_t kLvSendBusy = 0x3b00;     // send_event while the inbound mailbox is full
constexpr std::uint32_t kLvLog = 0x3c00;          // logs a line, exits with status 7
constexpr std::uint32_t kLvFatal = 0x3d00;        // fatal() on the SPU host thread
constexpr std::uint32_t kLvFault = 0x3e00;        // a GET from an uncommitted guest address
constexpr std::uint32_t kLvCodeSize = 0x1000;
constexpr std::uint32_t kLvDataLs = 0x8000;  // FILL segment, 0x100 bytes of kLvFillValue
constexpr std::uint32_t kLvFillValue = 0xa5a5a5a5;
constexpr std::uint32_t kLvBuf = 0x8200;  // the word of kLvMain; the values of kLvCollect

constexpr std::uint32_t kLvEventValue = 0x1234;
constexpr std::uint32_t kLvMainStatus = 0x2b;
constexpr std::uint32_t kLvPort = 16;

// Channel access that leaves the function when the hook asks the thread to leave.
#define LV_WR(ch, value)                                                             \
    do {                                                                             \
        if (!tox2SpuWriteChannel(ctx, ls, site, (ch), (value))) TOX2_SPU_SF_LEAVE(); \
    } while (0)
#define LV_RD(ch, out)                                                            \
    do {                                                                          \
        if (!tox2SpuReadChannel(ctx, ls, site, (ch), (out))) TOX2_SPU_SF_LEAVE(); \
    } while (0)
#define LV_STOP(code)                                                 \
    do {                                                              \
        if (!tox2SpuStop(ctx, ls, site, (code))) TOX2_SPU_SF_LEAVE(); \
    } while (0)

// GET or PUT of `size` bytes between LS `lsa` and guest address `ea`, with the tag wait.
#define LV_DMA(lsa, ea, size, cmd)                                          \
    do {                                                                    \
        std::uint32_t stat = 0;                                             \
        LV_WR(tox2::chan::kMfcLsa, (lsa));                                  \
        LV_WR(tox2::chan::kMfcEah, static_cast<std::uint32_t>((ea) >> 32)); \
        LV_WR(tox2::chan::kMfcEal, static_cast<std::uint32_t>(ea));         \
        LV_WR(tox2::chan::kMfcSize, (size));                                \
        LV_WR(tox2::chan::kMfcTagId, 1);                                    \
        LV_WR(tox2::chan::kMfcCmd, (cmd));                                  \
        LV_WR(tox2::chan::kWrTagMask, 2);                                   \
        LV_WR(tox2::chan::kWrTagUpdate, 2);                                 \
        LV_RD(tox2::chan::kRdTagStat, &stat);                               \
    } while (0)

TOX2_SPU_FUNC(lvMain) {
    TOX2_SPU_SF_ENTER(kLvMain);
    const std::uint32_t site = kLvMain + 4;
    const std::uint64_t ea = TOX2_VR_D(ctx->gpr[3], 0);
    LV_DMA(kLvBuf, ea, 4, tox2::mfc::kGet);
    const std::uint32_t word = loadBe32(ls + kLvBuf) + 1;
    storeBe32(ls + kLvBuf, word);
    LV_DMA(kLvBuf, ea, 4, tox2::mfc::kPut);
    LV_WR(tox2::chan::kWrOutMbox, word);  // data3 of the event
    LV_WR(tox2::chan::kWrOutIntrMbox, ((64u + kLvPort) << 24) | kLvEventValue);
    LV_WR(tox2::chan::kWrOutMbox, kLvMainStatus);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvExitGroup) {
    TOX2_SPU_SF_ENTER(kLvExitGroup);
    const std::uint32_t site = kLvExitGroup + 4;
    LV_WR(tox2::chan::kWrOutMbox, TOX2_VR_W(ctx->gpr[3], 1));
    LV_STOP(0x101);
}

TOX2_SPU_FUNC(lvWaitMbox) {
    TOX2_SPU_SF_ENTER(kLvWaitMbox);
    const std::uint32_t site = kLvWaitMbox + 4;
    std::uint32_t value = 0;
    LV_RD(tox2::chan::kRdInMbox, &value);
    LV_WR(tox2::chan::kWrOutMbox, value);
    LV_STOP(0x102);
}

// Fills the outbound mailbox, so that the next write blocks, then collects four inbound values into LS and
// waits for a fifth.
TOX2_SPU_FUNC(lvCollect) {
    TOX2_SPU_SF_ENTER(kLvCollect);
    const std::uint32_t site = kLvCollect + 4;
    LV_WR(tox2::chan::kWrOutMbox, 0xaa);
    LV_WR(tox2::chan::kWrOutMbox, 0xbb);
    for (std::uint32_t i = 0; i < 4; i++) {
        std::uint32_t value = 0;
        LV_RD(tox2::chan::kRdInMbox, &value);
        storeBe32(ls + kLvBuf + 4 * i, value);
    }
    std::uint32_t value = 0;
    LV_RD(tox2::chan::kRdInMbox, &value);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvThrowTwo) {
    TOX2_SPU_SF_ENTER(kLvThrowTwo);
    const std::uint32_t site = kLvThrowTwo + 4;
    LV_WR(tox2::chan::kWrOutMbox, 0x11);
    LV_WR(tox2::chan::kWrOutIntrMbox, ((64u + kLvPort) << 24) | 1);
    LV_WR(tox2::chan::kWrOutMbox, 0x22);
    LV_WR(tox2::chan::kWrOutIntrMbox, ((64u + kLvPort) << 24) | 2);
    LV_WR(tox2::chan::kWrOutMbox, 5);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvSendEvent) {
    TOX2_SPU_SF_ENTER(kLvSendEvent);
    const std::uint32_t site = kLvSendEvent + 4;
    std::uint32_t first = 0, second = 0;
    LV_WR(tox2::chan::kWrOutMbox, 0x99);
    LV_WR(tox2::chan::kWrOutIntrMbox, (kLvPort << 24) | 0xabc);
    LV_RD(tox2::chan::kRdInMbox, &first);
    LV_WR(tox2::chan::kWrOutMbox, 0x98);
    LV_WR(tox2::chan::kWrOutIntrMbox, ((kLvPort + 1) << 24));
    LV_RD(tox2::chan::kRdInMbox, &second);
    LV_WR(tox2::chan::kWrOutMbox, first == 0 ? second : 0xdead0001);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvFlag) {
    TOX2_SPU_SF_ENTER(kLvFlag);
    const std::uint32_t site = kLvFlag + 4;
    std::uint32_t first = 0, second = 0;
    LV_WR(tox2::chan::kWrOutMbox, TOX2_VR_W(ctx->gpr[3], 1));
    LV_WR(tox2::chan::kWrOutIntrMbox, (128u << 24) | 5);
    LV_RD(tox2::chan::kRdInMbox, &first);
    LV_WR(tox2::chan::kWrOutMbox, TOX2_VR_W(ctx->gpr[3], 1));
    LV_WR(tox2::chan::kWrOutIntrMbox, (128u << 24) | 64);
    LV_RD(tox2::chan::kRdInMbox, &second);
    LV_WR(tox2::chan::kWrOutMbox, first == 0 ? second : 0xdead0001);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvBadStop) {
    TOX2_SPU_SF_ENTER(kLvBadStop);
    const std::uint32_t site = kLvBadStop + 4;
    LV_STOP(0x1234);
}

TOX2_SPU_FUNC(lvEmptyExit) {
    TOX2_SPU_SF_ENTER(kLvEmptyExit);
    const std::uint32_t site = kLvEmptyExit + 4;
    LV_STOP(0x100);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvIntrEmpty) {
    TOX2_SPU_SF_ENTER(kLvIntrEmpty);
    const std::uint32_t site = kLvIntrEmpty + 4;
    LV_WR(tox2::chan::kWrOutIntrMbox, (64u << 24));
}

TOX2_SPU_FUNC(lvIntrBadCode) {
    TOX2_SPU_SF_ENTER(kLvIntrBadCode);
    const std::uint32_t site = kLvIntrBadCode + 4;
    LV_WR(tox2::chan::kWrOutMbox, 1);
    LV_WR(tox2::chan::kWrOutIntrMbox, (144u << 24));
}

// Blocks in WrOutMbox like lvCollect so the test can fill the inbound mailbox, then sends an event: the busy
// reply must be the only entry left in the inbound mailbox.
TOX2_SPU_FUNC(lvSendBusy) {
    TOX2_SPU_SF_ENTER(kLvSendBusy);
    const std::uint32_t site = kLvSendBusy + 4;
    std::uint32_t reply = 0;
    LV_WR(tox2::chan::kWrOutMbox, 0xaa);
    LV_WR(tox2::chan::kWrOutMbox, 0xbb);
    LV_WR(tox2::chan::kWrOutIntrMbox, (kLvPort << 24) | 1);
    LV_RD(tox2::chan::kRdInMbox, &reply);
    const std::uint32_t left = tox2SpuChannelCount(ctx, ls, tox2::chan::kRdInMbox);
    LV_WR(tox2::chan::kWrOutMbox, left == 0 ? reply : 0xdead0002);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvLog) {
    TOX2_SPU_SF_ENTER(kLvLog);
    const std::uint32_t site = kLvLog + 4;
    tox2::logInfo("tox2: test: hello from spu");
    LV_WR(tox2::chan::kWrOutMbox, 7);
    LV_STOP(0x102);
}

TOX2_SPU_FUNC(lvFatal) {
    (void)ls;
    TOX2_SPU_SF_ENTER(kLvFatal);
    tox2::fatal(tox2::FatalKind::GuestFault, "spu test fault");
}

TOX2_SPU_FUNC(lvFault) {
    TOX2_SPU_SF_ENTER(kLvFault);
    const std::uint32_t site = kLvFault + 4;
    LV_DMA(kLvBuf, std::uint64_t{0x30000000}, 16, tox2::mfc::kGet);
    LV_STOP(0x102);
}

// The image is registered with its guest address, which is only known once the fixture has memory.
Tox2SpuSegment g_lv_segments[] = {{0, kLvMain, kLvCodeSize, 1}};
const Tox2SpuEntry kLvEntries[] = {
        {kLvMain, 1, {0xc0de0000}, lvMain},
        {kLvExitGroup, 1, {0xc0de0001}, lvExitGroup},
        {kLvWaitMbox, 1, {0xc0de0002}, lvWaitMbox},
        {kLvCollect, 1, {0xc0de0003}, lvCollect},
        {kLvThrowTwo, 1, {0xc0de0004}, lvThrowTwo},
        {kLvSendEvent, 1, {0xc0de0005}, lvSendEvent},
        {kLvFlag, 1, {0xc0de0006}, lvFlag},
        {kLvBadStop, 1, {0xc0de0007}, lvBadStop},
        {kLvEmptyExit, 1, {0xc0de0008}, lvEmptyExit},
        {kLvIntrEmpty, 1, {0xc0de0009}, lvIntrEmpty},
        {kLvIntrBadCode, 1, {0xc0de000a}, lvIntrBadCode},
        {kLvSendBusy, 1, {0xc0de000b}, lvSendBusy},
        {kLvLog, 1, {0xc0de000c}, lvLog},
        {kLvFatal, 1, {0xc0de000d}, lvFatal},
        {kLvFault, 1, {0xc0de000e}, lvFault},
};
Tox2SpuImageInfo g_lv_image = {"lv2test", 0, kLvCodeSize, kNoHash, kLvMain, 1, g_lv_segments, 15, kLvEntries};

std::mutex g_lv_log_mutex;
std::vector<std::string> g_lv_log;

int countLogs(std::string_view text) {
    std::lock_guard lock(g_lv_log_mutex);
    int n = 0;
    for (const std::string& line : g_lv_log) n += line.contains(text) ? 1 : 0;
    return n;
}

// A PPU thread as the tests need it: a register context, and every call made under the guest lock (which
// a waiting call gives up while it waits).
struct PpuCaller {
    std::unique_ptr<Tox2PpuContext> ctx = std::make_unique<Tox2PpuContext>();

    std::uint32_t call(std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                       std::uint64_t r6 = 0, std::uint64_t r7 = 0, std::uint64_t r8 = 0) {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        ctx->gpr[11] = number;
        ctx->gpr[3] = r3;
        ctx->gpr[4] = r4;
        ctx->gpr[5] = r5;
        ctx->gpr[6] = r6;
        ctx->gpr[7] = r7;
        ctx->gpr[8] = r8;
        tox2Syscall(ctx.get(), tox2::process().memory.base(), 0x10000);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    }
};

// Guest memory layout of the scratch block.
constexpr std::uint32_t kLvIdOut = 0x000;
constexpr std::uint32_t kLvGroupAttr = 0x010;
constexpr std::uint32_t kLvThreadOut = 0x030;
constexpr std::uint32_t kLvThreadAttr = 0x050;
constexpr std::uint32_t kLvArgs = 0x070;
constexpr std::uint32_t kLvImage = 0x0a0;
constexpr std::uint32_t kLvSegments = 0x0c0;
constexpr std::uint32_t kLvQueueAttr = 0x100;
constexpr std::uint32_t kLvQueueOut = 0x120;
constexpr std::uint32_t kLvCauseOut = 0x140;
constexpr std::uint32_t kLvStatusOut = 0x144;
constexpr std::uint32_t kLvValueOut = 0x148;
constexpr std::uint32_t kLvFlagAttr = 0x160;
constexpr std::uint32_t kLvFlagOut = 0x190;
constexpr std::uint32_t kLvFlagsOut = 0x198;
constexpr std::uint32_t kLvNames = 0x200;
constexpr std::uint32_t kLvWord = 0x300;  // the guest word of kLvMain, 16-byte aligned
constexpr std::uint32_t kLvCode = 0x1000;

struct Lv2Fixture {
    std::uint8_t* base;
    std::uint32_t scratch;
    PpuCaller ppu;
    std::vector<std::uint32_t> groups;

    explicit Lv2Fixture(bool capture_log = true) {
        tox2::resetSyncObjects();
        tox2::resetSpuThreadGroups();
        tox2::resetSpuImages();
        tox2::resetLogOnce();
        {
            std::lock_guard lock(g_lv_log_mutex);
            g_lv_log.clear();
        }
        if (capture_log)
            tox2::setLogSink([](std::string_view line) {
                std::lock_guard lock(g_lv_log_mutex);
                g_lv_log.emplace_back(line);
            });
        tox2::process().memory.reserve();
        base = tox2::process().memory.base();
        scratch = tox2::process().main.allocate(0x10000, 0x10000);

        // The code bytes: the check word of each entry sits at the entry's address.
        for (std::uint32_t i = 0; i < kLvCodeSize; i += 4) tox2Write32(base, scratch + kLvCode + i, 0xeeeeeeee);
        for (const Tox2SpuEntry& entry : kLvEntries) {
            tox2Write32(base, scratch + kLvCode + (entry.ls - kLvMain), entry.check[0]);
        }
        g_lv_segments[0].ea = scratch + kLvCode;
        g_lv_image.ea = scratch + kLvCode;
        tox2::registerSpuImages(&g_lv_image, 1);

        // sys_spu_image: {type, entry, segments, count}; three segments: code, fill, and an empty copy.
        const std::uint32_t segs = scratch + kLvSegments;
        const std::uint32_t words[] = {1, kLvMain, kLvCodeSize, scratch + kLvCode, 2, kLvDataLs, 0x100, kLvFillValue};
        for (std::uint32_t i = 0; i < 8; i++) tox2Write32(base, segs + 4 * i, words[i]);
        tox2Write32(base, scratch + kLvImage, 0);
        tox2Write32(base, scratch + kLvImage + 8, segs);
        tox2Write32(base, scratch + kLvImage + 12, 2);
        putText(scratch + kLvNames, "lvgroup");
        putText(scratch + kLvNames + 0x80, "lvthread");
        tox2Write32(base, scratch + kLvWord, 41);
    }

    ~Lv2Fixture() {
        // A group that is still running must be ended first: resetting would wait for host threads that
        // may need the guest lock.
        for (const std::uint32_t group : groups) ppu.call(177, group, 0);
        tox2::resetSpuThreadGroups();
        tox2::resetSpuImages();
        tox2::resetSyncObjects();
        tox2::process().main.free(scratch);
        tox2::setLogSink({});
        tox2::resetLogOnce();
        tox2::registerSpuImages(kSynthImages, 2);
    }

    void putText(std::uint32_t at, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); i++) tox2Write8(base, at + static_cast<std::uint32_t>(i), text[i]);
        tox2Write8(base, at + static_cast<std::uint32_t>(text.size()), 0);
    }

    std::uint32_t makeGroup(std::uint32_t num) {
        tox2Write32(base, scratch + kLvGroupAttr, 8);
        tox2Write32(base, scratch + kLvGroupAttr + 4, scratch + kLvNames);
        tox2Write32(base, scratch + kLvGroupAttr + 8, 0x18);
        CHECK(ppu.call(170, scratch + kLvIdOut, num, 250, scratch + kLvGroupAttr) == 0);
        groups.push_back(tox2Read32(base, scratch + kLvIdOut));
        return groups.back();
    }

    // Initializes thread `index` of `group` at `entry` with $3 = arg0 (a 64-bit value).
    std::uint32_t makeThread(std::uint32_t group, std::uint32_t index, std::uint32_t entry, std::uint64_t arg0 = 0) {
        tox2Write32(base, scratch + kLvImage + 4, entry);
        tox2Write32(base, scratch + kLvThreadAttr, scratch + kLvNames + 0x80);
        tox2Write32(base, scratch + kLvThreadAttr + 4, 9);
        tox2Write32(base, scratch + kLvThreadAttr + 8, 2);
        tox2Write64(base, scratch + kLvArgs, arg0);
        for (std::uint32_t i = 1; i < 4; i++) tox2Write64(base, scratch + kLvArgs + 8 * i, 0);
        CHECK(ppu.call(172, scratch + kLvThreadOut + 4 * index, group, index, scratch + kLvImage,
                       scratch + kLvThreadAttr, scratch + kLvArgs) == 0);
        return tox2Read32(base, scratch + kLvThreadOut + 4 * index);
    }

    std::uint32_t makeQueue(std::uint32_t size = 8) {
        tox2Write32(base, scratch + kLvQueueAttr, 1);
        tox2Write32(base, scratch + kLvQueueAttr + 4, 1);
        CHECK(ppu.call(128, scratch + kLvQueueOut, scratch + kLvQueueAttr, 0, size) == 0);
        return tox2Read32(base, scratch + kLvQueueOut);
    }

    // sys_spu_thread_group_join into the scratch outputs.
    std::uint32_t join(std::uint32_t group, PpuCaller& caller) {
        return caller.call(178, group, scratch + kLvCauseOut, scratch + kLvStatusOut);
    }
    std::uint32_t join(std::uint32_t group) { return join(group, ppu); }
    std::uint32_t cause() const { return tox2Read32(base, scratch + kLvCauseOut); }
    std::uint32_t status() const { return tox2Read32(base, scratch + kLvStatusOut); }

    bool blockedIn(std::uint32_t thread_id, std::string_view what) {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        const tox2::SpuThread* thread = tox2::spuThreadById(thread_id);
        return thread != nullptr && thread->state() == tox2::SpuState::Blocked && thread->blockedIn() != nullptr &&
               std::string_view(thread->blockedIn()).contains(what);
    }

    bool waitUntilBlocked(std::uint32_t thread_id, std::string_view what) {
        return waitUntil([&] { return blockedIn(thread_id, what); });
    }

    std::uint32_t readLs(std::uint32_t thread_id, std::uint32_t lsa, std::uint32_t type, std::uint64_t* value) {
        const std::uint32_t code = ppu.call(182, thread_id, lsa, scratch + kLvValueOut, type);
        *value = tox2Read64(base, scratch + kLvValueOut);
        return code;
    }
};

constexpr std::uint32_t kErrFault = tox2::kCellEfault;

void testLv2GroupRun() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kLvMain, f.scratch + kLvWord);
    CHECK(thread == 0x01000100);
    const std::uint32_t queue = f.makeQueue();
    CHECK(f.ppu.call(191, thread, queue, 1, kLvPort) == 0);

    // Before the first start there is no SPU thread to reach.
    std::uint64_t value = 0;
    CHECK(f.readLs(thread, kLvBuf, 4, &value) == tox2::kCellEstat);
    CHECK(f.ppu.call(190, thread, 1) == tox2::kCellEstat);
    CHECK(f.ppu.call(181, thread, kLvBuf, 1, 4) == tox2::kCellEstat);
    CHECK(tox2::spuThreadById(thread) == nullptr);

    for (std::uint32_t run = 0; run < 2; run++) {
        const std::uint32_t word = 41 + run;
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(countLogs("started at lv2test ls 0x03000") == static_cast<int>(run + 1));

        // The event arrives while the PPU side waits for it.
        CHECK(f.ppu.call(130, queue, 0, 5000000) == 0);
        const Tox2PpuContext& ctx = *f.ppu.ctx;
        CHECK(ctx.gpr[4] == 0xFFFFFFFF53505501ull);
        CHECK(ctx.gpr[5] == thread);
        CHECK(ctx.gpr[6] == ((std::uint64_t{kLvPort} << 32) | kLvEventValue));
        CHECK(ctx.gpr[7] == word + 1);

        CHECK(f.join(group) == 0);
        CHECK(f.cause() == 2 && f.status() == kLvMainStatus);
        CHECK(tox2Read32(f.base, f.scratch + kLvWord) == word + 1);
        CHECK(countLogs("stopped: cause 2, status 0x2b") == static_cast<int>(run + 1));
    }

    // The thread stays reachable after the run: LS reads and writes.
    CHECK(f.readLs(thread, kLvBuf, 4, &value) == 0 && value == 43);
    CHECK(f.readLs(thread, kLvDataLs + 0x40, 4, &value) == 0 && value == kLvFillValue);
    CHECK(f.readLs(thread, kLvDataLs + 0x41, 1, &value) == 0 && value == 0xa5);
    CHECK(f.readLs(thread, kLvDataLs + 0x100, 4, &value) == 0 && value == 0);  // past the fill segment
    CHECK(f.ppu.call(181, thread, kLvDataLs + 0x80, 0x1122334455667788ull, 8) == 0);
    CHECK(f.readLs(thread, kLvDataLs + 0x80, 8, &value) == 0 && value == 0x1122334455667788ull);
    CHECK(f.readLs(thread, kLvDataLs + 0x80, 4, &value) == 0 && value == 0x11223344);
    CHECK(f.readLs(thread, kLvDataLs + 0x84, 2, &value) == 0 && value == 0x5566);
    CHECK(f.readLs(thread, kLvDataLs + 0x87, 1, &value) == 0 && value == 0x88);
    CHECK(f.ppu.call(181, thread, kLvDataLs + 0x90, 0xfffffffff00dull, 2) == 0);
    CHECK(f.readLs(thread, kLvDataLs + 0x90, 4, &value) == 0 && value == 0xf00da5a5);
    CHECK(f.readLs(thread, kLvDataLs + 0x81, 4, &value) == tox2::kCellEinval);  // misaligned
    CHECK(f.readLs(thread, kLvDataLs, 3, &value) == tox2::kCellEinval);
    CHECK(f.readLs(thread, 0x3fffe, 4, &value) == tox2::kCellEinval);  // beyond the local store
    CHECK(f.readLs(thread, 0x40000, 1, &value) == tox2::kCellEinval);
    CHECK(f.readLs(thread, 0xFFFFFFFC, 4, &value) == tox2::kCellEinval);  // lsa + type wraps in 32 bits
    CHECK(f.ppu.call(181, thread, 0xFFFFFFFC, 0, 4) == tox2::kCellEinval);
    CHECK(f.readLs(0x01000999, kLvBuf, 4, &value) == tox2::kCellEsrch);
    CHECK(f.ppu.call(182, thread, kLvBuf, 0, 4) == kErrFault);
    CHECK(f.ppu.call(181, thread, kLvDataLs, 0, 5) == tox2::kCellEinval);
    CHECK(f.ppu.call(181, 0x01000999, kLvDataLs, 0, 4) == tox2::kCellEsrch);

    // Writing over code removes its image tag.
    {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        CHECK(trackedAs(*tox2::spuThreadById(thread), kLvMain, "lv2test"));
        CHECK(trackedAs(*tox2::spuThreadById(thread), kLvMain + 0xff0, "lv2test"));
    }
    CHECK(f.ppu.call(181, thread, kLvMain + 0x100, 0, 4) == 0);
    {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        CHECK(untracked(*tox2::spuThreadById(thread), kLvMain + 0x100));
        CHECK(trackedAs(*tox2::spuThreadById(thread), kLvMain, "lv2test"));
        CHECK(trackedAs(*tox2::spuThreadById(thread), kLvMain + 0x110, "lv2test"));
    }

    // Join takes the result: the next one waits for another run. A destroyed group is gone.
    CHECK(f.ppu.call(171, group) == 0);
    CHECK(tox2::spuThreadById(thread) == nullptr);
    CHECK(f.join(group) == tox2::kCellEsrch);
}

void testLv2GroupExit() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(2);
    const std::uint32_t waiting = f.makeThread(group, 0, kLvWaitMbox);
    f.makeThread(group, 1, kLvExitGroup, 0x77);
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(f.join(group) == 0);
    CHECK(f.cause() == 1 && f.status() == 0x77);
    CHECK(countLogs("stopped: cause 1, status 0x77") == 1);
    // The thread that was blocked in RdInMbox was halted by the group exit.
    std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
    CHECK(tox2::spuThreadById(waiting)->state() == tox2::SpuState::Finished);
}

void testLv2Terminate() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(2);
    const std::uint32_t first = f.makeThread(group, 0, kLvWaitMbox);
    const std::uint32_t second = f.makeThread(group, 1, kLvWaitMbox);
    CHECK(f.ppu.call(177, group, 1) == tox2::kCellEstat);  // not running
    CHECK(f.ppu.call(177, 0x04000900, 1) == tox2::kCellEsrch);
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(f.waitUntilBlocked(first, "RdInMbox") && f.waitUntilBlocked(second, "RdInMbox"));
    CHECK(f.ppu.call(171, group) == tox2::kCellEbusy);  // running
    CHECK(f.ppu.call(173, group) == tox2::kCellEstat);

    // Event queues are attached while the group runs (libsre does this).
    const std::uint32_t queue = f.makeQueue();
    const std::uint32_t spup = f.scratch + 0x1c0;
    tox2Write8(f.base, spup, 0xee);
    CHECK(f.ppu.call(251, group, queue, 0x4, spup) == 0 && tox2Read8(f.base, spup) == 2);

    CHECK(f.ppu.call(177, group, 0x55) == 0);
    CHECK(f.join(group) == 0);
    CHECK(f.cause() == 4 && f.status() == 0x55);
    CHECK(f.ppu.call(177, group, 0x55) == tox2::kCellEstat);  // stopped again

    // The group can run again after a terminate. A null cause pointer is an error only after the wait.
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(f.waitUntilBlocked(first, "RdInMbox"));
    tox2Write32(f.base, f.scratch + kLvStatusOut, 0);
    PpuCaller joiner;
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> result{0};
    std::thread helper([&] {
        result = joiner.call(178, group, 0, f.scratch + kLvStatusOut);
        done = true;
    });
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(!done);
    CHECK(f.ppu.call(177, group, 9) == 0);
    helper.join();
    CHECK(result == kErrFault);
    CHECK(f.status() == 9);  // the valid pointer is still written
    CHECK(f.ppu.call(171, group) == 0);
}

void testLv2JoinBeforeStart() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(1);
    f.makeThread(group, 0, kLvExitGroup, 0x31);

    PpuCaller first, second;
    std::atomic<bool> first_done{false}, second_done{false};
    std::atomic<std::uint32_t> first_result{0}, second_result{0};
    std::thread a([&] {
        first_result = f.join(group, first);
        first_done = true;
    });
    CHECK(waitUntil([&] {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        return tox2::spuGroupHasJoiner(group);
    }));
    std::thread b([&] {
        second_result = second.call(178, group, 0, 0);
        second_done = true;
    });
    // A second joiner is refused while the first one waits.
    CHECK(waitUntil([&] { return second_done.load(); }));
    CHECK(second_result == tox2::kCellEbusy);
    CHECK(!first_done);
    CHECK(f.ppu.call(171, group) == tox2::kCellEbusy);  // a joiner is waiting

    CHECK(f.ppu.call(173, group) == 0);
    a.join();
    b.join();
    CHECK(first_result == 0 && f.cause() == 1 && f.status() == 0x31);

    // The result was taken; a join after the next run returns at once.
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(waitUntil([&] { return countLogs("stopped: cause 1") == 2; }));
    CHECK(f.join(group) == 0 && f.cause() == 1);
}

void testLv2Mailbox() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kLvCollect);
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(f.waitUntilBlocked(thread, "WrOutMbox"));
    CHECK(f.ppu.call(190, 0x01000999, 1) == tox2::kCellEsrch);
    for (std::uint32_t value = 1; value <= 6; value++) CHECK(f.ppu.call(190, thread, value) == 0);
    // Four fit; the fifth and sixth replace the newest entry. One warning for the thread.
    CHECK(countLogs("inbound mailbox is full") == 1);
    {
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        tox2::SpuChannels& channels = tox2::spuThreadById(thread)->channels();
        CHECK(channels.inMboxCount() == 4);
        std::uint32_t out = 0;
        CHECK(channels.takeOutMbox(&out) && out == 0xaa);
    }
    CHECK(f.waitUntilBlocked(thread, "RdInMbox"));
    const std::uint32_t expected[] = {1, 2, 3, 6};
    for (std::uint32_t i = 0; i < 4; i++) {
        std::uint64_t value = 0;
        CHECK(f.readLs(thread, kLvBuf + 4 * i, 4, &value) == 0 && value == expected[i]);
    }
    CHECK(f.ppu.call(177, group, 0) == 0);
    CHECK(f.join(group) == 0 && f.cause() == 4);
}

void testLv2InterruptMailbox() {
    // The event queue is full: the SPU thread waits, gets its second event in once there is room, and the
    // wait ends at once when the group is terminated.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        const std::uint32_t thread = f.makeThread(group, 0, kLvThrowTwo);
        const std::uint32_t queue = f.makeQueue(1);
        CHECK(f.ppu.call(191, thread, queue, 1, kLvPort) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "owner wait"));
        CHECK(f.ppu.call(130, queue, 0, 5000000) == 0 && f.ppu.ctx->gpr[7] == 0x11);
        CHECK(f.ppu.call(130, queue, 0, 5000000) == 0 && f.ppu.ctx->gpr[7] == 0x22);
        CHECK(f.ppu.ctx->gpr[6] == ((std::uint64_t{kLvPort} << 32) | 2));
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 5);

        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "owner wait"));
        CHECK(f.ppu.call(177, group, 3) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 4 && f.status() == 3);
    }

    // send_event answers in the inbound mailbox: OK for a bound port, ENOTCONN for an unbound one.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        const std::uint32_t thread = f.makeThread(group, 0, kLvSendEvent);
        const std::uint32_t queue = f.makeQueue();
        CHECK(f.ppu.call(191, thread, queue, 1, kLvPort) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == tox2::kCellEnotconn);
        CHECK(f.ppu.call(130, queue, 0, 5000000) == 0);
        CHECK(f.ppu.ctx->gpr[5] == thread);
        CHECK(f.ppu.ctx->gpr[6] == ((std::uint64_t{kLvPort} << 32) | 0xabc));
        CHECK(f.ppu.ctx->gpr[7] == 0x99);
    }

    // Code 128 sets a bit of an event flag; bit 64 is invalid.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        tox2Write32(f.base, f.scratch + kLvFlagAttr, 1);
        tox2Write32(f.base, f.scratch + kLvFlagAttr + 20, 0x10000);
        CHECK(f.ppu.call(82, f.scratch + kLvFlagOut, f.scratch + kLvFlagAttr, 0) == 0);
        const std::uint32_t flag = tox2Read32(f.base, f.scratch + kLvFlagOut);
        f.makeThread(group, 0, kLvFlag, flag);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == tox2::kCellEinval);
        CHECK(f.ppu.call(139, flag, f.scratch + kLvFlagsOut) == 0);
        CHECK(tox2Read64(f.base, f.scratch + kLvFlagsOut) == 0x20);
    }

    // An empty outbound mailbox at the exit codes gives status 0 and a warning; 0x100 goes on.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        f.makeThread(group, 0, kLvEmptyExit);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 0);
        CHECK(countLogs("with an empty outbound mailbox, status 0") == 1);
    }
    // A send_event with a full inbound mailbox answers EBUSY as the only entry, and sends nothing.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        const std::uint32_t thread = f.makeThread(group, 0, kLvSendBusy);
        const std::uint32_t queue = f.makeQueue();
        CHECK(f.ppu.call(191, thread, queue, 1, kLvPort) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "WrOutMbox"));
        for (std::uint32_t value = 1; value <= 4; value++) CHECK(f.ppu.call(190, thread, value) == 0);
        {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            std::uint32_t out = 0;
            CHECK(tox2::spuThreadById(thread)->channels().takeOutMbox(&out) && out == 0xaa);
        }
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == tox2::kCellEbusy);
        CHECK(f.ppu.call(130, queue, 0, 2000) == tox2::kCellEtimedout);
    }
}

void testLv2Teardown() {
    // A fixture that goes away with a running group ends it.
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(2);
    f.makeThread(group, 0, kLvWaitMbox);
    f.makeThread(group, 1, kLvWaitMbox);
    CHECK(f.ppu.call(173, group) == 0);
    tox2::resetSpuThreadGroups();
    CHECK(f.ppu.call(177, group, 0) == tox2::kCellEsrch);
}

// Children: each ends in a stop line that the parent checks.
int runLv2Child(std::string_view mode) {
    tox2::installProcessSettings();
    Lv2Fixture f(false);  // the log goes to the child's stderr
    const std::uint32_t group = f.makeGroup(1);
    if (mode == "--lv2-badstop-child") {
        f.makeThread(group, 0, kLvBadStop);
    } else if (mode == "--lv2-intr-empty-child") {
        f.makeThread(group, 0, kLvIntrEmpty);
    } else if (mode == "--lv2-intr-code-child") {
        f.makeThread(group, 0, kLvIntrBadCode);
    } else if (mode == "--lv2-fatal-child") {
        // The game installs the census printer; the child does it to see the SPU section.
        tox2::installCensus();
        f.makeThread(group, 0, kLvFatal, 0x1234);
    } else if (mode == "--lv2-fault-child") {
        tox2::installFaultHandler();
        f.makeThread(group, 0, kLvFault);
    } else {
        return 99;
    }
    f.ppu.call(173, group);
    f.join(group);  // the stop ends the process
    return 0;
}

void testLv2StopChildren() {
    struct Case {
        const wchar_t* arg;
        const char* line;
    };
    const Case cases[] = {
            {L"--lv2-badstop-child",
             "tox2: stop: spu: stop 0x1234 at ls 0x03704 on thread 0x01000100: not implemented"},
            {L"--lv2-intr-empty-child",
             "tox2: stop: spu: WrOutIntrMbox 0x40000000 on thread 0x01000100 with an empty outbound mailbox"},
            {L"--lv2-intr-code-child",
             "tox2: stop: spu: WrOutIntrMbox 0x90000000 on thread 0x01000100: code 144 is not implemented"},
    };
    for (const Case& c : cases) {
        const ChildResult r = runChild(c.arg);
        const bool found = r.output.find(c.line) != std::string::npos;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(found);
        CHECK(r.output.find("trace (oldest first)") != std::string::npos);
        if (!found) std::fprintf(stderr, "child output:\n%s\n", r.output.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// Observability: log prefix, fatal dump, supervisor view, census.
// ---------------------------------------------------------------------------------------------

// A line written on an SPU host thread carries the lv2 thread id; the PPU side keeps its own prefix.
void testSpuLogPrefix() {
    Lv2Fixture f;
    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kLvLog);
    CHECK(thread == 0x01000100);
    CHECK(f.ppu.call(173, group) == 0);
    CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 7);
    CHECK(countLogs("[spu 01000100] tox2: test: hello from spu") == 1);
    // The owner's callbacks run on the same host thread.
    CHECK(countLogs("[spu 01000100] tox2: spu: group") == 1);
    CHECK(countLogs("[host] tox2: spu: thread 0x01000100 \"lvthread\" started at lv2test") == 1);
    tox2::logInfo("tox2: test: back on the host");
    CHECK(countLogs("[host] tox2: test: back on the host") == 1);
}

// Starts a thread that stops at an address no image covers, so the stop and the census come from a host thread.
int runHostUnknownChild() {
    tox2::installProcessSettings();
    tox2::installCensus();
    registerSynthImages();
    const auto guest = makeGuest();
    tox2::SpuThread thread(0x01000109, "unknown", nullptr);
    loadSynthImages(thread, guest);
    TOX2_VR_W(thread.context().gpr[4], 0) = 0x6000;
    thread.start(kLsGoto);
    thread.join();
    return 0;  // not reached: the stop ends the process
}

void testSpuFatalChildren() {
    // fatal() (not spuStop) on an SPU thread: the SPU dump, no PPU dump, the census with the SPU section.
    {
        const ChildResult r = runChild(L"--lv2-fatal-child");
        const std::string& out = r.output;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(out.contains("[spu 01000100] tox2: stop: fatal guest fault: spu test fault"));
        CHECK(out.contains("spu thread 0x01000100 \"lvthread\": jump_target 0x03d00"));
        CHECK(out.contains("trace (oldest first): 03d00"));
        CHECK(out.contains("registers (non-zero, words 0..3):"));
        CHECK(out.contains("$3   00000000 00001234"));
        CHECK(out.contains("ls map:") && out.contains("lv2test segment 0 delta 0"));
        CHECK(!out.contains("guest stack:") && !out.contains("  r0  ") && !out.contains("  lr "));
        CHECK(out.contains("census: spu 1 threads, 1 dispatches, 0 unknown targets"));
        CHECK(out.contains("  thread 0x01000100 lvthread: running lv2test ls 0x03d00"));
        if (!out.contains("trace (oldest first): 03d00")) std::fprintf(stderr, "child output:\n%s\n", out.c_str());
    }
    // A guest fault inside an MFC transfer reaches fatal() from the fault handler on the same host thread.
    {
        const ChildResult r = runChild(L"--lv2-fault-child");
        const std::string& out = r.output;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(out.contains("[spu 01000100] tox2: stop: fatal guest fault: read from guest address 0x30000000"));
        CHECK(out.contains("spu thread 0x01000100 \"lvthread\": jump_target 0x03e00"));
        CHECK(out.contains("trace (oldest first): 03e00"));
        CHECK(out.contains("registers (non-zero, words 0..3):") && out.contains("ls map:"));
        CHECK(!out.contains("guest stack:") && !out.contains("  r0  ") && !out.contains("  lr "));
        if (!out.contains("trace (oldest first): 03e00")) std::fprintf(stderr, "child output:\n%s\n", out.c_str());
    }
    // An unknown target counts in the census of the same stop.
    {
        const ChildResult r = runChild(L"--spu-host-unknown-child");
        const std::string& out = r.output;
        CHECK(r.exit_code == static_cast<DWORD>(tox2::ExitCode::Fatal));
        CHECK(
                out.contains("[spu 01000109] tox2: stop: spu: unknown target ls 0x06000 from ls 0x03024 on thread "
                             "0x01000109 (untracked)"));
        CHECK(out.contains("census: spu 1 threads, 1 dispatches, 1 unknown targets"));
        CHECK(out.contains("  image synth: 1 dispatches, 1 distinct entries"));
        if (!out.contains("1 unknown targets")) std::fprintf(stderr, "child output:\n%s\n", out.c_str());
    }
}

// The supervisor input has one entry per started SPU thread; a PPU wait on SPU work is a stall only when the SPU
// threads wait as well.
void testSpuSupervisor() {
    using tox2::ThreadHealth;
    using tox2::ThreadState;
    using tox2::ThreadVerdict;
    const ThreadHealth ppu_waiting{ThreadState::Blocked, true, 0, false};

    // A running SPU thread keeps the run alive however long the PPU threads have waited.
    {
        registerSynthImages();
        const auto guest = makeGuest();
        tox2::SpuThread thread(0x01000120, "spin", nullptr);
        loadSynthImages(thread, guest);
        g_spinning = false;
        CHECK(thread.start(kLsSpin));
        CHECK(waitUntil([] { return g_spinning.load(); }));
        std::vector<ThreadHealth> health{ppu_waiting, ppu_waiting};
        tox2::appendSpuHealth(health);
        CHECK(health.size() == 3);
        CHECK(health[2].state == ThreadState::Running && !health[2].service);
        std::size_t stalled = 0;
        CHECK(tox2::judgeThreads(health, 100000, &stalled) == ThreadVerdict::Ok && stalled == 2);
        thread.requestHalt();
        thread.join();
        // A finished thread is no longer part of the input.
        health = {ppu_waiting};
        tox2::appendSpuHealth(health);
        CHECK(health.size() == 1);
        CHECK(tox2::judgeThreads(health, 100000) == ThreadVerdict::Stall);
    }

    // An SPU thread blocked in an indefinite channel wait counts like a blocked PPU thread, after two seconds.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        const std::uint32_t thread = f.makeThread(group, 0, kLvWaitMbox);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "RdInMbox"));
        std::vector<ThreadHealth> health{ppu_waiting};
        tox2::appendSpuHealth(health);
        CHECK(health.size() == 2);
        CHECK(health[1].state == ThreadState::Blocked && health[1].indefinite_wait);
        const std::int64_t since = health[1].blocked_since_ms;
        CHECK(since > 0);
        std::size_t stalled = 0;
        CHECK(tox2::judgeThreads(health, since + 2500, &stalled) == ThreadVerdict::Stall && stalled == 2);
        CHECK(tox2::judgeThreads(health, since + 500, &stalled) == ThreadVerdict::Ok && stalled == 1);

        // The mailbox wakes it; the wait is over and so is the stall.
        CHECK(f.ppu.call(190, thread, 5) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 5);
        health = {ppu_waiting};
        tox2::appendSpuHealth(health);
        CHECK(health.size() == 1);
    }
}

// The census of a run that started twice: counters survive the fresh thread objects.
void testSpuCensus() {
    Lv2Fixture f;
    tox2::resetCensus();
    CHECK(!tox2::formatCensus().contains("census: spu"));

    const std::uint32_t group = f.makeGroup(1);
    const std::uint32_t thread = f.makeThread(group, 0, kLvMain, f.scratch + kLvWord);
    CHECK(thread == 0x01000100);
    for (std::uint32_t run = 0; run < 2; run++) {
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == kLvMainStatus);
    }
    const std::string text = tox2::formatCensus();
    const std::size_t at = text.find("census: spu");
    const std::string expected =
            "census: spu 1 threads, 2 dispatches, 0 unknown targets\n"
            "  thread 0x01000100 lvthread: finished lv2test ls 0x03000\n"
            "  image lv2test: 2 dispatches, 1 distinct entries\n"
            "  mfc get: 2, put: 2, getllar: 0, putllc: 0 ok 0 failed\n"
            "  channel rdch 24 MFC_RdTagStat: 4\n"
            "  stop 0x102: 2";
    // The section comes last, after the thread section.
    CHECK(at != std::string::npos && text.substr(at) == expected);
    CHECK(text.find("census: threads") < at);
    if (at == std::string::npos || text.substr(at) != expected) std::fprintf(stderr, "census:\n%s\n", text.c_str());

    // Forgetting the counters removes the section until the next start.
    tox2::resetCensus();
    CHECK(!tox2::formatCensus().contains("census: spu"));
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
    testChanReplaceInMbox();
    testChanHaltOnCondvar();
    testChanStopChildren();
    testLv2GroupRun();
    testLv2GroupExit();
    testLv2Terminate();
    testLv2JoinBeforeStart();
    testLv2Mailbox();
    testLv2InterruptMailbox();
    testLv2Teardown();
    testLv2StopChildren();
    testSpuLogPrefix();
    testSpuFatalChildren();
    testSpuSupervisor();
    testSpuCensus();

    std::printf("spu runtime tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

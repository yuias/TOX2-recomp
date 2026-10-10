// Self-checking unit tests for the SPU runtime pieces. No generated code and no guest address space is
// involved: tests address plain host buffers through the same base + ea convention.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/reservation.h"

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

// Live version of the same guarantee: a writer to chunk 5 racing with PUTLLCs that change chunk 0 never
// sees its value go backwards, because PUTLLC leaves chunk 5 alone or fails.
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

void testWaitersAreNotLost() {
    // Writers racing with a waiter that re-arms at once: no wait may run to its (long) timeout.
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint8_t zeros[16] = {};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        while (!done.load()) {
            table.put(mem.base(), 0, zeros, 16);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    bool timed_out = false;
    for (int i = 0; i < 300 && !timed_out; i++) {
        const std::uint64_t seen = table.storeEpoch();
        if (table.waitStoreEpoch(seen, nullptr, milliseconds(1000)) == tox2::StoreWait::Timeout) timed_out = true;
    }
    done = true;
    writer.join();
    CHECK(!timed_out);
}

void testSharedTable() {
    CHECK(&tox2::reservations() == &tox2::reservations());
    TestMemory mem;
    tox2::Reservation res;
    std::uint8_t ls[128];
    tox2::reservations().getllar(mem.base(), 0, ls, res);
    CHECK(tox2::reservations().putllc(mem.base(), 0, ls, res));
}

}  // namespace

int main() {
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
    testSharedTable();

    std::printf("spu runtime tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

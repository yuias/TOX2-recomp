// Self-checking unit tests for the SPU runtime pieces. Most tests address plain host buffers through the
// same base + ea convention; the lv2 group tests use the process's guest memory and the syscall table.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_priority.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/threads.h"
#include "core/timer_resolution.h"
#include "lv2/cell_error.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "spu/spu_census.h"
#include "spu/spu_capture.h"
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

    // A range over three lines is written in per-line pieces and bumps each stripe, but the epoch only once.
    const std::uint64_t epoch_before = table.storeEpoch();
    table.put(mem.base(), 64, src, 288);
    CHECK(std::memcmp(mem.bytes + 64, src, 288) == 0);
    CHECK(mem.bytes[63] == 0 && mem.bytes[352] == 0);
    CHECK(table.storeEpoch() == epoch_before + 1);

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

// put() against a memcpy reference over destination offsets and sizes, covering the 8-byte fast path of
// storeBytes and the narrower pieces. A put of bytes that are already there must change nothing but still
// invalidate a reservation on the line.
void testPutMatchesReference() {
    struct alignas(128) Buffer {
        std::uint8_t bytes[1024];
    };
    auto buffer = std::make_unique<Buffer>();
    std::uint8_t* const base = buffer->bytes;
    std::array<std::uint8_t, 1024> initial;
    std::array<std::uint8_t, 1024> src;
    for (std::uint32_t i = 0; i < initial.size(); i++) {
        initial[i] = static_cast<std::uint8_t>(i * 13 + 5);
        src[i] = static_cast<std::uint8_t>(i * 29 + 7);
    }

    tox2::ReservationTable table;
    std::vector<std::uint32_t> sizes = {1, 2, 4, 8};
    for (std::uint32_t size = 16; size <= 512; size += 16) sizes.push_back(size);
    int cases_run = 0;
    for (std::uint32_t offset = 0; offset < 16; offset++) {
        for (const std::uint32_t size : sizes) {
            // The small sizes are naturally aligned stores by contract.
            if (size <= 8 && (offset & (size - 1)) != 0) continue;
            cases_run++;
            std::memcpy(base, initial.data(), initial.size());
            std::array<std::uint8_t, 1024> expected = initial;
            std::memcpy(expected.data() + offset, src.data(), size);

            table.put(base, offset, src.data(), size);
            CHECK(std::memcmp(base, expected.data(), expected.size()) == 0);

            // The same bytes again: memory stays as it is, the reservation on the first line is lost.
            tox2::Reservation res;
            std::uint8_t ls[128];
            table.getllar(base, offset, ls, res);
            CHECK(!table.lost(base, res));
            table.put(base, offset, src.data(), size);
            CHECK(std::memcmp(base, expected.data(), expected.size()) == 0);
            CHECK(table.lost(base, res));
        }
    }
    CHECK(cases_run > 0);
}

// One 16 KB MFC PUT spans 128 lines; the epoch and the publish count must move once, not per line.
void testPutPublishesOnce() {
    constexpr std::uint32_t kSize = 0x4000;
    constexpr std::uint32_t kLine100 = 99 * tox2::kReservationLineSize;
    struct alignas(128) Block {
        std::uint8_t bytes[kSize] = {};
    };
    auto block = std::make_unique<Block>();
    std::uint8_t* base = block->bytes;
    const std::vector<std::uint8_t> src(kSize, 0x5a);

    tox2::ReservationTable table;
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(base, kLine100, ls, res);
    const std::uint64_t epoch_before = table.storeEpoch();
    const std::uint64_t publishes_before = table.stats().publishes;
    table.put(base, 0, src.data(), kSize);
    CHECK(table.storeEpoch() == epoch_before + 1);
    CHECK(table.stats().publishes == publishes_before + 1);
    CHECK(table.lost(base, res));
    CHECK(std::memcmp(base, src.data(), kSize) == 0);

    // An empty PUT writes nothing and publishes nothing.
    table.put(base, 0, src.data(), 0);
    CHECK(table.storeEpoch() == epoch_before + 1);
    CHECK(table.stats().publishes == publishes_before + 1);

    // The single-line writers publish once each.
    table.putlluc(base, 0, src.data());
    CHECK(table.stats().publishes == publishes_before + 2);
    table.getllar(base, 0, ls, res);
    std::uint8_t changed[128];
    std::memset(changed, 0x11, sizeof(changed));
    CHECK(table.putllc(base, 0, changed, res));
    CHECK(table.stats().publishes == publishes_before + 3);
}

// Stripe version of the line that contains `ea`, as a PPU lwarx would record it.
std::uint64_t stripeVersion(tox2::ReservationTable& table, std::uint8_t* base, std::uint32_t ea) {
    std::uint64_t value, version;
    table.loadReserved(base, ea, 4, value, version);
    return version;
}

void testStoreConditional() {
    tox2::ReservationTable table;
    TestMemory mem;
    storeBe32(mem.bytes + 4, 10);
    CHECK(table.storeConditional(mem.base(), 4, 4, 10, stripeVersion(table, mem.base(), 4), 11));
    CHECK(loadBe32(mem.bytes + 4) == 11);
    // value changed since the load
    CHECK(!table.storeConditional(mem.base(), 4, 4, 10, stripeVersion(table, mem.base(), 4), 12));
    CHECK(loadBe32(mem.bytes + 4) == 11);

    const std::uint64_t big = 0x0102030405060708ull;
    const std::uint64_t swapped = std::byteswap(big);
    std::memcpy(mem.bytes + 16, &swapped, 8);
    CHECK(mem.bytes[16] == 1 && mem.bytes[23] == 8);
    CHECK(table.storeConditional(mem.base(), 16, 8, big, stripeVersion(table, mem.base(), 16), 0x1122334455667788ull));
    CHECK(mem.bytes[16] == 0x11 && mem.bytes[23] == 0x88);
    CHECK(!table.storeConditional(mem.base(), 16, 8, big, stripeVersion(table, mem.base(), 16), 1));
    CHECK(!table.storeConditional(mem.base(), 16, 2, 0, stripeVersion(table, mem.base(), 16), 0));  // bad width

    // A successful store-conditional invalidates an SPU reservation on the line; a failed one does not.
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(mem.base(), 0, ls, res);
    CHECK(!table.storeConditional(mem.base(), 4, 4, 99, stripeVersion(table, mem.base(), 4), 100));
    CHECK(!table.lost(mem.base(), res));
    CHECK(table.storeConditional(mem.base(), 4, 4, 11, stripeVersion(table, mem.base(), 4), 100));
    CHECK(table.lost(mem.base(), res));
}

// A PPU reservation is line-granular: a reservation-aware write to any word of the line since the load
// fails the store-conditional, a write to another line does not.
void testPpuReservationIsLineGranular() {
    tox2::ReservationTable table;
    TestMemory mem;
    constexpr std::uint32_t kWord = 128 + 0x40;    // lwarx target
    constexpr std::uint32_t kNeighbour = 128 + 8;  // another 8-byte chunk of the same line
    storeBe32(mem.bytes + kWord, 5);
    std::uint64_t value, version;

    // PUTLLC that changes only another chunk of the line: the word itself is intact, the version moved.
    table.loadReserved(mem.base(), kWord, 4, value, version);
    CHECK(value == 5);
    tox2::Reservation res;
    std::uint8_t ls[128];
    table.getllar(mem.base(), 128, ls, res);
    storeBe32(ls + (kNeighbour - 128), 0xabcd);
    CHECK(table.putllc(mem.base(), 128, ls, res));
    CHECK(loadBe32(mem.bytes + kWord) == 5);
    CHECK(!table.storeConditional(mem.base(), kWord, 4, value, version, 6));
    CHECK(loadBe32(mem.bytes + kWord) == 5);

    // A fresh load takes the new version and succeeds.
    table.loadReserved(mem.base(), kWord, 4, value, version);
    CHECK(table.storeConditional(mem.base(), kWord, 4, value, version, 6));
    CHECK(loadBe32(mem.bytes + kWord) == 6);

    // PUTLLC on a different line (a different stripe) does not disturb the reservation.
    table.loadReserved(mem.base(), kWord, 4, value, version);
    table.getllar(mem.base(), 256, ls, res);
    storeBe32(ls, 1);
    CHECK(table.putllc(mem.base(), 256, ls, res));
    CHECK(table.storeConditional(mem.base(), kWord, 4, value, version, 7));
    CHECK(loadBe32(mem.bytes + kWord) == 7);

    // PUTLLUC and PUT on the line also fail it; so does another store-conditional.
    table.loadReserved(mem.base(), kWord, 4, value, version);
    table.putlluc(mem.base(), 128, ls);
    CHECK(!table.storeConditional(mem.base(), kWord, 4, value, version, 8));
    storeBe32(ls, 2);
    table.loadReserved(mem.base(), kWord, 4, value, version);
    table.put(mem.base(), kNeighbour, ls, 4);
    CHECK(!table.storeConditional(mem.base(), kWord, 4, value, version, 8));
    table.loadReserved(mem.base(), kWord, 4, value, version);
    CHECK(table.storeConditional(mem.base(), kNeighbour, 4, 2, stripeVersion(table, mem.base(), kNeighbour), 3));
    CHECK(!table.storeConditional(mem.base(), kWord, 4, value, version, 8));

    // The 8-byte variant (ldarx/stdcx.) follows the same rule.
    constexpr std::uint32_t kDouble = 128 + 0x48;
    const std::uint64_t pattern = std::byteswap(0x0102030405060708ull);
    std::memcpy(mem.bytes + kDouble, &pattern, 8);
    table.loadReserved(mem.base(), kDouble, 8, value, version);
    CHECK(value == 0x0102030405060708ull);
    table.getllar(mem.base(), 128, ls, res);
    storeBe32(ls + 0x70, 9);
    CHECK(table.putllc(mem.base(), 128, ls, res));
    CHECK(!table.storeConditional(mem.base(), kDouble, 8, value, version, 1));
    table.loadReserved(mem.base(), kDouble, 8, value, version);
    CHECK(table.storeConditional(mem.base(), kDouble, 8, value, version, 0x1122334455667788ull));
    CHECK(mem.bytes[kDouble] == 0x11 && mem.bytes[kDouble + 7] == 0x88);

    // A plain store to the word itself is still caught by the value compare.
    table.loadReserved(mem.base(), kWord, 4, value, version);
    storeBe32(mem.bytes + kWord, 77);
    CHECK(!table.storeConditional(mem.base(), kWord, 4, value, version, 8));
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
                std::uint64_t seen, version;
                table.loadReserved(mem.base(), kPpuWord, 4, seen, version);
                if (table.storeConditional(mem.base(), kPpuWord, 4, seen, version, seen + 1)) break;
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

// A PUTLLC that parks inside its critical section, after the snapshot compare and before the chunk stores.
struct ParkedPutllc {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void parkHook(void* arg) {
    auto* park = static_cast<ParkedPutllc*>(arg);
    park->entered.store(true);
    while (!park->release.load()) std::this_thread::sleep_for(milliseconds(1));
}

void testDescribeLoss() {
    tox2::ReservationTable table;
    TestMemory mem;
    constexpr std::uint32_t kLine = 128;
    tox2::Reservation res;
    alignas(16) std::uint8_t ls[128];

    // Nothing written.
    table.getllar(mem.base(), kLine, ls, res);
    CHECK(!table.lost(mem.base(), res) && !table.versionMoved(res) && !table.contentDiffers(mem.base(), res));
    tox2::ReservationLoss loss = table.describeLoss(mem.base(), res);
    CHECK(!loss.version_moved && loss.changed_chunks == 0);

    // A plain byte store at +0x72 moves neither the version nor the epoch, but changes chunk 14.
    plainStore8(mem.bytes + kLine + 0x72, 0xff);
    CHECK(table.lost(mem.base(), res) && !table.versionMoved(res) && table.contentDiffers(mem.base(), res));
    loss = table.describeLoss(mem.base(), res);
    CHECK(!loss.version_moved && loss.changed_chunks == 1u << 14);
    CHECK(loss.now[14] == chunkAt(mem.bytes + kLine + 0x70) && ((loss.now[14] >> 16) & 0xff) == 0xff);
    CHECK(loss.now[0] == chunkAt(mem.bytes + kLine));

    // A table write of the bytes that are already there moves the version only.
    table.getllar(mem.base(), kLine, ls, res);
    table.put(mem.base(), kLine + 0x70, mem.bytes + kLine + 0x70, 8);
    CHECK(table.lost(mem.base(), res) && table.versionMoved(res) && !table.contentDiffers(mem.base(), res));
    loss = table.describeLoss(mem.base(), res);
    CHECK(loss.version_moved && loss.changed_chunks == 0);

    // A reservation that is not valid is never lost.
    const tox2::Reservation none;
    CHECK(!table.lost(mem.base(), none) && !table.versionMoved(none) && !table.contentDiffers(mem.base(), none));
    loss = table.describeLoss(mem.base(), none);
    CHECK(!loss.version_moved && loss.changed_chunks == 0);

    // A PUTLLC that has changed the line but not yet released the stripe must not be reported as a plain store:
    // describeLoss waits for it and then sees the version move.
    ParkedPutllc park;
    tox2::Reservation writer_res, reader_res;
    table.getllar(mem.base(), kLine, ls, writer_res);
    table.getllar(mem.base(), kLine, ls, reader_res);
    ls[0] ^= 0x5a;
    table.setCompareHook(&parkHook, &park);
    std::thread writer([&] { CHECK(table.putllc(mem.base(), kLine, ls, writer_res)); });
    while (!park.entered.load()) std::this_thread::sleep_for(milliseconds(1));
    std::atomic<bool> returned{false};
    tox2::ReservationLoss waited;
    std::thread reader([&] {
        waited = table.describeLoss(mem.base(), reader_res);
        returned.store(true);
    });
    std::this_thread::sleep_for(milliseconds(200));
    CHECK(!returned.load());
    park.release.store(true);
    writer.join();
    reader.join();
    table.setCompareHook(nullptr, nullptr);
    CHECK(returned.load() && waited.version_moved);
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

void testStoreWaitReportsSleep() {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint32_t halt = 0;
    std::uint8_t zeros[16] = {};
    // Starts true so a missing write of the flag shows up.
    bool slept = true;

    // The epoch already moved: returns before sleeping.
    std::uint64_t seen = table.storeEpoch();
    CHECK(table.waitStoreEpoch(seen - 1, &halt, milliseconds(3000), tox2::kWatchAnyLine, &slept) ==
          tox2::StoreWait::Epoch);
    CHECK(!slept);

    // A flag that is already set.
    slept = true;
    __atomic_store_n(&halt, 1u, __ATOMIC_RELEASE);
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(3000), tox2::kWatchAnyLine, &slept) == tox2::StoreWait::Halt);
    CHECK(!slept);
    __atomic_store_n(&halt, 0u, __ATOMIC_RELEASE);

    // A zero timeout never waits.
    slept = true;
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(0), tox2::kWatchAnyLine, &slept) == tox2::StoreWait::Timeout);
    CHECK(!slept);

    // Nothing happens: the wait sleeps until its deadline.
    slept = false;
    CHECK(table.waitStoreEpoch(seen, &halt, milliseconds(20), tox2::kWatchAnyLine, &slept) == tox2::StoreWait::Timeout);
    CHECK(slept);

    // A write to the watched line ends a sleep that started after `seen` was read.
    seen = table.storeEpoch();
    std::atomic<bool> started{false};
    tox2::StoreWait result = tox2::StoreWait::Halt;
    bool waiter_slept = false;
    Clock::time_point finished;
    std::thread waiter([&] {
        started.store(true);
        result = table.waitStoreEpoch(seen, &halt, milliseconds(10000), 0, &waiter_slept);
        finished = Clock::now();
    });
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(milliseconds(200));
    const auto put_time = Clock::now();
    table.put(mem.base(), 0, zeros, 16);
    waiter.join();
    CHECK(result == tox2::StoreWait::Epoch);
    CHECK(waiter_slept);
    CHECK(finished - put_time < milliseconds(2000));

    // The slept pointer is optional.
    CHECK(table.waitStoreEpoch(table.storeEpoch() - 1, &halt, milliseconds(3000)) == tox2::StoreWait::Epoch);
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

// A waiter that names a line is only woken by writes to that line's stripe, a wildcard by every epoch change, and
// everybody by wake(). The timeouts are long so that a missed wake-up shows as a Timeout result.
void testUnrelatedPutDoesNotNotify() {
    // kWatched and kSameStripe are one stripe count of lines apart, so they share a stripe.
    constexpr std::uint32_t kWatched = 0x1000;
    constexpr std::uint32_t kUnrelated = 0x8000;
    constexpr std::uint32_t kSameStripe = kWatched + tox2::kReservationStripeCount * tox2::kReservationLineSize;
    struct alignas(128) Block {
        std::uint8_t bytes[kSameStripe + tox2::kReservationLineSize] = {};
    };
    auto block = std::make_unique<Block>();
    std::uint8_t* base = block->bytes;
    const std::vector<std::uint8_t> src(0x1000, 0x5a);

    tox2::ReservationTable table;
    constexpr int kPending = -1;
    // Waits on its own thread; `result` holds the StoreWait once it returns.
    struct Waiter {
        std::atomic<int> result{kPending};
        std::thread thread;
    };
    const auto start = [&](Waiter& w, std::uint64_t seen, std::uint32_t line, const std::uint32_t* halt = nullptr) {
        w.thread = std::thread([&table, &w, seen, line, halt] {
            w.result.store(static_cast<int>(table.waitStoreEpoch(seen, halt, milliseconds(3000), line)));
        });
    };
    const auto settle = [] { std::this_thread::sleep_for(milliseconds(100)); };

    // Writes elsewhere move the epoch and leave the notify counter alone; a write to the line wakes the waiter.
    {
        Waiter w;
        start(w, table.storeEpoch(), kWatched);
        settle();
        const std::uint64_t notifies_before = table.stats().notifies;
        for (int i = 0; i < 1000; i++) table.put(base, kUnrelated, src.data(), 16);
        CHECK(table.stats().notifies == notifies_before);
        CHECK(w.result.load() == kPending);
        const auto put_at = Clock::now();
        table.put(base, kWatched, src.data(), 16);
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
        CHECK(Clock::now() - put_at < milliseconds(100));
        CHECK(table.stats().notifies == notifies_before + 1);
    }

    // A wildcard waiter is woken by a write to any line.
    {
        Waiter w;
        start(w, table.storeEpoch(), tox2::kWatchAnyLine);
        settle();
        const std::uint64_t notifies_before = table.stats().notifies;
        table.put(base, kUnrelated, src.data(), 16);
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
        CHECK(table.stats().notifies == notifies_before + 1);
    }

    // A line that shares the stripe invalidates the reservation (lost()), so it wakes the waiter too.
    {
        Waiter w;
        start(w, table.storeEpoch(), kWatched);
        settle();
        table.put(base, kSameStripe, src.data(), 16);
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
    }

    // A multi-line PUT wakes the waiters of the lines in the middle and at both ends of its range, and so do the
    // single-line writers.
    for (const std::uint32_t line : {0x2000u, 0x2080u, 0x2300u}) {
        Waiter w;
        start(w, table.storeEpoch(), line);
        settle();
        table.put(base, 0x2000 + 100, src.data(), 0x300);  // lines 0x2000..0x2300
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
    }
    {
        Waiter w;
        start(w, table.storeEpoch(), 0x3000);
        settle();
        table.put(base, 0x3000 - 16, src.data(), 0x400);  // starts one line below, ends well above
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
    }
    {
        tox2::Reservation res;
        std::uint8_t ls[128];
        table.getllar(base, 0x4000, ls, res);
        Waiter w;
        start(w, table.storeEpoch(), 0x4000);
        settle();
        std::uint8_t changed[128];
        std::memset(changed, 0x11, sizeof(changed));
        CHECK(table.putllc(base, 0x4000, changed, res));
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
    }
    {
        Waiter w;
        start(w, table.storeEpoch(), 0x4000);
        settle();
        table.putlluc(base, 0x4000, src.data());
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
    }

    // wake() ends a line waiter's wait for a halt, and a halt that was set before the wait returns at once.
    {
        std::uint32_t halt = 0;
        Waiter w;
        start(w, table.storeEpoch(), kWatched, &halt);
        settle();
        __atomic_store_n(&halt, 1u, __ATOMIC_RELEASE);
        table.wake();
        w.thread.join();
        CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Halt));
        CHECK(table.waitStoreEpoch(table.storeEpoch(), &halt, milliseconds(3000), kWatched) == tox2::StoreWait::Halt);
    }

    // One more line waiter than slots: the extra one is a wildcard, and every waiter is woken by its own line.
    {
        constexpr int kLineWaiters = static_cast<int>(tox2::kWatchSlots);
        std::array<Waiter, kLineWaiters + 1> waiters;
        const auto lineOf = [](int i) { return 0x10000 + i * tox2::kReservationLineSize; };
        const std::uint64_t seen = table.storeEpoch();
        for (int i = 0; i < kLineWaiters; i++) start(waiters[i], seen, lineOf(i));
        settle();
        // Every slot is taken by a different waiter, so nobody hears a write that none of them watch.
        std::uint64_t notifies_before = table.stats().notifies;
        table.put(base, kUnrelated, src.data(), 16);
        CHECK(table.stats().notifies == notifies_before);
        for (int i = 0; i < kLineWaiters; i++) CHECK(waiters[i].result.load() == kPending);

        // The 17th has no slot left and becomes a wildcard, which hears that write.
        start(waiters[kLineWaiters], table.storeEpoch(), lineOf(kLineWaiters));
        settle();
        notifies_before = table.stats().notifies;
        table.put(base, kUnrelated, src.data(), 16);
        CHECK(table.stats().notifies == notifies_before + 1);

        for (int i = 0; i <= kLineWaiters; i++) table.put(base, lineOf(i), src.data(), 16);
        for (auto& w : waiters) {
            w.thread.join();
            CHECK(w.result.load() == static_cast<int>(tox2::StoreWait::Epoch));
        }
    }
}

// A write to one waiter's line ends that waiter's sleep only. The other waiter keeps sleeping, even though the epoch
// moved, until wake().
void testWatchLineWakesOnlyItsWaiter() {
    constexpr std::uint32_t kLineA = 0;
    constexpr std::uint32_t kLineB = tox2::kReservationLineSize;  // the next line: another stripe
    tox2::ReservationTable table;
    TestMemory mem;
    std::uint32_t halt = 0;
    std::uint8_t src[16] = {1};
    constexpr int kPending = -1;
    std::atomic<int> result_a{kPending};
    std::atomic<int> result_b{kPending};
    const std::uint64_t seen = table.storeEpoch();
    std::thread waiter_a(
            [&] { result_a.store(static_cast<int>(table.waitStoreEpoch(seen, &halt, milliseconds(3000), kLineA))); });
    std::thread waiter_b(
            [&] { result_b.store(static_cast<int>(table.waitStoreEpoch(seen, &halt, milliseconds(3000), kLineB))); });
    // Both waiters have to be asleep before the write, or they return on the moved epoch without sleeping.
    std::this_thread::sleep_for(milliseconds(100));

    const auto put_at = Clock::now();
    table.put(mem.base(), kLineA, src, 16);
    waiter_a.join();
    CHECK(Clock::now() - put_at < milliseconds(2000));
    CHECK(result_a.load() == static_cast<int>(tox2::StoreWait::Epoch));

    std::this_thread::sleep_for(milliseconds(200));
    CHECK(result_b.load() == kPending);

    table.wake();
    waiter_b.join();
    CHECK(result_b.load() == static_cast<int>(tox2::StoreWait::Epoch));
}

// The notifying store helpers go through the process reservation table: a sleeping line waiter wakes and a
// reservation on the line is lost, which a plain tox2Write8 does not do.
void testNotifyWriteWakesLineWaiter() {
    constexpr std::uint32_t kLine = tox2::kReservationLineSize;  // line 1 of the buffer; the next line follows it
    constexpr std::uint32_t kByte = kLine + 0x72;
    tox2::ReservationTable& table = tox2::reservations();
    TestMemory mem;
    std::uint8_t* base = mem.base();
    std::uint32_t halt = 0;

    // Starts a waiter on kLine that has been asleep for 100 ms (an earlier write would only move the epoch).
    const auto startWaiter = [&](std::atomic<int>& result, milliseconds timeout) {
        const std::uint64_t seen = table.storeEpoch();
        std::thread waiter([&result, &table, &halt, seen, timeout] {
            result.store(static_cast<int>(table.waitStoreEpoch(seen, &halt, timeout, kLine)));
        });
        std::this_thread::sleep_for(milliseconds(100));
        return waiter;
    };
    constexpr int kPending = -1;

    // 1 and 2: the store wakes the waiter, and fails reservations taken before it.
    {
        tox2::Reservation lost_res;
        tox2::Reservation putllc_res;
        std::uint8_t ls[128];
        table.getllar(base, kLine, ls, lost_res);
        table.getllar(base, kLine, ls, putllc_res);
        std::atomic<int> result{kPending};
        std::thread waiter = startWaiter(result, milliseconds(3000));
        CHECK(result.load() == kPending);
        const auto store_at = Clock::now();
        tox2NotifyWrite8(base, kByte, 0xff);
        waiter.join();
        CHECK(Clock::now() - store_at < milliseconds(2000));
        CHECK(result.load() == static_cast<int>(tox2::StoreWait::Epoch));
        CHECK(base[kByte] == 0xff);
        CHECK(table.lost(base, lost_res));
        CHECK(table.versionMoved(lost_res));
        std::uint8_t src[128] = {};
        CHECK(!table.putllc(base, kLine, src, putllc_res));
        CHECK(base[kByte] == 0xff);

        // A store that leaves the bytes as they are still moves the version: the content compare alone would miss it.
        table.getllar(base, kLine, ls, lost_res);
        CHECK(!table.lost(base, lost_res));
        tox2NotifyWrite8(base, kByte, 0xff);
        CHECK(table.versionMoved(lost_res));
        CHECK(table.lost(base, lost_res));
    }

    // 3: the plain store is the control. It changes the byte but neither moves the version nor wakes the waiter.
    {
        tox2::Reservation res;
        std::uint8_t ls[128];
        table.getllar(base, kLine, ls, res);
        std::atomic<int> result{kPending};
        std::thread waiter = startWaiter(result, milliseconds(200));
        tox2Write8(base, kByte, 0x11);
        waiter.join();
        CHECK(result.load() == static_cast<int>(tox2::StoreWait::Timeout));
        CHECK(base[kByte] == 0x11);
        CHECK(!table.versionMoved(res));
        CHECK(table.lost(base, res));  // by the content compare only
    }

    // 4: wider stores write what tox2WriteN writes, including a misaligned one and one that crosses into the next line.
    {
        TestMemory reference;
        std::uint8_t* ref = reference.base();
        std::memcpy(ref, base, sizeof(TestMemory::bytes));
        tox2::Reservation first;
        tox2::Reservation second;
        std::uint8_t ls[128];
        table.getllar(base, kLine, ls, first);
        table.getllar(base, 2 * kLine, ls, second);

        tox2NotifyWrite16(base, kLine + 0x10, 0x1234);
        tox2Write16(ref, kLine + 0x10, 0x1234);
        tox2NotifyWrite32(base, kLine + 0x20, 0x01020304u);
        tox2Write32(ref, kLine + 0x20, 0x01020304u);
        tox2NotifyWrite64(base, kLine + 0x28, 0x0102030405060708ull);
        tox2Write64(ref, kLine + 0x28, 0x0102030405060708ull);
        tox2NotifyWrite64(base, kLine + 0x44, 0x1112131415161718ull);  // not 8-aligned
        tox2Write64(ref, kLine + 0x44, 0x1112131415161718ull);
        CHECK(std::memcmp(base, ref, sizeof(TestMemory::bytes)) == 0);
        CHECK(table.versionMoved(first));
        CHECK(!table.versionMoved(second));

        table.getllar(base, kLine, ls, first);
        tox2NotifyWrite32(base, kLine + 0x7e, 0xa1b2c3d4u);
        tox2Write32(ref, kLine + 0x7e, 0xa1b2c3d4u);
        CHECK(std::memcmp(base, ref, sizeof(TestMemory::bytes)) == 0);
        CHECK(base[kLine + 0x7e] == 0xa1 && base[2 * kLine + 1] == 0xd4);
        CHECK(table.versionMoved(first));
        CHECK(table.versionMoved(second));
    }
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
    CHECK(!table.storeConditional(base, 4, 4, 1, stripeVersion(table, base, 4), 2));
    CHECK(!table.lost(base, res));
    CHECK(!table.storeConditional(base, kCollidingLine + 4, 4, 1, stripeVersion(table, base, 4), 2));
    CHECK(!table.lost(base, res));

    // Concurrent part: lost() may see the stripe while a failing writer holds it.
    for (const std::uint32_t target : {4u, kCollidingLine + 4}) {
        std::atomic<bool> done{false};
        std::thread failer([&] {
            const std::uint64_t version = stripeVersion(table, base, target);
            for (int i = 0; i < 200000; i++) table.storeConditional(base, target, 4, 1, version, 2);
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

// Failed PUTLLCs are counted by the check that rejected them. The table is local, so the counts are exact. A stripe
// held by another writer (Locked) has no deterministic setup.
void testPutllcFailReasons() {
    tox2::ReservationTable table;
    TestMemory mem;
    tox2::Reservation res;
    std::uint8_t ls[128];
    std::uint8_t zeros[128] = {};
    using tox2::PutllcFail;
    const auto fails = [&](PutllcFail why) { return table.stats().putllc_fails[static_cast<std::size_t>(why)]; };

    CHECK(!table.putllc(mem.base(), 0, ls, res));  // no reservation
    CHECK(fails(PutllcFail::Lost) == 1);
    table.getllar(mem.base(), 0, ls, res);
    CHECK(!table.putllc(mem.base(), 128, ls, res));  // another line
    CHECK(fails(PutllcFail::Lost) == 2);

    table.getllar(mem.base(), 0, ls, res);
    table.put(mem.base(), 0x40, zeros, 16);  // same bytes, the version moves
    CHECK(!table.putllc(mem.base(), 0, ls, res));
    CHECK(fails(PutllcFail::Moved) == 1);

    table.getllar(mem.base(), 0, ls, res);
    plainStore8(mem.bytes + 0x72, 0xff);  // no version change
    CHECK(!table.putllc(mem.base(), 0, ls, res));
    CHECK(fails(PutllcFail::Data) == 1);

    table.getllar(mem.base(), 0, ls, res);
    CHECK(table.putllc(mem.base(), 0, ls, res));
    const tox2::ReservationStats stats = table.stats();
    CHECK(stats.putllc_fails[static_cast<std::size_t>(PutllcFail::Lost)] == 2);
    CHECK(stats.putllc_fails[static_cast<std::size_t>(PutllcFail::Locked)] == 0);
    CHECK(stats.putllc_fails[static_cast<std::size_t>(PutllcFail::Moved)] == 1);
    CHECK(stats.putllc_fails[static_cast<std::size_t>(PutllcFail::Data)] == 1);
}

// A notify is counted under the kind of the writer whose epoch bump found a waiter; wake() always notifies.
void testNotifyByWriterKind() {
    tox2::ReservationTable table;
    TestMemory mem;
    std::atomic<bool> stop{false};
    std::thread waiter([&] {
        while (!stop.load()) table.waitStoreEpoch(table.storeEpoch(), nullptr, milliseconds(20));
    });
    const auto notified = [&](tox2::WriterKind kind) {
        return table.stats().notifies_by_kind[static_cast<std::size_t>(kind)];
    };
    // Repeats the write until the waiter is registered and sees it, which takes a few tries at most.
    const auto until_notified = [&](tox2::WriterKind kind, auto&& write) {
        const auto deadline = Clock::now() + milliseconds(3000);
        while (notified(kind) == 0 && Clock::now() < deadline) {
            write();
            std::this_thread::sleep_for(milliseconds(1));
        }
        return notified(kind) != 0;
    };
    std::uint8_t ls[128] = {};
    std::uint8_t zeros[16] = {};
    tox2::Reservation res;
    CHECK(until_notified(tox2::WriterKind::Put, [&] { table.put(mem.base(), 0, zeros, 16); }));
    CHECK(until_notified(tox2::WriterKind::Putlluc, [&] { table.putlluc(mem.base(), 0, ls); }));
    CHECK(until_notified(tox2::WriterKind::Putllc, [&] {
        table.getllar(mem.base(), 0, ls, res);
        ls[0] ^= 1;
        table.putllc(mem.base(), 0, ls, res);
    }));
    CHECK(until_notified(tox2::WriterKind::PpuStoreConditional, [&] {
        std::uint64_t value = 0, version = 0;
        table.loadReserved(mem.base(), 0, 4, value, version);
        table.storeConditional(mem.base(), 0, 4, value, version, value + 1);
    }));
    CHECK(notified(tox2::WriterKind::Wake) == 0);
    table.wake();
    CHECK(notified(tox2::WriterKind::Wake) == 1);
    stop = true;
    table.wake();
    waiter.join();
    const tox2::ReservationStats stats = table.stats();
    std::uint64_t sum = 0;
    for (const std::uint64_t n : stats.notifies_by_kind) sum += n;
    CHECK(stats.notifies == sum);
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
std::atomic<std::uint32_t> g_spin_laps{0};

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
        // Registers the halt check must not leave stale in ctx: the compiler may keep them in host registers
        // across the back edge, so the taken path has to store them.
        TOX2_VR_W(ctx->gpr[5], 0) += 1;
        TOX2_VR_W(ctx->gpr[6], 0) = ~TOX2_VR_W(ctx->gpr[5], 0);
        g_spin_laps.fetch_add(1, std::memory_order_relaxed);
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

// Overwriting tracked LS with the bytes it already holds from a non-image address keeps the tags; a changed
// byte untags only the unit that holds it.
void testTrackingIdenticalWrite() {
    registerSynthImages();
    auto guest = makeGuest();
    constexpr std::uint32_t kMirror = 0xa000;  // a copy of the code segment outside any image
    std::memcpy(guest.data() + kMirror, guest.data() + kEaA, 0x100);

    const auto loaded = [&] {
        auto thread = std::make_unique<tox2::SpuThread>(11, "identical", nullptr);
        tox2::copyToLs(*thread, guest.data(), kEaA, 0x3000, 0x100);
        return thread;
    };

    // (a) The copy from the image address tags every unit.
    auto thread = loaded();
    for (std::uint32_t lsa = 0x3000; lsa < 0x3100; lsa += 0x10) CHECK(trackedAs(*thread, lsa, "synth"));

    // (b) The same bytes from elsewhere keep the tags and the record.
    tox2::copyToLs(*thread, guest.data(), kMirror, 0x3000, 0x100);
    for (std::uint32_t lsa = 0x3000; lsa < 0x3100; lsa += 0x10) {
        CHECK(trackedAs(*thread, lsa, "synth") && tox2::trackedImageAt(*thread, lsa)->delta == 0);
    }

    // (c) One changed byte untags its unit only.
    guest[kMirror + 0x45] ^= 0x01;
    tox2::copyToLs(*thread, guest.data(), kMirror, 0x3000, 0x100);
    CHECK(std::memcmp(thread->ls() + 0x3000, guest.data() + kMirror, 0x100) == 0);
    CHECK(trackedAs(*thread, 0x3030, "synth") && untracked(*thread, 0x3040) && trackedAs(*thread, 0x3050, "synth"));
    CHECK(trackedAs(*thread, 0x3000, "synth") && trackedAs(*thread, 0x30f0, "synth"));
    guest[kMirror + 0x45] ^= 0x01;

    // (d) A copy that covers part of two units compares only the bytes it writes: differences just outside
    // the copied range do not matter, one inside untags that unit and leaves the other tagged.
    thread = loaded();
    guest[kMirror + 0x07] ^= 0x01;
    guest[kMirror + 0x18] ^= 0x01;
    tox2::copyToLs(*thread, guest.data(), kMirror + 0x08, 0x3008, 0x10);
    CHECK(trackedAs(*thread, 0x3000, "synth") && trackedAs(*thread, 0x3010, "synth"));
    CHECK(trackedAs(*thread, 0x3020, "synth"));
    guest[kMirror + 0x17] ^= 0x01;
    tox2::copyToLs(*thread, guest.data(), kMirror + 0x08, 0x3008, 0x10);
    CHECK(trackedAs(*thread, 0x3000, "synth") && untracked(*thread, 0x3010) && trackedAs(*thread, 0x3020, "synth"));
    CHECK(std::memcmp(thread->ls() + 0x3008, guest.data() + kMirror + 0x08, 0x10) == 0);
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

// Image setup of the tag summary tests: image 0 has a 13 KiB code segment and a 2 KiB data segment behind it, image 1
// one 6 KiB code segment. Copy sources outside every code segment: a mirror of the image bytes at kTagMirrorEa and
// unrelated bytes at kTagDataEa.
constexpr std::uint32_t kTagGuestSize = 0x40000;
constexpr std::uint32_t kTagImage0Ea = 0x10000;
constexpr std::uint32_t kTagImage0Ls = 0x4000;
constexpr std::uint32_t kTagImage0Code = 0x3400;
constexpr std::uint32_t kTagImage0Size = 0x3c00;
constexpr std::uint32_t kTagImage1Ea = 0x14000;
constexpr std::uint32_t kTagImage1Ls = 0x9000;
constexpr std::uint32_t kTagImage1Size = 0x1800;
constexpr std::uint32_t kTagMirrorEa = 0x20000;
constexpr std::uint32_t kTagDataEa = 0x30000;
constexpr std::uint32_t kTagDataSize = 0x8000;

const Tox2SpuSegment kTagSegments0[] = {
        {kTagImage0Ea, kTagImage0Ls, kTagImage0Code, 1},
        {kTagImage0Ea + kTagImage0Code, kTagImage0Ls + kTagImage0Code, kTagImage0Size - kTagImage0Code, 0}};
const Tox2SpuSegment kTagSegments1[] = {{kTagImage1Ea, kTagImage1Ls, kTagImage1Size, 1}};
const Tox2SpuImageInfo kTagImages[] = {
        {"tag0", kTagImage0Ea, kTagImage0Size, kNoHash, kTagImage0Ls, 2, kTagSegments0, 0, nullptr},
        {"tag1", kTagImage1Ea, kTagImage1Size, kNoHash, kTagImage1Ls, 1, kTagSegments1, 0, nullptr},
};

// Deterministic bytes (xorshift64), so a run can be repeated.
struct TagRandom {
    std::uint64_t x;
    std::uint32_t next() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return static_cast<std::uint32_t>(x >> 32);
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

std::vector<std::uint8_t> makeTagGuest() {
    std::vector<std::uint8_t> guest(kTagGuestSize);
    TagRandom rng{0x9e3779b97f4a7c15ull};
    for (std::uint32_t i = kTagImage0Ea; i < kTagImage1Ea + kTagImage1Size; i++) {
        guest[i] = static_cast<std::uint8_t>(rng.next());
    }
    std::memcpy(guest.data() + kTagMirrorEa, guest.data() + kTagImage0Ea, 0x6000);
    for (std::uint32_t i = 0; i < kTagDataSize; i++) guest[kTagDataEa + i] = static_cast<std::uint8_t>(rng.next());
    return guest;
}

// Summary bits of blocks [first, last), as the word array holds them.
std::array<std::uint64_t, tox2::kSpuTagBlockWords> blockBits(std::uint32_t first, std::uint32_t last) {
    std::array<std::uint64_t, tox2::kSpuTagBlockWords> words{};
    for (std::uint32_t b = first; b < last; b++) words[b / 64] |= std::uint64_t{1} << (b % 64);
    return words;
}

// No tagged unit sits in a block whose summary bit is clear.
bool summaryCoversTags(const tox2::SpuThread& thread) {
    for (std::uint32_t unit = 0; unit < tox2::kSpuTagCount; unit++) {
        if (untracked(thread, unit * tox2::kSpuTagUnit)) continue;
        if (!thread.blockMayBeTagged(unit / tox2::kSpuTagBlockUnits)) return false;
    }
    return true;
}

// A copy over a tagged run untags exactly the units whose written bytes differ. The copy is not 16-aligned at either
// end, so the first and last unit are partial and only the bytes the copy writes count; units in the middle are
// compared whole, and neighbouring differing units are untagged together.
void testUntagChangedUnitsRuns() {
    tox2::registerSpuImages(kTagImages, 2);
    constexpr std::uint32_t kOffset = 0x240;  // into the code segment, a multiple of 16
    constexpr std::uint32_t kBase = kTagImage0Ls + kOffset;
    constexpr std::uint32_t kStart = 0x13;   // first copied byte, relative to kBase
    constexpr std::uint32_t kLength = 0x95;  // the copy ends at 0xa8
    constexpr std::uint32_t kUnits = 14;     // units 0 and 11..13 lie outside the copy
    struct Case {
        const char* name;
        std::vector<std::uint32_t> flipped;   // source bytes that differ from LS, relative to kBase
        std::vector<std::uint32_t> untagged;  // expected untagged units, relative to kBase / 16
    };
    const Case cases[] = {
            {"one interior unit and both edge units", {0x14, 0x55, 0xa7}, {1, 5, 10}},
            {"bytes next to the copy, interior runs", {0x12, 0x30, 0x4f, 0x5f, 0x70, 0xa8}, {3, 4, 5, 7}},
            {"edges inside runs", {0x1f, 0x20, 0x3a, 0x6b, 0x90, 0xa0}, {1, 2, 3, 6, 9, 10}},
            {"nothing differs", {0x12, 0xa8, 0xb0}, {}},
            {"every unit",
             {0x13, 0x28, 0x38, 0x49, 0x5a, 0x6b, 0x7c, 0x8d, 0x9e, 0xa7},
             {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}},
    };
    for (const Case& c : cases) {
        auto guest = makeTagGuest();
        tox2::SpuThread thread(23, "untag-runs", nullptr);
        tox2::copyToLs(thread, guest.data(), kTagImage0Ea, kTagImage0Ls, kTagImage0Size);
        for (std::uint32_t x : c.flipped) guest[kTagMirrorEa + kOffset + x] ^= 0x01;
        tox2::copyToLs(thread, guest.data(), kTagMirrorEa + kOffset + kStart, kBase + kStart, kLength);
        CHECK(std::memcmp(thread.ls() + kBase + kStart, guest.data() + kTagMirrorEa + kOffset + kStart, kLength) == 0);
        for (std::uint32_t unit = 0; unit < kUnits; unit++) {
            const bool expect_untagged = std::find(c.untagged.begin(), c.untagged.end(), unit) != c.untagged.end();
            const bool is_untagged = untracked(thread, kBase + unit * 16);
            if (is_untagged != expect_untagged) std::fprintf(stderr, "case '%s': unit %u\n", c.name, unit);
            CHECK(is_untagged == expect_untagged);
            if (!is_untagged) CHECK(trackedAs(thread, kBase + unit * 16, "tag0"));
        }
        CHECK(trackedAs(thread, kBase - 16, "tag0") && trackedAs(thread, kBase + kUnits * 16, "tag0"));
        CHECK(summaryCoversTags(thread));
    }
}

void testTaggedBlockSummary() {
    tox2::registerSpuImages(kTagImages, 2);
    const auto guest = makeTagGuest();
    constexpr std::uint32_t kBlock = tox2::kSpuTagBlock;
    constexpr std::uint32_t kFirstCodeBlock = kTagImage0Ls / kBlock;
    constexpr std::uint32_t kCodeBlocks = kTagImage0Code / kBlock;

    {
        tox2::SpuThread thread(20, "summary", nullptr);
        CHECK(thread.taggedBlocks() == blockBits(0, 0));

        // The bits of exactly the blocks that hold code.
        tox2::copyToLs(thread, guest.data(), kTagImage0Ea, kTagImage0Ls, kTagImage0Size);
        CHECK(thread.taggedBlocks() == blockBits(kFirstCodeBlock, kFirstCodeBlock + kCodeBlocks));
        CHECK(summaryCoversTags(thread));

        // Other bytes over the whole of one tagged block untag it and clear its bit; the neighbours stay.
        tox2::copyToLs(thread, guest.data(), kTagDataEa, kTagImage0Ls + kBlock, kBlock);
        auto expected = blockBits(kFirstCodeBlock, kFirstCodeBlock + kCodeBlocks);
        expected[(kFirstCodeBlock + 1) / 64] &= ~(std::uint64_t{1} << ((kFirstCodeBlock + 1) % 64));
        CHECK(thread.taggedBlocks() == expected);
        CHECK(untracked(thread, kTagImage0Ls + kBlock) && untracked(thread, kTagImage0Ls + 2 * kBlock - 16));
        CHECK(trackedAs(thread, kTagImage0Ls, "tag0") && trackedAs(thread, kTagImage0Ls + 2 * kBlock, "tag0"));

        // The same bytes from outside an image keep tags and bits.
        tox2::copyToLs(thread, guest.data(), kTagMirrorEa + 2 * kBlock, kTagImage0Ls + 2 * kBlock, 3 * kBlock);
        CHECK(thread.taggedBlocks() == expected);
        CHECK(trackedAs(thread, kTagImage0Ls + 2 * kBlock, "tag0") &&
              trackedAs(thread, kTagImage0Ls + 5 * kBlock - 16, "tag0"));

        // A copy that covers part of a block untags only the units whose bytes differ and keeps the bit.
        tox2::copyToLs(thread, guest.data(), kTagDataEa, kTagImage0Ls + 6 * kBlock + 0x40, 0x40);
        CHECK(untracked(thread, kTagImage0Ls + 6 * kBlock + 0x40) &&
              trackedAs(thread, kTagImage0Ls + 6 * kBlock, "tag0"));
        CHECK(thread.taggedBlocks() == expected);
        CHECK(summaryCoversTags(thread));

        // The whole LS untagged: no bit is left.
        tox2::untagLs(thread, 0, kTox2SpuLsSize);
        CHECK(thread.taggedBlocks() == blockBits(0, 0));
        CHECK(untracked(thread, kTagImage0Ls));
    }

    {
        // A bit that is set while its block holds no tag (a partial untag leaves it) is cleared by the next copy that
        // covers the whole block.
        tox2::SpuThread thread(21, "stale", nullptr);
        tox2::copyToLs(thread, guest.data(), kTagImage0Ea, kTagImage0Ls, kTagImage0Size);
        const std::uint32_t block_ls = kTagImage0Ls + 3 * kBlock;
        tox2::untagLs(thread, block_ls, kBlock - 16);
        CHECK(trackedAs(thread, block_ls + kBlock - 16, "tag0"));
        tox2::untagLs(thread, block_ls + kBlock - 16, 16);
        CHECK(untracked(thread, block_ls + kBlock - 16));
        CHECK(((thread.taggedBlocks()[(block_ls / kBlock) / 64] >> ((block_ls / kBlock) % 64)) & 1u) != 0);
        tox2::copyToLs(thread, guest.data(), kTagMirrorEa + 3 * kBlock, block_ls, kBlock);
        CHECK(((thread.taggedBlocks()[(block_ls / kBlock) / 64] >> ((block_ls / kBlock) % 64)) & 1u) == 0);
        CHECK(summaryCoversTags(thread));
    }

    {
        // A load at a delta that is not a multiple of the block size: the edge blocks get a bit as soon as one unit
        // of them is tagged.
        tox2::SpuThread thread(22, "shifted", nullptr);
        tox2::copyToLs(thread, guest.data(), kTagImage1Ea, kTagImage1Ls + 0x3f0, 0x20);
        CHECK(thread.taggedBlocks() == blockBits((kTagImage1Ls + 0x3f0) / kBlock, (kTagImage1Ls + 0x3f0) / kBlock + 2));
    }

    // The code span: [0x10000, 0x15800) is the union of the code segments; the data segment of image 0 lies inside it.
    CHECK(tox2::spuCodeRangeOverlaps(kTagImage0Ea, 1));
    CHECK(tox2::spuCodeRangeOverlaps(kTagImage0Ea - 16, 17));
    CHECK(!tox2::spuCodeRangeOverlaps(kTagImage0Ea - 16, 16));
    CHECK(tox2::spuCodeRangeOverlaps(kTagImage1Ea + kTagImage1Size - 1, 16));
    CHECK(!tox2::spuCodeRangeOverlaps(kTagImage1Ea + kTagImage1Size, 16));
    CHECK(!tox2::spuCodeRangeOverlaps(kTagDataEa, kTagDataSize));
    registerSynthImages();
    CHECK(tox2::spuCodeRangeOverlaps(kEaA, 1) && !tox2::spuCodeRangeOverlaps(0, 0x100));
}

// The per-unit rule that copyToLs follows, written out without the summary: a tagged unit loses its tag when the bytes
// the copy writes into it differ from what it holds; then the units wholly inside a code segment's intersection with
// the copy are tagged for that segment.
class TagReference {
public:
    struct Tag {
        bool tagged = false;
        std::uint32_t image = 0, segment = 0;
        std::int32_t delta = 0;
    };

    TagReference() : ls_(kTox2SpuLsSize, 0), tags_(tox2::kSpuTagCount) {}

    void copy(const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size) {
        std::uint32_t pos = lsa & (kTox2SpuLsSize - 1u);
        while (size > 0) {
            const std::uint32_t n = std::min<std::uint32_t>(size, kTox2SpuLsSize - pos);
            piece(base, ea, pos, n);
            ea += n;
            size -= n;
            pos = 0;
        }
    }

    void untag(std::uint32_t lsa, std::uint32_t size) {
        std::uint64_t pos = lsa & (kTox2SpuLsSize - 1u);
        std::uint64_t left = size;
        while (left > 0) {
            const std::uint64_t n = std::min<std::uint64_t>(left, kTox2SpuLsSize - pos);
            for (std::uint64_t u = pos / 16; u < (pos + n + 15) / 16; u++) tags_[u].tagged = false;
            left -= n;
            pos = 0;
        }
    }

    const std::vector<std::uint8_t>& ls() const { return ls_; }
    const Tag& tag(std::uint32_t unit) const { return tags_[unit]; }

private:
    void piece(const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size) {
        const std::uint32_t end = lsa + size;
        for (std::uint32_t u = lsa / 16; u < (end + 15) / 16; u++) {
            if (!tags_[u].tagged) continue;
            const std::uint32_t lo = std::max(lsa, u * 16);
            const std::uint32_t hi = std::min(end, (u + 1) * 16);
            if (std::memcmp(ls_.data() + lo, base + ea + (lo - lsa), hi - lo) != 0) tags_[u].tagged = false;
        }
        std::memcpy(ls_.data() + lsa, base + ea, size);
        const std::uint64_t ea_end = std::uint64_t{ea} + size;
        for (std::uint32_t i = 0; i < 2; i++) {
            for (std::uint32_t j = 0; j < kTagImages[i].segment_count; j++) {
                const Tox2SpuSegment& s = kTagImages[i].segments[j];
                if (s.code == 0) continue;
                const std::uint64_t a = std::max<std::uint64_t>(ea, s.ea);
                const std::uint64_t b = std::min<std::uint64_t>(ea_end, std::uint64_t{s.ea} + s.size);
                if (a >= b) continue;
                const std::uint32_t lo = static_cast<std::uint32_t>((lsa + (a - ea) + 15) & ~15ull);
                const std::uint32_t hi = static_cast<std::uint32_t>((lsa + (b - ea)) & ~15ull);
                if (lo >= hi) continue;
                const auto delta = static_cast<std::int32_t>(std::int64_t{lsa} - (std::int64_t{ea} - s.ea) - s.ls);
                for (std::uint32_t u = lo / 16; u < hi / 16; u++) tags_[u] = Tag{true, i, j, delta};
            }
        }
    }

    std::vector<std::uint8_t> ls_;
    std::vector<Tag> tags_;
};

bool sameAsReference(const tox2::SpuThread& thread, const TagReference& ref) {
    if (std::memcmp(thread.ls(), ref.ls().data(), kTox2SpuLsSize) != 0) return false;
    for (std::uint32_t unit = 0; unit < tox2::kSpuTagCount; unit++) {
        const auto tracked = tox2::trackedImageAt(thread, unit * tox2::kSpuTagUnit);
        const TagReference::Tag& want = ref.tag(unit);
        if (tracked.has_value() != want.tagged) return false;
        if (tracked &&
            (tracked->image_index != want.image || tracked->segment != want.segment || tracked->delta != want.delta)) {
            return false;
        }
    }
    return true;
}

// Random copies and untags against the reference above. Image bytes land at the link address and at a few fixed
// offsets from it (so the distinct load records stay few), mirror bytes with some flipped bytes make equal and
// different runs inside tagged code, and unrelated bytes over any LS range make the usual untags.
void testCopyToLsMatchesReference() {
    tox2::registerSpuImages(kTagImages, 2);
    auto guest = makeTagGuest();
    tox2::SpuThread thread(23, "reference", nullptr);
    TagReference ref;
    TagRandom rng{0x2545f4914f6cdd1dull};
    constexpr std::int32_t kDeltas[] = {0, 0, 0, 8, 16, -16, 0x400};
    int mismatches = 0;
    int summary_failures = 0;

    for (int op = 0; op < 2000; op++) {
        const std::uint32_t kind = rng.below(10);
        std::uint32_t ea = 0, lsa = 0, size = 0;
        if (kind < 3) {
            // Image bytes, whole or in pieces.
            const bool second = rng.below(4) == 0;
            const std::uint32_t image_ea = second ? kTagImage1Ea : kTagImage0Ea;
            const std::uint32_t image_ls = second ? kTagImage1Ls : kTagImage0Ls;
            const std::uint32_t image_size = second ? kTagImage1Size : kTagImage0Size;
            const std::uint32_t off = rng.below(3) == 0 ? 0 : rng.below(image_size);
            size = rng.below(3) == 0 ? image_size - off : 1 + rng.below(std::min(image_size - off, 0x1800u));
            ea = image_ea + off;
            lsa = static_cast<std::uint32_t>(std::int64_t{image_ls} + kDeltas[rng.below(7)] + off);
        } else if (kind < 6) {
            // Mirror bytes onto the code area.
            ea = kTagMirrorEa + rng.below(0x5000);
            lsa = kTagImage0Ls + (ea - kTagMirrorEa) + (rng.below(3) == 0 ? rng.below(64) : 0);
            size = 1 + rng.below(0x1800);
        } else if (kind < 8) {
            // Unrelated bytes anywhere, now and then across the end of the LS.
            ea = kTagDataEa + rng.below(kTagDataSize - 0x1800);
            lsa = rng.below(8) == 0 ? kTox2SpuLsSize - rng.below(0x200) : rng.below(0x12000);
            size = 1 + rng.below(rng.below(2) == 0 ? 0x40 : 0x1800);
        } else if (kind == 8) {
            const std::uint32_t at = kTagImage0Ls + rng.below(0x5000);
            const std::uint32_t n = rng.below(3) == 0 ? rng.below(0x1000) : rng.below(0x60);
            tox2::untagLs(thread, at, n);
            ref.untag(at, n);
            if (!sameAsReference(thread, ref)) mismatches++;
            if (!summaryCoversTags(thread)) summary_failures++;
            continue;
        } else {
            // Flip or restore a mirror byte, so mirror copies are equal or different from the LS in new places.
            const std::uint32_t at = rng.below(0x5000);
            if (rng.below(2) == 0) {
                guest[kTagMirrorEa + at] ^= 0x5a;
            } else {
                guest[kTagMirrorEa + at] = guest[kTagImage0Ea + at];
            }
            continue;
        }
        tox2::copyToLs(thread, guest.data(), ea, lsa, size);
        ref.copy(guest.data(), ea, lsa, size);
        if (!sameAsReference(thread, ref)) {
            mismatches++;
            std::fprintf(stderr, "reference mismatch after op %d (kind %u): ea %#x ls %#x size %#x\n", op, kind, ea,
                         lsa, size);
        }
        if (!summaryCoversTags(thread)) summary_failures++;
    }
    CHECK(mismatches == 0);
    CHECK(summary_failures == 0);
    // The run did tag and untag, not only one of them.
    int tagged = 0;
    for (std::uint32_t unit = 0; unit < tox2::kSpuTagCount; unit++) tagged += ref.tag(unit).tagged ? 1 : 0;
    std::printf("reference run: %d of %u units tagged at the end\n", tagged, tox2::kSpuTagCount);
    registerSynthImages();
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

// ---------------------------------------------------------------------------------------------
// Span capture (spu/spu_capture.h).
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kCapS = 0x6000;     // computes on LS and returns by dispatch to r0
constexpr std::uint32_t kCapE = 0x6010;     // ends the run itself, as a return into code that does a channel wait would
constexpr std::uint32_t kCapGetS = 0x6020;  // issues a GET, then dispatches to r0
constexpr std::uint32_t kCapGetE = 0x6030;
constexpr std::uint32_t kCapS3 = 0x6040;  // dispatches to kCapMid, which dispatches to r0
constexpr std::uint32_t kCapMid = 0x6050;
constexpr std::uint32_t kCapE3 = 0x6060;
constexpr std::uint32_t kCapEa = 0x6000;  // guest address of the image bytes (the LS address of the load)
constexpr std::uint32_t kCapCodeSize = 0x70;
constexpr std::uint32_t kCapEaB = 0x7000;
constexpr std::uint32_t kCapWork = 0x2000;   // the words kCapS reads and writes
constexpr std::uint32_t kCapGetLs = 0x3000;  // LS target of the GET
constexpr std::uint32_t kCapGetEa = 0x100;   // guest source of the GET

#define CAP_WR(ch, value)                                                            \
    do {                                                                             \
        if (!tox2SpuWriteChannel(ctx, ls, site, (ch), (value))) TOX2_SPU_SF_LEAVE(); \
    } while (0)

std::uint32_t loadLe32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// What kCapS does to the registers and the LS, for the reference model.
void capModel(std::array<Tox2Vr, 128>& gpr, std::vector<std::uint8_t>& ls) {
    const std::uint32_t a = loadBe32(ls.data() + kCapWork);
    storeBe32(ls.data() + kCapWork + 4, a * 3 + TOX2_VR_W(gpr[3], 0));
    TOX2_VR_W(gpr[4], 0) = a ^ 0x5a5a;
}

TOX2_SPU_FUNC(capS) {
    TOX2_SPU_SF_ENTER(kCapS);
    const std::uint32_t a = loadBe32(ls + kCapWork);
    storeBe32(ls + kCapWork + 4, a * 3 + TOX2_VR_W(ctx->gpr[3], 0));
    TOX2_VR_W(ctx->gpr[4], 0) = a ^ 0x5a5a;
    TOX2_SPU_SF_DISPATCH(kCapS + 8, TOX2_VR_W(ctx->gpr[0], 0));
}

TOX2_SPU_FUNC(capEnd) {
    (void)ls;
    TOX2_SPU_SF_ENTER(kCapE);
    TOX2_SPU_SF_LEAVE();
}

TOX2_SPU_FUNC(capGetS) {
    TOX2_SPU_SF_ENTER(kCapGetS);
    const std::uint32_t site = kCapGetS + 4;
    CAP_WR(tox2::chan::kMfcLsa, kCapGetLs);
    CAP_WR(tox2::chan::kMfcEah, 0);
    CAP_WR(tox2::chan::kMfcEal, kCapGetEa);
    CAP_WR(tox2::chan::kMfcSize, 16);
    CAP_WR(tox2::chan::kMfcTagId, 1);
    CAP_WR(tox2::chan::kMfcCmd, tox2::mfc::kGet);
    TOX2_SPU_SF_DISPATCH(kCapGetS + 8, TOX2_VR_W(ctx->gpr[0], 0));
}

TOX2_SPU_FUNC(capS3) {
    TOX2_SPU_SF_ENTER(kCapS3);
    TOX2_SPU_SF_DISPATCH(kCapS3 + 4, kCapMid);
}

TOX2_SPU_FUNC(capMid) {
    TOX2_SPU_SF_ENTER(kCapMid);
    TOX2_SPU_SF_DISPATCH(kCapMid + 4, TOX2_VR_W(ctx->gpr[0], 0));
}

const Tox2SpuSegment kCapSegments[] = {{kCapEa, kCapEa, kCapCodeSize, 1}};
const Tox2SpuEntry kCapEntries[] = {
        {kCapS, 1, {0xc0de0001}, capS},      {kCapE, 1, {0xc0de0002}, capEnd}, {kCapGetS, 1, {0xc0de0003}, capGetS},
        {kCapGetE, 1, {0xc0de0004}, capEnd}, {kCapS3, 1, {0xc0de0005}, capS3}, {kCapMid, 1, {0xc0de0006}, capMid},
        {kCapE3, 1, {0xc0de0007}, capEnd},
};
const Tox2SpuSegment kCapSegmentsB[] = {{kCapEaB, kCapEaB, 0x20, 1}};
const Tox2SpuEntry kCapEntriesB[] = {{kCapEaB, 1, {0xc0de0008}, capEnd}};
const Tox2SpuImageInfo kCapImages[] = {
        {"capsyn", kCapEa, kCapCodeSize, kNoHash, kCapS, 1, kCapSegments, 7, kCapEntries},
        {"capsyn_b", kCapEaB, 0x20, kNoHash, kCapEaB, 1, kCapSegmentsB, 1, kCapEntriesB},
};

std::vector<std::uint8_t> makeCapGuest() {
    std::vector<std::uint8_t> guest(0x10000, 0x5a);
    for (std::uint32_t e = 0; e < 7; e++) putBe32(guest, kCapEa + 0x10 * e, 0xc0de0001 + e);
    putBe32(guest, kCapEaB, 0xc0de0008);
    for (std::uint32_t i = 0; i < 16; i++) guest[kCapGetEa + i] = static_cast<std::uint8_t>(0xa0 + i);
    return guest;
}

std::filesystem::path capTestDir() {
    return std::filesystem::temp_directory_path() / ("tox2_spu_capture_test_" + std::to_string(GetCurrentProcessId()));
}

// A thread with the capture images loaded, the GET source in guest memory and a recognisable register and LS state.
struct CapThread {
    std::vector<std::uint8_t> guest = makeCapGuest();
    std::unique_ptr<tox2::SpuThread> thread;

    explicit CapThread(std::uint32_t id) {
        thread = std::make_unique<tox2::SpuThread>(id, "cap", nullptr);
        thread->channels().mfc().setGuestBase(guest.data());
        for (std::uint32_t i = 0; i < tox2::kSpuTagCount * tox2::kSpuTagUnit; i++) {
            thread->ls()[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8) + 7);
        }
        tox2::copyToLs(*thread, guest.data(), kCapEa, kCapEa, kCapCodeSize);
        tox2::copyToLs(*thread, guest.data(), kCapEaB, kCapEaB, 0x20);
        Tox2SpuContext& ctx = thread->context();
        for (std::uint32_t r = 0; r < 128; r++) {
            for (std::uint32_t i = 0; i < 16; i++) ctx.gpr[r].u8[i] = static_cast<std::uint8_t>(r * 16 + i * 7 + 1);
        }
        for (std::uint32_t i = 0; i < 16; i++) ctx.fpscr.u8[i] = static_cast<std::uint8_t>(0x40 + i);
        ctx.srr0 = 0x1234;
        ctx.int_enabled = 1;
    }

    // Runs the span from `start`; the function there returns to `end` through r0.
    tox2::SpuLeave run(std::uint32_t start, std::uint32_t end) {
        TOX2_VR_W(thread->context().gpr[0], 0) = end;
        return tox2::runSpu(*thread, start);
    }
};

void expectSameCapture(const tox2::SpuCapture& a, const tox2::SpuCapture& b) {
    CHECK(a.image == b.image && a.sha256 == b.sha256);
    CHECK(a.start == b.start && a.end == b.end && a.thread_id == b.thread_id);
    CHECK(a.span_cycles == b.span_cycles && a.ms_since_install == b.ms_since_install);
    CHECK(a.dispatches == b.dispatches && a.sp_start == b.sp_start && a.sp_end == b.sp_end && a.flags == b.flags);
    CHECK(std::memcmp(a.pre_gpr.data(), b.pre_gpr.data(), sizeof(Tox2Vr) * 128) == 0);
    CHECK(std::memcmp(&a.pre_fpscr, &b.pre_fpscr, sizeof(Tox2Vr)) == 0);
    CHECK(a.pre_srr0 == b.pre_srr0 && a.pre_int_enabled == b.pre_int_enabled);
    CHECK(a.pre_ls == b.pre_ls);
    CHECK(std::memcmp(a.post_gpr.data(), b.post_gpr.data(), sizeof(Tox2Vr) * 128) == 0);
    CHECK(std::memcmp(&a.post_fpscr, &b.post_fpscr, sizeof(Tox2Vr)) == 0);
    CHECK(a.post_jump_target == b.post_jump_target);
    CHECK(a.post_ls == b.post_ls);
    CHECK(a.tag_runs.size() == b.tag_runs.size());
    for (std::size_t i = 0; i < std::min(a.tag_runs.size(), b.tag_runs.size()); i++) {
        const tox2::SpuCaptureTagRun& x = a.tag_runs[i];
        const tox2::SpuCaptureTagRun& y = b.tag_runs[i];
        CHECK(x.image == y.image && x.segment == y.segment && x.delta == y.delta && x.lo == y.lo && x.hi == y.hi);
    }
}

void testSpuCaptureFormat() {
    tox2::SpuCapture c;
    c.image = "synth";
    c.sha256 = kNoHash;
    c.start = 0x04cdc;
    c.end = 0x05710;
    c.thread_id = 0x0100abcd;
    c.span_cycles = 0x1122334455667788ull;
    c.ms_since_install = 123456789012ull;
    c.dispatches = 3;
    c.sp_start = 0x3ffd0;
    c.sp_end = 0x3ffe0;
    c.flags = 0;
    for (std::uint32_t r = 0; r < 128; r++) {
        for (std::uint32_t i = 0; i < 16; i++) {
            c.pre_gpr[r].u8[i] = static_cast<std::uint8_t>(r + i);
            c.post_gpr[r].u8[i] = static_cast<std::uint8_t>(r * 3 + i * 5);
        }
    }
    for (std::uint32_t i = 0; i < 16; i++) {
        c.pre_fpscr.u8[i] = static_cast<std::uint8_t>(0x10 + i);
        c.post_fpscr.u8[i] = static_cast<std::uint8_t>(0x90 + i);
    }
    c.pre_srr0 = 0x2468;
    c.pre_int_enabled = 1;
    c.post_jump_target = 0x05710;
    c.pre_ls.resize(kTox2SpuLsSize);
    c.post_ls.resize(kTox2SpuLsSize);
    for (std::uint32_t i = 0; i < kTox2SpuLsSize; i++) {
        c.pre_ls[i] = static_cast<std::uint8_t>(i * 7 + (i >> 9));
        c.post_ls[i] = static_cast<std::uint8_t>(i * 13 + (i >> 11) + 1);
    }
    c.tag_runs = {{"synth", 0, 0, 0x3000, 0x3100},
                  {"synth_b", 0, -0x1000, 0x5000, 0x5020},
                  {"synth", 0, 0x20, 0x8000, 0x8040}};

    std::string error;
    std::vector<std::uint8_t> bytes;
    CHECK(tox2::serializeSpuCapture(c, bytes, error));
    CHECK(bytes.size() == tox2::spuCaptureFileSize(3));
    CHECK(bytes.size() == 160u + 2 * (2080u + 262144u) + 3 * 48u);
    CHECK(std::memcmp(bytes.data(), "TOX2SPUC", 8) == 0);
    CHECK(loadLe32(bytes.data() + 8) == 1 && loadLe32(bytes.data() + 12) == 160);
    CHECK(std::memcmp(bytes.data() + 16, "synth\0", 6) == 0 && bytes[16 + 31] == 0);
    CHECK(loadLe32(bytes.data() + 112) == 0x04cdc && loadLe32(bytes.data() + 116) == 0x05710);
    CHECK(loadLe32(bytes.data() + 120) == 0x0100abcd && loadLe32(bytes.data() + 124) == 3);
    CHECK(std::memcmp(bytes.data() + 160, c.pre_gpr.data(), 2048) == 0);
    CHECK(std::memcmp(bytes.data() + 160 + 2048, &c.pre_fpscr, 16) == 0);
    CHECK(loadLe32(bytes.data() + 160 + 2064) == 0x2468 && loadLe32(bytes.data() + 160 + 2068) == 1);
    CHECK(std::memcmp(bytes.data() + 160 + 2080, c.pre_ls.data(), 262144) == 0);
    CHECK(loadLe32(bytes.data() + 160 + 2080 + 262144 + 2064) == 0x05710);
    CHECK(std::memcmp(bytes.data() + 160 + 2 * 2080 + 262144, c.post_ls.data(), 262144) == 0);

    const std::optional<tox2::SpuCapture> back = tox2::parseSpuCapture(bytes, error);
    CHECK(back.has_value());
    if (back) expectSameCapture(c, *back);
    CHECK(back && back->tag_runs[1].delta == -0x1000);

    // Through a file, validated against the table the replay would register.
    const std::filesystem::path dir = capTestDir();
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "round.spucap";
    CHECK(tox2::writeSpuCapture(path, c, error));
    CHECK(std::filesystem::file_size(path) == bytes.size());
    const std::optional<tox2::SpuCapture> read = tox2::readSpuCapture(path, kSynthImages, 2, error);
    CHECK(read.has_value());
    if (read) expectSameCapture(c, *read);

    // Rejections.
    const auto parseFails = [&](const std::vector<std::uint8_t>& data) {
        std::string why;
        return !tox2::parseSpuCapture(data, why).has_value() && !why.empty();
    };
    std::vector<std::uint8_t> broken = bytes;
    broken[0] = 'X';
    CHECK(parseFails(broken));
    broken = bytes;
    broken[8] = 2;
    CHECK(parseFails(broken));
    broken = bytes;
    broken[12] = 176;
    CHECK(parseFails(broken));
    broken = bytes;
    broken.pop_back();
    CHECK(parseFails(broken));
    broken = bytes;
    broken.push_back(0);
    CHECK(parseFails(broken));
    broken = bytes;
    broken[124] = 4;  // four tag runs announced, three present
    CHECK(parseFails(broken));
    CHECK(parseFails({bytes.begin(), bytes.begin() + 100}));

    tox2::SpuCapture other = c;
    other.image = "synth_c";
    CHECK(!tox2::validateSpuCapture(other, kSynthImages, 2, error));
    other = c;
    other.sha256 = std::string(64, '1');
    CHECK(!tox2::validateSpuCapture(other, kSynthImages, 2, error));
    other = c;
    other.tag_runs[1].image = "nowhere";
    CHECK(!tox2::validateSpuCapture(other, kSynthImages, 2, error));
    other = c;
    other.tag_runs[0].segment = 1;  // a data segment
    CHECK(!tox2::validateSpuCapture(other, kSynthImages, 2, error));
    other = c;
    other.tag_runs[0].hi = kTox2SpuLsSize + 16;
    CHECK(!tox2::validateSpuCapture(other, kSynthImages, 2, error));
    CHECK(tox2::validateSpuCapture(c, kSynthImages, 2, error));

    other = c;
    other.pre_ls.pop_back();
    CHECK(!tox2::serializeSpuCapture(other, bytes, error));
    other = c;
    other.image = std::string(33, 'n');
    CHECK(!tox2::serializeSpuCapture(other, bytes, error));
    CHECK(!tox2::readSpuCapture(dir / "missing.spucap", kSynthImages, 2, error).has_value());

    std::filesystem::remove_all(dir);
}

void testSpuCaptureSelectorParse() {
    std::string error;
    auto sel = tox2::parseSpuCaptureSelector("eboot_skin:0x04cdc:0x05710:dispatches=1", error);
    CHECK(sel && sel->image == "eboot_skin" && sel->starts == std::vector<std::uint32_t>{0x4cdc} &&
          sel->end == 0x5710 && sel->max_dispatches == 1);
    sel = tox2::parseSpuCaptureSelector("eboot_animation:05628+0X05680+56b0:4910", error);
    const std::vector<std::uint32_t> animation_starts = {0x5628, 0x5680, 0x56b0};
    CHECK(sel && sel->image == "eboot_animation" && sel->starts == animation_starts && sel->end == 0x4910 &&
          sel->max_dispatches == 0);

    for (const char* bad : {"", "a", "a:5", "a:5:6:dispatches=1:x", ":5:6", "a::6", "a:5:", "a:5+:6", "a:+5:6",
                            "a:zz:6", "a:5:0x", "a:5:6:", "a:5:6:dispatches=", "a:5:6:dispatches=x",
                            "a:5:6:dispatches=-1", "a:5:6:count=1", "a:100000000:6"}) {
        error.clear();
        CHECK(!tox2::parseSpuCaptureSelector(bad, error).has_value() && !error.empty());
    }
}

void testSpuCaptureInstall() {
    const std::filesystem::path dir = capTestDir();
    std::filesystem::remove_all(dir);
    std::string error;

    // No selector: the original table, whatever the settings.
    tox2::SpuCaptureSettings settings;
    CHECK(tox2::installSpuCapture(kCapImages, 2, {}, settings, error) == kCapImages);

    std::vector<tox2::SpuCaptureSelector> selectors;
    for (const char* text :
         {"capsyn:6000:6010:dispatches=1", "capsyn:6020:6030", "capsyn:6040:6060:dispatches=1", "capsyn:6040:6060"}) {
        auto sel = tox2::parseSpuCaptureSelector(text, error);
        CHECK(sel.has_value());
        if (sel) selectors.push_back(*sel);
    }
    settings.dir = (dir / "a").string();
    settings.count = 4;

    // Each failure leaves no table behind.
    {
        auto bad = selectors;
        bad[0].image = "nowhere";
        CHECK(tox2::installSpuCapture(kCapImages, 2, bad, settings, error) == nullptr && error.contains("nowhere"));
        bad = selectors;
        bad[0].starts = {0x6004};
        error.clear();
        CHECK(tox2::installSpuCapture(kCapImages, 2, bad, settings, error) == nullptr && !error.empty());
        bad = selectors;
        bad[0].end = 0x6014;
        error.clear();
        CHECK(tox2::installSpuCapture(kCapImages, 2, bad, settings, error) == nullptr && !error.empty());
        bad = selectors;
        bad[0].starts = {kCapS, kCapE};
        error.clear();
        CHECK(tox2::installSpuCapture(kCapImages, 2, bad, settings, error) == nullptr && !error.empty());
        tox2::SpuCaptureSettings no_dir = settings;
        no_dir.dir.clear();
        error.clear();
        CHECK(tox2::installSpuCapture(kCapImages, 2, selectors, no_dir, error) == nullptr && !error.empty());
        no_dir = settings;
        no_dir.every = 0;
        error.clear();
        CHECK(tox2::installSpuCapture(kCapImages, 2, selectors, no_dir, error) == nullptr && !error.empty());

        // 17 starts and an end are 18 distinct entries; 16 slots exist.
        std::vector<Tox2SpuEntry> many;
        for (std::uint32_t i = 0; i < 18; i++) many.push_back({0x8000 + 0x10 * i, 1, {0xc0de0100 + i}, capEnd});
        const Tox2SpuSegment seg = {0x8000, 0x8000, 0x120, 1};
        const Tox2SpuImageInfo many_image = {"many", 0x8000, 0x120, kNoHash, 0x8000, 1, &seg, 18, many.data()};
        tox2::SpuCaptureSelector wide;
        wide.image = "many";
        for (std::uint32_t i = 0; i < 17; i++) wide.starts.push_back(0x8000 + 0x10 * i);
        wide.end = 0x8110;
        error.clear();
        CHECK(tox2::installSpuCapture(&many_image, 1, std::span(&wide, 1), settings, error) == nullptr &&
              error.contains("16"));
        wide.starts.resize(15);  // 15 starts and the end are 16 entries
        error.clear();
        CHECK(tox2::installSpuCapture(&many_image, 1, std::span(&wide, 1), settings, error) != nullptr);
    }

    const Tox2SpuImageInfo* table = tox2::installSpuCapture(kCapImages, 2, selectors, settings, error);
    CHECK(table != nullptr && table != kCapImages);
    if (table == nullptr) {
        std::fprintf(stderr, "install failed: %s\n", error.c_str());
        return;
    }
    // Same image data; only the selected entries run another function.
    CHECK(table[1].entries == kCapEntriesB);
    CHECK(table[0].entry_count == 7 && table[0].entries != kCapEntries);
    for (std::uint32_t e = 0; e < 7; e++) {
        CHECK(table[0].entries[e].ls == kCapEntries[e].ls && table[0].entries[e].check[0] == kCapEntries[e].check[0]);
        CHECK((table[0].entries[e].host != kCapEntries[e].host) == (e != 5));
    }
    CHECK(std::string_view(table[0].name) == "capsyn" && table[0].segments == kCapSegments);

    tox2::registerSpuImages(table, 2);

    // A pure span: pre-state is the set-up state, post-state is what the reference model computes.
    CapThread t(0x01000200);
    TOX2_VR_W(t.thread->context().gpr[0], 0) = kCapE;
    std::array<Tox2Vr, 128> pre_gpr;
    std::memcpy(pre_gpr.data(), t.thread->context().gpr, sizeof pre_gpr);
    std::array<Tox2Vr, 128> expect_gpr = pre_gpr;
    const std::vector<std::uint8_t> pre_ls(t.thread->ls(), t.thread->ls() + kTox2SpuLsSize);
    std::vector<std::uint8_t> expect_ls = pre_ls;
    const Tox2Vr pre_fpscr = t.thread->context().fpscr;
    std::vector<std::string> lines;
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    CHECK(t.run(kCapS, kCapE) == tox2::SpuLeave::Returned);
    capModel(expect_gpr, expect_ls);

    const std::filesystem::path file = dir / "a" / "capsyn_06000_06010_0.spucap";
    CHECK(std::filesystem::exists(file));
    const std::optional<tox2::SpuCapture> c = tox2::readSpuCapture(file, kCapImages, 2, error);
    CHECK(c.has_value());
    if (c) {
        CHECK(c->image == "capsyn" && c->sha256 == kNoHash);
        CHECK(c->start == kCapS && c->end == kCapE && c->thread_id == 0x01000200);
        CHECK(c->dispatches == 1 && c->flags == 0 && c->span_cycles > 0);
        CHECK(c->sp_start == TOX2_VR_W(pre_gpr[1], 0) && c->sp_end == c->sp_start);
        CHECK(std::memcmp(c->pre_gpr.data(), pre_gpr.data(), sizeof pre_gpr) == 0);
        CHECK(TOX2_VR_W(c->pre_gpr[4], 0) != TOX2_VR_W(c->post_gpr[4], 0));
        CHECK(std::memcmp(&c->pre_fpscr, &pre_fpscr, sizeof pre_fpscr) == 0);
        CHECK(c->pre_srr0 == 0x1234 && c->pre_int_enabled == 1);
        CHECK(c->pre_ls == pre_ls);
        CHECK(std::memcmp(c->post_gpr.data(), expect_gpr.data(), sizeof expect_gpr) == 0);
        CHECK(std::memcmp(&c->post_fpscr, &pre_fpscr, sizeof pre_fpscr) == 0);
        CHECK(c->post_jump_target == kCapE);
        CHECK(c->post_ls == expect_ls);
        CHECK(c->tag_runs.size() == 2);
        if (c->tag_runs.size() == 2) {
            CHECK(c->tag_runs[0].image == "capsyn" && c->tag_runs[0].segment == 0 && c->tag_runs[0].delta == 0 &&
                  c->tag_runs[0].lo == kCapEa && c->tag_runs[0].hi == kCapEa + kCapCodeSize);
            CHECK(c->tag_runs[1].image == "capsyn_b" && c->tag_runs[1].lo == kCapEaB &&
                  c->tag_runs[1].hi == kCapEaB + 0x20);
        }
    }
    CHECK(!lines.empty() && lines[0].contains("tox2: spu capture: wrote ") &&
          lines[0].contains("capsyn_06000_06010_0") &&
          lines[0].contains("image capsyn start 0x06000 end 0x06010 thread 0x01000200 span cycles "));
    CHECK(!lines.empty() && lines[0].contains("dispatches 1 sp "));

    // A span that issues a GET is not pure: no file for it.
    lines.clear();
    CHECK(t.run(kCapGetS, kCapGetE) == tox2::SpuLeave::Returned);
    CHECK(t.thread->channels().mfc().counters().get == 1);
    CHECK(!std::filesystem::exists(dir / "a" / "capsyn_06020_06030_0.spucap"));
    CHECK(lines.empty());

    // Two dispatches in the span: the selector that allows one rejects it, the one without a limit takes it. They
    // share both entries.
    CHECK(t.run(kCapS3, kCapE3) == tox2::SpuLeave::Returned);
    CHECK(!std::filesystem::exists(dir / "a" / "capsyn_06040_06060_1.spucap"));
    const std::filesystem::path file3 = dir / "a" / "capsyn_06040_06060_0.spucap";
    const std::optional<tox2::SpuCapture> c3 = tox2::readSpuCapture(file3, kCapImages, 2, error);
    CHECK(c3 && c3->dispatches == 2 && c3->start == kCapS3 && c3->end == kCapE3);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "a")) files += entry.is_regular_file() ? 1 : 0;
    CHECK(files == 2);

    // A run that reaches the end entry with no open span writes nothing and does not fail.
    CHECK(t.run(kCapE, kCapE) == tox2::SpuLeave::Returned);
    tox2::setLogSink({});

    // Every second accepted span, two files at most; the third and later spans cost no file.
    tox2::SpuCaptureSettings every = settings;
    every.dir = (dir / "b" / "nested").string();
    every.count = 2;
    every.every = 2;
    auto one = tox2::parseSpuCaptureSelector("capsyn:6000:6010", error);
    CHECK(one.has_value());
    if (!one) return;
    table = tox2::installSpuCapture(kCapImages, 2, std::span(&*one, 1), every, error);
    CHECK(table != nullptr);
    if (table == nullptr) return;
    tox2::registerSpuImages(table, 2);
    CapThread u(0x01000201);
    for (int run = 0; run < 6; run++) CHECK(u.run(kCapS, kCapE) == tox2::SpuLeave::Returned);
    CHECK(std::filesystem::exists(dir / "b" / "nested" / "capsyn_06000_06010_0.spucap"));
    CHECK(std::filesystem::exists(dir / "b" / "nested" / "capsyn_06000_06010_1.spucap"));
    CHECK(!std::filesystem::exists(dir / "b" / "nested" / "capsyn_06000_06010_2.spucap"));
    // The second file holds the fourth span, whose pre-state already carries the first three results.
    const auto second =
            tox2::readSpuCapture(dir / "b" / "nested" / "capsyn_06000_06010_1.spucap", kCapImages, 2, error);
    CHECK(second && second->thread_id == 0x01000201);
    if (second) {
        std::array<Tox2Vr, 128> gpr = second->pre_gpr;
        std::vector<std::uint8_t> ls_model = second->pre_ls;
        capModel(gpr, ls_model);
        CHECK(second->post_ls == ls_model);
        CHECK(std::memcmp(second->post_gpr.data(), gpr.data(), sizeof gpr) == 0);
        CHECK(!c || second->pre_ls != c->pre_ls);
    }

    // Not armed yet.
    tox2::SpuCaptureSettings late = settings;
    late.dir = (dir / "c").string();
    late.after_seconds = 3600;
    table = tox2::installSpuCapture(kCapImages, 2, std::span(&*one, 1), late, error);
    CHECK(table != nullptr);
    if (table == nullptr) return;
    tox2::registerSpuImages(table, 2);
    CapThread v(0x01000202);
    CHECK(v.run(kCapS, kCapE) == tox2::SpuLeave::Returned);
    CHECK(std::filesystem::is_directory(dir / "c") && std::filesystem::is_empty(dir / "c"));

    // A failed write does not consume the count: the directory becomes a regular file after the install, the span
    // writes nothing, and once the directory is back the one file the count allows is still written.
    tox2::SpuCaptureSettings failing = settings;
    failing.dir = (dir / "d").string();
    failing.count = 1;
    table = tox2::installSpuCapture(kCapImages, 2, std::span(&*one, 1), failing, error);
    CHECK(table != nullptr);
    if (table == nullptr) return;
    tox2::registerSpuImages(table, 2);
    std::filesystem::remove(dir / "d");
    {
        std::ofstream blocker(dir / "d");
    }
    CapThread w(0x01000203);
    lines.clear();
    tox2::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    CHECK(w.run(kCapS, kCapE) == tox2::SpuLeave::Returned);
    CHECK(std::filesystem::is_regular_file(dir / "d"));
    CHECK(lines.size() == 1 && lines[0].contains("cannot write"));
    CHECK(!lines.empty() && !lines[0].contains("complete"));
    std::filesystem::remove(dir / "d");
    std::filesystem::create_directories(dir / "d");
    lines.clear();
    for (int run = 0; run < 3; run++) CHECK(w.run(kCapS, kCapE) == tox2::SpuLeave::Returned);
    std::size_t retried = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "d")) retried += entry.is_regular_file() ? 1 : 0;
    CHECK(retried == 1);
    std::size_t complete_lines = 0;
    for (const std::string& line : lines) complete_lines += line.contains("complete, 1 files") ? 1 : 0;
    CHECK(complete_lines == 1);
    tox2::setLogSink({});

    // A span that another SPU thread opened on the same host thread is not closed by this thread's end entry; the
    // opener's own end still closes it.
    tox2::SpuCaptureSettings shared = settings;
    shared.dir = (dir / "e").string();
    table = tox2::installSpuCapture(kCapImages, 2, std::span(&*one, 1), shared, error);
    CHECK(table != nullptr);
    if (table == nullptr) return;
    tox2::registerSpuImages(table, 2);
    CapThread opener(0x01000204);
    CapThread other(0x01000205);
    CHECK(opener.run(kCapS, kCapEaB) == tox2::SpuLeave::Returned);  // never reaches kCapE
    CHECK(other.run(kCapE, kCapE) == tox2::SpuLeave::Returned);
    CHECK(std::filesystem::is_empty(dir / "e"));
    CHECK(opener.run(kCapE, kCapE) == tox2::SpuLeave::Returned);
    const auto closed = tox2::readSpuCapture(dir / "e" / "capsyn_06000_06010_0.spucap", kCapImages, 2, error);
    CHECK(closed && closed->thread_id == 0x01000204);

    registerSynthImages();
    std::filesystem::remove_all(dir);
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
        g_spin_laps = 0;
        CHECK(thread.start(kLsSpin));
        const auto deadline = Clock::now() + milliseconds(5000);
        while (!g_spinning && Clock::now() < deadline) std::this_thread::yield();
        CHECK(g_spinning);
        CHECK(thread.state() == tox2::SpuState::Running);
        // Let the loop run long enough to be in its steady state before the request.
        while (g_spin_laps.load(std::memory_order_relaxed) < 10000 && Clock::now() < deadline)
            std::this_thread::yield();
        thread.requestHalt();
        thread.join();
        CHECK(owner.finished == 2 && owner.how == tox2::SpuLeave::Halt);
        CHECK(thread.state() == tox2::SpuState::Finished);
        // The thread left at the back edge: its site is recorded, and the registers hold the last completed
        // lap (no lap is lost or half stored).
        const auto& ctx = thread.context();
        const std::uint32_t laps = g_spin_laps.load();
        CHECK(laps >= 10000);
        CHECK(ctx.jump_site == kLsSpin + 4);
        CHECK(TOX2_VR_W(ctx.gpr[5], 0) == laps);
        CHECK(TOX2_VR_W(ctx.gpr[6], 0) == ~laps);
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

    // A plain write is noticed one poll later (best of a few tries: the wait only polls for it).
    const double plain_bound_ms = std::chrono::duration<double, std::milli>(tox2::kPlainStorePoll).count() + 5.0;
    double best = 1e9;
    for (int attempt = 0; attempt < 8 && best >= plain_bound_ms; attempt++) {
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
    CHECK(best < plain_bound_ms);
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

// An idle wait for the lost-line event polls rarely: the game's SPURS kernels sit in it for the whole run. At a 1 ms
// poll the 300 ms below gives 150-300 timeouts.
void testChanIdlePollRate() {
    const tox2::TimerResolution timer_resolution;
    CHECK(tox2::kPlainStorePoll >= milliseconds(10));
    CHECK(tox2::kPlainStorePoll <= milliseconds(100));

    ChanFixture f;
    CHECK(f.getllar() == 4);
    CHECK(f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kLockLineLost));
    CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kLockLineLost));
    const tox2::SpuReservationWaits before = tox2::spuReservationWaitSnapshot();
    {
        AsyncRead reader(f, tox2::chan::kRdEventStat);
        CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
        std::this_thread::sleep_for(milliseconds(300));
        f.thread.requestHalt();
        CHECK(waitUntil([&] { return reader.done.load(); }));
        CHECK(!reader.ok);
    }
    const tox2::SpuReservationWaits after = tox2::spuReservationWaitSnapshot();
    CHECK(after.timeout - before.timeout <= 40);
}

// A reservation broken by a plain store is noticed only when the wait times out; one broken through the table ends the
// wait with an epoch change. The counters are process-wide, so each case looks at the change it made.
void testChanReservationWaitCounts() {
    // Plain store: Timeout, then lost.
    {
        ChanFixture f;
        CHECK(f.getllar() == 4);
        CHECK(f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kLockLineLost));
        CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kLockLineLost));
        const tox2::SpuReservationWaits before = tox2::spuReservationWaitSnapshot();
        {
            AsyncRead reader(f, tox2::chan::kRdEventStat);
            CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
            std::this_thread::sleep_for(milliseconds(5));
            f.otherPlainStore();
            CHECK(waitUntil([&] { return reader.done.load(); }));
            CHECK(reader.ok && reader.value == tox2::spu_event::kLockLineLost);
        }
        const tox2::SpuReservationWaits after = tox2::spuReservationWaitSnapshot();
        CHECK(after.lost_wait.count - before.lost_wait.count == 1);
        // The wait that timed out was timed.
        CHECK(after.lost_wait.sum_us > before.lost_wait.sum_us);
        CHECK(after.timeout - before.timeout >= 1);
        CHECK(after.epoch == before.epoch);
        // Nothing moved the stripe version: counted as plain.
        CHECK(after.lost_plain - before.lost_plain == 1);
        CHECK(after.lost_table - before.lost_table == 0);
    }
    // A put that lands after the timeout moves the version. Timing cannot place it there deterministically, so the
    // classification is driven directly.
    {
        const tox2::SpuReservationWaits before = tox2::spuReservationWaitSnapshot();
        tox2::Reservation res;
        tox2::ReservationLoss loss;
        loss.version_moved = true;
        tox2::spuCountLostAfterTimeout(std::chrono::microseconds(1100), 0x1000, 0x01000300, 0x00cf4, res, loss);
        const tox2::SpuReservationWaits after = tox2::spuReservationWaitSnapshot();
        CHECK(after.lost_table - before.lost_table == 1);
        CHECK(after.lost_plain - before.lost_plain == 0);
        CHECK(after.lost_wait.count - before.lost_wait.count == 1);
        CHECK(after.lost_table + after.lost_plain == after.lost_wait.count);
    }
    // Table put: the wait ends with an epoch change. A poll timeout can expire between the version bump and the
    // notify, which shows as a lost-after-timeout instead, so a few tries.
    {
        bool epoch_seen = false;
        for (int attempt = 0; attempt < 8 && !epoch_seen; attempt++) {
            ChanFixture f;
            CHECK(f.getllar() == 4);
            CHECK(f.wr(tox2::chan::kWrEventMask, tox2::spu_event::kLockLineLost));
            CHECK(f.wr(tox2::chan::kWrEventAck, tox2::spu_event::kLockLineLost));
            const tox2::SpuReservationWaits before = tox2::spuReservationWaitSnapshot();
            {
                AsyncRead reader(f, tox2::chan::kRdEventStat);
                CHECK(waitUntil([&] { return f.blockedIn("RdEventStat"); }));
                std::this_thread::sleep_for(milliseconds(5));
                f.otherPutllc();
                CHECK(waitUntil([&] { return reader.done.load(); }));
                CHECK(reader.ok && reader.value == tox2::spu_event::kLockLineLost);
            }
            const tox2::SpuReservationWaits after = tox2::spuReservationWaitSnapshot();
            epoch_seen = after.epoch - before.epoch == 1 && after.lost_wait.count == before.lost_wait.count;
        }
        CHECK(epoch_seen);
    }
}

// Epoch returns are counted by whether the wait slept; the perf-line segment prints the deltas.
void testReservationWaitSplit() {
    const tox2::SpuReservationWaits before = tox2::spuReservationWaitSnapshot();
    tox2::spuCountReservationWait(tox2::StoreWait::Epoch, false);
    tox2::spuCountReservationWait(tox2::StoreWait::Epoch, true);
    tox2::spuCountReservationWait(tox2::StoreWait::Timeout, true);
    const tox2::SpuReservationWaits after = tox2::spuReservationWaitSnapshot();
    CHECK(after.epoch_before_sleep - before.epoch_before_sleep == 1);
    CHECK(after.epoch_woken - before.epoch_woken == 1);
    CHECK(after.epoch - before.epoch == 2);
    CHECK(after.timeout - before.timeout == 1);
    CHECK(after.halt == before.halt);
    CHECK(after.epoch == after.epoch_before_sleep + after.epoch_woken);

    tox2::SpuReservationWaits some;
    some.epoch = 7;
    some.epoch_before_sleep = 5;
    some.epoch_woken = 2;
    some.timeout = 3;
    some.lost_wait.count = 1;
    some.lost_table = 1;
    some.woken_us = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    some.notifies = {10, 20, 30, 40, 50};
    CHECK(tox2::spuReservationWaitSegment(some, {}) ==
          "rsv-wait epoch 7 timeout 3 lost-after-timeout 1 pre-sleep 5 woken 2 "
          "woken-us <2 1 2-5 2 5-10 3 10-20 4 20-50 5 50-100 6 100-500 7 500-1000 8 >1000 9 "
          "notify putllc 10 put 20 putlluc 30 ppu 40 wake 50 lost-after-timeout table 1 plain 0");
    tox2::SpuReservationWaits later = some;
    later.epoch += 1;
    later.woken_us[8] += 2;
    later.notifies[4] += 3;
    CHECK(tox2::spuReservationWaitSegment(later, some) ==
          "rsv-wait epoch 1 timeout 0 lost-after-timeout 0 pre-sleep 0 woken 0 "
          "woken-us <2 0 2-5 0 5-10 0 10-20 0 20-50 0 50-100 0 100-500 0 500-1000 0 >1000 2 "
          "notify putllc 0 put 0 putlluc 0 ppu 0 wake 3 lost-after-timeout table 0 plain 0");
    CHECK(tox2::spuReservationWaitSegment(some, some).empty());
    CHECK(tox2::spuReservationWaitSegment({}, {}).empty());

    // Slept times land in the bucket whose upper bound they are below; the bounds themselves go up a bucket.
    const tox2::SpuReservationWaits hist_before = tox2::spuReservationWaitSnapshot();
    for (const std::int64_t us : {0, 1, 2, 4, 5, 19, 20, 99, 100, 499, 999, 1000, 5000}) {
        tox2::spuCountWokenSleep(std::chrono::microseconds(us));
    }
    const tox2::SpuReservationWaits hist_after = tox2::spuReservationWaitSnapshot();
    const std::array<std::uint64_t, tox2::kWokenBuckets> expected_buckets = {2, 2, 1, 1, 1, 1, 2, 1, 2};
    for (std::size_t i = 0; i < tox2::kWokenBuckets; i++) {
        CHECK(hist_after.woken_us[i] - hist_before.woken_us[i] == expected_buckets[i]);
    }

    const tox2::SpuPutllcFails none{};
    CHECK(tox2::spuPutllcFailSegment(none, none).empty());
    CHECK(tox2::spuPutllcFailSegment({4, 3, 2, 1}, {1, 1, 1, 1}) == "putllc-fail lost 3 locked 2 moved 1 data 0");
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
constexpr std::uint32_t kLvReceive = 0x3f00;      // stop 0x110 in a loop, see lvReceive
constexpr std::uint32_t kLvTryReceive = 0x3f80;   // one stop 0x111, see lvTryReceive
constexpr std::uint32_t kLvCodeSize = 0x1000;
constexpr std::uint32_t kLvDataLs = 0x8000;  // FILL segment, 0x100 bytes of kLvFillValue
constexpr std::uint32_t kLvFillValue = 0xa5a5a5a5;
constexpr std::uint32_t kLvBuf = 0x8200;  // the word of kLvMain; the values of kLvCollect; the replies of lvReceive

constexpr std::uint32_t kLvEventValue = 0x1234;
constexpr std::uint32_t kLvMainStatus = 0x2b;
constexpr std::uint32_t kLvPort = 16;

constexpr std::uint32_t kLvQueueKey = 0x6e754153;  // the key lvReceive and lvTryReceive get as argument 0
constexpr std::uint32_t kLvQuit = 0x51554954;      // data1 that ends lvReceive
constexpr std::uint32_t kLvReceiveRounds = 8;

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

// Receives on the queue bound to the key in argument 0 until a reply is not OK or its first data word is
// kLvQuit, and keeps the four words of every reply in LS at kLvBuf + 16 * round. Exits with the round count.
TOX2_SPU_FUNC(lvReceive) {
    TOX2_SPU_SF_ENTER(kLvReceive);
    const std::uint32_t site = kLvReceive + 4;
    const std::uint32_t key = TOX2_VR_W(ctx->gpr[3], 1);
    std::uint32_t rounds = 0;
    while (rounds < kLvReceiveRounds) {
        std::uint32_t words[4] = {};
        LV_WR(tox2::chan::kWrOutMbox, key);
        LV_STOP(0x110);
        LV_RD(tox2::chan::kRdInMbox, &words[0]);
        if (words[0] == 0) {
            for (std::uint32_t i = 1; i < 4; i++) LV_RD(tox2::chan::kRdInMbox, &words[i]);
        }
        for (std::uint32_t i = 0; i < 4; i++) storeBe32(ls + kLvBuf + 16 * rounds + 4 * i, words[i]);
        rounds++;
        if (words[0] != 0 || words[1] == kLvQuit) break;
    }
    LV_WR(tox2::chan::kWrOutMbox, rounds);
    LV_STOP(0x102);
}

// One stop 0x111 on the key in argument 0; the reply is kept at kLvBuf and its status is the exit status. A
// non-zero argument 1 writes the key twice, so that the thread waits in WrOutMbox until the test took the first.
TOX2_SPU_FUNC(lvTryReceive) {
    TOX2_SPU_SF_ENTER(kLvTryReceive);
    const std::uint32_t site = kLvTryReceive + 4;
    const std::uint32_t key = TOX2_VR_W(ctx->gpr[3], 1);
    std::uint32_t words[4] = {};
    if (TOX2_VR_W(ctx->gpr[4], 1) != 0) LV_WR(tox2::chan::kWrOutMbox, key);
    LV_WR(tox2::chan::kWrOutMbox, key);
    LV_STOP(0x111);
    LV_RD(tox2::chan::kRdInMbox, &words[0]);
    if (words[0] == 0) {
        for (std::uint32_t i = 1; i < 4; i++) LV_RD(tox2::chan::kRdInMbox, &words[i]);
    }
    for (std::uint32_t i = 0; i < 4; i++) storeBe32(ls + kLvBuf + 4 * i, words[i]);
    LV_WR(tox2::chan::kWrOutMbox, words[0]);
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
        {kLvReceive, 1, {0xc0de000f}, lvReceive},
        {kLvTryReceive, 1, {0xc0de0010}, lvTryReceive},
};
Tox2SpuImageInfo g_lv_image = {"lv2test", 0, kLvCodeSize, kNoHash, kLvMain, 1, g_lv_segments, 17, kLvEntries};

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
constexpr std::uint32_t kLvPortOut = 0x150;
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

    // `type` 0 is a group with a scheduler context, which the blocking receive needs.
    std::uint32_t makeGroup(std::uint32_t num, std::uint32_t type = 0x18, std::uint32_t priority = 250) {
        tox2Write32(base, scratch + kLvGroupAttr, 8);
        tox2Write32(base, scratch + kLvGroupAttr + 4, scratch + kLvNames);
        tox2Write32(base, scratch + kLvGroupAttr + 8, type);
        CHECK(ppu.call(170, scratch + kLvIdOut, num, priority, scratch + kLvGroupAttr) == 0);
        groups.push_back(tox2Read32(base, scratch + kLvIdOut));
        return groups.back();
    }

    // Initializes thread `index` of `group` at `entry` with $3 = arg0 (a 64-bit value).
    std::uint32_t makeThread(std::uint32_t group, std::uint32_t index, std::uint32_t entry, std::uint64_t arg0 = 0,
                             std::uint64_t arg1 = 0) {
        tox2Write32(base, scratch + kLvImage + 4, entry);
        tox2Write32(base, scratch + kLvThreadAttr, scratch + kLvNames + 0x80);
        tox2Write32(base, scratch + kLvThreadAttr + 4, 9);
        tox2Write32(base, scratch + kLvThreadAttr + 8, 2);
        tox2Write64(base, scratch + kLvArgs, arg0);
        tox2Write64(base, scratch + kLvArgs + 8, arg1);
        for (std::uint32_t i = 2; i < 4; i++) tox2Write64(base, scratch + kLvArgs + 8 * i, 0);
        CHECK(ppu.call(172, scratch + kLvThreadOut + 4 * index, group, index, scratch + kLvImage,
                       scratch + kLvThreadAttr, scratch + kLvArgs) == 0);
        return tox2Read32(base, scratch + kLvThreadOut + 4 * index);
    }

    std::uint32_t makeQueue(std::uint32_t size = 8, std::uint32_t type = 1) {
        tox2Write32(base, scratch + kLvQueueAttr, 1);
        tox2Write32(base, scratch + kLvQueueAttr + 4, type);
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

    // A local event port connected to `queue`.
    std::uint32_t makePort(std::uint32_t queue) {
        CHECK(ppu.call(134, scratch + kLvPortOut, 1, 0) == 0);
        const std::uint32_t port = tox2Read32(base, scratch + kLvPortOut);
        CHECK(ppu.call(136, port, queue) == 0);
        return port;
    }

    // The word `index` of the LS buffer at kLvBuf (see lvReceive and lvTryReceive).
    std::uint32_t bufWord(std::uint32_t thread_id, std::uint32_t index) {
        std::uint64_t value = 0;
        CHECK(readLs(thread_id, kLvBuf + 4 * index, 4, &value) == 0);
        return static_cast<std::uint32_t>(value);
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

// Host priority of an SPU group's threads: only a group with a scheduler context and a priority number of 31 or
// less is raised.
void testLv2HostPriority() {
    const auto priorityOf = [](std::uint32_t type, std::uint32_t lv2_priority) {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, type, lv2_priority);
        const std::uint32_t thread = f.makeThread(group, 0, kLvWaitMbox);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "RdInMbox"));
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        return tox2::spuThreadById(thread)->hostPriority();
    };
    CHECK(priorityOf(0, 16) == THREAD_PRIORITY_HIGHEST);
    CHECK(priorityOf(0, 31) == THREAD_PRIORITY_HIGHEST);
    CHECK(priorityOf(0, 100) == THREAD_PRIORITY_NORMAL);
    // No scheduler context: the priority number is not even range-checked.
    CHECK(priorityOf(0x18, 16) == THREAD_PRIORITY_NORMAL);

    tox2::setHostPrioritiesEnabled(false);
    CHECK(priorityOf(0, 16) == THREAD_PRIORITY_NORMAL);
    tox2::setHostPrioritiesEnabled(true);
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

// stop 0x110 and 0x111: replies, blocking, wake-up by an event, cancel by destroy and by terminate.
void testLv2ReceiveEvent() {
    constexpr std::uint32_t kEbusy = tox2::kCellEbusy, kEinval = tox2::kCellEinval;

    // Stored events come at once, later ones wake the thread; only the low 32 bits of the data arrive. The
    // SPU number is 1 in a one-thread group, as in the game.
    {
        Lv2Fixture f;
        // Earlier tests leave receive timing under the same thread id.
        tox2::resetCensus();
        // The owner callbacks of the SPU host thread take the guest lock as class Spu.
        const auto spu_holds = [] {
            return tox2::guestLock().stats().classes[static_cast<std::size_t>(tox2::GuestLockClass::Spu)].hold.count;
        };
        const std::uint64_t spu_holds_before = spu_holds();
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 1, kLvReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        const std::uint32_t port = f.makePort(queue);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(138, port, 0x1100000011ull, 0x22, 0x3300000033ull) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "0x110"));
        CHECK(f.ppu.call(130, queue, 0, 1000) == kEinval);  // a PPU thread cannot receive from an SPU queue
        CHECK(f.ppu.call(138, port, 0x44, 0x5500000055ull, 0x66) == 0);
        // The thread leaves the wait, stores the reply and waits again.
        CHECK(waitUntil([&] { return f.bufWord(thread, 5) == 0x44; }));
        CHECK(f.waitUntilBlocked(thread, "0x110"));
        std::this_thread::sleep_for(
                std::chrono::milliseconds(30));  // the third receive waits at least this long for its event
        CHECK(f.ppu.call(138, port, kLvQuit, 0, 0) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 3);
        const std::uint32_t expected[] = {0, 0x11, 0x22, 0x33, 0, 0x44, 0x55, 0x66, 0, kLvQuit, 0, 0};
        for (std::uint32_t i = 0; i < 12; i++) CHECK(f.bufWord(thread, i) == expected[i]);
        CHECK(spu_holds() > spu_holds_before);

        // Three receives replied, so two gaps between a reply and the next arrival were measured.
        {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            const tox2::SpuThread* cpu = tox2::spuThreadById(thread);
            CHECK(cpu->receiveWait().count == 3 && cpu->receiveBusy().count == 2);
            CHECK(cpu->receiveWait().max_us >= 25000);
        }
        const std::vector<tox2::SpuReceiveRow> receive_rows = tox2::spuReceiveSnapshot();
        CHECK(receive_rows.size() == 1);
        if (!receive_rows.empty()) {
            CHECK(receive_rows[0].id == thread && receive_rows[0].name == "lvthread");
            CHECK(receive_rows[0].wait.count == 3 && receive_rows[0].busy.count == 2);
            CHECK(receive_rows[0].wait.max_us >= 25000);
        }
        const std::vector<std::string> census_lines = tox2::spuCensusLines();
        CHECK(std::ranges::any_of(census_lines, [](const std::string& line) {
            return line.contains(" lvthread receive ") && line.contains("busy n 2") && line.contains("wait n 3");
        }));
        std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
        CHECK(tox2::spuThreadById(thread)->stopCounts().at(0x110) == 3);
    }

    // A group type without a scheduler context cannot block; a missing binding gives EINVAL as well.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1);
        const std::uint32_t thread = f.makeThread(group, 0, kLvReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        const std::uint32_t port = f.makePort(queue);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(138, port, 1, 2, 3) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 1);
        CHECK(f.bufWord(thread, 0) == kEinval);
    }
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvReceive, kLvQueueKey);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 1);
        CHECK(f.bufWord(thread, 0) == kEinval);
    }

    // tryreceive: BUSY on an empty queue, the event when there is one, EINVAL once the key is unbound or the
    // queue is gone. The binding survives the runs.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvTryReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        const std::uint32_t port = f.makePort(queue);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.status() == kEbusy && f.bufWord(thread, 0) == kEbusy);

        CHECK(f.ppu.call(138, port, 0xa1, 0xa2, 0xa3) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.status() == 0);
        CHECK(f.bufWord(thread, 1) == 0xa1 && f.bufWord(thread, 2) == 0xa2 && f.bufWord(thread, 3) == 0xa3);

        CHECK(f.ppu.call(138, port, 0xb1, 0xb2, 0xb3) == 0);
        CHECK(f.ppu.call(194, thread, kLvQueueKey) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.status() == kEinval);

        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(129, queue, 0) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.join(group) == 0 && f.status() == kEinval);
    }

    // A word still in the inbound mailbox makes the call answer BUSY, as the only word left.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvTryReceive, kLvQueueKey, 1);
        const std::uint32_t queue = f.makeQueue(8, 2);
        const std::uint32_t port = f.makePort(queue);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(138, port, 1, 2, 3) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "WrOutMbox"));
        CHECK(f.ppu.call(190, thread, 7) == 0);
        {
            std::lock_guard<tox2::GuestLock> lock(tox2::guestLock());
            std::uint32_t out = 0;
            CHECK(tox2::spuThreadById(thread)->channels().takeOutMbox(&out) && out == kLvQueueKey);
        }
        CHECK(f.join(group) == 0 && f.status() == kEbusy);
    }

    // Destroying the queue: refused while the thread waits, otherwise the waiter gets ECANCELED.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "0x110"));
        CHECK(f.ppu.call(129, queue, 0) == tox2::kCellEbusy);
        CHECK(f.waitUntilBlocked(thread, "0x110"));
        CHECK(f.ppu.call(129, queue, 1) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 1);
        CHECK(f.bufWord(thread, 0) == tox2::kCellEcanceled);
    }

    // Terminate ends a waiting thread. The queue keeps no waiter afterwards: the next run finds the event that was
    // sent in between, through the binding that survived.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        const std::uint32_t port = f.makePort(queue);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "0x110"));
        CHECK(f.ppu.call(177, group, 0x66) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 4 && f.status() == 0x66);

        CHECK(f.ppu.call(138, port, 0x77, 0x78, 0x79) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "0x110"));  // the stored event was taken, the next receive waits
        CHECK(f.bufWord(thread, 1) == 0x77 && f.bufWord(thread, 3) == 0x79);
        CHECK(f.ppu.call(138, port, kLvQuit, 0, 0) == 0);
        CHECK(f.join(group) == 0 && f.cause() == 2 && f.status() == 2);
    }

    // A fixture that goes away while a thread waits ends the group.
    {
        Lv2Fixture f;
        const std::uint32_t group = f.makeGroup(1, 0);
        const std::uint32_t thread = f.makeThread(group, 0, kLvReceive, kLvQueueKey);
        const std::uint32_t queue = f.makeQueue(8, 2);
        CHECK(f.ppu.call(193, thread, queue, kLvQueueKey) == 0);
        CHECK(f.ppu.call(173, group) == 0);
        CHECK(f.waitUntilBlocked(thread, "0x110"));
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
    std::string text = tox2::formatCensus();
    // The table and its wait counters are process-wide and their totals depend on earlier tests: check the two
    // lines, then drop them.
    const std::size_t table_at = text.find("\n  reservation table: ");
    CHECK(table_at != std::string::npos);
    if (table_at != std::string::npos) {
        const std::size_t table_end = text.find('\n', table_at + 1);
        const std::size_t waits_at = text.find("\n  reservation waits: epoch ", table_at + 1);
        CHECK(waits_at == table_end);
        const std::size_t waits_end = waits_at == std::string::npos ? waits_at : text.find('\n', waits_at + 1);
        if (waits_at != std::string::npos) {
            const std::string waits_line = text.substr(waits_at, waits_end - waits_at);
            CHECK(waits_line.contains(" (lost after timeout "));
            CHECK(waits_line.contains("table "));
            CHECK(waits_line.contains("plain "));
        }
        text.erase(table_at, waits_end == std::string::npos ? std::string::npos : waits_end - table_at);
    }
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
    testPutMatchesReference();
    testPutPublishesOnce();
    testStoreConditional();
    testPpuReservationIsLineGranular();
    testIncrementStress();
    testCompareWindow();
    testDescribeLoss();
    testUnchangedChunkSurvivesRace();
    testWaitForStoreEpoch();
    testStoreWaitReportsSleep();
    testWaitersAreNotLost();
    testUnrelatedPutDoesNotNotify();
    testWatchLineWakesOnlyItsWaiter();
    testNotifyWriteWakesLineWaiter();
    testFailedWritersDoNotLoseReservation();
    testPutllcFailReasons();
    testNotifyByWriterKind();
    testGetllarIsNotTorn();
    testSharedTable();
    testDispatchChain();
    testDispatchAcrossImages();
    testTrackingIntersection();
    testTrackingIdenticalWrite();
    testTaggedBlockSummary();
    testUntagChangedUnitsRuns();
    testCopyToLsMatchesReference();
    testCopyWrapsAtLsEnd();
    testFirstImageLog();
    testCheckImages();
    testSpuCaptureFormat();
    testSpuCaptureSelectorParse();
    testSpuCaptureInstall();
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
    testChanIdlePollRate();
    testChanReservationWaitCounts();
    testReservationWaitSplit();
    testChanReplaceInMbox();
    testChanHaltOnCondvar();
    testChanStopChildren();
    testLv2GroupRun();
    testLv2GroupExit();
    testLv2HostPriority();
    testLv2Terminate();
    testLv2JoinBeforeStart();
    testLv2Mailbox();
    testLv2InterruptMailbox();
    testLv2ReceiveEvent();
    testLv2Teardown();
    testLv2StopChildren();
    testSpuLogPrefix();
    testSpuFatalChildren();
    testSpuSupervisor();
    testSpuCensus();

    std::printf("spu runtime tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// Runtime hooks that emitted code calls for CPU-level behaviour: reservations, cache and sync
// instructions, the timebase, FPSCR access, and the fatal instruction paths.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/log.h"
#include "core/reservation.h"
#include "tox2/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

std::int64_t queryCounter() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t counterFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

// Initialised during static initialisation, so it is the process start for all practical purposes.
const std::int64_t g_start_counter = queryCounter();

// delta * mul / frequency without a 128-bit division (needs compiler-rt, which is not linked).
// The remainder term stays below frequency * mul, far under 2^64 for a QPC frequency in the MHz.
std::uint64_t scaleCounter(std::uint64_t delta, std::uint64_t mul) {
    const auto frequency = static_cast<std::uint64_t>(counterFrequency());
    return (delta / frequency) * mul + (delta % frequency) * mul / frequency;
}

std::uint64_t elapsedCounter() {
    const std::int64_t now = queryCounter();
    return now > g_start_counter ? static_cast<std::uint64_t>(now - g_start_counter) : 0;
}

// One block per host thread, written only by that thread, so counting adds no shared-cache-line write to the
// reservation hooks. Each counter is an atomic so that the reader's loads are not a data race.
struct alignas(64) ReservationCounters {
    std::atomic<std::uint64_t> lwarx{0}, stwcx{0}, stwcx_failed{0};

    static void bump(std::atomic<std::uint64_t>& counter) {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

std::mutex g_counters_mutex;
// Blocks are never freed: a thread that exits keeps its totals in the sum.
std::vector<ReservationCounters*> g_counters;

ReservationCounters& threadCounters() {
    thread_local ReservationCounters* counters = [] {
        auto* block = new ReservationCounters;
        std::lock_guard lock(g_counters_mutex);
        g_counters.push_back(block);
        return block;
    }();
    return *counters;
}

void countStoreConditional(bool ok) {
    ReservationCounters& counters = threadCounters();
    ReservationCounters::bump(counters.stwcx);
    if (!ok) ReservationCounters::bump(counters.stwcx_failed);
}

}  // namespace

namespace tox2 {

std::uint64_t systemTimeUs() { return scaleCounter(elapsedCounter(), 1000000u); }

PpuReservationStats ppuReservationStats() {
    PpuReservationStats total;
    std::lock_guard lock(g_counters_mutex);
    for (const ReservationCounters* block : g_counters) {
        total.lwarx += block->lwarx.load(std::memory_order_relaxed);
        total.stwcx += block->stwcx.load(std::memory_order_relaxed);
        total.stwcx_failed += block->stwcx_failed.load(std::memory_order_relaxed);
    }
    return total;
}

}  // namespace tox2

extern "C" {

void tox2Unhandled(Tox2PpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    tox2::fatal(tox2::FatalKind::UnhandledInstruction, std::format("word {:#010x} at {:#010x}", word, site));
}

void tox2Trap(Tox2PpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    tox2::fatal(tox2::FatalKind::Trap, std::format("at {:#010x}", site));
}

// A reservation holds while the address matches, memory still contains the value that lwarx/ldarx
// observed (an ABA rewrite of the word is accepted) and the stripe version of the line is unchanged. The
// version moves on every reservation-aware write (PUTLLC, PUTLLUC, MFC PUT through the table, another
// store-conditional), so a write to any word of the line since the lwarx fails the store like it does on
// hardware. Guest loops read neighbouring words of the line between lwarx and stwcx. and rely on that to
// not act on a torn snapshot. Lines that share a stripe can fail the store spuriously; the guest's retry
// loops absorb that. Plain PPU stores to other words of the line still do not cancel the reservation:
// they are not routed through the table (see docs/spu.md). Every store-conditional clears the
// reservation, successful or not. The checks and the write happen under the stripe lock, so they are
// atomic against SPU GETLLAR/PUTLLC and PUT on the same line even though SPU threads run without the guest lock.
uint32_t tox2Lwarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    ReservationCounters::bump(threadCounters().lwarx);
    uint64_t value;
    tox2::reservations().loadReserved(base, ea, 4, value, ctx->reserve_version);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return static_cast<uint32_t>(value);
}

uint64_t tox2Ldarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    ReservationCounters::bump(threadCounters().lwarx);
    uint64_t value;
    tox2::reservations().loadReserved(base, ea, 8, value, ctx->reserve_version);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

bool tox2Stwcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea &&
                    tox2::reservations().storeConditional(base, ea, 4, ctx->reserve_value, ctx->reserve_version, value);
    ctx->reserve_valid = 0;
    countStoreConditional(ok);
    return ok;
}

bool tox2Stdcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea &&
                    tox2::reservations().storeConditional(base, ea, 8, ctx->reserve_value, ctx->reserve_version, value);
    ctx->reserve_valid = 0;
    countStoreConditional(ok);
    return ok;
}

void tox2Dcbz(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    (void)ctx;
    std::memset(base + (ea & ~static_cast<uint32_t>(kTox2DcbzLine - 1)), 0, kTox2DcbzLine);
}

uint64_t tox2Mftb(Tox2PpuContext* ctx) {
    (void)ctx;
    return scaleCounter(elapsedCounter(), tox2::kTimebaseFrequency);
}

uint64_t tox2Mffs(Tox2PpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void tox2Mtfsf(Tox2PpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | (static_cast<uint32_t>(value) & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void tox2Mtfsb(Tox2PpuContext* ctx, uint32_t bit, uint32_t value) {
    const uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"

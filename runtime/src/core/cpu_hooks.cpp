// Runtime hooks that emitted code calls for CPU-level behaviour: reservations, cache and sync
// instructions, the timebase, FPSCR access, and the fatal instruction paths.

#include <cstdint>
#include <cstring>
#include <format>

#include "core/census.h"
#include "core/fatal.h"
#include "core/log.h"
#include "dal/ppu_context.h"

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

}  // namespace

namespace dal {

std::uint64_t systemTimeUs() { return scaleCounter(elapsedCounter(), 1000000u); }

}  // namespace dal

extern "C" {

void dalUnhandled(DalPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    dal::fatal(dal::FatalKind::UnhandledInstruction, std::format("word {:#010x} at {:#010x}", word, site));
}

void dalTrap(DalPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    dal::fatal(dal::FatalKind::Trap, std::format("at {:#010x}", site));
}

// All guest code runs under the guest lock, so a reservation holds while the address matches and
// memory still contains the value that lwarx/ldarx observed (an ABA rewrite is accepted). Every
// store-conditional clears it, successful or not.
uint32_t dalLwarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    const uint32_t value = dalRead32(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

uint64_t dalLdarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    const uint64_t value = dalRead64(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

bool dalStwcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && dalRead32(base, ea) == ctx->reserve_value;
    if (ok) dalWrite32(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

bool dalStdcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && dalRead64(base, ea) == ctx->reserve_value;
    if (ok) dalWrite64(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

void dalDcbz(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    (void)ctx;
    std::memset(base + (ea & ~static_cast<uint32_t>(kDalDcbzLine - 1)), 0, kDalDcbzLine);
}

// Guest threads never run in parallel, so no instruction needs a host barrier.
void dalSync(DalPpuContext* ctx, uint32_t kind) {
    (void)ctx;
    (void)kind;
}

uint64_t dalMftb(DalPpuContext* ctx) {
    (void)ctx;
    return scaleCounter(elapsedCounter(), dal::kTimebaseFrequency);
}

uint64_t dalMffs(DalPpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void dalMtfsf(DalPpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | (static_cast<uint32_t>(value) & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void dalMtfsb(DalPpuContext* ctx, uint32_t bit, uint32_t value) {
    const uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"

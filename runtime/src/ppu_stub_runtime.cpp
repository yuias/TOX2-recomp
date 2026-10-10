// Minimal runtime for the contract hooks in dal/ppu_context.h. It lets generated code be linked
// and exercised without the real runtime: dispatch works, everything that would need the system
// (HLE calls, unknown targets, unhandled instructions) reports and exits.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dal/ppu_context.h"
#include "dal/ppu_stub.h"

// Generated code defines these. They are weak so that targets without generated code still link;
// dalHleCall then reports the bare index.
extern "C" {
extern const DalImportInfo kDalPpuImports[] __attribute__((weak));
extern const uint32_t kDalPpuImportCount __attribute__((weak));
}

namespace {

// Function-local so registration is safe during static initialisation of other translation units.
std::vector<DalFuncMapping>& funcTable() {
    static std::vector<DalFuncMapping> table;
    return table;
}

// std::abort raises a modal "Debug Error!" dialog in debug CRT builds, which stalls unattended test
// runs; exit with a status code instead.
const char* g_label = nullptr;

[[noreturn]] void fatalExit() {
    if (g_label != nullptr) std::fprintf(stderr, "dal: while running %s\n", g_label);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace

extern "C" {

void dalStubResetFuncs(void) { funcTable().clear(); }

void dalStubSetLabel(const char* label) { g_label = label; }

void dalRegisterFuncs(const DalFuncMapping* mappings, uint32_t count) {
    std::vector<DalFuncMapping>& table = funcTable();
    table.insert(table.end(), mappings, mappings + count);
    std::stable_sort(table.begin(), table.end(),
                     [](const DalFuncMapping& a, const DalFuncMapping& b) { return a.guest < b.guest; });
}

DalPpuFunc* dalLookup(uint32_t addr) {
    const std::vector<DalFuncMapping>& table = funcTable();
    auto it = std::lower_bound(table.begin(), table.end(), addr,
                               [](const DalFuncMapping& m, uint32_t a) { return m.guest < a; });
    return (it != table.end() && it->guest == addr) ? it->host : nullptr;
}

void dalUnknownTarget(DalPpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "dal: unknown dynamic target 0x%08x (from 0x%08x)\n", target, site);
    fatalExit();
}

// The musttail keeps dynamic transfers from adding a host frame per hop.
void dalDispatch(DalPpuContext* __restrict ctx, uint8_t* __restrict base) {
    DalPpuFunc* f = dalLookup(ctx->jump_target);
    if (f != nullptr) {
        [[clang::musttail]] return f(ctx, base);
    }
    dalUnknownTarget(ctx, base, ctx->jump_target, ctx->jump_site);
}

void dalHleCall(DalPpuContext* ctx, uint8_t* base, uint32_t import_index) {
    (void)ctx;
    (void)base;
    if (kDalPpuImports != nullptr && &kDalPpuImportCount != nullptr && import_index < kDalPpuImportCount) {
        const DalImportInfo& info = kDalPpuImports[import_index];
        std::fprintf(stderr, "dal: HLE call %s::%s (import %u, stub 0x%08x, fnid 0x%08x)\n", info.module, info.name,
                     import_index, info.stub, info.fnid);
    } else {
        std::fprintf(stderr, "dal: HLE call, import index %u\n", import_index);
    }
    fatalExit();
}

// Reports success so that a harness can observe the call.
void dalSyscall(DalPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)base;
    (void)site;
    ctx->gpr[3] = 0;
}

void dalYield(DalPpuContext* ctx, uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kDalPreemptBudget;
}

void dalUnhandled(DalPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "dal: unhandled instruction 0x%08x at 0x%08x\n", word, site);
    fatalExit();
}

void dalTrap(DalPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "dal: trap at 0x%08x\n", site);
    fatalExit();
}

// Single-threaded reservation: it holds while the address matches and memory still contains the
// value that lwarx/ldarx observed. Every store-conditional clears it, successful or not.
uint32_t dalLwarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint32_t value = dalRead32(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

uint64_t dalLdarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint64_t value = dalRead64(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

bool dalStwcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && dalRead32(base, ea) == ctx->reserve_value;
    if (ok) dalWrite32(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

bool dalStdcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && dalRead64(base, ea) == ctx->reserve_value;
    if (ok) dalWrite64(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

void dalDcbz(DalPpuContext* ctx, uint8_t* base, uint32_t ea) {
    (void)ctx;
    std::memset(base + (ea & ~(uint32_t)(kDalDcbzLine - 1)), 0, kDalDcbzLine);
}

void dalSync(DalPpuContext* ctx, uint32_t kind) {
    (void)ctx;
    (void)kind;
}

uint64_t dalMftb(DalPpuContext* ctx) {
    (void)ctx;
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t dalMffs(DalPpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void dalMtfsf(DalPpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | ((uint32_t)value & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void dalMtfsb(DalPpuContext* ctx, uint32_t bit, uint32_t value) {
    uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"

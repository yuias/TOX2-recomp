// Minimal runtime for the contract hooks in tox2/ppu_context.h. It lets generated code be linked
// and exercised without the real runtime: dispatch works, everything that would need the system
// (HLE calls, unknown targets, unhandled instructions) reports and exits.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tox2/ppu_context.h"
#include "tox2/ppu_stub.h"

// Generated code defines these. They are weak so that targets without generated code still link;
// tox2HleCall then reports the bare index.
extern "C" {
extern const Tox2ImportInfo kTox2PpuImports[] __attribute__((weak));
extern const uint32_t kTox2PpuImportCount __attribute__((weak));
}

namespace {

// Function-local so registration is safe during static initialisation of other translation units.
std::vector<Tox2FuncMapping>& funcTable() {
    static std::vector<Tox2FuncMapping> table;
    return table;
}

// std::abort raises a modal "Debug Error!" dialog in debug CRT builds, which stalls unattended test
// runs; exit with a status code instead.
const char* g_label = nullptr;

[[noreturn]] void fatalExit() {
    if (g_label != nullptr) std::fprintf(stderr, "tox2: while running %s\n", g_label);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace

extern "C" {

void tox2StubResetFuncs(void) { funcTable().clear(); }

void tox2StubSetLabel(const char* label) { g_label = label; }

void tox2RegisterFuncs(const Tox2FuncMapping* mappings, uint32_t count) {
    std::vector<Tox2FuncMapping>& table = funcTable();
    table.insert(table.end(), mappings, mappings + count);
    std::stable_sort(table.begin(), table.end(),
                     [](const Tox2FuncMapping& a, const Tox2FuncMapping& b) { return a.guest < b.guest; });
}

Tox2PpuFunc* tox2Lookup(uint32_t addr) {
    const std::vector<Tox2FuncMapping>& table = funcTable();
    auto it = std::lower_bound(table.begin(), table.end(), addr,
                               [](const Tox2FuncMapping& m, uint32_t a) { return m.guest < a; });
    return (it != table.end() && it->guest == addr) ? it->host : nullptr;
}

void tox2UnknownTarget(Tox2PpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "tox2: unknown dynamic target 0x%08x (from 0x%08x)\n", target, site);
    fatalExit();
}

// The musttail keeps dynamic transfers from adding a host frame per hop.
void tox2Dispatch(Tox2PpuContext* __restrict ctx, uint8_t* __restrict base) {
    Tox2PpuFunc* f = tox2Lookup(ctx->jump_target);
    if (f != nullptr) {
        [[clang::musttail]] return f(ctx, base);
    }
    tox2UnknownTarget(ctx, base, ctx->jump_target, ctx->jump_site);
}

void tox2HleCall(Tox2PpuContext* ctx, uint8_t* base, uint32_t import_index) {
    (void)ctx;
    (void)base;
    if (kTox2PpuImports != nullptr && &kTox2PpuImportCount != nullptr && import_index < kTox2PpuImportCount) {
        const Tox2ImportInfo& info = kTox2PpuImports[import_index];
        std::fprintf(stderr, "tox2: HLE call %s::%s (import %u, stub 0x%08x, fnid 0x%08x)\n", info.module, info.name,
                     import_index, info.stub, info.fnid);
    } else {
        std::fprintf(stderr, "tox2: HLE call, import index %u\n", import_index);
    }
    fatalExit();
}

void tox2HleCallImport(Tox2PpuContext* ctx, uint8_t* base, const Tox2ImportInfo* import) {
    (void)ctx;
    (void)base;
    const char* name = import->name != nullptr ? import->name : "-";
    std::fprintf(stderr, "tox2: HLE call %s::%s (stub 0x%08x, fnid 0x%08x)\n", import->module, name, import->stub,
                 import->fnid);
    fatalExit();
}

// Reports success so that a harness can observe the call.
void tox2Syscall(Tox2PpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)base;
    (void)site;
    ctx->gpr[3] = 0;
}

void tox2Yield(Tox2PpuContext* ctx, uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kTox2PreemptBudget;
}

void tox2Unhandled(Tox2PpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "tox2: unhandled instruction 0x%08x at 0x%08x\n", word, site);
    fatalExit();
}

void tox2Trap(Tox2PpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "tox2: trap at 0x%08x\n", site);
    fatalExit();
}

// Single-threaded reservation: it holds while the address matches and memory still contains the
// value that lwarx/ldarx observed. Every store-conditional clears it, successful or not. There is no
// reservation table here, so reserve_version stays unused (the real runtime checks it per line).
uint32_t tox2Lwarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint32_t value = tox2Read32(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

uint64_t tox2Ldarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint64_t value = tox2Read64(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

bool tox2Stwcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && tox2Read32(base, ea) == ctx->reserve_value;
    if (ok) tox2Write32(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

bool tox2Stdcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && tox2Read64(base, ea) == ctx->reserve_value;
    if (ok) tox2Write64(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

void tox2Dcbz(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea) {
    (void)ctx;
    std::memset(base + (ea & ~(uint32_t)(kTox2DcbzLine - 1)), 0, kTox2DcbzLine);
}

uint64_t tox2Mftb(Tox2PpuContext* ctx) {
    (void)ctx;
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t tox2Mffs(Tox2PpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void tox2Mtfsf(Tox2PpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | ((uint32_t)value & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void tox2Mtfsb(Tox2PpuContext* ctx, uint32_t bit, uint32_t value) {
    uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"

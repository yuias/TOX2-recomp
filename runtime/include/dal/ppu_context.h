#pragma once

// Contract between generated PPU code and the runtime: types, the CPU context, runtime hooks, and
// the small inline helpers that emitted code calls. Generated code is C23; the runtime is C++26,
// so this header compiles as both (it is wrapped in extern "C" for C++). Only <stdint.h> and
// <stddef.h> are included; clang builtins replace the C library string and math headers so that
// each generated translation unit parses little and the header can be checked with -ffreestanding.
//
// Naming: functions camelCase, types PascalCase, constants kPascalCase (as enumerators), macros
// DAL_*. The generated code adds address-derived identifiers that deliberately do not follow the
// table: fn_%08x (functions), imp_%08x (import trampolines), L_%08x (labels), and the semantics
// harness uses case_<name>.
//
// Guest memory: `base` points at a flat, big-endian guest address space. The runtime must reserve
// 4 GiB + kDalGuestGuardSize bytes so that an unaligned access at the top of the 32-bit space
// stays inside the allocation. All guest accesses go through dalRead*/dalWrite* (any alignment).
//
// Vector registers: host lane i holds big-endian element (n - 1 - i), i.e. the whole 16-byte
// register is byte-reversed relative to guest memory (RPCS3 PPUInterpreter / PPUThread.h `v128`
// byte reversal; XenonRecomp `XenonUtils/ppc_context.h` does the same). Element-wise operations
// then need no index mapping; use the DAL_VR_* accessors for big-endian element numbers.
//
// Condition register: one byte (0 or 1) per CR bit, bit 4*field + {0 lt, 1 gt, 2 eq, 3 so/un}, as
// in RPCS3 `cr_bits` and XenonRecomp `PPCCRRegister`. XER is unpacked the same way.
//
// HLE boundary: there is no per-import function. Each import is an index into the generated
// kDalPpuImports table (stub address, FNID, module, name); emitted code calls DAL_HLE(index) and
// the runtime resolves (module, FNID) to an implementation.

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DAL_FORCEINLINE static inline __attribute__((always_inline))

enum DalPpuLimits { kDalTraceSize = 64, kDalDcbzLine = 128, kDalPreemptBudget = 4096, kDalGuestGuardSize = 0x10000 };

enum DalSyncKind { kDalSyncHeavy = 0, kDalSyncLight = 1, kDalSyncEieio = 2, kDalSyncIsync = 3 };

typedef uint8_t DalU8x16 __attribute__((vector_size(16)));
typedef int8_t DalS8x16 __attribute__((vector_size(16)));
typedef uint16_t DalU16x8 __attribute__((vector_size(16)));
typedef int16_t DalS16x8 __attribute__((vector_size(16)));
typedef uint32_t DalU32x4 __attribute__((vector_size(16)));
typedef int32_t DalS32x4 __attribute__((vector_size(16)));
typedef uint64_t DalU64x2 __attribute__((vector_size(16)));
typedef float DalF32x4 __attribute__((vector_size(16)));

typedef union DalVr {
    DalU8x16 u8;
    DalS8x16 s8;
    DalU16x8 u16;
    DalS16x8 s16;
    DalU32x4 u32;
    DalS32x4 s32;
    DalU64x2 u64;
    DalF32x4 f32;
} DalVr;

// Big-endian element i of a register (see the lane layout note above).
#define DAL_VR_B(v, i) ((v).u8[15 - (i)])
#define DAL_VR_SB(v, i) ((v).s8[15 - (i)])
#define DAL_VR_H(v, i) ((v).u16[7 - (i)])
#define DAL_VR_SH(v, i) ((v).s16[7 - (i)])
#define DAL_VR_W(v, i) ((v).u32[3 - (i)])
#define DAL_VR_SW(v, i) ((v).s32[3 - (i)])
#define DAL_VR_F(v, i) ((v).f32[3 - (i)])
#define DAL_VR_D(v, i) ((v).u64[1 - (i)])

// Both views are needed: fctiwz/stfiwx/fneg operate on the bit pattern.
typedef union DalFpr {
    double f64;
    uint64_t u64;
} DalFpr;

typedef struct DalPpuContext {
    uint64_t gpr[32];
    DalFpr fpr[32];
    DalVr vr[32];
    uint8_t cr[32];
    uint64_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_count;
    uint32_t fpscr;
    uint32_t vrsave;
    uint8_t vscr_nj, vscr_sat;        // NJ defaults to 1 (RPCS3 PPUThread.h `nj = true`); SAT is sticky
    uint32_t jump_target, jump_site;  // dynamic transfers (input of dalDispatch)
    uint32_t reserve_addr;
    uint64_t reserve_value;
    uint8_t reserve_valid;
    int32_t preempt_budget;
    uint32_t trace_pos;
    uint32_t trace[kDalTraceSize];
    void* runtime;  // owned by the runtime (thread object)
} DalPpuContext;

static_assert(sizeof(DalVr) == 16, "DalVr must be 16 bytes");
static_assert(alignof(DalVr) == 16, "DalVr must be 16-byte aligned");
static_assert(sizeof(DalFpr) == 8, "DalFpr must be 8 bytes");
static_assert((kDalTraceSize & (kDalTraceSize - 1)) == 0, "trace ring size must be a power of two");
static_assert(offsetof(DalPpuContext, vr) % 16 == 0, "vr[] must stay 16-byte aligned");

typedef void DalPpuFunc(DalPpuContext* __restrict ctx, uint8_t* __restrict base);
typedef struct DalFuncMapping {
    uint32_t guest;
    DalPpuFunc* host;
} DalFuncMapping;
typedef struct DalImportInfo {
    uint32_t stub;
    uint32_t fnid;
    const char* module;
    const char* name;
} DalImportInfo;

#define DAL_PPU_FUNC(name) DAL_NOINLINE void name(DalPpuContext* __restrict ctx, uint8_t* __restrict base)

// Runtime hooks, implemented by the runtime.
void dalDispatch(DalPpuContext* __restrict ctx, uint8_t* __restrict base);  // target = jump_target
void dalUnknownTarget(DalPpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site);
void dalRegisterFuncs(const DalFuncMapping* mappings, uint32_t count);
DalPpuFunc* dalLookup(uint32_t addr);  // nullptr when unknown
void dalHleCall(DalPpuContext* ctx, uint8_t* base, uint32_t import_index);
void dalSyscall(DalPpuContext* ctx, uint8_t* base, uint32_t site);  // number read from r11
void dalYield(DalPpuContext* ctx, uint8_t* base);
[[noreturn]] void dalUnhandled(DalPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word);
[[noreturn]] void dalTrap(DalPpuContext* ctx, uint8_t* base, uint32_t site);
uint32_t dalLwarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea);
uint64_t dalLdarx(DalPpuContext* ctx, uint8_t* base, uint32_t ea);
bool dalStwcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value);
bool dalStdcx(DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value);
void dalDcbz(DalPpuContext* ctx, uint8_t* base, uint32_t ea);
void dalSync(DalPpuContext* ctx, uint32_t kind);  // DalSyncKind
uint64_t dalMftb(DalPpuContext* ctx);
uint64_t dalMffs(DalPpuContext* ctx);                                    // FPSCR image in the low 32 bits
void dalMtfsf(DalPpuContext* ctx, uint32_t field_mask, uint64_t value);  // bit 0x80 >> i = field i
void dalMtfsb(DalPpuContext* ctx, uint32_t bit, uint32_t value);

// Defined by generated code (ppu_tables.c).
extern const DalFuncMapping kDalPpuFuncMappings[];
extern const uint32_t kDalPpuFuncMappingCount;
extern const DalImportInfo kDalPpuImports[];
extern const uint32_t kDalPpuImportCount;
extern const char kDalPpuExecutableSha256[];

// Guest memory. Addresses are uint32_t (the guest address space is 32-bit); every access is
// big-endian and may be unaligned.
DAL_FORCEINLINE uint8_t dalRead8(const uint8_t* base, uint32_t ea) { return base[ea]; }

DAL_FORCEINLINE uint16_t dalRead16(const uint8_t* base, uint32_t ea) {
    uint16_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap16(v);
}

DAL_FORCEINLINE uint32_t dalRead32(const uint8_t* base, uint32_t ea) {
    uint32_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap32(v);
}

DAL_FORCEINLINE uint64_t dalRead64(const uint8_t* base, uint32_t ea) {
    uint64_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap64(v);
}

DAL_FORCEINLINE void dalWrite8(uint8_t* base, uint32_t ea, uint8_t value) { base[ea] = value; }

DAL_FORCEINLINE void dalWrite16(uint8_t* base, uint32_t ea, uint16_t value) {
    value = __builtin_bswap16(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

DAL_FORCEINLINE void dalWrite32(uint8_t* base, uint32_t ea, uint32_t value) {
    value = __builtin_bswap32(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

DAL_FORCEINLINE void dalWrite64(uint8_t* base, uint32_t ea, uint64_t value) {
    value = __builtin_bswap64(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

// lvx/stvx semantics: the low four address bits are ignored.
DAL_FORCEINLINE DalVr dalReadVr(const uint8_t* base, uint32_t ea) {
    DalU8x16 raw;
    __builtin_memcpy(&raw, base + (ea & ~15u), sizeof raw);
    DalVr v;
    v.u8 = __builtin_shufflevector(raw, raw, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    return v;
}

DAL_FORCEINLINE void dalWriteVr(uint8_t* base, uint32_t ea, DalVr value) {
    DalU8x16 raw = __builtin_shufflevector(value.u8, value.u8, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __builtin_memcpy(base + (ea & ~15u), &raw, sizeof raw);
}

// Words are given in big-endian element order (w0 is element 0).
DAL_FORCEINLINE DalVr dalVrFromBeWords(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    DalU32x4 lanes = {w3, w2, w1, w0};
    DalVr v;
    v.u32 = lanes;
    return v;
}

DAL_FORCEINLINE uint32_t dalVrBeWord(DalVr v, uint32_t i) { return v.u32[3 - (i & 3)]; }

// Condition register and XER.
DAL_FORCEINLINE void dalSetCrField(DalPpuContext* ctx, uint32_t field, uint32_t nibble) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = (nibble >> 3) & 1;
    bits[1] = (nibble >> 2) & 1;
    bits[2] = (nibble >> 1) & 1;
    bits[3] = nibble & 1;
}

DAL_FORCEINLINE uint32_t dalGetCrField(const DalPpuContext* ctx, uint32_t field) {
    const uint8_t* bits = &ctx->cr[4 * field];
    return ((uint32_t)bits[0] << 3) | ((uint32_t)bits[1] << 2) | ((uint32_t)bits[2] << 1) | bits[3];
}

DAL_FORCEINLINE void dalCmpS(DalPpuContext* ctx, uint32_t field, int64_t a, int64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

DAL_FORCEINLINE void dalCmpU(DalPpuContext* ctx, uint32_t field, uint64_t a, uint64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

// Rc=1 in 64-bit mode: CR0 from the signed 64-bit result (RPCS3 `ppu_cr_set<s64>`).
DAL_FORCEINLINE void dalRecord(DalPpuContext* ctx, uint64_t r) { dalCmpS(ctx, 0, (int64_t)r, 0); }

DAL_FORCEINLINE uint32_t dalMfcr(const DalPpuContext* ctx) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 32; i++) r |= (uint32_t)(ctx->cr[i] & 1) << (31 - i);
    return r;
}

// crm bit 0x80 >> f selects field f.
DAL_FORCEINLINE void dalMtcrf(DalPpuContext* ctx, uint32_t crm, uint32_t value) {
    for (uint32_t f = 0; f < 8; f++) {
        if (crm & (0x80u >> f)) dalSetCrField(ctx, f, (value >> (28 - 4 * f)) & 15);
    }
}

DAL_FORCEINLINE uint64_t dalMfxer(const DalPpuContext* ctx) {
    return ((uint64_t)(ctx->xer_so & 1) << 31) | ((uint64_t)(ctx->xer_ov & 1) << 30) |
           ((uint64_t)(ctx->xer_ca & 1) << 29) | (ctx->xer_count & 0x7f);
}

DAL_FORCEINLINE void dalMtxer(DalPpuContext* ctx, uint64_t value) {
    ctx->xer_so = (value >> 31) & 1;
    ctx->xer_ov = (value >> 30) & 1;
    ctx->xer_ca = (value >> 29) & 1;
    ctx->xer_count = value & 0x7f;
}

// OV = ov; SO is sticky.
DAL_FORCEINLINE void dalSetOv(DalPpuContext* ctx, bool ov) {
    ctx->xer_ov = ov;
    ctx->xer_so = ctx->xer_so | ctx->xer_ov;
}

// Function entry and loop back edges: the only places where the runtime may preempt.
DAL_FORCEINLINE void dalLoop(DalPpuContext* ctx, uint8_t* base) {
    if (--ctx->preempt_budget < 0) dalYield(ctx, base);
}

DAL_FORCEINLINE void dalEnter(DalPpuContext* ctx, uint8_t* base, uint32_t addr) {
    ctx->trace[ctx->trace_pos & (kDalTraceSize - 1)] = addr;
    ctx->trace_pos++;
    dalLoop(ctx, base);
}

// Emitted code names its parameters `ctx` and `base`; the macros rely on that.
#define DAL_ENTER(addr) dalEnter(ctx, base, (addr))
#define DAL_LOOP(site)      \
    do {                    \
        (void)(site);       \
        dalLoop(ctx, base); \
    } while (0)
#define DAL_CALL(fn, ret) \
    do {                  \
        ctx->lr = (ret);  \
        fn(ctx, base);    \
    } while (0)
// The target is evaluated before LR is written (bclrl may target LR itself).
#define DAL_CALL_DYN(site, target, ret)       \
    do {                                      \
        uint32_t dal_t_ = (uint32_t)(target); \
        ctx->lr = (ret);                      \
        ctx->jump_site = (site);              \
        ctx->jump_target = dal_t_;            \
        dalDispatch(ctx, base);               \
    } while (0)
#define DAL_TAIL(fn) DAL_MUSTTAIL return fn(ctx, base)
#define DAL_TAIL_DYN(site, target)                  \
    do {                                            \
        ctx->jump_site = (site);                    \
        ctx->jump_target = (uint32_t)(target);      \
        DAL_MUSTTAIL return dalDispatch(ctx, base); \
    } while (0)
#define DAL_HLE(index) dalHleCall(ctx, base, (index))
#define DAL_SYSCALL(site) dalSyscall(ctx, base, (site))
#define DAL_TRAP(site) dalTrap(ctx, base, (site))
#define DAL_UNHANDLED(site, word) dalUnhandled(ctx, base, (site), (word))

#ifdef __cplusplus
}  // extern "C"
#endif

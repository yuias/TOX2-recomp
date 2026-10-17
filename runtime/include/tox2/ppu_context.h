#pragma once

// Contract between generated PPU code and the runtime: types, the CPU context, runtime hooks, and
// the small inline helpers that emitted code calls. Generated code is C23; the runtime is C++26,
// so this header compiles as both (it is wrapped in extern "C" for C++). Only <stdint.h> and
// <stddef.h> are included; clang builtins replace the C library string and math headers so that
// each generated translation unit parses little and the header can be checked with -ffreestanding.
//
// Naming: functions camelCase, types PascalCase, constants kPascalCase (as enumerators), macros
// TOX2_*. The generated code adds address-derived identifiers that deliberately do not follow the
// table: fn_%08x (functions), imp_%08x (import trampolines), L_%08x (labels), and the semantics
// harness uses case_<name>. A module other than the main executable has its functions and
// trampolines prefixed with the module's symbol prefix (libsre_fn_%08x).
//
// Guest memory: `base` points at a flat, big-endian guest address space. The runtime must reserve
// 4 GiB + kTox2GuestGuardSize bytes so that an unaligned access at the top of the 32-bit space
// stays inside the allocation. All guest accesses go through tox2Read*/tox2Write* (any alignment).
//
// Vector registers: host lane i holds big-endian element (n - 1 - i), i.e. the whole 16-byte
// register is byte-reversed relative to guest memory (RPCS3 PPUInterpreter / PPUThread.h `v128`
// byte reversal; XenonRecomp `XenonUtils/ppc_context.h` does the same). Element-wise operations
// then need no index mapping; use the TOX2_VR_* accessors for big-endian element numbers.
//
// Condition register: one byte (0 or 1) per CR bit, bit 4*field + {0 lt, 1 gt, 2 eq, 3 so/un}, as
// in RPCS3 `cr_bits` and XenonRecomp `PPCCRRegister`. XER is unpacked the same way.
//
// HLE boundary: there is no per-import function. Each import is an index into the generated
// kTox2PpuImports table (stub address, FNID, module, name); emitted code calls TOX2_HLE(index) and
// the runtime resolves (module, FNID) to an implementation. A module's trampolines call
// TOX2_HLE_IMPORT with a pointer into that module's own import table, so the indices of two images
// never collide.

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOX2_FORCEINLINE static inline __attribute__((always_inline))

enum Tox2PpuLimits {
    kTox2TraceSize = 64,
    kTox2DcbzLine = 128,
    kTox2PreemptBudget = 4096,
    kTox2GuestGuardSize = 0x10000
};

enum Tox2SyncKind { kTox2SyncHeavy = 0, kTox2SyncLight = 1, kTox2SyncEieio = 2, kTox2SyncIsync = 3 };

typedef uint8_t Tox2U8x16 __attribute__((vector_size(16)));
typedef int8_t Tox2S8x16 __attribute__((vector_size(16)));
typedef uint16_t Tox2U16x8 __attribute__((vector_size(16)));
typedef int16_t Tox2S16x8 __attribute__((vector_size(16)));
typedef uint32_t Tox2U32x4 __attribute__((vector_size(16)));
typedef int32_t Tox2S32x4 __attribute__((vector_size(16)));
typedef uint64_t Tox2U64x2 __attribute__((vector_size(16)));
typedef float Tox2F32x4 __attribute__((vector_size(16)));

typedef union Tox2Vr {
    Tox2U8x16 u8;
    Tox2S8x16 s8;
    Tox2U16x8 u16;
    Tox2S16x8 s16;
    Tox2U32x4 u32;
    Tox2S32x4 s32;
    Tox2U64x2 u64;
    Tox2F32x4 f32;
} Tox2Vr;

// Big-endian element i of a register (see the lane layout note above).
#define TOX2_VR_B(v, i) ((v).u8[15 - (i)])
#define TOX2_VR_SB(v, i) ((v).s8[15 - (i)])
#define TOX2_VR_H(v, i) ((v).u16[7 - (i)])
#define TOX2_VR_SH(v, i) ((v).s16[7 - (i)])
#define TOX2_VR_W(v, i) ((v).u32[3 - (i)])
#define TOX2_VR_SW(v, i) ((v).s32[3 - (i)])
#define TOX2_VR_F(v, i) ((v).f32[3 - (i)])
#define TOX2_VR_D(v, i) ((v).u64[1 - (i)])

// Both views are needed: fctiwz/stfiwx/fneg operate on the bit pattern.
typedef union Tox2Fpr {
    double f64;
    uint64_t u64;
} Tox2Fpr;

typedef struct Tox2PpuContext {
    uint64_t gpr[32];
    Tox2Fpr fpr[32];
    Tox2Vr vr[32];
    uint8_t cr[32];
    uint64_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_count;
    uint32_t fpscr;
    uint32_t vrsave;
    uint8_t vscr_nj, vscr_sat;        // NJ defaults to 1 (RPCS3 PPUThread.h `nj = true`); SAT is sticky
    uint32_t jump_target, jump_site;  // dynamic transfers (input of tox2Dispatch)
    uint32_t reserve_addr;
    uint64_t reserve_value;
    uint64_t reserve_version;  // reservation stripe version at lwarx/ldarx
    uint8_t reserve_valid;
    int32_t preempt_budget;
    uint32_t trace_pos;
    uint32_t trace[kTox2TraceSize];
    void* runtime;  // owned by the runtime (thread object)
} Tox2PpuContext;

static_assert(sizeof(Tox2Vr) == 16, "Tox2Vr must be 16 bytes");
static_assert(alignof(Tox2Vr) == 16, "Tox2Vr must be 16-byte aligned");
static_assert(sizeof(Tox2Fpr) == 8, "Tox2Fpr must be 8 bytes");
static_assert((kTox2TraceSize & (kTox2TraceSize - 1)) == 0, "trace ring size must be a power of two");
static_assert(offsetof(Tox2PpuContext, vr) % 16 == 0, "vr[] must stay 16-byte aligned");

typedef void Tox2PpuFunc(Tox2PpuContext* __restrict ctx, uint8_t* __restrict base);
typedef struct Tox2FuncMapping {
    uint32_t guest;
    Tox2PpuFunc* host;
} Tox2FuncMapping;
typedef struct Tox2ImportInfo {
    uint32_t stub;
    uint32_t fnid;
    const char* module;
    const char* name;
} Tox2ImportInfo;

// A function export of a module: the guest address of its descriptor {entry, toc}.
typedef struct Tox2ExportInfo {
    uint32_t opd;
    uint32_t fnid;
    const char* module;
} Tox2ExportInfo;

#define TOX2_PPU_FUNC(name) TOX2_NOINLINE void name(Tox2PpuContext* __restrict ctx, uint8_t* __restrict base)

// Runtime hooks, implemented by the runtime.
void tox2Dispatch(Tox2PpuContext* __restrict ctx, uint8_t* __restrict base);  // target = jump_target
void tox2UnknownTarget(Tox2PpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site);
void tox2RegisterFuncs(const Tox2FuncMapping* mappings, uint32_t count);
Tox2PpuFunc* tox2Lookup(uint32_t addr);  // nullptr when unknown
void tox2HleCall(Tox2PpuContext* ctx, uint8_t* base, uint32_t import_index);
// Import call from the trampoline of a module other than the main executable; `import` points into
// that module's own import table.
void tox2HleCallImport(Tox2PpuContext* ctx, uint8_t* base, const Tox2ImportInfo* import);
void tox2Syscall(Tox2PpuContext* ctx, uint8_t* base, uint32_t site);  // number read from r11
void tox2Yield(Tox2PpuContext* ctx, uint8_t* base);
[[noreturn]] void tox2Unhandled(Tox2PpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word);
[[noreturn]] void tox2Trap(Tox2PpuContext* ctx, uint8_t* base, uint32_t site);
uint32_t tox2Lwarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea);
uint64_t tox2Ldarx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea);
bool tox2Stwcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value);
bool tox2Stdcx(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value);
// Same arguments and value order as tox2WriteN, but the store goes through the reservation table: it bumps the stripe
// version of the line and wakes the waiters on it, like an SPU PUT. For the few plain stores that an SPU waits on
// (see docs/spu.md); a plain tox2WriteN is invisible to a sleeping waiter. Any alignment; a store that is not
// naturally aligned is split into byte stores.
void tox2NotifyWrite8(uint8_t* base, uint32_t ea, uint8_t value);
void tox2NotifyWrite16(uint8_t* base, uint32_t ea, uint16_t value);
void tox2NotifyWrite32(uint8_t* base, uint32_t ea, uint32_t value);
void tox2NotifyWrite64(uint8_t* base, uint32_t ea, uint64_t value);
void tox2Dcbz(Tox2PpuContext* ctx, uint8_t* base, uint32_t ea);
uint64_t tox2Mftb(Tox2PpuContext* ctx);
uint64_t tox2Mffs(Tox2PpuContext* ctx);                                    // FPSCR image in the low 32 bits
void tox2Mtfsf(Tox2PpuContext* ctx, uint32_t field_mask, uint64_t value);  // bit 0x80 >> i = field i
void tox2Mtfsb(Tox2PpuContext* ctx, uint32_t bit, uint32_t value);

// Defined by generated code (ppu_tables.c).
extern const Tox2FuncMapping kTox2PpuFuncMappings[];
extern const uint32_t kTox2PpuFuncMappingCount;
extern const Tox2ImportInfo kTox2PpuImports[];
extern const uint32_t kTox2PpuImportCount;
extern const char kTox2PpuExecutableSha256[];

// Guest memory. Addresses are uint32_t (the guest address space is 32-bit); every access is
// big-endian and may be unaligned.
TOX2_FORCEINLINE uint8_t tox2Read8(const uint8_t* base, uint32_t ea) { return base[ea]; }

TOX2_FORCEINLINE uint16_t tox2Read16(const uint8_t* base, uint32_t ea) {
    uint16_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap16(v);
}

TOX2_FORCEINLINE uint32_t tox2Read32(const uint8_t* base, uint32_t ea) {
    uint32_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap32(v);
}

TOX2_FORCEINLINE uint64_t tox2Read64(const uint8_t* base, uint32_t ea) {
    uint64_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap64(v);
}

TOX2_FORCEINLINE void tox2Write8(uint8_t* base, uint32_t ea, uint8_t value) { base[ea] = value; }

TOX2_FORCEINLINE void tox2Write16(uint8_t* base, uint32_t ea, uint16_t value) {
    value = __builtin_bswap16(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

TOX2_FORCEINLINE void tox2Write32(uint8_t* base, uint32_t ea, uint32_t value) {
    value = __builtin_bswap32(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

TOX2_FORCEINLINE void tox2Write64(uint8_t* base, uint32_t ea, uint64_t value) {
    value = __builtin_bswap64(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

// lvx/stvx semantics: the low four address bits are ignored.
TOX2_FORCEINLINE Tox2Vr tox2ReadVr(const uint8_t* base, uint32_t ea) {
    Tox2U8x16 raw;
    __builtin_memcpy(&raw, base + (ea & ~15u), sizeof raw);
    Tox2Vr v;
    v.u8 = __builtin_shufflevector(raw, raw, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    return v;
}

TOX2_FORCEINLINE void tox2WriteVr(uint8_t* base, uint32_t ea, Tox2Vr value) {
    Tox2U8x16 raw = __builtin_shufflevector(value.u8, value.u8, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __builtin_memcpy(base + (ea & ~15u), &raw, sizeof raw);
}

// Words are given in big-endian element order (w0 is element 0).
TOX2_FORCEINLINE Tox2Vr tox2VrFromBeWords(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    Tox2U32x4 lanes = {w3, w2, w1, w0};
    Tox2Vr v;
    v.u32 = lanes;
    return v;
}

TOX2_FORCEINLINE uint32_t tox2VrBeWord(Tox2Vr v, uint32_t i) { return v.u32[3 - (i & 3)]; }

// Condition register and XER.
TOX2_FORCEINLINE void tox2SetCrField(Tox2PpuContext* ctx, uint32_t field, uint32_t nibble) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = (nibble >> 3) & 1;
    bits[1] = (nibble >> 2) & 1;
    bits[2] = (nibble >> 1) & 1;
    bits[3] = nibble & 1;
}

TOX2_FORCEINLINE uint32_t tox2GetCrField(const Tox2PpuContext* ctx, uint32_t field) {
    const uint8_t* bits = &ctx->cr[4 * field];
    return ((uint32_t)bits[0] << 3) | ((uint32_t)bits[1] << 2) | ((uint32_t)bits[2] << 1) | bits[3];
}

TOX2_FORCEINLINE void tox2CmpS(Tox2PpuContext* ctx, uint32_t field, int64_t a, int64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

TOX2_FORCEINLINE void tox2CmpU(Tox2PpuContext* ctx, uint32_t field, uint64_t a, uint64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

// Rc=1 in 64-bit mode: CR0 from the signed 64-bit result (RPCS3 `ppu_cr_set<s64>`).
TOX2_FORCEINLINE void tox2Record(Tox2PpuContext* ctx, uint64_t r) { tox2CmpS(ctx, 0, (int64_t)r, 0); }

TOX2_FORCEINLINE uint32_t tox2Mfcr(const Tox2PpuContext* ctx) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 32; i++) r |= (uint32_t)(ctx->cr[i] & 1) << (31 - i);
    return r;
}

// crm bit 0x80 >> f selects field f.
TOX2_FORCEINLINE void tox2Mtcrf(Tox2PpuContext* ctx, uint32_t crm, uint32_t value) {
    for (uint32_t f = 0; f < 8; f++) {
        if (crm & (0x80u >> f)) tox2SetCrField(ctx, f, (value >> (28 - 4 * f)) & 15);
    }
}

TOX2_FORCEINLINE uint64_t tox2Mfxer(const Tox2PpuContext* ctx) {
    return ((uint64_t)(ctx->xer_so & 1) << 31) | ((uint64_t)(ctx->xer_ov & 1) << 30) |
           ((uint64_t)(ctx->xer_ca & 1) << 29) | (ctx->xer_count & 0x7f);
}

TOX2_FORCEINLINE void tox2Mtxer(Tox2PpuContext* ctx, uint64_t value) {
    ctx->xer_so = (value >> 31) & 1;
    ctx->xer_ov = (value >> 30) & 1;
    ctx->xer_ca = (value >> 29) & 1;
    ctx->xer_count = value & 0x7f;
}

// OV = ov; SO is sticky.
TOX2_FORCEINLINE void tox2SetOv(Tox2PpuContext* ctx, bool ov) {
    ctx->xer_ov = ov;
    ctx->xer_so = ctx->xer_so | ctx->xer_ov;
}

// Function entry and loop back edges: the only places where the runtime may preempt.
TOX2_FORCEINLINE void tox2Loop(Tox2PpuContext* ctx, uint8_t* base) {
    if (--ctx->preempt_budget < 0) tox2Yield(ctx, base);
}

TOX2_FORCEINLINE void tox2Enter(Tox2PpuContext* ctx, uint8_t* base, uint32_t addr) {
    ctx->trace[ctx->trace_pos & (kTox2TraceSize - 1)] = addr;
    ctx->trace_pos++;
    tox2Loop(ctx, base);
}

// Emitted code names its parameters `ctx` and `base`; the macros rely on that.
#define TOX2_ENTER(addr) tox2Enter(ctx, base, (addr))
#define TOX2_LOOP(site)      \
    do {                     \
        (void)(site);        \
        tox2Loop(ctx, base); \
    } while (0)
#define TOX2_CALL(fn, ret) \
    do {                   \
        ctx->lr = (ret);   \
        fn(ctx, base);     \
    } while (0)
// The target is evaluated before LR is written (bclrl may target LR itself).
#define TOX2_CALL_DYN(site, target, ret)       \
    do {                                       \
        uint32_t tox2_t_ = (uint32_t)(target); \
        ctx->lr = (ret);                       \
        ctx->jump_site = (site);               \
        ctx->jump_target = tox2_t_;            \
        tox2Dispatch(ctx, base);               \
    } while (0)
// The block keeps a label directly in front of a tail call from taking the attribute (C23 parses
// "L: __attribute__((musttail)) return" as a label attribute and rejects it).
#define TOX2_TAIL(fn)                       \
    do {                                    \
        TOX2_MUSTTAIL return fn(ctx, base); \
    } while (0)
#define TOX2_TAIL_DYN(site, target)                   \
    do {                                              \
        ctx->jump_site = (site);                      \
        ctx->jump_target = (uint32_t)(target);        \
        TOX2_MUSTTAIL return tox2Dispatch(ctx, base); \
    } while (0)
#define TOX2_HLE(index) tox2HleCall(ctx, base, (index))
#define TOX2_HLE_IMPORT(table, index) tox2HleCallImport(ctx, base, &(table)[(index)])
#define TOX2_SYSCALL(site) tox2Syscall(ctx, base, (site))
// sync, lwsync, eieio and isync. The empty asm takes `base` as an input and clobbers memory: an opaque call
// without `base` would not order accesses through the `__restrict` guest pointer. Only the heavy sync needs a
// host fence, since SPU threads run in parallel with PPU threads and observe guest memory.
#define TOX2_SYNC(kind)                                                        \
    do {                                                                       \
        __asm__ __volatile__("" : : "r"(base) : "memory");                     \
        if ((kind) == kTox2SyncHeavy) __atomic_thread_fence(__ATOMIC_SEQ_CST); \
    } while (0)
#define TOX2_TRAP(site) tox2Trap(ctx, base, (site))
#define TOX2_UNHANDLED(site, word) tox2Unhandled(ctx, base, (site), (word))

#ifdef __cplusplus
}  // extern "C"
#endif

#pragma once

// Contract between generated SPU code and the runtime: the SPU context, the tables the generated
// code provides, the runtime hooks and the inline local-store accessors. Nothing here depends on how
// control transfers are lowered; that lives in one header per lowering (spu_lower_stackfree.h), so a
// second lowering adds a header and an emitter module without touching this file.
//
// Like ppu_context.h this compiles as C23 and C++26 and with -ffreestanding.
//
// Registers use Tox2Vr with the PPU's byte-reversed lane layout: TOX2_VR_W(v, 0) is the preferred-slot
// word, lane-wise operations need no index mapping, and a quadword load or store reverses the bytes
// once. The local store (LS) is kept as it appears in memory, big-endian. Generated functions name
// their parameters `ctx` and `ls`; the macros rely on that. Guest memory is never visible to
// generated SPU code, only the runtime's MFC hooks touch it.

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"
#include "ppu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

enum Tox2SpuLimits { kTox2SpuLsSize = 0x40000, kTox2SpuTraceSize = 64, kTox2SpuNoExit = 0xffffffffu };

typedef struct Tox2SpuContext {
    Tox2Vr gpr[128];       // byte-reversed lanes, as Tox2Vr in ppu_context.h
    Tox2Vr fpscr;          // fscrrd/fscrwr image; flags are not derived from operations
    uint32_t jump_target;  // LS address of the current transfer: dispatch input and entry-switch key
    uint32_t jump_site;    // LS address of the transferring instruction
    uint32_t srr0;
    uint32_t halt;        // non-zero: leave at the next check; written by other threads (__atomic)
    uint32_t exit_ls;     // a dispatch to this LS address returns to the run loop; kTox2SpuNoExit = none
    uint8_t int_enabled;  // RdMachStat bit 0; set and cleared by the E/D bits and iret
    uint32_t trace_pos;
    uint32_t trace[kTox2SpuTraceSize];
    void* runtime;  // the runtime's thread object
} Tox2SpuContext;

static_assert(alignof(Tox2SpuContext) == 16, "Tox2SpuContext must be 16-byte aligned");
static_assert(sizeof(Tox2SpuContext) % 16 == 0, "Tox2SpuContext size must keep arrays of contexts aligned");
static_assert(offsetof(Tox2SpuContext, gpr) == 0, "gpr[] must stay first");
static_assert(offsetof(Tox2SpuContext, fpscr) % 16 == 0, "fpscr must stay 16-byte aligned");
static_assert((kTox2SpuTraceSize & (kTox2SpuTraceSize - 1)) == 0, "trace ring size must be a power of two");

typedef void Tox2SpuFunc(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls);

// One dispatchable block.
typedef struct Tox2SpuEntry {
    uint32_t ls;
    uint32_t check_count;  // 1..4: words compared with LS before the jump
    uint32_t check[4];     // instruction words (generated tables only)
    Tox2SpuFunc* host;
} Tox2SpuEntry;

// ea is the absolute guest address of the segment's bytes.
typedef struct Tox2SpuSegment {
    uint32_t ea, ls, size, code;
} Tox2SpuSegment;

typedef struct Tox2SpuImageInfo {
    const char* name;
    uint32_t ea, size;   // the whole image in guest memory (ELF: header to end of last segment)
    const char* sha256;  // of those bytes; checked by the runtime at load
    uint32_t entry_point;
    uint32_t segment_count;
    const Tox2SpuSegment* segments;
    uint32_t entry_count;  // sorted by ls
    const Tox2SpuEntry* entries;
} Tox2SpuImageInfo;

#define TOX2_SPU_FUNC(name) TOX2_NOINLINE void name(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls)

// Runtime hooks, implemented by the runtime.
void tox2SpuDispatch(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls);  // target = jump_target
// The channel and stop hooks return false when the thread must leave.
bool tox2SpuReadChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value);
bool tox2SpuWriteChannel(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value);
uint32_t tox2SpuChannelCount(Tox2SpuContext* ctx, uint8_t* ls, uint32_t ch);
bool tox2SpuStop(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code);  // true: continue at site + 4
[[noreturn]] void tox2SpuHalt(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site);    // heq/hgt/hlgt taken
[[noreturn]] void tox2SpuUnhandled(Tox2SpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word);

// Defined by generated code (spu_tables.c).
extern const Tox2SpuImageInfo kTox2SpuImages[];
extern const uint32_t kTox2SpuImageCount;

// Quadword access to the local store: the address wraps at the LS size and its low four bits are
// ignored (lqd/stqd semantics). The byte reversal turns big-endian LS bytes into the register layout.
TOX2_FORCEINLINE Tox2Vr tox2SpuLoadQ(const uint8_t* ls, uint32_t addr) {
    Tox2U8x16 raw;
    __builtin_memcpy(&raw, ls + (addr & ((uint32_t)kTox2SpuLsSize - 16u)), sizeof raw);
    Tox2Vr v;
    v.u8 = __builtin_shufflevector(raw, raw, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    return v;
}

TOX2_FORCEINLINE void tox2SpuStoreQ(uint8_t* ls, uint32_t addr, Tox2Vr value) {
    Tox2U8x16 raw = __builtin_shufflevector(value.u8, value.u8, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __builtin_memcpy(ls + (addr & ((uint32_t)kTox2SpuLsSize - 16u)), &raw, sizeof raw);
}

// The preferred slot is word 0.
TOX2_FORCEINLINE uint32_t tox2SpuPreferred(Tox2Vr v) { return TOX2_VR_W(v, 0); }

// Block entry: the trace ring that the fatal-stop dump prints.
TOX2_FORCEINLINE void tox2SpuEnter(Tox2SpuContext* ctx, uint32_t addr) {
    ctx->trace[ctx->trace_pos & (kTox2SpuTraceSize - 1)] = addr;
    ctx->trace_pos++;
}

// Acquire pairs with the release store of the thread that requests the halt, so state it wrote
// before the request is visible to the leaving thread.
TOX2_FORCEINLINE bool tox2SpuHaltRequested(const Tox2SpuContext* ctx) {
    return __atomic_load_n(&ctx->halt, __ATOMIC_ACQUIRE) != 0;
}

#define TOX2_SPU_ENTER(addr) tox2SpuEnter(ctx, (addr))

#ifdef __cplusplus
}  // extern "C"
#endif

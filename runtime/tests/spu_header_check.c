// Checks the SPU contract header's inline helpers and the stack-free lowering macros against a minimal
// dispatcher written here; the real dispatcher belongs to the runtime.

#include <stdio.h>

#include "tox2/spu_context.h"
#include "tox2/spu_lower_stackfree.h"

static int g_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static_assert(alignof(Tox2SpuContext) == 16);
static_assert(offsetof(Tox2SpuContext, gpr) == 0);
static_assert(offsetof(Tox2SpuContext, gpr[127]) == 127 * 16);
static_assert(offsetof(Tox2SpuContext, fpscr) == 128 * 16);
static_assert(sizeof(Tox2SpuContext) % alignof(Tox2SpuContext) == 0);

static Tox2SpuContext g_ctx;
static uint8_t g_ls[kTox2SpuLsSize] __attribute__((aligned(16)));

// Functions and dispatch table

static int g_rejected_entries;

TOX2_SPU_FUNC(funcA);
TOX2_SPU_FUNC(funcB);
TOX2_SPU_FUNC(funcLeaf);
TOX2_SPU_FUNC(funcGuarded);

static const Tox2SpuEntry kEntries[] = {
        {0x100, 0, {0}, funcA},
        {0x200, 0, {0}, funcB},
        {0x300, 0, {0}, funcLeaf},
        {0x400, 2, {0x12345678u, 0x9abcdef0u}, funcGuarded},
};

static uint32_t loadWord(const uint8_t* ls, uint32_t addr) {
    uint32_t v;
    __builtin_memcpy(&v, ls + addr, sizeof v);
    return __builtin_bswap32(v);
}

void tox2SpuDispatch(Tox2SpuContext* __restrict ctx, uint8_t* __restrict ls) {
    uint32_t target = ctx->jump_target & 0x3fffcu;
    for (uint32_t i = 0; i < sizeof kEntries / sizeof kEntries[0]; i++) {
        const Tox2SpuEntry* entry = &kEntries[i];
        if (entry->ls != target) continue;
        for (uint32_t w = 0; w < entry->check_count; w++) {
            if (loadWord(ls, target + 4 * w) != entry->check[w]) {
                g_rejected_entries++;
                return;
            }
        }
        TOX2_MUSTTAIL return entry->host(ctx, ls);
    }
    g_rejected_entries++;
}

// A function that jumps to a known block of another function.
TOX2_SPU_FUNC(funcA) {
    TOX2_SPU_SF_ENTER(0x100);
    ctx->gpr[3] = tox2VrFromBeWords(0xaaaa0001u, 0, 0, 0);
    TOX2_SPU_SF_JUMP(funcB, 0x200);
}

// A computed transfer: the link register is written after the target is read, the way the emitter
// lowers `bisl $0,$0`.
TOX2_SPU_FUNC(funcB) {
    TOX2_SPU_SF_ENTER(0x200);
    uint32_t target = tox2SpuPreferred(ctx->gpr[0]);
    ctx->gpr[0] = tox2VrFromBeWords(0x204u, 0, 0, 0);
    TOX2_SPU_SF_DISPATCH(0x200, target);
}

TOX2_SPU_FUNC(funcLeaf) {
    (void)ls;
    TOX2_SPU_SF_ENTER(0x300);
    ctx->gpr[5] = tox2VrFromBeWords(0xbbbb0002u, 0, 0, 0);
    if (tox2SpuPreferred(ctx->gpr[6]) != 0) TOX2_SPU_SF_LEAVE();
    ctx->gpr[5] = tox2VrFromBeWords(0xdeadu, 0, 0, 0);  // not reached when gpr[6] is set
}

TOX2_SPU_FUNC(funcGuarded) {
    (void)ls;
    TOX2_SPU_SF_ENTER(0x400);
    ctx->gpr[7] = tox2VrFromBeWords(1, 0, 0, 0);
}

// Loops until the halt request is seen; the body raises the request itself after five rounds.
TOX2_SPU_FUNC(funcLoop) {
    (void)ls;
    for (;;) {
        TOX2_VR_W(ctx->gpr[8], 0)++;
        if (TOX2_VR_W(ctx->gpr[8], 0) == 5) __atomic_store_n(&ctx->halt, 1, __ATOMIC_RELEASE);
        TOX2_SPU_SF_LOOP(0x500);
    }
}

// Checks

static void checkLocalStore(void) {
    for (uint32_t i = 0; i < sizeof g_ls; i++) g_ls[i] = 0;
    Tox2Vr v = tox2VrFromBeWords(0x00010203u, 0x04050607u, 0x08090a0bu, 0x0c0d0e0fu);

    tox2SpuStoreQ(g_ls, 0x1235, v);  // low address bits are ignored: stored at 0x1230
    for (uint32_t i = 0; i < 16; i++) CHECK(g_ls[0x1230 + i] == i);
    CHECK(g_ls[0x122f] == 0 && g_ls[0x1240] == 0);

    Tox2Vr r = tox2SpuLoadQ(g_ls, 0x123a);
    CHECK(__builtin_memcmp(&r, &v, sizeof r) == 0);
    CHECK(tox2SpuPreferred(r) == 0x00010203u);
    CHECK(TOX2_VR_W(r, 3) == 0x0c0d0e0fu);

    // The address wraps at the LS size.
    tox2SpuStoreQ(g_ls, kTox2SpuLsSize + 0x20, v);
    CHECK(g_ls[0x20] == 0x00 && g_ls[0x2f] == 0x0f);
    Tox2Vr w = tox2SpuLoadQ(g_ls, 0xfffc0000u + 0x20);
    CHECK(__builtin_memcmp(&w, &v, sizeof w) == 0);

    // The last quadword is reachable and does not run past the buffer.
    tox2SpuStoreQ(g_ls, kTox2SpuLsSize - 16, v);
    CHECK(g_ls[kTox2SpuLsSize - 1] == 0x0f);
}

static void checkTrace(Tox2SpuContext* ctx) {
    ctx->trace_pos = 0;
    for (uint32_t i = 0; i < 70; i++) TOX2_SPU_ENTER(0x1000 + 4 * i);
    CHECK(ctx->trace_pos == 70);
    CHECK(ctx->trace[(70 - 1) & (kTox2SpuTraceSize - 1)] == 0x1000 + 4 * 69);
    CHECK(ctx->trace[5] == 0x1000 + 4 * 69);  // 69 & 63; the ring holds the newest 64
    CHECK(ctx->trace[6] == 0x1000 + 4 * 6);
}

static void checkLowering(Tox2SpuContext* ctx, uint8_t* ls) {
    // A -> (JUMP) B -> (DISPATCH to gpr[0]) leaf; the link written by B is the return site.
    ctx->halt = 0;
    ctx->trace_pos = 0;
    ctx->gpr[0] = tox2VrFromBeWords(0x40300u, 0, 0, 0);  // out-of-range bits are masked by the dispatcher
    ctx->gpr[6] = tox2VrFromBeWords(0, 0, 0, 0);
    ctx->jump_target = 0;
    funcA(ctx, ls);
    CHECK(tox2SpuPreferred(ctx->gpr[3]) == 0xaaaa0001u);
    CHECK(tox2SpuPreferred(ctx->gpr[0]) == 0x204u);
    CHECK(tox2SpuPreferred(ctx->gpr[5]) == 0xdeadu);  // the leaf ran to its end
    CHECK(ctx->jump_site == 0x200);
    CHECK(ctx->trace_pos == 3);
    CHECK(ctx->trace[0] == 0x100 && ctx->trace[1] == 0x200 && ctx->trace[2] == 0x300);

    // LEAVE returns before the statements after it.
    ctx->gpr[6] = tox2VrFromBeWords(1, 0, 0, 0);
    funcLeaf(ctx, ls);
    CHECK(tox2SpuPreferred(ctx->gpr[5]) == 0xbbbb0002u);

    // A halt request is seen at block entry.
    ctx->gpr[5] = tox2VrFromBeWords(0, 0, 0, 0);
    __atomic_store_n(&ctx->halt, 1, __ATOMIC_RELEASE);
    CHECK(tox2SpuHaltRequested(ctx));
    funcLeaf(ctx, ls);
    CHECK(tox2SpuPreferred(ctx->gpr[5]) == 0);
    ctx->halt = 0;
    CHECK(!tox2SpuHaltRequested(ctx));

    // A halt request is seen on a backward edge.
    ctx->gpr[8] = tox2VrFromBeWords(0, 0, 0, 0);
    funcLoop(ctx, ls);
    CHECK(tox2SpuPreferred(ctx->gpr[8]) == 5);
    CHECK(ctx->jump_site == 0x500);
    ctx->halt = 0;
}

static void checkEntryCheck(Tox2SpuContext* ctx, uint8_t* ls) {
    ctx->gpr[7] = tox2VrFromBeWords(0, 0, 0, 0);
    ctx->jump_target = 0x400;
    g_rejected_entries = 0;

    tox2SpuDispatch(ctx, ls);  // LS does not hold the check words
    CHECK(g_rejected_entries == 1);
    CHECK(tox2SpuPreferred(ctx->gpr[7]) == 0);

    Tox2Vr words = tox2VrFromBeWords(0x12345678u, 0x9abcdef0u, 0, 0);
    tox2SpuStoreQ(ls, 0x400, words);
    tox2SpuDispatch(ctx, ls);
    CHECK(g_rejected_entries == 1);
    CHECK(tox2SpuPreferred(ctx->gpr[7]) == 1);

    ctx->jump_target = 0x700;  // not a dispatchable entry
    tox2SpuDispatch(ctx, ls);
    CHECK(g_rejected_entries == 2);
}

int main(void) {
    checkLocalStore();
    checkTrace(&g_ctx);
    checkLowering(&g_ctx, g_ls);
    checkEntryCheck(&g_ctx, g_ls);

    if (g_failures != 0) {
        fprintf(stderr, "spu header check: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("spu header ok");
    return 0;
}

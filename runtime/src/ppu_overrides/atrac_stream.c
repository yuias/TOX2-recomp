// Hand-written replacements for two recompiled functions of the game's ATRAC stream: the loader (guest 0x00956a00)
// and the decoder setup (0x00955f50). Each is the recompiled body with a few edits, so that the flag at obj+0x244, the
// ring copy and the byte count at obj+0x234 are never interleaved with the decoder setup; the original lets the
// loader run between the decoder's count read and its flag store, which misaligns the ring.
//
// Loader (0x00956a00): the flag check, the direct ring copy and the byte count run under one hold of the mutex at
// obj+0x21c; the original takes the mutex only after the copy.
//   1. The mutex is locked right after the free-space early exit (0x00956a34).
//   2. The staging path unlocks it again before its own lock at 0x00956a94.
//   3. The direct path drops its lock at 0x00956af8; the mutex is still held from edit 1.
// The free-space call stays outside the lock, as in the original: only this loader adds to obj+0x234.
//
// Decoder setup (0x00955f50): the mutex is held across the first SetData and the flag is raised only after SetData
// succeeded; the original unlocks before SetData and sets the flag later, at 0x009561e4.
//   1. The unlock at 0x00955ff0 and the lock at 0x0095602c around the first SetData are dropped.
//   2. After a successful SetData (0x00956038, 0x0095606c) the flag is stored as at 0x009561e4, before the unlock at
//      0x00956084. A failed SetData leaves it 0.
//
// Only guest-visible interfaces are used (syscalls and generated functions), so the file links against test fakes
// without a generated tree.

#include "tox2/ppu.h"

TOX2_PPU_FUNC(fn_00954a08);
TOX2_PPU_FUNC(fn_00955eb8);
TOX2_PPU_FUNC(fn_00985a08);
TOX2_PPU_FUNC(imp_00c90028);
TOX2_PPU_FUNC(imp_00c90048);
TOX2_PPU_FUNC(imp_00c90068);
TOX2_PPU_FUNC(imp_00c90088);
TOX2_PPU_FUNC(imp_00c900a8);
TOX2_PPU_FUNC(imp_00c900c8);
TOX2_PPU_FUNC(imp_00c900e8);
TOX2_PPU_FUNC(imp_00c90108);
TOX2_PPU_FUNC(imp_00c90128);
TOX2_PPU_FUNC(imp_00c90148);
TOX2_PPU_FUNC(imp_00c90168);
TOX2_PPU_FUNC(imp_00c90188);
TOX2_PPU_FUNC(imp_00c901a8);
TOX2_PPU_FUNC(imp_00c901c8);
TOX2_PPU_FUNC(imp_00c901e8);
TOX2_PPU_FUNC(imp_00c908e8);

// The body stays in the shape of the generated code so that it can be diffed against fn_00956a00_generated.
// clang-format off
TOX2_PPU_FUNC(fn_00956a00) {
    TOX2_ENTER(0x00956a00u);
    {
        uint64_t t0 = (ctx->gpr[1] + 0xffffffffffffff70ull);
        tox2Write64(base, (uint32_t)t0, (uint64_t)ctx->gpr[1]);
        ctx->gpr[1] = t0;
    }
    ctx->gpr[0] = ctx->lr;
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull), (uint64_t)ctx->gpr[30]);
    ctx->gpr[30] = ctx->gpr[5] | ctx->gpr[5];
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000070ull), (uint64_t)ctx->gpr[28]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull), (uint64_t)ctx->gpr[29]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000088ull), (uint64_t)ctx->gpr[31]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a0ull), (uint64_t)ctx->gpr[0]);
    ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];
    ctx->gpr[29] = ctx->gpr[4] | ctx->gpr[4];
    TOX2_CALL(fn_00955eb8, 0x00956a2cu);
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[3]);
    ctx->gpr[28] = 0x0000000000000000ull;
    if (ctx->cr[29]) goto L_00956ab0;
    // Lock before the flag check so the check, the copy and the count update are one critical section.
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x0000000000000066ull;
    TOX2_SYSCALL(0x00956af8u);
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000244ull));
    ctx->gpr[28] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[4] = ctx->gpr[29] & 0x00000000ffffffffull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    ctx->gpr[5] = ctx->gpr[28] | ctx->gpr[28];
    if (ctx->cr[30]) goto L_00956ad4;
    // Staging path: it takes its own lock at 0x00956a94, so release the one taken above.
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[11] = 0x0000000000000068ull;
    TOX2_SYSCALL(0x00956aacu);
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000250ull));
    ctx->gpr[28] = ctx->gpr[30] & 0x00000000ffffffffull;
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[0], (uint32_t)ctx->gpr[30]);
    if (ctx->cr[28]) goto L_00956b44;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000024cull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (ctx->cr[30]) goto L_00956b58;
L_00956a6c:
    ctx->gpr[4] = ctx->gpr[29] & 0x00000000ffffffffull;
    ctx->gpr[5] = ctx->gpr[28] | ctx->gpr[28];
    ctx->gpr[3] = ctx->gpr[3] & 0x00000000ffffffffull;
    TOX2_CALL(fn_00985a08, 0x00956a7cu);
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[11] = 0x0000000000000066ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000254ull), (uint32_t)ctx->gpr[30]);
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000250ull), (uint32_t)ctx->gpr[30]);
    TOX2_SYSCALL(0x00956a94u);
    ctx->gpr[11] = 0x000000000000006cull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    TOX2_SYSCALL(0x00956aa0u);
    ctx->gpr[11] = 0x0000000000000068ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    TOX2_SYSCALL(0x00956aacu);
L_00956ab0:
    ctx->gpr[0] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a0ull));
    ctx->gpr[3] = ctx->gpr[28] | ctx->gpr[28];
    ctx->gpr[29] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull));
    ctx->gpr[28] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000070ull));
    ctx->lr = ctx->gpr[0];
    ctx->gpr[30] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[31] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000088ull));
    ctx->gpr[1] = ctx->gpr[1] + 0x0000000000000090ull;
    return;
L_00956ad4:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[0];
    ctx->gpr[3] = ctx->gpr[3] & 0x00000000ffffffffull;
    TOX2_CALL(fn_00985a08, 0x00956ae8u);
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    ctx->gpr[11] = 0x000000000000006cull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[29] = ctx->gpr[30] + ctx->gpr[29];
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull), (uint32_t)ctx->gpr[29]);
    TOX2_SYSCALL(0x00956b10u);
    ctx->gpr[11] = 0x0000000000000068ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    TOX2_SYSCALL(0x00956b1cu);
    ctx->gpr[0] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a0ull));
    ctx->gpr[3] = ctx->gpr[28] | ctx->gpr[28];
    ctx->gpr[29] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull));
    ctx->gpr[28] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000070ull));
    ctx->lr = ctx->gpr[0];
    ctx->gpr[30] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[31] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000088ull));
    ctx->gpr[1] = ctx->gpr[1] + 0x0000000000000090ull;
    return;
L_00956b44:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000024cull));
    TOX2_CALL(fn_00954a08, 0x00956b4cu);
    ctx->gpr[3] = 0x0000000000000000ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000024cull), (uint32_t)ctx->gpr[3]);
L_00956b58:
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[2] + 0x0000000000002694ull));
    ctx->gpr[28] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[3] = ctx->gpr[28] | ctx->gpr[28];
    ctx->gpr[11] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000000ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000000ull));
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull), (uint64_t)ctx->gpr[2]);
    ctx->ctr = ctx->gpr[0];
    ctx->gpr[2] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000004ull));
    TOX2_CALL_DYN(0x00956b78u, ctx->ctr, 0x00956b7cu);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000024cull), (uint32_t)ctx->gpr[3]);
    { TOX2_LOOP(0x00956b84u); goto L_00956a6c; }
}
// clang-format on

// Same as the loader: the body stays diffable against fn_00955f50_generated.
// clang-format off
TOX2_PPU_FUNC(fn_00955f50) {
    TOX2_ENTER(0x00955f50u);
    ctx->gpr[12] = tox2Mfcr(ctx);
    {
        uint64_t t0 = (ctx->gpr[1] + 0xffffffffffffff10ull);
        tox2Write64(base, (uint32_t)t0, (uint64_t)ctx->gpr[1]);
        ctx->gpr[1] = t0;
    }
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a8ull), (uint64_t)ctx->gpr[23]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b8ull), (uint64_t)ctx->gpr[25]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000e8ull), (uint64_t)ctx->gpr[31]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b0ull), (uint64_t)ctx->gpr[24]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000c0ull), (uint64_t)ctx->gpr[26]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000c8ull), (uint64_t)ctx->gpr[27]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d0ull), (uint64_t)ctx->gpr[28]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d8ull), (uint64_t)ctx->gpr[29]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000e0ull), (uint64_t)ctx->gpr[30]);
    ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];
    ctx->gpr[25] = ctx->gpr[3] | ctx->gpr[3];
    tox2Write32(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000f8ull), (uint32_t)ctx->gpr[12]);
    ctx->gpr[0] = ctx->lr;
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000100ull), (uint64_t)ctx->gpr[0]);
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[3] + 0x0000000000000230ull));
    ctx->gpr[23] = tox2Read32(base, (uint32_t)(ctx->gpr[3] + 0x0000000000000004ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) goto L_0095647c;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[3] + 0x000000000000021cull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x0000000000000066ull;
    TOX2_SYSCALL(0x00955facu);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[29], 0);
    if (ctx->cr[30]) goto L_00955fdc;
    goto L_00955fe8;
L_00955fc0:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x000000000000006bull;
    TOX2_SYSCALL(0x00955fccu);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[29], 0);
    if (!ctx->cr[30]) goto L_00955fe8;
L_00955fdc:
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) { TOX2_LOOP(0x00955fe4u); goto L_00955fc0; }
L_00955fe8:
    // The mutex stays locked across the first SetData; the original unlocks here and locks again at 0x0095602c.
    ctx->gpr[26] = ctx->gpr[25] + 0x0000000000000008ull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
    ctx->gpr[28] = ctx->gpr[1] + 0x0000000000000070ull;
    ctx->gpr[5] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    ctx->gpr[30] = ctx->gpr[26] & 0x00000000ffffffffull;
    ctx->gpr[6] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000228ull));
    ctx->gpr[7] = ctx->gpr[28] | ctx->gpr[28];
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    TOX2_CALL(imp_00c900e8, 0x00956018u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[29] = ctx->gpr[3] | ctx->gpr[3];
    tox2CmpS(ctx, 4, (int32_t)ctx->gpr[29], 0);
    if (!ctx->cr[18]) goto L_00956070;
    {
        // Raise the flag while the mutex is still held and only after SetData succeeded.
        ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
        ctx->gpr[0] = tox2Read16(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000020ull));
        ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[0], 24) & 0x0000000000ffffffull;
        ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 8) & 0x00000000ffffff00ull;
        ctx->gpr[0] = ctx->gpr[0] | ctx->gpr[9];
        ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 0) & 0x000000000000ffffull;
        tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000244ull), (uint32_t)ctx->gpr[0]);
        goto L_0095607c;
    }
L_0095603c:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x000000000000006bull;
    TOX2_SYSCALL(0x00956048u);
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
    ctx->gpr[5] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000234ull));
    ctx->gpr[7] = ctx->gpr[28] & 0x00000000ffffffffull;
    ctx->gpr[6] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000228ull));
    TOX2_CALL(imp_00c900e8, 0x00956064u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (ctx->cr[30]) {
        ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
        ctx->gpr[0] = tox2Read16(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000020ull));
        ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[0], 24) & 0x0000000000ffffffull;
        ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 8) & 0x00000000ffffff00ull;
        ctx->gpr[0] = ctx->gpr[0] | ctx->gpr[9];
        ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 0) & 0x000000000000ffffull;
        tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000244ull), (uint32_t)ctx->gpr[0]);
        goto L_0095607c;
    }
L_00956070:
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) { TOX2_LOOP(0x00956078u); goto L_0095603c; }
L_0095607c:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[11] = 0x0000000000000068ull;
    TOX2_SYSCALL(0x00956084u);
L_00956088:
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) goto L_009561e4;
L_00956094:
    ctx->gpr[8] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
L_00956098:
    ctx->gpr[8] = ctx->gpr[8] & 0x00000000ffffffffull;
    ctx->gpr[10] = 0xffffffffaaaa0000ull;
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[10] = ctx->gpr[10] | 0x000000000000aaabull;
    ctx->gpr[11] = 0x0000000000000066ull;
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[8] + 0x0000000000000018ull));
    ctx->gpr[7] = ctx->gpr[9] + 0x0000000000000001ull;
    ctx->gpr[10] = tox2Mulhwu(ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[10] = tox2Rotl32Dup(ctx->gpr[10], 31) & 0x000000007fffffffull;
    tox2Write32(base, (uint32_t)(ctx->gpr[8] + 0x0000000000000018ull), (uint32_t)ctx->gpr[7]);
    ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[10], 1) & 0x00000000fffffffeull;
    ctx->gpr[0] = ctx->gpr[0] + ctx->gpr[10];
    ctx->gpr[9] = ctx->gpr[9] - ctx->gpr[0];
    ctx->gpr[0] = 0xfffffffffffffffeull;
    ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[9], 2) & 0x00000000fffffffcull;
    ctx->gpr[9] = ctx->gpr[9] + 0x0000000000000020ull;
    ctx->gpr[9] = (uint64_t)(int64_t)(int32_t)ctx->gpr[9];
    ctx->gpr[8] = ctx->gpr[8] + ctx->gpr[9];
    tox2Write32(base, (uint32_t)(ctx->gpr[8] + 0x000000000000000cull), (uint32_t)ctx->gpr[0]);
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    TOX2_SYSCALL(0x009560e8u);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000018ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000000ull));
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[0], (uint32_t)ctx->gpr[9]);
    if (ctx->cr[28]) goto L_00956128;
    goto L_00956134;
L_00956104:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x000000000000006bull;
    TOX2_SYSCALL(0x00956110u);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000000ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000018ull));
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[0], (uint32_t)ctx->gpr[9]);
    if (!ctx->cr[29]) goto L_00956134;
L_00956128:
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) { TOX2_LOOP(0x00956130u); goto L_00956104; }
L_00956134:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[11] = 0x0000000000000068ull;
    TOX2_SYSCALL(0x0095613cu);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000208ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[29], 0);
    if (ctx->cr[30]) goto L_00956158;
    ctx->gpr[3] = ctx->gpr[26] & 0x00000000ffffffffull;
    TOX2_CALL(imp_00c90108, 0x00956154u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
L_00956158:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000022cull));
    ctx->gpr[30] = 0x0000000000000000ull;
    ctx->gpr[29] = 0x0000000000000020ull;
    TOX2_CALL(fn_00954a08, 0x00956168u);
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000208ull));
    TOX2_CALL(fn_00954a08, 0x00956174u);
L_00956178:
    ctx->gpr[0] = (uint64_t)(int64_t)(int32_t)ctx->gpr[29];
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[29] = ctx->gpr[29] + 0x0000000000000004ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + ctx->gpr[0]));
    TOX2_CALL(fn_00954a08, 0x0095618cu);
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[30], 2);
    ctx->gpr[30] = ctx->gpr[30] + 0x0000000000000001ull;
    if (!ctx->cr[30]) { TOX2_LOOP(0x00956198u); goto L_00956178; }
    ctx->gpr[3] = 0x0000000000000000ull;
    TOX2_CALL(imp_00c908e8, 0x009561a4u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000100ull));
    ctx->gpr[12] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000f8ull));
    ctx->gpr[23] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a8ull));
    ctx->lr = ctx->gpr[0];
    ctx->gpr[24] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b0ull));
    tox2SetCrField(ctx, 4, (uint32_t)(ctx->gpr[12] >> 12) & 15u);
    ctx->gpr[25] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b8ull));
    ctx->gpr[26] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000c0ull));
    ctx->gpr[27] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000c8ull));
    ctx->gpr[28] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d0ull));
    ctx->gpr[29] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d8ull));
    ctx->gpr[30] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000e0ull));
    ctx->gpr[31] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000e8ull));
    ctx->gpr[1] = ctx->gpr[1] + 0x00000000000000f0ull;
    return;
L_009561e4:
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
    ctx->gpr[24] = tox2Read32(base, (uint32_t)(ctx->gpr[2] + 0x0000000000002694ull));
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000070ull));
    ctx->gpr[0] = tox2Read16(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000020ull));
    ctx->gpr[11] = tox2Read32(base, (uint32_t)(ctx->gpr[24] + 0x0000000000000000ull));
    ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[0], 24) & 0x0000000000ffffffull;
    ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 8) & 0x00000000ffffff00ull;
    ctx->gpr[0] = ctx->gpr[0] | ctx->gpr[9];
    ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[0], 0) & 0x000000000000ffffull;
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000244ull), (uint32_t)ctx->gpr[0]);
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000000ull));
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull), (uint64_t)ctx->gpr[2]);
    ctx->ctr = ctx->gpr[0];
    ctx->gpr[2] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000004ull));
    TOX2_CALL_DYN(0x0095621cu, ctx->ctr, 0x00956220u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000208ull), (uint32_t)ctx->gpr[3]);
    if (ctx->cr[30]) { TOX2_LOOP(0x0095622cu); goto L_00956094; }
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[2] + 0x0000000000002698ull));
    ctx->gpr[6] = ctx->gpr[1] + 0x0000000000000094ull;
    ctx->gpr[11] = 0x0000000000000000ull;
    ctx->gpr[10] = ctx->gpr[6] | ctx->gpr[6];
    ctx->gpr[8] = 0x0000000000000001ull;
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000000ull));
    ctx->gpr[9] = 0x0000000000000008ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000094ull), (uint32_t)ctx->gpr[0]);
    ctx->ctr = ctx->gpr[9];
L_00956254:
    ctx->gpr[9] = ctx->gpr[10] + ctx->gpr[11];
    ctx->gpr[11] = ctx->gpr[11] + 0x0000000000000001ull;
    ctx->gpr[9] = ctx->gpr[9] & 0x00000000ffffffffull;
    tox2Write8(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000004ull), (uint8_t)ctx->gpr[8]);
    ctx->ctr -= 1;
    if (ctx->ctr != 0) { TOX2_LOOP(0x00956264u); goto L_00956254; }
    ctx->gpr[30] = ctx->gpr[26] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000208ull));
    ctx->gpr[5] = 0x0000000000000064ull;
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    TOX2_CALL(imp_00c90068, 0x0095627cu);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (!ctx->cr[30]) { TOX2_LOOP(0x00956284u); goto L_00956094; }
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[27] = 0x0000000000000000ull;
    ctx->gpr[4] = ctx->gpr[4] + 0x0000000000000014ull;
    ctx->gpr[28] = 0x0000000000000020ull;
    ctx->gpr[4] = ctx->gpr[4] & 0x00000000ffffffffull;
    TOX2_CALL(imp_00c90048, 0x009562a4u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000084ull;
    TOX2_CALL(imp_00c900c8, 0x009562b4u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[8] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    ctx->gpr[11] = ctx->gpr[8] & 0x00000000ffffffffull;
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000014ull));
    ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[9], 2) & 0x00000000fffffffcull;
    ctx->gpr[0] = tox2Mullw(ctx->gpr[0], ctx->gpr[9]);
    tox2Write32(base, (uint32_t)(ctx->gpr[11] + 0x000000000000001cull), (uint32_t)ctx->gpr[0]);
L_009562d4:
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[24] + 0x0000000000000000ull));
    ctx->gpr[29] = ctx->gpr[8] & 0x00000000ffffffffull;
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000000ull));
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x000000000000001cull));
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull), (uint64_t)ctx->gpr[2]);
    ctx->ctr = ctx->gpr[0];
    ctx->gpr[2] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000004ull));
    TOX2_CALL_DYN(0x009562f0u, ctx->ctr, 0x009562f4u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[8] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[9] = (uint64_t)(int64_t)(int32_t)ctx->gpr[28];
    ctx->gpr[11] = ctx->gpr[8] & 0x00000000ffffffffull;
    tox2CmpS(ctx, 6, (int32_t)ctx->gpr[27], 2);
    ctx->gpr[28] = ctx->gpr[28] + 0x0000000000000004ull;
    ctx->gpr[27] = ctx->gpr[27] + 0x0000000000000001ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[29] + ctx->gpr[9]), (uint32_t)ctx->gpr[3]);
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + ctx->gpr[9]));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) { TOX2_LOOP(0x0095631cu); goto L_00956098; }
    if (!ctx->cr[26]) { TOX2_LOOP(0x00956320u); goto L_009562d4; }
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    TOX2_CALL(imp_00c90168, 0x0095632cu);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (!ctx->cr[29]) goto L_00956368;
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000090ull;
    ctx->gpr[5] = ctx->gpr[1] + 0x0000000000000080ull;
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    TOX2_CALL(imp_00c901c8, 0x00956348u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = 0x0000000000000000ull;
    ctx->gpr[5] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = 0x0000000000000000ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000022cull), (uint32_t)ctx->gpr[0]);
    TOX2_CALL(imp_00c90028, 0x00956364u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
L_00956368:
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000088ull;
    ctx->gpr[5] = ctx->gpr[1] + 0x000000000000008cull;
    TOX2_CALL(imp_00c90188, 0x00956378u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x000000000000008cull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 1);
    if (ctx->cr[30]) goto L_009564a8;
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
L_0095638c:
    ctx->gpr[24] = tox2Rotl32Dup(ctx->gpr[0], 0) & 0x00000000000000ffull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[24], 0);
    if (!ctx->cr[30]) { TOX2_LOOP(0x00956394u); goto L_00956094; }
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[4] = ctx->gpr[25] + 0x0000000000000238ull;
    ctx->gpr[5] = ctx->gpr[25] + 0x000000000000023cull;
    ctx->gpr[6] = ctx->gpr[25] + 0x0000000000000240ull;
    ctx->gpr[4] = ctx->gpr[4] & 0x00000000ffffffffull;
    ctx->gpr[5] = ctx->gpr[5] & 0x00000000ffffffffull;
    ctx->gpr[6] = ctx->gpr[6] & 0x00000000ffffffffull;
    ctx->gpr[27] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000018ull));
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    TOX2_CALL(imp_00c90088, 0x009563c0u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000254ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[9], 0);
    if (!ctx->cr[30]) goto L_009565c4;
L_009563d0:
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000080ull;
    TOX2_CALL(imp_00c901e8, 0x009563dcu);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000258ull), (uint32_t)ctx->gpr[9]);
    if (!ctx->cr[30]) { TOX2_LOOP(0x009563f0u); goto L_00956094; }
    ctx->gpr[9] = 0xffffffffaaaa0000ull;
    ctx->gpr[11] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[9] = ctx->gpr[9] | 0x000000000000aaabull;
    ctx->gpr[5] = ctx->gpr[1] + 0x0000000000000074ull;
    ctx->gpr[6] = ctx->gpr[1] + 0x0000000000000078ull;
    ctx->gpr[7] = ctx->gpr[1] + 0x000000000000007cull;
    ctx->gpr[9] = tox2Mulhwu(ctx->gpr[27], ctx->gpr[9]);
    ctx->gpr[9] = tox2Rotl32Dup(ctx->gpr[9], 31) & 0x000000007fffffffull;
    ctx->gpr[0] = tox2Rotl32Dup(ctx->gpr[9], 1) & 0x00000000fffffffeull;
    ctx->gpr[0] = ctx->gpr[0] + ctx->gpr[9];
    ctx->gpr[9] = ctx->gpr[27] - ctx->gpr[0];
    ctx->gpr[28] = tox2Rotl32Dup(ctx->gpr[9], 2) & 0x00000000fffffffcull;
    ctx->gpr[0] = ctx->gpr[28] + 0x0000000000000020ull;
    ctx->gpr[29] = (uint64_t)(int64_t)(int32_t)ctx->gpr[0];
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + ctx->gpr[29]));
    TOX2_CALL(imp_00c90148, 0x00956438u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = ctx->gpr[3] ^ 0x0000000080610000ull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 818);
    if (ctx->cr[30]) goto L_00956624;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (ctx->cr[30]) goto L_009564c0;
    ctx->gpr[0] = ctx->gpr[3] ^ 0x0000000080610000ull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 817);
    if (ctx->cr[30]) goto L_0095646c;
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000090ull;
    TOX2_CALL(imp_00c901a8, 0x00956468u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
L_0095646c:
    ctx->gpr[0] = 0x0000000000000001ull;
    ctx->gpr[8] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    tox2Write8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000248ull), (uint8_t)ctx->gpr[0]);
    { TOX2_LOOP(0x00956478u); goto L_00956098; }
L_0095647c:
    ctx->gpr[5] = tox2Read32(base, (uint32_t)(ctx->gpr[3] + 0x0000000000000228ull));
    ctx->gpr[26] = ctx->gpr[3] + 0x0000000000000008ull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000224ull));
    ctx->gpr[7] = ctx->gpr[1] + 0x0000000000000070ull;
    ctx->gpr[3] = ctx->gpr[26] & 0x00000000ffffffffull;
    ctx->gpr[6] = ctx->gpr[5] | ctx->gpr[5];
    TOX2_CALL(imp_00c900e8, 0x00956498u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[3], 0);
    if (!ctx->cr[30]) { TOX2_LOOP(0x009564a0u); goto L_00956094; }
    { TOX2_LOOP(0x009564a4u); goto L_00956088; }
L_009564a8:
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = 0xffffffffffffffffull;
    TOX2_CALL(imp_00c90128, 0x009564b4u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    { TOX2_LOOP(0x009564bcu); goto L_0095638c; }
L_009564c0:
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (!ctx->cr[30]) { TOX2_LOOP(0x009564c8u); goto L_0095646c; }
    ctx->gpr[7] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000248ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[7], 0);
    if (!ctx->cr[30]) { TOX2_LOOP(0x009564d4u); goto L_00956094; }
    ctx->gpr[11] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[10] = (uint64_t)(int64_t)(int32_t)ctx->gpr[28];
    ctx->gpr[6] = ctx->gpr[23] & 0x00000000ffffffffull;
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000074ull));
    ctx->gpr[8] = ctx->gpr[11] + ctx->gpr[29];
    ctx->gpr[10] = ctx->gpr[11] + ctx->gpr[10];
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000018ull));
    ctx->gpr[9] = ctx->gpr[9] + 0x0000000000000001ull;
    tox2Write32(base, (uint32_t)(ctx->gpr[8] + 0x000000000000000cull), (uint32_t)ctx->gpr[0]);
    tox2Write32(base, (uint32_t)(ctx->gpr[10] + 0x0000000000000004ull), (uint32_t)ctx->gpr[7]);
    tox2Write32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000018ull), (uint32_t)ctx->gpr[9]);
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[6] + 0x00000000000000ffull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (!ctx->cr[30]) goto L_00956520;
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[9], 0x00000002u);
    if (!ctx->cr[29]) goto L_00956520;
    ctx->gpr[0] = 0x0000000000000001ull;
    tox2Write8(base, (uint32_t)(ctx->gpr[6] + 0x00000000000000ffull), (uint8_t)ctx->gpr[0]);
L_00956520:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x0000000000000066ull;
    TOX2_SYSCALL(0x0095652cu);
    ctx->gpr[29] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000000ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000018ull));
    ctx->gpr[9] = ctx->gpr[9] + 0x0000000000000002ull;
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[0]);
    if (!ctx->cr[28]) goto L_009565a8;
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (!ctx->cr[30]) goto L_009565a8;
    ctx->gpr[29] = ctx->gpr[1] + 0x0000000000000090ull;
    goto L_00956568;
L_0095655c:
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (!ctx->cr[30]) goto L_009565a8;
L_00956568:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x000000000000006bull;
    TOX2_SYSCALL(0x00956574u);
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[29] & 0x00000000ffffffffull;
    TOX2_CALL(imp_00c901e8, 0x00956584u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[11] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000004ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000090ull));
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000258ull), (uint32_t)ctx->gpr[0]);
    ctx->gpr[9] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000000ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000018ull));
    ctx->gpr[9] = ctx->gpr[9] + 0x0000000000000002ull;
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[0], (uint32_t)ctx->gpr[9]);
    if (ctx->cr[29]) { TOX2_LOOP(0x009565a4u); goto L_0095655c; }
L_009565a8:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[11] = 0x0000000000000068ull;
    TOX2_SYSCALL(0x009565b0u);
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[0], 0);
    if (ctx->cr[30]) { TOX2_LOOP(0x009565bcu); goto L_0095638c; }
    { TOX2_LOOP(0x009565c0u); goto L_00956094; }
L_009565c4:
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000023cull));
    ctx->gpr[28] = ctx->gpr[9] | ctx->gpr[9];
    tox2CmpU(ctx, 7, (uint32_t)ctx->gpr[9], (uint32_t)ctx->gpr[0]);
    if (!ctx->cr[29]) goto L_009565d8;
    ctx->gpr[28] = ctx->gpr[0] | ctx->gpr[0];
L_009565d8:
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000250ull));
    ctx->gpr[29] = ctx->gpr[28] & 0x00000000ffffffffull;
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000024cull));
    ctx->gpr[4] = ctx->gpr[4] - ctx->gpr[9];
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000238ull));
    ctx->gpr[5] = ctx->gpr[29] | ctx->gpr[29];
    ctx->gpr[4] = ctx->gpr[4] + ctx->gpr[0];
    ctx->gpr[4] = ctx->gpr[4] & 0x00000000ffffffffull;
    TOX2_CALL(fn_00985a08, 0x009565fcu);
    ctx->gpr[3] = ctx->gpr[30] | ctx->gpr[30];
    ctx->gpr[4] = ctx->gpr[29] | ctx->gpr[29];
    TOX2_CALL(imp_00c900a8, 0x0095660cu);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000254ull));
    ctx->gpr[0] = ctx->gpr[0] - ctx->gpr[28];
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000258ull), (uint32_t)ctx->gpr[24]);
    tox2Write32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000254ull), (uint32_t)ctx->gpr[0]);
    { TOX2_LOOP(0x00956620u); goto L_009563d0; }
L_00956624:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[11] = 0x0000000000000066ull;
    TOX2_SYSCALL(0x00956630u);
    ctx->gpr[4] = 0x0000000000000000ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000220ull));
    ctx->gpr[11] = 0x000000000000006bull;
    TOX2_SYSCALL(0x00956640u);
    ctx->gpr[11] = 0x0000000000000068ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x000000000000021cull));
    TOX2_SYSCALL(0x0095664cu);
    ctx->gpr[0] = tox2Read8(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000218ull));
    { TOX2_LOOP(0x00956654u); goto L_0095638c; }
}
// clang-format on

// Hand-written replacement for the ATRAC stream loader (guest 0x00956a00). It is the recompiled body with three
// edits, so the flag check at obj+0x244, the direct ring copy and the byte count at obj+0x234 run under one hold of
// the mutex at obj+0x21c; the original takes the mutex only after the copy, which lets the decoder setup
// (0x00955f50) set the flag and read the count in between.
//   1. The mutex is locked right after the free-space early exit (0x00956a34).
//   2. The staging path unlocks it again before its own lock at 0x00956a94.
//   3. The direct path drops its lock at 0x00956af8; the mutex is still held from edit 1.
// The free-space call stays outside the lock, as in the original: only this loader adds to obj+0x234.
//
// Only guest-visible interfaces are used (syscalls and generated functions), so the file links against test fakes
// without a generated tree.

#include "tox2/ppu.h"

TOX2_PPU_FUNC(fn_00954a08);
TOX2_PPU_FUNC(fn_00955eb8);
TOX2_PPU_FUNC(fn_00985a08);

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

// Hand-written replacement for the recompiled save DLC check (guest 0x00523fb8). After a save is read, the original
// takes the save's set of DLC items (section SAVE_DATA_ID_ITEM_DATA_MANAGER, offset 0x3000) and returns 0 when one of
// them belongs to no installed DLC. The save task stores that result at +5 of its state (0x10daecc), drops the save and
// the game shows "the rights of the downloadable content used by this save could not be confirmed". With no DLC
// installed, every save that used DLC fails this way.
//
// The body is the recompiled one with one edit: the "not licensed" exit at 0x0052422c returns 1 (li r31,1) instead of
// 0, so the check always passes. The item vector is still freed on that path. Items of DLC that is not installed have
// no item record; the item and equipment menus were checked with such a save.
//
// Only generated functions are called, so the file links against test fakes without a generated tree.

#include "tox2/ppu.h"

TOX2_PPU_FUNC(fn_001da10c);
TOX2_PPU_FUNC(fn_00522b44);
TOX2_PPU_FUNC(fn_005333d8);
TOX2_PPU_FUNC(fn_0053c81c);
TOX2_PPU_FUNC(fn_009b4088);

TOX2_PPU_FUNC(fn_00523fb8) {
    TOX2_ENTER(0x00523fb8u);
    ctx->gpr[0] = 0x0000000000000000ull;
    ctx->gpr[4] = ctx->lr;
    {
        uint64_t t0 = (ctx->gpr[1] + 0xffffffffffffff40ull);
        tox2Write64(base, (uint32_t)t0, (uint64_t)ctx->gpr[1]);
        ctx->gpr[1] = t0;
    }
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b8ull), (uint64_t)ctx->gpr[31]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000090ull), (uint64_t)ctx->gpr[26]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000098ull), (uint64_t)ctx->gpr[27]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a0ull), (uint64_t)ctx->gpr[28]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a8ull), (uint64_t)ctx->gpr[29]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b0ull), (uint64_t)ctx->gpr[30]);
    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d0ull), (uint64_t)ctx->gpr[4]);
    ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];
    tox2Write32(base, (uint32_t)(ctx->gpr[1] + 0x000000000000007cull), (uint32_t)ctx->gpr[0]);
    tox2Write32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000074ull), (uint32_t)ctx->gpr[0]);
    tox2Write32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull), (uint32_t)ctx->gpr[0]);
    TOX2_CALL(fn_005333d8, 0x00523ff4u);
    ctx->gpr[3] = ctx->gpr[3] & 0x00000000ffffffffull;
    ctx->gpr[5] = ctx->gpr[31] & 0x00000000ffffffffull;
    ctx->gpr[4] = ctx->gpr[1] + 0x0000000000000070ull;
    TOX2_CALL(fn_0053c81c, 0x00524008u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000074ull));
    ctx->gpr[26] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000078ull));
    ctx->gpr[3] = ctx->gpr[31] | ctx->gpr[31];
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[31], (int32_t)ctx->gpr[26]);
    if (ctx->cr[30]) goto L_00524120;
    ctx->gpr[11] = ctx->gpr[31] & 0x00000000ffffffffull;
    ctx->gpr[7] = ctx->gpr[26] - ctx->gpr[31];
    ctx->gpr[27] = ctx->gpr[1] + 0x0000000000000080ull;
    ctx->gpr[6] = ctx->gpr[7] + 0xfffffffffffffffcull;
    ctx->gpr[28] = ctx->gpr[1] + 0x0000000000000084ull;
    ctx->gpr[29] = tox2Rotl64(ctx->gpr[6], 62) & 0x0000000000000003ull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[11] + 0x0000000000000000ull));
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    TOX2_CALL(fn_00522b44, 0x00524044u);
    ctx->gpr[30] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x00524050u);
    ctx->gpr[8] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[8], (int32_t)ctx->gpr[30]);
    if (ctx->cr[30]) goto L_0052422c;
    ctx->gpr[30] = ctx->gpr[31] + 0x0000000000000004ull;
    tox2CmpS(ctx, 0, (int32_t)ctx->gpr[26], (int32_t)ctx->gpr[30]);
    if (ctx->cr[2]) goto L_0052411c;
    tox2CmpS(ctx, 1, (int32_t)ctx->gpr[29], 0);
    if (ctx->cr[6]) goto L_00524200;
    tox2CmpS(ctx, 6, (int32_t)ctx->gpr[29], 1);
    if (ctx->cr[26]) goto L_005240e4;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[29], 2);
    if (ctx->cr[30]) goto L_005240b4;
    ctx->gpr[10] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[10] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x00524094u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x005240a0u);
    ctx->gpr[12] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 0, (int32_t)ctx->gpr[12], (int32_t)ctx->gpr[31]);
    if (ctx->cr[2]) goto L_0052422c;
    ctx->gpr[30] = ctx->gpr[30] + 0x0000000000000004ull;
L_005240b4:
    ctx->gpr[29] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[29] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x005240c4u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x005240d0u);
    ctx->gpr[0] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 1, (int32_t)ctx->gpr[0], (int32_t)ctx->gpr[31]);
    if (ctx->cr[6]) goto L_0052422c;
    ctx->gpr[30] = ctx->gpr[30] + 0x0000000000000004ull;
L_005240e4:
    ctx->gpr[31] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[31] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x005240f4u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x00524100u);
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 6, (int32_t)ctx->gpr[4], (int32_t)ctx->gpr[31]);
    if (ctx->cr[26]) goto L_0052422c;
    ctx->gpr[30] = ctx->gpr[30] + 0x0000000000000004ull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[26], (int32_t)ctx->gpr[30]);
    if (!ctx->cr[30]) goto L_00524200;
L_0052411c:
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000074ull));
L_00524120:
    ctx->gpr[31] = 0x0000000000000001ull;
L_00524124:
    tox2CmpS(ctx, 1, (int32_t)ctx->gpr[3], 0);
    if (ctx->cr[6]) goto L_00524138;
    ctx->gpr[3] = ctx->gpr[3] & 0x00000000ffffffffull;
    TOX2_CALL(fn_009b4088, 0x00524134u);
    ctx->gpr[2] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull));
L_00524138:
    ctx->gpr[5] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000d0ull));
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)ctx->gpr[31];
    ctx->gpr[26] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000090ull));
    ctx->gpr[27] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000098ull));
    ctx->lr = ctx->gpr[5];
    ctx->gpr[28] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a0ull));
    ctx->gpr[29] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000a8ull));
    ctx->gpr[30] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b0ull));
    ctx->gpr[31] = tox2Read64(base, (uint32_t)(ctx->gpr[1] + 0x00000000000000b8ull));
    ctx->gpr[1] = ctx->gpr[1] + 0x00000000000000c0ull;
    return;
L_00524164:
    ctx->gpr[29] = ctx->gpr[30] + 0x0000000000000004ull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[9] = ctx->gpr[29] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x00524178u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x00524184u);
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 0, (int32_t)ctx->gpr[3], (int32_t)ctx->gpr[31]);
    if (ctx->cr[2]) goto L_0052422c;
    ctx->gpr[11] = ctx->gpr[29] + 0x0000000000000004ull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[5] = ctx->gpr[11] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[5] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x005241a8u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x005241b4u);
    ctx->gpr[6] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 1, (int32_t)ctx->gpr[6], (int32_t)ctx->gpr[31]);
    if (ctx->cr[6]) goto L_0052422c;
    ctx->gpr[8] = ctx->gpr[30] + 0x000000000000000cull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[7] = ctx->gpr[8] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[7] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x005241d8u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x005241e4u);
    ctx->gpr[10] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 6, (int32_t)ctx->gpr[10], (int32_t)ctx->gpr[31]);
    if (ctx->cr[26]) goto L_0052422c;
    ctx->gpr[30] = ctx->gpr[30] + 0x0000000000000010ull;
    tox2CmpS(ctx, 7, (int32_t)ctx->gpr[26], (int32_t)ctx->gpr[30]);
    if (ctx->cr[30]) {
        TOX2_LOOP(0x005241fcu);
        goto L_0052411c;
    }
L_00524200:
    ctx->gpr[9] = ctx->gpr[30] & 0x00000000ffffffffull;
    ctx->gpr[3] = ctx->gpr[27] & 0x00000000ffffffffull;
    ctx->gpr[4] = tox2Read32(base, (uint32_t)(ctx->gpr[9] + 0x0000000000000000ull));
    TOX2_CALL(fn_00522b44, 0x00524210u);
    ctx->gpr[31] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000080ull));
    ctx->gpr[3] = ctx->gpr[28] & 0x00000000ffffffffull;
    TOX2_CALL(fn_001da10c, 0x0052421cu);
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000084ull));
    tox2CmpS(ctx, 0, (int32_t)ctx->gpr[3], (int32_t)ctx->gpr[31]);
    if (!ctx->cr[2]) {
        TOX2_LOOP(0x00524228u);
        goto L_00524164;
    }
L_0052422c:
    // Edit: 1 (licensed) where the original returns 0.
    ctx->gpr[31] = 0x0000000000000001ull;
    ctx->gpr[3] = tox2Read32(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000074ull));
    {
        TOX2_LOOP(0x00524234u);
        goto L_00524124;
    }
}

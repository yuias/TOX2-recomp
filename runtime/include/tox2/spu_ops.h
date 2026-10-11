#pragma once

// Instruction semantics helpers for generated SPU code. Each helper takes the source registers by
// value and returns the new register image, so `ctx->gpr[rt] = tox2SpuOp(ctx->gpr[ra], ...)` is
// correct when rt aliases a source. Lane-wise operations work on the host lanes directly, because
// Tox2Vr keeps one host lane per big-endian element (see spu_context.h); only operations that name an
// element (the preferred slot, doubleword halves) go through TOX2_VR_*.
//
// The harness builds with UBSan traps, so every shift count is in range, signed overflow is avoided
// by doing the arithmetic on unsigned lanes, and rotates are written without a shift by the width.
//
// Like the other contract headers this compiles as C23 and C++26 and with -ffreestanding.

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Constants -------------------------------------------------------------------------------

TOX2_FORCEINLINE Tox2Vr tox2SpuSplatW(uint32_t x) {
    Tox2Vr v;
    v.u32 = (Tox2U32x4){x, x, x, x};
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuSplatH(uint16_t x) {
    Tox2Vr v;
    v.u16 = (Tox2U16x8){x, x, x, x, x, x, x, x};
    return v;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuSplatB(uint8_t x) {
    Tox2Vr v;
    v.u8 = (Tox2U8x16){x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x};
    return v;
}

// --- Add and subtract ------------------------------------------------------------------------

TOX2_FORCEINLINE Tox2Vr tox2SpuA(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 + b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuAh(Tox2Vr a, Tox2Vr b) {
    a.u16 = a.u16 + b.u16;
    return a;
}

// Subtracts from. sf computes RB - RA.
TOX2_FORCEINLINE Tox2Vr tox2SpuSf(Tox2Vr a, Tox2Vr b) {
    b.u32 = b.u32 - a.u32;
    return b;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuSfh(Tox2Vr a, Tox2Vr b) {
    b.u16 = b.u16 - a.u16;
    return b;
}

// Carry out of x + y + cin (cin is 0 or 1 per lane), as 0 or 1.
TOX2_FORCEINLINE Tox2U32x4 tox2SpuCarryOut(Tox2U32x4 x, Tox2U32x4 y, Tox2U32x4 cin) {
    Tox2U32x4 sum = x + y;
    Tox2U32x4 sum_c = sum + cin;
    return ((Tox2U32x4)(sum < x) | (Tox2U32x4)(sum_c < sum)) & 1u;
}

// The extended forms take their carry-in from the least significant bit of each word of RT.
TOX2_FORCEINLINE Tox2Vr tox2SpuAddx(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    a.u32 = a.u32 + b.u32 + (rt.u32 & 1u);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCg(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = tox2SpuCarryOut(a.u32, b.u32, (Tox2U32x4){0, 0, 0, 0});
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCgx(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    Tox2Vr r;
    r.u32 = tox2SpuCarryOut(a.u32, b.u32, rt.u32 & 1u);
    return r;
}

// RB - RA - 1 + carry-in, i.e. RB + ~RA + carry-in.
TOX2_FORCEINLINE Tox2Vr tox2SpuSfx(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    b.u32 = b.u32 + ~a.u32 + (rt.u32 & 1u);
    return b;
}

// Borrow generate is the carry of RB + ~RA + 1: 1 when RB - RA does not borrow.
TOX2_FORCEINLINE Tox2Vr tox2SpuBg(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (Tox2U32x4)(b.u32 >= a.u32) & 1u;
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuBgx(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    Tox2Vr r;
    r.u32 = tox2SpuCarryOut(~a.u32, b.u32, rt.u32 & 1u);
    return r;
}

// --- Multiply --------------------------------------------------------------------------------

// Halfword views of the words: "lo" is the low halfword of each word (bits 16 to 31 in ISA numbering).
TOX2_FORCEINLINE Tox2S32x4 tox2SpuLoS(Tox2Vr v) { return (Tox2S32x4)(v.u32 << 16) >> 16; }
TOX2_FORCEINLINE Tox2S32x4 tox2SpuHiS(Tox2Vr v) { return v.s32 >> 16; }
TOX2_FORCEINLINE Tox2U32x4 tox2SpuLoU(Tox2Vr v) { return v.u32 & 0xffffu; }
TOX2_FORCEINLINE Tox2U32x4 tox2SpuHiU(Tox2Vr v) { return v.u32 >> 16; }

// A 16 x 16 bit product always fits in 32 bits (at most 2^30 signed), so the signed lanes cannot overflow.
TOX2_FORCEINLINE Tox2Vr tox2SpuMpy(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.s32 = tox2SpuLoS(a) * tox2SpuLoS(b);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuMpyu(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = tox2SpuLoU(a) * tox2SpuLoU(b);
    return r;
}

// High halfword of RA times low halfword of RB, signed, shifted left 16.
TOX2_FORCEINLINE Tox2Vr tox2SpuMpyh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (Tox2U32x4)(tox2SpuHiS(a) * tox2SpuLoS(b)) << 16;
    return r;
}

// Signed low halfword product shifted right 16 (arithmetic).
TOX2_FORCEINLINE Tox2Vr tox2SpuMpys(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.s32 = (tox2SpuLoS(a) * tox2SpuLoS(b)) >> 16;
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuMpyhh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.s32 = tox2SpuHiS(a) * tox2SpuHiS(b);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuMpyhha(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    rt.u32 = rt.u32 + (Tox2U32x4)(tox2SpuHiS(a) * tox2SpuHiS(b));
    return rt;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuMpyhhu(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = tox2SpuHiU(a) * tox2SpuHiU(b);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuMpyhhau(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    rt.u32 = rt.u32 + tox2SpuHiU(a) * tox2SpuHiU(b);
    return rt;
}

// RT = low(RA) * low(RB) + RC, signed halfwords, 32-bit sum.
TOX2_FORCEINLINE Tox2Vr tox2SpuMpya(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    c.u32 = c.u32 + (Tox2U32x4)(tox2SpuLoS(a) * tox2SpuLoS(b));
    return c;
}

// --- Byte operations -------------------------------------------------------------------------

// (a + b + 1) >> 1 without the carry out of the byte: (a | b) - ((a ^ b) >> 1).
TOX2_FORCEINLINE Tox2Vr tox2SpuAvgb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u8 = (a.u8 | b.u8) - ((a.u8 ^ b.u8) >> 1);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuAbsdb(Tox2Vr a, Tox2Vr b) {
    Tox2U8x16 gt = (Tox2U8x16)(a.u8 > b.u8);
    Tox2Vr r;
    r.u8 = ((a.u8 - b.u8) & gt) | ((b.u8 - a.u8) & ~gt);
    return r;
}

TOX2_FORCEINLINE Tox2U32x4 tox2SpuByteSum(Tox2U32x4 w) {
    return (w & 0xffu) + ((w >> 8) & 0xffu) + ((w >> 16) & 0xffu) + (w >> 24);
}

// Per word: the sum of RB's four bytes in the upper halfword, the sum of RA's in the lower.
TOX2_FORCEINLINE Tox2Vr tox2SpuSumb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (tox2SpuByteSum(b.u32) << 16) | tox2SpuByteSum(a.u32);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCntb(Tox2Vr a) {
    Tox2U8x16 x = a.u8;
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    x = (x + (x >> 4)) & 0x0f;
    a.u8 = x;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuClz(Tox2Vr a) {
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t w = a.u32[i];
        a.u32[i] = w != 0 ? (uint32_t)__builtin_clz(w) : 32u;
    }
    return a;
}

// --- Sign extension --------------------------------------------------------------------------

// Each halfword takes its low byte, sign-extended.
TOX2_FORCEINLINE Tox2Vr tox2SpuXsbh(Tox2Vr a) {
    a.s16 = (Tox2S16x8)(a.u16 << 8) >> 8;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuXshw(Tox2Vr a) {
    a.s32 = (Tox2S32x4)(a.u32 << 16) >> 16;
    return a;
}

// Each doubleword takes its low word (the odd word), sign-extended.
TOX2_FORCEINLINE Tox2Vr tox2SpuXswd(Tox2Vr a) {
    Tox2Vr r;
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t lo = TOX2_VR_W(a, 2 * i + 1);
        TOX2_VR_W(r, 2 * i) = (uint32_t)((int32_t)lo >> 31);
        TOX2_VR_W(r, 2 * i + 1) = lo;
    }
    return r;
}

// --- Logical ---------------------------------------------------------------------------------

TOX2_FORCEINLINE Tox2Vr tox2SpuAnd(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 & b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuAndc(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 & ~b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuOr(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 | b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuOrc(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 | ~b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuXor(Tox2Vr a, Tox2Vr b) {
    a.u32 = a.u32 ^ b.u32;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuNand(Tox2Vr a, Tox2Vr b) {
    a.u32 = ~(a.u32 & b.u32);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuNor(Tox2Vr a, Tox2Vr b) {
    a.u32 = ~(a.u32 | b.u32);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuEqv(Tox2Vr a, Tox2Vr b) {
    a.u32 = ~(a.u32 ^ b.u32);
    return a;
}

// Bit select: RB where the RC bit is 1, RA where it is 0.
TOX2_FORCEINLINE Tox2Vr tox2SpuSelb(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    a.u32 = (b.u32 & c.u32) | (a.u32 & ~c.u32);
    return a;
}

// --- Compare ---------------------------------------------------------------------------------

TOX2_FORCEINLINE Tox2Vr tox2SpuCeqb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u8 = (Tox2U8x16)(a.u8 == b.u8);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCeqh(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u16 = (Tox2U16x8)(a.u16 == b.u16);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCeq(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (Tox2U32x4)(a.u32 == b.u32);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCgtb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u8 = (Tox2U8x16)(a.s8 > b.s8);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCgth(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u16 = (Tox2U16x8)(a.s16 > b.s16);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCgt(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (Tox2U32x4)(a.s32 > b.s32);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuClgtb(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u8 = (Tox2U8x16)(a.u8 > b.u8);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuClgth(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u16 = (Tox2U16x8)(a.u16 > b.u16);
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuClgt(Tox2Vr a, Tox2Vr b) {
    Tox2Vr r;
    r.u32 = (Tox2U32x4)(a.u32 > b.u32);
    return r;
}

// --- Shift and rotate of elements ------------------------------------------------------------
//
// The count of each element comes from the same element of RB (the immediate forms splat it). Only
// the low bits take part: 6 for words and 5 for halfwords in the shifts (a count above the width gives
// 0, or sign fill for the arithmetic shifts), 5 and 4 in the rotates. The right shifts use the negated
// count, so a count of -n shifts right by n.

TOX2_FORCEINLINE Tox2Vr tox2SpuShl(Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 count = b.u32 & 0x3fu;
    a.u32 = (a.u32 << (count & 31u)) & (Tox2U32x4)(count < 32u);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuShlh(Tox2Vr a, Tox2Vr b) {
    Tox2U16x8 count = b.u16 & 0x1f;
    a.u16 = (a.u16 << (count & 15)) & (Tox2U16x8)(count < 16);
    return a;
}

// A rotate by 0 would shift by the width, so the complementary count is reduced modulo the width.
TOX2_FORCEINLINE Tox2Vr tox2SpuRot(Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 count = b.u32 & 31u;
    a.u32 = (a.u32 << count) | (a.u32 >> ((32u - count) & 31u));
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRoth(Tox2Vr a, Tox2Vr b) {
    Tox2U16x8 count = b.u16 & 15;
    a.u16 = (a.u16 << count) | (a.u16 >> ((16 - count) & 15));
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotm(Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 count = (0u - b.u32) & 0x3fu;
    a.u32 = (a.u32 >> (count & 31u)) & (Tox2U32x4)(count < 32u);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRothm(Tox2Vr a, Tox2Vr b) {
    Tox2U16x8 count = (0 - b.u16) & 0x1f;
    a.u16 = (a.u16 >> (count & 15)) & (Tox2U16x8)(count < 16);
    return a;
}

// An arithmetic shift by the width or more gives the sign fill, which a shift by width - 1 also gives.
TOX2_FORCEINLINE Tox2Vr tox2SpuRotma(Tox2Vr a, Tox2Vr b) {
    Tox2U32x4 count = (0u - b.u32) & 0x3fu;
    count = count ^ ((count ^ 31u) & (Tox2U32x4)(count > 31u));
    a.s32 = a.s32 >> count;
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotmah(Tox2Vr a, Tox2Vr b) {
    Tox2U16x8 count = (0 - b.u16) & 0x1f;
    count = count ^ ((count ^ 15) & (Tox2U16x8)(count > 15));
    a.s16 = a.s16 >> count;
    return a;
}

// --- Quadword shifts and rotates -------------------------------------------------------------
//
// With the byte-reversed lanes the whole register is one little-endian 128-bit integer whose top bit
// is bit 0 of the architected register, so an architected left shift is a plain left shift of that
// integer and a byte shift is a shift by 8 times the count. Every helper takes the count word the
// instruction names (the preferred word of RB, or the sign-extended I7) and applies its own mask, so
// the register and immediate forms share the code.

__extension__ typedef unsigned __int128 Tox2SpuU128;

TOX2_FORCEINLINE Tox2SpuU128 tox2SpuToInt(Tox2Vr v) {
    Tox2SpuU128 x;
    __builtin_memcpy(&x, &v, sizeof x);
    return x;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFromInt(Tox2SpuU128 x) {
    Tox2Vr v;
    __builtin_memcpy(&v, &x, sizeof v);
    return v;
}

// Rotate by 0 < n < 128 bits, and by 0 without a shift by the width.
TOX2_FORCEINLINE Tox2SpuU128 tox2SpuRotl128(Tox2SpuU128 x, uint32_t n) { return (x << n) | (x >> ((128u - n) & 127u)); }

TOX2_FORCEINLINE Tox2Vr tox2SpuShlqbi(Tox2Vr a, uint32_t count) {
    return tox2SpuFromInt(tox2SpuToInt(a) << (count & 7u));
}

// A byte count above 15 shifts every byte out.
TOX2_FORCEINLINE Tox2Vr tox2SpuShlqby(Tox2Vr a, uint32_t count) {
    uint32_t s = count & 0x1fu;
    return tox2SpuFromInt(s < 16u ? tox2SpuToInt(a) << (8u * s) : (Tox2SpuU128)0);
}

// The "from bit shift count" forms take the byte count from bits 24 to 28 of the word.
TOX2_FORCEINLINE Tox2Vr tox2SpuShlqbybi(Tox2Vr a, uint32_t count) { return tox2SpuShlqby(a, count >> 3); }

TOX2_FORCEINLINE Tox2Vr tox2SpuRotqbi(Tox2Vr a, uint32_t count) {
    return tox2SpuFromInt(tox2SpuRotl128(tox2SpuToInt(a), count & 7u));
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotqby(Tox2Vr a, uint32_t count) {
    return tox2SpuFromInt(tox2SpuRotl128(tox2SpuToInt(a), 8u * (count & 15u)));
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotqbybi(Tox2Vr a, uint32_t count) { return tox2SpuRotqby(a, count >> 3); }

// The mask forms shift right by the negated count.
TOX2_FORCEINLINE Tox2Vr tox2SpuRotqmbi(Tox2Vr a, uint32_t count) {
    return tox2SpuFromInt(tox2SpuToInt(a) >> ((0u - count) & 7u));
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotqmby(Tox2Vr a, uint32_t count) {
    uint32_t s = (0u - count) & 0x1fu;
    return tox2SpuFromInt(s < 16u ? tox2SpuToInt(a) >> (8u * s) : (Tox2SpuU128)0);
}

TOX2_FORCEINLINE Tox2Vr tox2SpuRotqmbybi(Tox2Vr a, uint32_t count) { return tox2SpuRotqmby(a, count >> 3); }

// --- Shuffle and generate controls -----------------------------------------------------------

// Byte k (architected numbering) of RA || RB is selected by the matching byte of RC. The control
// bytes are read lane by lane because RC and RT share the same lane order.
TOX2_FORCEINLINE Tox2Vr tox2SpuShufb(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    uint8_t concat[32];
    for (uint32_t k = 0; k < 16; k++) {
        concat[k] = a.u8[15 - k];
        concat[16 + k] = b.u8[15 - k];
    }
    Tox2Vr r;
    for (uint32_t lane = 0; lane < 16; lane++) {
        uint8_t sel = c.u8[lane];
        uint8_t v = concat[sel & 0x1fu];
        if ((sel & 0xc0u) == 0x80u) {
            v = 0x00;
        } else if ((sel & 0xe0u) == 0xc0u) {
            v = 0xff;
        } else if ((sel & 0xe0u) == 0xe0u) {
            v = 0x80;
        }
        r.u8[lane] = v;
    }
    return r;
}

// The mask for inserting an element of `size` bytes (1, 2, 4 or 8) at the aligned position of `addr`:
// bytes 0x10..0x1f select RB-side data (the old quadword), the element's own bytes select the
// right-aligned bytes of the preferred slot (the leftmost eight for a doubleword).
TOX2_FORCEINLINE Tox2Vr tox2SpuGenControl(uint32_t addr, uint32_t size) {
    uint32_t t = addr & (16u - size);
    uint32_t first = size < 8u ? 4u - size : 0u;
    Tox2Vr r;
    for (uint32_t j = 0; j < 16; j++) TOX2_VR_B(r, j) = (uint8_t)(0x10u + j);
    for (uint32_t i = 0; i < size; i++) TOX2_VR_B(r, t + i) = (uint8_t)(first + i);
    return r;
}

// --- Form select mask and gather bits --------------------------------------------------------
//
// The leftmost element takes the most significant of the selected bits, which is the highest lane.

TOX2_FORCEINLINE Tox2Vr tox2SpuFsmb(uint32_t s) {
    Tox2Vr r;
    for (uint32_t lane = 0; lane < 16; lane++) r.u8[lane] = (uint8_t)(0u - ((s >> lane) & 1u));
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFsmh(uint32_t s) {
    Tox2Vr r;
    for (uint32_t lane = 0; lane < 8; lane++) r.u16[lane] = (uint16_t)(0u - ((s >> lane) & 1u));
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFsm(uint32_t s) {
    Tox2Vr r;
    for (uint32_t lane = 0; lane < 4; lane++) r.u32[lane] = 0u - ((s >> lane) & 1u);
    return r;
}

// The gathered bits go to the preferred word; the other words are zero.
TOX2_FORCEINLINE Tox2Vr tox2SpuGatherResult(uint32_t bits) {
    Tox2Vr r = tox2SpuSplatW(0);
    TOX2_VR_W(r, 0) = bits;
    return r;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuGbb(Tox2Vr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 16; lane++) bits |= (uint32_t)(a.u8[lane] & 1u) << lane;
    return tox2SpuGatherResult(bits);
}

TOX2_FORCEINLINE Tox2Vr tox2SpuGbh(Tox2Vr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 8; lane++) bits |= (uint32_t)(a.u16[lane] & 1u) << lane;
    return tox2SpuGatherResult(bits);
}

TOX2_FORCEINLINE Tox2Vr tox2SpuGb(Tox2Vr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 4; lane++) bits |= (a.u32[lane] & 1u) << lane;
    return tox2SpuGatherResult(bits);
}

TOX2_FORCEINLINE Tox2Vr tox2SpuOrx(Tox2Vr a) { return tox2SpuGatherResult(a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3]); }

// --- Floating-point status and control register ------------------------------------------------
//
// fscrwr stores a register as it is (its unused bits are undefined); fscrrd returns the register with
// the unused bits forced to zero. The used bits per word are the rounding controls (bits 20:23 of
// word 0), the IEEE exception flags (50:55 of word 1, 82:87 of word 2), the single-precision flags
// (29:31, 61:63, 93:95, 125:127) and the divide-by-zero flags (116:119 of word 3).
TOX2_FORCEINLINE Tox2Vr tox2SpuFscrRead(Tox2Vr fpscr) {
    // Host lanes run opposite to the big-endian words, so lane 0 holds word 3.
    const Tox2U32x4 used = {0x00000f07u, 0x00003f07u, 0x00003f07u, 0x00000f07u};
    fpscr.u32 = fpscr.u32 & used;
    return fpscr;
}

#ifdef __cplusplus
}  // extern "C"
#endif

// The floating-point helpers build on the types above.
#include "spu_fp.h"

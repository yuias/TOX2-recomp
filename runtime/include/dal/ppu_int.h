#pragma once

#include "ppu_context.h"

// Integer helpers for generated code. Arithmetic stays on unsigned types so that no emitted
// expression has undefined behaviour.

// Signed overflow of the 64-bit sum r = a + b: both addends have the same sign and the result's
// differs (RPCS3 `ADD`; `SUBF` passes `~a` for the minuend).
DAL_FORCEINLINE bool dalAddOverflows(uint64_t a, uint64_t b, uint64_t r) { return ((a ^ r) & (b ^ r)) >> 63; }

// r = a + b + carry_in (0 or 1); XER.CA = carry out of bit 63 (RPCS3 `add64_flags`). The subtract
// forms pass `~a` and a carry_in of 1 or CA, so CA = !borrow.
DAL_FORCEINLINE uint64_t dalAddCarry(DalPpuContext* ctx, uint64_t a, uint64_t b, uint64_t carry_in) {
    uint64_t sum = a + b;
    uint64_t r = sum + carry_in;
    ctx->xer_ca = (sum < a) | (r < sum);
    return r;
}

// mullw: full product of the sign-extended low words; it overflows when it does not fit in 32 bits
// (RPCS3 `MULLW`). The product of two int32 values always fits in int64.
DAL_FORCEINLINE uint64_t dalMullw(uint64_t a, uint64_t b) {
    return (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
}

DAL_FORCEINLINE bool dalMullwOverflows(uint64_t product) { return (int64_t)product != (int64_t)(int32_t)product; }

// mulhw/mulhwu: high word of the 32x32 product. mulhw sign-extends the word into rD, mulhwu
// zero-extends (RPCS3 `MULHW`, `MULHWU`).
DAL_FORCEINLINE uint64_t dalMulhw(uint64_t a, uint64_t b) {
    return (uint64_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
}

DAL_FORCEINLINE uint64_t dalMulhwu(uint64_t a, uint64_t b) {
    return ((uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b) >> 32;
}

// mulhd/mulhdu: high 64 bits of the 128-bit product (RPCS3 `MULHD`, `MULHDU`).
__extension__ typedef __int128 DalI128;
__extension__ typedef unsigned __int128 DalU128;

DAL_FORCEINLINE uint64_t dalMulhd(uint64_t a, uint64_t b) {
    return (uint64_t)(((DalI128)(int64_t)a * (DalI128)(int64_t)b) >> 64);
}

DAL_FORCEINLINE uint64_t dalMulhdu(uint64_t a, uint64_t b) { return (uint64_t)(((DalU128)a * (DalU128)b) >> 64); }

// mulld OV: the signed 128-bit product does not fit in 64 bits (RPCS3 `MULLD`).
DAL_FORCEINLINE bool dalMulldOverflows(uint64_t a, uint64_t b, uint64_t low) {
    return dalMulhd(a, b) != (uint64_t)((int64_t)low >> 63);
}

// Division: a zero divisor, or INT_MIN / -1 for the signed forms, gives result 0 and OV = 1 instead
// of a host trap (RPCS3 `DIVW`, `DIVWU`, `DIVD`, `DIVDU`). divw/divwu results are zero-extended.
DAL_FORCEINLINE bool dalDivwOverflows(uint64_t a, uint64_t b) {
    int32_t ra = (int32_t)a, rb = (int32_t)b;
    return rb == 0 || (ra == INT32_MIN && rb == -1);
}

DAL_FORCEINLINE uint64_t dalDivw(uint64_t a, uint64_t b) {
    if (dalDivwOverflows(a, b)) return 0;
    return (uint64_t)(uint32_t)((int32_t)a / (int32_t)b);
}

DAL_FORCEINLINE bool dalDivwuOverflows(uint64_t b) { return (uint32_t)b == 0; }

DAL_FORCEINLINE uint64_t dalDivwu(uint64_t a, uint64_t b) {
    return dalDivwuOverflows(b) ? 0 : (uint64_t)((uint32_t)a / (uint32_t)b);
}

DAL_FORCEINLINE bool dalDivdOverflows(uint64_t a, uint64_t b) {
    return b == 0 || (a == (uint64_t)INT64_MIN && b == ~0ull);
}

DAL_FORCEINLINE uint64_t dalDivd(uint64_t a, uint64_t b) {
    return dalDivdOverflows(a, b) ? 0 : (uint64_t)((int64_t)a / (int64_t)b);
}

DAL_FORCEINLINE uint64_t dalDivdu(uint64_t a, uint64_t b) { return b == 0 ? 0 : a / b; }

// cntlzw looks at the low word only; both forms return the operand width for zero (the builtin
// is undefined there).
DAL_FORCEINLINE uint64_t dalCntlzw(uint64_t v) {
    uint32_t w = (uint32_t)v;
    return w ? (uint64_t)__builtin_clz(w) : 32u;
}

DAL_FORCEINLINE uint64_t dalCntlzd(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64u; }

// rlw*: rotate the low word and duplicate it into both halves, so that masks reaching into the
// high half (MB > ME) see the rotated word there (RPCS3 `dup32`, `rol32`). The count is mod 32.
DAL_FORCEINLINE uint64_t dalRotl32Dup(uint64_t v, uint32_t n) {
    uint32_t w = (uint32_t)v;
    n &= 31;
    uint32_t r = n ? (w << n) | (w >> (32 - n)) : w;
    return ((uint64_t)r << 32) | r;
}

DAL_FORCEINLINE uint64_t dalRotl64(uint64_t v, uint32_t n) {
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}

// slw/srw: the amount is the low 6 bits of rB; 32..63 gives 0. The result is zero-extended.
DAL_FORCEINLINE uint64_t dalSlw(uint64_t v, uint64_t amount) {
    return (amount & 0x20) ? 0 : (uint64_t)(uint32_t)(v << (amount & 31));
}

DAL_FORCEINLINE uint64_t dalSrw(uint64_t v, uint64_t amount) {
    return (amount & 0x20) ? 0 : (uint64_t)((uint32_t)v >> (amount & 31));
}

// sld/srd: the amount is the low 7 bits of rB; 64..127 gives 0.
DAL_FORCEINLINE uint64_t dalSld(uint64_t v, uint64_t amount) { return (amount & 0x40) ? 0 : v << (amount & 63); }

DAL_FORCEINLINE uint64_t dalSrd(uint64_t v, uint64_t amount) { return (amount & 0x40) ? 0 : v >> (amount & 63); }

// sraw/srawi: arithmetic shift of the sign-extended low word (`amount` is taken mod 64); CA = the
// word is negative and a 1 bit was shifted out. Amounts above 31 give the sign fill (RPCS3 `SRAW`).
DAL_FORCEINLINE uint64_t dalSraw(DalPpuContext* ctx, uint64_t v, uint64_t amount) {
    uint32_t w = (uint32_t)v;
    uint32_t n = (uint32_t)(amount & 63);
    bool negative = w >> 31;
    if (n > 31) {
        ctx->xer_ca = negative;
        return negative ? ~0ull : 0;
    }
    ctx->xer_ca = negative && (w & ((1u << n) - 1)) != 0;
    // Shift on the unsigned word and refill the vacated bits with the sign.
    uint32_t r = (w >> n) | (negative && n ? ~0u << (32 - n) : 0);
    return (uint64_t)(int64_t)(int32_t)r;
}

// srad/sradi: 64-bit form; the amount is the low 7 bits of rB (RPCS3 `SRAD`).
DAL_FORCEINLINE uint64_t dalSrad(DalPpuContext* ctx, uint64_t v, uint64_t amount) {
    uint32_t n = (uint32_t)(amount & 127);
    bool negative = v >> 63;
    if (n > 63) {
        ctx->xer_ca = negative;
        return negative ? ~0ull : 0;
    }
    ctx->xer_ca = negative && (v & ((1ull << n) - 1)) != 0;
    return (v >> n) | (negative && n ? ~0ull << (64 - n) : 0);
}

// lfs/stfs: the FPR holds a double. `(double)` is exact (denormal floats stay normal doubles) and
// `(float)` rounds to nearest even (RPCS3 `LFS`, `STFS`).
DAL_FORCEINLINE double dalReadF32(const uint8_t* base, uint32_t ea) {
    uint32_t bits = dalRead32(base, ea);
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return (double)f;
}

DAL_FORCEINLINE void dalWriteF32(uint8_t* base, uint32_t ea, double value) {
    float f = (float)value;
    uint32_t bits;
    __builtin_memcpy(&bits, &f, sizeof bits);
    dalWrite32(base, ea, bits);
}

// lswi/lswx: `count` bytes into consecutive registers starting at `rd` (wrapping after r31),
// four per register from the high end; a final partial word is zero-filled (RPCS3 `LSWX`).
DAL_FORCEINLINE void dalLoadString(DalPpuContext* ctx, const uint8_t* base, uint32_t ea, uint32_t rd, uint32_t count) {
    for (; count >= 4; count -= 4, ea += 4, rd = (rd + 1) & 31) {
        ctx->gpr[rd] = dalRead32(base, ea);
    }
    if (count) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            value |= (uint32_t)dalRead8(base, ea + i) << ((3 ^ i) * 8);
        }
        ctx->gpr[rd] = value;
    }
}

// stswi/stswx, the mirror of dalLoadString (RPCS3 `STSWX`).
DAL_FORCEINLINE void dalStoreString(const DalPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t rs, uint32_t count) {
    for (; count >= 4; count -= 4, ea += 4, rs = (rs + 1) & 31) {
        dalWrite32(base, ea, (uint32_t)ctx->gpr[rs]);
    }
    if (count) {
        uint32_t value = (uint32_t)ctx->gpr[rs];
        for (uint32_t i = 0; i < count; ++i) {
            dalWrite8(base, ea + i, (uint8_t)(value >> ((3 ^ i) * 8)));
        }
    }
}

// stwcx./stdcx.: CR0 = {lt 0, gt 0, eq = store happened, so = XER.SO} (RPCS3 `STWCX`).
DAL_FORCEINLINE void dalSetCr0Conditional(DalPpuContext* ctx, bool stored) {
    ctx->cr[0] = 0;
    ctx->cr[1] = 0;
    ctx->cr[2] = stored;
    ctx->cr[3] = ctx->xer_so;
}

#pragma once

// Floating-point helpers for generated SPU code, included at the end of spu_ops.h (so it relies on
// the types and rules declared there).
//
// Single precision uses the SPU's extended range (SPU ISA 1.2, 9.1): an exponent field of 0 is zero
// whatever the other bits are, 255 is an ordinary exponent, and the result is truncated toward zero
// with saturation instead of Inf/NaN. Host `double` only supplies exact arithmetic here (every
// extended-range single is a normal double, a product of two is exact, and TwoSum makes a sum
// exact); the rounding is done on the bits, because rounding the double result to nearest first and
// then truncating gives different words. Needs IEEE double arithmetic in the default environment and
// -ffp-contract=off (the codegen option), which keeps the TwoSum statements unfused.
//
// FPSCR is not read or written: the single-precision flags are never set (a recorded deviation).

#ifdef __cplusplus
extern "C" {
#endif

// --- Single precision: exact arithmetic and packing -----------------------------------------------

// The exact value of a single-precision word; denormals (exponent field 0) are zero.
TOX2_FORCEINLINE double tox2SpuSfToDouble(uint32_t w) {
    uint32_t e = (w >> 23) & 0xffu;
    uint64_t bits = 0;
    if (e != 0u) {
        // Rebias from 127 to 1023: e - 127 + 1023.
        bits = ((uint64_t)(w >> 31) << 63) | ((uint64_t)(e + 896u) << 52) | ((uint64_t)(w & 0x7fffffu) << 29);
    }
    double d;
    __builtin_memcpy(&d, &bits, sizeof d);
    return d;
}

// Error-free sum: *hi + *lo is exactly a + b, with *hi the sum rounded to nearest. One operation per
// statement so that no reassociation or contraction can change the error term.
TOX2_FORCEINLINE void tox2SpuTwoSum(double a, double b, double* hi, double* lo) {
    double s = a + b;
    double bb = s - a;
    double t = s - bb;
    double e1 = a - t;
    double e2 = b - bb;
    *hi = s;
    *lo = e1 + e2;
}

// Packs the exact value hi + lo, where hi is a double and lo is zero or the TwoSum error
// (|lo| <= ulp(hi) / 2), as a single-precision word: zero below Smin, saturated above Smax, else
// truncated toward zero to 24 significant bits.
TOX2_FORCEINLINE uint32_t tox2SpuSfFromExact(double hi, double lo) {
    uint64_t h;
    uint64_t l;
    __builtin_memcpy(&h, &hi, sizeof h);
    __builtin_memcpy(&l, &lo, sizeof l);
    uint32_t sign = (uint32_t)(h >> 63) << 31;
    uint32_t exp = (uint32_t)(h >> 52) & 0x7ffu;
    if (exp < 897u) return 0u;  // |hi| < 2^-126, zeros included; every zero result is +0
    if (exp >= 1152u) return sign | 0x7fffffffu;
    uint32_t w = ((exp - 896u) << 23) | (uint32_t)((h >> 29) & 0x7fffffu);
    // hi is exactly a 24-bit value and the remainder pulls the magnitude down: the exact value lies
    // just inside hi. (A double that rounded onto a 24-bit value cannot cross it otherwise.)
    if ((h & 0x1fffffffu) == 0u && lo != 0.0 && (l >> 63) != (h >> 63)) {
        w--;
        if ((w >> 23) == 0u) return 0u;  // fell below Smin
    }
    return sign | w;
}

// --- Single precision: arithmetic -----------------------------------------------------------------

TOX2_FORCEINLINE uint32_t tox2SpuSfAdd(uint32_t a, uint32_t b) {
    double hi;
    double lo;
    tox2SpuTwoSum(tox2SpuSfToDouble(a), tox2SpuSfToDouble(b), &hi, &lo);
    return tox2SpuSfFromExact(hi, lo);
}

TOX2_FORCEINLINE uint32_t tox2SpuSfSub(uint32_t a, uint32_t b) {
    double hi;
    double lo;
    tox2SpuTwoSum(tox2SpuSfToDouble(a), -tox2SpuSfToDouble(b), &hi, &lo);
    return tox2SpuSfFromExact(hi, lo);
}

TOX2_FORCEINLINE uint32_t tox2SpuSfMul(uint32_t a, uint32_t b) {
    return tox2SpuSfFromExact(tox2SpuSfToDouble(a) * tox2SpuSfToDouble(b), 0.0);
}

// a * b + c with an exact, unbounded product (no rounding, saturation or flush of the product).
TOX2_FORCEINLINE uint32_t tox2SpuSfMulAdd(uint32_t a, uint32_t b, uint32_t c) {
    double hi;
    double lo;
    tox2SpuTwoSum(tox2SpuSfToDouble(a) * tox2SpuSfToDouble(b), tox2SpuSfToDouble(c), &hi, &lo);
    return tox2SpuSfFromExact(hi, lo);
}

// a * b - c.
TOX2_FORCEINLINE uint32_t tox2SpuSfMulSub(uint32_t a, uint32_t b, uint32_t c) {
    double hi;
    double lo;
    tox2SpuTwoSum(tox2SpuSfToDouble(a) * tox2SpuSfToDouble(b), -tox2SpuSfToDouble(c), &hi, &lo);
    return tox2SpuSfFromExact(hi, lo);
}

// c - a * b.
TOX2_FORCEINLINE uint32_t tox2SpuSfNegMulSub(uint32_t a, uint32_t b, uint32_t c) {
    double hi;
    double lo;
    tox2SpuTwoSum(-(tox2SpuSfToDouble(a) * tox2SpuSfToDouble(b)), tox2SpuSfToDouble(c), &hi, &lo);
    return tox2SpuSfFromExact(hi, lo);
}

// --- Single precision: register helpers (RT = f(RA, RB[, RC])) --------------------------------------
//
// Host lanes run opposite to the big-endian words, but each lane is independent, so the loops need no
// index mapping.

TOX2_FORCEINLINE Tox2Vr tox2SpuFa(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfAdd(a.u32[lane], b.u32[lane]);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFs(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfSub(a.u32[lane], b.u32[lane]);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFm(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfMul(a.u32[lane], b.u32[lane]);
    return a;
}

// RA * RB + RC.
TOX2_FORCEINLINE Tox2Vr tox2SpuFma(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfMulAdd(a.u32[lane], b.u32[lane], c.u32[lane]);
    return a;
}

// RA * RB - RC.
TOX2_FORCEINLINE Tox2Vr tox2SpuFms(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfMulSub(a.u32[lane], b.u32[lane], c.u32[lane]);
    return a;
}

// RC - RA * RB.
TOX2_FORCEINLINE Tox2Vr tox2SpuFnms(Tox2Vr a, Tox2Vr b, Tox2Vr c) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfNegMulSub(a.u32[lane], b.u32[lane], c.u32[lane]);
    return a;
}

#ifdef __cplusplus
}  // extern "C"
#endif

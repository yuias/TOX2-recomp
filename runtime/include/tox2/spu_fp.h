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

// --- Single precision: compares (ISA 9.1, p.231-234; always extended range) -----------------------
//
// A word with exponent field 0 counts as +0 whatever its sign and fraction. The remaining words
// order like sign-magnitude integers, which is what a key of +-(w & 0x7fffffff) gives.

TOX2_FORCEINLINE uint32_t tox2SpuSfMagnitude(uint32_t w) { return (w & 0x7f800000u) == 0u ? 0u : (w & 0x7fffffffu); }

TOX2_FORCEINLINE int32_t tox2SpuSfKey(uint32_t w) {
    int32_t m = (int32_t)tox2SpuSfMagnitude(w);
    return (w >> 31) != 0u ? -m : m;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFceq(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfKey(a.u32[lane]) == tox2SpuSfKey(b.u32[lane]) ? 0xffffffffu : 0u;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFcmeq(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfMagnitude(a.u32[lane]) == tox2SpuSfMagnitude(b.u32[lane]) ? 0xffffffffu : 0u;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFcgt(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfKey(a.u32[lane]) > tox2SpuSfKey(b.u32[lane]) ? 0xffffffffu : 0u;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFcmgt(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfMagnitude(a.u32[lane]) > tox2SpuSfMagnitude(b.u32[lane]) ? 0xffffffffu : 0u;
    }
    return a;
}

// --- Single precision: estimates (ISA 9.1, p.215-219) ---------------------------------------------
//
// Recorded deviation: the ISA does not publish the base and step tables of the hardware, so the
// estimate is the exact reciprocal (or reciprocal square root) truncated to 24 bits, and `fi` has no
// step left to apply. The ISA's frest / fi / fnms / fma sequence then stays within its 1 ulp bound.

TOX2_FORCEINLINE uint32_t tox2SpuSfRecipEstimate(uint32_t w) {
    uint32_t e = (w >> 23) & 0xffu;
    if (e == 0u) return ((w >> 31) << 31) | 0x7fffffffu;  // 1/0: the largest number, signed
    // |x| >= 2^126: 1/x underflows to zero in hardware, although 1/2^126 would be Smin.
    if (e >= 253u) return 0u;
    return tox2SpuSfFromExact(1.0 / tox2SpuSfToDouble(w), 0.0);
}

TOX2_FORCEINLINE uint32_t tox2SpuSfRsqrtEstimate(uint32_t w) {
    if ((w & 0x7f800000u) == 0u) return 0x7fffffffu;
    return tox2SpuSfFromExact(1.0 / __builtin_elementwise_sqrt(tox2SpuSfToDouble(w & 0x7fffffffu)), 0.0);
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFrest(Tox2Vr a) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfRecipEstimate(a.u32[lane]);
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuFrsqest(Tox2Vr a) {
    for (uint32_t lane = 0; lane < 4; lane++) a.u32[lane] = tox2SpuSfRsqrtEstimate(a.u32[lane]);
    return a;
}

// RB already holds the final estimate (see above); RA, the operand of the estimate, is not needed.
TOX2_FORCEINLINE Tox2Vr tox2SpuFi(Tox2Vr a, Tox2Vr b) {
    (void)a;
    return b;
}

#ifdef __cplusplus
}  // extern "C"
#endif

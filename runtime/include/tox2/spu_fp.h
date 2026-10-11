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
// Double precision (ISA 9.2) is host double arithmetic in round to nearest; the emitted code stops on
// any other FPSCR rounding mode before it calls these helpers.
//
// FPSCR is not read or written: the single-precision and double-precision flags are never set (a
// recorded deviation).

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

// --- Single precision: conversions (ISA 9.1, p.220-223) -------------------------------------------
//
// The caller passes the scale (155 - I8 for csflt/cuflt, 173 - I8 for cflts/cfltu), which the emitter
// has already checked to lie in 0..127; the ISA leaves other scales undefined. Both 2^scale and
// 2^-scale are normal doubles, and a 32-bit integer or an extended-range single times either is exact,
// so the only rounding is the final truncation, and range checks precede every cast.

// 2^n for n in -127..127.
TOX2_FORCEINLINE double tox2SpuPow2(int32_t n) {
    uint64_t bits = (uint64_t)(uint32_t)(n + 1023) << 52;
    double d;
    __builtin_memcpy(&d, &bits, sizeof d);
    return d;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCsflt(Tox2Vr a, uint32_t scale) {
    double factor = tox2SpuPow2(-(int32_t)scale);
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfFromExact((double)a.s32[lane] * factor, 0.0);
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCuflt(Tox2Vr a, uint32_t scale) {
    double factor = tox2SpuPow2(-(int32_t)scale);
    for (uint32_t lane = 0; lane < 4; lane++) {
        a.u32[lane] = tox2SpuSfFromExact((double)a.u32[lane] * factor, 0.0);
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCflts(Tox2Vr a, uint32_t scale) {
    double factor = tox2SpuPow2((int32_t)scale);
    for (uint32_t lane = 0; lane < 4; lane++) {
        double d = tox2SpuSfToDouble(a.u32[lane]) * factor;
        if (d >= 2147483648.0) {
            a.u32[lane] = 0x7fffffffu;
        } else if (d <= -2147483648.0) {
            a.u32[lane] = 0x80000000u;
        } else {
            a.u32[lane] = (uint32_t)(int32_t)d;
        }
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuCfltu(Tox2Vr a, uint32_t scale) {
    double factor = tox2SpuPow2((int32_t)scale);
    for (uint32_t lane = 0; lane < 4; lane++) {
        double d = tox2SpuSfToDouble(a.u32[lane]) * factor;
        if (d < 0.0) {
            a.u32[lane] = 0u;
        } else if (d >= 4294967296.0) {
            a.u32[lane] = 0xffffffffu;
        } else {
            a.u32[lane] = (uint32_t)d;
        }
    }
    return a;
}

// --- Double precision (ISA 9.2, p.197-199, 203-230) -----------------------------------------------
//
// Recorded choices: a denormal operand is forced to zero of the same sign (the optional behaviour of
// p.198-199; denormal results are kept), and every NaN result is the default QNaN, which the ISA
// allows with NaN inputs too (p.197) and which x86 would give with the sign bit set. The doubleword
// lanes of a register are independent, so the loops use the host lanes without an index mapping.

TOX2_FORCEINLINE double tox2SpuDfFromBits(uint64_t bits) {
    if ((bits & 0x7ff0000000000000ull) == 0u) bits &= 0x8000000000000000ull;
    double d;
    __builtin_memcpy(&d, &bits, sizeof d);
    return d;
}

TOX2_FORCEINLINE uint64_t tox2SpuDfToBits(double d) {
    if (__builtin_isnan(d)) return 0x7ff8000000000000ull;
    uint64_t bits;
    __builtin_memcpy(&bits, &d, sizeof bits);
    return bits;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfa(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfToBits(tox2SpuDfFromBits(a.u64[lane]) + tox2SpuDfFromBits(b.u64[lane]));
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfs(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfToBits(tox2SpuDfFromBits(a.u64[lane]) - tox2SpuDfFromBits(b.u64[lane]));
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfm(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfToBits(tox2SpuDfFromBits(a.u64[lane]) * tox2SpuDfFromBits(b.u64[lane]));
    }
    return a;
}

// RA * RB, plus or minus the accumulator RT, with a single rounding. The negated forms negate the
// rounded result; a NaN result is the default QNaN whatever its sign (p.211, 214), which negating
// before the NaN check gives as well.
TOX2_FORCEINLINE uint64_t tox2SpuDfFusedLane(uint64_t a, uint64_t b, uint64_t acc, bool subtract, bool negate) {
    double c = tox2SpuDfFromBits(acc);
    double r = __builtin_fma(tox2SpuDfFromBits(a), tox2SpuDfFromBits(b), subtract ? -c : c);
    return tox2SpuDfToBits(negate ? -r : r);
}

// RA * RB + RT.
TOX2_FORCEINLINE Tox2Vr tox2SpuDfma(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFusedLane(a.u64[lane], b.u64[lane], rt.u64[lane], false, false);
    }
    return a;
}

// RA * RB - RT.
TOX2_FORCEINLINE Tox2Vr tox2SpuDfms(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFusedLane(a.u64[lane], b.u64[lane], rt.u64[lane], true, false);
    }
    return a;
}

// -(RA * RB - RT).
TOX2_FORCEINLINE Tox2Vr tox2SpuDfnms(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFusedLane(a.u64[lane], b.u64[lane], rt.u64[lane], true, true);
    }
    return a;
}

// -(RA * RB + RT).
TOX2_FORCEINLINE Tox2Vr tox2SpuDfnma(Tox2Vr a, Tox2Vr b, Tox2Vr rt) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFusedLane(a.u64[lane], b.u64[lane], rt.u64[lane], false, true);
    }
    return a;
}

// --- Double precision: conversions (ISA 9.2.1, p.198, 224-225) ------------------------------------
//
// Both work on IEEE single words (Table 9-4), where exponent field 255 is Inf or NaN, unlike the
// extended range of the single-precision arithmetic above.

TOX2_FORCEINLINE uint64_t tox2SpuDfFromSingle(uint32_t w) {
    uint64_t sign = (uint64_t)(w >> 31) << 63;
    uint32_t e = (w >> 23) & 0xffu;
    uint32_t f = w & 0x7fffffu;
    if (e == 0u) return sign;  // zero or denormal: a signed zero
    if (e == 0xffu) return f == 0u ? (sign | 0x7ff0000000000000ull) : 0x7ff8000000000000ull;
    // Rebias from 127 to 1023: e - 127 + 1023.
    return sign | ((uint64_t)(e + 896u) << 52) | ((uint64_t)f << 29);
}

// The two single-precision words 0 and 2 become the doublewords; words 1 and 3 are ignored.
TOX2_FORCEINLINE Tox2Vr tox2SpuFesd(Tox2Vr a) {
    Tox2Vr r;
    TOX2_VR_D(r, 0) = tox2SpuDfFromSingle(TOX2_VR_W(a, 0));
    TOX2_VR_D(r, 1) = tox2SpuDfFromSingle(TOX2_VR_W(a, 2));
    return r;
}

// Round to nearest: overflow gives Inf and denormal results are kept; the cast of an out-of-range
// double to float is defined in IEEE arithmetic (clang's undefined-behaviour check does not cover it).
TOX2_FORCEINLINE uint32_t tox2SpuSingleFromDf(uint64_t bits) {
    double d = tox2SpuDfFromBits(bits);
    if (__builtin_isnan(d)) return 0x7fc00000u;
    float f = (float)d;
    uint32_t w;
    __builtin_memcpy(&w, &f, sizeof w);
    return w;
}

// Each doubleword becomes a single in word 0 or 2; words 1 and 3 are zero.
TOX2_FORCEINLINE Tox2Vr tox2SpuFrds(Tox2Vr a) {
    Tox2Vr r = tox2SpuSplatW(0);
    TOX2_VR_W(r, 0) = tox2SpuSingleFromDf(TOX2_VR_D(a, 0));
    TOX2_VR_W(r, 2) = tox2SpuSingleFromDf(TOX2_VR_D(a, 1));
    return r;
}

// --- Double precision: compares (ISA 9.2, p.226-229) ----------------------------------------------
//
// Denormal operands are zero as above; a NaN makes every compare false; zeros are equal and never
// greater whatever their signs. The magnitude forms clear the sign first.

TOX2_FORCEINLINE double tox2SpuDfMagnitude(uint64_t bits) { return tox2SpuDfFromBits(bits & 0x7fffffffffffffffull); }

TOX2_FORCEINLINE Tox2Vr tox2SpuDfceq(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFromBits(a.u64[lane]) == tox2SpuDfFromBits(b.u64[lane]) ? ~0ull : 0ull;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfcmeq(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfMagnitude(a.u64[lane]) == tox2SpuDfMagnitude(b.u64[lane]) ? ~0ull : 0ull;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfcgt(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfFromBits(a.u64[lane]) > tox2SpuDfFromBits(b.u64[lane]) ? ~0ull : 0ull;
    }
    return a;
}

TOX2_FORCEINLINE Tox2Vr tox2SpuDfcmgt(Tox2Vr a, Tox2Vr b) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        a.u64[lane] = tox2SpuDfMagnitude(a.u64[lane]) > tox2SpuDfMagnitude(b.u64[lane]) ? ~0ull : 0ull;
    }
    return a;
}

// --- Double precision: test special value (ISA 9.2, p.230) ----------------------------------------
//
// Tests the raw operand, so a denormal is a denormal here. `mask` is the 7-bit immediate: 0x40 NaN,
// 0x20 +Inf, 0x10 -Inf, 0x08 +0, 0x04 -0, 0x02 positive denormal, 0x01 negative denormal.
TOX2_FORCEINLINE Tox2Vr tox2SpuDftsv(Tox2Vr a, uint32_t mask) {
    for (uint32_t lane = 0; lane < 2; lane++) {
        uint64_t bits = a.u64[lane];
        bool negative = (bits >> 63) != 0u;
        bool fraction = (bits & 0x000fffffffffffffull) != 0u;
        uint32_t e = (uint32_t)(bits >> 52) & 0x7ffu;
        uint32_t cls = 0u;
        if (e == 0x7ffu) {
            cls = fraction ? 0x40u : (negative ? 0x10u : 0x20u);
        } else if (e == 0u) {
            cls = fraction ? (negative ? 0x01u : 0x02u) : (negative ? 0x04u : 0x08u);
        }
        a.u64[lane] = (cls & mask) != 0u ? ~0ull : 0ull;
    }
    return a;
}

#ifdef __cplusplus
}  // extern "C"
#endif

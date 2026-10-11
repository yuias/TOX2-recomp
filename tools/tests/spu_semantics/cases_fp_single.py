"""Single-precision cases: `fa`, `fs`, `fm`, `fma`, `fms`, `fnms`, `fceq`, `fcmeq`, `fcgt`, `fcmgt`, `frest`,
`frsqest`, `fi`.

Every case runs one instruction on registers whose four words differ. Source registers are $3, $4 and
$5, the result goes to $6. The expected words are written out by hand from SPU ISA 1.2, 9.1 (p.195-196)
and the instruction pages (fa p.202, fs p.204, fm p.206, fma p.208, fnms p.210, fms p.212, frest p.215-216,
frsqest p.217-218, fi p.219, fceq p.231, fcmeq p.232, fcgt p.233, fcmgt p.234); none comes from running the
emitter, and this file holds no model of the arithmetic.

Rules used in the derivations (extended-range mode):

- A word with exponent field 0 is zero, whatever its sign and fraction; exponent field 255 is an ordinary
  exponent. Smin = 0x00800000 = 2^-126, Smax = 0x7fffffff = (2^24 - 1) * 2^105.
- The result is the exact value truncated toward zero to 24 significant bits, so `1 - 2^-60` is
  0x3f7fffff (not 1.0, which rounding to nearest would give).
- A result above Smax in magnitude is `sign | 0x7fffffff`; a zero result or one below Smin is +0 (so
  `x - x`, `(-0) + (-0)` and underflow give 0x00000000).
- The product inside fma, fms and fnms is exact and unbounded; only the final value is limited.
- The compares treat a word with exponent field 0 as +0; two zeros are equal and never greater. fceq and
  fcgt order by sign and magnitude, fcmeq and fcmgt by magnitude. True is 0xffffffff, false 0.
- The estimates are the exact reciprocal (square root) truncated to 24 bits, a recorded deviation from
  the hardware tables that the ISA does not publish: frest of a zero exponent is sign | 0x7fffffff, of
  |x| >= 2^126 is +0; frsqest of a zero exponent is 0x7fffffff and its sign is 0. fi returns RB.

Word values used below: 1.0 = 0x3f800000, 2.0 = 0x40000000, 3.0 = 0x40400000, 4.0 = 0x40800000,
6.0 = 0x40c00000, 7.0 = 0x40e00000, 5.0 = 0x40a00000; 0x0d800000 is 2^-100, 0x21800000 is 2^-60.
True = 0xffffffff.
"""

from tests import spu_asm

from . import Case

FP_REF = "SPU ISA 1.2, 9.1 Single Precision (Extended-Range Mode)"
FA_REF = FP_REF + ": fa"
FS_REF = FP_REF + ": fs"
FM_REF = FP_REF + ": fm"
FMA_REF = FP_REF + ": fma"
FMS_REF = FP_REF + ": fms"
FNMS_REF = FP_REF + ": fnms"
FCEQ_REF = FP_REF + ": fceq"
FCMEQ_REF = FP_REF + ": fcmeq"
FCGT_REF = FP_REF + ": fcgt"
FCMGT_REF = FP_REF + ": fcmgt"
FREST_REF = FP_REF + ": frest"
FRSQEST_REF = FP_REF + ": frsqest"
FI_REF = FP_REF + ": fi"
SEQUENCE_REF = FREST_REF + " programming note (p.215)"

T = 0xFFFFFFFF


def rr(name: str, mnemonic: str, ref: str, a, b, out) -> Case:
    """RT = op(RA, RB) with $3, $4 -> $6; operands and result are four words each, word 0 first."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3, RB=4),), {"r3": a, "r4": b}, {"r6": out})


def r(name: str, mnemonic: str, ref: str, a, out) -> Case:
    """RT = op(RA) with $3 -> $6."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3),), {"r3": a}, {"r6": out})


def rrr(name: str, mnemonic: str, ref: str, a, b, c, out) -> Case:
    """RT = op(RA, RB, RC) with $3, $4, $5 -> $6."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT4=6, RA=3, RB=4, RC=5),),
                {"r3": a, "r4": b, "r5": c}, {"r6": out})


CASES: list[Case] = [
    # --- fa ---
    # w0: 1 - 2^-60 truncates to the largest value below 1 (0x3f7fffff). w1: 1 + 2^-60 is 1.0. w2:
    # -1 + 2^-60 is -(1 - 2^-60). w3: (1 + 2^-23) - 1 = 2^-23, exponent field 127 - 23 = 104 = 0x68.
    rr("fa_truncates_toward_zero", "fa", FA_REF,
       (0x3F800000, 0x3F800000, 0xBF800000, 0x3F800001), (0xA1800000, 0x21800000, 0x21800000, 0xBF800000),
       (0x3F7FFFFF, 0x3F800000, 0xBF7FFFFF, 0x34000000)),
    # w0: 2^128 + 2^128 = 2^129 > Smax. w1: the same, negative. w2: 2^128 + 2^127 = 1.5 * 2^128, a valid
    # number (exponent 255, fraction 0x400000). w3: Smax - 2^128 = (2^23 - 1) * 2^105 =
    # (2 - 2^-22) * 2^127, exponent 254, fraction 0x7ffffe.
    rr("fa_saturates_and_keeps_exponent_255", "fa", FA_REF,
       (0x7F800000, 0xFF800000, 0x7F800000, 0x7FFFFFFF), (0x7F800000, 0xFF800000, 0x7F000000, 0xFF800000),
       (0x7FFFFFFF, 0xFFFFFFFF, 0x7FC00000, 0x7F7FFFFE)),
    # w0: (-0) + (-0) is +0. w1: 1 + (-1) is +0. w2: a denormal operand is zero, so 0 + 1.0 = 1.0.
    # w3: two denormals (exponent field 0, non-zero fractions) add to +0.
    rr("fa_zero_results_are_positive_and_denormals_are_zero", "fa", FA_REF,
       (0x80000000, 0x3F800000, 0x00000001, 0x80000001), (0x80000000, 0xBF800000, 0x3F800000, 0x80400000),
       (0x00000000, 0x00000000, 0x3F800000, 0x00000000)),
    # --- fs ---
    # w0: 3 - 1 = 2. w1: (1 + 2^-23) * 2^-126 - 2^-126 = 2^-149 < Smin, so +0. w2: x - x = +0. w3:
    # 1 - 2^-60 truncates to 0x3f7fffff.
    rr("fs_subtracts_rb_from_ra", "fs", FS_REF,
       (0x40400000, 0x00800001, 0x3F800000, 0x3F800000), (0x3F800000, 0x00800000, 0x3F800000, 0x21800000),
       (0x40000000, 0x00000000, 0x00000000, 0x3F7FFFFF)),
    # w0: 2^128 - (-2^128) = 2^129 saturates. w1: -2^128 - 2^128 saturates negative. w2: both denormal,
    # so 0 - 0 = +0. w3: a denormal RA is zero, 0 - 1.0 = -1.0.
    rr("fs_saturates_and_denormals_are_zero", "fs", FS_REF,
       (0x7F800000, 0xFF800000, 0x00000005, 0x00000001), (0xFF800000, 0x7F800000, 0x80000007, 0x3F800000),
       (0x7FFFFFFF, 0xFFFFFFFF, 0x00000000, 0xBF800000)),
    # --- fm ---
    # w0: (1 + 2049 * 2^-23)^2 = 1 + 4098 * 2^-23 + 2049^2 * 2^-46, the last term being half an LSb, so
    # truncation gives fraction 4098 = 0x1002 (rounding to nearest would give 0x1003). w1: 2^128 * 1.0 is
    # a number (exponent 255). w2: 2^-126 * 2^-1 = 2^-127 < Smin, +0. w3: (-1) * (+0) is +0.
    rr("fm_truncates_and_exponent_255_is_a_number", "fm", FM_REF,
       (0x3F800801, 0x7F800000, 0x00800000, 0xBF800000), (0x3F800801, 0x3F800000, 0x3F000000, 0x00000000),
       (0x3F801002, 0x7F800000, 0x00000000, 0x00000000)),
    # w0: 2 * 3 = 6. w1: -2 * 3 = -6. w2: 2^128 * 2^128 = 2^256 saturates. w3: -2^128 * 2^128 saturates
    # negative.
    rr("fm_signs_and_saturation", "fm", FM_REF,
       (0x40000000, 0xC0000000, 0x7F800000, 0xFF800000), (0x40400000, 0x40400000, 0x7F800000, 0x7F800000),
       (0x40C00000, 0xC0C00000, 0x7FFFFFFF, 0xFFFFFFFF)),
    # w0: Smin * 1.0 is exactly Smin, not flushed. w1: Smin * (0.5 - 2^-25) < Smin, +0. w2: a denormal
    # operand is zero. w3: (-0) * (-1) = +0.
    rr("fm_smin_boundary_and_zero_operands", "fm", FM_REF,
       (0x00800000, 0x00800000, 0x00000001, 0x80000000), (0x3F800000, 0x3EFFFFFF, 0x7F7FFFFF, 0xBF800000),
       (0x00800000, 0x00000000, 0x00000000, 0x00000000)),
    # --- fma (RA * RB + RC) ---
    # w0: 2^-200 + 1 = 1 + 2^-200 truncates to 1.0 (the product below Smin is not flushed). w1: a
    # denormal RA is zero, so the result is RC = 1.0. w2: 2 * 3 + 1 = 7. w3: 2^256 + 1 saturates.
    rrr("fma_exact_product", "fma", FMA_REF,
        (0x0D800000, 0x00400000, 0x40000000, 0x7F800000), (0x0D800000, 0x7F7FFFFF, 0x40400000, 0x7F800000),
        (0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000),
        (0x3F800000, 0x3F800000, 0x40E00000, 0x7FFFFFFF)),
    # w0: (1)(-1) + 1 = +0. w1: 1 + (-0) = 1.0. w2: 2^-127 + 2^-126 = 1.5 * 2^-126, the product below Smin
    # still counts (exponent field 1, fraction 0x400000). w3: (2^-100)(-2^-100) + 1 = 1 - 2^-200
    # truncates to 0x3f7fffff.
    rrr("fma_cancellation_and_sign", "fma", FMA_REF,
        (0x3F800000, 0x3F800000, 0x00800000, 0x0D800000), (0xBF800000, 0x3F800000, 0x3F000000, 0x8D800000),
        (0x3F800000, 0x80000000, 0x00800000, 0x3F800000),
        (0x00000000, 0x3F800000, 0x00C00000, 0x3F7FFFFF)),
    # --- fms (RA * RB - RC) ---
    # w0: 2 * 3 - 1 = 5. w1: 2^-200 - 1 = -(1 - 2^-200) truncates to -0x3f7fffff. w2: 2 * 2 - 4 = +0. w3:
    # -2 * 3 - 1 = -7.
    rrr("fms_subtracts_rc_from_the_product", "fms", FMS_REF,
        (0x40000000, 0x0D800000, 0x40000000, 0xC0000000), (0x40400000, 0x0D800000, 0x40000000, 0x40400000),
        (0x3F800000, 0x3F800000, 0x40800000, 0x3F800000),
        (0x40A00000, 0xBF7FFFFF, 0x00000000, 0xC0E00000)),
    # w0: a denormal RC is zero, 1 * 1 - 0 = 1.0. w1: -2^256 - 1 saturates negative. w2: 2^-127 -
    # (-2^-126) = 1.5 * 2^-126. w3: 2 * 3 - 6 = +0.
    rrr("fms_denormal_rc_saturation_and_cancellation", "fms", FMS_REF,
        (0x3F800000, 0xFF800000, 0x00800000, 0x40000000), (0x3F800000, 0x7F800000, 0x3F000000, 0x40400000),
        (0x00000007, 0x3F800000, 0x80800000, 0x40C00000),
        (0x3F800000, 0xFFFFFFFF, 0x00C00000, 0x00000000)),
    # --- fnms (RC - RA * RB) ---
    # w0: 1 - 2^-200 truncates to 0x3f7fffff. w1: 7 - 6 = 1. w2: 1 - 6 = -5. w3: 1 - 2^256 saturates
    # negative.
    rrr("fnms_subtracts_the_product_from_rc", "fnms", FNMS_REF,
        (0x0D800000, 0x40000000, 0x40000000, 0x7F800000), (0x0D800000, 0x40400000, 0x40400000, 0x7F800000),
        (0x3F800000, 0x40E00000, 0x3F800000, 0x3F800000),
        (0x3F7FFFFF, 0x3F800000, 0xC0A00000, 0xFFFFFFFF)),
    # w0: 1 - (2^-100)(-2^-100) = 1 + 2^-200 truncates to 1.0. w1: a denormal RA is zero, 2 - 0 = 2.
    # w2: 1 - 1 = +0. w3: -2^-126 - 2^-127 = -1.5 * 2^-126.
    rrr("fnms_sign_denormal_and_cancellation", "fnms", FNMS_REF,
        (0x0D800000, 0x00000007, 0x3F800000, 0x00800000), (0x8D800000, 0x7F7FFFFF, 0x3F800000, 0x3F000000),
        (0x3F800000, 0x40000000, 0x3F800000, 0x80800000),
        (0x3F800000, 0x40000000, 0x00000000, 0x80C00000)),
    # --- fceq ---
    # w0: +0 and -0 are equal. w1: a denormal is zero. w2: 1.0 and 1.0 + 2^-23 differ. w3: 1.0 and -1.0
    # differ in sign.
    rr("fceq_zeros_are_equal", "fceq", FCEQ_REF,
       (0x00000000, 0x00000001, 0x3F800000, 0x3F800000), (0x80000000, 0x00000000, 0x3F800001, 0xBF800000),
       (T, T, 0, 0)),
    # w0: a negative denormal and a positive one are both zero. w1: equal negative numbers. w2: 2^128 equals
    # itself (exponent 255 is a number). w3: 2^128 and Smax differ.
    rr("fceq_denormals_and_exponent_255", "fceq", FCEQ_REF,
       (0x80400000, 0xBF800000, 0x7F800000, 0x7F800000), (0x00000001, 0xBF800000, 0x7F800000, 0x7FFFFFFF),
       (T, T, T, 0)),
    # --- fcmeq ---
    # w0: |-1.0| = |1.0|. w1: two denormals are zero whatever their signs. w2: 1.0 and 1.0 + 2^-23 differ.
    # w3: |-2^128| = |2^128|.
    rr("fcmeq_compares_magnitudes", "fcmeq", FCMEQ_REF,
       (0xBF800000, 0x00000001, 0x3F800000, 0x7F800000), (0x3F800000, 0x80400000, 0x3F800001, 0xFF800000),
       (T, T, 0, T)),
    # --- fcgt ---
    # w0: 2^128 > the largest number below it, 0x7f7fffff (exponent 255 is a number, not an Inf or a NaN).
    # w1: -0 is not greater than +0. w2: -1.0 > -2.0. w3: +0 is not greater than a negative denormal (zero).
    rr("fcgt_orders_by_sign_and_magnitude", "fcgt", FCGT_REF,
       (0x7F800000, 0x80000000, 0xBF800000, 0x00000000), (0x7F7FFFFF, 0x00000000, 0xC0000000, 0x80000001),
       (T, 0, T, 0)),
    # w0: equal values are not greater. w1: 1.0 > -1.0. w2: -2.0 > -1.0 is false. w3: 1.0 > a denormal
    # (zero).
    rr("fcgt_signs_and_denormals", "fcgt", FCGT_REF,
       (0x3F800000, 0x3F800000, 0xC0000000, 0x3F800000), (0x3F800000, 0xBF800000, 0xBF800000, 0x00400000),
       (0, T, 0, T)),
    # --- fcmgt ---
    # w0: |-2.0| > |1.0|. w1: |1.0| > |-2.0| is false. w2: two zeros are never greater (a negative denormal
    # against +0). w3: |-1.0| > a denormal (zero).
    rr("fcmgt_compares_magnitudes", "fcmgt", FCMGT_REF,
       (0xC0000000, 0x3F800000, 0x80000001, 0xBF800000), (0x3F800000, 0xC0000000, 0x00000000, 0x00400000),
       (T, 0, 0, T)),
    # --- frest ---
    # w0: 1 / 2 = 0.5. w1: 1 / 3 = 0.0101...b truncates to 0x3eaaaaaa (rounding to nearest gives
    # 0x3eaaaaab). w2: 1/0 is the largest number, 0x7fffffff. w3: a denormal is zero, so 1/(-0) is
    # sign | 0x7fffffff.
    r("frest_reciprocal_and_zero", "frest", FREST_REF,
      (0x40000000, 0x40400000, 0x00000000, 0x80400000), (0x3F000000, 0x3EAAAAAA, 0x7FFFFFFF, 0xFFFFFFFF)),
    # w0: |x| = 2^126 is the "Big" range, 0. w1: 1 / -0.25 = -4. w2: 1 / 2^125 = 2^-125, exponent field 2.
    # w3: 1 / 2^-126 = 2^126, exponent field 253.
    r("frest_big_and_range_ends", "frest", FREST_REF,
      (0x7E800000, 0xBE800000, 0x7E000000, 0x00800000), (0x00000000, 0xC0800000, 0x01000000, 0x7E800000)),
    # w0: 2^127 is Big. w1: -2^128 is Big and the result is +0. w2: 1 / (1 + 2^-23) = 1 - 2^-23 + 2^-46 - ...
    # truncates to 1 - 2^-23 = 0xfffffe * 2^-24, 0x3f7ffffe. w3: 1 / -2 = -0.5.
    r("frest_big_sign_and_truncation", "frest", FREST_REF,
      (0x7F000000, 0xFF800000, 0x3F800001, 0xC0000000), (0x00000000, 0x00000000, 0x3F7FFFFE, 0xBF000000)),
    # --- frsqest ---
    # w0: 1 / sqrt(4) = 0.5. w1: the operand is |x|, so -4 gives the same. w2: a zero exponent gives
    # 0x7fffffff. w3: so does a negative denormal (the sign of the result is 0).
    r("frsqest_reciprocal_square_root", "frsqest", FRSQEST_REF,
      (0x40800000, 0xC0800000, 0x00000000, 0x80000001), (0x3F000000, 0x3F000000, 0x7FFFFFFF, 0x7FFFFFFF)),
    # w0: 1 / sqrt(2) = 0.70710678.. = 0xb504f3.33.. * 2^-24 truncates to 0xb504f3, 0x3f3504f3. w1:
    # 1 / sqrt(1) = 1. w2: 1 / sqrt(0.25) = 2. w3: 1 / sqrt(2^128) = 2^-64, exponent field 63.
    r("frsqest_truncates_and_scales", "frsqest", FRSQEST_REF,
      (0x40000000, 0x3F800000, 0x3E800000, 0x7F800000), (0x3F3504F3, 0x3F800000, 0x40000000, 0x1F800000)),
    # w0: 1 / sqrt(2^-126) = 2^63, exponent field 190. w1: 2^-64 for -2^128 as well. w2: 1 / sqrt(9) = 1/3
    # truncates to 0x3eaaaaaa. w3: a positive denormal gives 0x7fffffff.
    r("frsqest_range_ends_and_sign", "frsqest", FRSQEST_REF,
      (0x00800000, 0xFF800000, 0x41100000, 0x00400000), (0x5F000000, 0x1F800000, 0x3EAAAAAA, 0x7FFFFFFF)),
    # --- fi ---
    # fi returns RB whatever RA is (the estimate already is the final value).
    rr("fi_returns_rb", "fi", FI_REF,
       (0x40400000, 0x00000000, 0xFFFFFFFF, 0x3F800000), (0x3EAAAAAA, 0x7FFFFFFF, 0xC0800000, 0x00000001),
       (0x3EAAAAAA, 0x7FFFFFFF, 0xC0800000, 0x00000001)),
    # The ISA's sequence (p.215): y0 = frest x; y1 = fi x, y0; t1 = fnms x, y1, 1.0; y2 = fma t1, y1, y1.
    # w0: x = 3, y0 = y1 = 0xaaaaaa * 2^-25 = 1/3 - 2^-25/3, so 3 * y1 = 1 - 2^-24 and t1 = 2^-24
    # (0x33800000); y2 = y1 * (1 + 2^-24) = 0xaaaaaa.aaaa * 2^-25 truncates to 0x3eaaaaaa (3 * Y < 1 <=
    # 3 * INC(Y)). w1: x = 2, y1 = 0.5, t1 = 0, y2 = 0.5. w2: x = 0, y1 = 0x7fffffff, t1 = 1.0 and
    # y2 = 2 * Smax saturates to 0x7fffffff. w3: x = -3 mirrors w0.
    Case("frest_fi_fnms_fma_sequence", SEQUENCE_REF,
         (spu_asm.encode("frest", RT=6, RA=3), spu_asm.encode("fi", RT=7, RA=3, RB=6),
          spu_asm.encode("fnms", RT4=8, RA=3, RB=7, RC=5), spu_asm.encode("fma", RT4=9, RA=8, RB=7, RC=7)),
         {"r3": (0x40400000, 0x40000000, 0x00000000, 0xC0400000), "r5": (0x3F800000,) * 4},
         {"r7": (0x3EAAAAAA, 0x3F000000, 0x7FFFFFFF, 0xBEAAAAAA),
          "r8": (0x33800000, 0x00000000, 0x3F800000, 0x33800000),
          "r9": (0x3EAAAAAA, 0x3F000000, 0x7FFFFFFF, 0xBEAAAAAA)}),
]

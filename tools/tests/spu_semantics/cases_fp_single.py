"""Single-precision cases: `fa`, `fs`, `fm`, `fma`, `fms`, `fnms`.

Every case runs one instruction on registers whose four words differ. Source registers are $3, $4 and
$5, the result goes to $6. The expected words are written out by hand from SPU ISA 1.2, 9.1 (p.195-196)
and the instruction pages (fa p.202, fs p.204, fm p.206, fma p.208, fnms p.210, fms p.212); none comes
from running the emitter, and this file holds no model of the arithmetic.

Rules used in the derivations (extended-range mode):

- A word with exponent field 0 is zero, whatever its sign and fraction; exponent field 255 is an ordinary
  exponent. Smin = 0x00800000 = 2^-126, Smax = 0x7fffffff = (2^24 - 1) * 2^105.
- The result is the exact value truncated toward zero to 24 significant bits, so `1 - 2^-60` is
  0x3f7fffff (not 1.0, which rounding to nearest would give).
- A result above Smax in magnitude is `sign | 0x7fffffff`; a zero result or one below Smin is +0 (so
  `x - x`, `(-0) + (-0)` and underflow give 0x00000000).
- The product inside fma, fms and fnms is exact and unbounded; only the final value is limited.

Word values used below: 1.0 = 0x3f800000, 2.0 = 0x40000000, 3.0 = 0x40400000, 4.0 = 0x40800000,
6.0 = 0x40c00000, 7.0 = 0x40e00000, 5.0 = 0x40a00000; 0x0d800000 is 2^-100, 0x21800000 is 2^-60.
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


def rr(name: str, mnemonic: str, ref: str, a, b, out) -> Case:
    """RT = op(RA, RB) with $3, $4 -> $6; operands and result are four words each, word 0 first."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3, RB=4),), {"r3": a, "r4": b}, {"r6": out})


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
]

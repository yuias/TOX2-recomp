"""Double-precision cases: `dfa`, `dfs`, `dfm`, `dfma`, `dfms`, `dfnms`, `dfnma`, `fesd`, `frds`, `dfceq`,
`dfcmeq`, `dfcgt`, `dfcmgt`, `dftsv` and the rounding-mode guard.

Every case runs one instruction on registers whose two doublewords differ. Source registers are $3 (RA)
and $4 (RB), the result goes to $6, which also holds RT before dfma, dfms, dfnms and dfnma. The expected
words are written out by hand from SPU ISA 1.2, 9.2 (p.197-199) and the instruction pages (dfa p.203, dfs
p.205, dfm p.207, dfma p.209, dfnms p.211, dfms p.213, dfnma p.214, frds p.224, fesd p.225, dfceq p.226,
dfcmeq p.227, dfcgt p.228, dfcmgt p.229, dftsv p.230); none comes from running the emitter, and this file
holds no model of the arithmetic. `dw` only splits two 64-bit values into the four words of a register.

Rules used in the derivations:

- Each doubleword is an IEEE double in round to nearest, ties to even. A result above Dmax in magnitude is
  Inf. A NaN result is the default QNaN 0x7ff8000000000000, also with NaN inputs (p.197).
- Recorded choices: a denormal operand is forced to zero of the same sign (p.198-199); a denormal result is
  kept. So 0x8000000000000001 times 1.0 is -0, while 0x0010000000000000 * 0.5 = 2^-1023 is the denormal
  0x0008000000000000.
- dfma, dfms, dfnms and dfnma take the product RA * RB exactly and round once. dfnms and dfnma negate the rounded
  result unless it is a NaN (p.211, 214). With x = 1 + 2^-27 (0x3ff0000002000000), x * x = 1 + 2^-26 + 2^-54,
  which rounds to 1 + 2^-26 (0x3ff0000004000000) when it is rounded alone, so x * x - (1 + 2^-26) is 2^-54
  (0x3c90000000000000) only for a fused operation.
- fesd reads words 0 and 2 as IEEE singles (exponent 255 is Inf or NaN, a denormal is a signed zero, a NaN
  is the default double QNaN); frds rounds each doubleword to a single in word 0 or 2 and puts zeros in words
  1 and 3 (overflow is Inf, a NaN is 0x7fc00000, denormal results are kept).
- The compares are false for a NaN, treat zeros as equal and never greater, and flush denormals; the magnitude
  forms compare absolute values. True is all ones in the doubleword. dftsv tests the raw operand against the
  classes enabled in I7: 0x40 NaN, 0x20 +Inf, 0x10 -Inf, 0x08 +0, 0x04 -0, 0x02 +denormal, 0x01 -denormal.
- The rounding operations (dfa dfs dfm dfma dfms dfnms dfnma frds) stop at the instruction when either FPSCR
  rounding field (bits 20-23, 0x00000f00 of word 0) is not 0: the case records `unhandled` (site, word) and
  leaves RT unchanged. fesd, the compares and dftsv do not look at it.

Double values used below: 1.0 = 0x3ff0000000000000, 2.0 = 0x4000000000000000, 3.0 = 0x4008000000000000,
4.0 = 0x4010000000000000, 5.0 = 0x4014000000000000, 6.0 = 0x4018000000000000, 7.0 = 0x401c000000000000,
0.5 = 0x3fe0000000000000; Dmax = 0x7fefffffffffffff. -x sets bit 63 (`NEG`).
"""

from tests import spu_asm

from . import CODE_BASE, Case

FP_REF = "SPU ISA 1.2, 9.2 Double Precision"
DFA_REF = FP_REF + ": dfa"
DFS_REF = FP_REF + ": dfs"
DFM_REF = FP_REF + ": dfm"
DFMA_REF = FP_REF + ": dfma"
DFMS_REF = FP_REF + ": dfms"
DFNMS_REF = FP_REF + ": dfnms"
DFNMA_REF = FP_REF + ": dfnma"
FESD_REF = FP_REF + ": fesd (9.2.1)"
FRDS_REF = FP_REF + ": frds (9.2.1)"
DFCEQ_REF = FP_REF + ": dfceq"
DFCMEQ_REF = FP_REF + ": dfcmeq"
DFCGT_REF = FP_REF + ": dfcgt"
DFCMGT_REF = FP_REF + ": dfcmgt"
DFTSV_REF = FP_REF + ": dftsv"
GUARD_REF = FP_REF + ": FPSCR rounding mode (p.197)"

NEG = 0x8000000000000000
ONE = 0x3FF0000000000000
TWO = 0x4000000000000000
THREE = 0x4008000000000000
FOUR = 0x4010000000000000
FIVE = 0x4014000000000000
SIX = 0x4018000000000000
SEVEN = 0x401C000000000000
HALF = 0x3FE0000000000000
INF = 0x7FF0000000000000
QNAN = 0x7FF8000000000000
SNAN = 0x7FF0000000000001
DMAX = 0x7FEFFFFFFFFFFFFF
X = 0x3FF0000002000000  # 1 + 2^-27
Y = 0x3FF0000004000000  # 1 + 2^-26
TINY = 0x3C90000000000000  # 2^-54
DENORM = 0x0000000000000001
T = 0xFFFFFFFF
TT = (T, T)
FF = (0, 0)
SENTINEL = (0x11111111, 0x22222222, 0x33333333, 0x44444444)


def dw(hi: int, lo: int) -> tuple[int, int, int, int]:
    """The four words of a register from its doublewords 0 (hi) and 1 (lo)."""
    return (hi >> 32, hi & 0xFFFFFFFF, lo >> 32, lo & 0xFFFFFFFF)


def words(*pairs: tuple[int, int]) -> tuple[int, int, int, int]:
    """A register from two (word, word) pairs, for compare results."""
    return (*pairs[0], *pairs[1])


def rr(name: str, mnemonic: str, ref: str, a, b, out) -> Case:
    """RT = op(RA, RB) with $3, $4 -> $6; operands and result are four words each, word 0 first."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3, RB=4),), {"r3": a, "r4": b}, {"r6": out})


def acc(name: str, mnemonic: str, ref: str, a, b, rt, out) -> Case:
    """RT = op(RA, RB, RT) with $3, $4, $6 -> $6."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3, RB=4),), {"r3": a, "r4": b, "r6": rt}, {"r6": out})


def r(name: str, mnemonic: str, ref: str, a, out) -> Case:
    """RT = op(RA) with $3 -> $6."""
    return Case(name, ref, (spu_asm.encode(mnemonic, RT=6, RA=3),), {"r3": a}, {"r6": out})


def tsv(name: str, i7: int, a, out) -> Case:
    """RT = dftsv(RA, I7) with $3 -> $6."""
    return Case(name, DFTSV_REF, (spu_asm.encode("dftsv", RT=6, RA=3, U7=i7),), {"r3": a}, {"r6": out})


def guard(name: str, mnemonic: str, fpscr_word: int) -> Case:
    """The instruction at CODE_BASE stops on the FPSCR rounding mode: RT stays, the stop is recorded."""
    if mnemonic == "frds":
        word = spu_asm.encode(mnemonic, RT=6, RA=3)
    else:
        word = spu_asm.encode(mnemonic, RT=6, RA=3, RB=4)
    return Case(name, GUARD_REF, (word,),
                {"r3": dw(ONE, TWO), "r4": dw(TWO, THREE), "r6": SENTINEL, "fpscr": (fpscr_word, 0, 0, 0)},
                {"r6": SENTINEL, "fpscr": (fpscr_word, 0, 0, 0)}, channels={"unhandled": [(CODE_BASE, word)]})


CASES: list[Case] = [
    # --- dfa ---
    # d0: 1 + 2 = 3. d1: 1 + 1.5 * 2^-53 = 1 + 0.75 ulp (ulp 2^-52) rounds up to 1 + 2^-52.
    rr("dfa_adds_and_rounds_to_nearest", "dfa", DFA_REF,
       dw(ONE, ONE), dw(TWO, 0x3CA8000000000000), dw(THREE, 0x3FF0000000000001)),
    # 1 + 2^-53 and (1 + 2^-52) + 2^-53 are exactly half an ulp above a double: ties to even, so d0 stays
    # 1.0 (even fraction) and d1 goes up to 1 + 2^-51 (0x...02) instead of down to the odd 0x...01.
    rr("dfa_ties_round_to_even", "dfa", DFA_REF,
       dw(ONE, 0x3FF0000000000001), dw(0x3CA0000000000000, 0x3CA0000000000000), dw(ONE, 0x3FF0000000000002)),
    # d0: Inf + (-Inf) is invalid, the default QNaN. d1: a signaling NaN operand also gives the default QNaN.
    rr("dfa_invalid_operations_give_the_default_qnan", "dfa", DFA_REF,
       dw(INF, SNAN), dw(NEG | INF, ONE), dw(QNAN, QNAN)),
    # d0: Dmax + Dmax = 2^1024 - 2^971 is above Dmax + half an ulp, so +Inf. d1: a negative QNaN operand
    # still gives the default (positive) QNaN.
    rr("dfa_overflow_and_negative_nan_input", "dfa", DFA_REF,
       dw(DMAX, 0xFFF8000000000001), dw(DMAX, ONE), dw(INF, QNAN)),
    # d0: two positive denormals are zeros, so the sum is +0 (it would be 0x001ffffffffffffe unforced). d1:
    # two negative denormals are -0 and -0 + -0 is -0.
    rr("dfa_denormal_operands_are_zero", "dfa", DFA_REF,
       dw(0x000FFFFFFFFFFFFF, NEG | DENORM), dw(0x000FFFFFFFFFFFFF, NEG | DENORM), dw(0, NEG)),
    # --- dfs ---
    # d0: 3 - 1 = 2. d1: 1 - 3 = -2.
    rr("dfs_subtracts_rb_from_ra", "dfs", DFS_REF,
       dw(THREE, ONE), dw(ONE, THREE), dw(TWO, NEG | TWO)),
    # d0: Inf - Inf is invalid. d1: both operands are normal (exponent field 1); their difference 2^-1074 is
    # the smallest denormal and is kept.
    rr("dfs_invalid_and_denormal_result", "dfs", DFS_REF,
       dw(INF, 0x0010000000000001), dw(INF, 0x0010000000000000), dw(QNAN, DENORM)),
    # d0: 1.5 - 1.5 is +0 in round to nearest. d1: (-0) - (+0) = (-0) + (-0) = -0.
    rr("dfs_zero_results", "dfs", DFS_REF,
       dw(0x3FF8000000000000, NEG), dw(0x3FF8000000000000, 0), dw(0, NEG)),
    # --- dfm ---
    # d0: a negative denormal operand is -0, and -0 * 1.0 = -0. d1: the smallest normal times 0.5 is 2^-1023,
    # a denormal result (fraction 0x8000000000000), which is kept.
    rr("dfm_denormal_operand_and_denormal_result", "dfm", DFM_REF,
       dw(NEG | DENORM, 0x0010000000000000), dw(ONE, HALF), dw(NEG, 0x0008000000000000)),
    # d0: 2 * 3 = 6. d1: -2 * 3 = -6.
    rr("dfm_signs", "dfm", DFM_REF,
       dw(TWO, NEG | TWO), dw(THREE, THREE), dw(SIX, NEG | SIX)),
    # d0: (1 + u)^2 = 1 + 2u + u^2 with u = 2^-52, rounded to 1 + 2u. d1: 1.5 * (1 + u) = 1.5 + 1.5u, which is
    # halfway between 1.5 + u and 1.5 + 2u; the even fraction is 1.5 + 2u.
    rr("dfm_rounds_to_nearest_even", "dfm", DFM_REF,
       dw(0x3FF0000000000001, 0x3FF8000000000000), dw(0x3FF0000000000001, 0x3FF0000000000001),
       dw(0x3FF0000000000002, 0x3FF8000000000002)),
    # d0: Dmax * 2 overflows to +Inf. d1: Inf * 0 is invalid.
    rr("dfm_overflow_and_invalid", "dfm", DFM_REF,
       dw(DMAX, INF), dw(TWO, 0), dw(INF, QNAN)),
    # --- dfma (RA * RB + RT) ---
    # d0: 2 * 3 + 1 = 7. d1: x * x + (-Y) with the exact product 1 + 2^-26 + 2^-54 is 2^-54.
    acc("dfma_fused_multiply_add", "dfma", DFMA_REF,
        dw(TWO, X), dw(THREE, X), dw(ONE, NEG | Y), dw(SEVEN, TINY)),
    # d0: Inf * 0 + 1 is invalid. d1: (-0) * 1 = -0, and the denormal RT is forced to -0, so -0 + -0 = -0
    # (unforced it would be 0x8000000000000001).
    acc("dfma_invalid_and_denormal_accumulator", "dfma", DFMA_REF,
        dw(INF, NEG), dw(0, ONE), dw(ONE, NEG | DENORM), dw(QNAN, NEG)),
    # d0: 1 * 1 + (-Inf) = -Inf. d1: 2 * 2 + 0.5 = 4.5 (0x4012000000000000).
    acc("dfma_infinity_and_sum", "dfma", DFMA_REF,
        dw(ONE, TWO), dw(ONE, TWO), dw(NEG | INF, HALF), dw(NEG | INF, 0x4012000000000000)),
    # --- dfms (RA * RB - RT) ---
    # d0: 2 * 3 - 1 = 5. d1: x * x - Y = 2^-54.
    acc("dfms_fused_multiply_subtract", "dfms", DFMS_REF,
        dw(TWO, X), dw(THREE, X), dw(ONE, Y), dw(FIVE, TINY)),
    # d0: 2 * 2 - 4 = +0. d1: -2 * 3 - 1 = -7.
    acc("dfms_zero_and_negative_results", "dfms", DFMS_REF,
        dw(TWO, NEG | TWO), dw(TWO, THREE), dw(FOUR, ONE), dw(0, NEG | SEVEN)),
    # d0: Inf * 0 - 1 is invalid. d1: 1 * 1 - Inf = -Inf.
    acc("dfms_invalid_and_infinity", "dfms", DFMS_REF,
        dw(INF, ONE), dw(0, ONE), dw(ONE, INF), dw(QNAN, NEG | INF)),
    # --- dfnms (-(RA * RB - RT)) ---
    # d0: -(2 * 3 - 1) = -5. d1: -(x * x - Y) = -2^-54.
    acc("dfnms_negates_the_fused_result", "dfnms", DFNMS_REF,
        dw(TWO, X), dw(THREE, X), dw(ONE, Y), dw(NEG | FIVE, NEG | TINY)),
    # d0: the multiply-subtract is a NaN (Inf * 0 - 1), and a NaN is not negated: the sign bit stays 0. d1:
    # -(2 * 2 - 4) = -(+0) = -0.
    acc("dfnms_nan_is_not_negated", "dfnms", DFNMS_REF,
        dw(INF, TWO), dw(0, TWO), dw(ONE, FOUR), dw(QNAN, NEG)),
    # --- dfnma (-(RA * RB + RT)) ---
    # d0: -(1 * 1 + 1) = -2. d1: -(x * x + (-Y)) = -2^-54.
    acc("dfnma_negates_the_fused_result", "dfnma", DFNMA_REF,
        dw(ONE, X), dw(ONE, X), dw(ONE, NEG | Y), dw(NEG | TWO, NEG | TINY)),
    # d0: Inf * 0 + 1 is a NaN, not negated. d1: -(-1 * 1 + 1) = -(+0) = -0.
    acc("dfnma_nan_is_not_negated", "dfnma", DFNMA_REF,
        dw(INF, NEG | ONE), dw(0, ONE), dw(ONE, ONE), dw(QNAN, NEG)),
    # --- fesd (words 0 and 2, IEEE single) ---
    # d0: word 0 0x7f800000 is +Inf. d1: word 2 0x3f800000 is 1.0. Words 1 and 3 are ignored.
    r("fesd_infinity_and_one", "fesd", FESD_REF,
      (0x7F800000, 0xDEADBEEF, 0x3F800000, 0x12345678), dw(INF, ONE)),
    # d0: a negative single denormal is -0. d1: 0x7fc00000 is a QNaN, which becomes the default double QNaN.
    r("fesd_denormal_and_qnan", "fesd", FESD_REF,
      (0x80000001, 0xFFFFFFFF, 0x7FC00000, 0xFFFFFFFF), dw(NEG, QNAN)),
    # d0: 0xff800000 is -Inf. d1: 0x7f800001 is a signaling NaN, and any NaN gives the default QNaN.
    r("fesd_negative_infinity_and_snan", "fesd", FESD_REF,
      (0xFF800000, 0, 0x7F800001, 0), dw(NEG | INF, QNAN)),
    # d0: 0xc0490fdb = -(1 + 0x490fdb * 2^-23) * 2^1, exponent 1024 = 0x400, fraction 0x490fdb << 29. d1: the
    # smallest single normal 2^-126 has double exponent 1023 - 126 = 897 = 0x381.
    r("fesd_normal_values", "fesd", FESD_REF,
      (0xC0490FDB, 0, 0x00800000, 0), dw(0xC00921FB60000000, 0x3810000000000000)),
    # --- frds (rounded to nearest into words 0 and 2) ---
    # d0: 1.0 is 0x3f800000. d1: 2^128 is above the largest single, +Inf.
    r("frds_one_and_overflow", "frds", FRDS_REF,
      dw(ONE, 0x47F0000000000000), (0x3F800000, 0, 0x7F800000, 0)),
    # d0: a signaling NaN gives 0x7fc00000. d1: a negative double denormal is -0, 0x80000000.
    r("frds_nan_and_denormal_operand", "frds", FRDS_REF,
      dw(SNAN, NEG | DENORM), (0x7FC00000, 0, 0x80000000, 0)),
    # d0: 1 + 2^-24 + 2^-52 is just above half a single ulp (2^-23), rounds up to 0x3f800001. d1: 1 + 3 * 2^-24
    # is exactly 1.5 single ulps, a tie between 0x3f800001 and 0x3f800002 that goes to the even one.
    r("frds_rounds_to_nearest", "frds", FRDS_REF,
      dw(0x3FF0000010000001, 0x3FF0000030000000), (0x3F800001, 0, 0x3F800002, 0)),
    # d0: 1 + 2^-24 is a tie between 0x3f800000 and 0x3f800001, even is 0x3f800000. d1: the largest single,
    # (2 - 2^-23) * 2^127 (double exponent 1150 = 0x47e), is exact.
    r("frds_tie_to_even_and_largest_single", "frds", FRDS_REF,
      dw(0x3FF0000010000000, 0x47EFFFFFE0000000), (0x3F800000, 0, 0x7F7FFFFF, 0)),
    # d0: (2 - 2^-24) * 2^127 is the midpoint between the largest single and 2^128; the largest single has an
    # odd fraction, so the tie goes up and overflows to +Inf. d1: -Inf stays -Inf.
    r("frds_overflow_tie_and_negative_infinity", "frds", FRDS_REF,
      dw(0x47EFFFFFF0000000, NEG | INF), (0x7F800000, 0, 0xFF800000, 0)),
    # d0: 2^-149 (double exponent 874 = 0x36a) is the smallest single denormal. d1: 2^-127 is the denormal
    # with fraction 0x400000.
    r("frds_denormal_results_are_kept", "frds", FRDS_REF,
      dw(0x36A0000000000000, 0x3800000000000000), (0x00000001, 0, 0x00400000, 0)),
    # d0: 2^-151 is below half the smallest denormal, so +0. d1: -2^-151 gives -0.
    r("frds_underflow_to_zero", "frds", FRDS_REF,
      dw(0x3680000000000000, NEG | 0x3680000000000000), (0, 0, 0x80000000, 0)),
    # --- dfceq ---
    # d0: +0 equals -0. d1: a NaN is not equal even to itself.
    rr("dfceq_zeros_are_equal_and_nan_is_not", "dfceq", DFCEQ_REF,
       dw(0, QNAN), dw(NEG, QNAN), words(TT, FF)),
    # d0: two denormals of different values and signs are both zero. d1: 2.0 and 1.0 differ.
    rr("dfceq_denormals_are_zero", "dfceq", DFCEQ_REF,
       dw(DENORM, TWO), dw(NEG | 2, ONE), words(TT, FF)),
    # d0: +Inf and -Inf differ. d1: 1.0 equals 1.0.
    rr("dfceq_infinities_differ", "dfceq", DFCEQ_REF,
       dw(INF, ONE), dw(NEG | INF, ONE), words(FF, TT)),
    # --- dfcgt ---
    # d0: a NaN is not greater than 1.0. d1: 2.0 > 1.0.
    rr("dfcgt_nan_is_false", "dfcgt", DFCGT_REF,
       dw(QNAN, TWO), dw(ONE, ONE), words(FF, TT)),
    # d0: +0 > -0 is false (zeros are never greater). d1: -1.0 > -2.0.
    rr("dfcgt_zeros_and_negative_values", "dfcgt", DFCGT_REF,
       dw(0, NEG | ONE), dw(NEG, NEG | TWO), words(FF, TT)),
    # d0: +Inf > Dmax. d1: a denormal is zero, and zero is not greater than -0.
    rr("dfcgt_infinity_and_denormal", "dfcgt", DFCGT_REF,
       dw(INF, DENORM), dw(DMAX, NEG), words(TT, FF)),
    # --- dfcmeq ---
    # d0: |-1.0| = |1.0|. d1: 1.0 and 2.0 differ.
    rr("dfcmeq_compares_magnitudes", "dfcmeq", DFCMEQ_REF,
       dw(NEG | ONE, ONE), dw(ONE, TWO), words(TT, FF)),
    # d0: a NaN is not equal to a NaN. d1: -0 and +0 have equal magnitudes.
    rr("dfcmeq_nan_and_zeros", "dfcmeq", DFCMEQ_REF,
       dw(QNAN, NEG), dw(QNAN, 0), words(FF, TT)),
    # --- dfcmgt ---
    # d0: |-2.0| > |1.0|. d1: |1.0| > |-2.0| is false.
    rr("dfcmgt_compares_magnitudes", "dfcmgt", DFCMGT_REF,
       dw(NEG | TWO, ONE), dw(ONE, NEG | TWO), words(TT, FF)),
    # d0: a denormal is zero, and zero is not greater than -0 in magnitude. d1: |-3.0| > |2.0|.
    rr("dfcmgt_denormal_and_zero", "dfcmgt", DFCMGT_REF,
       dw(DENORM, NEG | THREE), dw(NEG, TWO), words(FF, TT)),
    # d0: |-Inf| > Dmax. d1: a NaN is false.
    rr("dfcmgt_infinity_and_nan", "dfcmgt", DFCMGT_REF,
       dw(NEG | INF, QNAN), dw(DMAX, ONE), words(TT, FF)),
    # --- dftsv (I7 enables classes; the operand is tested raw) ---
    # d0: a positive denormal is class 0x02, so I7 0x02 is true. d1: a negative denormal is class 0x01.
    tsv("dftsv_positive_denormal", 0x02, dw(DENORM, NEG | DENORM), words(TT, FF)),
    tsv("dftsv_negative_denormal", 0x01, dw(DENORM, NEG | DENORM), words(FF, TT)),
    # d0: 1.0 is not a NaN. d1: a signaling NaN (exponent all ones, fraction non-zero) is class 0x40.
    tsv("dftsv_nan", 0x40, dw(ONE, SNAN), words(FF, TT)),
    tsv("dftsv_positive_infinity", 0x20, dw(INF, NEG | INF), words(TT, FF)),
    tsv("dftsv_negative_infinity", 0x10, dw(INF, NEG | INF), words(FF, TT)),
    tsv("dftsv_positive_zero", 0x08, dw(0, NEG), words(TT, FF)),
    tsv("dftsv_negative_zero", 0x04, dw(0, NEG), words(FF, TT)),
    # I7 0x7f enables every class: d0 1.0 matches none, d1 a negative denormal matches. I7 0 enables none.
    tsv("dftsv_all_classes", 0x7F, dw(ONE, NEG | DENORM), words(FF, TT)),
    tsv("dftsv_no_class", 0x00, dw(0, QNAN), words(FF, FF)),
    # --- rounding-mode guard (p.197): RN0 is bits 0x00000c00, RN1 bits 0x00000300 of word 0 ---
    guard("dfa_stops_on_rounding_mode_rn0", "dfa", 0x00000400),
    guard("dfs_stops_on_rounding_mode_rn0_high_bit", "dfs", 0x00000800),
    guard("dfm_stops_on_rounding_mode_rn1", "dfm", 0x00000200),
    guard("dfma_stops_on_rounding_mode_rn1_low_bit", "dfma", 0x00000100),
    guard("dfms_stops_on_rounding_mode_rn0_both_bits", "dfms", 0x00000C00),
    guard("dfnms_stops_on_rounding_mode_rn1_both_bits", "dfnms", 0x00000300),
    guard("dfnma_stops_on_every_rounding_bit", "dfnma", 0x00000F00),
    guard("frds_stops_on_rounding_mode", "frds", 0x00000500),
    # Round to nearest (both fields 0) computes: 1 + 2 = 3, 2 + 3 = 5.
    Case("dfa_computes_with_round_to_nearest", GUARD_REF, (spu_asm.encode("dfa", RT=6, RA=3, RB=4),),
         {"r3": dw(ONE, TWO), "r4": dw(TWO, THREE), "r6": SENTINEL, "fpscr": (0, 0, 0, 0)},
         {"r6": dw(THREE, FIVE)}),
    # Every FPSCR bit outside the two rounding fields is set, in word 0 and in the other words; the
    # operation still runs.
    Case("dfa_ignores_other_fpscr_bits", GUARD_REF, (spu_asm.encode("dfa", RT=6, RA=3, RB=4),),
         {"r3": dw(ONE, TWO), "r4": dw(TWO, THREE), "r6": SENTINEL,
          "fpscr": (0xFFFFF0FF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)},
         {"r6": dw(THREE, FIVE)}),
    # frds also runs with a clean FPSCR: 1.0 and 2.0 as singles.
    Case("frds_computes_with_round_to_nearest", GUARD_REF, (spu_asm.encode("frds", RT=6, RA=3),),
         {"r3": dw(ONE, TWO), "r6": SENTINEL, "fpscr": (0, 0, 0, 0)},
         {"r6": (0x3F800000, 0, 0x40000000, 0)}),
    # fesd, the compares and dftsv are exact and run with every rounding bit set. $3 holds the doublewords
    # 0x3f80000000000000 (2^-7) and 0xbf80000000000000 (-2^-7) and, as singles, 1.0 and -1.0; $4 holds 2^-7
    # twice; $5 holds +0 and -0.
    Case("exact_operations_ignore_the_rounding_mode", GUARD_REF,
         (spu_asm.encode("fesd", RT=7, RA=3), spu_asm.encode("dfceq", RT=8, RA=3, RB=4),
          spu_asm.encode("dfcmeq", RT=9, RA=3, RB=4), spu_asm.encode("dfcgt", RT=10, RA=3, RB=4),
          spu_asm.encode("dfcmgt", RT=11, RA=3, RB=4), spu_asm.encode("dftsv", RT=12, RA=5, U7=0x0C)),
         {"r3": (0x3F800000, 0, 0xBF800000, 0), "r4": (0x3F800000, 0, 0x3F800000, 0), "r5": (0, 0, 0x80000000, 0),
          "fpscr": (0x00000F00, 0, 0, 0)},
         {"r7": dw(ONE, NEG | ONE), "r8": words(TT, FF), "r9": words(TT, TT), "r10": words(FF, FF),
          "r11": words(FF, FF), "r12": words(TT, TT)}),
]

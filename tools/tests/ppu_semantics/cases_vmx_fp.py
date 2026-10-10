"""vmx_fp cases: single-precision vector arithmetic, rounding, conversions and compares.

Expected values come from two sources, never from the emitter. The literal cases pin the key edge
cases with values worked out by hand (derivations in the comments). The matrix cases feed boundary
bit patterns (zeros, denormals, FLT_MIN, FLT_MAX, infinities, NaNs) to every mnemonic with Non-Java
mode on and off and compute the expectation lane by lane with an independent Python model: IEEE
arithmetic in double followed by one rounding to float for the single operations, exact `Fraction`
arithmetic with explicit round-to-nearest-even for the fused multiply-adds, and the RPCS3 rules for
denormal flushing and NaN selection written out per mnemonic. Vector words are in big-endian
element order; float lanes are bit patterns.

Two behaviours are inferred rather than read, because `gv_maxfs`/`gv_minfs` live in a header that
is not among the reference sources: for equal operands (including +0/-0) `vmaxfp`/`vminfp` return
the second operand, as x86 `maxps`/`minps` do.
"""

from __future__ import annotations

import itertools
import math
import struct
from fractions import Fraction

from tests import ppc_asm as asm

from . import Case

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

CASES = []

ONES = 0xFFFFFFFF
QNAN = 0x7FC00000
NJ_OFF = {"vscr_nj": 0}


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def op(name: str, a, b, expect, tag: str, c=None, init=None, rc=False, **extra) -> Case:
    """vD = name(vA = v1, vB = v2[, vC = v4]) into v3."""
    fields = {"vC": 4} if c is not None else {}
    if rc:
        fields["Rc_vc"] = 1
    state = {"v1": a, "v2": b, **({"v4": c} if c is not None else {}), **(init or {})}
    return case("%s_%s" % (name, tag), name.upper() + ("_" if rc else ""),
                enc(name, vD=3, vA=1, vB=2, **fields), state, {"v3": expect, **extra})


def unary(name: str, b, expect, tag: str, uimm=None, init=None, **extra) -> Case:
    """vD = name(vB = v2[, UIMM5]) into v3."""
    fields = {"UIMM5": uimm} if uimm is not None else {}
    return case("%s_%s" % (name, tag), name.upper(), enc(name, vD=3, vB=2, **fields),
                {"v2": b, **(init or {})}, {"v3": expect, **extra})


# --- float bit patterns -------------------------------------------------------------------------


def f32_bits(x: float) -> int:
    """Round a Python float to float32 (nearest even) and return its bits; NaN is the default NaN."""
    if x != x:
        return QNAN
    try:
        return struct.unpack("<I", struct.pack("<f", x))[0]
    except OverflowError:  # the value rounds to infinity
        return 0xFF800000 if x < 0 else 0x7F800000


def fl(x: float) -> int:
    return f32_bits(x)


def val(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def is_nan(bits: int) -> bool:
    return (bits & 0x7FFFFFFF) > 0x7F800000


def flush(bits: int, nj: bool) -> int:
    """Non-Java mode: a denormal keeps only its sign."""
    return bits & 0x80000000 if nj and not bits & 0x7F800000 else bits


def pick_nan(result: int, *operands: int) -> int:
    """First NaN operand quieted (payload kept), else the result with a NaN made the default NaN."""
    for operand in operands:
        if is_nan(operand):
            return operand | QNAN
    return QNAN if is_nan(result) else result


def lanes_of(items, width: int = 4):
    """Split into groups of `width`, padding the last group from the start."""
    items = list(items)
    items += items[:(-len(items)) % width]
    return [tuple(items[i:i + width]) for i in range(0, len(items), width)]


# Boundary patterns: zeros, +-1, +-denormal, +-FLT_MIN, FLT_MAX, infinities, a quiet NaN.
VALUES = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x00000001, 0x80000001, 0x00800000,
          0x80800000, 0x7F7FFFFF, 0x7F800000, 0xFF800000, QNAN]


# --- literal cases: add, subtract ---------------------------------------------------------------

CASES += [
    op("vaddfp", (fl(1.0), fl(2.0), fl(-1.5), fl(0.5)), (fl(2.0), fl(0.5), fl(1.5), fl(0.25)),
       (fl(3.0), fl(2.5), 0, fl(0.75)), "basic"),
    # Inputs and results are flushed. w0: 2^-149 + 2^-149 -> 0 + 0. w1: -2^-149 + 0 -> -0 + +0 = +0.
    # w2: (2^-126 + 2^-149) - 2^-126 = 2^-149, a denormal result -> +0. w3: -2^-149 -> -0.
    # Unflushed: 2^-148, -2^-149, 2^-149, -2^-149.
    op("vaddfp", (0x00000001, 0x80000001, 0x00800001, 0x80800001),
       (0x00000001, 0x00000000, 0x80800000, 0x00800000), (0, 0, 0, 0x80000000), "denormals_flushed"),
    op("vaddfp", (0x00000001, 0x80000001, 0x00800001, 0x80800001),
       (0x00000001, 0x00000000, 0x80800000, 0x00800000), (2, 0x80000001, 1, 0x80000001),
       "denormals_kept_without_nj", init=NJ_OFF),
    # w0: both NaN, a wins, signalling NaN quieted. w1: only b is NaN (negative, payload kept).
    # w2: inf + -inf is a NaN from no NaN operand: default NaN. w3: only a is NaN.
    op("vaddfp", (0x7F800001, fl(1.0), 0x7F800000, 0xFFC00005),
       (0x7FC00002, 0xFF800123, 0xFF800000, fl(1.0)), (0x7FC00001, 0xFFC00123, QNAN, 0xFFC00005),
       "nan_propagation"),
    op("vsubfp", (fl(3.0), fl(1.0), fl(0.5), 0x7F800000), (fl(1.0), fl(1.0), fl(1.5), 0x7F800000),
       (fl(2.0), 0, fl(-1.0), QNAN), "basic"),
    # w0: 2^-149 -> 0. w1: 2^-149 - (-0): flushed 0 - -0 = +0. w2: -2^-126 - -(2^-126 + 2^-149) =
    # 2^-149 -> +0. w3: a NaN b operand.
    op("vsubfp", (0x00800001, 0x00000001, 0x80800000, fl(1.0)),
       (0x00800000, 0x80000000, 0x80800001, 0x7F800001), (0, 0, 0, 0x7FC00001), "denormals_flushed"),
    op("vsubfp", (0x00800001, 0x00000001, 0x80800000, fl(1.0)),
       (0x00800000, 0x80000000, 0x80800001, 0x7F800001), (1, 1, 1, 0x7FC00001),
       "denormals_kept_without_nj", init=NJ_OFF),
]

# --- literal cases: fused multiply-add ----------------------------------------------------------

# 1 + 2^-23 and 1 - 2^-23: the exact product is 1 - 2^-46, which is not a float. Unfused it rounds
# to 1.0 and 1.0 + -1.0 gives 0; fused the sum is exactly -2^-46 (exponent field 127 - 46 = 81).
FMA_A, FMA_C = 0x3F800001, 0x3F7FFFFE

CASES += [
    # d = a * c + b with b the addend: w0 = 2*3 + 1 = 7 (a*b + c would give 5), w1 = 1*4 + -4 = +0,
    # w2 = -1*5 + 1 = -4, w3 = 0.5*8 + 0.5 = 4.5.
    op("vmaddfp", (fl(2.0), fl(1.0), fl(-1.0), fl(0.5)), (fl(1.0), fl(-4.0), fl(1.0), fl(0.5)),
       (fl(7.0), 0, fl(-4.0), fl(4.5)), "addend_is_vb", c=(fl(3.0), fl(4.0), fl(5.0), fl(8.0))),
    op("vmaddfp", (FMA_A,) * 4, (fl(-1.0),) * 4, (0xA8800000,) * 4, "single_rounding",
       c=(FMA_C,) * 4),
    # w0: FLT_MIN * 0.5 = 2^-127 denormal -> +0 (kept: 0x00400000). w1: denormal a times 2^100 plus 0:
    # flushed a gives +0; unflushed 2^-149 * 2^100 = 2^-49 = 0x27000000. w2, w3: a NaN or inf * 0
    # operand gives the default NaN, not the operand's payload.
    op("vmaddfp", (0x00800000, 0x00000001, 0xFFC00005, 0x7F800000), (0, 0, fl(1.0), fl(1.0)),
       (0, 0, QNAN, QNAN), "flushed_and_default_nan", c=(fl(0.5), 0x71800000, fl(1.0), 0)),
    op("vmaddfp", (0x00800000, 0x00000001, 0xFFC00005, 0x7F800000), (0, 0, fl(1.0), fl(1.0)),
       (0x00400000, 0x27000000, QNAN, QNAN), "unflushed_without_nj",
       c=(fl(0.5), 0x71800000, fl(1.0), 0), init=NJ_OFF),
    # -(a*c - b): w0 = -(6 - 1), w1 = -(1 - 1) = -0, w2 = -(2 - 5) = 3, w3 = -(-2 - -3) = -1.
    op("vnmsubfp", (fl(2.0), fl(1.0), fl(0.5), fl(-1.0)), (fl(1.0), fl(1.0), fl(5.0), fl(-3.0)),
       (fl(-5.0), 0x80000000, fl(3.0), fl(-1.0)), "basic", c=(fl(3.0), fl(1.0), fl(4.0), fl(2.0))),
    # Fused: -((1 - 2^-46) - 1) = +2^-46 (0x28800000).
    op("vnmsubfp", (FMA_A,) * 4, (fl(1.0),) * 4, (0x28800000,) * 4, "single_rounding",
       c=(FMA_C,) * 4),
    # FLT_MIN * FLT_MIN - FLT_MIN is -FLT_MIN (the tiny product is lost), negated: +FLT_MIN, which is
    # normal and so survives the flush. w1: -(2^-127 - 0) = -2^-127 denormal -> -0 (kept without NJ:
    # 0x80400000). w2: a NaN operand gives the default NaN. w3: -(1 - 1) = -0.
    op("vnmsubfp", (0x00800000, 0x00800000, 0x7FC00007, fl(1.0)), (0x00800000, 0, fl(1.0), fl(1.0)),
       (0x00800000, 0x80000000, QNAN, 0x80000000), "flt_min_and_flush", c=(0x00800000, fl(0.5), fl(1.0), fl(1.0))),
    op("vnmsubfp", (0x00800000,) * 4, (0,) * 4, (0x80400000,) * 4, "denormal_kept_without_nj",
       c=(fl(0.5),) * 4, init=NJ_OFF),
]

# --- literal cases: maximum and minimum ---------------------------------------------------------

CASES += [
    op("vmaxfp", (fl(1.0), fl(-1.0), fl(3.0), 0xFF800000), (fl(2.0), fl(-2.0), fl(3.0), 0),
       (fl(2.0), fl(-1.0), fl(3.0), 0), "basic"),
    op("vminfp", (fl(1.0), fl(-1.0), fl(3.0), 0xFF800000), (fl(2.0), fl(-2.0), fl(3.0), 0),
       (fl(1.0), fl(-2.0), fl(3.0), 0xFF800000), "basic"),
    # Equal zeros return the second operand (inferred x86 maxps/minps behaviour, see the docstring).
    op("vmaxfp", (0x00000000, 0x80000000, 0x00000000, 0x80000000),
       (0x80000000, 0x00000000, 0x00000000, 0x80000000),
       (0x80000000, 0x00000000, 0x00000000, 0x80000000), "equal_zeros_take_b"),
    op("vminfp", (0x00000000, 0x80000000, 0x00000000, 0x80000000),
       (0x80000000, 0x00000000, 0x00000000, 0x80000000),
       (0x80000000, 0x00000000, 0x00000000, 0x80000000), "equal_zeros_take_b"),
    # NaN: w0 b only, w1 both (a first, payload and sign kept), w2 a only, w3 quiet a.
    op("vmaxfp", (fl(1.0), 0xFF800123, 0x7FC00007, 0x7FC00009), (0x7F800001, 0x7FC00002, fl(1.0), 0),
       (0x7FC00001, 0xFFC00123, 0x7FC00007, 0x7FC00009), "nan_propagation"),
    op("vminfp", (fl(1.0), 0xFF800123, 0x7FC00007, 0x7FC00009), (0x7F800001, 0x7FC00002, fl(1.0), 0),
       (0x7FC00001, 0xFFC00123, 0x7FC00007, 0x7FC00009), "nan_propagation"),
    # The result is flushed but the inputs are not: max(2^-149, -0) picks the denormal, which is then
    # flushed to +0; with flushed inputs it would be -0 (equal zeros take b).
    op("vmaxfp", (0x00000001, 0x80000001, 0x00000001, 0x00000002),
       (0x80000000, 0x80000002, 0x00000002, 0x00000001), (0, 0x80000000, 0, 0), "result_flushed"),
    op("vmaxfp", (0x00000001, 0x80000001, 0x00000001, 0x00000002),
       (0x80000000, 0x80000002, 0x00000002, 0x00000001), (1, 0x80000001, 2, 2),
       "denormals_kept_without_nj", init=NJ_OFF),
    # min(-2^-149, +0) is -2^-149, flushed to -0.
    op("vminfp", (0x80000001, 0x00000001, 0x00000001, 0x00000002),
       (0x00000000, 0x80000000, 0x00000002, 0x00000001), (0x80000000, 0x80000000, 0, 0),
       "result_flushed"),
]

# --- literal cases: estimates and transcendental functions --------------------------------------

CASES += [
    unary("vrefp", (fl(1.0), fl(2.0), fl(4.0), fl(-0.5)), (fl(1.0), fl(0.5), fl(0.25), fl(-2.0)),
          "exact"),
    unary("vrefp", (0, 0x80000000, 0x7F800000, 0xFF800123), (0x7F800000, 0xFF800000, 0, 0xFFC00123),
          "zero_inf_nan"),
    # 2^-127 is denormal: flushed to +0 -> +inf (unflushed: 2^127 = 0x7F000000); -2^-127 likewise.
    # 2^127 -> 2^-127, a denormal result: flushed to +0 (kept: 0x00400000).
    unary("vrefp", (0x00400000, 0x80400000, 0x7F000000, 0x00000001),
          (0x7F800000, 0xFF800000, 0, 0x7F800000), "denormals_flushed"),
    unary("vrefp", (0x00400000, 0x80400000, 0x7F000000, 0x00000001),
          (0x7F000000, 0xFF000000, 0x00400000, 0x7F800000), "denormals_kept_without_nj",
          init=NJ_OFF),
    unary("vrsqrtefp", (fl(4.0), fl(1.0), fl(0.25), 0x7F800000), (fl(0.5), fl(1.0), fl(2.0), 0),
          "exact"),
    # +0 -> +inf, -0 -> -inf, negative -> default NaN, NaN operand quieted.
    unary("vrsqrtefp", (0, 0x80000000, fl(-4.0), 0x7F800001), (0x7F800000, 0xFF800000, QNAN, 0x7FC00001),
          "zero_negative_nan"),
    # 2^-128 is denormal: flushed to +0 -> +inf, -2^-128 to -0 -> -inf; unflushed 1 / 2^-64 = 2^64 =
    # 0x5F800000.
    unary("vrsqrtefp", (0x00200000, 0x80200000, 0, 0), (0x7F800000, 0xFF800000, 0x7F800000, 0x7F800000),
          "denormal_flushed"),
    unary("vrsqrtefp", (0x00200000, fl(1.0), 0, 0), (0x5F800000, fl(1.0), 0x7F800000, 0x7F800000),
          "denormal_kept_without_nj", init=NJ_OFF),
    unary("vexptefp", (0, fl(1.0), fl(-1.0), fl(3.0)), (fl(1.0), fl(2.0), fl(0.5), fl(8.0)), "exact"),
    unary("vexptefp", (0x80000000, 0xFF800000, 0x7F800000, 0x7F800001), (fl(1.0), 0, 0x7F800000, QNAN),
          "special"),
    unary("vlogefp", (fl(1.0), fl(8.0), fl(0.25), fl(2.0)), (0, fl(3.0), fl(-2.0), fl(1.0)), "exact"),
    unary("vlogefp", (0, fl(-1.0), 0x7F800000, 0x7F800001), (0xFF800000, QNAN, 0x7F800000, QNAN),
          "special"),
]

# --- literal cases: round to integral -----------------------------------------------------------

CASES += [
    unary("vrfin", (fl(0.5), fl(1.5), fl(2.5), fl(-0.5)), (0, fl(2.0), fl(2.0), 0x80000000),
          "ties_to_even"),
    unary("vrfin", (fl(-1.5), fl(-2.5), 0x4B000001, 0x7F800001), (fl(-2.0), fl(-2.0), 0x4B000001, 0x7FC00001),
          "negative_integral_nan"),
    unary("vrfiz", (fl(1.5), fl(-1.5), fl(0.75), fl(-0.75)), (fl(1.0), fl(-1.0), 0, 0x80000000),
          "toward_zero"),
    unary("vrfip", (fl(1.25), fl(-1.25), fl(0.25), fl(-0.25)), (fl(2.0), fl(-1.0), fl(1.0), 0x80000000),
          "up"),
    unary("vrfim", (fl(1.25), fl(-1.25), fl(0.25), fl(-0.25)), (fl(1.0), fl(-2.0), 0, fl(-1.0)),
          "down"),
    unary("vrfip", (0x7F800000, 0xFF800000, 0x7FC00003, 0x00000000), (0x7F800000, 0xFF800000, 0x7FC00003, 0),
          "inf_nan_zero"),
    # vrfim and vrfip flush the input first: floor(-2^-149) is -0 flushed but -1.0 unflushed; ceil(2^-149)
    # is +0 flushed but 1.0 unflushed.
    unary("vrfim", (0x80000001, 0x00000001, 0x80000000, fl(1.0)), (0x80000000, 0, 0x80000000, fl(1.0)),
          "denormal_input_flushed"),
    unary("vrfim", (0x80000001, 0x00000001, 0x80000000, fl(1.0)), (fl(-1.0), 0, 0x80000000, fl(1.0)),
          "denormal_input_kept_without_nj", init=NJ_OFF),
    unary("vrfip", (0x00000001, 0x80000001, 0, fl(1.0)), (0, 0x80000000, 0, fl(1.0)),
          "denormal_input_flushed"),
    unary("vrfip", (0x00000001, 0x80000001, 0, fl(1.0)), (fl(1.0), 0x80000000, 0, fl(1.0)),
          "denormal_input_kept_without_nj", init=NJ_OFF),
    # Nearest-even and toward-zero give a signed zero for a denormal either way.
    unary("vrfin", (0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF), (0, 0x80000000, 0, 0x80000000),
          "denormals"),
    unary("vrfiz", (0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF), (0, 0x80000000, 0, 0x80000000),
          "denormals"),
    unary("vrfin", (0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF), (0, 0x80000000, 0, 0x80000000),
          "denormals_without_nj", init=NJ_OFF),
]

# --- literal cases: conversions -----------------------------------------------------------------

CASES += [
    unary("vcfsx", (1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000), (fl(1.0), fl(-1.0), 0x4F000000, 0xCF000000),
          "scale_0", uimm=0),
    # 2^-4 is exponent field 123 (0x3D800000); 0x7FFFFFFF rounds to 2^31 first, then 2^27 (0x4D000000).
    unary("vcfsx", (1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000), (0x3D800000, 0xBD800000, 0x4D000000, 0xCD000000),
          "scale_4", uimm=4),
    unary("vcfsx", (1, 0, 0x80000000, 0x40000000), (0x30000000, 0, fl(-1.0), fl(0.5)), "scale_31",
          uimm=31),
    # 2^24 + 1 ties down to 2^24; 2^24 + 3 ties up to the even 2^24 + 4; -(2^24 + 1) -> -2^24.
    unary("vcfsx", (0x01000001, 0x01000003, 0xFEFFFFFF, 0), (0x4B800000, 0x4B800002, 0xCB800000, 0),
          "rounds_to_even", uimm=0),
    unary("vcfux", (1, 0xFFFFFFFF, 0x80000000, 0x01000001), (fl(1.0), 0x4F800000, 0x4F000000, 0x4B800000),
          "scale_0", uimm=0),
    unary("vcfux", (1, 0xFFFFFFFF, 0x80000000, 0x01000001), (fl(0.5), 0x4F000000, 0x4E800000, 0x4B000000),
          "scale_1", uimm=1),
    # Not affected by Non-Java mode: no flush on either side.
    unary("vcfux", (1, 0xFFFFFFFF, 0x80000000, 0x01000001), (fl(0.5), 0x4F000000, 0x4E800000, 0x4B000000),
          "scale_1_without_nj", uimm=1, init=NJ_OFF),
    # w0 1.5 -> 1, w1 -1.5 -> -1, w2 2^31 - 128 is the largest float below the range -> 0x7FFFFF80,
    # w3 -2^31 is exactly representable: no SAT.
    unary("vctsxs", (fl(1.5), fl(-1.5), 0x4EFFFFFF, 0xCF000000), (1, 0xFFFFFFFF, 0x7FFFFF80, 0x80000000),
          "in_range", uimm=0, vscr_sat=0),
    # Saturation: 2^31 and +inf clamp high, -2^31 - 256 and -inf clamp low; one lane is enough for SAT.
    unary("vctsxs", (0x4F000000, fl(1.0), fl(1.0), fl(1.0)), (0x7FFFFFFF, 1, 1, 1), "clamps_high_lane0",
          uimm=0, vscr_sat=1),
    unary("vctsxs", (fl(1.0), 0xCF000001, fl(1.0), fl(1.0)), (1, 0x80000000, 1, 1), "clamps_low_lane1",
          uimm=0, vscr_sat=1),
    unary("vctsxs", (fl(1.0), fl(1.0), 0x7F800000, 0xFF800000), (1, 1, 0x7FFFFFFF, 0x80000000),
          "clamps_infinities", uimm=0, vscr_sat=1),
    # NaN gives 0 and no SAT; neither does 0.5 or -0.5 (truncation to 0).
    unary("vctsxs", (QNAN, fl(0.5), fl(-0.5), 0xFF800123), (0, 0, 0, 0), "nan_is_zero_without_sat",
          uimm=0, vscr_sat=0),
    unary("vctsxs", (fl(1.0),) * 4, (1,) * 4, "sat_stays_set", uimm=0, init={"vscr_sat": 1}, vscr_sat=1),
    # Scale 2^3: 1.5 -> 12, -0.25 -> -2, 0.125 -> 1, 2^28 * 8 = 2^31 clamps high.
    unary("vctsxs", (fl(1.5), fl(-0.25), fl(0.125), 0x4D800000), (12, 0xFFFFFFFE, 1, 0x7FFFFFFF),
          "scale_3", uimm=3, vscr_sat=1),
    # Scale 2^31: 0.5 -> 2^30, -1.0 -> -2^31 exactly, -0.5 -> -2^30, 0 -> 0: no SAT.
    unary("vctsxs", (fl(0.5), fl(-1.0), fl(-0.5), 0), (0x40000000, 0x80000000, 0xC0000000, 0),
          "scale_31", uimm=31, vscr_sat=0),
    unary("vctsxs", (fl(1.0), 0, 0, 0), (0x7FFFFFFF, 0, 0, 0), "scale_31_clamps", uimm=31, vscr_sat=1),
    # vctuxs: 2^32 - 256 is the largest float below the range.
    unary("vctuxs", (fl(1.5), fl(0.5), 0x4F7FFFFF, 0x80000000), (1, 0, 0xFFFFFF00, 0), "in_range",
          uimm=0, vscr_sat=0),
    unary("vctuxs", (0x4F800000, fl(1.0), fl(1.0), fl(1.0)), (0xFFFFFFFF, 1, 1, 1), "clamps_high",
          uimm=0, vscr_sat=1),
    unary("vctuxs", (fl(1.0), fl(1.0), 0x7F800000, fl(1.0)), (1, 1, 0xFFFFFFFF, 1), "clamps_infinity",
          uimm=0, vscr_sat=1),
    unary("vctuxs", (fl(-1.0), fl(1.0), fl(1.0), fl(1.0)), (0, 1, 1, 1), "clamps_negative", uimm=0,
          vscr_sat=1),
    # RPCS3 `VCTUXS` flags every scaled value below 0, so -0.5 sets SAT although it truncates to 0.
    unary("vctuxs", (fl(-0.5), fl(1.0), fl(1.0), fl(1.0)), (0, 1, 1, 1), "small_negative_sets_sat",
          uimm=0, vscr_sat=1),
    unary("vctuxs", (QNAN, 0xFF800123, fl(0.5), 0), (0, 0, 0, 0), "nan_is_zero_without_sat", uimm=0,
          vscr_sat=0),
    # Scale 2^4: 1.0 -> 16, 0.0625 -> 1, 2^28 * 16 = 2^32 clamps.
    unary("vctuxs", (fl(1.0), fl(0.0625), 0x4D800000, 0), (16, 1, 0xFFFFFFFF, 0), "scale_4", uimm=4,
          vscr_sat=1),
]

# --- literal cases: compares --------------------------------------------------------------------

CR6_ALL, CR6_NONE, CR6_MIXED = 8, 2, 0

CASES += [
    # +0 equals -0; NaN equals nothing, not even itself.
    op("vcmpeqfp", (fl(1.0), fl(2.0), QNAN, 0), (fl(1.0), fl(3.0), QNAN, 0x80000000),
       (ONES, 0, 0, ONES), "basic"),
    op("vcmpgefp", (fl(2.0), fl(1.0), fl(1.0), QNAN), (fl(1.0), fl(1.0), fl(2.0), fl(1.0)),
       (ONES, ONES, 0, 0), "basic"),
    op("vcmpgtfp", (fl(2.0), fl(1.0), fl(1.0), QNAN), (fl(1.0), fl(1.0), fl(2.0), fl(1.0)),
       (ONES, 0, 0, 0), "basic"),
    op("vcmpgtfp", (fl(1.0),) * 4, (fl(0.5),) * 4, (ONES,) * 4, "without_rc_keeps_cr6", init={"cr6": 5},
       cr6=5),
]
for _name in ("vcmpeqfp", "vcmpgefp", "vcmpgtfp"):
    _true = ((fl(2.0),) * 4, (fl(1.0),) * 4) if _name != "vcmpeqfp" else ((fl(2.0),) * 4,) * 2
    _false = ((fl(1.0),) * 4, (fl(2.0),) * 4) if _name != "vcmpeqfp" else ((fl(1.0),) * 4, (fl(2.0),) * 4)
    CASES.append(op(_name, *_true, (ONES,) * 4, "rc_all_true", init={"cr6": 5}, rc=True, cr6=CR6_ALL))
    CASES.append(op(_name, *_false, (0,) * 4, "rc_none_true", init={"cr6": 5}, rc=True, cr6=CR6_NONE))
    _mixed_a = (fl(2.0), fl(1.0), fl(2.0), fl(1.0))
    _mixed_b = (fl(2.0), fl(2.0), fl(1.0), fl(1.0)) if _name == "vcmpeqfp" else (fl(1.0), fl(2.0), fl(1.0), fl(2.0))
    _mixed = {"vcmpeqfp": (ONES, 0, 0, ONES), "vcmpgefp": (ONES, 0, ONES, 0),
              "vcmpgtfp": (ONES, 0, ONES, 0)}[_name]
    CASES.append(op(_name, _mixed_a, _mixed_b, _mixed, "rc_mixed", init={"cr6": 5}, rc=True,
                    cr6=CR6_MIXED))

CASES += [
    # Bit 0 (0x80000000): a > b or unordered. Bit 1 (0x40000000): a < -b or unordered.
    # w0 1 vs 2 in bounds; w1 3 > 2; w2 -3 < -2; w3 NaN is both.
    op("vcmpbfp", (fl(1.0), fl(3.0), fl(-3.0), QNAN), (fl(2.0),) * 4, (0, 0x80000000, 0x40000000, 0xC0000000),
       "bounds"),
    # b negative: 0 > -1 and 0 < 1, so both bits; the bounds are inclusive (2 vs 2, -2 vs 2, 0 vs 0).
    op("vcmpbfp", (0, fl(2.0), fl(-2.0), 0), (fl(-1.0), fl(2.0), fl(2.0), 0), (0xC0000000, 0, 0, 0),
       "inclusive_and_negative_bound"),
    op("vcmpbfp", (fl(1.0), fl(-1.0), 0, fl(2.0)), (fl(2.0), fl(1.0), 0, fl(2.0)), (0,) * 4,
       "rc_all_in_bounds", init={"cr6": 5}, rc=True, cr6=CR6_NONE),
    op("vcmpbfp", (fl(1.0), fl(3.0), 0, fl(2.0)), (fl(2.0), fl(2.0), 0, fl(2.0)),
       (0, 0x80000000, 0, 0), "rc_one_out_of_bounds", init={"cr6": 5}, rc=True, cr6=CR6_MIXED),
    op("vcmpbfp", (fl(1.0), fl(3.0), 0, fl(2.0)), (fl(2.0), fl(2.0), 0, fl(2.0)),
       (0, 0x80000000, 0, 0), "without_rc_keeps_cr6", init={"cr6": 5}, cr6=5),
]


# --- matrix cases: independent Python model -----------------------------------------------------


def model_add(a: int, b: int, nj: bool, sign: int = 1) -> int:
    a, b = flush(a, nj), flush(b, nj)
    return flush(pick_nan(f32_bits(val(a) + sign * val(b)), a, b), nj)


def model_pick(a: int, b: int, nj: bool, take_a) -> int:
    chosen = a if take_a(val(a), val(b)) else b  # an unordered or equal compare takes b
    return flush(pick_nan(chosen, a, b), nj)


def recip(x: float) -> float:
    return math.copysign(math.inf, x) if x == 0 else 1.0 / x


def model_recip(b: int, nj: bool) -> int:
    b = flush(b, nj)
    return flush(pick_nan(f32_bits(recip(val(b))), b), nj)


def model_rsqrt(b: int, nj: bool) -> int:
    b = flush(b, nj)
    x = val(b)
    if is_nan(b) or x < 0:
        root = math.nan
    else:
        root = val(f32_bits(math.sqrt(x)))
    return flush(pick_nan(f32_bits(recip(root)) if root == root else QNAN, b), nj)


def model_round(kind: str, bits: int, nj: bool) -> int:
    if kind in ("p", "m"):  # only these two handlers flush their input
        bits = flush(bits, nj)
    x = val(bits)
    if is_nan(bits) or math.isinf(x):
        return flush(pick_nan(bits, bits), nj)
    n = {"n": round, "z": math.trunc, "p": math.ceil, "m": math.floor}[kind](x)
    result = f32_bits(math.copysign(0.0, x) if n == 0 else float(n))
    return flush(pick_nan(result, bits), nj)


def round_fraction(q: Fraction) -> int:
    """Nearest-even float32 bits of a nonzero rational."""
    m = abs(q)
    e = m.numerator.bit_length() - m.denominator.bit_length()
    while Fraction(2) ** e > m:
        e -= 1
    while Fraction(2) ** (e + 1) <= m:
        e += 1
    quantum = Fraction(2) ** (max(e, -126) - 23)
    n = m / quantum
    whole = n.numerator // n.denominator
    rest = n - whole
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and whole % 2):
        whole += 1
    rounded = whole * quantum
    bits = 0x7F800000 if rounded >= Fraction(2) ** 128 else f32_bits(float(rounded))
    return bits | (0x80000000 if q < 0 else 0)


def fma_bits(a: int, c: int, b: int) -> int:
    """a * c + b with one rounding, on operands that are not NaN."""
    xa, xc, xb = val(a), val(c), val(b)
    if math.isinf(xa) or math.isinf(xc) or math.isinf(xb):
        return f32_bits(xa * xc + xb)
    product = Fraction(xa) * Fraction(xc)
    total = product + Fraction(xb)
    if total == 0:
        both_negative = product == 0 and xb == 0 and ((a >> 31) ^ (c >> 31)) == 1 and b >> 31 == 1
        return 0x80000000 if both_negative else 0
    return round_fraction(total)


def model_madd(a: int, b: int, c: int, nj: bool, negate: bool) -> int:
    a, b, c = flush(a, nj), flush(b, nj), flush(c, nj)
    if is_nan(a) or is_nan(b) or is_nan(c):
        return QNAN
    if negate:
        r = fma_bits(a, c, b ^ 0x80000000)
        r = QNAN if is_nan(r) else r ^ 0x80000000
    else:
        r = fma_bits(a, c, b)
    return flush(r, nj)


def model_scale(bits: int, uimm: int) -> float:
    return val(f32_bits(val(bits) * 2.0 ** uimm))


def model_convert_to_int(bits: int, uimm: int, signed: bool) -> tuple[int, int]:
    if is_nan(bits):
        return 0, 0
    scaled = model_scale(bits, uimm)
    lo, hi = (-2.0 ** 31, 2.0 ** 31) if signed else (0.0, 2.0 ** 32)
    if scaled < lo:
        return (0x80000000 if signed else 0), 1
    if scaled >= hi:
        return (0x7FFFFFFF if signed else 0xFFFFFFFF), 1
    return int(scaled) & 0xFFFFFFFF, 0


def model_bounds(a: int, b: int) -> int:
    x, y = val(a), val(b)
    above = not x <= y
    below = not x >= -y
    return (0x80000000 if above else 0) | (0x40000000 if below else 0)


def tag_of(nj: bool) -> str:
    return "" if nj else "nj0_"


def init_of(nj: bool) -> dict:
    return {} if nj else dict(NJ_OFF)


PAIR_VECTORS = [(tuple(p[0] for p in lanes), tuple(p[1] for p in lanes))
                for lanes in lanes_of(itertools.product(VALUES, repeat=2))]

for _nj in (True, False):
    for _index, (_a, _b) in enumerate(PAIR_VECTORS):
        _tag = "%smatrix_%d" % (tag_of(_nj), _index)
        _init = init_of(_nj)
        CASES.append(op("vaddfp", _a, _b, tuple(model_add(x, y, _nj) for x, y in zip(_a, _b)), _tag,
                        init=_init))
        CASES.append(op("vsubfp", _a, _b, tuple(model_add(x, y, _nj, -1) for x, y in zip(_a, _b)), _tag,
                        init=_init))
        CASES.append(op("vmaxfp", _a, _b, tuple(model_pick(x, y, _nj, lambda p, q: p > q)
                                                for x, y in zip(_a, _b)), _tag, init=_init))
        CASES.append(op("vminfp", _a, _b, tuple(model_pick(x, y, _nj, lambda p, q: p < q)
                                                for x, y in zip(_a, _b)), _tag, init=_init))

# Fused multiply-add: a deterministic sample of triples, including the 1 +- 2^-23 pair and a large
# power of two so that products land in the denormal range.
FMA_VALUES = VALUES + [FMA_A, FMA_C, 0x40000000, 0x71800000, 0x3F000000]
FMA_TRIPLES = list(itertools.product(FMA_VALUES, repeat=3))[::37]
for _nj in (True, False):
    for _index, _lanes in enumerate(lanes_of(FMA_TRIPLES)):
        _a, _b, _c = (tuple(t[k] for t in _lanes) for k in range(3))
        _tag = "%smatrix_%d" % (tag_of(_nj), _index)
        for _name, _negate in (("vmaddfp", False), ("vnmsubfp", True)):
            CASES.append(op(_name, _a, _b, tuple(model_madd(x, y, z, _nj, _negate)
                                                 for x, y, z in zip(_a, _b, _c)), _tag, c=_c,
                            init=init_of(_nj)))

# One-operand operations.
for _nj in (True, False):
    for _index, _lanes in enumerate(lanes_of(VALUES)):
        _tag = "%smatrix_%d" % (tag_of(_nj), _index)
        CASES.append(unary("vrefp", _lanes, tuple(model_recip(x, _nj) for x in _lanes), _tag,
                           init=init_of(_nj)))
        CASES.append(unary("vrsqrtefp", _lanes, tuple(model_rsqrt(x, _nj) for x in _lanes), _tag,
                           init=init_of(_nj)))
        for _kind in "nzpm":
            CASES.append(unary("vrfi" + _kind, _lanes, tuple(model_round(_kind, x, _nj) for x in _lanes),
                               _tag, init=init_of(_nj)))

# Conversions to float and back, over integer and float boundary values and several scales.
INTS = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0x01000001, 0x00FFFFFF, 0xFFFFFF00,
        0x12345678, 0xEDCBA988, 0x40000000]
FLOATS = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0xBF000000, 0x4EFFFFFF,
          0x4F000000, 0xCF000000, 0x4F7FFFFF, 0x4F800000, 0x7F800000, 0xFF800000, QNAN, 0x00000001,
          0x7F7FFFFF]
for _uimm in (0, 1, 5, 31):
    for _index, _lanes in enumerate(lanes_of(INTS)):
        _tag = "matrix_u%d_%d" % (_uimm, _index)
        CASES.append(unary("vcfsx", _lanes, tuple(
            f32_bits(float(x - (1 << 32) if x >> 31 else x) / 2.0 ** _uimm) for x in _lanes), _tag,
            uimm=_uimm))
        CASES.append(unary("vcfux", _lanes, tuple(f32_bits(float(x) / 2.0 ** _uimm) for x in _lanes),
                           _tag, uimm=_uimm))
    for _index, _lanes in enumerate(lanes_of(FLOATS)):
        _tag = "matrix_u%d_%d" % (_uimm, _index)
        for _name, _signed in (("vctsxs", True), ("vctuxs", False)):
            _results = [model_convert_to_int(x, _uimm, _signed) for x in _lanes]
            CASES.append(unary(_name, _lanes, tuple(r[0] for r in _results), _tag, uimm=_uimm,
                               vscr_sat=int(any(r[1] for r in _results))))

# Compares, with and without CR6.
for _index, (_a, _b) in enumerate(PAIR_VECTORS):
    _tag = "matrix_%d" % _index
    for _name, _test in (("vcmpeqfp", lambda p, q: p == q), ("vcmpgefp", lambda p, q: p >= q),
                         ("vcmpgtfp", lambda p, q: p > q)):
        _result = tuple(ONES if _test(val(x), val(y)) else 0 for x, y in zip(_a, _b))
        _cr6 = CR6_ALL if all(_result) else CR6_NONE if not any(_result) else CR6_MIXED
        CASES.append(op(_name, _a, _b, _result, _tag, init={"cr6": 5}, rc=True, cr6=_cr6))
    _result = tuple(model_bounds(x, y) for x, y in zip(_a, _b))
    CASES.append(op("vcmpbfp", _a, _b, _result, _tag, init={"cr6": 5}, rc=True,
                    cr6=CR6_NONE if not any(_result) else CR6_MIXED))
    CASES.append(op("vcmpbfp", _a, _b, _result, "plain_" + _tag, init={"cr6": 5}, cr6=5))

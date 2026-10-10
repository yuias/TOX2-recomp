"""fp cases: scalar arithmetic, single-precision rounding, fused multiply-add, conversions, compares.

Expected values are derived by hand from the architecture and the cited RPCS3 handlers; FPR values
are given as floats where exact and as raw bit patterns otherwise. Inputs sit in f1, f2 (and f3 for
the three-operand forms), results in f4 unless a case says otherwise.
"""

from tests import ppc_asm as asm

from . import Case

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

QNAN = 0x7FF8000000000000
POS_INF = 0x7FF0000000000000
NEG_INF = 0xFFF0000000000000
NEG_ZERO = 0x8000000000000000
INT64_MAX = 0x7FFFFFFFFFFFFFFF
INT64_MIN = 0x8000000000000000
# A 32-bit result is stored sign-extended.
INT32_MAX = 0x000000007FFFFFFF
INT32_MIN = 0xFFFFFFFF80000000
TWO_POW_31 = 2.0 ** 31
TWO_POW_63 = 2.0 ** 63
# 1 + 2^-12 and its square rounded to float (1 + 2^-11 + 2^-24 is a tie that rounds to even).
ONE_PLUS_2_12 = 0x3FF0010000000000
ONE_PLUS_2_11 = 0x3FF0020000000000
# a = 1 + 2^-27, c = 1 - 2^-27: a * c = 1 - 2^-54 needs the fused operation to survive.
FUSED_A = 1.0 + 2.0 ** -27
FUSED_C = 1.0 - 2.0 ** -27
TWO_POW_M54 = 0x3C90000000000000
NEG_TWO_POW_M54 = 0xBC90000000000000


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def unary(name: str, mnemonic: str, value, result, **flags) -> Case:
    """`<mnemonic> f4, f1` with f1 = `value`."""
    return case(name, mnemonic.upper(), enc(mnemonic, frD=4, frB=1, **flags), {"f1": value},
                {"f4": result})


def binary(name: str, mnemonic: str, a, b, result) -> Case:
    """`<mnemonic> f4, f1, f2` (frC instead of frB for the multiplies)."""
    field = "frC" if mnemonic.startswith("fmul") else "frB"
    return case(name, mnemonic.upper(), enc(mnemonic, frD=4, frA=1, **{field: 2}),
                {"f1": a, "f2": b}, {"f4": result})


def fused(name: str, mnemonic: str, a, c, b, result) -> Case:
    """`<mnemonic> f4, f1, f2(c), f3(b)`: a * c + b."""
    return case(name, mnemonic.upper(), enc(mnemonic, frD=4, frA=1, frC=2, frB=3),
                {"f1": a, "f2": c, "f3": b}, {"f4": result})


CASES = []

# --- double-precision arithmetic --------------------------------------------------------------

CASES += [
    binary("fadd_adds", "fadd", 1.5, 2.25, 3.75),
    # 1 + 2^-53 is a tie that rounds to even; 1 + 2^-52 is representable.
    binary("fadd_tie_rounds_to_even", "fadd", 1.0, 2.0 ** -53, 1.0),
    binary("fadd_smallest_step", "fadd", 1.0, 2.0 ** -52, 0x3FF0000000000001),
    case("fadd_destination_aliases_source", "FADD", enc("fadd", frD=1, frA=1, frB=2),
         {"f1": 1.5, "f2": 2.25}, {"f1": 3.75}),
    binary("fsub_subtracts_b_from_a", "fsub", 1.0, 2.5, -1.5),
    binary("fmul_uses_frc", "fmul", 1.5, 4.0, 6.0),
    # (1 + 2^-12)^2 = 1 + 2^-11 + 2^-24, exact in double.
    binary("fmul_keeps_double_precision", "fmul", ONE_PLUS_2_12, ONE_PLUS_2_12, 0x3FF0020010000000),
    binary("fdiv_divides_a_by_b", "fdiv", 1.0, 4.0, 0.25),
    binary("fdiv_by_zero_is_infinity", "fdiv", 1.0, 0.0, POS_INF),
    binary("fdiv_negative_by_negative_zero", "fdiv", 1.0, NEG_ZERO, NEG_INF),
    unary("fsqrt_exact", "fsqrt", 2.25, 1.5),
]

# --- single-precision arithmetic: the double result is rounded to float -----------------------

TIE_UP = 2.0 ** -24 + 2.0 ** -40       # above the half-ulp of a float at 1.0
FLOAT_ONE_PLUS_ULP = 0x3FF0000020000000

CASES += [
    binary("fadds_adds", "fadds", 1.5, 2.25, 3.75),
    binary("fadds_tie_rounds_to_even", "fadds", 1.0, 2.0 ** -24, 1.0),
    binary("fadds_rounds_up_above_the_tie", "fadds", 1.0, TIE_UP, FLOAT_ONE_PLUS_ULP),
    binary("fadds_overflow_gives_infinity", "fadds", 3.0e38, 3.0e38, POS_INF),
    binary("fsubs_subtracts_b_from_a", "fsubs", 1.0, -TIE_UP, FLOAT_ONE_PLUS_ULP),
    binary("fmuls_uses_frc", "fmuls", 1.5, 4.0, 6.0),
    binary("fmuls_tie_rounds_to_even", "fmuls", ONE_PLUS_2_12, ONE_PLUS_2_12, ONE_PLUS_2_11),
    binary("fdivs_rounds_to_float", "fdivs", 1.0, 3.0, 0x3FD5555560000000),
    unary("fsqrts_rounds_to_float", "fsqrts", 2.0, 0x3FF6A09E60000000),
    unary("frsp_rounds_to_float", "frsp", 0x3FF0000030000000, 0x3FF0000040000000),
    unary("frsp_tie_rounds_to_even", "frsp", 0x3FF0000010000000, 1.0),
    unary("frsp_overflow_gives_infinity", "frsp", 1.0e300, POS_INF),
    unary("frsp_negative_overflow_gives_negative_infinity", "frsp", -1.0e300, NEG_INF),
    unary("frsp_tie_below_smallest_denormal_gives_zero", "frsp", 0x3690000000000000, 0),
]

# --- estimates --------------------------------------------------------------------------------

CASES += [
    unary("fres_of_four", "fres", 4.0, 0.25),
    unary("frsqrte_of_four", "frsqrte", 4.0, 0.5),
]

# --- multiply-add family ----------------------------------------------------------------------

CASES += [
    fused("fmadd_adds_the_product", "fmadd", 2.0, 3.0, 1.0, 7.0),
    fused("fmsub_subtracts_b", "fmsub", 2.0, 3.0, 1.0, 5.0),
    fused("fnmadd_negates_the_sum", "fnmadd", 2.0, 3.0, 1.0, -7.0),
    fused("fnmsub_negates_the_difference", "fnmsub", 2.0, 3.0, 1.0, -5.0),
    fused("fmadd_is_fused", "fmadd", FUSED_A, FUSED_C, -1.0, NEG_TWO_POW_M54),
    fused("fmsub_is_fused", "fmsub", FUSED_A, FUSED_C, 1.0, NEG_TWO_POW_M54),
    fused("fnmadd_is_fused", "fnmadd", FUSED_A, FUSED_C, -1.0, TWO_POW_M54),
    fused("fnmsub_is_fused", "fnmsub", FUSED_A, FUSED_C, 1.0, TWO_POW_M54),
    case("fmadd_destination_aliases_addend", "FMADD", enc("fmadd", frD=3, frA=1, frC=2, frB=3),
         {"f1": 2.0, "f2": 3.0, "f3": 1.0}, {"f3": 7.0}),
    fused("fmadds_adds_the_product", "fmadds", 2.0, 3.0, 1.0, 7.0),
    fused("fmsubs_subtracts_b", "fmsubs", 2.0, 3.0, 1.0, 5.0),
    fused("fnmadds_negates_the_sum", "fnmadds", 2.0, 3.0, 1.0, -7.0),
    fused("fnmsubs_negates_the_difference", "fnmsubs", 2.0, 3.0, 1.0, -5.0),
    # Single rounding of the exact product: an unfused fmadds would give 0.
    fused("fmadds_is_fused", "fmadds", FUSED_A, FUSED_C, -1.0, NEG_TWO_POW_M54),
    # Exact sum 1 + 2^-11 + 2^-24 + 2^-30 lies above the tie, so it rounds up to 1 + 2^-11 + 2^-23.
    fused("fmadds_rounds_the_result_to_float", "fmadds", ONE_PLUS_2_12, ONE_PLUS_2_12, 2.0 ** -30,
          0x3FF0020020000000),
    fused("fmadds_overflow_gives_infinity", "fmadds", 3.0e38, 4.0, 0.0, POS_INF),
]

# --- fsel, sign operations --------------------------------------------------------------------


CASES += [
    case("fsel_positive_picks_c", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": 1.0, "f2": 10.0, "f3": 20.0}, {"f4": 10.0}),
    case("fsel_negative_picks_b", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": -1.0, "f2": 10.0, "f3": 20.0}, {"f4": 20.0}),
    case("fsel_positive_zero_picks_c", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": 0.0, "f2": 10.0, "f3": 20.0}, {"f4": 10.0}),
    case("fsel_negative_zero_picks_c", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": NEG_ZERO, "f2": 10.0, "f3": 20.0}, {"f4": 10.0}),
    case("fsel_nan_picks_b", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": QNAN, "f2": 10.0, "f3": 20.0}, {"f4": 20.0}),
    case("fsel_negative_nan_picks_b", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": 0xFFF8000000000000, "f2": 10.0, "f3": 20.0}, {"f4": 20.0}),
    case("fsel_negative_infinity_picks_b", "FSEL", enc("fsel", frD=4, frA=1, frC=2, frB=3),
         {"f1": NEG_INF, "f2": 10.0, "f3": 20.0}, {"f4": 20.0}),
    unary("fneg_flips_the_sign", "fneg", 1.0, -1.0),
    unary("fneg_of_zero_gives_negative_zero", "fneg", 0.0, NEG_ZERO),
    unary("fneg_of_nan_keeps_the_payload", "fneg", 0x7FF8000000000123, 0xFFF8000000000123),
    unary("fabs_clears_the_sign", "fabs", -2.5, 2.5),
    unary("fabs_of_negative_nan_keeps_the_payload", "fabs", 0xFFF8000000000123,
          0x7FF8000000000123),
    unary("fnabs_sets_the_sign", "fnabs", 1.0, -1.0),
    unary("fnabs_keeps_a_negative_value", "fnabs", -1.0, -1.0),
    unary("fmr_copies_the_bits", "fmr", 1.25, 1.25),
    # A signalling NaN is not quieted: fmr is a bit copy.
    unary("fmr_keeps_a_signalling_nan", "fmr", 0x7FF0000000000001, 0x7FF0000000000001),
    case("fmr_destination_aliases_source", "FMR", enc("fmr", frD=1, frB=1),
         {"f1": 0x7FF0000000000001}, {"f1": 0x7FF0000000000001}),
]

# --- conversions to integer -------------------------------------------------------------------

CASES += [
    unary("fctiwz_truncates_toward_zero", "fctiwz", 1.9, 1),
    unary("fctiwz_negative_sign_extends", "fctiwz", -1.5, 0xFFFFFFFFFFFFFFFF),
    unary("fctiwz_of_2_pow_31_saturates_high", "fctiwz", TWO_POW_31, INT32_MAX),
    unary("fctiwz_just_below_2_pow_31", "fctiwz", 2147483647.9, INT32_MAX),
    unary("fctiwz_of_minus_2_pow_31", "fctiwz", -TWO_POW_31, INT32_MIN),
    unary("fctiwz_below_minus_2_pow_31_saturates_low", "fctiwz", -TWO_POW_31 - 1.0, INT32_MIN),
    unary("fctiwz_of_nan_gives_int32_min", "fctiwz", QNAN, INT32_MIN),
    unary("fctiwz_of_positive_infinity", "fctiwz", POS_INF, INT32_MAX),
    unary("fctiwz_of_negative_infinity", "fctiwz", NEG_INF, INT32_MIN),
    unary("fctiw_rounds_half_to_even_down", "fctiw", 2.5, 2),
    unary("fctiw_rounds_half_to_even_up", "fctiw", 3.5, 4),
    unary("fctiw_rounds_negative_half_to_even", "fctiw", -2.5, 0xFFFFFFFFFFFFFFFE),
    # 2^31 - 0.5 rounds to 2^31, which saturates.
    unary("fctiw_rounding_into_range_end_saturates", "fctiw", 2147483647.5, INT32_MAX),
    unary("fctiw_of_nan_gives_int32_min", "fctiw", QNAN, INT32_MIN),
    unary("fctidz_truncates_toward_zero", "fctidz", -1.5, 0xFFFFFFFFFFFFFFFF),
    unary("fctidz_large_value", "fctidz", 1.0e18, 0x0DE0B6B3A7640000),
    unary("fctidz_of_2_pow_63_saturates_high", "fctidz", TWO_POW_63, INT64_MAX),
    unary("fctidz_of_minus_2_pow_63", "fctidz", -TWO_POW_63, INT64_MIN),
    unary("fctidz_below_minus_2_pow_63_saturates_low", "fctidz", -TWO_POW_63 - 2048.0, INT64_MIN),
    unary("fctidz_of_nan_gives_int64_min", "fctidz", QNAN, INT64_MIN),
    unary("fctidz_of_positive_infinity", "fctidz", POS_INF, INT64_MAX),
    unary("fctidz_of_negative_infinity", "fctidz", NEG_INF, INT64_MIN),
    unary("fctid_rounds_half_to_even_down", "fctid", 2.5, 2),
    unary("fctid_rounds_half_to_even_up", "fctid", 3.5, 4),
    unary("fctid_of_nan_gives_int64_min", "fctid", QNAN, INT64_MIN),
    unary("fctid_of_2_pow_63_saturates_high", "fctid", TWO_POW_63, INT64_MAX),
    # The register bits are the integer; the result is a double.
    unary("fcfid_positive", "fcfid", 5, 5.0),
    unary("fcfid_negative", "fcfid", 0xFFFFFFFFFFFFFFFE, -2.0),
    unary("fcfid_of_int64_max_rounds_up_to_2_pow_63", "fcfid", INT64_MAX, 0x43E0000000000000),
    unary("fcfid_of_int64_min", "fcfid", INT64_MIN, 0xC3E0000000000000),
]

# --- compares ---------------------------------------------------------------------------------


def compare(name: str, mnemonic: str, a, b, nibble: int) -> Case:
    """`fcmp* cr2, f1, f2`; FPCC (FPSCR bits 16..19) gets the same nibble, the rest is kept."""
    # xer_so = 1 proves the so/un bit is not taken from XER.
    return case(name, mnemonic.upper(), enc(mnemonic, crfD=2, frA=1, frB=2),
                {"f1": a, "f2": b, "xer_so": 1, "fpscr": 0xFFFFFFFF},
                {"cr2": nibble, "fpscr": 0xFFFF0FFF | nibble << 12})


CASES += [
    compare("fcmpu_less", "fcmpu", 1.0, 2.0, 0b1000),
    compare("fcmpu_greater", "fcmpu", 2.0, 1.0, 0b0100),
    compare("fcmpu_equal", "fcmpu", 1.5, 1.5, 0b0010),
    compare("fcmpu_zeros_are_equal", "fcmpu", 0.0, NEG_ZERO, 0b0010),
    compare("fcmpu_infinity_is_greater", "fcmpu", POS_INF, 1.0, 0b0100),
    compare("fcmpu_nan_in_a_is_unordered", "fcmpu", QNAN, 1.0, 0b0001),
    compare("fcmpu_nan_in_b_is_unordered", "fcmpu", 1.0, QNAN, 0b0001),
    compare("fcmpu_two_nans_are_unordered", "fcmpu", QNAN, QNAN, 0b0001),
    compare("fcmpo_less", "fcmpo", 1.0, 2.0, 0b1000),
    compare("fcmpo_nan_is_unordered", "fcmpo", QNAN, 1.0, 0b0001),
    case("fcmpu_leaves_other_cr_fields", "FCMPU", enc("fcmpu", crfD=3, frA=1, frB=2),
         {"f1": 1.0, "f2": 2.0, "cr": 0x12345678, "fpscr": 0},
         {"cr": 0x12385678, "fpscr": 0x8000}),
]

# --- FPSCR moves ------------------------------------------------------------------------------

CASES += [
    case("mffs_reads_the_image_into_the_low_word", "MFFS", enc("mffs", frD=1),
         {"f1": 0xFFFFFFFFFFFFFFFF, "fpscr": 0x12345678}, {"f1": 0x12345678}),
    case("mtfsf_writes_the_selected_fields", "MTFSF", enc("mtfsf", FM=0x81, frB=1),
         {"f1": 0xFFFFFFFFABCDEF12, "fpscr": 0}, {"fpscr": 0xA0000002}),
    case("mtfsf_keeps_unselected_fields", "MTFSF", enc("mtfsf", FM=0x10, frB=1),
         {"f1": 0x00000000, "fpscr": 0xFFFFFFFF}, {"fpscr": 0xFFF0FFFF}),
    case("mtfsf_all_fields", "MTFSF", enc("mtfsf", FM=0xFF, frB=1),
         {"f1": 0xFFFFFFFF76543210, "fpscr": 0xFFFFFFFF}, {"fpscr": 0x76543210}),
    case("mtfsfi_writes_one_field", "MTFSFI", enc("mtfsfi", crfD=3, IMM_fp=9),
         {"fpscr": 0xFFFFFFFF}, {"fpscr": 0xFFF9FFFF}),
    case("mtfsb1_sets_a_bit", "MTFSB1", enc("mtfsb1", crbD=3), {"fpscr": 0},
         {"fpscr": 0x10000000}),
    case("mtfsb1_bit_31_is_the_least_significant", "MTFSB1", enc("mtfsb1", crbD=31),
         {"fpscr": 0}, {"fpscr": 1}),
    case("mtfsb0_clears_a_bit", "MTFSB0", enc("mtfsb0", crbD=3), {"fpscr": 0xFFFFFFFF},
         {"fpscr": 0xEFFFFFFF}),
    case("mcrfs_copies_the_fpcc_field", "MCRFS", enc("mcrfs", crfD=1, crfS=4),
         {"fpscr": 0x0000A000}, {"cr1": 0xA}),
    case("mcrfs_copies_the_first_field", "MCRFS", enc("mcrfs", crfD=6, crfS=0),
         {"fpscr": 0x50000000}, {"cr6": 5}),
    case("mffs_mtfsf_round_trip", "MFFS", [enc("mffs", frD=1), enc("mtfsb1", crbD=3),
                                         enc("mtfsf", FM=0xFF, frB=1)],
         {"fpscr": 0x00005000}, {"fpscr": 0x00005000, "f1": 0x00005000}),
]

# --- Rc=1: CR1 copies FPSCR bits 0..3 --------------------------------------------------------

CASES += [
    case("fadd_record_copies_fpscr_to_cr1", "FADD", enc("fadd", frD=4, frA=1, frB=2, Rc=1),
         {"f1": 1.0, "f2": 2.0, "fpscr": 0xA0000000}, {"f4": 3.0, "cr1": 0xA}),
    case("fmr_record_copies_fpscr_to_cr1", "FMR", enc("fmr", frD=4, frB=1, Rc=1),
         {"f1": 1.0, "fpscr": 0x50000000}, {"f4": 1.0, "cr1": 0x5}),
    case("fmadds_record_copies_fpscr_to_cr1", "FMADDS",
         enc("fmadds", frD=4, frA=1, frC=2, frB=3, Rc=1),
         {"f1": 2.0, "f2": 3.0, "f3": 1.0, "fpscr": 0x30000000}, {"f4": 7.0, "cr1": 0x3}),
    case("mtfsb1_record_sees_the_new_bit", "MTFSB1", enc("mtfsb1", crbD=0, Rc=1),
         {"fpscr": 0}, {"fpscr": 0x80000000, "cr1": 8}),
    case("fadd_without_record_leaves_cr1", "FADD", enc("fadd", frD=4, frA=1, frB=2),
         {"f1": 1.0, "f2": 2.0, "fpscr": 0xA0000000, "cr1": 3}, {"f4": 3.0, "cr1": 3}),
]

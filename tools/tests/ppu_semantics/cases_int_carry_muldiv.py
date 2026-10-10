"""int_carry_muldiv cases: carrying add/subtract, multiply and divide.

Expected values are derived by hand from the architecture and the cited RPCS3 handler. CA is the
carry out of the 64-bit add (a subtract is `~a + b + 1` or `+ CA`, so CA = no borrow); OV/SO follow
OE; Rc records the signed 64-bit result. CR0 nibbles are lt=8 gt=4 eq=2 so=1. Register layout:
`op r3,r4[,r5]`, chains use r6,r7,r8 for the second instruction.
"""

from tests import ppc_asm as asm

from . import Case

M64 = 0xFFFFFFFFFFFFFFFF
MIN64 = 0x8000000000000000
MAX64 = 0x7FFFFFFFFFFFFFFF
LT, GT, EQ, SO = 8, 4, 2, 1

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

CASES = []


def neg(n: int) -> int:
    """Two's complement of `n` as an unsigned 64-bit value."""
    return -n & M64


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def rrr(name: str, mnemonic: str, ref: str, a: int, b: int, expect: dict, init=None, **flags):
    """`mnemonic r3,r4,r5` with r4 = a, r5 = b."""
    return case(name, ref, enc(mnemonic, rD=3, rA=4, rB=5, **flags),
                {"r4": a, "r5": b, **(init or {})}, expect)


def rr(name: str, mnemonic: str, ref: str, a: int, expect: dict, init=None, **flags):
    """`mnemonic r3,r4` with r4 = a."""
    return case(name, ref, enc(mnemonic, rD=3, rA=4, **flags), {"r4": a, **(init or {})}, expect)


def ri(name: str, mnemonic: str, ref: str, a: int, simm: int, expect: dict, init=None):
    """`mnemonic r3,r4,simm` with r4 = a."""
    return case(name, ref, enc(mnemonic, rD=3, rA=4, SIMM=simm), {"r4": a, **(init or {})}, expect)


# --- addic, addic. ---------------------------------------------------------------------------

CASES += [
    ri("addic_minus_1_from_0", "addic", "ADDIC", 0, -1, {"r3": M64, "xer_ca": 0}),
    ri("addic_minus_1_from_1", "addic", "ADDIC", 1, -1, {"r3": 0, "xer_ca": 1}),
    # A 32-bit carry would be set here; the 64-bit sum has none.
    ri("addic_32_bit_carry_is_not_ca", "addic", "ADDIC", 0xFFFFFFFF, 1,
       {"r3": 0x100000000, "xer_ca": 0}),
    ri("addic_carry_out_at_2_64", "addic", "ADDIC", M64, 1, {"r3": 0, "xer_ca": 1}),
    ri("addic_clears_ca", "addic", "ADDIC", 0, 1, {"r3": 1, "xer_ca": 0}, {"xer_ca": 1}),
    # SIMM -32768 sign-extends to 0xffff...8000; + 0x8000 = 2^64.
    ri("addic_simm_sign_extended", "addic", "ADDIC", 0x8000, -32768, {"r3": 0, "xer_ca": 1}),
    case("addic_rd_is_ra", "ADDIC", enc("addic", rD=4, rA=4, SIMM=2), {"r4": M64},
         {"r4": 1, "xer_ca": 1}),
    ri("addic_does_not_record", "addic", "ADDIC", 0, -1, {"r3": M64, "cr0": 0xA}, {"cr0": 0xA}),
    ri("addic_does_not_touch_ov", "addic", "ADDIC", MAX64, 1,
       {"r3": MIN64, "xer_ov": 0, "xer_so": 0, "xer_ca": 0}),
    ri("addic_dot_negative", "addic.", "ADDIC", 0, -1, {"r3": M64, "xer_ca": 0, "cr0": LT}),
    ri("addic_dot_zero_with_carry", "addic.", "ADDIC", 1, -1, {"r3": 0, "xer_ca": 1, "cr0": EQ}),
    ri("addic_dot_positive", "addic.", "ADDIC", 5, 1, {"r3": 6, "xer_ca": 0, "cr0": GT}),
    ri("addic_dot_copies_so", "addic.", "ADDIC", 5, 1, {"r3": 6, "cr0": GT | SO},
       {"xer_so": 1}),
    # Negative only as a 32-bit value: the 64-bit result is positive.
    ri("addic_dot_32_bit_negative_is_positive", "addic.", "ADDIC", 0x7FFFFFFF, 1,
       {"r3": 0x80000000, "cr0": GT}),
]


# --- addc ------------------------------------------------------------------------------------

CASES += [
    rrr("addc_basic", "addc", "ADDC", 1, 2, {"r3": 3, "xer_ca": 0}),
    rrr("addc_carry_out_at_2_64", "addc", "ADDC", M64, 1, {"r3": 0, "xer_ca": 1}),
    rrr("addc_32_bit_carry_is_not_ca", "addc", "ADDC", 0xFFFFFFFF, 1,
        {"r3": 0x100000000, "xer_ca": 0}),
    rrr("addc_both_msb", "addc", "ADDC", MIN64, MIN64, {"r3": 0, "xer_ca": 1}),
    # addc ignores the incoming CA and overwrites it.
    rrr("addc_ignores_and_clears_ca", "addc", "ADDC", 1, 1, {"r3": 2, "xer_ca": 0},
        {"xer_ca": 1}),
    rrr("addc_no_ov_without_oe", "addc", "ADDC", MAX64, 1,
        {"r3": MIN64, "xer_ov": 0, "xer_so": 0, "xer_ca": 0}),
    rrr("addco_overflow_without_carry", "addc", "ADDC", MAX64, 1,
        {"r3": MIN64, "xer_ov": 1, "xer_so": 1, "xer_ca": 0}, OE=1),
    rrr("addco_carry_without_overflow", "addc", "ADDC", M64, 1,
        {"r3": 0, "xer_ov": 0, "xer_so": 0, "xer_ca": 1}, OE=1),
    rrr("addc_rc_zero", "addc", "ADDC", M64, 1, {"r3": 0, "xer_ca": 1, "cr0": EQ}, Rc=1),
    rrr("addc_rc_negative", "addc", "ADDC", M64, 0, {"r3": M64, "xer_ca": 0, "cr0": LT}, Rc=1),
    rrr("addco_rc_overflow", "addc", "ADDC", MAX64, 1,
        {"r3": MIN64, "xer_ov": 1, "cr0": LT | SO}, OE=1, Rc=1),
    case("addco_rd_is_ra", "ADDC", enc("addc", rD=4, rA=4, rB=5, OE=1), {"r4": MAX64, "r5": 1},
         {"r4": MIN64, "xer_ov": 1}),
    case("addc_rd_is_rb", "ADDC", enc("addc", rD=5, rA=4, rB=5), {"r4": 3, "r5": 4}, {"r5": 7}),
]


# --- adde ------------------------------------------------------------------------------------

CASES += [
    rrr("adde_carry_in", "adde", "ADDE", 1, 2, {"r3": 4, "xer_ca": 0}, {"xer_ca": 1}),
    rrr("adde_no_carry_in", "adde", "ADDE", 1, 2, {"r3": 3, "xer_ca": 0}),
    # The carry comes from the carry-in alone: a + b = 2^64 - 1.
    rrr("adde_carry_from_carry_in_only", "adde", "ADDE", M64, 0, {"r3": 0, "xer_ca": 1},
        {"xer_ca": 1}),
    rrr("adde_both_carries", "adde", "ADDE", M64, M64, {"r3": M64, "xer_ca": 1}, {"xer_ca": 1}),
    rrr("adde_carry_out_without_carry_in", "adde", "ADDE", M64, M64,
        {"r3": M64 - 1, "xer_ca": 1}),
    # 128-bit add: (5:2^64-1) + (6:1) = (12:0)
    case("adde_chain_128_bit", "ADDC", [enc("addc", rD=3, rA=4, rB=5),
                                        enc("adde", rD=6, rA=7, rB=8)],
         {"r4": M64, "r5": 1, "r7": 5, "r8": 6}, {"r3": 0, "r6": 12, "xer_ca": 0}),
    # (2^64-1:2^64-1) + (0:1) = (0:0) with a carry out of the top word.
    case("adde_chain_carry_out_of_top", "ADDE", [enc("addc", rD=3, rA=4, rB=5),
                                                 enc("adde", rD=6, rA=7, rB=8)],
         {"r4": M64, "r5": 1, "r7": M64, "r8": 0}, {"r3": 0, "r6": 0, "xer_ca": 1}),
    rrr("addeo_overflow_from_carry_in", "adde", "ADDE", MAX64, 0,
        {"r3": MIN64, "xer_ov": 1, "xer_so": 1, "xer_ca": 0}, {"xer_ca": 1}, OE=1),
    # MIN64 + (-1) + 1 = MIN64: fits, carry out of bit 63 is 1.
    rrr("addeo_no_overflow_exact_min", "adde", "ADDE", MIN64, M64,
        {"r3": MIN64, "xer_ov": 0, "xer_so": 0, "xer_ca": 1}, {"xer_ca": 1}, OE=1),
    rrr("adde_rc_zero", "adde", "ADDE", M64, 0, {"r3": 0, "xer_ca": 1, "cr0": EQ},
        {"xer_ca": 1}, Rc=1),
    case("adde_rd_is_rb", "ADDE", enc("adde", rD=5, rA=4, rB=5), {"r4": 3, "r5": 4, "xer_ca": 1},
         {"r5": 8, "xer_ca": 0}),
]


# --- addze -----------------------------------------------------------------------------------

CASES += [
    rr("addze_carry_in", "addze", "ADDZE", 5, {"r3": 6, "xer_ca": 0}, {"xer_ca": 1}),
    rr("addze_no_carry_in", "addze", "ADDZE", 5, {"r3": 5, "xer_ca": 0}),
    rr("addze_carry_out", "addze", "ADDZE", M64, {"r3": 0, "xer_ca": 1}, {"xer_ca": 1}),
    rr("addze_no_carry_out_without_carry_in", "addze", "ADDZE", M64, {"r3": M64, "xer_ca": 0}),
    rr("addzeo_overflow", "addze", "ADDZE", MAX64,
       {"r3": MIN64, "xer_ov": 1, "xer_so": 1, "xer_ca": 0}, {"xer_ca": 1}, OE=1),
    rr("addzeo_no_overflow", "addze", "ADDZE", MAX64,
       {"r3": MAX64, "xer_ov": 0, "xer_so": 1}, {"xer_ca": 0, "xer_so": 1}, OE=1),
    rr("addze_rc", "addze", "ADDZE", M64, {"r3": 0, "xer_ca": 1, "cr0": EQ}, {"xer_ca": 1}, Rc=1),
    case("addze_rd_is_ra", "ADDZE", enc("addze", rD=4, rA=4), {"r4": 9, "xer_ca": 1},
         {"r4": 10}),
]


# --- addme -----------------------------------------------------------------------------------

CASES += [
    rr("addme_carry_in", "addme", "ADDME", 5, {"r3": 5, "xer_ca": 1}, {"xer_ca": 1}),
    rr("addme_no_carry_in", "addme", "ADDME", 5, {"r3": 4, "xer_ca": 1}),
    rr("addme_zero_no_carry_in", "addme", "ADDME", 0, {"r3": M64, "xer_ca": 0}),
    rr("addme_zero_carry_in", "addme", "ADDME", 0, {"r3": 0, "xer_ca": 1}, {"xer_ca": 1}),
    rr("addmeo_overflow", "addme", "ADDME", MIN64,
       {"r3": MAX64, "xer_ov": 1, "xer_so": 1, "xer_ca": 1}, OE=1),
    rr("addmeo_carry_in_avoids_overflow", "addme", "ADDME", MIN64,
       {"r3": MIN64, "xer_ov": 0, "xer_so": 0, "xer_ca": 1}, {"xer_ca": 1}, OE=1),
    rr("addme_rc_negative", "addme", "ADDME", 0, {"r3": M64, "xer_ca": 0, "cr0": LT}, Rc=1),
]


# --- subfc: rB - rA --------------------------------------------------------------------------

CASES += [
    rrr("subfc_no_borrow", "subfc", "SUBFC", 3, 10, {"r3": 7, "xer_ca": 1}),
    rrr("subfc_borrow", "subfc", "SUBFC", 10, 3, {"r3": neg(7), "xer_ca": 0}),
    rrr("subfc_equal", "subfc", "SUBFC", 5, 5, {"r3": 0, "xer_ca": 1}),
    rrr("subfc_zero_minus_zero", "subfc", "SUBFC", 0, 0, {"r3": 0, "xer_ca": 1}),
    rrr("subfc_zero_minus_one", "subfc", "SUBFC", 1, 0, {"r3": M64, "xer_ca": 0}),
    # CA is an unsigned compare: 0xffff... - 1 does not borrow.
    rrr("subfc_unsigned_no_borrow", "subfc", "SUBFC", 1, M64, {"r3": M64 - 1, "xer_ca": 1}),
    # The incoming CA is ignored.
    rrr("subfc_ignores_ca", "subfc", "SUBFC", 3, 10, {"r3": 7, "xer_ca": 1}, {"xer_ca": 0}),
    rrr("subfco_overflow", "subfc", "SUBFC", 1, MIN64,
        {"r3": MAX64, "xer_ov": 1, "xer_so": 1, "xer_ca": 1}, OE=1),
    rrr("subfco_no_overflow", "subfc", "SUBFC", 10, 3,
        {"r3": neg(7), "xer_ov": 0, "xer_so": 0, "xer_ca": 0}, OE=1),
    rrr("subfc_rc_negative", "subfc", "SUBFC", 10, 3, {"r3": neg(7), "xer_ca": 0, "cr0": LT},
        Rc=1),
    rrr("subfc_rc_zero", "subfc", "SUBFC", 5, 5, {"r3": 0, "xer_ca": 1, "cr0": EQ}, Rc=1),
    rrr("subfco_rc_overflow", "subfc", "SUBFC", 1, MIN64,
        {"r3": MAX64, "xer_ov": 1, "cr0": GT | SO}, OE=1, Rc=1),
    case("subfc_rd_is_ra", "SUBFC", enc("subfc", rD=4, rA=4, rB=5), {"r4": 3, "r5": 10},
         {"r4": 7, "xer_ca": 1}),
]


# --- subfe: ~rA + rB + CA --------------------------------------------------------------------

CASES += [
    rrr("subfe_no_borrow_in", "subfe", "SUBFE", 3, 10, {"r3": 7, "xer_ca": 1}, {"xer_ca": 1}),
    rrr("subfe_borrow_in", "subfe", "SUBFE", 3, 10, {"r3": 6, "xer_ca": 1}),
    rrr("subfe_borrow_in_equal_operands", "subfe", "SUBFE", 5, 5, {"r3": M64, "xer_ca": 0}),
    rrr("subfe_equal_operands_no_borrow_in", "subfe", "SUBFE", 5, 5, {"r3": 0, "xer_ca": 1},
        {"xer_ca": 1}),
    # (5:0) - (2:1): low word borrows, high word 5 - 2 - 1 = 2.
    case("subfe_chain_128_bit", "SUBFE", [enc("subfc", rD=3, rA=4, rB=5),
                                          enc("subfe", rD=6, rA=7, rB=8)],
         {"r4": 1, "r5": 0, "r7": 2, "r8": 5}, {"r3": M64, "r6": 2, "xer_ca": 1}),
    # (0:0) - (1:0): high word borrows out.
    case("subfe_chain_borrow_out_of_top", "SUBFE", [enc("subfc", rD=3, rA=4, rB=5),
                                                    enc("subfe", rD=6, rA=7, rB=8)],
         {"r4": 0, "r5": 0, "r7": 1, "r8": 0}, {"r3": 0, "r6": M64, "xer_ca": 0}),
    # ~MIN64 + MAX64 + 0 overflows (MAX64 - MIN64 - 1).
    rrr("subfeo_overflow", "subfe", "SUBFE", MIN64, MAX64,
        {"r3": M64 - 1, "xer_ov": 1, "xer_so": 1, "xer_ca": 0}, OE=1),
    rrr("subfe_rc_negative", "subfe", "SUBFE", 5, 5, {"r3": M64, "xer_ca": 0, "cr0": LT}, Rc=1),
    case("subfe_rd_is_ra", "SUBFE", enc("subfe", rD=4, rA=4, rB=5),
         {"r4": 3, "r5": 10, "xer_ca": 1}, {"r4": 7, "xer_ca": 1}),
]


# --- subfze: ~rA + CA ------------------------------------------------------------------------

CASES += [
    rr("subfze_carry_in", "subfze", "SUBFZE", 5, {"r3": neg(5), "xer_ca": 0}, {"xer_ca": 1}),
    rr("subfze_no_carry_in", "subfze", "SUBFZE", 5, {"r3": neg(6), "xer_ca": 0}),
    rr("subfze_zero_carry_in", "subfze", "SUBFZE", 0, {"r3": 0, "xer_ca": 1}, {"xer_ca": 1}),
    rr("subfze_zero_no_carry_in", "subfze", "SUBFZE", 0, {"r3": M64, "xer_ca": 0}),
    rr("subfzeo_overflow", "subfze", "SUBFZE", MIN64,
       {"r3": MIN64, "xer_ov": 1, "xer_so": 1, "xer_ca": 0}, {"xer_ca": 1}, OE=1),
    rr("subfze_rc_negative", "subfze", "SUBFZE", 5, {"r3": neg(5), "xer_ca": 0, "cr0": LT},
       {"xer_ca": 1}, Rc=1),
]


# --- subfme: ~rA + (-1) + CA -----------------------------------------------------------------

CASES += [
    rr("subfme_carry_in", "subfme", "SUBFME", 5, {"r3": neg(6), "xer_ca": 1}, {"xer_ca": 1}),
    rr("subfme_no_carry_in", "subfme", "SUBFME", 5, {"r3": neg(7), "xer_ca": 1}),
    rr("subfme_all_ones_no_carry_in", "subfme", "SUBFME", M64, {"r3": M64, "xer_ca": 0}),
    rr("subfme_all_ones_carry_in", "subfme", "SUBFME", M64, {"r3": 0, "xer_ca": 1},
       {"xer_ca": 1}),
    rr("subfmeo_overflow", "subfme", "SUBFME", MAX64,
       {"r3": MAX64, "xer_ov": 1, "xer_so": 1, "xer_ca": 1}, OE=1),
    rr("subfme_rc_zero", "subfme", "SUBFME", M64, {"r3": 0, "xer_ca": 1, "cr0": EQ},
       {"xer_ca": 1}, Rc=1),
]


# --- subfic: SIMM - rA -----------------------------------------------------------------------

CASES += [
    ri("subfic_no_borrow", "subfic", "SUBFIC", 3, 10, {"r3": 7, "xer_ca": 1}),
    ri("subfic_borrow", "subfic", "SUBFIC", 10, 3, {"r3": neg(7), "xer_ca": 0}),
    ri("subfic_minus_1_minus_0", "subfic", "SUBFIC", 0, -1, {"r3": M64, "xer_ca": 1}),
    ri("subfic_zero_minus_zero", "subfic", "SUBFIC", 0, 0, {"r3": 0, "xer_ca": 1}),
    ri("subfic_zero_minus_one", "subfic", "SUBFIC", 1, 0, {"r3": M64, "xer_ca": 0}),
    ri("subfic_minus_1_minus_minus_1", "subfic", "SUBFIC", M64, -1, {"r3": 0, "xer_ca": 1}),
    ri("subfic_clears_ca", "subfic", "SUBFIC", 10, 3, {"r3": neg(7), "xer_ca": 0}, {"xer_ca": 1}),
    ri("subfic_does_not_record", "subfic", "SUBFIC", 3, 3, {"r3": 0, "cr0": 0xA}, {"cr0": 0xA}),
    case("subfic_rd_is_ra", "SUBFIC", enc("subfic", rD=4, rA=4, SIMM=100), {"r4": 1},
         {"r4": 99, "xer_ca": 1}),
]


# --- mulli -----------------------------------------------------------------------------------

CASES += [
    ri("mulli_basic", "mulli", "MULLI", 6, 7, {"r3": 42}),
    ri("mulli_negative_immediate", "mulli", "MULLI", 7, -3, {"r3": neg(21)}),
    ri("mulli_negative_both", "mulli", "MULLI", neg(5), -3, {"r3": 15}),
    ri("mulli_64_bit", "mulli", "MULLI", 0x100000001, 3, {"r3": 0x300000003}),
    ri("mulli_wraps", "mulli", "MULLI", MIN64, 2, {"r3": 0}),
    ri("mulli_max_times_minus_1", "mulli", "MULLI", MAX64, -1, {"r3": MIN64 + 1}),
    ri("mulli_min_times_minus_1_wraps", "mulli", "MULLI", MIN64, -1, {"r3": MIN64}),
    ri("mulli_leaves_ca_and_cr0", "mulli", "MULLI", 2, 2, {"r3": 4, "xer_ca": 1, "cr0": 0x5},
       {"xer_ca": 1, "cr0": 0x5}),
]


# --- mullw -----------------------------------------------------------------------------------

CASES += [
    rrr("mullw_basic", "mullw", "MULLW", 6, 7, {"r3": 42}),
    rrr("mullw_uses_low_words", "mullw", "MULLW", 0xAAAAAAAA00000003, 0xBBBBBBBB00000004,
        {"r3": 12}),
    # The product is the full 64-bit value of the sign-extended words.
    rrr("mullw_negative", "mullw", "MULLW", 0xFFFFFFFF, 5, {"r3": neg(5)}),
    rrr("mullw_int32_min_times_2", "mullw", "MULLW", 0x80000000, 2, {"r3": 0xFFFFFFFF00000000}),
    rrr("mullw_max_squared", "mullw", "MULLW", 0x7FFFFFFF, 0x7FFFFFFF,
        {"r3": 0x3FFFFFFF00000001}),
    rrr("mullw_no_ov_without_oe", "mullw", "MULLW", 0x10000, 0x10000,
        {"r3": 0x100000000, "xer_ov": 0, "xer_so": 0}),
    rrr("mullwo_overflow", "mullw", "MULLW", 0x10000, 0x10000,
        {"r3": 0x100000000, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("mullwo_no_overflow", "mullw", "MULLW", 0x7FFFFFFF, 1,
        {"r3": 0x7FFFFFFF, "xer_ov": 0, "xer_so": 0}, OE=1),
    # +2^31 does not fit in int32.
    rrr("mullwo_int32_min_times_minus_1", "mullw", "MULLW", 0x80000000, 0xFFFFFFFF,
        {"r3": 0x80000000, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("mullwo_int32_min_times_1_fits", "mullw", "MULLW", 0x80000000, 1,
        {"r3": 0xFFFFFFFF80000000, "xer_ov": 0, "xer_so": 0}, OE=1),
    rrr("mullwo_clears_ov_keeps_so", "mullw", "MULLW", 2, 3,
        {"r3": 6, "xer_ov": 0, "xer_so": 1}, {"xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("mullw_rc_negative", "mullw", "MULLW", 0xFFFFFFFF, 5, {"r3": neg(5), "cr0": LT}, Rc=1),
    # Low word 0 but a non-zero 64-bit result: gt, not eq.
    rrr("mullw_rc_records_the_64_bit_result", "mullw", "MULLW", 0x10000, 0x10000,
        {"r3": 0x100000000, "cr0": GT}, Rc=1),
    rrr("mullw_rc_zero", "mullw", "MULLW", 0, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("mullwo_rc_overflow", "mullw", "MULLW", 0x10000, 0x10000,
        {"r3": 0x100000000, "xer_ov": 1, "cr0": GT | SO}, OE=1, Rc=1),
    case("mullw_rd_is_ra", "MULLW", enc("mullw", rD=4, rA=4, rB=5), {"r4": 6, "r5": 7},
         {"r4": 42}),
]


# --- mulld -----------------------------------------------------------------------------------

CASES += [
    rrr("mulld_basic", "mulld", "MULLD", 6, 7, {"r3": 42}),
    # (2^32 + 1)^2 = 2^64 + 2^33 + 1
    rrr("mulld_64_bit", "mulld", "MULLD", 0x100000001, 0x100000001, {"r3": 0x200000001}),
    rrr("mulld_negative", "mulld", "MULLD", neg(3), 5, {"r3": neg(15)}),
    rrr("mulld_no_ov_without_oe", "mulld", "MULLD", 1 << 32, 1 << 32,
        {"r3": 0, "xer_ov": 0, "xer_so": 0}),
    rrr("mulldo_overflow_2_64", "mulld", "MULLD", 1 << 32, 1 << 32,
        {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    # 2^63: the low word looks negative but the high word is 0.
    rrr("mulldo_overflow_2_63", "mulld", "MULLD", 1 << 31, 1 << 32,
        {"r3": MIN64, "xer_ov": 1, "xer_so": 1}, OE=1),
    # -2^63 fits.
    rrr("mulldo_min_fits", "mulld", "MULLD", 0xFFFFFFFF80000000, 1 << 32,
        {"r3": MIN64, "xer_ov": 0, "xer_so": 0}, OE=1),
    rrr("mulldo_min_times_minus_1", "mulld", "MULLD", MIN64, M64,
        {"r3": MIN64, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("mulldo_clears_ov_keeps_so", "mulld", "MULLD", 3, 4,
        {"r3": 12, "xer_ov": 0, "xer_so": 1}, {"xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("mulld_rc_negative", "mulld", "MULLD", neg(3), 5, {"r3": neg(15), "cr0": LT}, Rc=1),
    rrr("mulld_rc_zero", "mulld", "MULLD", 0, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("mulldo_rc_overflow", "mulld", "MULLD", 1 << 32, 1 << 32,
        {"r3": 0, "xer_ov": 1, "cr0": EQ | SO}, OE=1, Rc=1),
    case("mulld_rd_is_rb", "MULLD", enc("mulld", rD=5, rA=4, rB=5), {"r4": 6, "r5": 7},
         {"r5": 42}),
]


# --- mulhw: sign-extended high word ----------------------------------------------------------

CASES += [
    rrr("mulhw_max_squared", "mulhw", "MULHW", 0x7FFFFFFF, 0x7FFFFFFF, {"r3": 0x3FFFFFFF}),
    # -1 * 1 = -1: the high word is all ones, sign-extended into rD.
    rrr("mulhw_minus_1_times_1", "mulhw", "MULHW", 0xFFFFFFFF, 1, {"r3": M64}),
    rrr("mulhw_min_squared", "mulhw", "MULHW", 0x80000000, 0x80000000, {"r3": 0x40000000}),
    rrr("mulhw_min_times_2", "mulhw", "MULHW", 0x80000000, 2, {"r3": M64}),
    # -2^31 * (2^31 - 1) = -(2^62 - 2^31); the arithmetic shift rounds down to -2^30.
    rrr("mulhw_min_times_max", "mulhw", "MULHW", 0x80000000, 0x7FFFFFFF,
        {"r3": 0xFFFFFFFFC0000000}),
    rrr("mulhw_2_32", "mulhw", "MULHW", 0x10000, 0x10000, {"r3": 1}),
    rrr("mulhw_uses_low_words", "mulhw", "MULHW", 0xAAAAAAAA00000002, 0xBBBBBBBB00000003,
        {"r3": 0}),
    rrr("mulhw_rc_negative", "mulhw", "MULHW", 0xFFFFFFFF, 1, {"r3": M64, "cr0": LT}, Rc=1),
    rrr("mulhw_rc_positive", "mulhw", "MULHW", 0x10000, 0x10000, {"r3": 1, "cr0": GT}, Rc=1),
    rrr("mulhw_rc_zero", "mulhw", "MULHW", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),
]


# --- mulhwu: zero-extended high word ---------------------------------------------------------

CASES += [
    rrr("mulhwu_max_squared", "mulhwu", "MULHWU", 0xFFFFFFFF, 0xFFFFFFFF,
        {"r3": 0xFFFFFFFE}),
    rrr("mulhwu_2_32", "mulhwu", "MULHWU", 0x80000000, 2, {"r3": 1}),
    rrr("mulhwu_uses_low_words", "mulhwu", "MULHWU", 0xAAAAAAAA00000002, 0xBBBBBBBB00000003,
        {"r3": 0}),
    # The result is zero-extended, so it is positive as a 64-bit value.
    rrr("mulhwu_rc_zero_extended_is_positive", "mulhwu", "MULHWU", 0xFFFFFFFF, 0xFFFFFFFF,
        {"r3": 0xFFFFFFFE, "cr0": GT}, Rc=1),
    rrr("mulhwu_rc_zero", "mulhwu", "MULHWU", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),
]


# --- mulhd, mulhdu ---------------------------------------------------------------------------

CASES += [
    rrr("mulhd_min_squared", "mulhd", "MULHD", MIN64, MIN64, {"r3": 1 << 62}),
    rrr("mulhd_minus_1_times_1", "mulhd", "MULHD", M64, 1, {"r3": M64}),
    rrr("mulhd_minus_1_squared", "mulhd", "MULHD", M64, M64, {"r3": 0}),
    rrr("mulhd_2_64", "mulhd", "MULHD", 1 << 32, 1 << 32, {"r3": 1}),
    # (2^63 - 1)^2 = 2^126 - 2^64 + 1
    rrr("mulhd_max_squared", "mulhd", "MULHD", MAX64, MAX64, {"r3": (1 << 62) - 1}),
    # -2^63 * (2^63 - 1) = -(2^126 - 2^63), which rounds down to -2^62 in the high half.
    rrr("mulhd_min_times_max", "mulhd", "MULHD", MIN64, MAX64, {"r3": 0xC000000000000000}),
    rrr("mulhd_rc_negative", "mulhd", "MULHD", M64, 1, {"r3": M64, "cr0": LT}, Rc=1),
    rrr("mulhd_rc_zero", "mulhd", "MULHD", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),
    # No OE form: bit 21 is ignored.
    rrr("mulhd_leaves_ov", "mulhd", "MULHD", 3, 4, {"r3": 0, "xer_ov": 1, "xer_so": 1},
        {"xer_ov": 1, "xer_so": 1}),

    # (2^64 - 1)^2 = 2^128 - 2^65 + 1
    rrr("mulhdu_max_squared", "mulhdu", "MULHDU", M64, M64, {"r3": M64 - 1}),
    rrr("mulhdu_2_64", "mulhdu", "MULHDU", MIN64, 2, {"r3": 1}),
    rrr("mulhdu_max_times_2", "mulhdu", "MULHDU", M64, 2, {"r3": 1}),
    rrr("mulhdu_max_times_1", "mulhdu", "MULHDU", M64, 1, {"r3": 0}),
    rrr("mulhdu_min_squared", "mulhdu", "MULHDU", MIN64, MIN64, {"r3": 1 << 62}),
    rrr("mulhdu_rc_negative_as_signed", "mulhdu", "MULHDU", M64, M64,
        {"r3": M64 - 1, "cr0": LT}, Rc=1),
    rrr("mulhdu_rc_zero", "mulhdu", "MULHDU", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),
]


# --- divw ------------------------------------------------------------------------------------

CASES += [
    rrr("divw_basic", "divw", "DIVW", 20, 5, {"r3": 4}),
    # The result is the zero-extended 32-bit quotient.
    rrr("divw_negative_dividend", "divw", "DIVW", 0xFFFFFFEC, 5, {"r3": 0xFFFFFFFC}),
    # 7 / -2 truncates toward zero; the high words are ignored.
    rrr("divw_truncates_toward_zero", "divw", "DIVW", 0xDEADBEEF00000007, 0xCAFEBABEFFFFFFFE,
        {"r3": 0xFFFFFFFD}),
    rrr("divw_int32_min_by_1", "divw", "DIVW", 0x80000000, 1, {"r3": 0x80000000}),
    rrr("divw_by_zero", "divw", "DIVW", 5, 0, {"r3": 0, "xer_ov": 0, "xer_so": 0},
        {"r3": 0x1111}),
    rrr("divwo_by_zero", "divw", "DIVW", 5, 0, {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    # Only the low word of the divisor counts.
    rrr("divwo_low_word_of_divisor_is_zero", "divw", "DIVW", 5, 0x100000000,
        {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divw_int32_min_by_minus_1", "divw", "DIVW", 0x80000000, 0xFFFFFFFF,
        {"r3": 0, "xer_ov": 0, "xer_so": 0}, {"r3": 0x1111}),
    rrr("divwo_int32_min_by_minus_1", "divw", "DIVW", 0x80000000, 0xFFFFFFFF,
        {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divwo_no_overflow", "divw", "DIVW", 20, 5, {"r3": 4, "xer_ov": 0, "xer_so": 1},
        {"xer_ov": 1, "xer_so": 1}, OE=1),
    # A negative quotient is positive as a zero-extended 64-bit value.
    rrr("divw_rc_zero_extended_is_positive", "divw", "DIVW", 0xFFFFFFEC, 5,
        {"r3": 0xFFFFFFFC, "cr0": GT}, Rc=1),
    rrr("divw_rc_zero", "divw", "DIVW", 0, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("divwo_rc_by_zero", "divw", "DIVW", 5, 0, {"r3": 0, "xer_ov": 1, "cr0": EQ | SO},
        OE=1, Rc=1),
    case("divw_rd_is_ra", "DIVW", enc("divw", rD=4, rA=4, rB=5), {"r4": 20, "r5": 5},
         {"r4": 4}),
]


# --- divwu -----------------------------------------------------------------------------------

CASES += [
    rrr("divwu_basic", "divwu", "DIVWU", 20, 5, {"r3": 4}),
    rrr("divwu_unsigned", "divwu", "DIVWU", 0xFFFFFFFE, 2, {"r3": 0x7FFFFFFF}),
    rrr("divwu_uses_low_words", "divwu", "DIVWU", 0x100000005, 2, {"r3": 2}),
    rrr("divwu_by_zero", "divwu", "DIVWU", 5, 0, {"r3": 0, "xer_ov": 0, "xer_so": 0},
        {"r3": 0x1111}),
    rrr("divwuo_by_zero", "divwu", "DIVWU", 5, 0, {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divwuo_low_word_of_divisor_is_zero", "divwu", "DIVWU", 5, 0x100000000,
        {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    # INT_MIN / -1 is an ordinary unsigned division here.
    rrr("divwuo_no_signed_overflow_rule", "divwu", "DIVWU", 0x80000000, 0xFFFFFFFF,
        {"r3": 0, "xer_ov": 0, "xer_so": 0}, OE=1),
    rrr("divwuo_clears_ov_keeps_so", "divwu", "DIVWU", 20, 5,
        {"r3": 4, "xer_ov": 0, "xer_so": 1}, {"xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divwu_rc_high_bit_result", "divwu", "DIVWU", 0xFFFFFFFF, 1,
        {"r3": 0xFFFFFFFF, "cr0": GT}, Rc=1),
    rrr("divwu_rc_zero", "divwu", "DIVWU", 3, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("divwuo_rc_by_zero", "divwu", "DIVWU", 5, 0, {"r3": 0, "xer_ov": 1, "cr0": EQ | SO},
        OE=1, Rc=1),
]


# --- divd ------------------------------------------------------------------------------------

CASES += [
    rrr("divd_basic", "divd", "DIVD", 100, 7, {"r3": 14}),
    rrr("divd_truncates_toward_zero", "divd", "DIVD", neg(100), 7, {"r3": neg(14)}),
    rrr("divd_64_bit", "divd", "DIVD", 0x300000000, 3, {"r3": 0x100000000}),
    rrr("divd_min_by_1", "divd", "DIVD", MIN64, 1, {"r3": MIN64}),
    rrr("divd_by_zero", "divd", "DIVD", 5, 0, {"r3": 0, "xer_ov": 0, "xer_so": 0},
        {"r3": 0x1111}),
    rrr("divdo_by_zero", "divd", "DIVD", 5, 0, {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divd_int64_min_by_minus_1", "divd", "DIVD", MIN64, M64,
        {"r3": 0, "xer_ov": 0, "xer_so": 0}, {"r3": 0x1111}),
    rrr("divdo_int64_min_by_minus_1", "divd", "DIVD", MIN64, M64,
        {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divdo_no_overflow", "divd", "DIVD", 100, 7, {"r3": 14, "xer_ov": 0, "xer_so": 1},
        {"xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divd_rc_negative", "divd", "DIVD", neg(100), 7, {"r3": neg(14), "cr0": LT}, Rc=1),
    rrr("divd_rc_zero", "divd", "DIVD", 0, 7, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("divdo_rc_by_zero", "divd", "DIVD", 5, 0, {"r3": 0, "xer_ov": 1, "cr0": EQ | SO},
        OE=1, Rc=1),
]


# --- divdu -----------------------------------------------------------------------------------

CASES += [
    rrr("divdu_basic", "divdu", "DIVDU", 100, 7, {"r3": 14}),
    rrr("divdu_unsigned", "divdu", "DIVDU", M64, 2, {"r3": MAX64}),
    rrr("divdu_by_zero", "divdu", "DIVDU", 5, 0, {"r3": 0, "xer_ov": 0, "xer_so": 0},
        {"r3": 0x1111}),
    rrr("divduo_by_zero", "divdu", "DIVDU", 5, 0, {"r3": 0, "xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divduo_no_signed_overflow_rule", "divdu", "DIVDU", MIN64, M64,
        {"r3": 0, "xer_ov": 0, "xer_so": 0}, OE=1),
    rrr("divduo_clears_ov_keeps_so", "divdu", "DIVDU", 100, 7,
        {"r3": 14, "xer_ov": 0, "xer_so": 1}, {"xer_ov": 1, "xer_so": 1}, OE=1),
    rrr("divdu_rc_negative_as_signed", "divdu", "DIVDU", M64, 1, {"r3": M64, "cr0": LT}, Rc=1),
    rrr("divdu_rc_zero", "divdu", "DIVDU", 3, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    rrr("divduo_rc_by_zero", "divdu", "DIVDU", 5, 0, {"r3": 0, "xer_ov": 1, "cr0": EQ | SO},
        OE=1, Rc=1),
]

"""int_alu cases: arithmetic, logical, extension, count-leading-zeros and compares.

Expected values are derived by hand from the architecture and the cited RPCS3 handler. CR0 nibbles
are lt=8 gt=4 eq=2 so=1.
"""

from tests import ppc_asm as asm

from . import Case

M64 = 0xFFFFFFFFFFFFFFFF
MIN64 = 0x8000000000000000
MAX64 = 0x7FFFFFFFFFFFFFFF
LT, GT, EQ, SO = 8, 4, 2, 1

REF = "RPCS3 PPUInterpreter.cpp "


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


enc = asm.encode

CASES = []


# --- addi, addis -----------------------------------------------------------------------------

CASES += [
    case("addi_li", "ADDI", enc("addi", rD=3, rA=0, SIMM=5), {"r0": 0x1234}, {"r3": 5}),
    case("addi_li_negative", "ADDI", enc("addi", rD=3, rA=0, SIMM=-2), {}, {"r3": M64 - 1}),
    case("addi_ra_is_used", "ADDI", enc("addi", rD=3, rA=4, SIMM=-1),
         {"r4": 0x100000000}, {"r3": 0xFFFFFFFF}),
    case("addi_wraps_64", "ADDI", enc("addi", rD=3, rA=4, SIMM=1), {"r4": M64}, {"r3": 0}),
    case("addi_rd_is_ra", "ADDI", enc("addi", rD=4, rA=4, SIMM=3), {"r4": 10}, {"r4": 13}),
    case("addi_does_not_record", "ADDI", enc("addi", rD=3, rA=4, SIMM=-5),
         {"r4": 5, "cr0": 0xA}, {"r3": 0, "cr0": 0xA}),
    case("addis_lis", "ADDIS", enc("addis", rD=3, rA=0, SIMM=0x1234), {"r0": 7},
         {"r3": 0x12340000}),
    case("addis_lis_negative", "ADDIS", enc("addis", rD=3, rA=0, SIMM=-1), {},
         {"r3": 0xFFFFFFFFFFFF0000}),
    case("addis_sign_extends_to_64", "ADDIS", enc("addis", rD=3, rA=4, SIMM=-0x8000),
         {"r4": 0x80000000}, {"r3": 0}),
    case("addis_adds_ra", "ADDIS", enc("addis", rD=3, rA=4, SIMM=2), {"r4": 0x100000001},
         {"r3": 0x100020001}),
]


# --- add, subf, neg --------------------------------------------------------------------------

def arith(name: str, mnemonic: str, ref: str, a: int, b: int, expect: dict, init=None, **flags):
    """`mnemonic r3,r4,r5` with rA = r4, rB = r5."""
    return case(name, ref, enc(mnemonic, rD=3, rA=4, rB=5, **flags),
                dict(init or {}, r4=a, r5=b), expect)


CASES += [
    arith("add_basic", "add", "ADD", 3, 4, {"r3": 7}),
    arith("add_wraps_64", "add", "ADD", M64, 1, {"r3": 0, "xer_ov": 0}),
    case("add_ra0_is_register_zero", "ADD", enc("add", rD=3, rA=0, rB=5), {"r0": 7, "r5": 1},
         {"r3": 8}),
    case("add_rd_is_ra", "ADD", enc("add", rD=4, rA=4, rB=5), {"r4": 6, "r5": 1}, {"r4": 7}),
    arith("add_rc_negative_only_as_32_bit", "add", "ADD", 0x7FFFFFFF, 1,
          {"r3": 0x80000000, "cr0": GT}, Rc=1),
    arith("add_rc_negative", "add", "ADD", 0, M64, {"r3": M64, "cr0": LT}, Rc=1),
    arith("add_rc_zero", "add", "ADD", 5, M64 - 4, {"r3": 0, "cr0": EQ}, Rc=1),
    arith("add_rc_copies_so", "add", "ADD", 1, 1, {"r3": 2, "cr0": GT | SO}, {"xer_so": 1}, Rc=1),
    arith("addo_overflow", "add", "ADD", MAX64, 1,
          {"r3": MIN64, "xer_ov": 1, "xer_so": 1}, OE=1),
    arith("addo_overflow_negative", "add", "ADD", MIN64, M64,
          {"r3": MAX64, "xer_ov": 1, "xer_so": 1}, OE=1),
    arith("addo_clears_ov_keeps_so", "add", "ADD", 1, 2, {"r3": 3, "xer_ov": 0, "xer_so": 1},
          {"xer_ov": 1, "xer_so": 1}, OE=1),
    arith("addo_32_bit_carry_is_not_overflow", "add", "ADD", 0x7FFFFFFF, 1,
          {"r3": 0x80000000, "xer_ov": 0, "xer_so": 0}, OE=1),
    arith("addo_rc_overflow", "add", "ADD", MAX64, 1,
          {"r3": MIN64, "xer_ov": 1, "xer_so": 1, "cr0": LT | SO}, OE=1, Rc=1),
    case("addo_rd_is_ra", "ADD", enc("add", rD=4, rA=4, rB=5, OE=1), {"r4": MAX64, "r5": 1},
         {"r4": MIN64, "xer_ov": 1}),

    arith("subf_basic", "subf", "SUBF", 3, 10, {"r3": 7}),
    arith("subf_negative", "subf", "SUBF", 10, 3, {"r3": M64 - 6}),
    case("subf_rd_is_rb", "SUBF", enc("subf", rD=5, rA=4, rB=5), {"r4": 3, "r5": 10}, {"r5": 7}),
    arith("subf_rc_zero", "subf", "SUBF", 5, 5, {"r3": 0, "cr0": EQ}, Rc=1),
    arith("subf_rc_negative_only_as_64_bit", "subf", "SUBF", 0x80000000, 0,
          {"r3": 0xFFFFFFFF80000000, "cr0": LT}, Rc=1),
    arith("subfo_overflow_positive_subtrahend", "subf", "SUBF", 1, MIN64,
          {"r3": MAX64, "xer_ov": 1, "xer_so": 1}, OE=1),
    arith("subfo_overflow_negative_subtrahend", "subf", "SUBF", M64, MAX64,
          {"r3": MIN64, "xer_ov": 1, "xer_so": 1}, OE=1),
    arith("subfo_no_overflow", "subf", "SUBF", 0, MIN64, {"r3": MIN64, "xer_ov": 0}, OE=1),
    arith("subfo_clears_ov_keeps_so", "subf", "SUBF", 1, 2,
          {"r3": 1, "xer_ov": 0, "xer_so": 1}, {"xer_ov": 1, "xer_so": 1}, OE=1),
    arith("subfo_rc_overflow", "subf", "SUBF", 1, MIN64,
          {"r3": MAX64, "xer_ov": 1, "cr0": GT | SO}, OE=1, Rc=1),

    case("neg_basic", "NEG", enc("neg", rD=3, rA=4), {"r4": 5}, {"r3": M64 - 4}),
    case("neg_zero", "NEG", enc("neg", rD=3, rA=4), {"r4": 0}, {"r3": 0}),
    case("neg_rd_is_ra", "NEG", enc("neg", rD=4, rA=4), {"r4": 1}, {"r4": M64}),
    case("neg_rc_positive", "NEG", enc("neg", rD=3, rA=4, Rc=1), {"r4": M64}, {"r3": 1, "cr0": GT}),
    case("neg_rc_zero", "NEG", enc("neg", rD=3, rA=4, Rc=1), {"r4": 0}, {"r3": 0, "cr0": EQ}),
    case("nego_int64_min", "NEG", enc("neg", rD=3, rA=4, OE=1), {"r4": MIN64},
         {"r3": MIN64, "xer_ov": 1, "xer_so": 1}),
    case("nego_no_overflow_clears_ov", "NEG", enc("neg", rD=3, rA=4, OE=1),
         {"r4": 1, "xer_ov": 1, "xer_so": 1}, {"r3": M64, "xer_ov": 0, "xer_so": 1}),
    case("nego_rc_int64_min", "NEG", enc("neg", rD=3, rA=4, OE=1, Rc=1), {"r4": MIN64},
         {"r3": MIN64, "xer_ov": 1, "cr0": LT | SO}),
]


# --- logical ---------------------------------------------------------------------------------

LA = 0xFF00FF00FF00FF00
LB = 0x0FF00FF00FF00FF0


def logical(name: str, mnemonic: str, ref: str, value: int, **flags):
    return case(name, ref, enc(mnemonic, rA=3, rS=4, rB=5, **flags), {"r4": LA, "r5": LB},
                {"r3": value})


CASES += [
    logical("and_basic", "and", "AND", 0x0F000F000F000F00),
    logical("andc_basic", "andc", "ANDC", 0xF000F000F000F000),
    logical("or_basic", "or", "OR", 0xFFF0FFF0FFF0FFF0),
    logical("orc_basic", "orc", "ORC", 0xFF0FFF0FFF0FFF0F),
    logical("xor_basic", "xor", "XOR", 0xF0F0F0F0F0F0F0F0),
    logical("nor_basic", "nor", "NOR", 0x000F000F000F000F),
    logical("nand_basic", "nand", "NAND", 0xF0FFF0FFF0FFF0FF),
    logical("eqv_basic", "eqv", "EQV", 0x0F0F0F0F0F0F0F0F),
    case("mr", "OR", asm.mr(3, 4), {"r4": LA}, {"r3": LA}),
    case("mr_rc_negative", "OR", enc("or", rA=3, rS=4, rB=4, Rc=1), {"r4": LA},
         {"r3": LA, "cr0": LT}),
    case("and_rd_is_source", "AND", enc("and", rA=4, rS=4, rB=5), {"r4": LA, "r5": LB},
         {"r4": 0x0F000F000F000F00}),
    case("or_rb_zero_is_register", "OR", enc("or", rA=3, rS=4, rB=0), {"r4": 2, "r0": 1},
         {"r3": 3}),
    case("and_rc_negative_only_as_32_bit", "AND", enc("and", rA=3, rS=4, rB=5, Rc=1),
         {"r4": M64, "r5": 0x80000000}, {"r3": 0x80000000, "cr0": GT}),
    case("and_rc_zero", "AND", enc("and", rA=3, rS=4, rB=5, Rc=1), {"r4": 1, "r5": 2},
         {"r3": 0, "cr0": EQ}),
    case("andc_rc_copies_so", "ANDC", enc("andc", rA=3, rS=4, rB=5, Rc=1),
         {"r4": 3, "r5": 1, "xer_so": 1}, {"r3": 2, "cr0": GT | SO}),
    case("or_rc_negative", "OR", enc("or", rA=3, rS=4, rB=5, Rc=1), {"r4": MIN64, "r5": 0},
         {"r3": MIN64, "cr0": LT}),
    case("orc_rc_zero", "ORC", enc("orc", rA=3, rS=4, rB=5, Rc=1), {"r4": 0, "r5": M64},
         {"r3": 0, "cr0": EQ}),
    case("xor_rc_zero", "XOR", enc("xor", rA=3, rS=4, rB=5, Rc=1), {"r4": LA, "r5": LA},
         {"r3": 0, "cr0": EQ}),
    case("nor_rc_all_ones", "NOR", enc("nor", rA=3, rS=4, rB=5, Rc=1), {"r4": 0, "r5": 0},
         {"r3": M64, "cr0": LT}),
    case("nand_rc_negative", "NAND", enc("nand", rA=3, rS=4, rB=5, Rc=1), {"r4": 0, "r5": 0},
         {"r3": M64, "cr0": LT}),
    case("eqv_rc_negative", "EQV", enc("eqv", rA=3, rS=4, rB=5, Rc=1), {"r4": 5, "r5": 5},
         {"r3": M64, "cr0": LT}),
]


# --- logical immediates ----------------------------------------------------------------------

CASES += [
    case("nop", "ORI", 0x60000000, {"r0": 5, "cr0": 0xA}, {"r0": 5, "cr0": 0xA}),
    case("ori_basic", "ORI", enc("ori", rA=3, rS=4, UIMM=0x1234), {"r4": 0xFFFFFFFF00000000},
         {"r3": 0xFFFFFFFF00001234}),
    case("ori_is_zero_extended", "ORI", enc("ori", rA=3, rS=4, UIMM=0x8000), {"r4": 0},
         {"r3": 0x8000}),
    case("ori_does_not_record", "ORI", enc("ori", rA=3, rS=4, UIMM=0), {"r4": 0, "cr0": 0xA},
         {"r3": 0, "cr0": 0xA}),
    case("ori_not_nop_when_registers_differ", "ORI", enc("ori", rA=3, rS=4, UIMM=0),
         {"r4": 9}, {"r3": 9}),
    case("oris_basic", "ORIS", enc("oris", rA=3, rS=4, UIMM=0xABCD), {"r4": 0},
         {"r3": 0xABCD0000}),
    case("oris_keeps_high_half", "ORIS", enc("oris", rA=3, rS=4, UIMM=0x1), {"r4": M64 - 0xFFFFFFFF},
         {"r3": 0xFFFFFFFF00010000}),
    case("xori_basic", "XORI", enc("xori", rA=3, rS=4, UIMM=0xFFFF), {"r4": M64},
         {"r3": M64 - 0xFFFF}),
    case("xoris_basic", "XORIS", enc("xoris", rA=3, rS=4, UIMM=0x8000), {"r4": 0x80000000},
         {"r3": 0}),
    case("xoris_is_zero_extended", "XORIS", enc("xoris", rA=3, rS=4, UIMM=0x8000), {"r4": 0},
         {"r3": 0x80000000}),
    case("andi_records_positive", "ANDI", enc("andi.", rA=3, rS=4, UIMM=0x8000), {"r4": M64},
         {"r3": 0x8000, "cr0": GT}),
    case("andi_records_zero", "ANDI", enc("andi.", rA=3, rS=4, UIMM=0x00FF), {"r4": 0x100},
         {"r3": 0, "cr0": EQ}),
    case("andi_copies_so", "ANDI", enc("andi.", rA=3, rS=4, UIMM=0x00FF), {"r4": 0x100, "xer_so": 1},
         {"r3": 0, "cr0": EQ | SO}),
    case("andis_negative_only_as_32_bit", "ANDIS", enc("andis.", rA=3, rS=4, UIMM=0x8000),
         {"r4": M64}, {"r3": 0x80000000, "cr0": GT}),
    case("andis_records_zero", "ANDIS", enc("andis.", rA=3, rS=4, UIMM=0x0001), {"r4": 0xFFFF},
         {"r3": 0, "cr0": EQ}),
]


# --- extsb, extsh, extsw, cntlzw, cntlzd -----------------------------------------------------

def unary(name: str, mnemonic: str, ref: str, value: int, expect: dict, **flags):
    return case(name, ref, enc(mnemonic, rA=3, rS=4, **flags), {"r4": value}, expect)


CASES += [
    unary("extsb_negative", "extsb", "EXTSB", 0x80, {"r3": 0xFFFFFFFFFFFFFF80}),
    unary("extsb_positive_drops_upper", "extsb", "EXTSB", 0xFFFFFF7F, {"r3": 0x7F}),
    unary("extsb_rc_negative", "extsb", "EXTSB", 0x80, {"r3": 0xFFFFFFFFFFFFFF80, "cr0": LT},
          Rc=1),
    unary("extsb_rc_zero", "extsb", "EXTSB", 0x100, {"r3": 0, "cr0": EQ}, Rc=1),
    unary("extsh_negative", "extsh", "EXTSH", 0x8000, {"r3": 0xFFFFFFFFFFFF8000}),
    unary("extsh_positive_drops_upper", "extsh", "EXTSH", 0x12345, {"r3": 0x2345}),
    unary("extsh_rc_positive", "extsh", "EXTSH", 0x7FFF, {"r3": 0x7FFF, "cr0": GT}, Rc=1),
    unary("extsw_negative", "extsw", "EXTSW", 0x80000000, {"r3": 0xFFFFFFFF80000000}),
    unary("extsw_positive_drops_upper", "extsw", "EXTSW", 0x17FFFFFFF, {"r3": 0x7FFFFFFF}),
    unary("extsw_rc_negative", "extsw", "EXTSW", 0x80000000,
          {"r3": 0xFFFFFFFF80000000, "cr0": LT}, Rc=1),
    unary("extsw_rc_positive", "extsw", "EXTSW", 0xFFFFFFFF7FFFFFFF, {"r3": 0x7FFFFFFF, "cr0": GT},
          Rc=1),

    case("extsw_rd_is_rs", "EXTSW", enc("extsw", rA=4, rS=4), {"r4": 0x80000000},
         {"r4": 0xFFFFFFFF80000000}),

    unary("cntlzw_zero", "cntlzw", "CNTLZW", 0, {"r3": 32}),
    unary("cntlzw_ignores_upper_word", "cntlzw", "CNTLZW", 0xFFFFFFFF00000000, {"r3": 32}),
    unary("cntlzw_one", "cntlzw", "CNTLZW", 1, {"r3": 31}),
    unary("cntlzw_top_bit", "cntlzw", "CNTLZW", 0x80000000, {"r3": 0}),
    unary("cntlzw_mid", "cntlzw", "CNTLZW", 0x0000FFFF, {"r3": 16}),
    unary("cntlzw_rc_zero_input", "cntlzw", "CNTLZW", 0, {"r3": 32, "cr0": GT}, Rc=1),
    unary("cntlzw_rc_zero_result", "cntlzw", "CNTLZW", 0x80000000, {"r3": 0, "cr0": EQ}, Rc=1),
    unary("cntlzd_zero", "cntlzd", "CNTLZD", 0, {"r3": 64}),
    unary("cntlzd_one", "cntlzd", "CNTLZD", 1, {"r3": 63}),
    unary("cntlzd_top_bit", "cntlzd", "CNTLZD", MIN64, {"r3": 0}),
    unary("cntlzd_upper_word_only", "cntlzd", "CNTLZD", 0x100000000, {"r3": 31}),
    unary("cntlzd_rc_zero_input", "cntlzd", "CNTLZD", 0, {"r3": 64, "cr0": GT}, Rc=1),
    unary("cntlzd_rc_zero_result", "cntlzd", "CNTLZD", MIN64, {"r3": 0, "cr0": EQ}, Rc=1),
]


# --- compares --------------------------------------------------------------------------------

W = 0xFFFFFFFF00000001     # low word 1, negative as a 64-bit value


def compare(name: str, mnemonic: str, ref: str, a: int, nibble: int, b=0, crf=1, init=None, **f):
    if mnemonic in ("cmpi", "cmpli"):
        word = enc(mnemonic, crfD=crf, rA=4, **f)
    else:
        word = enc(mnemonic, crfD=crf, rA=4, rB=5, **f)
    return case(name, ref, word, dict(init or {}, r4=a, r5=b), {"cr%d" % crf: nibble})


CASES += [
    compare("cmpw_ignores_upper_word", "cmp", "CMP", W, EQ, 1, L=0),
    compare("cmpd_uses_all_bits", "cmp", "CMP", W, LT, 1, L=1),
    compare("cmpw_signed_low_word", "cmp", "CMP", 0x80000000, LT, 1, L=0),
    compare("cmpd_greater", "cmp", "CMP", 5, GT, 3, L=1),
    compare("cmpd_equal_high_values", "cmp", "CMP", MIN64, EQ, MIN64, L=1),
    compare("cmpw_copies_so", "cmp", "CMP", 2, GT | SO, 1, L=0, init={"xer_so": 1}),
    compare("cmpd_writes_selected_field_only", "cmp", "CMP", 3, EQ, 3, crf=7, L=1,
            init={"cr0": 0xA}),
    case("cmpd_leaves_other_fields", "CMP", enc("cmp", crfD=7, L=1, rA=4, rB=5),
         {"r4": 3, "r5": 3, "cr0": 0xA, "cr6": 0x5}, {"cr0": 0xA, "cr6": 0x5, "cr7": EQ}),
    compare("cmplw_unsigned_low_word", "cmpl", "CMPL", 0x80000000, GT, 1, L=0),
    compare("cmplw_ignores_upper_word", "cmpl", "CMPL", W, EQ, 1, L=0),
    compare("cmpld_unsigned_64", "cmpl", "CMPL", MIN64, GT, 1, L=1),
    compare("cmpld_less", "cmpl", "CMPL", 1, LT, M64, L=1),
    compare("cmplw_copies_so", "cmpl", "CMPL", 1, LT | SO, 2, L=0, init={"xer_so": 1}),
    case("cmplw_helper_form", "CMPL", asm.cmplw(2, 4, 5), {"r4": 7, "r5": 7}, {"cr2": EQ}),

    compare("cmpwi_ignores_upper_word", "cmpi", "CMPI", W, EQ, L=0, SIMM=1),
    compare("cmpdi_uses_all_bits", "cmpi", "CMPI", W, LT, L=1, SIMM=1),
    compare("cmpwi_minus_one_low_word", "cmpi", "CMPI", 0xFFFFFFFF, EQ, L=0, SIMM=-1),
    compare("cmpdi_minus_one_is_not_low_word", "cmpi", "CMPI", 0xFFFFFFFF, GT, L=1, SIMM=-1),
    compare("cmpdi_minus_one_equal", "cmpi", "CMPI", M64, EQ, L=1, SIMM=-1),
    compare("cmpdi_greater_than_negative", "cmpi", "CMPI", 0, GT, L=1, SIMM=-1),
    compare("cmpwi_less", "cmpi", "CMPI", 4, LT, L=0, SIMM=5),
    compare("cmpwi_copies_so", "cmpi", "CMPI", 5, EQ | SO, L=0, SIMM=5, init={"xer_so": 1}),

    compare("cmplwi_uimm_is_zero_extended", "cmpli", "CMPLI", 0xFFFF, EQ, L=0, UIMM=0xFFFF),
    compare("cmpldi_all_bits", "cmpli", "CMPLI", M64, GT, L=1, UIMM=0xFFFF),
    compare("cmplwi_ignores_upper_word", "cmpli", "CMPLI", 0x100000005, EQ, L=0, UIMM=5),
    compare("cmpldi_upper_word_counts", "cmpli", "CMPLI", 0x100000005, GT, L=1, UIMM=5),
    compare("cmplwi_unsigned_high_word", "cmpli", "CMPLI", 0x80000000, GT, L=0, UIMM=0xFFFF),
    compare("cmpldi_less", "cmpli", "CMPLI", 0, LT, L=1, UIMM=1),
    case("cmplwi_helper_form", "CMPLI", asm.cmplwi(0, 4, 9), {"r4": 9}, {"cr0": EQ}),
]

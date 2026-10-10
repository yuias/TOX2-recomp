"""int_rotate cases: rotate-and-mask, shifts and algebraic shifts.

Expected values are derived by hand from the architecture (IBM bit numbering, bit 0 = MSB) and the
cited RPCS3 handler. All forms are `op r3,r4[,r5]`. CR0 nibbles are lt=8 gt=4 eq=2 so=1.
"""

from tests import ppc_asm as asm

from . import Case

M64 = 0xFFFFFFFFFFFFFFFF
LT, GT, EQ, SO = 8, 4, 2, 1

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

CASES = []


def case(name: str, ref: str, word: int, init=None, expect=None) -> Case:
    return Case(name, REF + ref, (word,), init or {}, expect or {})


# --- rlwinm ----------------------------------------------------------------------------------

def rlwinm(name: str, r4: int, expect: dict, sh: int, mb: int, me: int, **flags) -> Case:
    return case(name, "RLWINM", enc("rlwinm", rA=3, rS=4, SH=sh, MB=mb, ME=me, **flags),
                {"r4": r4}, expect)


CASES += [
    # slwi 8 = rlwinm 8,0,23; the high word of rS does not matter.
    rlwinm("rlwinm_slwi_8", 0xAAAAAAAA12345678, {"r3": 0x34567800}, 8, 0, 23),
    # srwi 8 = rlwinm 24,8,31
    rlwinm("rlwinm_srwi_8", 0xAAAAAAAA12345678, {"r3": 0x00123456}, 24, 8, 31),
    # clrlwi 16 = rlwinm 0,16,31
    rlwinm("rlwinm_clrlwi_16", 0xFFFFFFFF89ABCDEF, {"r3": 0xCDEF}, 0, 16, 31),
    # rotlwi 4: whole word rotated, high half of rD cleared by the mask (32..63).
    rlwinm("rlwinm_rotlwi_4", 0xFFFFFFFF12345678, {"r3": 0x23456781}, 4, 0, 31),
    rlwinm("rlwinm_swap_halves", 0x12345678, {"r3": 0x56781234}, 16, 0, 31),
    rlwinm("rlwinm_sh_31", 1, {"r3": 0x80000000}, 31, 0, 31),
    rlwinm("rlwinm_sh_0_mask_all", 0xFFFFFFFF87654321, {"r3": 0x87654321}, 0, 0, 31),
    # MB > ME: mask(60,35) = 0xfffffffff000000f keeps the duplicated high word.
    rlwinm("rlwinm_wrap_mask_keeps_high_word", 0xA5A5A5A5, {"r3": 0xA5A5A5A5A0000005}, 0, 28, 3),
    # rotl 4 of 1 = 0x10, dup = 0x0000001000000010; & 0xfffffffff000000f = 0x0000001000000000.
    rlwinm("rlwinm_wrap_mask_with_rotate", 0x00000001, {"r3": 0x0000001000000000}, 4, 28, 3),
    # MB = ME + 1: the wrap mask is all ones.
    rlwinm("rlwinm_wrap_mask_all_ones", 0x80000001, {"r3": 0x8000000180000001}, 0, 1, 0),
    # Record uses the 64-bit result: negative here because of the duplicated high word...
    rlwinm("rlwinm_rc_wrap_negative", 0x80000001, {"r3": 0x8000000180000001, "cr0": LT},
           0, 28, 3, Rc=1),
    # ...but a negative low word alone is a positive 64-bit value.
    rlwinm("rlwinm_rc_low_word_negative_is_positive", 0x80000000,
           {"r3": 0x80000000, "cr0": GT}, 0, 0, 31, Rc=1),
    rlwinm("rlwinm_rc_zero", 0xFFFFFFFF00000000, {"r3": 0, "cr0": EQ}, 0, 0, 31, Rc=1),
    case("rlwinm_rc_copies_so", "RLWINM", enc("rlwinm", rA=3, rS=4, SH=0, MB=0, ME=31, Rc=1),
         {"r4": 5, "xer_so": 1}, {"r3": 5, "cr0": GT | SO}),
    case("rlwinm_ra_is_rs", "RLWINM", enc("rlwinm", rA=4, rS=4, SH=8, MB=0, ME=23),
         {"r4": 0x12345678}, {"r4": 0x34567800}),
]


# --- rlwnm -----------------------------------------------------------------------------------

def rlwnm(name: str, r4: int, r5: int, expect: dict, mb: int, me: int, **flags) -> Case:
    return case(name, "RLWNM", enc("rlwnm", rA=3, rS=4, rB=5, MB=mb, ME=me, **flags),
                {"r4": r4, "r5": r5}, expect)


CASES += [
    rlwnm("rlwnm_rotate_8", 0x12345678, 8, {"r3": 0x34567812}, 0, 31),
    # Only the low 5 bits of rB count: 36 -> 4, 32 -> 0.
    rlwnm("rlwnm_count_masked_to_5_bits", 0x12345678, 0xFFFFFF24, {"r3": 0x23456781}, 0, 31),
    rlwnm("rlwnm_count_32_is_0", 0x12345678, 32, {"r3": 0x12345678}, 0, 31),
    rlwnm("rlwnm_wrap_mask", 0xA5A5A5A5, 0, {"r3": 0xA5A5A5A5A0000005}, 28, 3),
    rlwnm("rlwnm_rc", 0x12345678, 4, {"r3": 0x23456781, "cr0": GT}, 0, 31, Rc=1),
]


# --- rlwimi ----------------------------------------------------------------------------------

def rlwimi(name: str, r3: int, r4: int, expect: dict, sh: int, mb: int, me: int, **flags) -> Case:
    return case(name, "RLWIMI", enc("rlwimi", rA=3, rS=4, SH=sh, MB=mb, ME=me, **flags),
                {"r3": r3, "r4": r4}, expect)


CASES += [
    # mask(48,55) = 0xff00; rotl 8 of 0xab = 0xab00.
    rlwimi("rlwimi_insert_byte", 0x1111111111111111, 0xAB, {"r3": 0x111111111111AB11}, 8, 16, 23),
    # Wrap mask: high word comes from the duplicated rotated word, ~mask = 0x0ffffff0 is kept.
    rlwimi("rlwimi_wrap_mask", M64, 0x12345678, {"r3": 0x123456781FFFFFF8}, 0, 28, 3),
    rlwimi("rlwimi_rc_negative", 0xFFFFFFFF00000000, 0x12,
           {"r3": 0xFFFFFFFF00000012, "cr0": LT}, 0, 0, 31, Rc=1),
    case("rlwimi_ra_is_rs", "RLWIMI", enc("rlwimi", rA=3, rS=3, SH=16, MB=0, ME=15),
         {"r3": 0x12345678}, {"r3": 0x56785678}),
]


# --- rldicl ----------------------------------------------------------------------------------

V = 0x1234567890ABCDEF


def rldicl(name: str, r4: int, expect: dict, sh: int, mb: int, **flags) -> Case:
    return case(name, "RLDICL", enc("rldicl", rA=3, rS=4, SH64=sh, MB64=mb, **flags),
                {"r4": r4}, expect)


CASES += [
    rldicl("rldicl_clrldi_32", V, {"r3": 0x90ABCDEF}, 0, 32),
    rldicl("rldicl_srdi_8", V, {"r3": 0x001234567890ABCD}, 56, 8),
    rldicl("rldicl_rotldi_4", V, {"r3": 0x234567890ABCDEF1}, 4, 0),
    rldicl("rldicl_sh_63", 1, {"r3": 0x8000000000000000}, 63, 0),
    rldicl("rldicl_sh_0_mb_0_is_mr", V, {"r3": V}, 0, 0),
    # SH64 = 40 (sh5 set) rotates by 5 bytes, MB64 = 48 (mb5 set) keeps the low 16 bits.
    rldicl("rldicl_split_sh_and_mb", V, {"r3": 0x7890}, 40, 48),
    rldicl("rldicl_mb_63", 3, {"r3": 1}, 0, 63),
    rldicl("rldicl_rc_positive", M64, {"r3": 0x7FFFFFFFFFFFFFFF, "cr0": GT}, 0, 1, Rc=1),
    rldicl("rldicl_rc_zero", 0x8000000000000000, {"r3": 0, "cr0": EQ}, 0, 1, Rc=1),
    rldicl("rldicl_rc_negative", 0x8000000000000000, {"r3": 0x8000000000000000, "cr0": LT},
           0, 0, Rc=1),
]


# --- rldicr ----------------------------------------------------------------------------------

def rldicr(name: str, r4: int, expect: dict, sh: int, me: int, **flags) -> Case:
    return case(name, "RLDICR", enc("rldicr", rA=3, rS=4, SH64=sh, MB64=me, **flags),
                {"r4": r4}, expect)


CASES += [
    # sldi 8 = rldicr 8,55
    rldicr("rldicr_sldi_8", V, {"r3": 0x34567890ABCDEF00}, 8, 55),
    rldicr("rldicr_sldi_40", V, {"r3": 0xABCDEF0000000000}, 40, 23),
    rldicr("rldicr_sh_0_me_63", V, {"r3": V}, 0, 63),
    # ME = 40 needs the sixth bit of the split field: the top 41 bits stay.
    rldicr("rldicr_split_me", M64, {"r3": 0xFFFFFFFFFF800000}, 0, 40),
    # rotl 56 of 0xf0 = 0xf000000000000000; mask(0,3) keeps the top nibble.
    rldicr("rldicr_rc_negative", 0xF0, {"r3": 0xF000000000000000, "cr0": LT}, 56, 3, Rc=1),
    rldicr("rldicr_rc_positive", V, {"r3": 0x234567890ABCDEF0, "cr0": GT}, 4, 59, Rc=1),
]


# --- rldic -----------------------------------------------------------------------------------

def rldic(name: str, r4: int, expect: dict, sh: int, mb: int, **flags) -> Case:
    return case(name, "RLDIC", enc("rldic", rA=3, rS=4, SH64=sh, MB64=mb, **flags),
                {"r4": r4}, expect)


CASES += [
    # mask(48,55) = 0xff00; rotl 8 of V ends in ...ef12.
    rldic("rldic_clrlsldi", V, {"r3": 0xEF00}, 8, 48),
    # sh = 60, mb = 8: wrapping mask(8,3) = 0xf0ffffffffffffff over rotr 4 = 0xf1234567890abcde.
    rldic("rldic_wrap_mask", V, {"r3": 0xF0234567890ABCDE}, 60, 8),
    rldic("rldic_sh_0_mb_0", V, {"r3": V}, 0, 0),
    rldic("rldic_rc_negative", V, {"r3": 0xF0234567890ABCDE, "cr0": LT}, 60, 8, Rc=1),
]


# --- rldimi ----------------------------------------------------------------------------------

def rldimi(name: str, r3: int, r4: int, expect: dict, sh: int, mb: int, **flags) -> Case:
    return case(name, "RLDIMI", enc("rldimi", rA=3, rS=4, SH64=sh, MB64=mb, **flags),
                {"r3": r3, "r4": r4}, expect)


CASES += [
    # mask(32,47) = 0xffff0000
    rldimi("rldimi_insert", M64, 0x1234, {"r3": 0xFFFFFFFF1234FFFF}, 16, 32),
    # sh = 0, mb = 60: replaces the low nibble only.
    rldimi("rldimi_low_nibble", M64, 0x5, {"r3": 0xFFFFFFFFFFFFFFF5}, 0, 60),
    # Wrapping mask(8,3): the kept bits of rA are 0x0f00000000000000.
    rldimi("rldimi_wrap_mask", M64, V, {"r3": 0xFF234567890ABCDE}, 60, 8),
    rldimi("rldimi_split_sh", 0, 0x00000000000000FF, {"r3": 0x000000FF00000000}, 32, 24),
    rldimi("rldimi_rc_zero", 0, 0, {"r3": 0, "cr0": EQ}, 8, 48, Rc=1),
]


# --- rldcl, rldcr ----------------------------------------------------------------------------

def rldcl(name: str, r4: int, r5: int, expect: dict, mb: int, **flags) -> Case:
    return case(name, "RLDCL", enc("rldcl", rA=3, rS=4, rB=5, MB64=mb, **flags),
                {"r4": r4, "r5": r5}, expect)


def rldcr(name: str, r4: int, r5: int, expect: dict, me: int, **flags) -> Case:
    return case(name, "RLDCR", enc("rldcr", rA=3, rS=4, rB=5, MB64=me, **flags),
                {"r4": r4, "r5": r5}, expect)


CASES += [
    rldcl("rldcl_rotate_8", V, 8, {"r3": 0x34567890ABCDEF12}, 0),
    # Only the low 6 bits of rB count: 0x48 -> 8.
    rldcl("rldcl_count_masked_to_6_bits", V, 0x48, {"r3": 0x34567890ABCDEF12}, 0),
    rldcl("rldcl_clear_high_word", V, 0, {"r3": 0x90ABCDEF}, 32),
    rldcl("rldcl_rc", V, 0, {"r3": 0x90ABCDEF, "cr0": GT}, 32, Rc=1),
    rldcr("rldcr_rotate_8_keep_top_56", V, 8, {"r3": 0x34567890ABCDEF00}, 55),
    rldcr("rldcr_count_masked_to_6_bits", V, 0x88, {"r3": 0x34567890ABCDEF00}, 55),
    rldcr("rldcr_rc", V, 4, {"r3": 0x234567890ABCDEF0, "cr0": GT}, 59, Rc=1),
]


# --- slw, srw, sld, srd ----------------------------------------------------------------------

def shift(name: str, mnemonic: str, ref: str, r4: int, r5: int, expect: dict, **flags) -> Case:
    return case(name, ref, enc(mnemonic, rA=3, rS=4, rB=5, **flags), {"r4": r4, "r5": r5}, expect)


CASES += [
    shift("slw_by_1_truncates", "slw", "SLW", 0x80000001, 1, {"r3": 2}),
    shift("slw_by_31", "slw", "SLW", 3, 31, {"r3": 0x80000000}),
    shift("slw_by_0", "slw", "SLW", 0xFFFFFFFF12345678, 0, {"r3": 0x12345678}),
    shift("slw_ignores_high_word", "slw", "SLW", 0xFFFFFFFF00000001, 4, {"r3": 0x10}),
    shift("slw_by_32_is_0", "slw", "SLW", 1, 32, {"r3": 0}),
    shift("slw_by_63_is_0", "slw", "SLW", M64, 63, {"r3": 0}),
    # 64 = 0x40: bit 0x20 is clear and the low five bits are 0, so this shifts by 0.
    shift("slw_by_64_shifts_by_0", "slw", "SLW", 3, 64, {"r3": 3}),
    shift("slw_count_uses_6_bits", "slw", "SLW", 3, 0x41, {"r3": 6}),
    shift("slw_rc_positive_64_bit", "slw", "SLW", 1, 31, {"r3": 0x80000000, "cr0": GT}, Rc=1),
    case("slw_rd_is_rs", "SLW", enc("slw", rA=4, rS=4, rB=5), {"r4": 1, "r5": 4}, {"r4": 0x10}),

    shift("srw_by_4", "srw", "SRW", 0x12345678, 4, {"r3": 0x01234567}),
    shift("srw_by_31_ignores_high_word", "srw", "SRW", 0xFFFFFFFF80000000, 31, {"r3": 1}),
    shift("srw_by_0_zero_extends", "srw", "SRW", 0xFFFFFFFF12345678, 0, {"r3": 0x12345678}),
    shift("srw_by_32_is_0", "srw", "SRW", M64, 32, {"r3": 0}),
    shift("srw_count_uses_6_bits", "srw", "SRW", 0x10, 0x41, {"r3": 8}),
    shift("srw_rc_zero", "srw", "SRW", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),

    shift("sld_by_4", "sld", "SLD", 0x1234, 4, {"r3": 0x12340}),
    shift("sld_by_63", "sld", "SLD", 1, 63, {"r3": 0x8000000000000000}),
    shift("sld_by_64_is_0", "sld", "SLD", 1, 64, {"r3": 0}),
    shift("sld_by_127_is_0", "sld", "SLD", M64, 0x7F, {"r3": 0}),
    shift("sld_by_128_shifts_by_0", "sld", "SLD", V, 0x80, {"r3": V}),
    shift("sld_rc_negative", "sld", "SLD", 1, 63, {"r3": 0x8000000000000000, "cr0": LT}, Rc=1),

    shift("srd_by_4", "srd", "SRD", 0xF000000000000000, 4, {"r3": 0x0F00000000000000}),
    shift("srd_by_63", "srd", "SRD", 0x8000000000000000, 63, {"r3": 1}),
    shift("srd_by_64_is_0", "srd", "SRD", M64, 64, {"r3": 0}),
    shift("srd_by_0", "srd", "SRD", V, 0, {"r3": V}),
    shift("srd_by_128_shifts_by_0", "srd", "SRD", V, 0x80, {"r3": V}),
    shift("srd_rc_zero", "srd", "SRD", 1, 1, {"r3": 0, "cr0": EQ}, Rc=1),
]


# --- sraw, srawi -----------------------------------------------------------------------------

NEG1 = M64


def sraw(name: str, r4: int, r5: int, expect: dict, init=None, **flags) -> Case:
    return case(name, "SRAW", enc("sraw", rA=3, rS=4, rB=5, **flags),
                dict(init or {}, r4=r4, r5=r5), expect)


def srawi(name: str, r4: int, sh: int, expect: dict, init=None, **flags) -> Case:
    return case(name, "SRAWI", enc("srawi", rA=3, rS=4, SH=sh, **flags),
                dict(init or {}, r4=r4), expect)


CASES += [
    sraw("sraw_exact_negative_no_ca", 0xFFFFFFF0, 4, {"r3": NEG1, "xer_ca": 0}),
    sraw("sraw_inexact_negative_sets_ca", 0xFFFFFFF1, 1, {"r3": M64 - 7, "xer_ca": 1}),
    sraw("sraw_positive_clears_ca", 0x7FFFFFFF, 4, {"r3": 0x07FFFFFF, "xer_ca": 0},
         {"xer_ca": 1}),
    sraw("sraw_ignores_high_word", 0x12345678_80000000, 31, {"r3": NEG1, "xer_ca": 0}),
    sraw("sraw_by_0_sign_extends", 0x80000000, 0, {"r3": 0xFFFFFFFF80000000, "xer_ca": 0},
         {"xer_ca": 1}),
    sraw("sraw_by_32_negative", 0x80000000, 32, {"r3": NEG1, "xer_ca": 1}),
    sraw("sraw_by_32_positive", 0x7FFFFFFF, 32, {"r3": 0, "xer_ca": 0}, {"xer_ca": 1}),
    sraw("sraw_by_40_negative", 0xFFFFFFFF, 40, {"r3": NEG1, "xer_ca": 1}),
    sraw("sraw_by_63_negative", 0x80000000, 63, {"r3": NEG1, "xer_ca": 1}),
    # 64 & 63 = 0: no shift.
    sraw("sraw_by_64_shifts_by_0", 0x80000000, 64, {"r3": 0xFFFFFFFF80000000, "xer_ca": 0}),
    sraw("sraw_rc_negative", 0x80000000, 0, {"r3": 0xFFFFFFFF80000000, "cr0": LT}, Rc=1),
    sraw("sraw_rc_zero", 0x00000001, 1, {"r3": 0, "cr0": EQ}, Rc=1),
    case("sraw_rd_is_rs", "SRAW", enc("sraw", rA=4, rS=4, rB=5), {"r4": 0xFFFFFFF1, "r5": 1},
         {"r4": M64 - 7, "xer_ca": 1}),

    srawi("srawi_minus_1_by_1_sets_ca", 0xFFFFFFFF, 1, {"r3": NEG1, "xer_ca": 1}),
    srawi("srawi_negative_even_clears_ca", 0xFFFFFFFC, 1, {"r3": M64 - 1, "xer_ca": 0}, {"xer_ca": 1}),
    srawi("srawi_minus_2_by_1", 0xFFFFFFFE, 1, {"r3": NEG1, "xer_ca": 0}),
    srawi("srawi_positive_clears_ca", 5, 1, {"r3": 2, "xer_ca": 0}, {"xer_ca": 1}),
    srawi("srawi_by_0", 0x80000000, 0, {"r3": 0xFFFFFFFF80000000, "xer_ca": 0}, {"xer_ca": 1}),
    srawi("srawi_by_31_inexact", 0x80000001, 31, {"r3": NEG1, "xer_ca": 1}),
    srawi("srawi_by_31_exact", 0x80000000, 31, {"r3": NEG1, "xer_ca": 0}),
    srawi("srawi_ignores_high_word", 0xFFFFFFFF00000010, 4, {"r3": 1, "xer_ca": 0}),
    srawi("srawi_rc_zero", 0xF, 4, {"r3": 0, "xer_ca": 0, "cr0": EQ}, Rc=1),
    srawi("srawi_rc_negative", 0x80000000, 4, {"r3": 0xFFFFFFFFF8000000, "xer_ca": 0, "cr0": LT},
          Rc=1),
    case("srawi_rd_is_rs", "SRAWI", enc("srawi", rA=3, rS=3, SH=1), {"r3": 0xFFFFFFFF},
         {"r3": NEG1, "xer_ca": 1}),
]


# --- srad, sradi -----------------------------------------------------------------------------

MIN64 = 0x8000000000000000


def srad(name: str, r4: int, r5: int, expect: dict, init=None, **flags) -> Case:
    return case(name, "SRAD", enc("srad", rA=3, rS=4, rB=5, **flags),
                dict(init or {}, r4=r4, r5=r5), expect)


def sradi(name: str, r4: int, sh: int, expect: dict, init=None, **flags) -> Case:
    return case(name, "SRADI", enc("sradi", rA=3, rS=4, SH64=sh, **flags),
                dict(init or {}, r4=r4), expect)


CASES += [
    srad("srad_by_63_exact", MIN64, 63, {"r3": NEG1, "xer_ca": 0}),
    srad("srad_inexact_sets_ca", MIN64 | 1, 1, {"r3": 0xC000000000000000, "xer_ca": 1}),
    srad("srad_positive_clears_ca", 0x100, 4, {"r3": 0x10, "xer_ca": 0}, {"xer_ca": 1}),
    srad("srad_by_64_negative", MIN64, 64, {"r3": NEG1, "xer_ca": 1}),
    srad("srad_by_100_positive", 0x7FFFFFFFFFFFFFFF, 100, {"r3": 0, "xer_ca": 0}, {"xer_ca": 1}),
    srad("srad_by_127_negative", V | MIN64, 0x7F, {"r3": NEG1, "xer_ca": 1}),
    # 128 & 127 = 0: no shift.
    srad("srad_by_128_shifts_by_0", MIN64 | 5, 0x80, {"r3": MIN64 | 5, "xer_ca": 0}),
    srad("srad_rc_negative", MIN64, 1, {"r3": 0xC000000000000000, "xer_ca": 0, "cr0": LT}, Rc=1),

    sradi("sradi_by_63_negative_exact", MIN64, 63, {"r3": NEG1, "xer_ca": 0}),
    sradi("sradi_by_63_minus_1", M64, 63, {"r3": NEG1, "xer_ca": 1}),
    sradi("sradi_by_63_positive", 0x7FFFFFFFFFFFFFFF, 63, {"r3": 0, "xer_ca": 0}, {"xer_ca": 1}),
    sradi("sradi_negative_odd_by_1", M64 - 2, 1, {"r3": M64 - 1, "xer_ca": 1}),
    sradi("sradi_by_0", M64 - 2, 0, {"r3": M64 - 2, "xer_ca": 0}, {"xer_ca": 1}),
    # SH64 = 33 uses the split bit; bit 32 of -2^32 is a shifted-out 1.
    sradi("sradi_split_sh_negative", 0xFFFFFFFF00000000, 33, {"r3": NEG1, "xer_ca": 1}),
    sradi("sradi_split_sh_positive", 0x0123456789ABCDEF, 40, {"r3": 0x12345, "xer_ca": 0}),
    sradi("sradi_rc_negative", 0xF000000000000000, 4,
          {"r3": 0xFF00000000000000, "xer_ca": 0, "cr0": LT}, Rc=1),
    sradi("sradi_rc_zero", 7, 4, {"r3": 0, "xer_ca": 0, "cr0": EQ}, Rc=1),
]

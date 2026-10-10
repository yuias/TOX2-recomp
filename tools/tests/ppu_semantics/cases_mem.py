"""mem cases: integer and FP loads and stores, update, indexed and byte-reversed forms, lmw/stmw.

Expected values are derived by hand from the architecture and the cited RPCS3 handlers. Guest data
lives at BASE; `r4` is the usual base register. `WIDE` is BASE with bit 32 set: effective addresses
truncate to 32 bits, but update forms write the untruncated sum back.
"""

from tests import ppc_asm as asm

from . import Case

M64 = 0xFFFFFFFFFFFFFFFF
BASE = 0x20000
WIDE = 0x100000000 + BASE

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode


def case(name: str, ref: str, words, init=None, expect=None, mem_init=None, mem_expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {}, mem_init or {},
                mem_expect or {})


def hexb(text: str) -> bytes:
    return bytes.fromhex(text)


def patched(offset: int, data: bytes, size: int = 16) -> bytes:
    """`size` bytes of 0xff with `data` written at `offset` (a store leaves the rest untouched)."""
    out = bytearray(b"\xff" * size)
    out[offset:offset + len(data)] = data
    return bytes(out)


FILL = {BASE: b"\xff" * 16}
# Eight bytes at BASE + 8 with distinct, sign-bit-set leading bytes.
DATA8 = hexb("8182838485868788")
DATA = {BASE + 8: DATA8}

CASES = []


# --- integer loads, D and DS forms ------------------------------------------------------------

# rD starts all-ones so that a missing zero or sign extension is visible.
CASES += [
    case("lbz_zero_extends", "LBZ", enc("lbz", rD=3, rA=4, D=8), {"r3": M64, "r4": BASE},
         {"r3": 0x81}, DATA),
    case("lhz_zero_extends", "LHZ", enc("lhz", rD=3, rA=4, D=8), {"r3": M64, "r4": BASE},
         {"r3": 0x8182}, DATA),
    case("lha_sign_extends", "LHA", enc("lha", rD=3, rA=4, D=8), {"r3": 0, "r4": BASE},
         {"r3": 0xFFFFFFFFFFFF8182}, DATA),
    case("lha_positive", "LHA", enc("lha", rD=3, rA=4, D=0), {"r3": M64, "r4": BASE},
         {"r3": 0x7FFE}, {BASE: hexb("7ffe")}),
    case("lwz_zero_extends", "LWZ", enc("lwz", rD=3, rA=4, D=8), {"r3": M64, "r4": BASE},
         {"r3": 0x81828384}, DATA),
    case("lwa_sign_extends", "LWA", enc("lwa", rD=3, rA=4, DS=8), {"r3": 0, "r4": BASE},
         {"r3": 0xFFFFFFFF81828384}, DATA),
    case("lwa_positive", "LWA", enc("lwa", rD=3, rA=4, DS=0), {"r3": M64, "r4": BASE},
         {"r3": 0x7FFFFFFF}, {BASE: hexb("7fffffff")}),
    case("ld_loads_64_bits", "LD", enc("ld", rD=3, rA=4, DS=8), {"r3": 0, "r4": BASE},
         {"r3": 0x8182838485868788}, DATA),
    case("lwz_is_big_endian", "LWZ", enc("lwz", rD=3, rA=4, D=0), {"r4": BASE},
         {"r3": 0x12345678}, {BASE: hexb("12345678")}),
    case("lwz_negative_displacement", "LWZ", enc("lwz", rD=3, rA=4, D=-8), {"r4": BASE + 0x10},
         {"r3": 0x81828384}, DATA),
    case("ld_negative_displacement", "LD", enc("ld", rD=3, rA=4, DS=-16), {"r4": BASE + 0x18},
         {"r3": 0x8182838485868788}, DATA),
    case("lwz_unaligned", "LWZ", enc("lwz", rD=3, rA=4, D=1), {"r4": BASE},
         {"r3": 0x22334455}, {BASE: hexb("1122334455667788")}),
    case("ld_unaligned", "LD", enc("ld", rD=3, rA=4, DS=4), {"r4": BASE - 3},
         {"r3": 0x2233445566778899}, {BASE: hexb("112233445566778899")}),
    case("lwz_rd_is_ra", "LWZ", enc("lwz", rD=3, rA=3, D=0), {"r3": BASE},
         {"r3": 0xCAFEBABE}, {BASE: hexb("cafebabe")}),
    case("lwz_truncates_ea_to_32_bits", "LWZ", enc("lwz", rD=3, rA=4, D=8), {"r4": WIDE},
         {"r3": 0x81828384}, DATA),
    case("lwz_ra0_is_a_literal", "LWZ", enc("lwz", rD=3, rA=0, D=0x100),
         {"r0": BASE, "r3": 0x55}, {"r3": 0}, {BASE + 0x100: hexb("deadbeef")}),
    case("ld_ra0_is_a_literal", "LD", enc("ld", rD=3, rA=0, DS=0x100),
         {"r0": BASE, "r3": 0x55}, {"r3": 0}, {BASE + 0x100: hexb("deadbeefdeadbeef")}),
]


# --- integer loads, update forms --------------------------------------------------------------

CASES += [
    case("lbzu_updates_ra", "LBZU", enc("lbzu", rD=3, rA=4, D=8), {"r3": M64, "r4": WIDE},
         {"r3": 0x81, "r4": WIDE + 8}, DATA),
    case("lhzu_updates_ra", "LHZU", enc("lhzu", rD=3, rA=4, D=8), {"r3": M64, "r4": WIDE},
         {"r3": 0x8182, "r4": WIDE + 8}, DATA),
    case("lhau_updates_ra", "LHAU", enc("lhau", rD=3, rA=4, D=8), {"r4": WIDE},
         {"r3": 0xFFFFFFFFFFFF8182, "r4": WIDE + 8}, DATA),
    case("lwzu_updates_ra", "LWZU", enc("lwzu", rD=3, rA=4, D=8), {"r3": M64, "r4": WIDE},
         {"r3": 0x81828384, "r4": WIDE + 8}, DATA),
    case("lwzu_negative_displacement", "LWZU", enc("lwzu", rD=3, rA=4, D=-8),
         {"r4": BASE + 0x10}, {"r3": 0x81828384, "r4": BASE + 8}, DATA),
    case("lwzu_ra_wins_when_rd_is_ra", "LWZU", enc("lwzu", rD=4, rA=4, D=8), {"r4": WIDE},
         {"r4": WIDE + 8}, DATA),
    case("ldu_updates_ra", "LDU", enc("ldu", rD=3, rA=4, DS=8), {"r4": WIDE},
         {"r3": 0x8182838485868788, "r4": WIDE + 8}, DATA),
    case("ldu_negative_displacement", "LDU", enc("ldu", rD=3, rA=4, DS=-8),
         {"r4": BASE + 0x10}, {"r3": 0x8182838485868788, "r4": BASE + 8}, DATA),
]


# --- integer loads, indexed forms -------------------------------------------------------------

def indexed(name: str, mnemonic: str, ref: str, expect: dict, *, init=None, update=False,
            mem_init=None):
    """`mnemonic r3,r4,r5` with r5 = 8; the effective address is r4 + 8."""
    init = dict({"r3": M64, "r4": BASE, "r5": 8}, **(init or {}))
    if update:
        expect = dict(expect, r4=init["r4"] + 8)
    return case(name, ref, enc(mnemonic, rD=3, rA=4, rB=5), init, expect, mem_init or DATA)


CASES += [
    indexed("lbzx_zero_extends", "lbzx", "LBZX", {"r3": 0x81}),
    indexed("lhzx_zero_extends", "lhzx", "LHZX", {"r3": 0x8182}),
    indexed("lhax_sign_extends", "lhax", "LHAX", {"r3": 0xFFFFFFFFFFFF8182}),
    indexed("lwzx_zero_extends", "lwzx", "LWZX", {"r3": 0x81828384}),
    indexed("lwax_sign_extends", "lwax", "LWAX", {"r3": 0xFFFFFFFF81828384}),
    indexed("ldx_loads_64_bits", "ldx", "LDX", {"r3": 0x8182838485868788}),
    indexed("lwzx_truncates_ea_to_32_bits", "lwzx", "LWZX", {"r3": 0x81828384},
            init={"r4": 0xFFFFFFFF00010000, "r5": 0x10008}),
    indexed("lbzux_updates_ra", "lbzux", "LBZUX", {"r3": 0x81}, init={"r4": WIDE}, update=True),
    indexed("lhzux_updates_ra", "lhzux", "LHZUX", {"r3": 0x8182}, init={"r4": WIDE}, update=True),
    indexed("lhaux_updates_ra", "lhaux", "LHAUX", {"r3": 0xFFFFFFFFFFFF8182},
            init={"r4": WIDE}, update=True),
    indexed("lwzux_updates_ra", "lwzux", "LWZUX", {"r3": 0x81828384}, init={"r4": WIDE},
            update=True),
    indexed("lwaux_updates_ra", "lwaux", "LWAUX", {"r3": 0xFFFFFFFF81828384},
            init={"r4": WIDE}, update=True),
    indexed("ldux_updates_ra", "ldux", "LDUX", {"r3": 0x8182838485868788}, init={"r4": WIDE},
            update=True),
    case("lwzx_ra0_uses_rb_only", "LWZX", enc("lwzx", rD=3, rA=0, rB=5),
         {"r0": BASE + 0x100, "r5": BASE + 8}, {"r3": 0x81828384}, DATA),
    case("lwzux_rd_is_ra", "LWZUX", enc("lwzux", rD=4, rA=4, rB=5), {"r4": WIDE, "r5": 8},
         {"r4": WIDE + 8}, DATA),
]


# --- byte-reversed loads and stores -----------------------------------------------------------

REVERSED = hexb("0102030405060708")

CASES += [
    case("lhbrx_reverses_bytes", "LHBRX", enc("lhbrx", rD=3, rA=4, rB=5),
         {"r3": M64, "r4": BASE, "r5": 0}, {"r3": 0x0201}, {BASE: REVERSED}),
    case("lwbrx_reverses_bytes", "LWBRX", enc("lwbrx", rD=3, rA=4, rB=5),
         {"r3": M64, "r4": BASE, "r5": 0}, {"r3": 0x04030201}, {BASE: REVERSED}),
    case("ldbrx_reverses_bytes", "LDBRX", enc("ldbrx", rD=3, rA=4, rB=5),
         {"r3": 0, "r4": BASE, "r5": 0}, {"r3": 0x0807060504030201}, {BASE: REVERSED}),
    case("lwbrx_ea_is_ra_plus_rb", "LWBRX", enc("lwbrx", rD=3, rA=4, rB=5),
         {"r4": BASE, "r5": 4}, {"r3": 0x08070605}, {BASE: REVERSED}),
    case("sthbrx_reverses_bytes", "STHBRX", enc("sthbrx", rS=3, rA=4, rB=5),
         {"r3": 0xAABBCCDD, "r4": BASE, "r5": 2}, {}, FILL, {BASE: patched(2, hexb("ddcc"))}),
    case("stwbrx_reverses_bytes", "STWBRX", enc("stwbrx", rS=3, rA=4, rB=5),
         {"r3": 0xAABBCCDD11223344, "r4": BASE, "r5": 4}, {}, FILL,
         {BASE: patched(4, hexb("44332211"))}),
    case("stdbrx_reverses_bytes", "STDBRX", enc("stdbrx", rS=3, rA=4, rB=5),
         {"r3": 0x0102030405060708, "r4": BASE, "r5": 8}, {}, FILL,
         {BASE: patched(8, hexb("0807060504030201"))}),
]


# --- integer stores ---------------------------------------------------------------------------

VALUE = 0xA1A2A3A4A5A6A7A8


def store(name: str, mnemonic: str, ref: str, offset: int, data: bytes, *, init=None,
          expect=None, **fields) -> Case:
    """`mnemonic r3` with r4 = BASE (unless `init` overrides it); memory starts as 0xff bytes."""
    init = dict({"r3": VALUE, "r4": BASE}, **(init or {}))
    return case(name, ref, enc(mnemonic, rS=3, rA=4, **fields), init, expect, FILL,
                {BASE: patched(offset, data)})


CASES += [
    store("stb_stores_low_byte", "stb", "STB", 3, hexb("a8"), D=3),
    store("sth_stores_low_half", "sth", "STH", 2, hexb("a7a8"), D=2),
    store("stw_stores_low_word", "stw", "STW", 4, hexb("a5a6a7a8"), D=4),
    store("std_stores_64_bits", "std", "STD", 8, hexb("a1a2a3a4a5a6a7a8"), DS=8),
    store("stw_negative_displacement", "stw", "STW", 4, hexb("a5a6a7a8"), D=-4,
          init={"r4": BASE + 8}),
    store("std_negative_displacement", "std", "STD", 0, hexb("a1a2a3a4a5a6a7a8"), DS=-8,
          init={"r4": BASE + 8}),
    store("stw_unaligned", "stw", "STW", 1, hexb("a5a6a7a8"), D=1),
    store("std_unaligned", "std", "STD", 3, hexb("a1a2a3a4a5a6a7a8"), DS=4, init={"r4": BASE - 1}),
    store("stw_truncates_ea_to_32_bits", "stw", "STW", 4, hexb("a5a6a7a8"), D=4,
          init={"r4": WIDE}),
    # The literal EA 0x100 is outside the data window; r0 + 0x100 would overwrite the fill.
    case("stw_ra0_is_a_literal", "STW", enc("stw", rS=3, rA=0, D=0x100),
         {"r0": BASE, "r3": 0x11223344}, {}, {BASE + 0x100: b"\xff" * 4},
         {BASE + 0x100: b"\xff" * 4}),
]

CASES += [
    store("stbu_updates_ra", "stbu", "STBU", 3, hexb("a8"), D=3,
          expect={"r4": WIDE + 3}, init={"r4": WIDE}),
    store("sthu_updates_ra", "sthu", "STHU", 2, hexb("a7a8"), D=2,
          expect={"r4": WIDE + 2}, init={"r4": WIDE}),
    store("stwu_updates_ra", "stwu", "STWU", 4, hexb("a5a6a7a8"), D=4,
          expect={"r4": WIDE + 4}, init={"r4": WIDE}),
    store("stdu_updates_ra", "stdu", "STDU", 8, hexb("a1a2a3a4a5a6a7a8"), DS=8,
          expect={"r4": WIDE + 8}, init={"r4": WIDE}),
    case("stwu_stores_old_ra_when_rs_is_ra", "STWU", enc("stwu", rS=4, rA=4, D=4),
         {"r4": WIDE}, {"r4": WIDE + 4}, FILL, {BASE: patched(4, hexb("00020000"))}),
    case("stdu_stack_frame_push", "STDU", enc("stdu", rS=1, rA=1, DS=-0x20), {"r1": BASE + 0x100},
         {"r1": BASE + 0xE0}, {BASE + 0xE0: b"\xff" * 16},
         {BASE + 0xE0: patched(0, hexb("0000000000020100"))}),
]


def store_indexed(name: str, mnemonic: str, ref: str, offset: int, data: bytes, *,
                  update=False, init=None) -> Case:
    """`mnemonic r3,r4,r5` with r5 = offset."""
    init = dict({"r3": VALUE, "r4": BASE, "r5": offset}, **(init or {}))
    expect = {"r4": init["r4"] + offset} if update else {}
    return case(name, ref, enc(mnemonic, rS=3, rA=4, rB=5), init, expect, FILL,
                {BASE: patched(offset, data)})


CASES += [
    store_indexed("stbx_stores_low_byte", "stbx", "STBX", 3, hexb("a8")),
    store_indexed("sthx_stores_low_half", "sthx", "STHX", 2, hexb("a7a8")),
    store_indexed("stwx_stores_low_word", "stwx", "STWX", 4, hexb("a5a6a7a8")),
    store_indexed("stdx_stores_64_bits", "stdx", "STDX", 8, hexb("a1a2a3a4a5a6a7a8")),
    # r4 + r5 = 0xFFFFFFFF00020004, which truncates to BASE + 4.
    case("stwx_truncates_ea_to_32_bits", "STWX", enc("stwx", rS=3, rA=4, rB=5),
         {"r3": VALUE, "r4": 0xFFFFFFFF0001FFFC, "r5": 8}, {}, FILL,
         {BASE: patched(4, hexb("a5a6a7a8"))}),
    store_indexed("stbux_updates_ra", "stbux", "STBUX", 3, hexb("a8"), update=True,
                  init={"r4": WIDE}),
    store_indexed("sthux_updates_ra", "sthux", "STHUX", 2, hexb("a7a8"), update=True,
                  init={"r4": WIDE}),
    store_indexed("stwux_updates_ra", "stwux", "STWUX", 4, hexb("a5a6a7a8"), update=True,
                  init={"r4": WIDE}),
    store_indexed("stdux_updates_ra", "stdux", "STDUX", 8, hexb("a1a2a3a4a5a6a7a8"), update=True,
                  init={"r4": WIDE}),
]

# --- load/store multiple ----------------------------------------------------------------------

WORDS = hexb("11111111222222223333333344444444")

CASES += [
    case("lmw_r29_loads_three_words", "LMW", enc("lmw", rD=29, rA=4, D=4),
         {"r4": BASE, "r28": 7, "r29": M64, "r30": M64, "r31": M64},
         {"r28": 7, "r29": 0x22222222, "r30": 0x33333333, "r31": 0x44444444}, {BASE: WORDS}),
    case("lmw_truncates_ea_to_32_bits", "LMW", enc("lmw", rD=30, rA=4, D=0),
         {"r4": WIDE, "r30": 0, "r31": 0}, {"r30": 0x11111111, "r31": 0x22222222},
         {BASE: WORDS}),
    case("lmw_r31_loads_one_word", "LMW", enc("lmw", rD=31, rA=4, D=-4),
         {"r4": BASE + 8}, {"r31": 0x22222222}, {BASE: WORDS}),
    case("stmw_r30_stores_two_low_words", "STMW", enc("stmw", rS=30, rA=4, D=4),
         {"r4": BASE, "r30": 0xAAAAAAAA11111111, "r31": 0xBBBBBBBB22222222}, {}, FILL,
         {BASE: patched(4, hexb("1111111122222222"))}),
    case("stmw_truncates_ea_to_32_bits", "STMW", enc("stmw", rS=31, rA=4, D=8),
         {"r4": WIDE, "r31": 0x5566778899AABBCC}, {}, FILL,
         {BASE: patched(8, hexb("99aabbcc"))}),
]


# --- string instructions ----------------------------------------------------------------------

BYTES = hexb("0102030405060708090a0b0c")

CASES += [
    case("lswi_partial_final_register", "LSWI", enc("lswi", rD=5, rA=4, NB=6),
         {"r4": BASE, "r5": M64, "r6": M64}, {"r5": 0x01020304, "r6": 0x05060000},
         {BASE: BYTES}),
    case("lswi_wraps_register_numbers", "LSWI", enc("lswi", rD=30, rA=4, NB=12),
         {"r4": BASE}, {"r30": 0x01020304, "r31": 0x05060708, "r0": 0x090A0B0C}, {BASE: BYTES}),
    case("lswi_nb_zero_loads_32_bytes", "LSWI", enc("lswi", rD=3, rA=4, NB=0),
         {"r4": BASE}, {"r3": 0x01010101, "r10": 0x08080808},
         {BASE: b"".join(bytes([i]) * 4 for i in range(1, 9))}),
    case("lswx_count_zero_changes_nothing", "LSWX", enc("lswx", rD=5, rA=4, rB=6),
         {"r4": BASE, "r5": 0x1234}, {"r5": 0x1234}, {BASE: BYTES}),
    case("stswi_partial_final_register", "STSWI", enc("stswi", rS=5, rA=4, NB=6),
         {"r4": BASE, "r5": 0xAABBCCDD, "r6": 0x11223344}, {}, FILL,
         {BASE: patched(0, hexb("aabbccdd1122"))}),
    case("stswx_count_zero_changes_nothing", "STSWX", enc("stswx", rS=5, rA=4, rB=6),
         {"r4": BASE, "r5": 0x1234}, {}, FILL, {BASE: b"\xff" * 16}),
]


# --- floating-point loads ---------------------------------------------------------------------

SINGLES = hexb("3fc00000" "00000001" "ff800000" "7fc00000")      # 1.5f, 2^-149, -inf, qNaN
PI = hexb("400921fb54442d18")
DENORMAL_AS_DOUBLE = 0x36A0000000000000     # 2^-149: exponent -149 + 1023 = 0x36a

CASES += [
    case("lfs_widens_single", "LFS", enc("lfs", frD=1, rA=4, D=0), {"r4": BASE},
         {"f1": 1.5}, {BASE: SINGLES}),
    case("lfs_widens_denormal_exactly", "LFS", enc("lfs", frD=1, rA=4, D=4), {"r4": BASE},
         {"f1": DENORMAL_AS_DOUBLE}, {BASE: SINGLES}),
    case("lfs_widens_negative_infinity", "LFS", enc("lfs", frD=1, rA=4, D=8), {"r4": BASE},
         {"f1": 0xFFF0000000000000}, {BASE: SINGLES}),
    case("lfs_widens_quiet_nan", "LFS", enc("lfs", frD=1, rA=4, D=12), {"r4": BASE},
         {"f1": 0x7FF8000000000000}, {BASE: SINGLES}),
    case("lfs_negative_displacement", "LFS", enc("lfs", frD=1, rA=4, D=-4), {"r4": BASE + 4},
         {"f1": 1.5}, {BASE: SINGLES}),
    case("lfs_truncates_ea_to_32_bits", "LFS", enc("lfs", frD=1, rA=4, D=0), {"r4": WIDE},
         {"f1": 1.5}, {BASE: SINGLES}),
    case("lfsu_updates_ra", "LFSU", enc("lfsu", frD=1, rA=4, D=4), {"r4": WIDE},
         {"f1": DENORMAL_AS_DOUBLE, "r4": WIDE + 4}, {BASE: SINGLES}),
    case("lfsx_widens_single", "LFSX", enc("lfsx", frD=1, rA=4, rB=5), {"r4": BASE, "r5": 0},
         {"f1": 1.5}, {BASE: SINGLES}),
    case("lfsux_updates_ra", "LFSUX", enc("lfsux", frD=1, rA=4, rB=5), {"r4": WIDE, "r5": 4},
         {"f1": DENORMAL_AS_DOUBLE, "r4": WIDE + 4}, {BASE: SINGLES}),
    case("lfd_moves_bits", "LFD", enc("lfd", frD=1, rA=4, D=0), {"r4": BASE},
         {"f1": 0x400921FB54442D18}, {BASE: PI}),
    case("lfd_moves_nan_bits", "LFD", enc("lfd", frD=1, rA=4, D=0), {"r4": BASE},
         {"f1": 0x7FF0000000000001}, {BASE: hexb("7ff0000000000001")}),
    case("lfdu_updates_ra", "LFDU", enc("lfdu", frD=1, rA=4, D=8), {"r4": WIDE},
         {"f1": 0x400921FB54442D18, "r4": WIDE + 8}, {BASE + 8: PI}),
    case("lfdx_moves_bits", "LFDX", enc("lfdx", frD=1, rA=4, rB=5), {"r4": BASE, "r5": 8},
         {"f1": 0x400921FB54442D18}, {BASE + 8: PI}),
    case("lfdux_updates_ra", "LFDUX", enc("lfdux", frD=1, rA=4, rB=5), {"r4": WIDE, "r5": 8},
         {"f1": 0x400921FB54442D18, "r4": WIDE + 8}, {BASE + 8: PI}),
]


# --- floating-point stores --------------------------------------------------------------------

def fp_store(name: str, mnemonic: str, ref: str, bits: int | float, offset: int, data: bytes, *,
             expect=None, init=None, **fields) -> Case:
    """`mnemonic f1` into the 0xff-filled block at BASE, r4 = BASE unless `init` overrides."""
    init = dict({"f1": bits, "r4": BASE}, **(init or {}))
    return case(name, ref, enc(mnemonic, frS=1, rA=4, **fields), init, expect, FILL,
                {BASE: patched(offset, data)})


# 1.1 = 0x3ff199999999999a: the 23 kept mantissa bits are 0xccccc and the dropped 29 bits are
# above one half, so the float rounds up to 0x3f8ccccd.
CASES += [
    fp_store("stfs_narrows_with_rounding", "stfs", "STFS", 0x3FF199999999999A, 4,
             hexb("3f8ccccd"), D=4),
    fp_store("stfs_tie_rounds_to_even_down", "stfs", "STFS", 0x3FF0000010000000, 0,
             hexb("3f800000"), D=0),
    fp_store("stfs_tie_rounds_to_even_up", "stfs", "STFS", 0x3FF0000030000000, 0,
             hexb("3f800002"), D=0),
    fp_store("stfs_overflow_gives_infinity", "stfs", "STFS", 1e300, 0, hexb("7f800000"), D=0),
    fp_store("stfs_denormal_result", "stfs", "STFS", DENORMAL_AS_DOUBLE, 0, hexb("00000001"),
             D=0),
    fp_store("stfs_denormal_tie_rounds_to_zero", "stfs", "STFS", 0x3690000000000000, 0,
             hexb("00000000"), D=0),
    fp_store("stfs_quiet_nan", "stfs", "STFS", 0x7FF8000000000000, 0, hexb("7fc00000"), D=0),
    fp_store("stfs_negative_displacement", "stfs", "STFS", 1.5, 0, hexb("3fc00000"), D=-4,
             init={"r4": BASE + 4}),
    fp_store("stfs_truncates_ea_to_32_bits", "stfs", "STFS", 1.5, 4, hexb("3fc00000"), D=4,
             init={"r4": WIDE}),
    fp_store("stfsu_updates_ra", "stfsu", "STFSU", 1.5, 4, hexb("3fc00000"), D=4,
             init={"r4": WIDE}, expect={"r4": WIDE + 4}),
    fp_store("stfd_moves_bits", "stfd", "STFD", 0x400921FB54442D18, 8, PI, D=8),
    fp_store("stfdu_updates_ra", "stfdu", "STFDU", 0x400921FB54442D18, 8, PI, D=8,
             init={"r4": WIDE}, expect={"r4": WIDE + 8}),
]
CASES += [
    case("stfsx_narrows_single", "STFSX", enc("stfsx", frS=1, rA=4, rB=5),
         {"f1": 1.5, "r4": BASE, "r5": 4}, {}, FILL, {BASE: patched(4, hexb("3fc00000"))}),
    case("stfsux_updates_ra", "STFSUX", enc("stfsux", frS=1, rA=4, rB=5),
         {"f1": 1.5, "r4": WIDE, "r5": 4}, {"r4": WIDE + 4}, FILL,
         {BASE: patched(4, hexb("3fc00000"))}),
    case("stfdx_moves_bits", "STFDX", enc("stfdx", frS=1, rA=4, rB=5),
         {"f1": 0x400921FB54442D18, "r4": BASE, "r5": 8}, {}, FILL, {BASE: patched(8, PI)}),
    case("stfdux_updates_ra", "STFDUX", enc("stfdux", frS=1, rA=4, rB=5),
         {"f1": 0x400921FB54442D18, "r4": WIDE, "r5": 8}, {"r4": WIDE + 8}, FILL,
         {BASE: patched(8, PI)}),
    case("stfiwx_stores_low_word_of_bits", "STFIWX", enc("stfiwx", frS=1, rA=4, rB=5),
         {"f1": 0x1122334455667788, "r4": BASE, "r5": 4}, {}, FILL,
         {BASE: patched(4, hexb("55667788"))}),
    case("stfiwx_ra0_uses_rb_only", "STFIWX", enc("stfiwx", frS=1, rA=0, rB=5),
         {"f1": 0xFFFFFFFF80000001, "r0": BASE + 0x100, "r5": BASE + 4}, {}, FILL,
         {BASE: patched(4, hexb("80000001"))}),
]

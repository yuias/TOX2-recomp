"""sys cases: SPR and CR moves, CR logical ops, reservations, dcbz and barriers.

Expected values are derived by hand from the architecture and the cited RPCS3 handlers. CR fields
are numbered 0..7 from the most significant nibble of the packed value; a nibble is lt=8 gt=4
eq=2 so=1, so CR bit n is bit n % 4 (from the left) of field n // 4. `r4` is the usual base
register and BASE the start of the guest data.
"""

from tests import ppc_asm as asm

from . import Case

M64 = 0xFFFFFFFFFFFFFFFF
BASE = 0x20000

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode


def case(name: str, ref: str, words, init=None, expect=None, mem_init=None, mem_expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {}, mem_init or {},
                mem_expect or {})


CASES = []


# --- mfspr, mtspr: LR, CTR, XER, VRSAVE ---------------------------------------------------------

CASES += [
    case("mflr", "MFSPR", enc("mfspr", rD=3, SPR=8), {"lr": 0x1122334455667788},
         {"r3": 0x1122334455667788, "lr": 0x1122334455667788}),
    case("mtlr", "MTSPR", enc("mtspr", SPR=8, rS=3), {"r3": 0x8877665544332211, "lr": 1},
         {"lr": 0x8877665544332211}),
    case("mfctr", "MFSPR", enc("mfspr", rD=3, SPR=9), {"ctr": M64}, {"r3": M64, "ctr": M64}),
    case("mtctr", "MTSPR", enc("mtspr", SPR=9, rS=3), {"r3": 0x123456789, "ctr": 0},
         {"ctr": 0x123456789}),
    case("mtlr_then_mflr_keeps_64_bits", "MTSPR",
         [enc("mtspr", SPR=8, rS=3), enc("mfspr", rD=5, SPR=8)], {"r3": 0xFEDCBA9876543210},
         {"r5": 0xFEDCBA9876543210}),
    # so 1, ov 0, ca 1, count 0x45 -> 0x80000000 | 0x20000000 | 0x45; rD is overwritten entirely.
    case("mfxer_packs_the_fields", "MFSPR", enc("mfspr", rD=3, SPR=1),
         {"r3": M64, "xer_so": 1, "xer_ov": 0, "xer_ca": 1, "xer_count": 0x45}, {"r3": 0xA0000045}),
    case("mfxer_all_set", "MFSPR", enc("mfspr", rD=3, SPR=1),
         {"xer_so": 1, "xer_ov": 1, "xer_ca": 1, "xer_count": 0x7F}, {"r3": 0xE000007F}),
    case("mfxer_clear", "MFSPR", enc("mfspr", rD=3, SPR=1), {"r3": M64}, {"r3": 0}),
    # Low word 0xa5a5a5a5: bit 31 set (so), bit 30 clear (ov), bit 29 set (ca), count 0xa5 & 0x7f.
    case("mtxer_unpacks_the_fields", "MTSPR", enc("mtspr", SPR=1, rS=3),
         {"r3": 0xFFFFFFFFA5A5A5A5, "xer_so": 0, "xer_ov": 1, "xer_ca": 0, "xer_count": 0x7F},
         {"xer_so": 1, "xer_ov": 0, "xer_ca": 1, "xer_count": 0x25}),
    case("mtxer_zero_clears", "MTSPR", enc("mtspr", SPR=1, rS=3),
         {"r3": 0, "xer_so": 1, "xer_ov": 1, "xer_ca": 1, "xer_count": 0x7F},
         {"xer_so": 0, "xer_ov": 0, "xer_ca": 0, "xer_count": 0}),
    case("mtxer_then_mfxer", "MTSPR", [enc("mtspr", SPR=1, rS=3), enc("mfspr", rD=5, SPR=1)],
         {"r3": 0x600000FF}, {"r5": 0x6000007F}),
    case("mfvrsave", "MFSPR", enc("mfspr", rD=3, SPR=256), {"r3": M64, "vrsave": 0xDEADBEEF},
         {"r3": 0xDEADBEEF}),
    case("mtvrsave_takes_the_low_word", "MTSPR", enc("mtspr", SPR=256, rS=3),
         {"r3": 0xFFFFFFFF00001234}, {"vrsave": 0x1234}),
]


# --- mfcr, mtcrf, mfocrf, mtocrf, mcrf ----------------------------------------------------------

CASES += [
    case("mfcr_packs_all_fields", "MFCR", enc("mfcr", rD=3), {"r3": M64, "cr": 0x12345678},
         {"r3": 0x12345678}),
    case("mfcr_from_fields", "MFCR", enc("mfcr", rD=3), {"cr0": 8, "cr7": 1}, {"r3": 0x80000001}),
    case("mtcrf_ff", "MTCRF", enc("mtcrf", CRM=0xFF, rS=3), {"r3": 0xFFFFFFFFA1B2C3D4},
         {"cr": 0xA1B2C3D4}),
    # CRM 0x81 selects fields 0 and 7: nibbles 5 and 9 of the low word.
    case("mtcrf_selects_fields", "MTCRF", enc("mtcrf", CRM=0x81, rS=3), {"r3": 0x5ABCDEF9},
         {"cr": 0x50000009}),
    case("mtcrf_keeps_unselected_fields", "MTCRF", enc("mtcrf", CRM=0x3C, rS=3),
         {"r3": 0x01234567, "cr": 0xFFFFFFFF}, {"cr": 0xFF2345FF}),
    case("mtcrf_zero_mask_changes_nothing", "MTCRF", enc("mtcrf", CRM=0, rS=3),
         {"r3": 0xFFFFFFFF, "cr": 0x11111111}, {"cr": 0x11111111}),
    # CRM 0x02 selects field 6: bits 7..4 of the register.
    case("mtocrf_single_field", "MTOCRF", enc("mtocrf", CRM=0x02, rS=3),
         {"r3": 0xA0, "cr": 0xFFFFFFFF}, {"cr": 0xFFFFFFAF}),
    case("mtocrf_field_0", "MTOCRF", enc("mtocrf", CRM=0x80, rS=3),
         {"r3": 0x30000000, "cr": 0xFFFFFFFF}, {"cr": 0x3FFFFFFF}),
    case("mfocrf_field_0", "MFOCRF", enc("mfocrf", rD=3, FXM=0x80),
         {"r3": M64, "cr": 0x9ABCDEF0}, {"r3": 0x90000000}),
    case("mfocrf_field_7", "MFOCRF", enc("mfocrf", rD=3, FXM=0x01),
         {"r3": M64, "cr": 0x9ABCDEF5}, {"r3": 0x5}),
    case("mfocrf_field_2", "MFOCRF", enc("mfocrf", rD=3, FXM=0x20),
         {"r3": M64, "cr": 0x12345678}, {"r3": 0x00300000}),
    case("mcrf_copies_a_field", "MCRF", enc("mcrf", crfD=2, crfS=5),
         {"cr5": 6, "cr2": 9}, {"cr2": 6, "cr5": 6}),
    case("mcrf_touches_only_the_destination", "MCRF", enc("mcrf", crfD=0, crfS=7),
         {"cr": 0x12345679}, {"cr": 0x92345679}),
]


# --- CR logical operations ----------------------------------------------------------------------

# a = CR bit 1 (cr0 gt), b = CR bit 5 (cr1 gt), result in CR bit 10 (cr2 eq); cr2 starts as 0xF
# so that both a missing set and a missing clear show up. The result bit is eq (2) inside cr2.
_TRUTH = {
    "crand": lambda a, b: a & b,
    "crandc": lambda a, b: a & (1 - b),
    "creqv": lambda a, b: 1 - (a ^ b),
    "crnand": lambda a, b: 1 - (a & b),
    "crnor": lambda a, b: 1 - (a | b),
    "cror": lambda a, b: a | b,
    "crorc": lambda a, b: a | (1 - b),
    "crxor": lambda a, b: a ^ b,
}

for _name, _truth in _TRUTH.items():
    for _a in (0, 1):
        for _b in (0, 1):
            _result = _truth(_a, _b)
            CASES.append(case(
                "%s_%d%d" % (_name, _a, _b), _name.upper(),
                enc(_name, crbD=10, crbA=1, crbB=5),
                {"cr0": 4 * _a, "cr1": 4 * _b, "cr2": 0xF},
                {"cr0": 4 * _a, "cr1": 4 * _b, "cr2": 0xD | 2 * _result}))

CASES += [
    # cror 2,1,2 is the "greater or equal" idiom: eq |= gt within cr0.
    case("cror_ge_idiom", "CROR", enc("cror", crbD=2, crbA=1, crbB=2), {"cr0": 4}, {"cr0": 6}),
    case("cror_ge_idiom_neither", "CROR", enc("cror", crbD=2, crbA=1, crbB=2), {"cr0": 8},
         {"cr0": 8}),
    # Across fields: cr7.so = cr1.eq | cr2.gt.
    case("cror_across_fields", "CROR", enc("cror", crbD=31, crbA=6, crbB=9),
         {"cr1": 0, "cr2": 4, "cr7": 0}, {"cr7": 1, "cr1": 0, "cr2": 4}),
    case("cror_across_fields_clear_stays_clear", "CROR", enc("cror", crbD=31, crbA=6, crbB=9),
         {"cr1": 8, "cr2": 8, "cr7": 0}, {"cr7": 0}),
    case("crxor_same_bit_clears", "CRXOR", enc("crxor", crbD=6, crbA=6, crbB=6),
         {"cr1": 0xF}, {"cr1": 0xD}),
    case("creqv_same_bit_sets", "CREQV", enc("creqv", crbD=6, crbA=6, crbB=6),
         {"cr1": 0}, {"cr1": 2}),
    case("crnor_same_bit_inverts_set", "CRNOR", enc("crnor", crbD=2, crbA=2, crbB=2),
         {"cr0": 2}, {"cr0": 0}),
    case("crnor_same_bit_inverts_clear", "CRNOR", enc("crnor", crbD=2, crbA=2, crbB=2),
         {"cr0": 0}, {"cr0": 2}),
    case("crandc_destination_is_first_source", "CRANDC", enc("crandc", crbD=1, crbA=1, crbB=5),
         {"cr0": 4, "cr1": 4}, {"cr0": 0, "cr1": 4}),
    # cr1.gt = cr0.gt | ~cr1.gt: the old destination value is read before it is overwritten.
    case("crorc_destination_is_second_source_set", "CRORC", enc("crorc", crbD=5, crbA=1, crbB=5),
         {"cr0": 0, "cr1": 4}, {"cr0": 0, "cr1": 0}),
    case("crorc_destination_is_second_source_clear", "CRORC",
         enc("crorc", crbD=5, crbA=1, crbB=5), {"cr0": 0, "cr1": 0}, {"cr0": 0, "cr1": 4}),
    case("crand_into_so_leaves_xer_alone", "CRAND", enc("crand", crbD=3, crbA=1, crbB=1),
         {"cr0": 4, "xer_so": 0, "r3": 7}, {"cr0": 4 | 1, "xer_so": 0, "r3": 7}),
]


# --- lwarx, ldarx, stwcx., stdcx. ---------------------------------------------------------------

def hexb(text: str) -> bytes:
    return bytes.fromhex(text)


LWARX = enc("lwarx", rD=5, rA=0, rB=4)
LDARX = enc("ldarx", rD=5, rA=0, rB=4)
STWCX = enc("stwcx.", rS=6, rA=0, rB=4)
STDCX = enc("stdcx.", rS=6, rA=0, rB=4)

CASES += [
    case("lwarx_zero_extends", "LWARX", LWARX, {"r4": BASE, "r5": M64},
         {"r5": 0x80000001}, {BASE: hexb("80000001")}),
    case("lwarx_indexed", "LWARX", enc("lwarx", rD=5, rA=4, rB=7), {"r4": BASE, "r7": 8},
         {"r5": 0x11223344}, {BASE + 8: hexb("11223344")}),
    case("lwarx_ra0_is_a_literal_zero_base", "LWARX", enc("lwarx", rD=5, rA=0, rB=7),
         {"r0": 0x99999999, "r7": BASE + 4}, {"r5": 0x55667788}, {BASE + 4: hexb("55667788")}),
    case("lwarx_stwcx_succeeds", "STWCX", [LWARX, STWCX],
         {"r4": BASE, "r6": 0xAABBCCDD, "cr0": 0xF},
         {"r5": 0x11223344, "cr0": 2},
         {BASE: hexb("11223344")}, {BASE: hexb("AABBCCDD")}),
    case("stwcx_success_copies_so", "STWCX", [LWARX, STWCX],
         {"r4": BASE, "r6": 1, "xer_so": 1}, {"cr0": 2 | 1}, {BASE: hexb("00000000")},
         {BASE: hexb("00000001")}),
    case("stwcx_stores_the_low_word", "STWCX", [LWARX, STWCX],
         {"r4": BASE, "r6": 0xFFFFFFFF01020304}, {"cr0": 2}, {BASE: hexb("ffffffffffffffff")},
         {BASE: hexb("01020304ffffffff")}),
    case("stwcx_without_reservation_fails", "STWCX", STWCX,
         {"r4": BASE, "r6": 0xAABBCCDD, "cr0": 0xE}, {"cr0": 0},
         {BASE: hexb("11223344")}, {BASE: hexb("11223344")}),
    case("stwcx_without_reservation_copies_so", "STWCX", STWCX,
         {"r4": BASE, "r6": 1, "xer_so": 1, "cr0": 0xE}, {"cr0": 1},
         {BASE: hexb("11223344")}, {BASE: hexb("11223344")}),
    case("stwcx_to_another_address_fails", "STWCX",
         [LWARX, enc("stwcx.", rS=6, rA=0, rB=7)],
         {"r4": BASE, "r7": BASE + 4, "r6": 0xAABBCCDD, "cr0": 0xF}, {"cr0": 0},
         {BASE: hexb("1122334455667788")}, {BASE: hexb("1122334455667788")}),
    case("second_stwcx_fails_after_success", "STWCX", [LWARX, STWCX, STWCX],
         {"r4": BASE, "r6": 0xAABBCCDD}, {"cr0": 0},
         {BASE: hexb("11223344")}, {BASE: hexb("AABBCCDD")}),
    case("ldarx_loads_64_bits", "LDARX", LDARX, {"r4": BASE, "r5": 0},
         {"r5": 0x8182838485868788}, {BASE: hexb("8182838485868788")}),
    case("ldarx_stdcx_succeeds", "STDCX", [LDARX, STDCX],
         {"r4": BASE, "r6": 0x0102030405060708, "cr0": 0xF},
         {"r5": 0x1122334455667788, "cr0": 2},
         {BASE: hexb("1122334455667788")}, {BASE: hexb("0102030405060708")}),
    case("stdcx_without_reservation_fails", "STDCX", STDCX,
         {"r4": BASE, "r6": 0x0102030405060708, "cr0": 0xE}, {"cr0": 0},
         {BASE: hexb("1122334455667788")}, {BASE: hexb("1122334455667788")}),
    # A reservation taken by lwarx does not cover a doubleword store-conditional elsewhere.
    case("stdcx_to_another_address_fails", "STDCX",
         [LDARX, enc("stdcx.", rS=6, rA=0, rB=7)],
         {"r4": BASE, "r7": BASE + 8, "r6": 1, "cr0": 0xF}, {"cr0": 0},
         {BASE: hexb("11223344556677888877665544332211")},
         {BASE: hexb("11223344556677888877665544332211")}),
]


# --- dcbz and no-effect instructions ------------------------------------------------------------

# Three 128-byte lines of 0xff; dcbz at an unaligned address clears only the middle one.
_LINES = b"\xff" * 0x180
_MIDDLE_ONLY = [(BASE, b"\xff" * 0x80), (BASE + 0x80, b"\x00" * 0x80), (BASE + 0x100, b"\xff" * 0x80)]

CASES += [
    case("dcbz_clears_the_whole_line", "DCBZ", enc("dcbz", rA=4, rB=5),
         {"r4": BASE, "r5": 0x80 + 0x45}, {}, {BASE: _LINES}, dict(_MIDDLE_ONLY)),
    case("dcbz_first_byte_of_a_line", "DCBZ", enc("dcbz", rA=4, rB=5),
         {"r4": BASE + 0x80, "r5": 0}, {}, {BASE: _LINES}, dict(_MIDDLE_ONLY)),
    case("dcbz_last_byte_of_a_line", "DCBZ", enc("dcbz", rA=4, rB=5),
         {"r4": BASE + 0xFF, "r5": 0}, {}, {BASE: _LINES}, dict(_MIDDLE_ONLY)),
    case("dcbz_ra0_uses_rb_alone", "DCBZ", enc("dcbz", rA=0, rB=5),
         {"r0": 0x1234, "r5": BASE + 0x90}, {}, {BASE: _LINES}, dict(_MIDDLE_ONLY)),
    case("dcbz_truncates_the_address", "DCBZ", enc("dcbz", rA=4, rB=5),
         {"r4": 0x100000000 + BASE, "r5": 0x80}, {}, {BASE: _LINES}, dict(_MIDDLE_ONLY)),
    case("barriers_and_cache_hints_change_nothing", "SYNC",
         [enc("sync", L_sync=0), enc("sync", L_sync=1), enc("eieio"), enc("isync"),
          enc("dcbt", rA=4, rB=5), enc("dcbtst", rA=4, rB=5), enc("dcbf", rA=4, rB=5),
          enc("dcbst", rA=4, rB=5), enc("icbi", rA=4, rB=5), enc("dst", rA=4, rB=5),
          enc("dstst", rA=4, rB=5), enc("dss")],
         {"r3": 0x1234, "r4": BASE, "r5": 0x10, "cr": 0x13579BDF},
         {"r3": 0x1234, "r4": BASE, "r5": 0x10, "cr": 0x13579BDF},
         {BASE: _LINES}, {BASE: _LINES}),
]


# The time base value is runtime-defined, so only the untouched state is checked here (the emitted
# call is covered by a text test).
CASES += [
    case("mftb_lower_leaves_other_state", "MFTB", enc("mftb", rD=3, TBR=268),
         {"r5": 7, "cr": 0x13579BDF, "lr": 9}, {"r5": 7, "cr": 0x13579BDF, "lr": 9}),
    case("mftb_upper_leaves_other_state", "MFTB", enc("mftb", rD=3, TBR=269),
         {"r5": 7, "cr": 0x13579BDF, "lr": 9}, {"r5": 7, "cr": 0x13579BDF, "lr": 9}),
]

"""vmx_perm cases: permute, merge, splat, pack, unpack and whole-register shifts.

Expected values are derived by hand from the architecture and the cited RPCS3 handlers. Vector words
are in big-endian element order. P and Q have no repeated or zero bytes, so a lane-order or
operand-order mistake changes the result. P || Q is the 32-byte source of `vperm` and `vsldoi`.
"""

from tests import ppc_asm as asm

from . import Case

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

# Bytes 01 12 23 34 45 56 67 78 89 9A AB BC CD DE EF F0.
P = (0x01122334, 0x45566778, 0x899AABBC, 0xCDDEEFF0)
# Bytes 0A 1B 2C 3D 4E 5F 60 71 82 93 A4 B5 C6 D7 E8 F9.
Q = (0x0A1B2C3D, 0x4E5F6071, 0x8293A4B5, 0xC6D7E8F9)

CASES = []


def case(name: str, ref: str, words, init=None, expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {})


def binop(name: str, a, b, expect, **extra) -> Case:
    return case("%s_%s" % (name, extra.pop("tag")), name.upper(), enc(name, vD=3, vA=1, vB=2),
                {"v1": a, "v2": b, **extra.pop("init", {})}, {"v3": expect, **extra})


# --- vperm --------------------------------------------------------------------------------------

PERM = enc("vperm", vD=3, vA=1, vB=2, vC=4)
SAME_AS_A = enc("vperm", vD=1, vA=1, vB=2, vC=4)

CASES += [
    case("vperm_identity_selects_vra", "VPERM", PERM,
         {"v1": P, "v2": Q, "v4": (0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F)},
         {"v3": P, "v1": P, "v2": Q}),
    case("vperm_indices_16_to_31_select_vrb", "VPERM", PERM,
         {"v1": P, "v2": Q, "v4": (0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F)},
         {"v3": Q}),
    # P[0] and Q[15] and so on, alternating between the two sources and reading in both directions.
    case("vperm_mixes_both_sources", "VPERM", PERM,
         {"v1": P, "v2": Q, "v4": (0x1F00100F, 0x011E021D, 0x031C041B, 0x051A0619)},
         {"v3": (0xF9010AF0, 0x12E823D7, 0x34C645B5, 0x56A46793)}),
    # Only the low five bits of each control byte count: E5 -> 5, 20 -> 0, 3F and FF -> 31,
    # 90 -> 16, 41 -> 1, A3 -> 3, 7C -> 28.
    case("vperm_ignores_the_top_three_control_bits", "VPERM", PERM,
         {"v1": P, "v2": Q, "v4": (0xE5203FFF, 0x9041A37C, 0x11121314, 0x00000000)},
         {"v3": (0x5601F9F9, 0x0A1234C6, 0x1B2C3D4E, 0x01010101)}),
    case("vperm_destination_is_vra", "VPERM", SAME_AS_A,
         {"v1": P, "v2": Q, "v4": (0x1F00100F, 0x011E021D, 0x031C041B, 0x051A0619)},
         {"v1": (0xF9010AF0, 0x12E823D7, 0x34C645B5, 0x56A46793), "v2": Q}),
    case("vperm_same_source_twice_repeats_the_register", "VPERM",
         enc("vperm", vD=3, vA=1, vB=1, vC=4),
         {"v1": P, "v4": (0x10000F1F, 0x00000000, 0x00000000, 0x00000000)},
         {"v3": (0x0101F0F0, 0x01010101, 0x01010101, 0x01010101)}),
]


# --- vsldoi -------------------------------------------------------------------------------------

def sldoi(shb: int, expect, name=None) -> Case:
    return case(name or "vsldoi_%d" % shb, "VSLDOI", enc("vsldoi", vD=3, vA=1, vB=2, SHB=shb),
                {"v1": P, "v2": Q}, {"v3": expect, "v1": P, "v2": Q})


CASES += [
    sldoi(0, P),
    sldoi(1, (0x12233445, 0x56677889, 0x9AABBCCD, 0xDEEFF00A)),
    sldoi(4, (0x45566778, 0x899AABBC, 0xCDDEEFF0, 0x0A1B2C3D)),
    sldoi(5, (0x56677889, 0x9AABBCCD, 0xDEEFF00A, 0x1B2C3D4E)),
    sldoi(8, (0x899AABBC, 0xCDDEEFF0, 0x0A1B2C3D, 0x4E5F6071)),
    sldoi(15, (0xF00A1B2C, 0x3D4E5F60, 0x718293A4, 0xB5C6D7E8)),
    case("vsldoi_destination_is_vra", "VSLDOI", enc("vsldoi", vD=1, vA=1, vB=2, SHB=4),
         {"v1": P, "v2": Q}, {"v1": (0x45566778, 0x899AABBC, 0xCDDEEFF0, 0x0A1B2C3D), "v2": Q}),
    case("vsldoi_same_register_rotates", "VSLDOI", enc("vsldoi", vD=3, vA=1, vB=1, SHB=4),
         {"v1": P}, {"v3": (0x45566778, 0x899AABBC, 0xCDDEEFF0, 0x01122334)}),
]


# --- merges -------------------------------------------------------------------------------------

CASES += [
    binop("vmrghw", P, Q, (0x01122334, 0x0A1B2C3D, 0x45566778, 0x4E5F6071), tag="a_before_b"),
    binop("vmrglw", P, Q, (0x899AABBC, 0x8293A4B5, 0xCDDEEFF0, 0xC6D7E8F9), tag="a_before_b"),
    binop("vmrghh", P, Q, (0x01120A1B, 0x23342C3D, 0x45564E5F, 0x67786071), tag="a_before_b"),
    binop("vmrglh", P, Q, (0x899A8293, 0xABBCA4B5, 0xCDDEC6D7, 0xEFF0E8F9), tag="a_before_b"),
    binop("vmrghb", P, Q, (0x010A121B, 0x232C343D, 0x454E565F, 0x67607871), tag="a_before_b"),
    binop("vmrglb", P, Q, (0x89829A93, 0xABA4BCB5, 0xCDC6DED7, 0xEFE8F0F9), tag="a_before_b"),
    case("vmrghw_destination_is_vrb", "VMRGHW", enc("vmrghw", vD=2, vA=1, vB=2),
         {"v1": P, "v2": Q}, {"v2": (0x01122334, 0x0A1B2C3D, 0x45566778, 0x4E5F6071)}),
    case("vmrglw_same_register_doubles_each_word", "VMRGLW", enc("vmrglw", vD=3, vA=1, vB=1),
         {"v1": P}, {"v3": (0x899AABBC, 0x899AABBC, 0xCDDEEFF0, 0xCDDEEFF0)}),
]


# --- splats -------------------------------------------------------------------------------------

def splat(name: str, uimm: int, expect) -> Case:
    return case("%s_%d" % (name, uimm), name.upper(), enc(name, vD=3, vB=2, UIMM5=uimm),
                {"v2": P}, {"v3": expect, "v2": P})


def splat_word(value: int):
    return (value,) * 4


CASES += [
    splat("vspltw", 0, splat_word(0x01122334)),
    splat("vspltw", 1, splat_word(0x45566778)),
    splat("vspltw", 2, splat_word(0x899AABBC)),
    splat("vspltw", 3, splat_word(0xCDDEEFF0)),
    # RPCS3 masks the element number with the lane count: 19 -> 3.
    splat("vspltw", 19, splat_word(0xCDDEEFF0)),
    splat("vsplth", 0, splat_word(0x01120112)),
    splat("vsplth", 3, splat_word(0x67786778)),
    splat("vsplth", 7, splat_word(0xEFF0EFF0)),
    splat("vsplth", 11, splat_word(0x67786778)),
    splat("vspltb", 0, splat_word(0x01010101)),
    splat("vspltb", 5, splat_word(0x56565656)),
    splat("vspltb", 15, splat_word(0xF0F0F0F0)),
    splat("vspltb", 21, splat_word(0x56565656)),
    case("vspltw_destination_is_the_source", "VSPLTW", enc("vspltw", vD=2, vB=2, UIMM5=2),
         {"v2": P}, {"v2": splat_word(0x899AABBC)}),
]


def splat_imm(name: str, simm: int, expect) -> Case:
    tag = "minus_%d" % -simm if simm < 0 else str(simm)
    return case("%s_%s" % (name, tag), name.upper(), enc(name, vD=3, SIMM5=simm),
                {"v3": P}, {"v3": expect})


CASES += [
    splat_imm("vspltisw", -1, splat_word(0xFFFFFFFF)),
    splat_imm("vspltisw", 0, splat_word(0)),
    splat_imm("vspltisw", 1, splat_word(1)),
    splat_imm("vspltisw", 15, splat_word(15)),
    splat_imm("vspltisw", -16, splat_word(0xFFFFFFF0)),
    splat_imm("vspltish", -2, splat_word(0xFFFEFFFE)),
    splat_imm("vspltish", 7, splat_word(0x00070007)),
    splat_imm("vspltish", -16, splat_word(0xFFF0FFF0)),
    splat_imm("vspltisb", -1, splat_word(0xFFFFFFFF)),
    splat_imm("vspltisb", 5, splat_word(0x05050505)),
    splat_imm("vspltisb", -16, splat_word(0xF0F0F0F0)),
    splat_imm("vspltisb", 15, splat_word(0x0F0F0F0F)),
]


# --- packs --------------------------------------------------------------------------------------

CASES += [
    binop("vpkuhum", P, Q, (0x12345678, 0x9ABCDEF0, 0x1B3D5F71, 0x93B5D7F9), tag="truncates_a_then_b"),
    binop("vpkuwum", P, Q, (0x23346778, 0xABBCEFF0, 0x2C3D6071, 0xA4B5E8F9), tag="truncates_a_then_b"),
]



def be_words(data: str) -> tuple[int, ...]:
    """Four big-endian words from 32 hex digits."""
    return tuple(int(data[i:i + 8], 16) for i in range(0, 32, 8))


# Saturating packs: (name, clamping input, its packed bytes, in-range input, its packed bytes).
# Signed halfwords 1, 127 (kept), 128, -128 (kept), -129, 32767, -32768, -1 pack to signed bytes.
# Signed halfwords 0, 255 (kept), 256, -1, -32768, 32767, 128, 1 pack to unsigned bytes. Unsigned
# halfwords 0, 255 (kept), 256, 65535, 32768, 127, 1, 512 pack to unsigned bytes (32768 would be
# negative if read as signed). The word packs follow the same pattern with 16-bit results.
SATURATING_PACKS = [
    ("vpkshss", (0x0001007F, 0x0080FF80, 0xFF7F7FFF, 0x8000FFFF), "017F7F80807F80FF",
     (0x00000001, 0xFFFF0010, 0xFFF0007F, 0xFF800005), "0001FF10F07F8005"),
    ("vpkshus", (0x000000FF, 0x0100FFFF, 0x80007FFF, 0x00800001), "00FFFF0000FF8001",
     (0x001000F0, 0x00000001, 0x00020003, 0x000400FE), "10F00001020304FE"),
    ("vpkuhus", (0x000000FF, 0x0100FFFF, 0x8000007F, 0x00010200), "00FFFFFFFF7F01FF",
     (0x00100020, 0x00300040, 0x00500060, 0x00700080), "1020304050607080"),
    ("vpkswss", (0x00007FFF, 0x00008000, 0xFFFF8000, 0xFFFF7FFF), "7FFF7FFF80008000",
     (0x00000001, 0xFFFFFFFF, 0x00007FFF, 0xFFFF8000), "0001FFFF7FFF8000"),
    ("vpkswus", (0x0000FFFF, 0x00010000, 0xFFFFFFFF, 0x80000000), "FFFFFFFF00000000",
     (0x00001234, 0x00000000, 0x0000FFFF, 0x0000ABCD), "12340000FFFFABCD"),
    ("vpkuwus", (0x0000FFFF, 0x00010000, 0x80000000, 0x00000007), "FFFFFFFFFFFF0007",
     (0x00000000, 0x0000ABCD, 0x0000FFFE, 0x0000FFFF), "0000ABCDFFFEFFFF"),
]

for _name, _clamp, _clamp_packed, _fine, _fine_packed in SATURATING_PACKS:
    CASES += [
        binop(_name, _clamp, _fine, be_words(_clamp_packed + _fine_packed), tag="clamps_a_and_sets_sat",
              vscr_sat=1),
        binop(_name, _fine, _clamp, be_words(_fine_packed + _clamp_packed), tag="clamps_b_and_sets_sat",
              vscr_sat=1),
        binop(_name, _fine, _fine, be_words(_fine_packed + _fine_packed), tag="in_range_leaves_sat_clear",
              vscr_sat=0),
        binop(_name, _fine, _fine, be_words(_fine_packed + _fine_packed), tag="sat_stays_set",
              init={"vscr_sat": 1}, vscr_sat=1),
    ]

# Pixels a:8 r:8 g:8 b:8 -> 1:5:5:5 from bit 24, bits 23..19, 15..11 and 7..3 of each word.
# 01F80880 -> 1 11111 00001 10000 = FC30, FE07F00F -> 0 00000 11110 00001 = 03C1,
# 00380018 -> 0 00111 00000 00011 = 1C03, 01FFFFFF -> FFFF, 01000000 -> 8000,
# 00080808 -> 0 00001 00001 00001 = 0421, 00F800F8 -> 0 11111 00000 11111 = 7C1F.
PIXELS_A = (0x01F80880, 0xFE07F00F, 0x00380018, 0x01FFFFFF)
PIXELS_B = (0x00000000, 0x01000000, 0x00080808, 0x00F800F8)
PACKED_PIXELS = (0xFC3003C1, 0x1C03FFFF, 0x00008000, 0x04217C1F)

CASES += [
    binop("vpkpx", PIXELS_A, PIXELS_B, PACKED_PIXELS, tag="a_then_b"),
    binop("vpkpx", (0,) * 4, (0,) * 4, (0,) * 4, tag="zero", vscr_sat=0),
]


# --- unpacks ------------------------------------------------------------------------------------

# Bytes 80 FF 7F 01 02 80 FE 7D | 81 01 FF 00 7F 80 AA 55.
X = (0x80FF7F01, 0x0280FE7D, 0x8101FF00, 0x7F80AA55)


def unpack(name: str, expect, src=X) -> Case:
    return case(name + "_sign_extends", name.upper(), enc(name, vD=3, vB=2),
                {"v2": src}, {"v3": expect, "v2": src})


CASES += [
    unpack("vupkhsb", (0xFF80FFFF, 0x007F0001, 0x0002FF80, 0xFFFE007D)),
    unpack("vupklsb", (0xFF810001, 0xFFFF0000, 0x007FFF80, 0xFFAA0055)),
    unpack("vupkhsh", (0xFFFF80FF, 0x00007F01, 0x00000280, 0xFFFFFE7D)),
    unpack("vupklsh", (0xFFFF8101, 0xFFFFFF00, 0x00007F80, 0xFFFFAA55)),
    # The packed pixels of the vpkpx case: FC30 -> ff 1f 01 10, 03C1 -> 00 00 1e 01,
    # 1C03 -> 00 07 00 03, FFFF -> ff 1f 1f 1f, 0421 -> 00 01 01 01, 8000 -> ff 00 00 00,
    # 7C1F -> 00 1f 00 1f, 0000 -> 0. The high four halfwords are FC30 03C1 1C03 FFFF, the low
    # four 0000 8000 0421 7C1F.
    unpack("vupkhpx", (0xFF1F0110, 0x00001E01, 0x00070003, 0xFF1F1F1F), PACKED_PIXELS),
    unpack("vupklpx", (0x00000000, 0xFF000000, 0x00010101, 0x001F001F), PACKED_PIXELS),
    case("vupkhsb_destination_is_the_source", "VUPKHSB", enc("vupkhsb", vD=2, vB=2),
         {"v2": X}, {"v2": (0xFF80FFFF, 0x007F0001, 0x0002FF80, 0xFFFE007D)}),
]


# --- whole-register shifts ----------------------------------------------------------------------

def shift(name: str, count: int, expect, tag: str, a=P) -> Case:
    return case("%s_%s" % (name, tag), name.upper(), enc(name, vD=3, vA=1, vB=2),
                {"v1": a, "v2": (count,) * 4}, {"v3": expect, "v1": a})


# P << 3 and P >> 3 across word boundaries, P << 1, and the octet shifts by 5 and 15 bytes. The
# bit shifts splat the count into every byte, which the architecture requires.
CASES += [
    shift("vsl", 0x0B0B0B0B, (0x089119A2, 0x2AB33BC4, 0x4CD55DE6, 0x6EF77F80), "by_3"),
    shift("vsl", 0x01010101, (0x02244668, 0x8AACCEF1, 0x13355779, 0x9BBDDFE0), "by_1"),
    shift("vsl", 0x00000000, P, "by_0"),
    shift("vsr", 0x0B0B0B0B, (0x00224466, 0x88AACCEF, 0x11335577, 0x99BBDDFE), "by_3"),
    shift("vsr", 0x00000000, P, "by_0"),
    # Only byte 15 of vB counts for the octet shifts: 0x2F & 0x78 = 40 bits, 0x78 = 120 bits.
    case("vslo_by_5_octets", "VSLO", enc("vslo", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFF2F)},
         {"v3": (0x56677889, 0x9AABBCCD, 0xDEEFF000, 0x00000000)}),
    case("vslo_by_15_octets", "VSLO", enc("vslo", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0, 0, 0, 0x78)}, {"v3": (0xF0000000, 0, 0, 0)}),
    case("vslo_by_0_octets", "VSLO", enc("vslo", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0, 0, 0, 0x07)}, {"v3": P}),
    case("vsro_by_5_octets", "VSRO", enc("vsro", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFF2F)},
         {"v3": (0x00000000, 0x00011223, 0x34455667, 0x78899AAB)}),
    case("vsro_by_15_octets", "VSRO", enc("vsro", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0, 0, 0, 0x78)}, {"v3": (0, 0, 0, 0x00000001)}),
    case("vsro_by_0_octets", "VSRO", enc("vsro", vD=3, vA=1, vB=2),
         {"v1": P, "v2": (0, 0, 0, 0x07)}, {"v3": P}),
    case("vsl_destination_is_vra", "VSL", enc("vsl", vD=1, vA=1, vB=2),
         {"v1": P, "v2": (0x01010101,) * 4}, {"v1": (0x02244668, 0x8AACCEF1, 0x13355779, 0x9BBDDFE0)}),
]

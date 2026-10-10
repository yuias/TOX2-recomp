"""vmx_mem cases: vector loads and stores, lvsl/lvsr, bitwise logic, vsel and the VSCR moves.

Expected values are derived by hand from the architecture and the cited RPCS3 handlers. Guest data
lives at BASE (16-byte aligned). Vector words are in big-endian element order. Memory block `BLK`
has no zero bytes, so a zero fill is distinguishable from loaded data; stores start from 0xff
memory so that untouched neighbours are visible.
"""

from tests import ppc_asm as asm

from . import Case

BASE = 0x20000
WIDE = 0x100000000 + BASE

REF = "RPCS3 PPUInterpreter.cpp "

enc = asm.encode

# Block at BASE and the 16 bytes after it.
BLK = bytes.fromhex("011223344556677889" "9aabbccddeeff0")
BLK2 = bytes.fromhex("0a1b2c3d4e5f60718293a4b5c6d7e8f9")
BLK_WORDS = (0x01122334, 0x45566778, 0x899AABBC, 0xCDDEEFF0)
MEM = {BASE: BLK + BLK2}
FF = b"\xff" * 16
FILL = {BASE: FF + FF + FF}

CASES = []


def case(name: str, ref: str, words, init=None, expect=None, mem_init=None, mem_expect=None) -> Case:
    if not isinstance(words, (list, tuple)):
        words = [words]
    return Case(name, REF + ref, tuple(words), init or {}, expect or {}, mem_init or {},
                mem_expect or {})


def lv(name: str, **fields) -> int:
    return enc(name, vD=3, rA=4, rB=5, **fields)


def st(name: str, **fields) -> int:
    return enc(name, vS=3, rA=4, rB=5, **fields)


# --- lvx, lvxl and the element loads -----------------------------------------------------------

CASES += [
    case("lvx_loads_the_aligned_block", "LVX", lv("lvx"), {"r4": BASE, "r5": 0},
         {"v3": BLK_WORDS}, MEM),
    case("lvx_ignores_the_low_ea_bits", "LVX", lv("lvx"), {"r4": BASE, "r5": 0xF},
         {"v3": BLK_WORDS}, MEM),
    case("lvx_second_block", "LVX", lv("lvx"), {"r4": BASE, "r5": 0x11},
         {"v3": (0x0A1B2C3D, 0x4E5F6071, 0x8293A4B5, 0xC6D7E8F9)}, MEM),
    case("lvx_ra0_is_a_literal", "LVX", enc("lvx", vD=3, rA=0, rB=5),
         {"r0": 0x1234, "r5": BASE + 3}, {"v3": BLK_WORDS}, MEM),
    case("lvx_truncates_ea_to_32_bits", "LVX", lv("lvx"), {"r4": WIDE, "r5": 1},
         {"v3": BLK_WORDS}, MEM),
    case("lvxl_loads_the_aligned_block", "LVXL", lv("lvxl"), {"r4": BASE, "r5": 9},
         {"v3": BLK_WORDS}, MEM),
    # RPCS3 loads the whole block for the element loads; the other elements are undefined by the
    # architecture, so only the addressed element is architecturally visible.
    case("lvebx_offset_1", "LVEBX", lv("lvebx"), {"r4": BASE, "r5": 1}, {"v3": BLK_WORDS}, MEM),
    case("lvebx_offset_15", "LVEBX", lv("lvebx"), {"r4": BASE, "r5": 15}, {"v3": BLK_WORDS}, MEM),
    case("lvehx_offset_6", "LVEHX", lv("lvehx"), {"r4": BASE, "r5": 6}, {"v3": BLK_WORDS}, MEM),
    case("lvehx_odd_offset", "LVEHX", lv("lvehx"), {"r4": BASE, "r5": 7}, {"v3": BLK_WORDS}, MEM),
    case("lvewx_offset_0", "LVEWX", lv("lvewx"), {"r4": BASE, "r5": 0}, {"v3": BLK_WORDS}, MEM),
    case("lvewx_offset_12", "LVEWX", lv("lvewx"), {"r4": BASE, "r5": 12}, {"v3": BLK_WORDS}, MEM),
]


# --- lvlx / lvrx and the unaligned load idiom ---------------------------------------------------

CASES += [
    case("lvlx_sh_0_loads_everything", "LVLX", lv("lvlx"), {"r4": BASE, "r5": 0},
         {"v3": BLK_WORDS}, MEM),
    case("lvlx_sh_1", "LVLX", lv("lvlx"), {"r4": BASE, "r5": 1},
         {"v3": (0x12233445, 0x56677889, 0x9AABBCCD, 0xDEEFF000)}, MEM),
    case("lvlx_sh_7", "LVLX", lv("lvlx"), {"r4": BASE, "r5": 7},
         {"v3": (0x78899AAB, 0xBCCDDEEF, 0xF0000000, 0)}, MEM),
    case("lvlx_sh_15", "LVLX", lv("lvlx"), {"r4": BASE, "r5": 15},
         {"v3": (0xF0000000, 0, 0, 0)}, MEM),
    case("lvlx_overwrites_the_register", "LVLX", lv("lvlx"),
         {"r4": BASE, "r5": 15, "v3": (0xFFFFFFFF,) * 4}, {"v3": (0xF0000000, 0, 0, 0)}, MEM),
    case("lvlxl_matches_lvlx", "LVLXL", lv("lvlxl"), {"r4": BASE, "r5": 7},
         {"v3": (0x78899AAB, 0xBCCDDEEF, 0xF0000000, 0)}, MEM),
    case("lvlx_ra0_is_a_literal", "LVLX", enc("lvlx", vD=3, rA=0, rB=5),
         {"r0": 0x1234, "r5": BASE + 1}, {"v3": (0x12233445, 0x56677889, 0x9AABBCCD, 0xDEEFF000)},
         MEM),
    case("lvrx_sh_0_loads_nothing", "LVRX", lv("lvrx"), {"r4": BASE, "r5": 0},
         {"v3": (0, 0, 0, 0)}, MEM),
    case("lvrx_sh_1", "LVRX", lv("lvrx"), {"r4": BASE, "r5": 1},
         {"v3": (0, 0, 0, 0x01)}, MEM),
    case("lvrx_sh_7", "LVRX", lv("lvrx"), {"r4": BASE, "r5": 7},
         {"v3": (0, 0, 0x00011223, 0x34455667)}, MEM),
    case("lvrx_sh_15", "LVRX", lv("lvrx"), {"r4": BASE, "r5": 15},
         {"v3": (0x00011223, 0x34455667, 0x78899AAB, 0xBCCDDEEF)}, MEM),
    case("lvrxl_matches_lvrx", "LVRXL", lv("lvrxl"), {"r4": BASE, "r5": 7},
         {"v3": (0, 0, 0x00011223, 0x34455667)}, MEM),
    # lvlx at ea plus lvrx at ea + 16 reproduce the 16 unaligned bytes starting at ea.
    case("unaligned_load_idiom_sh_7", "LVLX LVRX VOR",
         [enc("lvlx", vD=1, rA=0, rB=3), enc("lvrx", vD=2, rA=3, rB=6),
          enc("vor", vD=4, vA=1, vB=2)],
         {"r3": BASE + 7, "r6": 16},
         {"v1": (0x78899AAB, 0xBCCDDEEF, 0xF0000000, 0), "v2": (0, 0, 0x000A1B2C, 0x3D4E5F60),
          "v4": (0x78899AAB, 0xBCCDDEEF, 0xF00A1B2C, 0x3D4E5F60)}, MEM),
    case("unaligned_load_idiom_aligned", "LVLX LVRX VOR",
         [enc("lvlx", vD=1, rA=0, rB=3), enc("lvrx", vD=2, rA=3, rB=6),
          enc("vor", vD=4, vA=1, vB=2)],
         {"r3": BASE, "r6": 16}, {"v2": (0, 0, 0, 0), "v4": BLK_WORDS}, MEM),
    case("unaligned_load_idiom_sh_15", "LVLX LVRX VOR",
         [enc("lvlx", vD=1, rA=0, rB=3), enc("lvrx", vD=2, rA=3, rB=6),
          enc("vor", vD=4, vA=1, vB=2)],
         {"r3": BASE + 15, "r6": 16},
         {"v4": (0xF00A1B2C, 0x3D4E5F60, 0x718293A4, 0xB5C6D7E8)}, MEM),
]


# --- stores -------------------------------------------------------------------------------------

V3 = {"v3": BLK_WORDS}


def stored(offset: int, data: bytes, size: int = 48) -> dict:
    """Memory image of `size` bytes of 0xff at BASE with `data` written at `offset`."""
    out = bytearray(b"\xff" * size)
    out[offset:offset + len(data)] = data
    return {BASE: bytes(out)}




CASES += [
    case("stvx_stores_the_aligned_block", "STVX", st("stvx"), {"r4": BASE, "r5": 0x15, **V3},
         mem_init=FILL, mem_expect=stored(16, BLK)),
    case("stvxl_stores_the_aligned_block", "STVXL", st("stvxl"), {"r4": BASE, "r5": 0x1F, **V3},
         mem_init=FILL, mem_expect=stored(16, BLK)),
    case("stvx_ra0_is_a_literal", "STVX", enc("stvx", vS=3, rA=0, rB=5),
         {"r0": 0x1234, "r5": BASE + 0x10, **V3}, mem_init=FILL, mem_expect=stored(16, BLK)),
    # stvlx stores big-endian bytes 0 .. 15-sh at ea.
    case("stvlx_sh_0", "STVLX", st("stvlx"), {"r4": BASE, "r5": 16, **V3}, mem_init=FILL,
         mem_expect=stored(16, BLK)),
    case("stvlx_sh_5", "STVLX", st("stvlx"), {"r4": BASE, "r5": 5, **V3}, mem_init=FILL,
         mem_expect=stored(5, BLK[:11])),
    case("stvlx_sh_15", "STVLX", st("stvlx"), {"r4": BASE, "r5": 15, **V3}, mem_init=FILL,
         mem_expect=stored(15, BLK[:1])),
    case("stvlxl_matches_stvlx", "STVLXL", st("stvlxl"), {"r4": BASE, "r5": 5, **V3},
         mem_init=FILL, mem_expect=stored(5, BLK[:11])),
    # stvrx stores big-endian bytes 16-sh .. 15 at the start of the block.
    case("stvrx_sh_0_stores_nothing", "STVRX", st("stvrx"), {"r4": BASE, "r5": 16, **V3},
         mem_init=FILL, mem_expect=stored(0, b"")),
    case("stvrx_sh_5", "STVRX", st("stvrx"), {"r4": BASE, "r5": 21, **V3}, mem_init=FILL,
         mem_expect=stored(16, BLK[11:])),
    case("stvrx_sh_15", "STVRX", st("stvrx"), {"r4": BASE, "r5": 31, **V3}, mem_init=FILL,
         mem_expect=stored(16, BLK[1:])),
    case("stvrxl_matches_stvrx", "STVRXL", st("stvrxl"), {"r4": BASE, "r5": 21, **V3},
         mem_init=FILL, mem_expect=stored(16, BLK[11:])),
    # Element stores write the element found at ea & 15 (naturally aligned), nothing else.
    case("stvebx_offset_13", "STVEBX", st("stvebx"), {"r4": BASE, "r5": 13, **V3},
         mem_init=FILL, mem_expect=stored(13, BLK[13:14])),
    case("stvebx_offset_0", "STVEBX", st("stvebx"), {"r4": BASE, "r5": 0, **V3}, mem_init=FILL,
         mem_expect=stored(0, BLK[:1])),
    case("stvebx_offset_15", "STVEBX", st("stvebx"), {"r4": BASE, "r5": 15, **V3}, mem_init=FILL,
         mem_expect=stored(15, BLK[15:])),
    case("stvehx_offset_6", "STVEHX", st("stvehx"), {"r4": BASE, "r5": 6, **V3}, mem_init=FILL,
         mem_expect=stored(6, BLK[6:8])),
    case("stvehx_odd_offset_is_aligned_down", "STVEHX", st("stvehx"), {"r4": BASE, "r5": 7, **V3},
         mem_init=FILL, mem_expect=stored(6, BLK[6:8])),
    case("stvehx_offset_14", "STVEHX", st("stvehx"), {"r4": BASE, "r5": 14, **V3}, mem_init=FILL,
         mem_expect=stored(14, BLK[14:])),
    case("stvewx_offset_0", "STVEWX", st("stvewx"), {"r4": BASE, "r5": 0, **V3}, mem_init=FILL,
         mem_expect=stored(0, BLK[:4])),
    case("stvewx_offset_8", "STVEWX", st("stvewx"), {"r4": BASE, "r5": 8, **V3}, mem_init=FILL,
         mem_expect=stored(8, BLK[8:12])),
    case("stvewx_unaligned_offset_is_aligned_down", "STVEWX", st("stvewx"),
         {"r4": BASE, "r5": 11, **V3}, mem_init=FILL, mem_expect=stored(8, BLK[8:12])),
    case("stvewx_offset_15_writes_the_last_word", "STVEWX", st("stvewx"),
         {"r4": BASE, "r5": 15, **V3}, mem_init=FILL, mem_expect=stored(12, BLK[12:])),
    case("stvewx_second_block", "STVEWX", st("stvewx"), {"r4": BASE, "r5": 20, **V3},
         mem_init=FILL, mem_expect=stored(20, BLK[4:8])),
]


# --- lvsl / lvsr --------------------------------------------------------------------------------

CASES += [
    case("lvsl_sh_0", "LVSL", lv("lvsl"), {"r4": BASE, "r5": 0},
         {"v3": (0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F)}),
    case("lvsl_sh_3", "LVSL", lv("lvsl"), {"r4": BASE, "r5": 3},
         {"v3": (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F101112)}),
    case("lvsl_sh_15", "LVSL", lv("lvsl"), {"r4": BASE, "r5": 15},
         {"v3": (0x0F101112, 0x13141516, 0x1718191A, 0x1B1C1D1E)}),
    case("lvsl_uses_only_the_low_nibble_of_the_sum", "LVSL", lv("lvsl"),
         {"r4": BASE + 0x1000, "r5": 0x23},
         {"v3": (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F101112)}),
    case("lvsl_ra0_is_a_literal", "LVSL", enc("lvsl", vD=3, rA=0, rB=5),
         {"r0": 7, "r5": 3}, {"v3": (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F101112)}),
    case("lvsr_sh_0", "LVSR", lv("lvsr"), {"r4": BASE, "r5": 0},
         {"v3": (0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F)}),
    case("lvsr_sh_3", "LVSR", lv("lvsr"), {"r4": BASE, "r5": 3},
         {"v3": (0x0D0E0F10, 0x11121314, 0x15161718, 0x191A1B1C)}),
    case("lvsr_sh_15", "LVSR", lv("lvsr"), {"r4": BASE, "r5": 15},
         {"v3": (0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10)}),
]


# --- logic and vsel -----------------------------------------------------------------------------

A = (0xFF00FF00, 0x12345678, 0xAAAAAAAA, 0xFFFFFFFF)
B = (0x0FF00FF0, 0xFFFF0000, 0x55555555, 0x00000001)
C = (0xFFFF0000, 0x0F0F0F0F, 0x00000000, 0xFFFFFFFF)


def logic(name: str, expect) -> Case:
    return case("%s_per_bit" % name, name.upper(), enc(name, vD=3, vA=1, vB=2),
                {"v1": A, "v2": B}, {"v3": expect, "v1": A, "v2": B})


CASES += [
    logic("vand", (0x0F000F00, 0x12340000, 0, 1)),
    logic("vandc", (0xF000F000, 0x00005678, 0xAAAAAAAA, 0xFFFFFFFE)),
    logic("vor", (0xFFF0FFF0, 0xFFFF5678, 0xFFFFFFFF, 0xFFFFFFFF)),
    logic("vxor", (0xF0F0F0F0, 0xEDCB5678, 0xFFFFFFFF, 0xFFFFFFFE)),
    logic("vnor", (0x000F000F, 0x0000A987, 0, 0)),
    case("vor_same_register_is_a_move", "VOR", enc("vor", vD=3, vA=1, vB=1), {"v1": A},
         {"v3": A, "v1": A}),
    case("vxor_same_register_clears", "VXOR", enc("vxor", vD=3, vA=1, vB=1), {"v1": A, "v3": B},
         {"v3": (0, 0, 0, 0)}),
    case("vand_destination_is_a_source", "VAND", enc("vand", vD=1, vA=1, vB=2),
         {"v1": A, "v2": B}, {"v1": (0x0F000F00, 0x12340000, 0, 1), "v2": B}),
    case("vandc_complements_vb", "VANDC", enc("vandc", vD=3, vA=2, vB=1), {"v1": A, "v2": B},
         {"v3": (0x00F000F0, 0xEDCB0000, 0x55555555, 0)}),
    case("vsel_picks_vb_where_vc_is_set", "VSEL", enc("vsel", vD=4, vA=1, vB=2, vC=3),
         {"v1": A, "v2": B, "v3": C},
         {"v4": (0x0FF0FF00, 0x1F3F5070, 0xAAAAAAAA, 0x00000001), "v3": C}),
    case("vsel_destination_is_vc", "VSEL", enc("vsel", vD=3, vA=1, vB=2, vC=3),
         {"v1": A, "v2": B, "v3": C},
         {"v3": (0x0FF0FF00, 0x1F3F5070, 0xAAAAAAAA, 0x00000001)}),
]


# --- VSCR (big-endian word 3) -------------------------------------------------------------------

CASES += [
    case("mfvscr_reports_nj_by_default", "MFVSCR", enc("mfvscr", vD=3), {"v3": (1, 2, 3, 4)},
         {"v3": (0, 0, 0, 0x00010000)}),
    case("mfvscr_reports_sat", "MFVSCR", enc("mfvscr", vD=3), {"vscr_sat": 1},
         {"v3": (0, 0, 0, 0x00010001)}),
    case("mfvscr_with_nj_clear", "MFVSCR", enc("mfvscr", vD=3), {"vscr_nj": 0, "vscr_sat": 1},
         {"v3": (0, 0, 0, 1)}),
    case("mtvscr_sets_nj_and_sat_from_word_3", "MTVSCR", enc("mtvscr", vB=2),
         {"v2": (0, 0, 0, 0x00010001), "vscr_nj": 0},
         {"vscr_nj": 1, "vscr_sat": 1}),
    case("mtvscr_ignores_the_other_words", "MTVSCR", enc("mtvscr", vB=2),
         {"v2": (0x00010001, 0xFFFFFFFF, 0xFFFFFFFF, 0), "vscr_nj": 1, "vscr_sat": 1},
         {"vscr_nj": 0, "vscr_sat": 0}),
    case("mtvscr_word_0_alone_changes_nothing", "MTVSCR", enc("mtvscr", vB=2),
         {"v2": (0x00010001, 0, 0, 0), "vscr_nj": 0},
         {"vscr_nj": 0, "vscr_sat": 0}),
    case("mtvscr_then_mfvscr_clears_nj", "MTVSCR MFVSCR",
         [enc("mtvscr", vB=2), enc("mfvscr", vD=3)],
         {"v2": (0, 0, 0, 1)},
         {"vscr_nj": 0, "vscr_sat": 1, "v3": (0, 0, 0, 1)}),
]

"""Integer-group cases: arithmetic, logic, compare, select, multiply, byte, shift and rotate instructions.

Every case runs one instruction (or a few) on registers whose four words differ, so a lane that is
mapped or masked wrongly shows. Source registers are $3, $4 and $5, the result goes to $6 unless the
instruction reads its old RT ($5 then). The expected words are written out by hand from the SPU ISA 1.2
rules below; none of them comes from running the emitter.

- Register forms operate on each word (w), halfword (h) or byte (b) separately, with wrap-around.
- Immediates: the RI10 forms sign-extend I10 to the element width; the byte forms (`andbi`, `orbi`,
  `xorbi`, `ceqbi`, `cgtbi`, `clgtbi`) use its low 8 bits; the unsigned compares `clgti`, `clgthi` and
  `clgtbi` compare against the sign-extended value as unsigned. `mpyi` multiplies by the sign-extended
  I10, `mpyui` by that value taken as an unsigned halfword. `il` sign-extends I16, `ilhu` shifts U16 left
  by 16, `ila` zero-extends I18, `iohl` ORs U16 into the low halfword.
- `sf`, `sfh`, `sfi` and `sfhi` compute the second operand minus the first (`sfi`: I10 - RA).
- `addx`, `cgx`, `sfx` and `bgx` take their carry-in from bit 31 (the least significant bit) of each
  RT word; `sfx` is RB - RA - 1 + carry-in. `bg` gives 1 when RB - RA does not borrow, 0 when it does.
- The multiplies use the low halfword of each word (signed for `mpy`, `mpys`, `mpya`, `mpyi`; unsigned
  for `mpyu`, `mpyui`); `mpyh` multiplies the high halfword of RA by the low halfword of RB and shifts
  the product left by 16; `mpys` shifts the product right by 16 arithmetically; the `mpyhh` family uses
  the high halfwords.
- `sumb` puts the sum of RB's four bytes in the upper halfword of a word and the sum of RA's in the
  lower one. `selb` takes RB bits where RC has ones and RA bits where it has zeros.
- Shifts and rotates use the count of the same element of RB (or the I7 immediate): the low 6 bits for
  word shifts and 5 for halfword shifts (a count above the element width gives 0), 5 and 4 bits for
  rotates. The right shifts (`rotm`, `rothm`, `rotma`, `rotmah`) use the negated count, so -n shifts right
  by n; the arithmetic ones fill with the sign when the count exceeds the width.
- `heq`, `hgt`, `hlgt` and their immediate forms compare the preferred words (`hgt` signed, `hlgt`
  unsigned; the immediate is sign-extended) and halt when the condition holds.
"""

from ps3recomp import spu
from tests import spu_asm

from . import CODE_BASE, Case
from .cases_control import at, mark, ran

CH_REF = "SPU ISA 1.2, Constant-Formation Instructions"
INT_REF = "SPU ISA 1.2, Integer and Logical Instructions"
CMP_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions"
SHIFT_REF = "SPU ISA 1.2, Shift and Rotate Instructions"

M = 0xFFFFFFFF
SENT = 0x5A5A5A5A
SENTINEL = (SENT, SENT, SENT, SENT)


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def t(name: str, ref: str, words, init: dict, expect: dict, **kwargs) -> Case:
    """A case from register numbers (not `rN` keys)."""
    if isinstance(words, int):
        words = [words]
    return Case(name, ref, tuple(words), {"r%d" % r: v for r, v in init.items()},
                {"r%d" % r: v for r, v in expect.items()}, **kwargs)


def rr(mnemonic: str, ref: str, a, b, out, name: str | None = None) -> Case:
    """RT = op(RA, RB) with $3, $4 -> $6."""
    return t(name or mnemonic, ref, enc(mnemonic, RT=6, RA=3, RB=4), {3: a, 4: b}, {6: out})


def acc(mnemonic: str, ref: str, a, b, rt, out, name: str | None = None) -> Case:
    """Forms that read the old RT: $3, $4 and RT = $5."""
    return t(name or mnemonic, ref, enc(mnemonic, RT=5, RA=3, RB=4), {3: a, 4: b, 5: rt}, {5: out})


def ri(mnemonic: str, ref: str, a, out, name: str, **fields) -> Case:
    """RT = op(RA, immediate) with $3 -> $6."""
    return t(name, ref, enc(mnemonic, RT=6, RA=3, **fields), {3: a}, {6: out})


def un(mnemonic: str, ref: str, a, out, name: str | None = None) -> Case:
    return t(name or mnemonic, ref, enc(mnemonic, RT=6, RA=3), {3: a}, {6: out})


def splat(word: int) -> tuple[int, int, int, int]:
    return (word,) * 4


CASES: list[Case] = []

# --- Constant formation ---------------------------------------------------------------------------

CASES += [
    t("il_sign_extends", CH_REF + ": il", enc("il", RT=6, I16=-2), {}, {6: splat(0xFFFFFFFE)}),
    t("il_positive", CH_REF + ": il", enc("il", RT=6, I16=0x7FFF), {}, {6: splat(0x00007FFF)}),
    t("ilh_fills_every_halfword", CH_REF + ": ilh", enc("ilh", RT=6, U16=0x8001), {}, {6: splat(0x80018001)}),
    t("ilhu_shifts_left_16", CH_REF + ": ilhu", enc("ilhu", RT=6, U16=0x1234), {}, {6: splat(0x12340000)}),
    t("ilhu_high_bit", CH_REF + ": ilhu", enc("ilhu", RT=6, U16=0x8000), {}, {6: splat(0x80000000)}),
    t("ila_zero_extends_18_bits", CH_REF + ": ila", enc("ila", RT=6, I18=0x3FFFF), {}, {6: splat(0x0003FFFF)}),
    t("ila_middle", CH_REF + ": ila", enc("ila", RT=6, I18=0x12345), {}, {6: splat(0x00012345)}),
    t("iohl_ors_the_low_halfword", CH_REF + ": iohl", enc("iohl", RT=6, U16=0x8001),
      {6: (0x12340000, 0xFFFF0000, 0x00001111, 0x80000000)},
      {6: (0x12348001, 0xFFFF8001, 0x00009111, 0x80008001)}),
]

# --- Add and subtract -----------------------------------------------------------------------------

CASES += [
    rr("a", INT_REF + ": a", (1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000), (2, 1, 1, 0x80000000),
       (3, 0, 0x80000000, 0)),
    t("a_destination_may_be_a_source", INT_REF + ": a", enc("a", RT=3, RA=3, RB=3),
      {3: (1, 0x80000000, 0x40000000, 0xFFFFFFFF)}, {3: (2, 0, 0x80000000, 0xFFFFFFFE)}),
    rr("ah", INT_REF + ": ah", (0x0001FFFF, 0x7FFF8000, 0x12345678, 0xFFFFFFFF),
       (0x00020001, 0x00018000, 0x11111111, 0x00010001),
       (0x00030000, 0x80000000, 0x23456789, 0x00000000)),
    rr("sf", INT_REF + ": sf", (1, 5, 0, 0x80000000), (3, 2, 0, 0x7FFFFFFF),
       (2, 0xFFFFFFFD, 0, 0xFFFFFFFF)),
    rr("sfh", INT_REF + ": sfh", (0x00010002, 0x00000001, 0x7FFF0000, 0xFFFF8000),
       (0x00030001, 0x00000000, 0x80000000, 0x00007FFF),
       (0x0002FFFF, 0x0000FFFF, 0x00010000, 0x0001FFFF)),
    ri("ai", INT_REF + ": ai", (0, 1, 0x80000000, 0x7FFFFFFF), (0xFFFFFFFF, 0, 0x7FFFFFFF, 0x7FFFFFFE),
       "ai_negative_immediate", I10=-1),
    ri("ai", INT_REF + ": ai", (1, 0xFFFFFE01, 0, 0x7FFFFE01), (0x200, 0, 0x1FF, 0x80000000),
       "ai_largest_immediate", I10=511),
    ri("ahi", INT_REF + ": ahi", (0x00010002, 0x00000001, 0x80007FFF, 0xFFFE0000),
       (0xFFFF0000, 0xFFFEFFFF, 0x7FFE7FFD, 0xFFFCFFFE), "ahi_negative_immediate", I10=-2),
    ri("sfi", INT_REF + ": sfi", (0, 1, 0xFFFFFFFF, 0x80000000), (0xFFFFFFFF, 0xFFFFFFFE, 0, 0x7FFFFFFF),
       "sfi_immediate_minus_register", I10=-1),
    ri("sfhi", INT_REF + ": sfhi", (0x00050006, 0x0000FFFF, 0x80000001, 0x00040000),
       (0x0000FFFF, 0x00050006, 0x80050004, 0x00010005), "sfhi_immediate_minus_register", I10=5),
    acc("addx", INT_REF + ": addx", (1, 0xFFFFFFFF, 0, 0x7FFFFFFF), (1, 0, 0, 0),
        (1, 1, 0, 0xFFFFFFFE), (3, 0, 0, 0x7FFFFFFF)),
    rr("cg", INT_REF + ": cg", (0xFFFFFFFF, 0x80000000, 1, 0), (1, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF),
       (1, 1, 0, 0)),
    acc("cgx", INT_REF + ": cgx", (0xFFFFFFFF, 1, 0x80000000, 0), (0, 0xFFFFFFFE, 0x7FFFFFFF, 0xFFFFFFFF),
        (1, 1, 0, 1), (1, 1, 0, 1)),
    acc("sfx", INT_REF + ": sfx", (1, 1, 0, 5), (3, 3, 0, 5), (1, 0, 0, 0xFFFFFFFF), (2, 1, 0xFFFFFFFF, 0)),
    rr("bg", INT_REF + ": bg", (1, 2, 0x80000000, 0), (2, 1, 0x7FFFFFFF, 0), (1, 0, 0, 1)),
    acc("bgx", INT_REF + ": bgx", (5, 5, 6, 0), (5, 5, 5, 0), (0, 1, 1, 1), (0, 1, 0, 1)),
]

# --- Multiply -------------------------------------------------------------------------------------

CASES += [
    rr("mpy", INT_REF + ": mpy", (0x0000FFFF, 0x12347FFF, 0x00008000, 0xABCD0003),
       (0x00000002, 0xFFFF0002, 0x00008000, 0x1111FFFE),
       (0xFFFFFFFE, 0x0000FFFE, 0x40000000, 0xFFFFFFFA)),
    rr("mpyu", INT_REF + ": mpyu", (0x0000FFFF, 0x12347FFF, 0x00008000, 0xABCD0003),
       (0x1234FFFF, 0xFFFF0002, 0x00008000, 0x1111FFFE),
       (0xFFFE0001, 0x0000FFFE, 0x40000000, 0x0002FFFA)),
    rr("mpyh", INT_REF + ": mpyh", (0x00020000, 0xFFFF0000, 0x80000000, 0x7FFF1234),
       (0x00000003, 0x00000005, 0x0000FFFF, 0xABCD0002),
       (0x00060000, 0xFFFB0000, 0x80000000, 0xFFFE0000)),
    rr("mpys", INT_REF + ": mpys", (0x00004000, 0x0000FFFF, 0x00008000, 0x00000003),
       (0x00004000, 0x00000002, 0x00007FFF, 0x00000004),
       (0x00001000, 0xFFFFFFFF, 0xFFFFC000, 0x00000000)),
    rr("mpyhh", INT_REF + ": mpyhh", (0x00020000, 0x80000000, 0xFFFF0000, 0x7FFF0000),
       (0x00030000, 0x80000000, 0x00020000, 0x7FFF0000),
       (6, 0x40000000, 0xFFFFFFFE, 0x3FFF0001)),
    acc("mpyhha", INT_REF + ": mpyhha", (0x00020000, 0xFFFF0000, 0x80000000, 0x00010000),
        (0x00030000, 0x00010000, 0x80000000, 0xFFFF0000), (10, 5, 0xC0000000, 0),
        (0x10, 4, 0, 0xFFFFFFFF)),
    rr("mpyhhu", INT_REF + ": mpyhhu", (0xFFFF0000, 0x00020000, 0x00010000, 0x1234FFFF),
       (0xFFFF0000, 0x80000000, 0x00010000, 0x00005678), (0xFFFE0001, 0x00010000, 1, 0)),
    acc("mpyhhau", INT_REF + ": mpyhhau", (0xFFFF0000, 0x00020000, 0x00030000, 0x00040000),
        (0xFFFF0000, 0x00010000, 0x00040000, 0), (1, 0xFFFFFFFF, 0, 7), (0xFFFE0002, 1, 12, 7)),
    ri("mpyi", INT_REF + ": mpyi", (0x00000002, 0x0000FFFF, 0x00007FFF, 0xFFFF8000),
       (0xFFFFFFFA, 3, 0xFFFE8003, 0x00018000), "mpyi_signed_immediate", I10=-3),
    ri("mpyui", INT_REF + ": mpyui", (0x00000002, 0x0000FFFF, 0x12340000, 0x00008000),
       (0x0001FFFE, 0xFFFE0001, 0, 0x7FFF8000), "mpyui_immediate_is_an_unsigned_halfword", I10=-1),
    t("mpya", INT_REF + ": mpya", enc("mpya", RT4=6, RA=3, RB=4, RC=5),
      {3: (0x00000002, 0x0000FFFF, 0x00008000, 0x00000003),
       4: (0x00000003, 0x00000002, 0x00008000, 0x0000FFFE),
       5: (1, 0x10, 0xC0000000, 0)},
      {6: (7, 14, 0, 0xFFFFFFFA)}),
]

# --- Byte operations and counts -------------------------------------------------------------------

CASES += [
    rr("avgb", INT_REF + ": avgb", (0x00FF0102, 0xFFFFFFFE, 0x10203040, 0x80007F01),
       (0x01FE0203, 0xFFFFFFFF, 0x20406080, 0x7FFF8000),
       (0x01FF0203, 0xFFFFFFFF, 0x18304860, 0x80808001)),
    rr("absdb", INT_REF + ": absdb", (0x00FF0102, 0x80007F10, 0x10203040, 0xAABBCCDD),
       (0xFF000201, 0x7FFF8010, 0x40302010, 0xAABBCCDD),
       (0xFFFF0101, 0x01FF0100, 0x30101030, 0)),
    rr("sumb", INT_REF + ": sumb", (0x01020304, 0xFFFFFFFF, 0x00000000, 0x80808080),
       (0x0A0B0C0D, 0x01010101, 0xFFFFFFFF, 0x00000001),
       (0x002E000A, 0x000403FC, 0x03FC0000, 0x00010200)),
    un("cntb", INT_REF + ": cntb", (0x00FF0F01, 0x80C0E0F0, 0x55AA3C7F, 0x12345678),
       (0x00080401, 0x01020304, 0x04040407, 0x02030404)),
    un("clz", INT_REF + ": clz", (0, 1, 0x80000000, 0x0000FFFF), (32, 31, 0, 16), "clz"),
    un("clz", INT_REF + ": clz", (0x7FFFFFFF, 0x00010000, 0x0F000000, 0xFFFFFFFF), (1, 15, 4, 0),
       "clz_other_positions"),
    un("xsbh", INT_REF + ": xsbh", (0x0080007F, 0x12FF3480, 0, 0xFFFF0100),
       (0xFF80007F, 0xFFFFFF80, 0, 0xFFFF0000)),
    un("xshw", INT_REF + ": xshw", (0x00008000, 0xFFFF7FFF, 0x12345678, 0x0001FFFF),
       (0xFFFF8000, 0x00007FFF, 0x00005678, 0xFFFFFFFF)),
    un("xswd", INT_REF + ": xswd", (0x12345678, 0x80000001, 0, 0x7FFFFFFF),
       (0xFFFFFFFF, 0x80000001, 0, 0x7FFFFFFF)),
    un("xswd", INT_REF + ": xswd", (0xFFFFFFFF, 2, 0x80000000, 0xFFFFFFFE), (0, 2, 0xFFFFFFFF, 0xFFFFFFFE),
       "xswd_other_signs"),
]

# --- Logical --------------------------------------------------------------------------------------

LA = (0xF0F0F0F0, 0x12345678, 0xFFFFFFFF, 0)
LB = (0xFF00FF00, 0xFFFF0000, 0x0F0F0F0F, 0xFFFFFFFF)

CASES += [
    rr("and", INT_REF + ": and", LA, LB, (0xF000F000, 0x12340000, 0x0F0F0F0F, 0)),
    rr("andc", INT_REF + ": andc", LA, LB, (0x00F000F0, 0x00005678, 0xF0F0F0F0, 0)),
    rr("or", INT_REF + ": or", LA, LB, (0xFFF0FFF0, 0xFFFF5678, 0xFFFFFFFF, 0xFFFFFFFF)),
    rr("orc", INT_REF + ": orc", LA, LB, (0xF0FFF0FF, 0x1234FFFF, 0xFFFFFFFF, 0)),
    rr("xor", INT_REF + ": xor", LA, LB, (0x0FF00FF0, 0xEDCB5678, 0xF0F0F0F0, 0xFFFFFFFF)),
    rr("nand", INT_REF + ": nand", LA, LB, (0x0FFF0FFF, 0xEDCBFFFF, 0xF0F0F0F0, 0xFFFFFFFF)),
    rr("nor", INT_REF + ": nor", LA, LB, (0x000F000F, 0x0000A987, 0, 0)),
    rr("eqv", INT_REF + ": eqv", LA, LB, (0xF00FF00F, 0x1234A987, 0x0F0F0F0F, 0)),
    ri("andi", INT_REF + ": andi", (0x12345678, 0xFFFFFFFF, 0x000000FF, 0x80000100),
       (0x12345600, 0xFFFFFF00, 0, 0x80000100), "andi_sign_extended_immediate", I10=-256),
    ri("ori", INT_REF + ": ori", (0, 0x0000000F, 0x80000000, 1), (0xF0, 0xFF, 0x800000F0, 0xF1),
       "ori_positive_immediate", I10=0x0F0),
    ri("ori", INT_REF + ": ori", (0, 1, 0x12345678, 0xFFFFFFFE), (0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFE),
       "ori_sign_extended_immediate", I10=-2),
    ri("xori", INT_REF + ": xori", (0, 0xFFFFFFFF, 0x12345678, 0x0F0F0F0F),
       (0xFFFFFFFF, 0, 0xEDCBA987, 0xF0F0F0F0), "xori_sign_extended_immediate", I10=-1),
    ri("andhi", INT_REF + ": andhi", (0x12345678, 0xFFFF0001, 0x00FF8000, 0x7FFF0003),
       (0x12345678, 0xFFFE0000, 0x00FE8000, 0x7FFE0002), "andhi_halfword_immediate", I10=-2),
    ri("orhi", INT_REF + ": orhi", (0x12345678, 0xFFFF0001, 0, 0x8000FF00),
       (0x12F456F8, 0xFFFF00F1, 0x00F000F0, 0x80F0FFF0), "orhi_halfword_immediate", I10=0x0F0),
    ri("xorhi", INT_REF + ": xorhi", (0x12345678, 0xFFFF0000, 0, 0x00FF8001),
       (0xEDCBA987, 0x0000FFFF, 0xFFFFFFFF, 0xFF007FFE), "xorhi_halfword_immediate", I10=-1),
    ri("andbi", INT_REF + ": andbi", (0x12345678, 0xFFFFFFFF, 0x0F0F0F0F, 0xF0F0F0F0),
       (0x10305070, 0xF0F0F0F0, 0, 0xF0F0F0F0), "andbi_uses_the_low_8_bits", I10=-16),
    ri("orbi", INT_REF + ": orbi", (0, 0x12345678, 0x7E7E7E7E, 0xFFFFFFFF),
       (0x81818181, 0x93B5D7F9, 0xFFFFFFFF, 0xFFFFFFFF), "orbi_byte_immediate", I10=0x081),
    ri("xorbi", INT_REF + ": xorbi", (0x12345678, 0, 0xFFFFFFFF, 0x80FF0001),
       (0xEDCBA987, 0xFFFFFFFF, 0, 0x7F00FFFE), "xorbi_byte_immediate", I10=-1),
    t("selb", INT_REF + ": selb", enc("selb", RT4=6, RA=3, RB=4, RC=5),
      {3: (0xAAAAAAAA, 0x11111111, 0x12345678, 0), 4: (0x55555555, 0x22222222, 0xFFFFFFFF, 0xFFFFFFFF),
       5: (0xFFFF0000, 0, 0xF0F0F0F0, 0xFFFFFFFF)},
      {6: (0x5555AAAA, 0x11111111, 0xF2F4F6F8, 0xFFFFFFFF)}),
]

# --- Compare --------------------------------------------------------------------------------------

CASES += [
    rr("ceqb", INT_REF + ": ceqb", (0x01020304, 0xFFFFFFFF, 0, 0x11223344),
       (0x01FF0304, 0xFFFFFFFE, 0x00FF0000, 0x11223344), (0xFF00FFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFFFFFF)),
    rr("ceqh", INT_REF + ": ceqh", (0x00010002, 0xFFFF8000, 0x12345678, 0),
       (0x00010003, 0xFFFF8001, 0x12345678, 0x00010000), (0xFFFF0000, 0xFFFF0000, 0xFFFFFFFF, 0x0000FFFF)),
    rr("ceq", INT_REF + ": ceq", (1, 2, 0xFFFFFFFF, 0), (1, 3, 0xFFFFFFFF, 0x80000000), (M, 0, M, 0)),
    rr("cgtb", INT_REF + ": cgtb", (0x7F800102, 0xFF00FE01, 0x80808080, 0x01020304),
       (0x80007F02, 0xFE01FF00, 0x7F7F7F7F, 0x01010505), (0xFF000000, 0xFF0000FF, 0, 0x00FF0000)),
    rr("cgth", INT_REF + ": cgth", (0x7FFF8000, 0xFFFF0001, 0x00010000, 0), (0x80007FFF, 0xFFFE0002, 0x0000FFFF, 0),
       (0xFFFF0000, 0xFFFF0000, M, 0)),
    rr("cgt", INT_REF + ": cgt", (1, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0x7FFFFFFF, 0, 0x80000000),
       (M, 0, 0, M)),
    rr("clgtb", INT_REF + ": clgtb", (0x7F800102, 0xFF00FE01, 0x80808080, 0x01020304),
       (0x80007F02, 0xFE01FF00, 0x7F7F7F7F, 0x01010505), (0x00FF0000, 0xFF0000FF, M, 0x00FF0000)),
    rr("clgth", INT_REF + ": clgth", (0x7FFF8000, 0xFFFF0001, 0x00010000, 0x80000000),
       (0x80007FFF, 0xFFFE0002, 0x0000FFFF, 0x7FFF0000), (0x0000FFFF, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000)),
    rr("clgt", INT_REF + ": clgt", (1, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF), (0, 0x7FFFFFFF, 0, 0x80000000),
       (M, M, M, 0)),
    ri("ceqi", INT_REF + ": ceqi", (0xFFFFFFFF, 0x0000FFFF, 1, 0), (M, 0, 0, 0),
       "ceqi_sign_extended_immediate", I10=-1),
    ri("ceqhi", INT_REF + ": ceqhi", (0xFFFF0001, 0x0000FFFF, 0x12345678, 0xFFFFFFFF),
       (0xFFFF0000, 0x0000FFFF, 0, M), "ceqhi_halfword_immediate", I10=-1),
    ri("ceqbi", INT_REF + ": ceqbi", (0xFF00FF01, 0x01020304, 0xFFFFFFFF, 0), (0xFF00FF00, 0, M, 0),
       "ceqbi_negative_immediate", I10=-1),
    ri("ceqbi", INT_REF + ": ceqbi", (0x05010501, 0x00000005, 0x05050505, 0x01050105),
       (0xFF00FF00, 0x000000FF, M, 0x00FF00FF), "ceqbi_low_8_bits_of_the_immediate", I10=0x105),
    ri("cgti", INT_REF + ": cgti", (0, 0xFFFFFFFF, 0xFFFFFFFE, 0x80000000), (M, 0, 0, 0),
       "cgti_sign_extended_immediate", I10=-1),
    ri("cgthi", INT_REF + ": cgthi", (0xFFFF0000, 0xFFFE8000, 0x7FFF0001, 0xFFFD0002),
       (M, 0, M, 0x0000FFFF), "cgthi_halfword_immediate", I10=-2),
    ri("cgtbi", INT_REF + ": cgtbi", (0xFF00FE7F, 0x80010203, 0, 0xFFFFFFFF),
       (0x00FF00FF, 0x00FFFFFF, M, 0), "cgtbi_compares_signed_bytes", I10=-1),
    ri("clgti", INT_REF + ": clgti", (0xFFFFFFFE, 0xFFFFFFFF, 0, 0x7FFFFFFF), (0, 0, 0, 0),
       "clgti_sign_extended_immediate_is_the_largest_value", I10=-1),
    ri("clgti", INT_REF + ": clgti", (4, 5, 6, 0xFFFFFFFF), (0, 0, M, M), "clgti_small_immediate", I10=5),
    ri("clgthi", INT_REF + ": clgthi", (0xFFFE0000, 0xFFFF0001, 0, 0), (0, 0, 0, 0),
       "clgthi_sign_extended_immediate_is_the_largest_value", I10=-1),
    ri("clgthi", INT_REF + ": clgthi", (0x00020003, 0x00040005, 0xFFFF0000, 0x00000004),
       (0, M, 0xFFFF0000, 0x0000FFFF), "clgthi_small_immediate", I10=3),
    ri("clgtbi", INT_REF + ": clgtbi", (0xFFFEFDFC, 0x7F80FF00, 0, 0xFF000000), (0, 0, 0, 0),
       "clgtbi_negative_immediate_is_0xff", I10=-1),
    ri("clgtbi", INT_REF + ": clgtbi", (0x7F80017E, 0x7FFFFF00, 0, 0x80808080),
       (0x00FF0000, 0x00FFFF00, 0, M), "clgtbi_low_8_bits_of_the_immediate", I10=0x17F),
]

# --- Shifts and rotates ---------------------------------------------------------------------------

CASES += [
    rr("shl", SHIFT_REF + ": shl", (1, 0x80000001, 0xFFFFFFFF, 0x12345678), (0, 1, 31, 32),
       (1, 2, 0x80000000, 0)),
    rr("shl", SHIFT_REF + ": shl", (0xFFFFFFFF, 0x12345678, 0x80000000, 1), (0x3F, 0x40, 0xFFFFFFFF, 4),
       (0, 0x12345678, 0, 0x10), "shl_count_is_six_bits"),
    rr("shlh", SHIFT_REF + ": shlh", (0x00018001, 0xFFFFFFFF, 0x12345678, 0x0001FFFF),
       (0x00010004, 0x000F0010, 0x00010020, 0x00110012),
       (0x00020010, 0x80000000, 0x24685678, 0)),
    rr("rot", SHIFT_REF + ": rot", (0x80000001, 0x12345678, 0xF0000000, 0x00000001), (1, 4, 0x20, 0x1F),
       (0x00000003, 0x23456781, 0xF0000000, 0x80000000)),
    rr("roth", SHIFT_REF + ": roth", (0x80010001, 0x12345678, 0xF000000F, 0x00010001),
       (0x00010004, 0x00000008, 0x0010000F, 0x00030003),
       (0x00030010, 0x12347856, 0xF0008007, 0x00080008)),
    rr("rotm", SHIFT_REF + ": rotm", (0x80000000, 0x80000000, 0xFFFFFFFF, 0x12345678),
       (0xFFFFFFFF, 0xFFFFFFE1, 0, 0xFFFFFFE0), (0x40000000, 1, 0xFFFFFFFF, 0)),
    rr("rotm", SHIFT_REF + ": rotm", (0x12345678, 0x87654321, 0xFFFFFFFF, 0xFFFFFFFF),
       (0xFFFFFFFC, 0xFFFFFFC0, 1, 0xFFFFFFDF), (0x01234567, 0x87654321, 0, 0), "rotm_negated_count_is_six_bits"),
    rr("rothm", SHIFT_REF + ": rothm", (0x80008000, 0x80008000, 0xFFFF1234, 0x12345678),
       (0xFFFFFFF1, 0x0000FFF0, 0xFFFC0001, 0xFFF8FFFC),
       (0x40000001, 0x80000000, 0x0FFF0000, 0x00120567)),
    rr("rotma", SHIFT_REF + ": rotma", (0x80000000, 0x80000000, 0x7FFFFFFF, 0x80000000),
       (0xFFFFFFFF, 0xFFFFFFE1, 0xFFFFFFFC, 0xFFFFFFE0), (0xC0000000, M, 0x07FFFFFF, M)),
    rr("rotma", SHIFT_REF + ": rotma", (0x7FFFFFFF, 0x80000000, 0x12345678, 0xFFFFFFF0),
       (0xFFFFFFD8, 1, 0, 0xFFFFFFF0), (0, M, 0x12345678, M), "rotma_count_beyond_the_width_fills_with_the_sign"),
    rr("rotmah", SHIFT_REF + ": rotmah", (0x80008000, 0x7FFF8000, 0x80010000, 0x1234FEDC),
       (0xFFFFFFF1, 0xFFE0FFF0, 0x00010000, 0xFFFCFFF8),
       (0xC000FFFF, 0x7FFFFFFF, 0xFFFF0000, 0x0123FFFE)),
    ri("shlhi", SHIFT_REF + ": shlhi", (0x80018001, 0x12345678, 0xFFFFFFFF, 0x00010002),
       (0x00020002, 0x2468ACF0, 0xFFFEFFFE, 0x00020004), "shlhi", I7=1),
    ri("shlhi", SHIFT_REF + ": shlhi", (0x80018001, 0x12345678, 0xFFFFFFFF, 0x00010002), (0, 0, 0, 0),
       "shlhi_negative_immediate_is_a_large_count", I7=-1),
    ri("shli", SHIFT_REF + ": shli", (1, 0x12345678, 0xF0000000, 0x0FFFFFFF),
       (0x10, 0x23456780, 0, 0xFFFFFFF0), "shli", I7=4),
    ri("shli", SHIFT_REF + ": shli", (1, 0x12345678, 0xF0000000, 0x0FFFFFFF), (0, 0, 0, 0),
       "shli_negative_immediate_is_a_large_count", I7=-1),
    ri("rothi", SHIFT_REF + ": rothi", (0x80018001, 0x12345678, 0x00010000, 0xFFFF0000),
       (0x00030003, 0x2468ACF0, 0x00020000, 0xFFFF0000), "rothi", I7=1),
    ri("rothi", SHIFT_REF + ": rothi", (0x80018001, 0x12345678, 0x00010000, 0xFFFF0000),
       (0x00030003, 0x2468ACF0, 0x00020000, 0xFFFF0000), "rothi_negative_immediate_uses_the_low_4_bits", I7=-15),
    ri("roti", SHIFT_REF + ": roti", (0x12345678, 0x80000001, 0xF0000000, 0x0000000F),
       (0x23456781, 0x00000018, 0x0000000F, 0x000000F0), "roti", I7=4),
    ri("rotmi", SHIFT_REF + ": rotmi", (0x12345678, 0x80000000, 0xFFFFFFFF, 1),
       (0x01234567, 0x08000000, 0x0FFFFFFF, 0), "rotmi", I7=-4),
    ri("rotmi", SHIFT_REF + ": rotmi", (0x12345678, 0x80000000, 0xFFFFFFFF, 1), (0, 0, 0, 0),
       "rotmi_positive_immediate_shifts_out_everything", I7=4),
    ri("rothmi", SHIFT_REF + ": rothmi", (0x12345678, 0x80008000, 0xFFFFFFFF, 1),
       (0x01230567, 0x08000800, 0x0FFF0FFF, 0), "rothmi", I7=-4),
    ri("rothmi", SHIFT_REF + ": rothmi", (0x12345678, 0x80008000, 0xFFFFFFFF, 1), (0, 0, 0, 0),
       "rothmi_positive_immediate_shifts_out_everything", I7=1),
    ri("rotmai", SHIFT_REF + ": rotmai", (0x12345678, 0x80000000, 0xFFFFFFF0, 0x7FFFFFFF),
       (0x01234567, 0xF8000000, 0xFFFFFFFF, 0x07FFFFFF), "rotmai", I7=-4),
    ri("rotmai", SHIFT_REF + ": rotmai", (0x80000000, 0x7FFFFFFF, 0xFFFFFFFF, 0),
       (M, 0, M, 0), "rotmai_positive_immediate_fills_with_the_sign", I7=1),
    ri("rotmahi", SHIFT_REF + ": rotmahi", (0x12348000, 0x7FFFFFFF, 0xF0F00F0F, 0xFFFF0000),
       (0x0123F800, 0x07FFFFFF, 0xFF0F00F0, 0xFFFF0000), "rotmahi", I7=-4),
    ri("rotmahi", SHIFT_REF + ": rotmahi", (0x80007FFF, 0x00018000, 0xFFFF0000, 0x12348000),
       (0xFFFF0000, 0x0000FFFF, 0xFFFF0000, 0x0000FFFF), "rotmahi_positive_immediate_fills_with_the_sign", I7=1),
]

# --- Conditional halts ----------------------------------------------------------------------------
#
# A halt that is not taken falls through to a `brsl $10,$+4` marker, which must have run; a taken one
# ends the case at once (the marker leaves its sentinel) and the harness records its site. Only the
# preferred words are compared; words 1 to 3 differ in the taken cases and agree in the other ones.


def halt_case(mnemonic: str, name: str, a: int, b: int | None, taken: bool, **fields) -> Case:
    word = enc(mnemonic, RA=3, **fields) if b is None else enc(mnemonic, RA=3, RB=4, **fields)
    init = {3: (a, 1, 2, 3) if taken else (a, 7, 7, 7), 10: SENTINEL}
    if b is not None:
        init[4] = (b, 9, 9, 9) if taken else (b, 7, 7, 7)
    if taken:
        return t(name, CMP_REF + ": " + mnemonic, [word, mark(1, 10)], init, {10: SENTINEL},
                 channels={"halts": [at(0)], "stops": []})
    return t(name, CMP_REF + ": " + mnemonic, [word, mark(1, 10)], init, {10: ran(1)})


CASES += [
    halt_case("heq", "heq_taken_when_preferred_words_are_equal", 5, 5, True),
    halt_case("heq", "heq_not_taken_when_they_differ", 5, 6, False),
    halt_case("hgt", "hgt_taken_signed", 1, 0xFFFFFFFF, True),
    halt_case("hgt", "hgt_not_taken_signed", 0xFFFFFFFF, 1, False),
    halt_case("hgt", "hgt_not_taken_when_equal", 3, 3, False),
    halt_case("hlgt", "hlgt_taken_unsigned", 0xFFFFFFFF, 1, True),
    halt_case("hlgt", "hlgt_not_taken_unsigned", 1, 0xFFFFFFFF, False),
    halt_case("heqi", "heqi_taken_with_sign_extended_immediate", 0xFFFFFFFF, None, True, I10=-1),
    halt_case("heqi", "heqi_not_taken_for_the_zero_extended_value", 0x000003FF, None, False, I10=-1),
    halt_case("hgti", "hgti_taken_signed", 0, None, True, I10=-1),
    halt_case("hgti", "hgti_not_taken_signed", 0xFFFFFFFE, None, False, I10=-1),
    halt_case("hlgti", "hlgti_taken_unsigned", 0x80000000, None, True, I10=1),
    halt_case("hlgti", "hlgti_not_taken_against_the_sign_extended_immediate", 0xFFFFFFFE, None, False, I10=-1),
]


def _check_coverage() -> None:
    """Every `int` mnemonic and the six halts have a case; a missing one fails the generator."""
    seen = {spu.decode(word, CODE_BASE).spec.mnemonic for case in CASES for word in case.words}
    wanted = {s.mnemonic for s in spu.ALL_OPS if s.group == "int" or "halt" in s.flags}
    missing = sorted(wanted - seen)
    if missing:
        raise ValueError("cases_int: no case for %s" % ", ".join(missing))


_check_coverage()

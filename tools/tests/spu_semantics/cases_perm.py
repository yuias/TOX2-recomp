"""Permute-group cases: quadword shifts and rotates, shuffle, generate controls, select masks, gathers.

Source registers are $3, $4 and $5, the result goes to $6 (or $3 where a case tests aliasing). The
expected words are written out by hand from the SPU ISA 1.2 pseudocode below; none of them comes from
running the emitter. Registers are shown as four big-endian words, so the bytes of `QA` read
00 01 02 ... 0f from left to right.

- Quadword shifts and rotates act on the whole 128-bit register, bit 0 being the leftmost bit. The count
  comes from the preferred word of RB (or the I7 immediate, sign-extended) and only some of its bits
  count: `shlqbi`, `rotqbi` use the low 3 bits; `shlqby` the low 5 (a count above 15 gives zero);
  `rotqby` the low 4; the `bybi` forms use bits 24 to 28 (the word shifted right by 3, low 5 bits; low
  4 bits for `rotqbybi`, where the two readings rotate the same way).
- The mask forms (`rotqmbi`, `rotqmby`, `rotqmbybi` and the immediates) shift right by the negated
  count: `(0 - count) & 7` bits, or `(0 - count) & 0x1f` bytes with zero fill (a result of 16 or more
  gives zero). `rotqmbybi` negates the extracted field, not the word.
- `shufb` numbers the 32 bytes of RA || RB; a control byte `10xxxxxx` gives 0x00, `110xxxxx` gives
  0xff, `111xxxxx` gives 0x80, and any other value selects byte `control & 0x1f`.
- The generate-controls instructions add the address operands, keep 4 (byte), 3 (halfword), 2 (word) or
  1 (doubleword) low bits of the sum rounded down to the element size, and start from the bytes
  0x10 ... 0x1f; the element's bytes are 03, 0203, 00010203 or 0001020304050607 from that byte on.
- `fsm`, `fsmh`, `fsmb` and `fsmbi` replicate the low 4, 8 or 16 bits (or the I16 immediate) into words,
  halfwords or bytes with the leftmost element taking the most significant selected bit. `gb`, `gbh`,
  `gbb` gather the least significant bit of each word, halfword or byte into the preferred word, first
  element in the most significant position, and clear the other words. `orx` ORs the four words into
  the preferred word and clears the others.
"""

from tests import spu_asm

from . import CODE_BASE, Case

SHIFT_REF = "SPU ISA 1.2, Shift and Rotate Instructions"
INT_REF = "SPU ISA 1.2, Integer and Logical Instructions"
CTL_REF = "SPU ISA 1.2, Memory-Load/Store Instructions"
CH_REF = "SPU ISA 1.2, Constant-Formation Instructions"

QA = (0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F)
QB = (0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F)
QC = (0xF0123456, 0x789ABCDE, 0x0FEDCBA9, 0x87654321)
ZERO = (0, 0, 0, 0)
# Contents of words that must not influence the result, and of a destination that must be overwritten.
JUNK = (0x99999999, 0x12345678, 0xFFFFFFFF)
OLD_RT = (0xDEADBEEF, 0xCAFEF00D, 0x0BADCAFE, 0x13572468)


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def t(name: str, ref: str, words, init: dict, expect: dict, **kwargs) -> Case:
    if isinstance(words, int):
        words = [words]
    return Case(name, ref, tuple(words), {"r%d" % r: v for r, v in init.items()},
                {"r%d" % r: v for r, v in expect.items()}, **kwargs)


def count(n: int) -> tuple[int, int, int, int]:
    """A count register: only the preferred word is the count."""
    return (n & 0xFFFFFFFF, *JUNK)


def shift(mnemonic: str, ref: str, a, n: int, out, name: str | None = None) -> Case:
    """RT = op(RA, count in RB) with $3, $4 -> $6."""
    return t(name or mnemonic, ref + ": " + mnemonic, enc(mnemonic, RT=6, RA=3, RB=4),
             {3: a, 4: count(n), 6: OLD_RT}, {6: out})


def shifti(mnemonic: str, ref: str, a, i7: int, out, name: str) -> Case:
    return t(name, ref + ": " + mnemonic, enc(mnemonic, RT=6, RA=3, I7=i7), {3: a, 6: OLD_RT}, {6: out})


def control(mnemonic: str, name: str, ra, rb_or_i7, out) -> Case:
    """Generate controls: x forms read RB, d forms take the I7 immediate."""
    if mnemonic.endswith("x"):
        return t(name, CTL_REF + ": " + mnemonic, enc(mnemonic, RT=6, RA=3, RB=4),
                 {3: (ra, *JUNK), 4: (rb_or_i7, *JUNK), 6: OLD_RT}, {6: out})
    return t(name, CTL_REF + ": " + mnemonic, enc(mnemonic, RT=6, RA=3, I7=rb_or_i7),
             {3: (ra, *JUNK), 6: OLD_RT}, {6: out})


CASES: list[Case] = []

# --- Quadword shifts and rotates by bits -----------------------------------------------------------

CASES += [
    shift("shlqbi", SHIFT_REF, QC, 4, (0x01234567, 0x89ABCDE0, 0xFEDCBA98, 0x76543210),
          "shlqbi_by_four_bits"),
    shift("shlqbi", SHIFT_REF, QA, 0xFFFFFFFF, (0x00810182, 0x02830384, 0x04850586, 0x06870780),
          "shlqbi_uses_the_low_three_bits_and_carries_across_bytes"),
    shift("shlqbi", SHIFT_REF, QA, 8, QA, "shlqbi_count_eight_is_zero_bits"),
    shifti("shlqbii", SHIFT_REF, QC, 1, (0xE02468AC, 0xF13579BC, 0x1FDB9753, 0x0ECA8642), "shlqbii_by_one_bit"),
    shifti("shlqbii", SHIFT_REF, QC, -7, (0xE02468AC, 0xF13579BC, 0x1FDB9753, 0x0ECA8642),
           "shlqbii_negative_immediate_keeps_three_bits"),
    shift("rotqbi", SHIFT_REF, QC, 4, (0x01234567, 0x89ABCDE0, 0xFEDCBA98, 0x7654321F), "rotqbi_by_four_bits"),
    shift("rotqbi", SHIFT_REF, QC, 0x0C, (0x01234567, 0x89ABCDE0, 0xFEDCBA98, 0x7654321F),
          "rotqbi_count_masks_to_three_bits"),
    shift("rotqbi", SHIFT_REF, QC, 8, QC, "rotqbi_count_eight_is_zero_bits"),
    shifti("rotqbii", SHIFT_REF, QC, 1, (0xE02468AC, 0xF13579BC, 0x1FDB9753, 0x0ECA8643), "rotqbii_by_one_bit"),
    shifti("rotqbii", SHIFT_REF, QC, -1, (0x091A2B3C, 0x4D5E6F07, 0xF6E5D4C3, 0xB2A190F8),
           "rotqbii_negative_immediate_is_seven_bits"),
    shift("rotqmbi", SHIFT_REF, QC, -4, (0x0F012345, 0x6789ABCD, 0xE0FEDCBA, 0x98765432),
          "rotqmbi_negated_count_shifts_right"),
    shift("rotqmbi", SHIFT_REF, QC, 1, (0x01E02468, 0xACF13579, 0xBC1FDB97, 0x530ECA86),
          "rotqmbi_positive_count_is_seven_bits_right"),
    shift("rotqmbi", SHIFT_REF, QC, 8, QC, "rotqmbi_count_eight_is_zero_bits"),
    shifti("rotqmbii", SHIFT_REF, QC, -4, (0x0F012345, 0x6789ABCD, 0xE0FEDCBA, 0x98765432),
           "rotqmbii_minus_four"),
    shifti("rotqmbii", SHIFT_REF, QC, 1, (0x01E02468, 0xACF13579, 0xBC1FDB97, 0x530ECA86),
           "rotqmbii_plus_one_is_seven_bits_right"),
]

# --- Quadword shifts and rotates by bytes ----------------------------------------------------------

CASES += [
    shift("shlqby", SHIFT_REF, QA, 4, (0x04050607, 0x08090A0B, 0x0C0D0E0F, 0), "shlqby_by_four_bytes"),
    shift("shlqby", SHIFT_REF, QA, 15, (0x0F000000, 0, 0, 0), "shlqby_by_fifteen_bytes"),
    shift("shlqby", SHIFT_REF, QA, 16, ZERO, "shlqby_by_sixteen_bytes_is_zero"),
    shift("shlqby", SHIFT_REF, QA, 0x1F, ZERO, "shlqby_by_thirty_one_bytes_is_zero"),
    shift("shlqby", SHIFT_REF, QA, 0x25, (0x05060708, 0x090A0B0C, 0x0D0E0F00, 0),
          "shlqby_uses_the_low_five_bits"),
    shifti("shlqbyi", SHIFT_REF, QA, 3, (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F000000), "shlqbyi_by_three"),
    shifti("shlqbyi", SHIFT_REF, QA, 16, ZERO, "shlqbyi_by_sixteen_is_zero"),
    shifti("shlqbyi", SHIFT_REF, QA, -1, ZERO, "shlqbyi_minus_one_is_thirty_one_bytes"),
    shifti("shlqbyi", SHIFT_REF, QA, -29, (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F000000),
           "shlqbyi_negative_immediate_keeps_five_bits"),
    shift("shlqbybi", SHIFT_REF, QA, 0x1D, (0x03040506, 0x0708090A, 0x0B0C0D0E, 0x0F000000),
          "shlqbybi_uses_bits_24_to_28"),
    shift("shlqbybi", SHIFT_REF, QA, 0x80, ZERO, "shlqbybi_sixteen_bytes_is_zero"),
    shift("shlqbybi", SHIFT_REF, QA, 0x104, QA, "shlqbybi_field_is_five_bits_wide"),
    shift("rotqby", SHIFT_REF, QA, 4, (0x04050607, 0x08090A0B, 0x0C0D0E0F, 0x00010203), "rotqby_by_four_bytes"),
    shift("rotqby", SHIFT_REF, QA, 16, QA, "rotqby_count_sixteen_is_zero_bytes"),
    shift("rotqby", SHIFT_REF, QA, 0x1F, (0x0F000102, 0x03040506, 0x0708090A, 0x0B0C0D0E),
          "rotqby_uses_the_low_four_bits"),
    shifti("rotqbyi", SHIFT_REF, QA, 4, (0x04050607, 0x08090A0B, 0x0C0D0E0F, 0x00010203), "rotqbyi_by_four"),
    shifti("rotqbyi", SHIFT_REF, QA, -12, (0x04050607, 0x08090A0B, 0x0C0D0E0F, 0x00010203),
           "rotqbyi_negative_immediate_keeps_four_bits"),
    shift("rotqbybi", SHIFT_REF, QB, 0x24, (0x14151617, 0x18191A1B, 0x1C1D1E1F, 0x10111213),
          "rotqbybi_uses_bits_24_to_28"),
    shift("rotqbybi", SHIFT_REF, QB, 0x80, QB, "rotqbybi_sixteen_bytes_is_zero_rotation"),
    shift("rotqmby", SHIFT_REF, QA, -4, (0, 0x00010203, 0x04050607, 0x08090A0B), "rotqmby_negated_count"),
    shift("rotqmby", SHIFT_REF, QA, -1, (0x00000102, 0x03040506, 0x0708090A, 0x0B0C0D0E),
          "rotqmby_one_byte_right"),
    shift("rotqmby", SHIFT_REF, QB, -15, (0, 0, 0, 0x00000010), "rotqmby_fifteen_bytes_right"),
    shift("rotqmby", SHIFT_REF, QA, -16, ZERO, "rotqmby_sixteen_bytes_is_zero"),
    shift("rotqmby", SHIFT_REF, QA, -17, ZERO, "rotqmby_seventeen_bytes_is_zero"),
    shift("rotqmby", SHIFT_REF, QA, 0, QA, "rotqmby_count_zero_keeps_the_register"),
    shifti("rotqmbyi", SHIFT_REF, QA, -4, (0, 0x00010203, 0x04050607, 0x08090A0B), "rotqmbyi_minus_four"),
    shifti("rotqmbyi", SHIFT_REF, QA, 4, ZERO, "rotqmbyi_plus_four_is_twenty_eight_bytes"),
    shifti("rotqmbyi", SHIFT_REF, QA, -1, (0x00000102, 0x03040506, 0x0708090A, 0x0B0C0D0E),
           "rotqmbyi_minus_one"),
    shift("rotqmbybi", SHIFT_REF, QA, 0xFFFFFFE0, (0, 0x00010203, 0x04050607, 0x08090A0B),
          "rotqmbybi_negates_the_extracted_field"),
    shift("rotqmbybi", SHIFT_REF, QA, 0x08, ZERO, "rotqmbybi_positive_field_is_thirty_one_bytes"),
    shift("rotqmbybi", SHIFT_REF, QA, 0, QA, "rotqmbybi_zero_keeps_the_register"),
]

# --- Shuffle bytes --------------------------------------------------------------------------------

SA = (0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF)
SB = (0xF0F1F2F3, 0xF4F5F6F7, 0xF8F9FAFB, 0xFCFDFEFF)


def shufb(name: str, rc, out, rt: int = 6, ra: int = 3) -> Case:
    init = {3: SA, 4: SB, 5: rc, 6: OLD_RT}
    return t(name, INT_REF + ": shufb", enc("shufb", RT4=rt, RA=ra, RB=4, RC=5), init, {rt: out})


CASES += [
    # 0x00..0x0f read RA, 0x10..0x1f read RB, and only the low 5 bits of an ordinary control byte count
    # (0x3f -> 0x1f, 0x2e -> 0x0e, 0x24 -> 0x04, 0x25 -> 0x05).
    shufb("shufb_selects_from_both_operands", (0x000F101F, 0x3F2E0102, 0x04081010, 0xFF1F2425),
          (0x00FFF0FF, 0xFFEE1122, 0x4488F0F0, 0x80FF4455)),
    shufb("shufb_special_control_bytes", (0x80BFC0DF, 0xE0FFA0C1, 0xF8E00000, 0x7F5F9FDF),
          (0x0000FFFF, 0x808000FF, 0x80800000, 0xFFFF00FF)),
    shufb("shufb_destination_may_be_ra", (0x000F101F, 0x3F2E0102, 0x04081010, 0xFF1F2425),
          (0x00FFF0FF, 0xFFEE1122, 0x4488F0F0, 0x80FF4455), rt=3),
    t("shufb_reverses_a_register", INT_REF + ": shufb", enc("shufb", RT4=6, RA=3, RB=3, RC=5),
      {3: QA, 5: (0x0F0E0D0C, 0x0B0A0908, 0x07060504, 0x03020100)},
      {6: (0x0F0E0D0C, 0x0B0A0908, 0x07060504, 0x03020100)}),
]

# --- Generate controls ----------------------------------------------------------------------------

CASES += [
    control("cbx", "cbx_adds_the_preferred_words", 0x13, 0x01,
            (0x10111213, 0x03151617, 0x18191A1B, 0x1C1D1E1F)),
    control("cbx", "cbx_position_zero", 0x20, 0, (0x03111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F)),
    control("cbd", "cbd_negative_immediate_wraps_to_position_fifteen", 0x20, -1,
            (0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E03)),
    control("chx", "chx_clears_the_low_bit", 0x05, 0x06, (0x10111213, 0x14151617, 0x18190203, 0x1C1D1E1F)),
    control("chd", "chd_adds_the_immediate", 0x100, 5, (0x10111213, 0x02031617, 0x18191A1B, 0x1C1D1E1F)),
    control("cwx", "cwx_clears_the_low_two_bits", 0x0A, 0x03, (0x10111213, 0x14151617, 0x18191A1B, 0x00010203)),
    control("cwd", "cwd_negative_immediate", 0, -8, (0x10111213, 0x14151617, 0x00010203, 0x1C1D1E1F)),
    control("cdx", "cdx_clears_the_low_three_bits", 4, 5, (0x10111213, 0x14151617, 0x00010203, 0x04050607)),
    control("cdd", "cdd_second_doubleword", 0x13, 5, (0x10111213, 0x14151617, 0x00010203, 0x04050607)),
    control("cdd", "cdd_first_doubleword", 3, 2, (0x00010203, 0x04050607, 0x18191A1B, 0x1C1D1E1F)),
]

# --- Form select mask ------------------------------------------------------------------------------


def one(mnemonic: str, ref: str, a, out, name: str | None = None) -> Case:
    """RT = op(RA) with $3 -> $6, the old $6 being non-zero."""
    return t(name or mnemonic, ref + ": " + mnemonic, enc(mnemonic, RT=6, RA=3), {3: a, 6: OLD_RT}, {6: out})


CASES += [
    one("fsm", INT_REF, (0xFFFFFFF5, *JUNK), (0, 0xFFFFFFFF, 0, 0xFFFFFFFF), "fsm_low_four_bits_leftmost_first"),
    one("fsm", INT_REF, (0x00000010, *JUNK), ZERO, "fsm_ignores_higher_bits"),
    one("fsm", INT_REF, (0xFFFFFFFF, *JUNK), (0xFFFFFFFF,) * 4, "fsm_all_ones"),
    one("fsmh", INT_REF, (0xABCD01A5, *JUNK), (0xFFFF0000, 0xFFFF0000, 0x0000FFFF, 0x0000FFFF),
        "fsmh_low_eight_bits"),
    one("fsmb", INT_REF, (0x12348A01, *JUNK), (0xFF000000, 0xFF00FF00, 0, 0x000000FF), "fsmb_low_sixteen_bits"),
    t("fsmbi", CH_REF + ": fsmbi", enc("fsmbi", RT=6, U16=0x8A01), {6: OLD_RT},
      {6: (0xFF000000, 0xFF00FF00, 0, 0x000000FF)}),
    t("fsmbi_word_mask", CH_REF + ": fsmbi", enc("fsmbi", RT=6, U16=0xF00F), {},
      {6: (0xFFFFFFFF, 0, 0, 0xFFFFFFFF)}),
]

# --- Gather bits and or across --------------------------------------------------------------------

CASES += [
    one("gbb", INT_REF, (0x01000302, 0xFFFE8180, 0x00010203, 0x0405070E), (0x0000AA56, 0, 0, 0),
        "gbb_first_byte_is_the_most_significant_bit"),
    one("gbh", INT_REF, (0xFFFF0000, 0x00010001, 0x00000001, 0xFFFE0000), (0xB4, 0, 0, 0),
        "gbh_first_halfword_is_the_most_significant_bit"),
    one("gb", INT_REF, (0x00000001, 0xFFFFFFFE, 0x80000001, 0x00000003), (0xB, 0, 0, 0),
        "gb_first_word_is_the_most_significant_bit"),
    one("gb", INT_REF, (0x80000000, 0x80000000, 0x80000000, 0x80000000), ZERO, "gb_only_counts_the_low_bit"),
    one("orx", INT_REF, (0x00010000, 0x00000200, 0x40000000, 0x00000008), (0x40010208, 0, 0, 0),
        "orx_ors_the_four_words_into_the_preferred_word"),
]


def _check_coverage() -> None:
    """Every `perm` mnemonic has a case; a missing one fails the generator."""
    from ps3recomp import spu

    seen = {spu.decode(word, CODE_BASE).spec.mnemonic for case in CASES for word in case.words}
    missing = sorted({s.mnemonic for s in spu.ALL_OPS if s.group == "perm"} - seen)
    if missing:
        raise ValueError("cases_perm: no case for %s" % ", ".join(missing))


_check_coverage()

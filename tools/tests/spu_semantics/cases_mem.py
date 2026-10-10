"""Memory-group cases: quadword loads and stores of the local store.

The expected values are written out by hand from the SPU ISA 1.2 chapter "Memory-Load/Store":

- The address of `lqd`/`stqd` is the preferred word of RA plus the I10 displacement scaled by 16; of
  `lqx`/`stqx` the sum of the preferred words of RA and RB; of `lqa`/`stqa` the I16 field scaled by 4 and
  sign-extended; of `lqr`/`stqr` the same offset added to the address of the instruction itself.
- Every address is reduced with LSLR (0x3ffff for the 256 KiB local store) and with 0xFFFFFFF0, so it
  wraps at 0x40000 and its low four bits are ignored. Sums wrap at 32 bits first.
- A quadword is stored big-endian: the first byte at the address is the leftmost byte of the register.

Every case surrounds the quadword with 0xEE bytes: a load must not read them and a store must not touch
them.
"""

from tests import spu_asm

from . import CODE_BASE, Case

MEM_REF = "SPU ISA 1.2, Memory-Load/Store Instructions"

QUAD = bytes(0x11 * i for i in range(16))
QUAD_WORDS = (0x00112233, 0x44556677, 0x8899AABB, 0xCCDDEEFF)
PAD = bytes([0xEE]) * 16
OLD_RT = (0xDEADBEEF, 0xCAFEF00D, 0x0BADCAFE, 0x13572468)
JUNK = (0x99999999, 0x12345678, 0xFFFFFFFF)


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def pref(value: int) -> tuple[int, int, int, int]:
    """A register whose preferred word is `value`; the other words must not matter."""
    return (value & 0xFFFFFFFF, *JUNK)


def load(name: str, mnemonic: str, words, init: dict, ls_addr: int, dest: int = 6) -> Case:
    """Loads QUAD, placed at `ls_addr` between pad bytes, into register `dest`."""
    if isinstance(words, int):
        words = [words]
    regs = {"r%d" % r: v for r, v in init.items()}
    regs.setdefault("r%d" % dest, OLD_RT)
    return Case(name, MEM_REF + ": " + mnemonic, tuple(words), regs, {"r%d" % dest: QUAD_WORDS},
                ls_init={ls_addr - 16: PAD + QUAD + PAD})


def store(name: str, mnemonic: str, words, init: dict, ls_addr: int) -> Case:
    """Stores QUAD_WORDS (register 6); only the 16 bytes at `ls_addr` change."""
    if isinstance(words, int):
        words = [words]
    regs = {"r%d" % r: v for r, v in init.items()}
    regs["r6"] = QUAD_WORDS
    return Case(name, MEM_REF + ": " + mnemonic, tuple(words), regs, {},
                ls_init={ls_addr - 16: PAD * 3}, ls_expect={ls_addr - 16: PAD + QUAD + PAD})


LNOP = enc("lnop")

CASES: list[Case] = [
    # --- lqd -----------------------------------------------------------------------------------
    load("lqd_adds_the_scaled_displacement", "lqd", enc("lqd", RT=6, RA=3, I10x16=0x20), {3: pref(0x10010)}, 0x10030),
    load("lqd_negative_displacement", "lqd", enc("lqd", RT=6, RA=3, I10x16=-16), {3: pref(0x10030)}, 0x10020),
    load("lqd_largest_displacement", "lqd", enc("lqd", RT=6, RA=3, I10x16=0x1FF0), {3: pref(0x10010)}, 0x12000),
    load("lqd_smallest_displacement", "lqd", enc("lqd", RT=6, RA=3, I10x16=-0x2000), {3: pref(0x12010)}, 0x10010),
    load("lqd_ignores_the_low_four_bits", "lqd", enc("lqd", RT=6, RA=3, I10x16=0), {3: pref(0x1002F)}, 0x10020),
    load("lqd_wraps_at_the_local_store_size", "lqd", enc("lqd", RT=6, RA=3, I10x16=0x30), {3: pref(0x50000)},
         0x10030),
    load("lqd_destination_may_be_the_base", "lqd", enc("lqd", RT=3, RA=3, I10x16=0), {3: pref(0x10040)}, 0x10040,
         dest=3),
    # --- lqx -----------------------------------------------------------------------------------
    load("lqx_adds_the_preferred_words", "lqx", enc("lqx", RT=6, RA=3, RB=4),
         {3: pref(0x10000), 4: pref(0x00000037)}, 0x10030),
    load("lqx_sum_wraps_at_32_bits", "lqx", enc("lqx", RT=6, RA=3, RB=4),
         {3: pref(0xFFFF0000), 4: pref(0x00020037)}, 0x10030),
    load("lqx_wraps_at_the_local_store_size", "lqx", enc("lqx", RT=6, RA=3, RB=4),
         {3: pref(0x30000), 4: pref(0x20038)}, 0x10030),
    # --- lqa -----------------------------------------------------------------------------------
    load("lqa_absolute_address", "lqa", enc("lqa", RT=6, ABS16=0x10040), {}, 0x10040),
    load("lqa_ignores_the_low_four_bits", "lqa", enc("lqa", RT=6, ABS16=0x10044), {}, 0x10040),
    load("lqa_negative_field_wraps_to_the_top_of_the_local_store", "lqa", enc("lqa", RT=6, ABS16=-0x10000), {},
         0x30000),
    # --- lqr -----------------------------------------------------------------------------------
    load("lqr_relative_to_the_instruction", "lqr", [LNOP, enc("lqr", RT=6, addr=CODE_BASE + 4, target=0x10050)], {},
         0x10050),
    load("lqr_first_word", "lqr", enc("lqr", RT=6, addr=CODE_BASE, target=0x10050), {}, 0x10050),
    load("lqr_wraps_at_the_local_store_size", "lqr", enc("lqr", RT=6, REL16=-0x20000), {}, 0x21000),
    # --- stqd ----------------------------------------------------------------------------------
    store("stqd_adds_the_scaled_displacement", "stqd", enc("stqd", RT=6, RA=3, I10x16=0x10), {3: pref(0x10020)},
          0x10030),
    store("stqd_negative_displacement", "stqd", enc("stqd", RT=6, RA=3, I10x16=-32), {3: pref(0x10050)}, 0x10030),
    store("stqd_ignores_the_low_four_bits", "stqd", enc("stqd", RT=6, RA=3, I10x16=0), {3: pref(0x1003F)}, 0x10030),
    store("stqd_wraps_at_the_local_store_size", "stqd", enc("stqd", RT=6, RA=3, I10x16=0x30), {3: pref(0x50000)},
          0x10030),
    # --- stqx ----------------------------------------------------------------------------------
    store("stqx_adds_the_preferred_words", "stqx", enc("stqx", RT=6, RA=3, RB=4),
          {3: pref(0x10000), 4: pref(0x00000037)}, 0x10030),
    store("stqx_sum_wraps_at_32_bits", "stqx", enc("stqx", RT=6, RA=3, RB=4),
          {3: pref(0xFFFF0000), 4: pref(0x00020037)}, 0x10030),
    store("stqx_wraps_at_the_local_store_size", "stqx", enc("stqx", RT=6, RA=3, RB=4),
          {3: pref(0x30000), 4: pref(0x20038)}, 0x10030),
    # --- stqa ----------------------------------------------------------------------------------
    store("stqa_absolute_address", "stqa", enc("stqa", RT=6, ABS16=0x10040), {}, 0x10040),
    store("stqa_ignores_the_low_four_bits", "stqa", enc("stqa", RT=6, ABS16=0x10044), {}, 0x10040),
    store("stqa_negative_field_wraps_to_the_top_of_the_local_store", "stqa", enc("stqa", RT=6, ABS16=-0x10000), {},
          0x30000),
    # --- stqr ----------------------------------------------------------------------------------
    store("stqr_relative_to_the_instruction", "stqr", [LNOP, enc("stqr", RT=6, addr=CODE_BASE + 4, target=0x10050)],
          {}, 0x10050),
    store("stqr_wraps_at_the_local_store_size", "stqr", enc("stqr", RT=6, REL16=-0x20000), {}, 0x21000),
]


def _check_coverage() -> None:
    """Every `mem` mnemonic has a case; a missing one fails the generator."""
    from ps3recomp import spu

    seen = {spu.decode(word, CODE_BASE).spec.mnemonic for case in CASES for word in case.words}
    missing = sorted({s.mnemonic for s in spu.ALL_OPS if s.group == "mem"} - seen)
    if missing:
        raise ValueError("cases_mem: no case for %s" % ", ".join(missing))


_check_coverage()

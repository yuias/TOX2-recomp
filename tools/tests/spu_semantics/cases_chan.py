"""Channel and system cases: `rdch`, `rchcnt`, `wrch`, `stopd`, `mfspr`/`mtspr` and the FPSCR moves.

The channel script of a case (`channels`) stands in for the runtime; the cases check what the
instruction does around the hook. The expected values come from the SPU ISA 1.2:

- `rdch` places the channel value in the preferred slot of RT and the other slots return zeros (note
  on 32-bit channels, p.248). `rchcnt` does the same with the channel capacity (p.249). `wrch` sends
  the preferred slot of RT (p.250).
- A channel access that cannot complete ends the thread: nothing after it runs. The scripted hook
  stands for that with `leave_channels`.
- `stopd` stops the program like `stop` (p.239). The gate images use it as a trap, so it ends the case
  as a halt at its own address.
- `mfspr` reads zeros and `mtspr` does nothing for an SPR that is not implemented (p.244, p.245).
- `fscrrd` returns the FPSCR with the unused bits forced to zero (p.236); `fscrwr` writes RA into the
  FPSCR and does not write RT, a false target (p.235). The used bits are listed in the FPSCR format
  table (p.200, p.201): bits 20:23, 29:31, 50:55, 61:63, 82:87, 93:95, 116:119 and 125:127, which in
  the four words are 0x00000f07, 0x00003f07, 0x00003f07 and 0x00000f07 (bit 0 is the most significant).
- `bid` and `binze` (the E form of `binz`) are indirect branches whose D and E bits change the
  interrupt-enable state when the branch is taken (interrupt facility, p.251).
"""

from tests import spu_asm

from . import CODE_BASE, Case

SENT = 0x5A5A5A5A
SENTINEL = (SENT, SENT, SENT, SENT)

RDCH_REF = "SPU ISA 1.2, Channel Instructions: rdch"
RCHCNT_REF = "SPU ISA 1.2, Channel Instructions: rchcnt"
WRCH_REF = "SPU ISA 1.2, Channel Instructions: wrch"
STOPD_REF = "SPU ISA 1.2, Control Instructions: stopd"
SPR_REF = "SPU ISA 1.2, Control Instructions: mfspr, mtspr"
FSCRRD_REF = "SPU ISA 1.2, Floating-Point Status and Control Register Read, FPSCR format"
FSCRWR_REF = "SPU ISA 1.2, Floating-Point Status and Control Register Write, FPSCR format"
IRQ_REF = "SPU ISA 1.2, SPU Interrupt Facility: feature bits D and E (Table 12-1)"


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def at(index: int) -> int:
    return CODE_BASE + 4 * index


def ran(index: int) -> tuple[int, int, int, int]:
    """Register value left by a link-writing instruction at word `index`."""
    return (at(index + 1), 0, 0, 0)


def mark(index: int, reg: int) -> int:
    return enc("brsl", RT=reg, addr=at(index), target=at(index + 1))


def case(name: str, ref: str, words, init=None, expect=None, **kwargs) -> Case:
    return Case(name, ref, tuple(words), init or {}, expect or {}, **kwargs)


def sentinels(*regs: int) -> dict:
    return {"r%d" % r: SENTINEL for r in regs}


FPSCR_USED = (0x00000F07, 0x00003F07, 0x00003F07, 0x00000F07)
ALL_ONES = (0xFFFFFFFF,) * 4

CASES = []

# --- rdch ---------------------------------------------------------------------------------------

CASES += [
    case("rdch_value_in_preferred_word", RDCH_REF,
         [enc("rdch", RT=10, CA=29)],
         sentinels(10), {"r10": (0x11223344, 0, 0, 0)}, channels={"reads": {29: (0x11223344,)}}),
    case("rdch_consumes_reads_in_order", RDCH_REF,
         [enc("rdch", RT=10, CA=29), enc("rdch", RT=11, CA=29), enc("rdch", RT=12, CA=24)],
         sentinels(10, 11, 12),
         {"r10": (1, 0, 0, 0), "r11": (2, 0, 0, 0), "r12": (0xFFFFFFFF, 0, 0, 0)},
         channels={"reads": {29: (1, 2), 24: (0xFFFFFFFF,)}}),
    # The hook refuses: the thread leaves, RT keeps its value and the next instruction does not run.
    case("rdch_hook_asks_to_leave", RDCH_REF,
         [enc("rdch", RT=10, CA=3), mark(1, 11)],
         sentinels(10, 11), {"r10": SENTINEL, "r11": SENTINEL}, channels={"leave_channels": (3,)}),
]

# --- rchcnt -------------------------------------------------------------------------------------

CASES += [
    case("rchcnt_count_in_preferred_word", RCHCNT_REF,
         [enc("rchcnt", RT=10, CA=24)],
         sentinels(10), {"r10": (3, 0, 0, 0)}, channels={"counts": {24: 3}}),
    # The capacity of a channel that has none is zero.
    case("rchcnt_zero_capacity", RCHCNT_REF,
         [enc("rchcnt", RT=10, CA=30)],
         sentinels(10), {"r10": (0, 0, 0, 0)}, channels={"counts": {24: 3}}),
    case("rchcnt_reads_no_value", RCHCNT_REF,
         [enc("rchcnt", RT=10, CA=29), enc("rdch", RT=11, CA=29)],
         sentinels(10, 11), {"r10": (1, 0, 0, 0), "r11": (0x77, 0, 0, 0)},
         channels={"counts": {29: 1}, "reads": {29: (0x77,)}}),
]

# --- wrch ---------------------------------------------------------------------------------------

CASES += [
    case("wrch_sends_the_preferred_word", WRCH_REF,
         [enc("wrch", CA=16, RT=3)],
         {"r3": (0xAAAAAAAA, 1, 2, 3)}, {"r3": (0xAAAAAAAA, 1, 2, 3)},
         channels={"writes": [(16, 0xAAAAAAAA)]}),
    case("wrch_records_writes_in_order", WRCH_REF,
         [enc("wrch", CA=17, RT=3), enc("wrch", CA=16, RT=4), enc("wrch", CA=17, RT=4)],
         {"r3": (7, 0, 0, 0), "r4": (0x80000000, 9, 9, 9)}, {},
         channels={"writes": [(17, 7), (16, 0x80000000), (17, 0x80000000)]}),
    # The write is performed, then the hook asks to leave: the next instruction does not run.
    case("wrch_hook_asks_to_leave", WRCH_REF,
         [enc("wrch", CA=3, RT=3), mark(1, 11)],
         dict(sentinels(11), r3=(5, 0, 0, 0)), {"r11": SENTINEL},
         channels={"writes": [(3, 5)], "leave_channels": (3,)}),
]

# The policy module saves the event and tag masks with rdch 11 and rdch 12 and restores them with wrch 1
# and wrch 22; the values go through unchanged.
CASES += [
    case("event_and_tag_masks_round_trip", RDCH_REF,
         [enc("rdch", RT=10, CA=11), enc("rdch", RT=11, CA=12),
          enc("wrch", CA=1, RT=10), enc("wrch", CA=22, RT=11)],
         sentinels(10, 11), {"r10": (0x400, 0, 0, 0), "r11": (0xFFFFFFFF, 0, 0, 0)},
         channels={"reads": {11: (0x400,), 12: (0xFFFFFFFF,)}, "writes": [(1, 0x400), (22, 0xFFFFFFFF)]}),
]

# --- stopd --------------------------------------------------------------------------------------

CASES += [
    case("stopd_ends_the_case_at_its_site", STOPD_REF,
         [mark(0, 10), enc("stopd", RC=0, RA=0, RB=0), mark(2, 11)],
         sentinels(10, 11), {"r10": ran(0), "r11": SENTINEL}, channels={"halts": [at(1)]}),
]

# --- mfspr, mtspr -------------------------------------------------------------------------------

CASES += [
    case("mfspr_reads_zeros", SPR_REF,
         [enc("mfspr", RT=10, SA=3)],
         sentinels(10), {"r10": (0, 0, 0, 0)}),
    case("mtspr_is_ignored", SPR_REF,
         [enc("mtspr", SA=3, RT=3), mark(1, 10)],
         dict(sentinels(10), r3=(1, 2, 3, 4)), {"r3": (1, 2, 3, 4), "r10": ran(1)}),
]

# --- fscrrd, fscrwr -----------------------------------------------------------------------------

CASES += [
    case("fscrrd_forces_the_unused_bits_to_zero", FSCRRD_REF,
         [enc("fscrrd", RT=10)],
         dict(sentinels(10), fpscr=ALL_ONES), {"r10": FPSCR_USED, "fpscr": ALL_ONES}),
    # Word by word: the used bits survive, the others are cleared.
    case("fscrrd_keeps_the_used_bits", FSCRRD_REF,
         [enc("fscrrd", RT=10)],
         dict(sentinels(10), fpscr=(0x13572465, 0x12345678, 0x9ABCDEF3, 0xFEDCBA98)),
         {"r10": (0x00000405, 0x00001600, 0x00001E03, 0x00000A00)}),
    case("fscrrd_of_a_clear_fpscr", FSCRRD_REF,
         [enc("fscrrd", RT=10)],
         sentinels(10), {"r10": (0, 0, 0, 0)}),
    case("fscrwr_stores_all_128_bits", FSCRWR_REF,
         [enc("fscrwr", RT=5, RA=4)],
         dict(sentinels(5), r4=(1, 2, 3, 4)), {"fpscr": (1, 2, 3, 4), "r4": (1, 2, 3, 4)}),
    # RT is a false target: it is not written.
    case("fscrwr_does_not_write_rt", FSCRWR_REF,
         [enc("fscrwr", RT=5, RA=4)],
         dict(sentinels(5), r4=(1, 2, 3, 4)), {"r5": SENTINEL}),
    case("fscrwr_then_fscrrd_round_trip", FSCRWR_REF,
         [enc("fscrwr", RT=0, RA=4), enc("fscrrd", RT=10)],
         dict(sentinels(10), r4=(0x00000A05, 0x00002001, 0x00000104, 0x00000802)),
         {"r10": (0x00000A05, 0x00002001, 0x00000104, 0x00000802)}),
]

# --- bid, binze: the interrupt-enable state around a jump to the next instruction ----------------

# The kernel's critical-section idiom: `brsl $2,$+4` reads the PC, `ai $2,$2,8` points past the
# next instruction, and `bid $2` (or `binze $11,$2`) jumps to that next instruction while it changes
# the interrupt state. Word 2 is the branch and word 3 is its target.
IDIOM_PREFIX = (mark(0, 2), enc("ai", RT=2, RA=2, I10=8))

CASES += [
    case("bid_jumps_to_the_next_instruction_and_disables", IRQ_REF,
         [*IDIOM_PREFIX, enc("bi", RA=2, D=1), mark(3, 10)],
         dict(sentinels(10), int_enabled=1), {"r10": ran(3), "int_enabled": 0}, entries=(at(3),)),
    # With a longer offset the branch is observable: the instruction after it is skipped.
    case("bid_skips_forward", IRQ_REF,
         [mark(0, 2), enc("ai", RT=2, RA=2, I10=12), enc("bi", RA=2, D=1), mark(3, 10), mark(4, 11)],
         dict(sentinels(10, 11), int_enabled=1),
         {"r10": SENTINEL, "r11": ran(4), "int_enabled": 0}, entries=(at(4),)),
    case("binze_taken_enables", IRQ_REF,
         [*IDIOM_PREFIX, enc("binz", RT=11, RA=2, E=1), mark(3, 10)],
         dict(sentinels(10), r11=(1, 0, 0, 0), int_enabled=0),
         {"r10": ran(3), "int_enabled": 1}, entries=(at(3),)),
    case("binze_not_taken_keeps_the_state", IRQ_REF,
         [*IDIOM_PREFIX, enc("binz", RT=11, RA=2, E=1), mark(3, 10)],
         dict(sentinels(10), r11=(0, 1, 1, 1), int_enabled=0),
         {"r10": ran(3), "int_enabled": 0}, entries=(at(3),)),
]

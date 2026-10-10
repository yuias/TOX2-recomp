"""Control-group cases: direct and indirect branches, links, the interrupt bits and `stop`.

Only control words are used, so every outcome is observed through link registers and the interrupt
state. A "marker" is `brsl $N,$+4`: it falls through and leaves `(site + 4, 0, 0, 0)` in `$N`
(ISA: Branch Relative and Set Link), so the register shows which markers ran. Registers start as the
sentinel in all four words; a register that still holds it was never written, and the link rule that
words 1 to 3 become 0 is checked by the same comparison.

Expected values are derived from the SPU ISA 1.2 description of each instruction:

- Targets are masked with LSLR (0x3ffff) and with ~3; the 256 KiB local store makes both masks
  `0x3fffc`. Indirect targets come from the preferred word of RA.
- `brz`/`brnz` test the preferred word of RT, `brhz`/`brhnz` the preferred halfword (bits 16 to 31 of
  that word).
- `brsl`/`brasl`/`bisl`/`bisled` write the address of the next instruction into the preferred word
  of RT and clear the other words. `bisl $0,$0` reads RA before the link overwrites it.
- The D feature bit of an indirect branch disables and the E bit enables interrupts; a plain form
  leaves the state alone. `iret` branches to SRR0 and takes the same bits.
"""

from tests import spu_asm

from . import CODE_BASE, EXIT_LS, Case

SENT = 0x5A5A5A5A
SENTINEL = (SENT, SENT, SENT, SENT)

BR_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: br"
BRA_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: bra"
BRSL_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: brsl"
BRASL_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: brasl"
BI_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: bi"
BISL_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: bisl"
BISLED_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: bisled"
BRZ_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: brz, brnz"
BRHZ_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: brhz, brhnz"
BIZ_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: biz, binz, bihz, bihnz"
IRET_REF = "SPU ISA 1.2, Compare, Branch, and Halt Instructions: iret"
STOP_REF = "SPU ISA 1.2, Control Instructions: stop"
HINT_REF = "SPU ISA 1.2, Hint-for-Branch and Control Instructions: hbr, hbrr, nop, lnop, sync, dsync"


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def at(index: int) -> int:
    return CODE_BASE + 4 * index


def ran(index: int) -> tuple[int, int, int, int]:
    """Register value left by a link-writing instruction at word `index`."""
    return (at(index + 1), 0, 0, 0)


def mark(index: int, reg: int) -> int:
    return enc("brsl", RT=reg, addr=at(index), target=at(index + 1))


def stop(code: int) -> int:
    return enc("stop", STOP=code)


def sentinels(*regs: int) -> dict:
    return {"r%d" % r: SENTINEL for r in regs}


def case(name: str, ref: str, words, init=None, expect=None, **kwargs) -> Case:
    return Case(name, ref, tuple(words), init or {}, expect or {}, **kwargs)


def exit_reg(low_bits: int = 0, other: int = 0) -> tuple[int, int, int, int]:
    """A register whose preferred word dispatches to the exit address."""
    return (EXIT_LS | low_bits, other, other, other)


CASES = []

# --- br, bra: relative and absolute targets -------------------------------------------------------

CASES += [
    case("br_skips_forward", BR_REF,
         [enc("br", addr=at(0), target=at(2)), mark(1, 10), mark(2, 11)],
         sentinels(10, 11), {"r10": SENTINEL, "r11": ran(2)}),
    case("bra_skips_forward", BRA_REF,
         [enc("bra", addr=at(0), target=at(2)), mark(1, 10), mark(2, 11)],
         sentinels(10, 11), {"r10": SENTINEL, "r11": ran(2)}),
    # A backward branch that is not taken: the loop edge exists but the word after it runs.
    case("brnz_backward_not_taken", BRZ_REF,
         [mark(0, 10), enc("brnz", RT=3, addr=at(1), target=at(0))],
         dict(sentinels(10), r3=(0, 7, 7, 7)), {"r10": ran(0)}),
    # A target outside the case's code is a computed dispatch; EXIT_LS ends the case.
    case("bra_outside_code_dispatches", BRA_REF,
         [mark(0, 10), enc("bra", addr=at(1), target=EXIT_LS)],
         sentinels(10), {"r10": ran(0)}, channels={"stops": []}),
    case("brasl_outside_code_links_then_dispatches", BRASL_REF,
         [enc("brasl", RT=9, addr=at(0), target=EXIT_LS), mark(1, 10)],
         sentinels(9, 10), {"r9": ran(0), "r10": SENTINEL}, channels={"stops": []}),
]

# --- brsl, brasl: link value, calls and returns ---------------------------------------------------

# word 0 calls word 3; the callee marks and returns through the link register; the return site
# (word 1) marks and ends in the stop at word 2.
CALL_BODY = (mark(1, 10), stop(0x102), mark(3, 11))

CASES += [
    case("brsl_link_in_r0_returns_through_bi", BRSL_REF,
         [enc("brsl", RT=0, addr=at(0), target=at(3)), *CALL_BODY, enc("bi", RA=0)],
         sentinels(0, 10, 11), {"r0": ran(0), "r10": ran(1), "r11": ran(3)}, channels={"stops": [0x102]}),
    case("brsl_link_in_other_register", BRSL_REF,
         [enc("brsl", RT=7, addr=at(0), target=at(3)), *CALL_BODY, enc("bi", RA=7)],
         sentinels(7, 10, 11), {"r7": ran(0), "r10": ran(1), "r11": ran(3)}),
    case("brasl_link_and_absolute_target", BRASL_REF,
         [enc("brasl", RT=0, addr=at(0), target=at(3)), *CALL_BODY, enc("bi", RA=0)],
         sentinels(0, 10, 11), {"r0": ran(0), "r10": ran(1), "r11": ran(3)}),
    # The get-PC idiom: the link is the next word and nothing else happens.
    case("brsl_to_next_word_reads_the_pc", BRSL_REF,
         [enc("brsl", RT=3, addr=at(0), target=at(1)), mark(1, 10)],
         sentinels(3, 10), {"r3": ran(0), "r10": ran(1)}),
    # Two levels: the inner callee returns to the middle function, which returns to the first.
    case("nested_calls_return_in_order", BRSL_REF,
         [enc("brsl", RT=7, addr=at(0), target=at(3)), mark(1, 10), stop(0x102),
          enc("brsl", RT=8, addr=at(3), target=at(6)), mark(4, 11), enc("bi", RA=7),
          mark(6, 12), enc("bi", RA=8)],
         sentinels(7, 8, 10, 11, 12),
         {"r7": ran(0), "r8": ran(3), "r10": ran(1), "r11": ran(4), "r12": ran(6)}),
]

# --- bi: target from the preferred word, masking, interrupt bits ----------------------------------

CASES += [
    case("bi_r0_dispatches_to_exit", BI_REF,
         [mark(0, 10), enc("bi", RA=0)],
         dict(sentinels(10), r0=exit_reg()), {"r10": ran(0)}, channels={"stops": []}),
    # Bits above LSLR and the two low bits are dropped; words 1 to 3 do not matter.
    case("bi_masks_the_target_and_reads_word_0", BI_REF,
         [mark(0, 10), enc("bi", RA=5)],
         dict(sentinels(10), r5=(EXIT_LS | 0x40003, 0xDEADBEEF, 0x12345678, 0x9ABCDEF0)),
         {"r10": ran(0)}, channels={"stops": []}),
    case("bi_reaches_a_listed_entry", BI_REF,
         [mark(0, 10), enc("bi", RA=5), mark(2, 11), mark(3, 12)],
         dict(sentinels(10, 11, 12), r5=(at(3) + 2, 0, 0, 0)),
         {"r10": ran(0), "r11": SENTINEL, "r12": ran(3)}, entries=(at(3),)),
]


def bi_state_case(label: str, mnemonic: str, flags: dict, before: int, after: int) -> Case:
    init = dict(r5=exit_reg(), int_enabled=before)
    return case("%s_%s" % (mnemonic, label), BI_REF if mnemonic == "bi" else IRET_REF,
                [enc(mnemonic, RA=5, **flags)] if mnemonic == "bi" else [enc(mnemonic, **flags)],
                init if mnemonic == "bi" else {"srr0": EXIT_LS, "int_enabled": before},
                {"int_enabled": after})


CASES += [
    bi_state_case("e_enables", "bi", {"E": 1}, 0, 1),
    bi_state_case("d_disables", "bi", {"D": 1}, 1, 0),
    bi_state_case("plain_keeps_enabled", "bi", {}, 1, 1),
    bi_state_case("plain_keeps_disabled", "bi", {}, 0, 0),
]

# --- bisl: the target is read before the link is written -----------------------------------------

BISL_BODY = (mark(1, 10), stop(0x102), mark(3, 11))

CASES += [
    case("bisl_r0_reads_the_target_before_the_link", BISL_REF,
         [enc("bisl", RT=0, RA=0), *BISL_BODY, enc("bi", RA=0)],
         dict(sentinels(10, 11), r0=(at(3), 0, 0, 0)),
         {"r0": ran(0), "r10": ran(1), "r11": ran(3)}, entries=(at(3),)),
    case("bisl_link_register_differs_from_target", BISL_REF,
         [enc("bisl", RT=5, RA=6), *BISL_BODY, enc("bi", RA=5)],
         dict(sentinels(5, 10, 11), r6=(at(3), 1, 2, 3)),
         {"r5": ran(0), "r6": (at(3), 1, 2, 3), "r10": ran(1), "r11": ran(3)}, entries=(at(3),)),
    case("bisl_to_exit_still_links", BISL_REF,
         [enc("bisl", RT=9, RA=5), mark(1, 10)],
         dict(sentinels(9, 10), r5=exit_reg()), {"r9": ran(0), "r10": SENTINEL}, channels={"stops": []}),
]

# --- brz, brnz, brhz, brhnz: which slot is tested -------------------------------------------------

# (mnemonic, value of r3, taken). The branch skips the first marker; r10 stays untouched when taken.
RELATIVE_ROWS = [
    ("brz", "zero_word", (0, 1, 2, 3), True),
    ("brz", "word_0_nonzero", (1, 0, 0, 0), False),
    ("brz", "high_halfword_set", (0x00010000, 0, 0, 0), False),
    ("brz", "sign_bit_set", (0x80000000, 0, 0, 0), False),
    ("brnz", "nonzero_word", (1, 0, 0, 0), True),
    ("brnz", "sign_bit_set", (0x80000000, 0, 0, 0), True),
    ("brnz", "zero_word_others_set", (0, 1, 2, 3), False),
    ("brhz", "low_halfword_zero", (0x00010000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF), True),
    ("brhz", "high_halfword_ignored", (0xFFFF0000, 0, 0, 0), True),
    ("brhz", "low_halfword_set", (0x0000FFFF, 0, 0, 0), False),
    ("brhz", "low_bit_set", (0x00000001, 0, 0, 0), False),
    ("brhnz", "low_halfword_set", (0x0000FFFF, 0, 0, 0), True),
    ("brhnz", "low_bit_set", (0x00000001, 0, 0, 0), True),
    ("brhnz", "high_halfword_only", (0xFFFF0000, 5, 5, 5), False),
    ("brhnz", "zero", (0, 0, 0, 0), False),
]


def relative_case(mnemonic: str, label: str, value, taken: bool) -> Case:
    words = [enc(mnemonic, RT=3, addr=at(0), target=at(2)), mark(1, 10), mark(2, 11)]
    expect = {"r10": SENTINEL if taken else ran(1), "r11": ran(2)}
    ref = BRZ_REF if mnemonic in ("brz", "brnz") else BRHZ_REF
    return case("%s_%s_%s" % (mnemonic, label, "taken" if taken else "not_taken"), ref, words,
                dict(sentinels(10, 11), r3=value), expect)


CASES += [relative_case(*row) for row in RELATIVE_ROWS]

# --- biz, binz, bihz, bihnz: conditions and the interrupt bits ------------------------------------

# The exit address is the branch target, so a taken branch ends the case and the marker after it is
# skipped. A branch that is not taken leaves the interrupt state alone (the bit change belongs to the
# transfer).
CONDITIONAL_ROWS = [
    # (mnemonic, label, value of r3, taken, flags, int_enabled before, after)
    ("biz", "zero_taken", (0, 9, 9, 9), True, {}, 1, 1),
    ("biz", "nonzero_not_taken", (1, 0, 0, 0), False, {}, 0, 0),
    ("biz", "high_halfword_not_taken", (0x00010000, 0, 0, 0), False, {}, 0, 0),
    ("binz", "nonzero_taken", (0x00010000, 0, 0, 0), True, {}, 0, 0),
    ("binz", "zero_not_taken", (0, 1, 1, 1), False, {}, 1, 1),
    ("bihz", "low_halfword_zero_taken", (0x00010000, 1, 1, 1), True, {}, 0, 0),
    ("bihz", "low_halfword_set_not_taken", (0x0000FFFF, 0, 0, 0), False, {}, 1, 1),
    ("bihnz", "low_halfword_set_taken", (0x00000001, 0, 0, 0), True, {}, 1, 1),
    ("bihnz", "high_halfword_only_not_taken", (0xFFFF0000, 1, 1, 1), False, {}, 0, 0),
    ("biz", "taken_e_enables", (0, 0, 0, 0), True, {"E": 1}, 0, 1),
    ("biz", "taken_d_disables", (0, 0, 0, 0), True, {"D": 1}, 1, 0),
    ("biz", "not_taken_e_keeps_state", (1, 0, 0, 0), False, {"E": 1}, 0, 0),
    ("biz", "not_taken_d_keeps_state", (1, 0, 0, 0), False, {"D": 1}, 1, 1),
    ("binz", "taken_e_enables", (1, 0, 0, 0), True, {"E": 1}, 0, 1),
    ("binz", "not_taken_d_keeps_state", (0, 0, 0, 0), False, {"D": 1}, 1, 1),
    ("bihz", "taken_d_disables", (0x00010000, 0, 0, 0), True, {"D": 1}, 1, 0),
    ("bihnz", "taken_e_enables", (0x0000FFFF, 0, 0, 0), True, {"E": 1}, 0, 1),
]


def conditional_case(mnemonic: str, label: str, value, taken: bool, flags: dict, before: int,
                     after: int) -> Case:
    words = [enc(mnemonic, RT=3, RA=5, **flags), mark(1, 10)]
    return case("%s_%s" % (mnemonic, label), BIZ_REF, words,
                dict(sentinels(10), r3=value, r5=exit_reg(), int_enabled=before),
                {"r10": SENTINEL if taken else ran(1), "int_enabled": after})


CASES += [conditional_case(*row) for row in CONDITIONAL_ROWS]

# --- bisled: taken when an event is pending ------------------------------------------------------

# Channel 0 is SPU_RdEventStat; a non-zero count means an event is pending.
CASES += [
    case("bisled_taken_when_event_pending", BISLED_REF,
         [enc("bisled", RT=9, RA=5), mark(1, 10)],
         dict(sentinels(9, 10), r5=exit_reg()), {"r9": ran(0), "r10": SENTINEL},
         channels={"counts": {0: 1}}),
    case("bisled_falls_through_without_event", BISLED_REF,
         [enc("bisled", RT=9, RA=5), mark(1, 10)],
         dict(sentinels(10), r5=exit_reg()), {"r10": ran(1)}, channels={"counts": {0: 0}}),
    case("bisled_taken_with_any_nonzero_count_and_e_bit", BISLED_REF,
         [enc("bisled", RT=9, RA=5, E=1), mark(1, 10)],
         dict(sentinels(9, 10), r5=exit_reg(), int_enabled=0),
         {"r9": ran(0), "r10": SENTINEL, "int_enabled": 1}, channels={"counts": {0: 3}}),
]

# --- iret: branch to SRR0 ------------------------------------------------------------------------

CASES += [
    case("iret_branches_to_srr0", IRET_REF,
         [mark(0, 10), enc("iret")],
         dict(sentinels(10), srr0=EXIT_LS), {"r10": ran(0), "srr0": EXIT_LS}, channels={"stops": []}),
    case("iret_masks_srr0", IRET_REF,
         [mark(0, 10), enc("iret")],
         dict(sentinels(10), srr0=EXIT_LS | 0x40003), {"r10": ran(0)}, channels={"stops": []}),
    case("iret_reaches_a_listed_entry", IRET_REF,
         [mark(0, 10), enc("iret"), mark(2, 11), mark(3, 12)],
         dict(sentinels(10, 11, 12), srr0=at(3)), {"r10": ran(0), "r11": SENTINEL, "r12": ran(3)},
         entries=(at(3),)),
    # A plain iret leaves the interrupt state as it is; the E and D forms change it like bie and bid.
    bi_state_case("plain_keeps_enabled", "iret", {}, 1, 1),
    bi_state_case("plain_keeps_disabled", "iret", {}, 0, 0),
    bi_state_case("e_enables", "iret", {"E": 1}, 0, 1),
    bi_state_case("d_disables", "iret", {"D": 1}, 1, 0),
]

# --- stop ----------------------------------------------------------------------------------------

CASES += [
    case("stop_0x100_continues", STOP_REF,
         [stop(0x100), mark(1, 10)],
         sentinels(10), {"r10": ran(1)}, channels={"stops": [0x100, 0x102]}),
    case("stop_with_a_large_code_continues", STOP_REF,
         [stop(0x3FFF), mark(1, 10)],
         sentinels(10), {"r10": ran(1)}, channels={"stops": [0x3FFF, 0x102]}),
    case("stop_0x102_ends_the_thread", STOP_REF,
         [mark(0, 10), stop(0x102), mark(2, 11)],
         sentinels(10, 11), {"r10": ran(0), "r11": SENTINEL}, channels={"stops": [0x102]}),
    case("stop_0x101_ends_the_thread", STOP_REF,
         [mark(0, 10), stop(0x101), mark(2, 11)],
         sentinels(10, 11), {"r10": ran(0), "r11": SENTINEL}, channels={"stops": [0x101]}),
    # The hook refuses to continue: nothing after the stop runs, not even the appended terminator.
    case("stop_hook_asks_to_leave", STOP_REF,
         [mark(0, 9), stop(0x100), mark(2, 10)],
         sentinels(9, 10), {"r9": ran(0), "r10": SENTINEL},
         channels={"stops": [0x100], "stop_leave": (0x100,)}),
]

# --- instructions without architectural effect on a private local store ---------------------------

CASES += [
    case("hints_and_sync_have_no_effect", HINT_REF,
         [enc("nop"), enc("lnop"), enc("sync"), enc("dsync"),
          enc("hbrr", addr=at(4), site=at(6), target=at(7)), enc("hbr", RA=3, addr=at(5), site=at(6)),
          mark(6, 10)],
         dict(sentinels(10), r3=(1, 2, 3, 4)), {"r3": (1, 2, 3, 4), "r10": ran(6)}),
]

# --- control transfers between functions ----------------------------------------------------------

CASES += [
    # Word 1 is a listed entry, so the first function ends in a jump to the second.
    case("fall_through_into_the_next_function", BR_REF,
         [mark(0, 10), mark(1, 11), mark(2, 12)],
         sentinels(10, 11, 12), {"r10": ran(0), "r11": ran(1), "r12": ran(2)}, entries=(at(1),)),
]

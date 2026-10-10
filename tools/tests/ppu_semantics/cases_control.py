"""Control-group cases: branches, calls, switches, syscalls and traps.

Only control words are used, so every outcome is observed through LR, CTR, CR and r3. The path a
case takes is identified by which `bl` site left its return address in LR; the callees are
`A` (calls `B`, so LR ends at A + 4 after it ran) and `B` (just returns).
"""

from tests import ppc_asm as asm

from . import CODE_BASE, Case

A = 0x18000
B = 0x18100
CALLEES = {A: (asm.bl(A, B),), B: (asm.blr(),)}
SENTINEL = 0x5A5A5A5A       # initial LR; untouched when no `bl` ran
A_RAN = A + 4               # LR after A (and B) ran
EQ = 2                      # CR field nibble with only EQ set
NOP = asm.tw(0, 0, 0)       # TO = 0 emits no statement

BC_REF = "RPCS3 PPUInterpreter.cpp BC"
BCLR_REF = "RPCS3 PPUInterpreter.cpp BCLR"
BCCTR_REF = "RPCS3 PPUInterpreter.cpp BCCTR"


def at(index: int) -> int:
    return CODE_BASE + 4 * index


def case(name: str, ref: str, words, init=None, expect=None, **kwargs) -> Case:
    return Case(name, ref, tuple(words), init or {}, expect or {}, callees=CALLEES, **kwargs)


def ret_of(index: int) -> int:
    """LR value left by a `bl` at word `index`."""
    return at(index + 1)


# --- bc: every BO class, taken and not taken (BI = 2, CR0 EQ) ----------------------------------

# (BO class, ctr, CR0 nibble, taken, CTR afterwards). A taken branch skips the `bl B` marker.
BC_ROWS = [
    ("bdnzf", 0, 2, 0, True, 1), ("bdnzf", 0, 1, 0, False, 0), ("bdnzf", 0, 2, EQ, False, 1),
    ("bdzf", 2, 1, 0, True, 0), ("bdzf", 2, 2, 0, False, 1), ("bdzf", 2, 1, EQ, False, 0),
    ("bf", 4, 5, 0, True, 5), ("bf", 4, 5, EQ, False, 5),
    ("bdnzt", 8, 2, EQ, True, 1), ("bdnzt", 8, 1, EQ, False, 0), ("bdnzt", 8, 2, 0, False, 1),
    ("bdzt", 10, 1, EQ, True, 0), ("bdzt", 10, 2, EQ, False, 1), ("bdzt", 10, 1, 0, False, 0),
    ("bt", 12, 5, EQ, True, 5), ("bt", 12, 5, 0, False, 5),
    ("bdnz", 16, 2, 0, True, 1), ("bdnz", 16, 1, 0, False, 0),
    ("bdz", 18, 1, 0, True, 0), ("bdz", 18, 2, 0, False, 1),
    ("always", 20, 5, 0, True, 5),
]


def bc_case(label: str, bo: int, ctr: int, nibble: int, taken: bool, ctr_after: int) -> Case:
    name = "bc_%s_%s_ctr%d_cr%d" % (label, "taken" if taken else "not_taken", ctr, nibble)
    words = [asm.bc(bo, 2, at(0), at(2)), asm.bl(at(1), B), asm.blr()]
    return case(name, BC_REF, words,
                {"ctr": ctr, "cr0": nibble, "lr": SENTINEL},
                {"ctr": ctr_after, "lr": SENTINEL if taken else ret_of(1)})


def bc_bit_case(label: str, bi: int, init: dict, taken: bool) -> Case:
    words = [asm.bc(12, bi, at(0), at(2)), asm.bl(at(1), B), asm.blr()]
    return case("bc_bit_%s" % label, BC_REF, words, dict(init, lr=SENTINEL),
                {"lr": SENTINEL if taken else ret_of(1)})


CASES = [bc_case(*row) for row in BC_ROWS]
CASES += [
    # BI selects the bit: lt, gt, so of CR0 and eq of CR7.
    bc_bit_case("lt_taken", 0, {"cr0": 8}, True),
    bc_bit_case("gt_taken", 1, {"cr0": 4}, True),
    bc_bit_case("so_taken", 3, {"cr0": 1}, True),
    bc_bit_case("cr7_eq_taken", 30, {"cr7": EQ}, True),
    bc_bit_case("eq_clear_among_others", 2, {"cr0": 8 | 4 | 1}, False),
    bc_bit_case("cr7_eq_clear", 30, {"cr0": EQ, "cr7": 8 | 4 | 1}, False),
]

# --- b, bl, bcl, backward loops -----------------------------------------------------------------

CASES += [
    case("b_forward_skip", "RPCS3 PPUInterpreter.cpp B",
         [asm.b(at(0), at(2)), asm.bl(at(1), B), asm.blr()],
         {"lr": SENTINEL}, {"lr": SENTINEL}),
    case("b_tail_callee", "RPCS3 PPUInterpreter.cpp B",
         [asm.b(at(0), A), asm.bl(at(1), B), asm.blr()],
         {"lr": SENTINEL}, {"lr": A_RAN}),
    case("b_tail_callee_returns", "RPCS3 PPUInterpreter.cpp B",
         [asm.b(at(0), B), asm.bl(at(1), B), asm.blr()],
         {"lr": SENTINEL}, {"lr": SENTINEL}),
    # bdz exits after three bl's; the backward b closes the loop.
    case("b_backward_loop", "RPCS3 PPUInterpreter.cpp B",
         [asm.bc(18, 0, at(0), at(3)), asm.bl(at(1), B), asm.b(at(2), at(0)), asm.blr()],
         {"ctr": 3}, {"ctr": 0, "lr": ret_of(1)}),
    case("bl_sets_lr", "RPCS3 PPUInterpreter.cpp B",
         [asm.bl(at(0), B), asm.blr()], {"lr": SENTINEL}, {"lr": ret_of(0)}),
    case("bl_nested_callee", "RPCS3 PPUInterpreter.cpp B",
         [asm.bl(at(0), A), asm.blr()], {"lr": SENTINEL}, {"lr": A_RAN}),
    # The get-PC idiom; both forms continue at the next word.
    case("bcl_get_pc", "RPCS3 PPUInterpreter.cpp BC",
         [asm.bc(20, 31, at(0), at(1), lk=True), asm.blr()],
         {"lr": SENTINEL}, {"lr": at(1)}),
    case("bcl_get_pc_decrements_ctr", "RPCS3 PPUInterpreter.cpp BC",
         [asm.bc(16, 0, at(0), at(1), lk=True), asm.blr()],
         {"ctr": 5, "lr": SENTINEL}, {"ctr": 4, "lr": at(1)}),
    # Backward conditional branches. The first word keeps the callee within bc range.
    case("bdnz_loop_counts_down", BC_REF,
         [asm.bl(at(0), B), asm.bdnz(at(1), at(0)), asm.blr()],
         {"ctr": 3}, {"ctr": 0, "lr": ret_of(0)}),
    case("bdnz_long_loop_yields", BC_REF,
         [asm.bdnz(at(0), at(0)), asm.blr()], {"ctr": 10000}, {"ctr": 0}),
    case("bdnz_ctr_wraps", BC_REF,
         [asm.bdnz(at(0), at(2)), asm.bl(at(1), B), asm.blr()],
         {"ctr": 0, "lr": SENTINEL}, {"ctr": 0xFFFFFFFFFFFFFFFF, "lr": SENTINEL}),
]

# bc to a callee: LI-style reach is 32 KiB, so the branch sits at word 1.
for _taken, _cr in ((True, EQ), (False, 0)):
    _tag = "taken" if _taken else "not_taken"
    CASES.append(case("bc_tail_callee_%s" % _tag, BC_REF,
                      [NOP, asm.bc(12, 2, at(1), A), asm.bl(at(2), B), asm.blr()],
                      {"cr0": _cr, "lr": SENTINEL}, {"lr": A_RAN if _taken else ret_of(2)}))
    CASES.append(case("bc_call_callee_%s" % _tag, BC_REF,
                      [NOP, asm.bc(12, 2, at(1), A, lk=True), asm.blr()],
                      {"cr0": _cr, "lr": SENTINEL}, {"lr": A_RAN if _taken else ret_of(1)}))

# --- bclr, bclrl ---------------------------------------------------------------------------------

BCLR_WORDS = [None, asm.bl(at(1), B), asm.blr()]     # word 0 is the bclr under test


def bclr_case(name: str, bo: int, init: dict, expect: dict) -> Case:
    return case(name, BCLR_REF, [asm.bclr(bo, 2)] + BCLR_WORDS[1:], dict(init, lr=SENTINEL),
                dict(expect))


CASES += [
    case("blr_returns_early", BCLR_REF, [asm.blr(), asm.bl(at(1), B), asm.blr()],
         {"lr": SENTINEL}, {"lr": SENTINEL}),
    bclr_case("bclr_beq_taken", 12, {"cr0": EQ}, {"lr": SENTINEL}),
    bclr_case("bclr_beq_not_taken", 12, {"cr0": 0}, {"lr": ret_of(1)}),
    bclr_case("bclr_bne_taken", 4, {"cr0": 0}, {"lr": SENTINEL}),
    bclr_case("bclr_bdnz_taken", 16, {"ctr": 2}, {"ctr": 1, "lr": SENTINEL}),
    bclr_case("bclr_bdnz_not_taken", 16, {"ctr": 1}, {"ctr": 0, "lr": ret_of(1)}),
    bclr_case("bclr_bdz_taken", 18, {"ctr": 1}, {"ctr": 0, "lr": SENTINEL}),
    # LK=1 reads the target from LR before overwriting it; callee A leaves LR at A + 4.
    case("bclrl_calls_lr_target", BCLR_REF, [asm.bclr(20, 0, lk=True)],
         {"lr": A}, {"lr": A_RAN}),
    case("bclrl_beq_taken", BCLR_REF, [asm.bclr(12, 2, lk=True)],
         {"lr": A, "cr0": EQ}, {"lr": A_RAN}),
    case("bclrl_beq_not_taken", BCLR_REF, [asm.bclr(12, 2, lk=True)],
         {"lr": A, "cr0": 0}, {"lr": at(1)}),
    case("bclrl_bdnz_taken", BCLR_REF, [asm.bclr(16, 0, lk=True)],
         {"lr": A, "ctr": 2}, {"lr": A_RAN, "ctr": 1}),
    case("bclrl_bdnz_not_taken", BCLR_REF, [asm.bclr(16, 0, lk=True)],
         {"lr": A, "ctr": 1}, {"lr": at(1), "ctr": 0}),
]

# --- bcctr, bcctrl, switches ---------------------------------------------------------------------

CASES += [
    case("bctr_dynamic_tail_callee", BCCTR_REF, [asm.bctr()], {"ctr": A}, {"lr": A_RAN}),
    case("bctrl_nested_callee", BCCTR_REF, [asm.bctrl()], {"ctr": A, "lr": SENTINEL},
         {"lr": A_RAN}),
    case("bctrl_leaf_callee", BCCTR_REF, [asm.bctrl()], {"ctr": B, "lr": SENTINEL},
         {"lr": at(1)}),
    case("bcctr_beq_taken", BCCTR_REF, [asm.bcctr(12, 2), asm.bl(at(1), B), asm.blr()],
         {"ctr": A, "cr0": EQ}, {"lr": A_RAN}),
    case("bcctr_beq_not_taken", BCCTR_REF, [asm.bcctr(12, 2), asm.bl(at(1), B), asm.blr()],
         {"ctr": A, "cr0": 0, "lr": SENTINEL}, {"lr": ret_of(1)}),
    case("bcctrl_beq_taken", BCCTR_REF, [asm.bcctr(12, 2, lk=True)],
         {"ctr": A, "cr0": EQ}, {"lr": A_RAN}),
    case("bcctrl_beq_not_taken", BCCTR_REF, [asm.bcctr(12, 2, lk=True)],
         {"ctr": A, "cr0": 0, "lr": SENTINEL}, {"lr": at(1)}),
]

SWITCH_WORDS = [asm.bctr(), asm.bl(at(1), B), asm.blr(), asm.bl(at(3), B), asm.blr()]
SWITCH_TARGETS = {at(0): (at(1), at(3))}


def switch_case(name: str, ctr: int, lr: int, **kwargs) -> Case:
    return case(name, BCCTR_REF, SWITCH_WORDS, {"ctr": ctr, "lr": SENTINEL},
                {"lr": lr}, switch_targets=kwargs.get("targets", SWITCH_TARGETS))


CASES += [
    switch_case("switch_first_target", at(1), ret_of(1)),
    switch_case("switch_second_target", at(3), ret_of(3)),
    # Not in the table: falls to tox2Dispatch.
    switch_case("switch_default_dispatches_callee", A, A_RAN),
    # A table entry outside the function is a tail transfer.
    switch_case("switch_callee_target", A, A_RAN, targets={at(0): (at(1), A)}),
    switch_case("switch_intra_target_with_callee_entry", at(1), ret_of(1),
                targets={at(0): (at(1), A)}),
    # A backward target adds a loop hook; ctr picks the exit.
    case("switch_backward_target", BCCTR_REF, [asm.bl(at(0), B), asm.bctr(), asm.blr()],
         {"ctr": at(2), "lr": SENTINEL}, {"lr": ret_of(0)},
         switch_targets={at(1): (at(0), at(2))}),
]

# --- sc ---------------------------------------------------------------------------------------

CASES.append(case("sc_stub_returns_zero_in_r3", "RPCS3 PPUInterpreter.cpp SC", [asm.sc()],
                  {"r3": 7, "r11": 0x1234}, {"r3": 0}))

# --- traps whose conditions are not met --------------------------------------------------------

# Each trap is followed by a `bl B`; reaching it (LR = ret_of(1)) proves the trap did not fire.


def trap_case(name: str, ref: str, word: int, init: dict) -> Case:
    return case(name, ref, [word, asm.bl(at(1), B), asm.blr()], dict(init, lr=SENTINEL),
                {"lr": ret_of(1)})


M32 = 0xFFFFFFFF
CASES += [
    trap_case("tw_to0_never_traps", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0, 3, 4),
              {"r3": 5, "r4": 5}),
    trap_case("twi_to0_never_traps", "RPCS3 PPUInterpreter.cpp TWI", asm.twi(0, 3, 5),
              {"r3": 5}),
    trap_case("td_to0_never_traps", "RPCS3 PPUInterpreter.cpp TD", asm.td(0, 3, 4),
              {"r3": 5, "r4": 5}),
    trap_case("tdi_to0_never_traps", "RPCS3 PPUInterpreter.cpp TDI", asm.tdi(0, 3, 5),
              {"r3": 5}),
    trap_case("tw_eq_not_met", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0x04, 3, 4),
              {"r3": 1, "r4": 2}),
    # tw compares the low words: 0x80000000 is negative, so it is not greater than 1.
    trap_case("tw_gt_uses_signed_low_word", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0x08, 3, 4),
              {"r3": 0x80000000, "r4": 1}),
    # A 64-bit compare would see 5 < 0x100000003.
    trap_case("tw_lt_ignores_high_word", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0x10, 3, 4),
              {"r3": 5, "r4": 0x100000003}),
    trap_case("tw_gtu_ignores_high_word", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0x01, 3, 4),
              {"r3": 0x100000001, "r4": 2}),
    trap_case("tw_ltu_not_met", "RPCS3 PPUInterpreter.cpp TW", asm.tw(0x02, 3, 4),
              {"r3": M32, "r4": 1}),
    trap_case("twi_gt_not_met", "RPCS3 PPUInterpreter.cpp TWI", asm.twi(0x08, 3, 5),
              {"r3": 3}),
    # SIMM -1 is 0xffffffff for the unsigned compare.
    trap_case("twi_gtu_negative_simm", "RPCS3 PPUInterpreter.cpp TWI", asm.twi(0x01, 3, -1),
              {"r3": 5}),
    trap_case("td_gt_is_64_bit", "RPCS3 PPUInterpreter.cpp TD", asm.td(0x08, 3, 4),
              {"r3": 5, "r4": 0x100000003}),
    trap_case("td_ltu_not_met", "RPCS3 PPUInterpreter.cpp TD", asm.td(0x02, 3, 4),
              {"r3": 5, "r4": 3}),
    trap_case("tdi_lt_negative_simm", "RPCS3 PPUInterpreter.cpp TDI", asm.tdi(0x10, 3, -1),
              {"r3": 5}),
]

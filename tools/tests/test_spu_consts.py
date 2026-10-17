"""Tests for the forward constant pass over one emitted SPU function."""

import re
import unittest

from ps3recomp import spu, spu_emit, spu_emit_core, spu_recover
from ps3recomp.spu_consts import KILL_ALL, fsmbi_words, function_constants
from ps3recomp.spu_flow import SpuCode

from tests import spu_asm
from tests.test_spu_recover import BASE, at, br, brsl, nop, ret, synthetic_recovery

R5 = 5
SPLAT = (0x00010203,) * 4


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def stop(code: int = 0x102) -> int:
    return enc("stop", STOP=code)


def set_r5() -> list[int]:
    return [enc("ilhu", RT=R5, U16=0x0001), enc("iohl", RT=R5, U16=0x0203)]


def shufb(rt: int = 6, rc: int = R5) -> int:
    return enc("shufb", RT4=rt, RA=3, RB=3, RC=rc)


def consts(words, *, seeds=(0,), owned=None, handled=lambda name: True):
    """`function_constants` over `words` placed at BASE; seeds and owned words are word indices."""
    code = SpuCode.from_words(BASE, words)
    owned = range(len(words)) if owned is None else owned
    return function_constants(code, frozenset(at(i) for i in owned), [at(i) for i in seeds], handled)


class ConstantPropagationTest(unittest.TestCase):
    def test_constant_set_before_a_loop_label_reaches_the_shufb_in_the_loop(self):
        words = [*set_r5(), shufb(), enc("ai", RT=3, RA=3, I10=-1), enc("brnz", RT=3, addr=at(4), target=at(2)),
                 stop()]
        known = consts(words).at(at(2))
        self.assertEqual(known, {R5: SPLAT})

    def test_different_constants_on_two_paths_are_dropped_and_equal_ones_kept(self):
        def program(second):
            return [enc("brnz", RT=3, addr=at(0), target=at(3)), enc("il", RT=R5, I16=1), br(2, 4),
                    enc("il", RT=R5, I16=second), shufb(), stop()]

        self.assertNotIn(R5, consts(program(2)).at(at(4)))
        self.assertEqual(consts(program(1)).at(at(4))[R5], (1,) * 4)

    def test_a_call_kills_everything_at_its_return_site(self):
        # Word 3 belongs to the callee; the return site (word 2) is a dispatchable block of its own.
        words = [enc("il", RT=R5, I16=1), brsl(1, 3), shufb(), ret()]
        c = consts(words, seeds=(0, 2), owned=(0, 1, 2))
        self.assertEqual(c.at(at(1)), {R5: (1,) * 4})
        self.assertEqual(c.at(at(2)), {})

    def test_return_site_of_a_recovered_call_has_no_constants(self):
        words = [enc("il", RT=R5, I16=1), brsl(1, 4), shufb(), stop(), ret()]
        rec = synthetic_recovery(words)
        db = spu_emit.parse_database(spu_recover.build_database(rec))
        model = spu_emit.emit_one(rec.image, db).model
        self.assertIn(at(2), model.entry_cases(at(0)))
        c = function_constants(model.code, frozenset(model.functions[at(0)]), model.entry_cases(at(0)),
                               spu_emit_core.is_handled)
        self.assertEqual(c.at(at(1)), {R5: (1,) * 4})
        self.assertEqual(c.at(at(2)), {})

    def test_a_dispatchable_block_never_gains_constants(self):
        words = [enc("il", RT=R5, I16=1), nop(), shufb(), stop()]
        self.assertEqual(consts(words, seeds=(0, 2)).at(at(2)), {})
        self.assertEqual(consts(words, seeds=(0,)).at(at(2)), {R5: (1,) * 4})

    def test_a_dispatchable_loop_head_stays_empty_although_the_back_edge_carries_a_constant(self):
        words = [shufb(), enc("il", RT=R5, I16=1), br(2, 0)]
        self.assertEqual(consts(words, seeds=(0,)).at(at(0)), {})

    def test_rt4_write_drops_the_register(self):
        words = [*set_r5(), enc("selb", RT4=R5, RA=3, RB=4, RC=6), shufb(), stop()]
        self.assertNotIn(R5, consts(words).at(at(3)))
        self.assertEqual(consts(words).at(at(2)), {R5: SPLAT})

    def test_link_writes_drop_the_register(self):
        pc_read = enc("brsl", RT=R5, addr=at(2), target=at(3))
        self.assertNotIn(R5, consts([*set_r5(), pc_read, shufb(), stop()]).at(at(3)))
        bisled = enc("bisled", RT=R5, RA=7)
        c = consts([*set_r5(), bisled, shufb(), stop()])
        self.assertNotIn(R5, c.at(at(3)))

    def test_other_registers_survive_a_write(self):
        words = [*set_r5(), enc("il", RT=7, I16=9), shufb(), stop()]
        self.assertEqual(consts(words).at(at(3)), {R5: SPLAT, 7: (9,) * 4})

    def test_fsmbi_and_word_ops_work_on_all_four_words(self):
        self.assertEqual(fsmbi_words(0x8001), (0xFF000000, 0, 0, 0x000000FF))
        self.assertEqual(fsmbi_words(0xFFFF), (0xFFFFFFFF,) * 4)
        self.assertEqual(fsmbi_words(0x0001 << 4), (0, 0, 0x000000FF, 0))
        words = [enc("fsmbi", RT=R5, U16=0x8001), enc("il", RT=6, I16=0x10), enc("or", RT=7, RA=R5, RB=6),
                 enc("il", RT=8, I16=-1), nop(), stop()]
        known = consts(words).at(at(4))
        self.assertEqual(known[R5], (0xFF000000, 0, 0, 0x000000FF))
        self.assertEqual(known[7], (0xFF000010, 0x10, 0x10, 0x000000FF))
        self.assertEqual(known[8], (0xFFFFFFFF,) * 4)

    def test_every_word_op_matches_the_backward_evaluator(self):
        # Same arithmetic as spu_flow.eval_const for the preferred slot.
        from ps3recomp.spu_flow import eval_const
        words = [enc("ilh", RT=3, U16=0x1234), enc("ila", RT=4, I18=0x3FFFF), enc("shli", RT=5, RA=3, I7=35),
                 enc("shli", RT=6, RA=3, I7=4), enc("sf", RT=7, RA=3, RB=4), enc("a", RT=8, RA=3, RB=4),
                 enc("andi", RT=9, RA=4, I10=-2), enc("ori", RT=10, RA=3, I10=-1), enc("ai", RT=11, RA=3, I10=-5),
                 enc("and", RT=12, RA=3, RB=4), enc("or", RT=13, RA=12, RB=12), nop(), stop()]
        known = consts(words).at(at(11))
        code = SpuCode.from_words(BASE, words)
        for reg in range(5, 14):
            self.assertEqual(len(set(known[reg])), 1, reg)
            self.assertEqual(known[reg][0], eval_const(code, at(11), reg), reg)

    def test_channel_stop_and_spr_hooks_kill_everything(self):
        hooks = [enc("rdch", RT=6, CA=13), enc("wrch", CA=21, RT=3), stop(0x100), enc("rchcnt", RT=6, CA=4),
                 enc("mfspr", RT=6), enc("mtspr", RT=6), enc("fscrrd", RT=6), enc("fscrwr", RA=3)]
        for hook in hooks:
            known = consts([*set_r5(), enc("il", RT=7, I16=1), hook, shufb(), stop()]).at(at(4))
            self.assertEqual(known, {}, hex(hook))

    def test_loads_drop_the_destination_only(self):
        words = [*set_r5(), enc("il", RT=7, I16=1), enc("lqr", RT=R5, addr=at(3), target=at(10)), shufb(), stop()]
        self.assertEqual(consts(words).at(at(4)), {7: (1,) * 4})

    def test_unhandled_mnemonic_kills_everything_after_it(self):
        words = [*set_r5(), nop(), shufb(), stop()]
        self.assertEqual(consts(words, handled=lambda name: name != "nop").at(at(3)), {})
        self.assertEqual(consts(words).at(at(3)), {R5: SPLAT})

    def test_shufb_that_overwrites_its_control_sees_the_old_value_only(self):
        words = [*set_r5(), shufb(rt=R5), nop(), stop()]
        c = consts(words)
        self.assertEqual(c.at(at(2)), {R5: SPLAT})
        self.assertNotIn(R5, c.at(at(3)))

    def test_owned_word_that_no_seed_reaches_has_no_constants(self):
        words = [stop(), enc("il", RT=R5, I16=1), shufb(), stop()]
        c = consts(words, seeds=(0,))
        self.assertEqual(c.at(at(2)), {})
        self.assertEqual(c.at(at(1)), {})

    def test_transfers_to_words_of_another_function_end_the_flow(self):
        words = [*set_r5(), br(2, 4), shufb(), stop(), ret()]
        c = consts(words, owned=(0, 1, 2, 3))
        self.assertEqual(c.at(at(3)), {})

    def test_conditional_halt_falls_through(self):
        words = [*set_r5(), enc("heqi", RA=3, I10=0), shufb(), stop()]
        self.assertEqual(consts(words).at(at(3)), {R5: SPLAT})

    def test_terminating_stop_has_no_successor(self):
        words = [*set_r5(), stop(0x102), shufb(), stop()]
        self.assertEqual(consts(words).at(at(3)), {})


class HookGuardTest(unittest.TestCase):
    CALL = re.compile(r"\b(tox2\w+)\(ctx, ls")
    NORETURN = frozenset({"tox2SpuHalt", "tox2SpuUnhandled"})

    def test_every_handler_that_calls_a_runtime_hook_is_in_kill_all(self):
        lowering = spu_emit_core.get_lowering("stackfree")
        handlers = spu_emit_core.load_handlers()
        self.assertGreater(len(handlers), 100)
        checked = 0
        for name, fn in sorted(handlers.items()):
            insn = spu.decode(spu_asm.encode(name), BASE)
            self.assertEqual(insn.spec.mnemonic, name)
            lines = fn(spu_emit_core.SpuOp(insn, lowering))
            if lines is None:
                continue
            checked += 1
            for call in self.CALL.findall("\n".join(lines)):
                if call not in self.NORETURN:
                    self.assertIn(name, KILL_ALL, "%s calls %s but is not in KILL_ALL" % (name, call))
        self.assertGreater(checked, 100)

    def test_kill_all_names_real_mnemonics(self):
        for name in KILL_ALL:
            self.assertIn(name, spu.SPECS_BY_NAME)


if __name__ == "__main__":
    unittest.main()

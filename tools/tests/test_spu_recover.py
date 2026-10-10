"""Tests for SPU function recovery on synthetic programs."""

import unittest

from ps3recomp.spu_flow import SpuCode
from ps3recomp.spu_recover import (CallSite, ConstantTarget, IndirectSite, TailEdge, recover)

from tests import spu_asm

BASE = 0x1000
DATA = 0x4000
OUTSIDE = 0x20000


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def at(index: int) -> int:
    return BASE + 4 * index


def nop() -> int:
    return enc("nop", RT=1)


def ret() -> int:
    return enc("bi", RA=0)


def build(words, data=()) -> SpuCode:
    blob = b"".join(w.to_bytes(4, "big") for w in words)
    return SpuCode([(BASE, blob)], data)


def run(words, entry=0, **kwargs):
    extra = [at(i) for i in kwargs.pop("extra", ())]
    data = kwargs.pop("data", ())
    return recover(build(words, data), at(entry), extra, **kwargs)


def br(index: int, target: int) -> int:
    return enc("br", addr=at(index), target=at(target))


def brsl(index: int, target: int, link: int = 0) -> int:
    return enc("brsl", RT=link, addr=at(index), target=at(target))


class CallReturnTest(unittest.TestCase):
    def test_call_and_its_return_site(self):
        words = [brsl(0, 3), nop(), enc("stop", STOP=0x102), nop(), ret()]
        result = run(words)
        self.assertTrue(result.converged)
        self.assertEqual(result.rounds, 2)
        self.assertEqual(sorted(result.functions), [at(0), at(3)])
        caller, callee = result.functions[at(0)], result.functions[at(3)]
        self.assertEqual(caller.sources, {"entry"})
        self.assertEqual(callee.sources, {"brsl"})
        self.assertEqual(caller.addrs, {at(0), at(1), at(2)})
        self.assertEqual(callee.addrs, {at(3), at(4)})
        self.assertEqual(caller.calls, {at(3)})
        self.assertEqual(caller.call_sites, (CallSite(at(0), "brsl", at(3), 0),))
        self.assertEqual(caller.return_sites(), (at(1),))
        self.assertEqual(caller.returns, ())
        self.assertEqual(callee.returns, (at(4),))
        self.assertEqual(callee.call_sites, ())
        self.assertEqual(callee.return_sites(), ())
        self.assertEqual(caller.tail_edges, ())

    def test_absolute_call_and_link_register(self):
        words = [enc("brasl", RT=2, addr=at(0), target=at(3)), nop(), enc("stop", STOP=0x102), ret()]
        result = run(words)
        self.assertEqual(result.functions[at(3)].sources, {"brasl"})
        self.assertEqual(result.functions[at(0)].call_sites, (CallSite(at(0), "brasl", at(3), 2),))

    def test_call_to_a_known_entry_keeps_its_sources(self):
        words = [brsl(0, 3), nop(), enc("stop", STOP=0x102), ret()]
        result = run(words, extra=[3])
        self.assertEqual(result.functions[at(3)].sources, {"extra"})
        self.assertEqual(result.rounds, 1)

    def test_entry_that_is_also_an_extra_entry(self):
        result = run([ret()], extra=[0])
        self.assertEqual(result.functions[at(0)].sources, {"entry", "extra"})

    def test_call_target_outside_the_code_is_external(self):
        words = [enc("brasl", RT=0, addr=at(0), target=OUTSIDE), enc("stop", STOP=0x102)]
        result = run(words)
        self.assertEqual(sorted(result.functions), [at(0)])
        self.assertEqual(result.functions[at(0)].external_targets, {OUTSIDE})
        self.assertEqual(result.functions[at(0)].calls, frozenset())

    def test_call_into_padding_is_rejected(self):
        words = [brsl(0, 3), enc("stop", STOP=0x102), nop(), 0]
        result = run(words)
        self.assertEqual(result.rejected_calls, ((at(0), at(3)),))
        self.assertEqual(sorted(result.functions), [at(0)])

    def test_no_stack_convention_is_assumed_for_the_link_register(self):
        # A call that links through a register other than $0 is still a call.
        words = [brsl(0, 3, link=5), nop(), enc("stop", STOP=0x102), enc("bi", RA=5)]
        result = run(words)
        self.assertEqual(result.functions[at(0)].call_sites[0].link, 5)
        # `bi $5` is not a return; no constant reaches it.
        self.assertEqual(result.functions[at(3)].returns, ())
        self.assertEqual(result.functions[at(3)].indirect_sites[0].resolution, "unresolved")


class ReturnAndIndirectTest(unittest.TestCase):
    def test_returns_with_interrupt_bits_and_conditional_returns(self):
        words = [enc("biz", RT=3, RA=0), enc("bi", RA=0, D=1), nop(), enc("bi", RA=0, E=1)]
        result = run(words)
        fn = result.functions[at(0)]
        self.assertEqual(fn.returns, (at(0), at(1)))
        self.assertEqual(fn.addrs, {at(0), at(1)})

    def test_unresolved_indirect_jump_reports_why(self):
        words = [enc("bi", RA=5), enc("iret")]
        result = run(words, extra=[1])
        self.assertEqual(result.functions[at(0)].indirect_sites[0],
                         IndirectSite(at(0), "bi", "unresolved", (), None, "$5 is not a recoverable constant"))
        iret = result.functions[at(1)].indirect_sites[0]
        self.assertEqual((iret.kind, iret.resolution), ("iret", "unresolved"))
        self.assertEqual([i.site for i in result.unresolved_indirect], [at(0), at(1)])

    def test_conditional_indirect_branch_falls_through(self):
        words = [enc("binz", RT=3, RA=4), nop(), ret()]
        result = run(words)
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1), at(2)})
        self.assertEqual(fn.indirect_sites[0].kind, "binz")
        self.assertEqual(fn.indirect_sites[0].resolution, "unresolved")

    def test_conditional_indirect_branch_to_a_constant(self):
        words = [enc("ila", RT=4, I18=at(4)), enc("binz", RT=3, RA=4), nop(), ret(), nop(), ret()]
        result = run(words)
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1), at(2), at(3)})
        self.assertEqual(result.functions[at(4)].sources, {"constant"})
        self.assertEqual(fn.tail_edges, (TailEdge(at(1), at(4), "constant"),))
        self.assertEqual(result.constant_targets, (ConstantTarget(at(1), at(4), "self"),))

    def test_unconditional_indirect_jump_to_a_constant_has_no_fallthrough(self):
        words = [enc("ila", RT=4, I18=at(3)), enc("bi", RA=4), enc("stop", STOP=0x102), ret()]
        result = run(words)
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})
        self.assertEqual(result.functions[at(3)].sources, {"constant"})
        self.assertEqual(result.functions[at(0)].tail_edges, (TailEdge(at(1), at(3), "constant"),))

    def test_jump_table_targets_are_edges_of_the_function(self):
        words = [enc("bi", RA=5), nop(), ret(), nop(), ret()]
        tables = {at(0): [at(3), at(1), at(3)]}
        result = run(words, jump_tables=tables)
        self.assertEqual(sorted(result.functions), [at(0)])
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1), at(2), at(3), at(4)})
        self.assertEqual(fn.indirect_sites, (IndirectSite(at(0), "bi", "jump_table", (at(1), at(3)), None),))
        self.assertEqual(result.unresolved_indirect, ())


class ConstantPointerTest(unittest.TestCase):
    def test_ila_and_bisl_inside_the_image(self):
        words = [enc("ila", RT=4, I18=at(3)), enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102), ret()]
        result = run(words)
        caller, callee = result.functions[at(0)], result.functions[at(3)]
        self.assertEqual(callee.sources, {"constant"})
        self.assertEqual(caller.calls, {at(3)})
        self.assertEqual(caller.addrs, {at(0), at(1), at(2)})
        self.assertEqual(caller.indirect_sites, (IndirectSite(at(1), "bisl", "constant", (at(3),), 0),))
        self.assertEqual(caller.return_sites(), (at(2),))
        self.assertEqual(result.constant_targets, (ConstantTarget(at(1), at(3), "self"),))
        self.assertEqual(result.unresolved_indirect, ())

    def test_ilhu_and_iohl_pair(self):
        words = [enc("ilhu", RT=4, U16=0), enc("iohl", RT=4, U16=at(4)), enc("bisl", RT=0, RA=4),
                 enc("stop", STOP=0x102), ret()]
        result = run(words)
        self.assertEqual(result.constant_targets, (ConstantTarget(at(2), at(4), "self"),))
        self.assertEqual(result.functions[at(4)].sources, {"constant"})

    def test_constant_outside_the_image(self):
        words = [enc("ila", RT=4, I18=OUTSIDE), enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102)]
        result = run(words)
        self.assertEqual(sorted(result.functions), [at(0)])
        self.assertEqual(result.functions[at(0)].external_targets, {OUTSIDE})
        self.assertEqual(result.constant_targets, (ConstantTarget(at(1), OUTSIDE, "external"),))
        self.assertEqual(result.functions[at(0)].indirect_sites[0].resolution, "constant")

    def test_pointer_loaded_from_a_table_in_the_image(self):
        words = [enc("lqr", RT=4, addr=at(0), target=DATA), enc("bisl", RT=0, RA=4),
                 enc("stop", STOP=0x102), ret()]
        table = at(3).to_bytes(4, "big") + bytes(12)
        result = run(words, data=[(DATA, table)])
        self.assertEqual(result.functions[at(3)].sources, {"constant"})
        self.assertEqual(result.constant_targets, (ConstantTarget(at(1), at(3), "self"),))

    def test_data_slot_that_is_not_a_code_address_is_not_a_target(self):
        words = [enc("lqr", RT=4, addr=at(0), target=DATA), enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102)]
        result = run(words, data=[(DATA, bytes(16))])
        self.assertEqual(result.constant_targets, ())
        self.assertEqual(result.functions[at(0)].indirect_sites[0].resolution, "unresolved")

    def test_register_set_on_another_path_is_not_a_constant(self):
        # idx3 is a join point: the first path never sets $4.
        words = [enc("brnz", RT=9, addr=at(0), target=at(3)), enc("ila", RT=4, I18=at(5)), nop(),
                 enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102), ret()]
        result = run(words)
        self.assertEqual(result.functions[at(0)].indirect_sites[0].resolution, "unresolved")
        self.assertEqual(sorted(result.functions), [at(0)])

    def test_constant_set_before_a_later_found_entry_is_not_carried_over(self):
        # idx3 is only reached by a constant call from idx2; it is a function entry, so the
        # value of $4 at idx2 must not leak into the use at idx4.
        words = [enc("ila", RT=4, I18=at(3)), enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102),
                 nop(), enc("bisl", RT=0, RA=4), ret()]
        result = run(words)
        callee = result.functions[at(3)]
        self.assertEqual(callee.indirect_sites[0].resolution, "unresolved")


class TerminationTest(unittest.TestCase):
    def test_stop_102_ends_the_walk(self):
        result = run([nop(), enc("stop", STOP=0x102), nop(), ret()])
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})
        self.assertEqual(result.invalid_flow, ())

    def test_stop_101_and_stopd_end_the_walk(self):
        result = run([enc("stop", STOP=0x101), nop(), ret()])
        self.assertEqual(result.functions[at(0)].addrs, {at(0)})
        result = run([enc("stopd", RC=1, RA=2, RB=3), nop(), ret()])
        self.assertEqual(result.functions[at(0)].addrs, {at(0)})

    def test_stop_100_falls_through(self):
        result = run([nop(), enc("stop", STOP=0x100), nop(), ret()])
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1), at(2), at(3)})

    def test_halt_with_a_constant_true_condition_ends_the_walk(self):
        words = [enc("il", RT=3, I16=0), enc("heqi", RA=3, I10=0), nop(), ret()]
        self.assertEqual(run(words).functions[at(0)].addrs, {at(0), at(1)})
        words[1] = enc("heqi", RA=3, I10=1)
        self.assertEqual(run(words).functions[at(0)].addrs, {at(0), at(1), at(2), at(3)})

    def test_halt_condition_read_from_memory_is_not_constant(self):
        # The image bytes are initial values; the program may have changed them.
        words = [enc("lqr", RT=3, addr=at(0), target=DATA), enc("heqi", RA=3, I10=0), nop(), ret()]
        result = run(words, data=[(DATA, bytes(16))])
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1), at(2), at(3)})

    def test_hints_do_not_change_flow(self):
        words = [enc("hbrr", addr=at(0), site=at(2), target=at(3)), nop(), br(2, 3), ret()]
        result = run(words)
        self.assertEqual(sorted(result.functions), [at(0)])
        self.assertEqual(result.functions[at(0)].addrs, {at(i) for i in range(4)})

    def test_flow_into_padding_and_off_the_image_is_reported(self):
        result = run([nop(), 0, ret()])
        self.assertEqual(result.invalid_flow, ((at(0), at(0), at(1)),))
        result = run([nop()])
        self.assertEqual(result.functions[at(0)].external_targets, {at(1)})

    def test_data_ranges_are_never_code(self):
        words = [nop(), nop(), ret()]
        result = run(words, data_ranges=[(at(1), at(2))])
        self.assertEqual(result.functions[at(0)].addrs, {at(0)})
        self.assertEqual(result.invalid_flow, ((at(0), at(0), at(1)),))

    def test_seed_that_is_not_code_is_rejected(self):
        result = run([nop(), 0, ret()], extra=[1, 2])
        self.assertEqual(result.rejected_seeds, ((at(1), "extra"),))
        self.assertEqual(sorted(result.functions), [at(0), at(2)])


class PartitionTest(unittest.TestCase):
    def test_tail_call_to_another_function(self):
        words = [nop(), br(1, 3), ret(), nop(), ret()]
        result = run(words, extra=[3])
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1)})
        self.assertEqual(fn.tail_edges, (TailEdge(at(1), at(3), "branch"),))

    def test_fall_through_into_another_entry(self):
        result = run([nop(), nop(), ret()], extra=[2])
        self.assertEqual(result.functions[at(0)].tail_edges, (TailEdge(at(1), at(2), "fallthrough"),))

    def test_shared_code_is_promoted_to_a_function(self):
        words = [nop(), br(1, 4), nop(), br(3, 4), nop(), ret()]
        result = run(words, extra=[2])
        self.assertEqual(sorted(result.functions), [at(0), at(2), at(4)])
        shared = result.functions[at(4)]
        self.assertEqual(shared.sources, {"promoted"})
        self.assertEqual(shared.addrs, {at(4), at(5)})
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})
        self.assertEqual(result.functions[at(0)].tail_edges, (TailEdge(at(1), at(4), "branch"),))
        self.assertEqual(result.functions[at(2)].tail_edges, (TailEdge(at(3), at(4), "branch"),))
        self.assertEqual(len(result.promotions), 1)
        promotion = result.promotions[0]
        self.assertEqual((promotion.addr, promotion.round, promotion.reached_from, promotion.edge_sites),
                         (at(4), 1, (at(0), at(2)), (at(1), at(3))))
        self.assertEqual(result.rounds, 2)

    def test_every_address_has_one_owner(self):
        words = [nop(), enc("brnz", RT=3, addr=at(1), target=at(5)), br(2, 6), nop(), ret(),
                 nop(), nop(), ret(), br(8, 5)]
        result = run(words, extra=[3, 8])
        owned = [a for fn in result.functions.values() for a in fn.addrs]
        self.assertEqual(len(owned), len(set(owned)))
        self.assertTrue(result.converged)

    def test_loop_with_a_call_stays_one_function(self):
        words = [brsl(0, 4), enc("brnz", RT=3, addr=at(1), target=at(0)), ret(), nop(), ret()]
        result = run(words)
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1), at(2)})
        self.assertEqual(result.functions[at(0)].return_sites(), (at(1),))


class ConvergenceTest(unittest.TestCase):
    def chain(self, length):
        words = []
        for i in range(length):
            words += [brsl(2 * i, 2 * i + 2), ret()]
        return words + [ret()]

    def test_chain_converges_when_the_last_call_is_found(self):
        result = run(self.chain(5))
        self.assertTrue(result.converged)
        self.assertEqual(result.rounds, 6)
        self.assertEqual(len(result.functions), 6)

    def test_round_limit_reports_no_convergence(self):
        result = run(self.chain(5), max_rounds=3)
        self.assertFalse(result.converged)
        self.assertEqual(result.rounds, 3)
        result = run(self.chain(5), max_rounds=6)
        self.assertTrue(result.converged)

    def test_default_limit_is_64_rounds(self):
        result = run(self.chain(70))
        self.assertFalse(result.converged)
        self.assertEqual(result.rounds, 64)

    def test_result_is_deterministic(self):
        words = [nop(), br(1, 4), nop(), br(3, 4), nop(), ret(), brsl(6, 5)]
        self.assertEqual(run(words, extra=[2]), run(words, extra=[2]))


if __name__ == "__main__":
    unittest.main()

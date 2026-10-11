"""Tests for SPU function recovery on synthetic programs."""

import contextlib
import io
import json
import unittest

import ps3tool
from ps3recomp import spu_images, spu_recover
from ps3recomp.spu_flow import SpuCode
from ps3recomp.spu_jumptab import JumpTable, resolve_jump_tables
from ps3recomp.spu_recover import (CallSite, ConstantTarget, IndirectSite, TailEdge, recover)

from tests import spu_asm
from tests.test_spu_images import CONTAINER_VA, RAW_AT, SpuFixtureTest, container_elf, sha

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


def table_program(count: int = 4, bound: int | None = None, guard: bool = True, table_at: int = 8,
                  target_words: list[int] | None = None) -> list[int]:
    """A switch: index in $5, table of `count` code addresses right after the `bi`, then the cases."""
    bound = count - 1 if bound is None else bound
    cases = target_words if target_words is not None else [at(table_at + count + i) for i in range(count)]
    words = [
        enc("ila", RT=13, I18=at(table_at)),
        enc("shli", RT=11, RA=5, I7=2),
        enc("clgti", RT=10, RA=5, I10=bound),
        enc("lqx", RT=12, RA=13, RB=11),
        enc("a", RT=9, RA=11, RB=13),
        enc("rotqby", RT=6, RA=12, RB=9),
        enc("brnz", RT=10, addr=at(6), target=at(table_at + 2 * count)) if guard else nop(),
        enc("bi", RA=6),
    ]
    words += cases
    words += [enc("stop", STOP=0x102)] * count
    words += [ret()]
    return words


class JumpTableTest(unittest.TestCase):
    def resolve(self, words, **kwargs):
        return resolve_jump_tables(build(words), **kwargs)

    def test_scaled_index_table_with_a_bound_check(self):
        found = self.resolve(table_program())
        self.assertEqual(found.notes, {})
        table = found.tables[at(7)]
        self.assertEqual(table, JumpTable(at(7), "table", at(8), tuple(at(12 + i) for i in range(4)), 4))
        self.assertEqual(found.data_ranges(), [(at(8), at(12))])

    def test_a_conditional_return_also_bounds_the_table(self):
        words = table_program()
        words[6] = enc("binz", RT=10, RA=0)
        table = self.resolve(words).tables[at(7)]
        self.assertEqual(table.count, 4)

    def test_the_bound_is_the_clgti_limit_plus_one(self):
        words = table_program(count=6, bound=4)
        self.assertEqual(self.resolve(words).tables[at(7)].count, 5)

    def test_targets_are_sorted_without_duplicates(self):
        words = table_program(target_words=[at(14), at(12), at(14), at(13)])
        self.assertEqual(self.resolve(words).tables[at(7)].targets, (at(12), at(13), at(14)))

    def test_missing_bound_check_is_noted_and_unresolved(self):
        found = self.resolve(table_program(guard=False))
        self.assertEqual(found.tables, {})
        self.assertIn("no bound check", found.notes[at(7)])
        result = run(table_program(guard=False), table_notes=found.notes)
        self.assertEqual(result.unresolved_indirect[0].site, at(7))
        self.assertIn("no bound check", result.unresolved_indirect[0].reason)

    def test_entry_that_is_not_code_is_noted(self):
        found = self.resolve(table_program(target_words=[at(12), at(12), at(12), 0x20000]))
        self.assertEqual(found.tables, {})
        self.assertIn("table entry 3", found.notes[at(7)])

    def test_the_compare_must_test_the_scaled_index(self):
        words = table_program()
        words[2] = enc("clgti", RT=10, RA=7, I10=3)
        self.assertIn("no bound check", self.resolve(words).notes[at(7)])

    def test_load_and_rotate_must_use_one_address(self):
        words = table_program()
        words[4] = enc("a", RT=9, RA=11, RB=14)
        found = self.resolve(words)
        self.assertEqual((found.tables, found.notes), ({}, {}))

    def test_index_that_is_not_scaled_by_4(self):
        words = table_program()
        words[1] = enc("shli", RT=11, RA=5, I7=3)
        self.assertIn("scaled by 4", self.resolve(words).notes[at(7)])

    def test_plain_indirect_jump_is_left_alone(self):
        found = self.resolve([enc("bi", RA=6), ret()])
        self.assertEqual((found.tables, found.notes), ({}, {}))

    def test_table_targets_become_edges_and_the_table_is_data(self):
        words = table_program()
        found = self.resolve(words)
        result = run(words, jump_tables={s: t.targets for s, t in found.tables.items()},
                     data_ranges=found.data_ranges())
        self.assertEqual(sorted(result.functions), [at(0)])
        fn = result.functions[at(0)]
        self.assertTrue({at(12 + i) for i in range(4)} <= fn.addrs)
        self.assertFalse(fn.addrs & {at(8 + i) for i in range(4)})
        self.assertEqual(result.unresolved_indirect, ())


def duff_program(mask: int = 0x70, shift: int = 2) -> list[int]:
    """brsl $5,$+4 reads the PC; the masked offset >> shift is added to it and the sum is branched to."""
    return [
        enc("brsl", RT=5, addr=at(0), target=at(1)),
        enc("andi", RT=6, RA=3, I10=mask),
        enc("rotmi", RT=4, RA=6, I7=-shift),
        enc("ai", RT=5, RA=5, I10=36),
        enc("a", RT=5, RA=5, RB=4),
        enc("bi", RA=5),
        nop(), nop(), nop(), nop(),
        *([nop()] * 7), ret(),
    ]


class ComputedOffsetTest(unittest.TestCase):
    def test_every_value_the_mask_allows_is_a_target(self):
        found = resolve_jump_tables(build(duff_program()))
        table = found.tables[at(5)]
        self.assertEqual((table.kind, table.table, table.count), ("computed", None, 8))
        self.assertEqual(table.targets, tuple(at(10 + i) for i in range(8)))
        self.assertEqual(found.data_ranges(), [])

    def test_targets_are_edges_inside_one_function(self):
        words = duff_program()
        found = resolve_jump_tables(build(words))
        result = run(words, jump_tables={s: t.targets for s, t in found.tables.items()})
        self.assertEqual(sorted(result.functions), [at(0)])
        self.assertTrue({at(10 + i) for i in range(8)} <= result.functions[at(0)].addrs)

    def test_target_outside_the_code_is_noted(self):
        found = resolve_jump_tables(build(duff_program()[:-4]))
        self.assertEqual(found.tables, {})
        self.assertIn("not a code address", found.notes[at(5)])

    def test_offset_must_be_masked(self):
        words = duff_program()
        words[1] = nop()
        self.assertEqual(resolve_jump_tables(build(words)).tables, {})


class PositionIndependentPointerTest(unittest.TestCase):
    def test_ila_plus_delta_register_is_a_constant_call_target(self):
        words = [enc("ila", RT=4, I18=at(5)), enc("a", RT=4, RA=4, RB=126), enc("bisl", RT=0, RA=4),
                 enc("stop", STOP=0x102), nop(), ret()]
        plain = run(words)
        self.assertEqual(sorted(plain.functions), [at(0)])
        self.assertEqual(plain.unresolved_indirect[0].site, at(2))
        folded = run(words, zero_regs=[126])
        self.assertEqual(sorted(folded.functions), [at(0), at(5)])
        self.assertEqual(folded.functions[at(5)].sources, {"constant"})
        self.assertEqual(folded.unresolved_indirect, ())

    def test_pointer_that_is_only_passed_on_still_seeds_a_function(self):
        words = [enc("ila", RT=24, I18=at(4)), enc("a", RT=5, RA=24, RB=126), enc("stop", STOP=0x102), nop(),
                 ret()]
        result = run(words, zero_regs=[126])
        self.assertEqual(sorted(result.functions), [at(0), at(4)])
        self.assertEqual(result.constant_targets, (ConstantTarget(at(1), at(4), "self", "pointer"),))
        self.assertEqual(sorted(run(words).functions), [at(0)])

    def test_pointer_to_padding_is_ignored(self):
        words = [enc("ila", RT=24, I18=at(4)), enc("a", RT=5, RA=24, RB=126), enc("stop", STOP=0x102), nop(), 0]
        result = run(words, zero_regs=[126])
        self.assertEqual(sorted(result.functions), [at(0)])
        self.assertEqual(result.constant_targets, ())


def synthetic_recovery(words, entry=0, extra=(), **kwargs) -> spu_recover.ImageRecovery:
    blob = b"".join(w.to_bytes(4, "big") for w in words)
    doc = {"name": "t_raw", "container": "libsre", "ea": "0x1000", "kind": "raw", "role": "spurs_policy_module",
           "size": "0x%x" % len(blob), "ls_base": "0x%x" % BASE, "entry": "0x%x" % at(entry),
           "lowering": "stackfree", "gate": True,
           "extra_entries": [{"ls": "0x%x" % at(i), "reason": "test"} for i in extra], **kwargs}
    reg = spu_images.parse_registry({"schema": 1, "images": [doc]}).images[0]
    seg = spu_images.SpuSegment(0x1000, BASE, len(blob), len(blob), True, 0)
    image = spu_images.SpuImage(reg, blob, "ab" * 32, (seg,), BASE, BASE + len(blob), at(entry))
    return spu_recover.recover_image(image, "cd" * 32)


def elf_recovery(words, data_words, data_at=DATA) -> spu_recover.ImageRecovery:
    """An ELF-like image with one code segment and a data segment holding `data_words`."""
    blob = b"".join(w.to_bytes(4, "big") for w in words)
    tail = b"".join(w.to_bytes(4, "big") for w in data_words)
    doc = {"name": "t_elf", "container": "eboot", "ea": "0x1000", "kind": "elf", "role": "spurs_task",
           "lowering": "stackfree", "gate": False, "extra_entries": []}
    reg = spu_images.parse_registry({"schema": 1, "images": [doc]}).images[0]
    segments = (spu_images.SpuSegment(0x1000, BASE, len(blob), len(blob), True, 0),
                spu_images.SpuSegment(0x1000 + len(blob), data_at, len(tail), len(tail) + 0x40, False, len(blob)))
    image = spu_images.SpuImage(reg, blob + tail, "ab" * 32, segments, BASE, data_at + len(tail) + 0x40, at(0))
    return spu_recover.recover_image(image, "cd" * 32)


class InitListTest(unittest.TestCase):
    # entry, a constructor, a second constructor, a destructor
    PROGRAM = [enc("stop", STOP=0x102), nop(), ret(), nop(), ret(), nop(), ret()]
    END = 0xFFFFFFFF

    def seeds(self, data_words, data_at=DATA):
        rec = elf_recovery(self.PROGRAM, data_words, data_at)
        return {a: s for a, s in spu_recover.dispatchable_blocks(rec).items()}, rec

    def test_list_pointers_are_seeded_with_their_list(self):
        blocks, rec = self.seeds([self.END, at(2), at(4), 0, self.END, at(6), 0])
        self.assertEqual(blocks[at(2)], ("ctors",))
        self.assertEqual(blocks[at(4)], ("ctors",))
        self.assertEqual(blocks[at(6)], ("dtors",))
        self.assertEqual(sorted(rec.result.functions), [at(0), at(2), at(4), at(6)])
        self.assertEqual(rec.result.rejected_seeds, ())

    def test_empty_lists_add_nothing(self):
        blocks, rec = self.seeds([self.END, 0, self.END, 0, 0, 0])
        self.assertEqual(blocks, {at(0): ("entry",)})
        self.assertEqual(spu_recover.gcc_init_lists(rec.image), ((), ()))

    def test_list_must_start_the_first_data_segment(self):
        blocks, _ = self.seeds([0, self.END, at(2), 0, self.END, 0])
        self.assertEqual(blocks, {at(0): ("entry",)})

    def test_unaligned_or_non_code_pointers_drop_the_list(self):
        for bad in (at(2) + 2, DATA, 0x20000, at(0) - 4, 0xFFFFFFFF):
            with self.subTest(pointer=hex(bad)):
                blocks, _ = self.seeds([self.END, at(2), bad, 0, self.END, at(6), 0])
                self.assertEqual(blocks, {at(0): ("entry",)})

    def test_bad_destructors_keep_the_constructors(self):
        blocks, _ = self.seeds([self.END, at(2), 0, self.END, 0x20000, 0])
        self.assertEqual(blocks, {at(0): ("entry",), at(2): ("ctors",)})

    def test_unterminated_list_is_ignored(self):
        blocks, _ = self.seeds([self.END, at(2), at(4)])
        self.assertEqual(blocks, {at(0): ("entry",)})

    def test_raw_images_have_no_lists(self):
        rec = synthetic_recovery(self.PROGRAM)
        self.assertEqual(spu_recover.gcc_init_lists(rec.image), ((), ()))


class DatabaseTest(unittest.TestCase):
    PROGRAM = [brsl(0, 3), nop(), enc("stop", STOP=0x102), ret(), nop()]

    def test_dispatchable_set_has_entries_return_sites_and_extra_entries(self):
        rec = synthetic_recovery(self.PROGRAM, extra=[4])
        self.assertEqual(spu_recover.dispatchable_blocks(rec),
                         {at(0): ("entry",), at(1): ("return",), at(3): ("brsl",), at(4): ("extra",)})

    def test_return_site_of_a_call_into_padding_is_not_dispatchable(self):
        rec = synthetic_recovery([brsl(0, 3), 0, enc("stop", STOP=0x102), enc("stop", STOP=0x102)])
        self.assertNotIn(at(1), spu_recover.dispatchable_blocks(rec))

    def test_jump_table_targets_and_constants_are_dispatchable(self):
        blocks = spu_recover.dispatchable_blocks(synthetic_recovery(table_program()))
        for i in range(4):
            self.assertEqual(blocks[at(12 + i)], ("jump_table",))
        rec = synthetic_recovery([enc("ila", RT=4, I18=at(4)), enc("bisl", RT=0, RA=4), enc("stop", STOP=0x102),
                                  nop(), ret()])
        self.assertEqual(spu_recover.dispatchable_blocks(rec)[at(4)], ("constant",))

    def test_registry_data_ranges_and_pic_registers_reach_the_recovery(self):
        words = [enc("br", addr=at(0), target=at(2)), nop(), ret()]
        rec = synthetic_recovery(words, data_ranges=[{"start": "0x%x" % at(2), "end": "0x%x" % at(3),
                                                      "reason": "x"}])
        self.assertEqual(rec.result.invalid_flow, ((at(0), at(0), at(2)),))
        words = [enc("ila", RT=4, I18=at(5)), enc("a", RT=4, RA=4, RB=126), enc("bisl", RT=0, RA=4),
                 enc("stop", STOP=0x102), nop(), ret()]
        rec = synthetic_recovery(words, pic_delta_regs=[126])
        self.assertIn(at(5), rec.result.functions)

    def test_database_content(self):
        words = table_program()
        doc = spu_recover.build_database(synthetic_recovery(words))
        self.assertEqual(doc["image"], "t_raw")
        self.assertEqual(doc["entry"], "0x1000")
        self.assertEqual(doc["input"]["container_sha256"], "cd" * 32)
        self.assertEqual(doc["input"]["image_sha256"], "ab" * 32)
        self.assertEqual(len(doc["input"]["registry_entry_sha256"]), 64)
        self.assertEqual(doc["segments"], [{"ls": "0x1000", "size": "0x%x" % (4 * len(words)), "code": True}])
        self.assertEqual(doc["jump_tables"][0]["table"], "0x1020")
        self.assertEqual(doc["jump_tables"][0]["count"], 4)
        self.assertEqual(doc["unresolved_indirect"], [])
        (fn,) = doc["functions"]
        self.assertEqual(fn["ranges"], [["0x1000", "0x1020"], ["0x1030", "0x1044"]])
        self.assertEqual(fn["indirect"][0]["resolution"], "jump_table")
        self.assertEqual(fn["indirect"][0]["targets"], doc["jump_tables"][0]["targets"])

    def test_calls_and_unresolved_sites_are_recorded(self):
        rec = synthetic_recovery([brsl(0, 3, link=2), enc("bi", RA=7), enc("stop", STOP=0x102), ret()])
        doc = spu_recover.build_database(rec)
        caller = doc["functions"][0]
        self.assertEqual(caller["call_sites"], [{"site": "0x1000", "kind": "brsl", "target": "0x100c", "link": 2}])
        unresolved = {"site": "0x1004", "kind": "bi", "reason": "$7 is not a recoverable constant"}
        self.assertEqual(caller["indirect"], [{**unresolved, "resolution": "unresolved"}])
        self.assertEqual(doc["unresolved_indirect"], [unresolved])
        self.assertEqual(doc["functions"][1]["returns"], ["0x100c"])

    def test_text_is_deterministic_ascii_with_lf_only(self):
        first = spu_recover.database_text(synthetic_recovery(table_program()))
        second = spu_recover.database_text(synthetic_recovery(table_program()))
        self.assertEqual(first, second)
        self.assertTrue(first.endswith(b"}\n"))
        self.assertNotIn(b"\r", first)
        first.decode("ascii")
        self.assertEqual(json.loads(first), spu_recover.build_database(synthetic_recovery(table_program())))

    def test_format_json_wraps_long_lists_and_keeps_short_ones_flat(self):
        self.assertEqual(spu_recover.format_json({"a": [1, 2]}), '{"a": [1, 2]}')
        text = spu_recover.format_json({"a": ["0x%x" % i for i in range(60)], "b": {"c": 1}})
        self.assertGreater(text.count("\n"), 3)
        self.assertTrue(all(len(line) <= spu_recover.JSON_WIDTH for line in text.split("\n")))
        self.assertEqual(json.loads(text)["a"][59], "0x3b")

    def test_report_counts(self):
        report = spu_recover.build_report(synthetic_recovery(self.PROGRAM + [0, nop()]))
        self.assertEqual(report["functions"], 2)
        self.assertEqual(report["code_words"], {"owned": 4, "unowned": 2, "zero": 1, "data": 0})
        self.assertEqual(report["unowned_ranges"], [["0x1010", "0x1014"], ["0x1018", "0x101c"]])
        self.assertEqual(report["dispatchable"], ["0x1000", "0x1004", "0x100c"])
        self.assertIn("functions 2", spu_recover.summary_line(report))


class CommandTest(SpuFixtureTest):
    """`spu-recover` on a synthetic libsre that holds one jump-table program."""

    def setUp(self):
        super().setUp()
        self.words = table_program()
        blob = b"".join(w.to_bytes(4, "big") for w in self.words)
        self.libsre = container_elf({RAW_AT: blob})
        self.libsre_path = self.write("libsre.reloc.elf", self.libsre)
        self.config_path = self.write_json("target.json", {"eboot_elf": {"sha256": sha(self.eboot)},
                                                           "libsre_reloc_elf": {"sha256": sha(self.libsre)}})
        entry = {"name": "t_table", "container": "libsre", "ea": "0x%x" % (CONTAINER_VA + RAW_AT),
                 "kind": "raw", "size": "0x%x" % len(blob), "ls_base": "0x%x" % BASE, "entry": "0x%x" % BASE,
                 "role": "spurs_policy_module", "lowering": "stackfree", "gate": True, "extra_entries": []}
        pic = dict(entry, name="t_pic", ls_base=None, role="exit_handler")
        del pic["entry"]
        self.registry = self.write_json("registry.json", {"schema": 1, "images": [entry, pic]})
        self.out = self.tmp / "db"
        self.report = self.tmp / "analysis" / "report.json"

    def recover_cli(self, *extra):
        argv = ["spu-recover", "--eboot", str(self.eboot_path), "--libsre", str(self.libsre_path),
                "--registry", str(self.registry), "--target-config", str(self.config_path),
                "--out", str(self.out), "--report", str(self.report), *extra]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ps3tool.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_writes_one_database_per_located_image_and_a_report(self):
        code, out, err = self.recover_cli()
        self.assertEqual((code, err), (0, ""))
        self.assertEqual([p.name for p in self.out.iterdir()], ["t_table.json"])
        self.assertIn("t_table functions 1 ", out)
        self.assertIn("1 databases, 1 written", out)
        db = json.loads((self.out / "t_table.json").read_text())
        self.assertEqual(db["input"]["container_sha256"], sha(self.libsre))
        self.assertEqual(db["jump_tables"][0]["count"], 4)
        report = json.loads(self.report.read_text())
        self.assertEqual([i["image"] for i in report["images"]], ["t_table"])

    def test_second_run_rewrites_nothing(self):
        self.recover_cli()
        before = {p: p.stat().st_mtime_ns for p in [*self.out.iterdir(), self.report]}
        code, out, _ = self.recover_cli()
        self.assertEqual(code, 0)
        self.assertIn("1 databases, 0 written", out)
        self.assertEqual(before, {p: p.stat().st_mtime_ns for p in [*self.out.iterdir(), self.report]})

    def test_container_that_is_not_pinned_exits_4(self):
        self.write_json("target.json", {"eboot_elf": {"sha256": sha(self.eboot)},
                                        "libsre_reloc_elf": {"sha256": "0" * 64}})
        code, _, err = self.recover_cli()
        self.assertEqual(code, 4)
        self.assertIn("does not match", err)
        self.assertFalse(self.out.exists())

    def test_naming_a_position_independent_image_is_a_usage_error(self):
        code, _, err = self.recover_cli("--images", "t_pic")
        self.assertEqual(code, 2)
        self.assertIn("position independent", err)


if __name__ == "__main__":
    unittest.main()

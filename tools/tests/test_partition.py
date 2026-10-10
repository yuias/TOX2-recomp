"""Tests for the seed -> partition fixpoint."""

import itertools
import unittest

from ps3recomp.jumptab import JumpTable
from ps3recomp.partition import (PartitionResult, RecoveredFunction, TailEdge, intra_successors,
                                 partition)
from ps3recomp.ppu_flow import CodeImage
from ps3recomp.seeds import ImportThunk, Seed

from tests import ppc_asm as asm

BASE = 0x10000
THUNK = 0x20000
OUTSIDE = 0x30000


def at(index: int) -> int:
    return BASE + 4 * index


def build(words) -> CodeImage:
    return CodeImage.from_words(BASE, words)


def run(words, starts, tables=None, thunks=None, **kwargs) -> PartitionResult:
    seeds = [Seed(at(i), "ghidra") for i in starts]
    return partition(build(words), seeds, tables or {}, thunks or {}, **kwargs)


def b_to(index: int, target: int, lk: bool = False) -> int:
    return asm.b(at(index), target, lk=lk)


def pad(count: int):
    return [asm.blr()] * count


class BasicsTest(unittest.TestCase):
    def test_single_function_with_loop(self):
        words = [asm.nop(), asm.nop(), asm.ble(0, at(2), at(0)), asm.blr()]
        result = run(words, [0])
        self.assertTrue(result.converged)
        self.assertEqual(result.rounds, 1)
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(i) for i in range(4)})
        self.assertEqual(fn.tail_edges, ())
        self.assertEqual(fn.sources, {"ghidra"})
        self.assertEqual(result.promotions, ())

    def test_call_discovery_creates_call_function(self):
        words = [b_to(0, at(4), lk=True), asm.blr(), asm.blr(), asm.blr(), asm.nop(), asm.blr()]
        result = run(words, [0])
        self.assertEqual(sorted(result.functions), [at(0), at(4)])
        self.assertEqual(result.functions[at(4)].sources, {"call"})
        self.assertEqual(result.functions[at(4)].addrs, {at(4), at(5)})
        self.assertEqual(result.functions[at(0)].calls, {at(4)})
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})
        self.assertEqual(result.rounds, 2)

    def test_call_to_known_start_keeps_sources(self):
        words = [b_to(0, at(2), lk=True), asm.blr(), asm.blr()]
        result = run(words, [0, 2])
        self.assertEqual(result.functions[at(2)].sources, {"ghidra"})
        self.assertEqual(result.rounds, 1)

    def test_tail_call_to_another_start(self):
        words = [asm.nop(), b_to(1, at(3)), asm.blr(), asm.blr()]
        result = run(words, [0, 3])
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1)})
        self.assertEqual(fn.tail_edges, (TailEdge(at(1), at(3), "branch"),))

    def test_fall_through_into_a_start(self):
        words = [asm.nop(), asm.nop(), asm.blr()]
        result = run(words, [0, 2])
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1)})
        self.assertEqual(fn.tail_edges, (TailEdge(at(1), at(2), "fallthrough"),))

    def test_seeds_on_invalid_or_outside_words_are_dropped(self):
        words = [asm.blr(), 0, asm.blr()]
        seeds = [Seed(at(0), "ghidra"), Seed(at(1), "bl"), Seed(OUTSIDE, "bl"),
                 Seed(at(0) + 2, "bl")]
        result = partition(build(words), seeds, {}, {})
        self.assertEqual(list(result.functions), [at(0)])

    def test_seed_sources_are_merged(self):
        words = [asm.blr()]
        seeds = [Seed(at(0), "opd"), Seed(at(0), "bl"), Seed(at(0), "opd")]
        result = partition(build(words), seeds, {}, {})
        self.assertEqual(result.functions[at(0)].sources, {"opd", "bl"})

    def test_intra_successors(self):
        words = [asm.ble(0, at(0), at(3)), asm.b(at(1), at(0), lk=True), asm.blr(),
                 asm.nop()]
        image = build(words)
        self.assertEqual(intra_successors(image, at(0), {}), [at(1), at(3)])
        self.assertEqual(intra_successors(image, at(1), {}), [at(2)])
        self.assertEqual(intra_successors(image, at(2), {}), [])


class SharedCodeTest(unittest.TestCase):
    # f0: nop; b T       f1: nop; b T       T: nop; nop; blr
    SHARED = [asm.nop(), 0, asm.blr(), asm.nop(), 0, asm.blr(), asm.nop(), asm.nop(), asm.blr()]

    def shared_words(self):
        words = list(self.SHARED)
        words[1] = b_to(1, at(6))
        words[4] = b_to(4, at(6))
        return words

    def assert_exclusive(self, result):
        owned = [a for fn in result.functions.values() for a in fn.addrs]
        self.assertEqual(len(owned), len(set(owned)))

    def test_shared_tail_promoted_once_at_its_entry(self):
        result = run(self.shared_words(), [0, 3])
        self.assertTrue(result.converged)
        self.assertEqual(sorted(result.functions), [at(0), at(3), at(6)])
        tail = result.functions[at(6)]
        self.assertEqual(tail.addrs, {at(6), at(7), at(8)})
        self.assertEqual(tail.sources, {"promoted"})
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})
        self.assertEqual(result.functions[at(3)].addrs, {at(3), at(4)})
        self.assertEqual(len(result.promotions), 1)
        promotion = result.promotions[0]
        self.assertEqual((promotion.addr, promotion.round), (at(6), 1))
        self.assertEqual(promotion.reached_from, (at(0), at(3)))
        self.assertEqual(promotion.edge_sites, (at(1), at(4)))
        self.assertEqual(result.functions[at(0)].tail_edges,
                         (TailEdge(at(1), at(6), "branch"),))
        self.assert_exclusive(result)

    def test_nested_sharing_converges(self):
        # s0: b P   s1: b P   s2: b Q   P: nop; b Q   Q: nop; blr
        words = [b_to(0, at(6)), asm.blr(), b_to(2, at(6)), asm.blr(),
                 b_to(4, at(9)), asm.blr(),
                 asm.nop(), b_to(7, at(9)), asm.blr(), asm.nop(), asm.blr()]
        result = run(words, [0, 2, 4])
        self.assertTrue(result.converged)
        self.assertEqual(sorted(result.functions), [at(0), at(2), at(4), at(6), at(9)])
        self.assertEqual(result.functions[at(6)].addrs, {at(6), at(7)})
        self.assertEqual(result.functions[at(9)].addrs, {at(9), at(10)})
        self.assertEqual({p.addr for p in result.promotions}, {at(6), at(9)})
        self.assert_exclusive(result)

    def test_shared_loop_entered_from_two_starts(self):
        # Both starts jump into a loop body; only the loop entry is promoted.
        words = [b_to(0, at(4)), asm.blr(), b_to(2, at(4)), asm.blr(),
                 asm.nop(), asm.nop(), asm.ble(0, at(6), at(4)), asm.blr()]
        result = run(words, [0, 2])
        self.assertEqual(sorted(result.functions), [at(0), at(2), at(4)])
        self.assertEqual(result.functions[at(4)].addrs, {at(i) for i in range(4, 8)})

    def test_seed_order_does_not_change_the_result(self):
        words = self.shared_words()
        seeds = [Seed(at(0), "ghidra"), Seed(at(3), "bl"), Seed(at(3), "opd"),
                 Seed(at(6), "data_ptr")]
        results = [partition(build(words), list(order), {}, {})
                   for order in itertools.permutations(seeds)]
        self.assertTrue(all(r == results[0] for r in results))
        self.assertEqual(results[0].functions[at(3)].sources, {"bl", "opd"})
        self.assertEqual(results[0].promotions, ())


    def test_seed_order_does_not_change_a_promoting_result(self):
        words = self.shared_words()
        seeds = [Seed(at(0), "ghidra"), Seed(at(3), "bl"), Seed(at(3), "opd")]
        results = [partition(build(words), list(order), {}, {})
                   for order in itertools.permutations(seeds)]
        self.assertTrue(all(r == results[0] for r in results))
        self.assertEqual(len(results[0].promotions), 1)
        self.assertEqual(results[0].promotions[0].addr, at(6))
        self.assertEqual(sorted(results[0].functions), [at(0), at(3), at(6)])
        self.assertEqual(results[0].functions[at(3)].sources, {"bl", "opd"})
        self.assertEqual(results[0].functions[at(6)].sources, {"promoted"})


class RoundLimitTest(unittest.TestCase):
    WORDS = [b_to(0, at(3), lk=True), asm.blr(), asm.blr(), asm.blr()]

    def test_max_rounds_one_is_not_converged(self):
        result = run(self.WORDS, [0], max_rounds=1)
        self.assertFalse(result.converged)
        self.assertEqual(result.rounds, 1)
        self.assertEqual(list(result.functions), [at(0)])

    def test_enough_rounds_converge(self):
        result = run(self.WORDS, [0], max_rounds=2)
        self.assertTrue(result.converged)
        self.assertEqual(result.rounds, 2)
        self.assertEqual(sorted(result.functions), [at(0), at(3)])


class AnomalyTest(unittest.TestCase):
    def test_flow_into_zero_word_is_invalid_flow(self):
        result = run([asm.nop(), asm.nop(), 0], [0])
        self.assertEqual(result.invalid_flow, ((at(0), at(1), at(2)),))
        self.assertEqual(result.functions[at(0)].addrs, {at(0), at(1)})

    def test_call_to_zero_word_is_rejected(self):
        result = run([b_to(0, at(3), lk=True), asm.blr(), asm.blr(), 0], [0])
        self.assertEqual(result.rejected_calls, ((at(0), at(3)),))
        self.assertEqual(list(result.functions), [at(0)])
        self.assertEqual(result.functions[at(0)].calls, frozenset())

    def test_fall_through_off_the_image_is_external(self):
        result = run([asm.nop()], [0])
        self.assertEqual(result.functions[at(0)].external_targets, {at(1)})


class ImportAndExternalTest(unittest.TestCase):
    THUNKS = {THUNK: ImportThunk(THUNK, "cellSysmodule", 0x1234, None, 0x40000)}

    def test_import_tail_edge_and_call(self):
        words = [asm.b(BASE, THUNK), asm.b(BASE + 4, THUNK, lk=True), asm.blr()]
        result = run(words, [0, 1], thunks=self.THUNKS)
        jumper = result.functions[at(0)]
        self.assertEqual(jumper.tail_edges, (TailEdge(at(0), THUNK, "import"),))
        self.assertEqual(jumper.external_targets, frozenset())
        caller = result.functions[at(1)]
        self.assertEqual(caller.import_calls, {THUNK})
        self.assertEqual(caller.calls, frozenset())
        self.assertEqual(caller.addrs, {at(1), at(2)})
        self.assertEqual(sorted(result.functions), [at(0), at(1)])

    def test_external_targets_are_recorded_not_followed(self):
        words = [asm.b(BASE, OUTSIDE, lk=True), asm.b(BASE + 4, OUTSIDE + 8)]
        result = run(words, [0])
        fn = result.functions[at(0)]
        self.assertEqual(fn.external_targets, {OUTSIDE, OUTSIDE + 8})
        self.assertEqual(fn.tail_edges, ())
        self.assertEqual(list(result.functions), [at(0)])


class JumpTableTest(unittest.TestCase):
    # 0: nop  1: bctr  2..4: table words  5, 6: case blocks  7: pad  8: b into the table
    def words(self):
        return [asm.nop(), asm.bctr(), 0, 0, 0, asm.nop(), asm.blr(), asm.blr(),
                asm.b(at(8), at(3))]

    def table(self, targets):
        return {at(1): JumpTable(at(1), at(2), "abs32", 3, tuple(targets), True)}

    def test_targets_are_followed_and_table_is_data(self):
        tables = self.table([at(5), at(6), at(5)])
        result = run(self.words(), [0], tables)
        self.assertEqual(list(result.functions), [at(0)])
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1), at(5), at(6)})
        self.assertEqual(fn.data_addrs, {at(2), at(3), at(4)})
        self.assertEqual(fn.indirect_sites, (at(1),))
        self.assertEqual(fn.ranges(), [(at(0), at(2)), (at(5), at(7))])
        self.assertEqual(fn.data_ranges(), [(at(2), at(5))])
        self.assertEqual(intra_successors(build(self.words()), at(1), tables), [at(5), at(6)])

    def test_flow_into_table_words_is_invalid_and_not_walked(self):
        tables = self.table([at(5), at(6), at(5)])
        result = run(self.words(), [0, 8], tables)
        self.assertEqual(result.invalid_flow, ((at(8), at(8), at(3)),))
        self.assertEqual(result.functions[at(8)].addrs, {at(8)})

    def test_table_words_are_not_seeds_or_call_targets(self):
        tables = self.table([at(5), at(6), at(5)])
        words = self.words()
        words[8] = asm.b(at(8), at(3), lk=True)
        result = run(words, [0, 3, 8], tables)
        self.assertNotIn(at(3), result.functions)
        self.assertEqual(result.rejected_calls, ((at(8), at(3)),))

    def test_table_target_that_is_a_start_becomes_a_tail_edge(self):
        tables = self.table([at(5), at(6), at(5)])
        result = run(self.words(), [0, 6], tables)
        fn = result.functions[at(0)]
        self.assertEqual(fn.addrs, {at(0), at(1), at(5)})
        self.assertIn(TailEdge(at(1), at(6), "branch"), fn.tail_edges)

    def test_table_outside_the_image_has_no_data_words(self):
        table = JumpTable(at(1), 0x50000, "abs32", 2, (at(5), at(6)), False)
        result = run(self.words(), [0], {at(1): table})
        fn = result.functions[at(0)]
        self.assertEqual(fn.data_addrs, frozenset())
        self.assertEqual(fn.addrs, {at(0), at(1), at(5), at(6)})


class RangesTest(unittest.TestCase):
    def test_ranges_merge_adjacent_words(self):
        fn = RecoveredFunction(at(0), frozenset({"x"}),
                               frozenset({at(0), at(1), at(2), at(5), at(6), at(9)}),
                               frozenset(), frozenset(), (), (), frozenset())
        self.assertEqual(fn.ranges(), [(at(0), at(3)), (at(5), at(7)), (at(9), at(10))])

    def test_empty_ranges(self):
        fn = RecoveredFunction(at(0), frozenset(), frozenset(), frozenset(), frozenset(),
                               (), (), frozenset())
        self.assertEqual(fn.ranges(), [])


if __name__ == "__main__":
    unittest.main()

"""Tests for switch-table recovery behind `bctr`."""

import struct
import unittest

from ps3recomp import jumptab
from ps3recomp.elf import ElfFile
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm
from tests.elf_fixture import FixtureSegment, build_elf64_be

BASE = 0x10000
TOC = 0x00109F80
TOC_SLOT = -8
WORDS = 0x80
# Case targets sit in a nop area at the end of the image; DEFAULT is the out-of-range exit.
CASES = BASE + 4 * 0x40
DEFAULT = BASE + 4 * 0x70
OUTSIDE_TABLE = 0x00201234     # lis 0x20 / addi 0x1234


def pc(code):
    return BASE + 4 * len(code)


def targets_for(count):
    return [CASES + 4 * (i % 5) for i in range(count)]


class Fixture:
    """Image plus a dict-backed memory holding the TOC slot and any table outside the image."""

    def __init__(self, code, tail=(), extra=None):
        words = list(code) + list(tail)
        words += [asm.nop()] * (WORDS - len(words))
        assert len(words) == WORDS, "test code does not fit the image"
        self.image = CodeImage.from_words(BASE, words)
        self.mem = {BASE + 4 * i: w for i, w in enumerate(words)}
        self.mem.update(extra or {})
        self.branch = BASE + 4 * max(i for i, w in enumerate(code) if w == asm.bctr())

    def read_mem(self, ea, size):
        out = b""
        for addr in range(ea, ea + size, 4):
            if addr not in self.mem:
                return None
            out += struct.pack(">I", self.mem[addr])
        return out

    def recover(self):
        return jumptab.recover_jump_tables(self.image, self.read_mem, TOC)

    def reason(self):
        tables, unresolved = self.recover()
        assert not tables, "unexpectedly resolved: %r" % tables
        return unresolved[self.branch].reason


def rel_fixture(code, count, offsets=None):
    """Code ending in `bctr` followed by `count` relative entries and a TOC slot for the table."""
    table = pc(code)
    targets = targets_for(count)
    entries = [(t - table) & 0xFFFFFFFF for t in targets] if offsets is None else offsets
    return Fixture(code, entries, {TOC + TOC_SLOT: table}), table, targets


def spine(index_op, *, load=asm.lwzx(0, 9, 11)):
    """The compiler's table dispatch: TOC slot -> scaled index -> load -> extsw -> add -> bctr."""
    return [asm.lwz(11, TOC_SLOT, 2), index_op, load, asm.extsw(0, 0), asm.add(0, 0, 11),
            asm.mtctr(0), asm.bctr()]


class RelativeShapeTest(unittest.TestCase):
    def check(self, code, count, **kwargs):
        fx, table, targets = rel_fixture(code, count, **kwargs)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.kind, jt.table, jt.count), ("rel32", table, count))
        self.assertEqual(list(jt.targets), targets)
        self.assertTrue(jt.table_in_text)
        return jt

    def test_bgt_bound_slwi_index(self):
        code = [asm.cmplwi(7, 9, 4), asm.bgt(7, BASE + 4, DEFAULT)]
        code += spine(asm.slwi(9, 9, 2))
        self.check(code, 5)

    def test_bge_bound_counts_k(self):
        code = [asm.cmplwi(7, 9, 4), asm.bge(7, BASE + 4, DEFAULT)]
        code += spine(asm.slwi(9, 9, 2))
        self.check(code, 4)

    def test_negative_entries_are_sign_extended(self):
        code = [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)]
        code += spine(asm.slwi(9, 9, 2))
        table = pc(code)
        # One target before the table (negative offset), one after it.
        offsets = [(BASE - table) & 0xFFFFFFFF, (CASES - table) & 0xFFFFFFFF]
        fx, _, _ = rel_fixture(code, 2, offsets=offsets)
        jt = fx.recover()[0][fx.branch]
        self.assertEqual(list(jt.targets), [BASE, CASES])

    def test_sldi_index(self):
        code = [asm.cmplwi(7, 9, 2), asm.bgt(7, BASE + 4, DEFAULT)]
        code += spine(asm.sldi(9, 9, 2))
        self.check(code, 3)

    def test_rldic_index_far_compare(self):
        # Compare well before the branch with unrelated code between (real shape: bound at
        # the top of a function, table dispatch further down).
        code = [asm.addi(9, 9, -0x415), asm.cmplwi(7, 9, 4)]
        code += [asm.mr(26, 5), asm.mr(29, 6), asm.mr(28, 7)] * 3
        code += [asm.bgt(7, pc(code), DEFAULT)]
        code += spine(asm.rldic(9, 9, 2, 30))
        self.check(code, 5)

    def test_clrlwi_alias_and_rlwinm_mask(self):
        code = [asm.addi(9, 9, -0x25), asm.clrlwi(0, 9, 24), asm.cmplwi(7, 0, 4)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        code += spine(asm.rlwinm(9, 9, 2, 22, 29))
        self.check(code, 5)

    def test_compare_on_copy_defined_before_index_source(self):
        # The compared register is a clrlwi copy of the register the index is computed from,
        # and the index source is a different register defined earlier.
        code = [asm.addi(3, 9, -0x62), asm.clrlwi(0, 3, 24), asm.cmplwi(7, 0, 4)]
        code += [asm.bgt(7, pc(code), DEFAULT), asm.rlwinm(9, 3, 2, 22, 29),
                 asm.lwz(11, TOC_SLOT, 2), asm.lwzx(0, 9, 11), asm.extsw(0, 0),
                 asm.add(0, 0, 11), asm.mtctr(0), asm.bctr()]
        self.check(code, 5)

    def test_live_in_index_source(self):
        code = [asm.clrlwi(9, 7, 24), asm.cmplwi(7, 9, 4)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        code += spine(asm.rlwinm(9, 7, 2, 22, 29))
        self.check(code, 5)

    def test_clrldi_and_mr_aliases(self):
        code = [asm.mr(5, 3), asm.clrldi(6, 5, 32), asm.cmplwi(7, 6, 3)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        code += spine(asm.slwi(9, 5, 2))
        self.check(code, 4)

    def test_cmplw_with_constant_register(self):
        code = [asm.li(6, 3), asm.cmplw(7, 9, 6), asm.bgt(7, BASE + 8, DEFAULT)]
        code += spine(asm.slwi(9, 9, 2))
        self.check(code, 4)

    def test_mr_between_load_and_add(self):
        code = [asm.cmplwi(7, 9, 4), asm.bgt(7, BASE + 4, DEFAULT),
                asm.lwz(11, TOC_SLOT, 2), asm.slwi(9, 9, 2), asm.lwzx(0, 9, 11),
                asm.mr(10, 0), asm.add(0, 10, 11), asm.mtctr(0), asm.bctr()]
        self.check(code, 5)

    def test_lwax_entries(self):
        code = [asm.cmplwi(7, 9, 4), asm.bgt(7, BASE + 4, DEFAULT)]
        code += spine(asm.slwi(9, 9, 2), load=asm.lwax(0, 9, 11))
        self.check(code, 5)

    def far_compare_code(self, distance):
        # A prologue that saves registers sits between the bound and the dispatch; the compare is
        # `distance` words before the `bctr`.
        code = [asm.cmplwi(7, 9, 4)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        spine_code = spine(asm.slwi(9, 9, 2))
        code += [asm.stw(26, 0x20, 1)] * (distance - len(spine_code) - len(code) + 1)
        code += spine_code
        self.assertEqual(len(code) - 1, distance)
        return code

    def test_compare_beyond_the_definition_window(self):
        # 34 words: the real shape of a function that stores its saved registers after the bound.
        self.check(self.far_compare_code(34), 5)

    def test_compare_past_the_walk_limit_is_not_a_bound(self):
        fx, _, _ = rel_fixture(self.far_compare_code(jumptab._BOUND_WALK_LIMIT + 1), 5)
        self.assertEqual(fx.reason(), "no_bound")

    def test_bgt_inside_table_path_is_not_a_bound(self):
        code = [asm.cmplwi(7, 9, 4)]
        code += [asm.bgt(7, pc(code), pc(code) + 8)]
        code += spine(asm.slwi(9, 9, 2))
        fx, _, _ = rel_fixture(code, 5)
        self.assertEqual(fx.reason(), "no_bound")


class EntryBranchShapeTest(unittest.TestCase):
    # `ble crN,L` enters the table block; the default path is the fall-through.
    def check(self, code, count):
        fx, table, targets = rel_fixture(code, count)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.count, list(jt.targets)), (count, targets))

    def test_block_start_after_return(self):
        code = [asm.clrlwi(0, 9, 24), asm.cmplwi(7, 0, 4), 0, asm.li(3, -1), asm.blr()]
        code[2] = asm.ble(7, BASE + 8, pc(code))
        code += spine(asm.rlwinm(9, 9, 2, 22, 29))
        self.check(code, 5)

    def test_block_start_after_unconditional_branch(self):
        code = [asm.cmplwi(7, 9, 4), 0, asm.li(3, -1), asm.b(BASE + 12, DEFAULT)]
        code[1] = asm.ble(7, BASE + 4, pc(code))
        code += spine(asm.slwi(9, 9, 2))
        self.check(code, 5)

    def test_ble_over_error_call(self):
        # cmplwi; ble L; bl <error>; nop; L: table block (bl hides the compare from a walk).
        code = [asm.clrlwi(0, 30, 28), asm.cmplwi(7, 0, 12), 0]
        code += [asm.b(pc(code), DEFAULT, lk=True), asm.nop()]
        code[2] = asm.ble(7, BASE + 8, pc(code))
        # The mask 26..29 would allow 16 entries; the bound says 13.
        code += spine(asm.rlwinm(9, 0, 2, 26, 29))
        self.check(code, 13)

    def far_entry_code(self, distance):
        # cmplwi; <filler>; ble L; blr; L: table block. The compare is `distance` words before the ble.
        code = [asm.cmplwi(7, 9, 4)] + [asm.mr(26, 5)] * (distance - 1) + [0, asm.blr()]
        code[distance] = asm.ble(7, BASE + 4 * distance, pc(code))
        return code + spine(asm.slwi(9, 9, 2))

    def test_compare_beyond_the_old_entry_window(self):
        self.check(self.far_entry_code(17), 5)

    def test_compare_at_the_entry_limit(self):
        self.check(self.far_entry_code(jumptab._ENTRY_COMPARE_LIMIT), 5)

    def test_compare_past_the_entry_limit_is_not_a_bound(self):
        fx, _, _ = rel_fixture(self.far_entry_code(jumptab._ENTRY_COMPARE_LIMIT + 1), 5)
        self.assertEqual(fx.reason(), "no_bound")

    def test_base_built_with_two_instructions(self):
        code = [asm.cmplwi(7, 9, 4), 0, asm.blr()]
        code[1] = asm.ble(7, BASE + 4, pc(code))
        table = BASE + 4 * 11
        code += [asm.lis(11, table >> 16), asm.addi(11, 11, table & 0xFFFF), asm.slwi(9, 9, 2),
                 asm.lwzx(0, 9, 11), asm.extsw(0, 0), asm.add(0, 0, 11), asm.mtctr(0),
                 asm.bctr()]
        self.assertEqual(pc(code), table)
        entries = [(t - table) & 0xFFFFFFFF for t in targets_for(5)]
        fx = Fixture(code, entries)
        jt = fx.recover()[0][fx.branch]
        self.assertEqual((jt.table, jt.count), (table, 5))

    def test_branch_to_the_middle_of_the_block_is_not_a_bound(self):
        code = [asm.cmplwi(7, 9, 4), 0, asm.blr()]
        # Target is the index op, three words after the spine start.
        spine_code = spine(asm.slwi(9, 9, 2))
        code[1] = asm.ble(7, BASE + 4, pc(code) + 4 * 4)
        code += spine_code
        fx, _, _ = rel_fixture(code, 5)
        self.assertEqual(fx.reason(), "no_bound")

    def test_register_redefined_between_compare_and_ble_is_rejected(self):
        code = [asm.cmplwi(7, 9, 4), asm.li(9, 0), 0, asm.blr()]
        code[2] = asm.ble(7, BASE + 8, pc(code))
        code += spine(asm.slwi(9, 9, 2))
        fx, _, _ = rel_fixture(code, 5)
        self.assertEqual(fx.reason(), "no_bound")

    def test_conflicting_entry_bounds(self):
        code = [asm.cmplwi(7, 9, 4), 0, asm.cmplwi(6, 9, 6), 0, asm.blr()]
        end = pc(code)
        code[1] = asm.ble(7, BASE + 4, end)
        code[3] = asm.ble(6, BASE + 12, end)
        code += spine(asm.slwi(9, 9, 2))
        fx, _, _ = rel_fixture(code, 5)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved[fx.branch].reason, "bad_bound")


class AbsoluteShapeTest(unittest.TestCase):
    def abs_code(self, base_ops, bound, index=None, load=None):
        code = list(bound)
        code += [index or asm.slwi(4, 9, 2)] + base_ops
        code += [load or asm.lwzx(0, 4, 5), asm.mtctr(0), asm.bctr()]
        return code

    def test_lis_addi_base_outside_the_image(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 3), asm.bgt(7, BASE + 4, DEFAULT)]
        targets = targets_for(4)
        extra = {OUTSIDE_TABLE + 4 * i: t for i, t in enumerate(targets)}
        fx = Fixture(self.abs_code(base_ops, bound), extra=extra)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.kind, jt.table, jt.count), ("abs32", OUTSIDE_TABLE, 4))
        self.assertEqual(list(jt.targets), targets)
        self.assertFalse(jt.table_in_text)

    def test_toc_loaded_base_table_inside_the_image(self):
        base_ops = [asm.ld(5, TOC_SLOT, 2)]
        bound = [asm.cmplwi(7, 9, 2), asm.bge(7, BASE + 4, DEFAULT)]
        code = self.abs_code(base_ops, bound)
        table = pc(code)
        # `ld` reads a doubleword; the descriptor-style slot keeps the address in its low half.
        fx = Fixture(code, targets_for(2), {TOC + TOC_SLOT: 0, TOC + TOC_SLOT + 4: table})
        jt = fx.recover()[0][fx.branch]
        self.assertEqual((jt.kind, jt.table, jt.count), ("abs32", table, 2))
        self.assertTrue(jt.table_in_text)
        self.assertEqual(list(jt.targets), targets_for(2))

    def test_addis_toc_base(self):
        base_ops = [asm.addis(5, 2, 1), asm.addi(5, 5, -0x10)]
        table = (TOC + 0x10000 - 0x10) & 0xFFFFFFFF
        bound = [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)]
        extra = {table + 4 * i: t for i, t in enumerate(targets_for(2))}
        fx = Fixture(self.abs_code(base_ops, bound), extra=extra)
        jt = fx.recover()[0][fx.branch]
        self.assertEqual((jt.table, jt.count), (table, 2))

    def test_lwax_and_base_index_swap(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)]
        extra = {OUTSIDE_TABLE + 4 * i: t for i, t in enumerate(targets_for(2))}
        code = self.abs_code(base_ops, bound, load=asm.lwax(0, 5, 4))
        fx = Fixture(code, extra=extra)
        jt = fx.recover()[0][fx.branch]
        self.assertEqual((jt.kind, jt.count), ("abs32", 2))

    def test_table_unreadable(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 3), asm.bgt(7, BASE + 4, DEFAULT)]
        fx = Fixture(self.abs_code(base_ops, bound))
        self.assertEqual(fx.reason(), "table_unreadable")

    def test_table_cut_short_is_not_returned_partially(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 3), asm.bgt(7, BASE + 4, DEFAULT)]
        extra = {OUTSIDE_TABLE + 4 * i: t for i, t in enumerate(targets_for(3))}
        fx = Fixture(self.abs_code(base_ops, bound), extra=extra)
        self.assertEqual(fx.reason(), "table_unreadable")

    def test_bad_target(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)]
        extra = {OUTSIDE_TABLE: CASES, OUTSIDE_TABLE + 4: 0x00300000}
        fx = Fixture(self.abs_code(base_ops, bound), extra=extra)
        tables, unresolved = fx.recover()
        self.assertEqual(tables, {})
        self.assertEqual((unresolved[fx.branch].reason, unresolved[fx.branch].detail),
                         ("bad_target", "0x00300000"))

    def test_bad_target_undecodable_word(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        bound = [asm.cmplwi(7, 9, 0), asm.bgt(7, BASE + 4, DEFAULT)]
        code = self.abs_code(base_ops, bound)
        tail = [0] * (WORDS - len(code))
        zero_word = BASE + 4 * len(code)
        fx = Fixture(code, tail, {OUTSIDE_TABLE: zero_word})
        self.assertEqual(fx.reason(), "bad_target")

    def test_bad_bound_zero_and_too_large(self):
        base_ops = [asm.lis(5, OUTSIDE_TABLE >> 16), asm.addi(5, 5, OUTSIDE_TABLE & 0xFFFF)]
        for bound in ([asm.cmplwi(7, 9, 0), asm.bge(7, BASE + 4, DEFAULT)],
                      [asm.cmplwi(7, 9, 5000), asm.bgt(7, BASE + 4, DEFAULT)]):
            with self.subTest(bound=bound):
                fx = Fixture(self.abs_code(base_ops, bound))
                self.assertEqual(fx.reason(), "bad_bound")


class UnresolvedReasonTest(unittest.TestCase):
    def reason(self, code):
        fx = Fixture(code, extra={TOC + TOC_SLOT: BASE})
        return fx.reason()

    def test_no_mtctr(self):
        self.assertEqual(self.reason([asm.nop(), asm.bctr()]), "no_mtctr")

    def test_no_mtctr_behind_a_call(self):
        code = [asm.mtctr(0), asm.b(BASE + 4, DEFAULT, lk=True), asm.bctr()]
        self.assertEqual(self.reason(code), "no_mtctr")

    def test_ctr_from_load(self):
        self.assertEqual(self.reason([asm.lwz(0, 0, 11), asm.mtctr(0), asm.bctr()]),
                         "ctr_from_load")

    def test_ctr_from_ld(self):
        self.assertEqual(self.reason([asm.ld(0, 8, 11), asm.mtctr(0), asm.bctr()]),
                         "ctr_from_load")

    def test_load_from_constant_address_is_unknown_def(self):
        code = [asm.lis(11, 0x20), asm.lwz(0, 0, 11), asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "unknown_def")

    def test_unknown_def(self):
        self.assertEqual(self.reason([asm.li(0, 4), asm.mtctr(0), asm.bctr()]), "unknown_def")

    def test_no_definition_at_all(self):
        self.assertEqual(self.reason([asm.mtctr(0), asm.bctr()]), "unknown_def")

    def test_add_without_table_load(self):
        code = [asm.add(0, 3, 4), asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "unknown_def")

    def test_no_table_base(self):
        code = [asm.slwi(9, 3, 2), asm.lwzx(0, 9, 4), asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "no_table_base")

    def test_add_operand_not_constant(self):
        code = [asm.lwz(11, TOC_SLOT, 2), asm.slwi(9, 3, 2), asm.lwzx(0, 9, 11),
                asm.add(0, 0, 5), asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "no_table_base")

    def test_no_index(self):
        code = [asm.lwz(11, TOC_SLOT, 2), asm.add(9, 3, 3), asm.lwzx(0, 9, 11),
                asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "no_index")

    def test_index_not_scaled_by_four(self):
        code = [asm.lwz(11, TOC_SLOT, 2), asm.slwi(9, 3, 3), asm.lwzx(0, 9, 11),
                asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "no_index")

    def test_both_load_operands_constant(self):
        code = [asm.lwz(11, TOC_SLOT, 2), asm.li(9, 8), asm.lwzx(0, 9, 11),
                asm.mtctr(0), asm.bctr()]
        self.assertEqual(self.reason(code), "no_index")

    def test_no_bound(self):
        self.assertEqual(self.reason(spine(asm.slwi(9, 9, 2))), "no_bound")

    def test_compare_on_unrelated_register(self):
        code = [asm.cmplwi(7, 6, 4), asm.bgt(7, BASE + 4, DEFAULT)]
        self.assertEqual(self.reason(code + spine(asm.slwi(9, 9, 2))), "no_bound")

    def test_compare_register_redefined_before_index(self):
        code = [asm.cmplwi(7, 9, 4), asm.li(9, 1), asm.bgt(7, BASE + 8, DEFAULT)]
        self.assertEqual(self.reason(code + spine(asm.slwi(9, 9, 2))), "no_bound")

    def test_condition_register_overwritten_after_compare(self):
        code = [asm.cmplwi(7, 9, 4), asm.cmplwi(7, 6, 0), asm.bgt(7, BASE + 8, DEFAULT)]
        self.assertEqual(self.reason(code + spine(asm.slwi(9, 9, 2))), "no_bound")

    def test_other_condition_field_is_not_the_bound(self):
        code = [asm.cmplwi(7, 9, 4), asm.bgt(6, BASE + 4, DEFAULT)]
        self.assertEqual(self.reason(code + spine(asm.slwi(9, 9, 2))), "no_bound")


TOC2 = 0x00119F80


class MultiTocTest(unittest.TestCase):
    """The same `lwz r11,-8(r2)` dispatch resolves through whichever TOC holds the table slot."""

    def code(self):
        return [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)] + spine(asm.slwi(9, 9, 2))

    def fixture(self, slots, *, second_table=False):
        code = self.code()
        table = pc(code)
        entries = [(t - table) & 0xFFFFFFFF for t in targets_for(2)]
        tail = list(entries)
        extra = {toc + TOC_SLOT: table for toc in slots}
        if second_table:
            # Same layout, different targets, for the TOC that must disagree.
            table2 = table + 8
            tail += [(CASES + 4 * 3 - table2) & 0xFFFFFFFF, (CASES + 4 * 4 - table2) & 0xFFFFFFFF]
            extra = {slots[0] + TOC_SLOT: table, slots[1] + TOC_SLOT: table2}
        return Fixture(code, tail, extra), table

    def recover(self, fx, tocs, descriptors=()):
        return jumptab.recover_jump_tables(fx.image, fx.read_mem, tocs, descriptors)

    def test_non_entry_toc_resolves_the_site(self):
        fx, table = self.fixture([TOC2])
        tables, unresolved = self.recover(fx, (TOC, TOC2))
        self.assertEqual(unresolved, {})
        self.assertEqual((tables[fx.branch].table, tables[fx.branch].count), (table, 2))
        self.assertEqual(list(tables[fx.branch].targets), targets_for(2))

    def test_entry_toc_alone_does_not_resolve_it(self):
        fx, _ = self.fixture([TOC2])
        tables, unresolved = self.recover(fx, (TOC,))
        self.assertEqual(tables, {})
        self.assertEqual(unresolved[fx.branch].reason, "no_table_base")

    def test_single_toc_int_and_sequence_agree(self):
        fx, _ = self.fixture([TOC])
        self.assertEqual(self.recover(fx, TOC), self.recover(fx, (TOC,)))

    def test_tocs_giving_the_same_table_resolve(self):
        fx, table = self.fixture([TOC, TOC2])
        tables, unresolved = self.recover(fx, (TOC, TOC2))
        self.assertEqual(unresolved, {})
        self.assertEqual(tables[fx.branch].table, table)

    def test_tocs_giving_different_tables_are_ambiguous(self):
        fx, _ = self.fixture([TOC, TOC2], second_table=True)
        tables, unresolved = self.recover(fx, (TOC, TOC2))
        self.assertEqual(tables, {})
        self.assertEqual(unresolved[fx.branch].reason, "ambiguous_toc")

    def test_owner_descriptor_toc_picks_its_table(self):
        fx, table = self.fixture([TOC, TOC2], second_table=True)
        # The function owning the site is the nearest descriptor start at or below it.
        far = BASE + 4 * (WORDS + 1)
        for owner_toc, expected in ((TOC, table), (TOC2, table + 8)):
            descriptors = [(BASE - 0x100, TOC2 + 8), (BASE, owner_toc), (far, 0)]
            tables, unresolved = self.recover(fx, (TOC, TOC2), descriptors)
            self.assertEqual(unresolved, {})
            self.assertEqual(tables[fx.branch].table, expected)
        self.assertEqual(list(tables[fx.branch].targets),
                         [CASES + 4 * 3, CASES + 4 * 4])

    def test_owner_toc_outside_the_yielding_tocs_is_ambiguous(self):
        fx, _ = self.fixture([TOC, TOC2], second_table=True)
        for descriptors in ([(BASE, TOC2 + 8)],          # owner names a third TOC
                            [(BASE + 4 * WORDS, TOC2)]):  # no descriptor at or below the site
            tables, unresolved = self.recover(fx, (TOC, TOC2, TOC2 + 8), descriptors)
            self.assertEqual(tables, {})
            self.assertEqual(unresolved[fx.branch].reason, "ambiguous_toc")

    def test_failure_keeps_the_furthest_reason(self):
        # No bound check: every TOC fails; the TOC that does hold the slot reaches no_bound.
        code = spine(asm.slwi(9, 9, 2))
        table = pc(code)
        fx = Fixture(code, [0, 0], {TOC2 + TOC_SLOT: table})
        _, unresolved = self.recover(fx, (TOC, TOC2))
        self.assertEqual(unresolved[fx.branch].reason, "no_bound")


class ThroughCallsTest(unittest.TestCase):
    """A table base kept across a call: in a callee-saved register or in a stack slot."""

    SLOT = 0x78

    def check(self, code, count):
        fx, table, targets = rel_fixture(code, count)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.table, jt.count, list(jt.targets)), (table, count, targets))

    def tail(self, base):
        return [asm.slwi(9, 3, 2), asm.lwzx(0, 9, base), asm.extsw(0, 0), asm.add(0, 0, base),
                asm.mtctr(0), asm.bctr()]

    def bound_after_call(self, code):
        # Call, then a bound check that leaves the table path.
        code += [asm.bl(pc(code), DEFAULT), asm.cmplwi(7, 3, 4)]
        return code + [asm.bgt(7, pc(code), DEFAULT)]

    def test_callee_saved_base_before_a_retry_loop_call(self):
        # lwz r30; L: bl f; cmplwi cr7,r3,5; bgt cr7,L; the base is defined above the loop's
        # back-edge, so only the constant evaluation may look past the call.
        code = [asm.lwz(30, TOC_SLOT, 2)]
        loop = pc(code)
        code += [asm.bl(pc(code), DEFAULT), asm.cmplwi(7, 3, 5)]
        code += [asm.bgt(7, pc(code), loop)]
        self.check(code + self.tail(30), 6)

    def test_base_spilled_to_a_stack_slot_across_a_call(self):
        code = [asm.lwz(9, TOC_SLOT, 2), asm.std(9, self.SLOT, 1)]
        code = self.bound_after_call(code)
        self.check(code + [asm.ld(11, self.SLOT, 1)] + self.tail(11), 5)

    def test_word_spill_and_reload(self):
        code = [asm.lwz(9, TOC_SLOT, 2), asm.stw(9, self.SLOT, 1)]
        code = self.bound_after_call(code)
        self.check(code + [asm.lwz(11, self.SLOT, 1)] + self.tail(11), 5)

    def test_volatile_base_is_not_carried_across_a_call(self):
        code = self.bound_after_call([asm.lwz(11, TOC_SLOT, 2)])
        fx, _, _ = rel_fixture(code + self.tail(11), 5)
        self.assertEqual(fx.reason(), "no_table_base")

    def test_stack_pointer_write_between_store_and_reload(self):
        code = [asm.lwz(9, TOC_SLOT, 2), asm.std(9, self.SLOT, 1), asm.addi(1, 1, -0x10)]
        code = self.bound_after_call(code)
        fx, _, _ = rel_fixture(code + [asm.ld(11, self.SLOT, 1)] + self.tail(11), 5)
        self.assertEqual(fx.reason(), "no_table_base")

    def test_overlapping_store_of_another_width_hides_the_spill(self):
        code = [asm.lwz(9, TOC_SLOT, 2), asm.std(9, self.SLOT, 1), asm.stw(0, self.SLOT + 4, 1)]
        code = self.bound_after_call(code)
        fx, _, _ = rel_fixture(code + [asm.ld(11, self.SLOT, 1)] + self.tail(11), 5)
        self.assertEqual(fx.reason(), "no_table_base")


class IndexCopiesAcrossCallsTest(unittest.TestCase):
    """The index and the compared value are callee-saved copies of one argument, with calls between."""

    def code(self, compared_src=4, index_src=4):
        code = [asm.clrldi(31, index_src, 32), asm.mr(29, compared_src)]
        code += [asm.bl(pc(code), DEFAULT)]
        code += [asm.bl(pc(code), DEFAULT), asm.cmplwi(7, 29, 8)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        return code + spine(asm.sldi(9, 31, 2))

    def test_copies_of_the_same_argument_share_a_bound(self):
        fx, table, targets = rel_fixture(self.code(), 9)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.table, jt.count, list(jt.targets)), (table, 9, targets))

    def test_copies_of_different_arguments_are_not_a_bound(self):
        fx, _, _ = rel_fixture(self.code(compared_src=5), 9)
        self.assertEqual(fx.reason(), "no_bound")

    def test_volatile_copies_are_not_followed_across_calls(self):
        code = [asm.clrldi(9, 4, 32), asm.mr(10, 4)]
        code += [asm.bl(pc(code), DEFAULT), asm.cmplwi(7, 10, 8)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        fx, _, _ = rel_fixture(code + spine(asm.sldi(9, 9, 2)), 9)
        self.assertEqual(fx.reason(), "no_bound")


class SpilledIndexTest(unittest.TestCase):
    """Unoptimised code: the index is spilled after its bound check and reloaded before scaling."""

    SLOT = 0x5BC

    def code(self, between=(), reload_slot=SLOT):
        code = [asm.lwz(9, 8, 3), asm.cmplwi(7, 9, 9), asm.stw(9, self.SLOT, 1)]
        code += [asm.bgt(7, pc(code), DEFAULT)]
        code += list(between)
        code += [asm.lwz(8, reload_slot, 1)]
        return code + spine(asm.rldic(9, 8, 2, 30))

    def test_reloaded_index_takes_the_bound_of_the_stored_register(self):
        fx, table, targets = rel_fixture(self.code(), 10)
        tables, unresolved = fx.recover()
        self.assertEqual(unresolved, {})
        jt = tables[fx.branch]
        self.assertEqual((jt.table, jt.count, list(jt.targets)), (table, 10, targets))

    def test_different_slot_offset(self):
        fx, _, _ = rel_fixture(self.code(reload_slot=self.SLOT + 8), 10)
        self.assertEqual(fx.reason(), "no_bound")

    def test_slot_stored_again_before_the_reload(self):
        fx, _, _ = rel_fixture(self.code(between=[asm.stw(6, self.SLOT, 1)]), 10)
        self.assertEqual(fx.reason(), "no_bound")

    def test_stack_pointer_written_before_the_reload(self):
        fx, _, _ = rel_fixture(self.code(between=[asm.addi(1, 1, -0x10)]), 10)
        self.assertEqual(fx.reason(), "no_bound")

    def test_stored_register_redefined_before_the_compare(self):
        # The compare reads a different value of the register than the one that was stored.
        code = [asm.lwz(9, 8, 3), asm.stw(9, self.SLOT, 1), asm.lwz(9, 12, 3),
                asm.cmplwi(7, 9, 9)]
        code += [asm.bgt(7, pc(code), DEFAULT), asm.lwz(8, self.SLOT, 1)]
        fx, _, _ = rel_fixture(code + spine(asm.rldic(9, 8, 2, 30)), 10)
        self.assertEqual(fx.reason(), "no_bound")


class RecoverAllTest(unittest.TestCase):
    def test_every_bctr_lands_in_exactly_one_dict(self):
        good = [asm.cmplwi(7, 9, 1), asm.bgt(7, BASE + 4, DEFAULT)] + spine(asm.slwi(9, 9, 2))
        bad = [asm.lwz(0, 0, 11), asm.mtctr(0), asm.bctr()]
        call = [asm.mtctr(12), asm.bctrl()]
        code = good + [asm.nop()] + bad + call
        table = BASE + 4 * len(code)
        entries = [(t - table) & 0xFFFFFFFF for t in targets_for(2)]
        fx = Fixture(code, entries, {TOC + TOC_SLOT: table})
        tables, unresolved = fx.recover()
        self.assertEqual(sorted(tables), [BASE + 4 * (len(good) - 1)])
        self.assertEqual(sorted(unresolved), [BASE + 4 * (len(code) - len(call) - 1)])
        self.assertEqual(unresolved[BASE + 4 * (len(code) - len(call) - 1)].reason,
                         "ctr_from_load")

    def test_reasons_are_documented(self):
        self.assertEqual(len(set(jumptab.UNRESOLVED_REASONS)), 10)


class MakeReaderTest(unittest.TestCase):
    def test_file_backed_bytes_only(self):
        data_va = 0x100000
        raw = build_elf64_be(entry=data_va, segments=[
            FixtureSegment(1, BASE, struct.pack(">4I", 1, 2, 3, 4), flags=5),
            FixtureSegment(1, data_va, struct.pack(">2I", 0xAABBCCDD, 0x11223344),
                           memsz=0x1000, flags=6),
        ])
        read_mem = jumptab.make_reader(ElfFile.from_bytes(raw))
        self.assertEqual(read_mem(BASE + 4, 8), struct.pack(">2I", 2, 3))
        self.assertEqual(read_mem(data_va + 4, 4), struct.pack(">I", 0x11223344))
        self.assertIsNone(read_mem(data_va + 8, 4))     # zero-filled tail
        self.assertIsNone(read_mem(data_va + 4, 8))     # straddles the file-backed end
        self.assertIsNone(read_mem(0x500000, 4))        # unmapped


if __name__ == "__main__":
    unittest.main()

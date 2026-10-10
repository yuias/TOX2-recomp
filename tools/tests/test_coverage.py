"""Tests for the coverage and Ghidra consistency report sections."""

import unittest

from ps3recomp.coverage import build_coverage, coverage_lines
from ps3recomp.ghidra_summary import GhidraFunction, GhidraSummary
from ps3recomp.jumptab import JumpTable
from ps3recomp.partition import partition
from ps3recomp.ppu_flow import CodeImage
from ps3recomp.seeds import Seed

from tests import ppc_asm as asm

BASE = 0x10000
BAD = 0x00000001      # not a valid instruction
LI = asm.li(3, 1)


def at(index: int) -> int:
    return BASE + 4 * index


def h(index: int) -> str:
    return "0x%08x" % at(index)


def build(words) -> CodeImage:
    return CodeImage.from_words(BASE, words)


def recover(words, starts, tables=None, sources=None):
    image = build(words)
    sources = sources or {}
    seeds = [Seed(at(i), sources.get(i, "ghidra")) for i in starts]
    result = partition(image, seeds, tables or {}, {})
    assert result.converged
    return image, result.functions


def make_summary(image, functions=(), code=(), data=(), refs=()):
    return GhidraSummary(
        sha256="0" * 64, text=(image.start, image.end),
        functions=tuple(GhidraFunction(at(i), None, False,
                                       tuple((at(a), at(b)) for a, b in body))
                        for i, body in functions),
        code_ranges=tuple((at(a), at(b)) for a, b in code),
        data_ranges=tuple((at(a), at(b)) for a, b in data),
        data_refs=tuple(refs), bctr_sites=())


class UncoveredTest(unittest.TestCase):
    def setUp(self):
        # idx: 0 f0 | 1-2 zero | 3 f | 4-5 padding | 6 f | 7-8 table | 9 f | 10-11 data | 12 f
        #      | 13-14 code
        self.words = [asm.blr(), 0, 0, asm.blr(), asm.nop(), 0, asm.blr(), 0x10, 0x20,
                      asm.blr(), BAD, BAD, asm.blr(), LI, asm.li(4, 2)]
        self.starts = [0, 3, 6, 9, 12]
        self.tables = {at(0): JumpTable(at(0), at(7), "rel32", 2, (at(9), at(9)), True)}

    def classes(self, summary=None):
        image, functions = recover(self.words, self.starts, self.tables)
        coverage, ghidra = build_coverage(image, functions, self.tables, [], summary)
        return coverage, ghidra, image

    def test_each_class_and_totals(self):
        coverage, ghidra, _ = self.classes()
        unc = coverage["uncovered"]
        self.assertEqual({k: (v["words"], v["runs"]) for k, v in unc["by_class"].items()},
                         {"zero": (2, 1), "padding": (2, 1), "jump_table": (2, 1),
                          "data": (2, 1), "code": (2, 1)})
        self.assertEqual((unc["words"], unc["runs"]), (10, 5))
        self.assertEqual((coverage["text_words"], coverage["owned_words"]), (15, 5))
        self.assertEqual(coverage["owned_pct"], 33.33)
        self.assertIsNone(ghidra)

    def test_valid_words_overlapping_a_ghidra_data_range_are_data(self):
        image = build(self.words)
        summary = make_summary(image, data=[(13, 15)])
        coverage, _, _ = self.classes(summary)
        by_class = coverage["uncovered"]["by_class"]
        self.assertEqual((by_class["data"]["words"], by_class["code"]["words"]), (4, 0))

    def test_valid_fraction_threshold(self):
        # 3 of 4 decodable words is code, 2 of 4 is data.
        for words, expected in (([LI, LI, LI, BAD], "code"), ([LI, BAD, LI, BAD], "data")):
            image, functions = recover([asm.blr()] + words, [0])
            coverage, _ = build_coverage(image, functions, {}, [])
            self.assertEqual(coverage["uncovered"]["by_class"][expected]["runs"], 1, words)

    def test_largest_code_runs(self):
        words = [asm.blr(), asm.mflr(0), LI, asm.blr(), LI, asm.li(4, 2), asm.li(5, 3)]
        # Stop the first run: owned blr between the two code runs.
        image, functions = recover(words, [0, 3])
        summary = make_summary(image, code=[(4, 6)])
        coverage, _ = build_coverage(image, functions, {}, [], summary)
        runs = coverage["uncovered"]["largest_code_runs"]
        self.assertEqual([(r["start"], r["end"], r["words"]) for r in runs],
                         [(h(4), h(7), 3), (h(1), h(3), 2)])
        self.assertEqual([r["starts_with_prologue"] for r in runs], [False, True])
        self.assertEqual([r["ghidra_code_words"] for r in runs], [2, 0])


class PrologueBucketsTest(unittest.TestCase):
    def test_buckets(self):
        # f0 (0..1), f at 2 (2..3), unowned code at 5; candidates: 2 (start), 1 (inside f0),
        # 5 (uncovered).
        words = [asm.nop(), asm.blr(), asm.nop(), asm.blr(), asm.blr(), LI]
        image, functions = recover(words, [0, 2])
        coverage, _ = build_coverage(image, functions, {}, [at(2), at(1), at(5)])
        self.assertEqual(coverage["prologue_candidates"],
                         {"total": 3, "already_start": 1, "inside_function": 1,
                          "in_uncovered": 1, "in_uncovered_code_runs": 1})


class ExtentsTest(unittest.TestCase):
    def test_categories(self):
        # f0 equal, f3 ours_larger, f7 ours_smaller, f11 different, ghidra start 16 missing.
        words = ([asm.nop(), asm.blr(), BAD] + [asm.nop(), asm.nop(), asm.blr(), BAD]
                 + [asm.nop(), asm.blr(), BAD, BAD] + [asm.nop(), asm.blr(), BAD, BAD, BAD]
                 + [asm.blr()])
        image, functions = recover(words, [0, 3, 7, 11])
        summary = make_summary(image, functions=[
            (0, [(0, 2)]),
            (3, [(3, 5)]),
            (7, [(7, 10)]),
            (11, [(11, 12), (13, 14)]),
            (16, [(16, 17)]),
        ])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        ext = ghidra["extents"]
        self.assertEqual({k: v["count"] for k, v in ext.items()},
                         {"equal": 1, "ours_larger": 1, "ours_smaller": 1, "different": 1,
                          "missing": 1})
        self.assertEqual(ext["equal"]["examples"],
                         [{"entry": h(0), "ghidra_words": 2, "our_words": 2}])
        larger = ext["ours_larger"]["examples"][0]
        self.assertEqual((larger["ghidra_words"], larger["our_words"], larger["only_ours"],
                          larger["only_ghidra"]), (2, 3, 1, 0))
        smaller = ext["ours_smaller"]["examples"][0]
        self.assertEqual((smaller["ghidra_words"], smaller["our_words"], smaller["only_ghidra"]),
                         (3, 2, 1))
        different = ext["different"]["examples"][0]
        self.assertEqual((different["only_ours"], different["only_ghidra"]), (1, 1))
        self.assertEqual(ext["missing"]["examples"], [{"entry": h(16), "ghidra_words": 1}])

    def test_ghidra_function_outside_text_is_ignored(self):
        image, functions = recover([asm.blr()], [0])
        summary = GhidraSummary("0" * 64, (image.start, image.end),
                                (GhidraFunction(0x500, None, False, ((0x500, 0x504),)),),
                                (), (), (), ())
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        self.assertEqual(sum(v["count"] for v in ghidra["extents"].values()), 0)


class CellTruncatedTest(unittest.TestCase):
    def test_extended_not_extended_and_multiple_points(self):
        lvlx = asm.lvlx(2, 0, 5)
        words = [
            asm.nop(), lvlx, asm.blr(),                                   # 0: extended
            asm.nop(), lvlx, asm.blr(),                                   # 3: split at 4
            asm.nop(), asm.lvrx(2, 0, 5), asm.nop(), asm.stvlx(2, 0, 5),  # 6: two points
            asm.blr(),
            asm.nop(), asm.blr(),                                         # 11: not truncated
        ]
        image, functions = recover(words, [0, 3, 4, 6, 11])
        summary = make_summary(image, functions=[
            (0, [(0, 1)]),
            (3, [(3, 4)]),
            (6, [(6, 7), (8, 9)]),
            (11, [(11, 12)]),
        ])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        cell = ghidra["cell_truncated"]
        self.assertEqual((cell["count"], cell["extended"], cell["points"],
                          cell["extended_points"]), (3, 2, 4, 3))
        self.assertEqual(cell["not_extended"],
                         [{"function": h(3), "end": h(4), "owner": h(4)}])

    def test_unowned_end_has_no_owner(self):
        words = [asm.nop(), asm.blr(), asm.lvlx(2, 0, 5)]
        image, functions = recover(words, [0])
        summary = make_summary(image, functions=[(0, [(0, 1)])])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        self.assertEqual(ghidra["cell_truncated"]["count"], 0)     # ends at a blr
        summary = make_summary(image, functions=[(0, [(0, 2)]), (1, [(1, 2)])])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        self.assertEqual(ghidra["cell_truncated"]["count"], 2)
        self.assertEqual(ghidra["cell_truncated"]["not_extended"],
                         [{"function": h(0), "end": h(2), "owner": None},
                          {"function": h(1), "end": h(2), "owner": None}])


class GhidraSectionsTest(unittest.TestCase):
    def test_code_not_owned(self):
        words = [asm.blr(), LI, LI, asm.blr(), LI]
        image, functions = recover(words, [0, 3])
        summary = make_summary(image, code=[(0, 3), (4, 5)])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        self.assertEqual(ghidra["code_not_owned"],
                         {"words": 3, "runs": 2,
                          "ranges": [[h(1), h(3)], [h(4), h(5)]]})

    def test_data_ref_classes(self):
        words = [asm.nop(), asm.nop(), asm.blr(), asm.blr(), 0x10, 0x20, BAD]
        tables = {at(0): JumpTable(at(0), at(4), "rel32", 2, (at(2), at(2)), True)}
        image, functions = recover(words, [0, 3], tables)
        refs = [
            (0x100, at(3), "DATA", False),     # function_start
            (0x100, at(2), "READ", True),      # jump_table_target
            (0x100, at(1), "READ", True),      # mid_function
            (0x100, at(4), "DATA", False),     # jump_table_data
            (0x100, at(6), "READ", True),      # uncovered
            (0x100, 0x500, "READ", True),      # outside_text
        ]
        summary = make_summary(image, refs=refs)
        _, ghidra = build_coverage(image, functions, tables, [], summary)
        refs_doc = ghidra["data_refs"]
        self.assertEqual(refs_doc["from_exec"],
                         {"function_start": 0, "jump_table_target": 1, "mid_function": 1,
                          "jump_table_data": 0, "uncovered": 1, "outside_text": 1})
        self.assertEqual(refs_doc["not_from_exec"],
                         {"function_start": 1, "jump_table_target": 0, "mid_function": 0,
                          "jump_table_data": 1, "uncovered": 0, "outside_text": 0})

    def test_starts_only_branch_reached(self):
        # f0 tail-branches to idx 3 (a Ghidra-only start); idx 5 is not reached at all.
        words = [asm.nop(), asm.b(at(1), at(3)), asm.blr(),
                 asm.nop(), asm.blr(),
                 asm.nop(), asm.blr()]
        image, functions = recover(words, [0, 3, 5])
        summary = make_summary(image, functions=[(0, [(0, 2)]), (3, [(3, 5)]), (5, [(5, 7)])])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        only = ghidra["starts_only_branch_reached"]
        self.assertEqual(only, {"count": 1, "examples": [{"entry": h(3), "from": [h(0)]}]})

    def test_called_or_opd_starts_are_not_candidates(self):
        words = [asm.b(at(0), at(3), lk=True), asm.b(at(1), at(4)), asm.blr(),
                 asm.blr(), asm.blr()]
        image, functions = recover(words, [0, 3, 4], sources={4: "opd"})
        summary = make_summary(image, functions=[(0, [(0, 2)]), (3, [(3, 4)]), (4, [(4, 5)])])
        _, ghidra = build_coverage(image, functions, {}, [], summary)
        self.assertEqual(ghidra["starts_only_branch_reached"]["count"], 0)


class LinesTest(unittest.TestCase):
    def test_lines(self):
        image, functions = recover([asm.blr(), BAD], [0])
        summary = make_summary(image, functions=[(0, [(0, 1)])])
        coverage, ghidra = build_coverage(image, functions, {}, [], summary)
        lines = coverage_lines(coverage, ghidra)
        self.assertEqual(lines, [
            "coverage: owned 1 / 2 words (50.0%); uncovered 1 words: zero 0, padding 0, "
            "jump_table 0, data 1, code 0",
            "ghidra extents: equal 1, ours_larger 0, ours_smaller 0, different 0, missing 0; "
            "cell-truncated 0, extended 0",
            "ghidra code not owned: 0 words in 0 ranges",
        ])
        self.assertEqual(len(coverage_lines(coverage, None)), 1)


if __name__ == "__main__":
    unittest.main()

"""Tests for seed collection, OPD/data-pointer scans and import thunk handling."""

import struct
import unittest

from ps3recomp import cell, seeds
from ps3recomp.elf import ElfFile
from ps3recomp.ghidra_summary import GhidraFunction, GhidraSummary
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm
from tests import elf_fixture as fx
from tests.elf_fixture import PS3_DATA_VA, PS3_TEXT_VA, FixtureSegment, build_elf64_be, \
    build_ps3_fixture

TEXT = PS3_TEXT_VA
DATA = PS3_DATA_VA
TOC = 0x28000
SLOT = 0x20150


def build_elf(text_words, data: bytes, entry_func: int = TEXT, toc: int = TOC) -> ElfFile:
    """Image with `.text` at TEXT, a data segment at DATA and an OPD for the entry at DATA."""
    text = struct.pack(">%dI" % len(text_words), *text_words)
    data = struct.pack(">II", entry_func, toc) + data
    raw = build_elf64_be(entry=DATA, segments=[
        FixtureSegment(1, TEXT, text, flags=5),
        FixtureSegment(1, DATA, data, memsz=0x10000, flags=6),
    ])
    return ElfFile.from_bytes(raw)


def image_of(*words: int) -> CodeImage:
    return CodeImage.from_words(TEXT, words)


def pack_words(values) -> bytes:
    return b"".join(struct.pack(">I", v) for v in values)


class OpdTest(unittest.TestCase):
    def test_fixture_descriptors(self):
        elf = ElfFile.from_bytes(build_ps3_fixture())
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x20))
        self.assertEqual(seeds.find_opd_descriptors(elf, image, TOC),
                         [(DATA, TEXT), (DATA + 8, TEXT + 0x18)])

    def test_wrong_toc_and_outside_func_rejected(self):
        elf = ElfFile.from_bytes(build_ps3_fixture())
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x20))
        self.assertEqual(seeds.find_opd_descriptors(elf, image, TOC + 8), [])
        # Shrinking the image drops the descriptor whose func is no longer inside it.
        small = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        self.assertEqual(seeds.find_opd_descriptors(elf, small, TOC), [(DATA, TEXT)])

    def test_toc_set_accepts_descriptors_of_every_toc(self):
        elf = ElfFile.from_bytes(build_ps3_fixture())
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x20))
        self.assertEqual(seeds.find_opd_descriptors(elf, image, {TOC + 8, TOC}),
                         [(DATA, TEXT), (DATA + 8, TEXT + 0x18)])

    def test_collect_tocs_entry_first_then_frequent_aligned_tocs(self):
        toc2 = DATA + 0x2000
        stray = DATA + 0x4004          # unaligned and rare
        rare = DATA + 0x4008           # aligned but named by too few descriptors
        pairs = [(TEXT, toc2)] * 20 + [(TEXT, stray)] * 20 + [(TEXT, rare)] * 3
        data = b"".join(struct.pack(">II", f, t) for f, t in pairs)
        elf = build_elf([asm.blr()] * 4, data)
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        self.assertEqual(seeds.collect_tocs(elf, image), (TOC, toc2))

    def test_collect_tocs_drops_candidates_outside_loaded_data(self):
        # Frequent and aligned, but inside the code segment and past the end of the data segment.
        in_text = TEXT + 0x20
        past_data = DATA + 0x10000
        pairs = [(TEXT, in_text)] * 20 + [(TEXT, past_data)] * 20
        data = b"".join(struct.pack(">II", f, t) for f, t in pairs)
        elf = build_elf([asm.blr()] * 4, data)
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        self.assertEqual(seeds.collect_tocs(elf, image), (TOC,))

    def test_executable_segment_is_not_scanned(self):
        # A descriptor-looking pair inside the code segment must not count.
        elf = build_elf([TEXT, TOC, asm.blr(), asm.blr()], b"")
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        self.assertEqual(seeds.find_opd_descriptors(elf, image, TOC), [(DATA, TEXT)])


class PrxSeedsTest(unittest.TestCase):
    BASE = 0x20000

    def setUp(self):
        self.elf = ElfFile.from_bytes(fx.build_prx_fixture(True, self.BASE))
        self.image = CodeImage.from_elf(self.elf, cell.find_code_range(self.elf))
        self.toc = self.BASE + fx.PRX_TOC
        self.funcs = [self.BASE + f for f in fx.PRX_FUNCS]

    def test_descriptors_sit_at_4_mod_8_and_are_found(self):
        self.assertEqual(seeds.find_opd_descriptors(self.elf, self.image, self.toc),
                         [(self.BASE + opd, func) for opd, func in zip(fx.PRX_OPDS, self.funcs)])

    def test_toc_is_the_module_toc(self):
        self.assertEqual(seeds.collect_tocs(self.elf, self.image), (self.toc,))

    def test_executable_scan_stays_8_aligned(self):
        # The same pair one word off the 8-byte grid is a descriptor only for a PRX.
        data = struct.pack(">I", 0) + struct.pack(">II", TEXT + 8, TOC)
        elf = build_elf([asm.blr()] * 4, data)
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        self.assertEqual(seeds.find_opd_descriptors(elf, image, TOC), [(DATA, TEXT)])

    def test_no_entry_seed_without_an_entry_function(self):
        thunks = seeds.find_import_thunks(self.elf, None)
        census = seeds.collect_recovery_seeds(
            self.elf, self.image, None, thunks, jump_table_targets=set(), extra=[], prologue=False,
            tocs=(self.toc,))
        self.assertFalse([s for s in census.seeds if s.source == "entry"])
        self.assertEqual([(s.addr, s.source) for s in census.seeds if s.source != "bl"],
                         [(self.funcs[0], "export")] + [(f, "opd") for f in self.funcs])

    def test_import_thunks_come_from_the_module_info(self):
        thunks = seeds.find_import_thunks(self.elf, None)
        self.assertEqual(sorted(thunks), [self.BASE + t for t in fx.PRX_THUNKS])
        self.assertEqual([(t.module, t.fnid, t.slot_addr) for _, t in sorted(thunks.items())],
                         [("fixImp", fnid, self.BASE + slot)
                          for fnid, slot in zip(fx.PRX_FNIDS, fx.PRX_SLOTS)])
        self.assertEqual(seeds.check_thunk_slots(self.elf, thunks), [])


class BlScanTest(unittest.TestCase):
    def test_only_linking_i_form_branches_count(self):
        image = image_of(
            asm.b(TEXT, TEXT + 0x20, lk=True),            # 0x10000 counted
            asm.b(TEXT + 4, TEXT + 0x20),                 # plain jump
            asm.bc(12, 2, TEXT + 8, TEXT + 0x20, lk=True),  # bcl: not an I-form
            asm.bc(20, 31, TEXT + 12, TEXT + 16, lk=True),  # get-PC idiom
            asm.b(TEXT + 16, TEXT + 0x20, lk=True),       # second site, same target
            asm.b(TEXT + 20, 0x00FF0000, lk=True),        # target outside the image
            asm.blr(), asm.blr(), asm.blr())
        self.assertEqual(seeds.scan_bl_sites(image),
                         {TEXT + 0x20: [TEXT, TEXT + 16], 0x00FF0000: [TEXT + 20]})


class PrologueTest(unittest.TestCase):
    def test_is_prologue(self):
        image = image_of(asm.stdu(1, -0x70, 1), asm.stdu(1, 0x10, 1), asm.stdu(2, -0x70, 1),
                         asm.mflr(0), asm.mflr(12), 0xFFFFFFFF)
        self.assertEqual([seeds.is_prologue(image, TEXT + 4 * i) for i in range(6)],
                         [True, False, False, True, False, False])
        self.assertFalse(seeds.is_prologue(image, TEXT + 0x100))
        self.assertFalse(seeds.is_prologue(image, TEXT + 2))

    def test_candidates_follow_a_flow_boundary(self):
        image = image_of(
            asm.blr(),
            asm.mflr(0),                  # 0x10004: after return
            asm.add(3, 3, 4),
            asm.stdu(1, -0x70, 1),        # after add: not a candidate
            0,
            asm.stdu(1, -0x70, 1),        # after zero padding
            asm.nop(),
            asm.mflr(0),                  # after nop
            asm.b(TEXT + 32, TEXT + 0x40),
            asm.mflr(0),                  # after an unconditional jump
            asm.bc(12, 2, TEXT + 40, TEXT + 0x40),
            asm.mflr(0),                  # after a conditional branch: falls through
            asm.mflr(0))                  # after another prologue word
        self.assertEqual(seeds.prologue_candidates(image),
                         [TEXT + 4, TEXT + 20, TEXT + 28, TEXT + 36])

    def test_first_word_is_not_a_candidate(self):
        self.assertEqual(seeds.prologue_candidates(image_of(asm.mflr(0), asm.blr())), [])


class DataPointerTest(unittest.TestCase):
    def setUp(self):
        self.words = [
            asm.mflr(0),                                  # 0x10000 prologue (entry func)
            asm.blr(),
            asm.mflr(0),                                  # 0x10008 prologue, jump-table target
            asm.blr(),
            asm.add(3, 3, 4),                             # 0x10010 not a prologue
            asm.blr(),
            asm.mflr(0),                                  # 0x10018 prologue
            asm.blr(),
        ]
        # Pointers follow the 8-byte entry descriptor at DATA. None of them targets TEXT, so
        # TEXT can only be seeded through the descriptor's function field.
        self.pointers = [TEXT + 8, TEXT + 0x10, TEXT + 0x18, TEXT + 2, TEXT + 0x100, TEXT + 8]
        self.elf = build_elf(self.words, pack_words(self.pointers))
        self.image = CodeImage.from_elf(self.elf, (TEXT, TEXT + 0x20))
        self.locs = [DATA + 8 + 4 * i for i in range(len(self.pointers))]

    def test_scan_finds_aligned_pointers_into_the_image(self):
        found = seeds.scan_data_pointers(self.elf, self.image, set())
        # The descriptor's function field is a pointer too until it is excluded.
        self.assertEqual(found, [(DATA, TEXT), (self.locs[0], TEXT + 8),
                                 (self.locs[1], TEXT + 0x10), (self.locs[2], TEXT + 0x18),
                                 (self.locs[5], TEXT + 8)])

    def test_exclude_and_skip(self):
        found = seeds.scan_data_pointers(self.elf, self.image, {DATA, self.locs[2]},
                                         skip=(self.locs[1], self.locs[1] + 4))
        self.assertEqual(found, [(self.locs[0], TEXT + 8), (self.locs[5], TEXT + 8)])

    def collect(self, **kwargs):
        args = dict(jump_table_targets=set(), extra=[], prologue=False)
        args.update(kwargs)
        return seeds.collect_recovery_seeds(self.elf, self.image, None, {}, **args)

    @staticmethod
    def sources(census, source):
        return sorted(s.addr for s in census.seeds if s.source == source)

    def test_pointers_need_a_prologue_target(self):
        census = self.collect()
        self.assertEqual(self.sources(census, "data_ptr"), [TEXT + 8, TEXT + 0x18])
        self.assertEqual(census.counts["data_ptr_candidates"], 3)
        self.assertEqual(census.counts["data_ptr_seeds"], 2)

    def test_jump_table_targets_are_not_seeded(self):
        census = self.collect(jump_table_targets={TEXT + 8})
        self.assertEqual(self.sources(census, "data_ptr"), [TEXT + 0x18])

    def test_descriptor_function_field_is_not_a_data_pointer(self):
        census = self.collect()
        self.assertEqual(self.sources(census, "opd"), [TEXT])
        self.assertEqual(self.sources(census, "data_ptr"), [TEXT + 8, TEXT + 0x18])
        self.assertEqual(census.counts["opd"], 1)


class CensusTest(unittest.TestCase):
    THUNK = 0x000DCF50

    def setUp(self):
        self.words = [
            asm.mflr(0),                                  # 0x10000 entry, ghidra
            asm.b(TEXT + 4, TEXT + 0x10, lk=True),        # bl -> 0x10010 (not ghidra)
            asm.b(TEXT + 8, self.THUNK, lk=True),         # bl -> import thunk
            asm.blr(),
            asm.mflr(0),                                  # 0x10010
            asm.blr(),
            0,                                            # 0x10018 invalid
            asm.blr(),
        ]
        self.elf = build_elf(self.words, b"")
        self.image = CodeImage.from_elf(self.elf, (TEXT, TEXT + 0x20))
        self.thunks = {self.THUNK: seeds.ImportThunk(self.THUNK, "mod", 1, "fn", SLOT),
                       self.THUNK + 32: seeds.ImportThunk(self.THUNK + 32, "mod", 2, None,
                                                          SLOT + 4)}

    def summary(self, entries):
        funcs = tuple(GhidraFunction(a, None, False, ()) for a in entries)
        return GhidraSummary("0" * 64, (TEXT, TEXT + 0x20), funcs, (), (), (), ())

    def test_counts_and_rejections(self):
        summary = self.summary([TEXT, self.THUNK, 0x10200])
        census = seeds.collect_recovery_seeds(
            self.elf, self.image, summary, self.thunks, jump_table_targets=set(),
            extra=[TEXT + 0x18, 0x00BAD000, self.THUNK + 32], prologue=True)
        self.assertEqual(census.counts, {
            "ghidra_functions": 3, "ghidra_in_text": 1, "opd": 1, "opd_not_ghidra": 0,
            "bl_sites": 2, "bl_targets": 2, "bl_targets_in_text": 1,
            "bl_targets_not_ghidra": 1, "bl_targets_thunk": 1, "bl_targets_other": 0,
            "thunks": 2, "thunks_never_bl": 1, "data_ptr_candidates": 0, "data_ptr_seeds": 0,
            "ghidra_data_refs_from_data": 0, "ghidra_data_ref_seeds": 0,
            "prologue_candidates": 1, "prologue_seeds": 1, "manual": 3,
            "union_ghidra_bl_opd": 4, "seeds_total": 5, "rejected": 6})
        self.assertEqual(census.seeds, (
            seeds.Seed(TEXT, "entry"), seeds.Seed(TEXT, "ghidra"), seeds.Seed(TEXT, "opd"),
            seeds.Seed(TEXT + 0x10, "bl"), seeds.Seed(TEXT + 0x10, "prologue")))
        self.assertEqual(census.bl_sites, {TEXT + 0x10: [TEXT + 4], self.THUNK: [TEXT + 8]})
        self.assertEqual(census.rejected, (
            (TEXT + 0x18, "manual", "invalid_insn"),
            (0x10200, "ghidra", "outside_text"),
            (self.THUNK, "bl", "import_thunk"),
            (self.THUNK, "ghidra", "import_thunk"),
            (self.THUNK + 32, "manual", "import_thunk"),
            (0x00BAD000, "manual", "outside_text")))

    def test_prologue_seeds_only_when_requested(self):
        words = [asm.blr(), asm.mflr(0), asm.blr(), asm.stdu(1, -0x70, 1)]
        elf = build_elf(words, b"")
        image = CodeImage.from_elf(elf, (TEXT, TEXT + 0x10))
        off = seeds.collect_recovery_seeds(elf, image, None, {}, jump_table_targets=set(),
                                           extra=[], prologue=False)
        on = seeds.collect_recovery_seeds(elf, image, None, {}, jump_table_targets=set(),
                                          extra=[], prologue=True)
        self.assertEqual((off.counts["prologue_candidates"], off.counts["prologue_seeds"]),
                         (2, 0))
        self.assertEqual((on.counts["prologue_candidates"], on.counts["prologue_seeds"]),
                         (2, 2))
        self.assertEqual([s.addr for s in on.seeds if s.source == "prologue"],
                         [TEXT + 4, TEXT + 12])
        self.assertFalse([s for s in off.seeds if s.source == "prologue"])


class ThunkSlotTest(unittest.TestCase):
    def build(self):
        words = [asm.li(12, 0), asm.oris(12, 12, SLOT >> 16), asm.lwz(12, SLOT & 0xFFFF, 12),
                 asm.lwz(0, 0, 12), asm.lwz(2, 4, 12), asm.mtctr(0), asm.bctr(), asm.nop()]
        return build_elf(words, b"")

    def test_matching_slot(self):
        elf = self.build()
        thunks = {TEXT: seeds.ImportThunk(TEXT, "mod", 1, None, SLOT)}
        self.assertEqual(seeds.check_thunk_slots(elf, thunks), [])

    def test_mismatching_slot(self):
        elf = self.build()
        thunks = {TEXT: seeds.ImportThunk(TEXT, "mod", 1, None, SLOT + 4)}
        self.assertEqual(seeds.check_thunk_slots(elf, thunks), [TEXT])

    def test_thunk_without_a_slot_load(self):
        elf = build_elf([asm.nop()] * 8, b"")
        thunks = {TEXT: seeds.ImportThunk(TEXT, "mod", 1, None, SLOT)}
        self.assertEqual(seeds.check_thunk_slots(elf, thunks), [TEXT])

    def test_stub_range(self):
        thunks = {0x100: seeds.ImportThunk(0x100, "m", 1, None, 0),
                  0x140: seeds.ImportThunk(0x140, "m", 2, None, 0)}
        self.assertEqual(seeds.stub_range(thunks), (0x100, 0x160))
        self.assertIsNone(seeds.stub_range({}))

    def test_no_imports_in_a_plain_image(self):
        elf = build_elf([asm.blr()], b"")
        self.assertEqual(seeds.find_import_thunks(elf, None), {})


class FixtureThunkTest(unittest.TestCase):
    def test_import_thunks_from_the_ps3_fixture(self):
        elf = ElfFile.from_bytes(build_ps3_fixture())
        thunks = seeds.find_import_thunks(elf, None)
        # The fixture's slot values are zero-initialised, so all imports share thunk 0.
        self.assertEqual(len(thunks), 1)
        thunk = next(iter(thunks.values()))
        self.assertEqual(thunk.module, "cellSysutil")
        self.assertIsNone(thunk.name)


if __name__ == "__main__":
    unittest.main()

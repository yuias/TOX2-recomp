import os
import struct
import unittest

from ps3recomp import cell
from ps3recomp.elf import ElfError, ElfFile

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSection, FixtureSegment, build_elf64_be


def _patched(data: bytes, elf: ElfFile, va: int, payload: bytes) -> ElfFile:
    buf = bytearray(data)
    off = elf.va_to_offset(va)
    buf[off:off + len(payload)] = payload
    return ElfFile.from_bytes(bytes(buf))


class Ps3FixtureTest(unittest.TestCase):
    def setUp(self):
        self.data = fx.build_ps3_fixture()
        self.elf = ElfFile.from_bytes(self.data)

    def test_entry_via_opd(self):
        entry = cell.parse_entry(self.elf)
        self.assertEqual(entry, cell.EntryInfo("opd", 0x20000, 0x10000, 0x28000))

    def test_entry_not_pointing_at_code(self):
        # The descriptor's func (0x20000 is data) is not executable.
        data = build_elf64_be(entry=0x20000, segments=[
            FixtureSegment(1, 0x10000, bytes(0x20), flags=5),
            FixtureSegment(1, 0x20000, struct.pack(">II", 0x20000, 0x28000), flags=6),
        ])
        entry = cell.parse_entry(ElfFile.from_bytes(data))
        self.assertEqual(entry, cell.EntryInfo("code", None, 0x20000, None))

    def test_entry_unreadable(self):
        data = build_elf64_be(entry=0x10000, segments=[
            FixtureSegment(1, 0x10000, bytes(4), flags=5),
        ])
        entry = cell.parse_entry(ElfFile.from_bytes(data))
        self.assertEqual(entry.kind, "code")
        self.assertEqual(entry.func, 0x10000)

    def test_opd_section(self):
        self.assertEqual(cell.parse_opd_section(self.elf),
                         [(0x10000, 0x28000), (0x10018, 0x28000)])

    def test_opd_section_absent_or_without_data(self):
        plain = ElfFile.from_bytes(build_elf64_be(entry=0, segments=[]))
        self.assertIsNone(cell.parse_opd_section(plain))

    def test_proc_param(self):
        pp = cell.parse_proc_param(self.elf)
        self.assertEqual(pp["addr"], 0x20300)
        self.assertEqual(pp["size"], 0x20)
        self.assertEqual(pp["magic"], 0x13BCC5F6)
        self.assertEqual(pp["version"], 0x00330000)
        self.assertEqual(pp["sdk_version"], 0x00440001)
        self.assertEqual(pp["primary_prio"], 1000)
        self.assertEqual(pp["primary_stacksize"], 0x10000)
        self.assertEqual(pp["malloc_pagesize"], 0x100000)
        self.assertEqual(pp["ppc_seg"], 0)
        self.assertEqual(len(pp["raw"]), 0x40)
        self.assertTrue(pp["raw"].startswith("0000002013bcc5f6"))

    def test_prx_param(self):
        px = cell.parse_prx_param(self.elf)
        self.assertEqual(px["addr"], 0x20340)
        self.assertEqual(px["size"], 0x28)
        self.assertEqual(px["magic"], 0x1B434CEC)
        self.assertEqual(px["version"], 4)
        self.assertEqual((px["libent_start"], px["libent_end"]), (0x20010, 0x2002C))
        self.assertEqual((px["libstub_start"], px["libstub_end"]), (0x20030, 0x20088))
        self.assertEqual(px["ver"], 1)

    def test_params_absent(self):
        plain = ElfFile.from_bytes(build_elf64_be(entry=0, segments=[]))
        self.assertIsNone(cell.parse_proc_param(plain))
        self.assertIsNone(cell.parse_prx_param(plain))
        self.assertIsNone(cell.parse_tls(plain))

    def test_imports(self):
        prx = cell.parse_prx_param(self.elf)
        mods = cell.parse_imports(self.elf, prx)
        self.assertEqual([m.name for m in mods], ["cellSysutil", "cellSpurs"])
        self.assertEqual([m.entry_addr for m in mods], [0x20030, 0x2005C])
        self.assertEqual([m.attributes for m in mods], [9, 9])
        self.assertEqual([m.version for m in mods], [1, 1])
        self.assertEqual([f.fnid for f in mods[0].functions], [0x0BAE8772, 0x9D98AFA0])
        self.assertEqual([f.fnid for f in mods[1].functions], [0xACFC8DBC])
        slots = [f.slot_addr for m in mods for f in m.functions]
        self.assertEqual(slots, [0x20150, 0x20154, 0x20158])
        self.assertTrue(all(f.slot_value == 0 for m in mods for f in m.functions))
        self.assertEqual(mods[0].variables, [])
        self.assertEqual(mods[0].num_tlsvar, 0)

    def test_exports(self):
        prx = cell.parse_prx_param(self.elf)
        mods = cell.parse_exports(self.elf, prx)
        self.assertEqual(len(mods), 1)
        mod = mods[0]
        self.assertIsNone(mod.name)
        self.assertEqual(mod.attributes, 0x8000)
        self.assertEqual(mod.entry_addr, 0x20010)
        self.assertEqual(mod.functions,
                         [cell.ExportFunc(0xBC9A0086, 0x20000, 0x10000, 0x28000)])

    def test_export_with_unreadable_opd(self):
        prx = cell.parse_prx_param(self.elf)
        elf = _patched(self.data, self.elf, 0x20104, struct.pack(">I", 0x90000))
        func = cell.parse_exports(elf, prx)[0].functions[0]
        self.assertEqual((func.opd_addr, func.func, func.toc), (0x90000, None, None))

    def test_tls(self):
        self.assertEqual(cell.parse_tls(self.elf), {
            "vaddr": 0x20180, "offset": self.elf.segments[4].p_offset,
            "filesz": 0, "memsz": 0x10, "align": 0x10})

    def test_zero_sized_libstub_entry(self):
        prx = cell.parse_prx_param(self.elf)
        elf = _patched(self.data, self.elf, 0x2005C, b"\0")
        with self.assertRaises(ElfError):
            cell.parse_imports(elf, prx)

    def test_zero_sized_libent_entry(self):
        prx = cell.parse_prx_param(self.elf)
        elf = _patched(self.data, self.elf, 0x20010, b"\0")
        with self.assertRaises(ElfError):
            cell.parse_exports(elf, prx)

    def test_unknown_layout_entry_is_recorded_and_skipped(self):
        prx = cell.parse_prx_param(self.elf)
        # A 0x1C-sized record at the libstub start is not a known import layout.
        elf = _patched(self.data, self.elf, 0x20030, b"\x1c")
        prx = dict(prx, libstub_end=0x20030 + 0x1C)
        mods = cell.parse_imports(elf, prx)
        self.assertEqual(len(mods), 1)
        self.assertTrue(mods[0].unknown_layout)
        self.assertEqual(mods[0].size, 0x1C)
        self.assertEqual(mods[0].functions, [])


BASE = 0x20000


class PrxFixtureTest(unittest.TestCase):
    def setUp(self):
        self.relocated = ElfFile.from_bytes(fx.build_prx_fixture(True, BASE))
        self.unrelocated = ElfFile.from_bytes(fx.build_prx_fixture(False, BASE))

    def test_type_and_unapplied_relocations(self):
        self.assertTrue(cell.is_prx(self.relocated))
        self.assertTrue(cell.is_prx(self.unrelocated))
        self.assertFalse(cell.has_unapplied_relocations(self.relocated))
        self.assertTrue(cell.has_unapplied_relocations(self.unrelocated))
        ps3 = ElfFile.from_bytes(fx.build_ps3_fixture())
        self.assertFalse(cell.is_prx(ps3))
        self.assertFalse(cell.has_unapplied_relocations(ps3))

    def test_module_info(self):
        info = cell.parse_module_info(self.relocated)
        self.assertEqual(info, cell.ModuleInfo(
            BASE + fx.PRX_MODULE_INFO, 0, 1, 1, fx.PRX_NAME, BASE + fx.PRX_TOC,
            BASE + fx.PRX_LIBENT[0], BASE + fx.PRX_LIBENT[1],
            BASE + fx.PRX_LIBSTUB[0], BASE + fx.PRX_LIBSTUB[1]))

    def test_module_info_of_the_unrelocated_file_uses_link_addresses(self):
        info = cell.parse_module_info(self.unrelocated)
        self.assertEqual((info.addr, info.toc), (fx.PRX_MODULE_INFO, fx.PRX_TOC))

    def test_module_info_is_none_for_an_executable(self):
        self.assertIsNone(cell.parse_module_info(ElfFile.from_bytes(fx.build_ps3_fixture())))

    def test_module_info_outside_the_code_segment(self):
        data = bytearray(fx.build_prx_fixture(True, BASE))
        # Program header 0 starts at 64; its p_paddr is at +24.
        struct.pack_into(">Q", data, 64 + 24, 1 << 20)
        with self.assertRaises(ElfError):
            cell.parse_module_info(ElfFile.from_bytes(bytes(data)))

    def test_module_info_needs_a_load_segment_first(self):
        data = bytearray(fx.build_prx_fixture(True, BASE))
        struct.pack_into(">I", data, 64, 0)
        with self.assertRaises(ElfError):
            cell.parse_module_info(ElfFile.from_bytes(bytes(data)))

    def test_library_tables_come_from_the_module_info(self):
        self.assertEqual(cell.library_tables(self.relocated), {
            "libent_start": BASE + fx.PRX_LIBENT[0], "libent_end": BASE + fx.PRX_LIBENT[1],
            "libstub_start": BASE + fx.PRX_LIBSTUB[0], "libstub_end": BASE + fx.PRX_LIBSTUB[1]})

    def test_library_tables_of_an_executable_are_its_prx_param(self):
        elf = ElfFile.from_bytes(fx.build_ps3_fixture())
        self.assertEqual(cell.library_tables(elf), cell.parse_prx_param(elf))
        plain = ElfFile.from_bytes(build_elf64_be(entry=0, segments=[]))
        self.assertIsNone(cell.library_tables(plain))

    def test_imports(self):
        mods = cell.parse_imports(self.relocated, cell.library_tables(self.relocated))
        self.assertEqual([m.name for m in mods], ["fixImp"])
        funcs = mods[0].functions
        self.assertEqual([f.fnid for f in funcs], list(fx.PRX_FNIDS))
        self.assertEqual([f.slot_addr for f in funcs], [BASE + s for s in fx.PRX_SLOTS])
        self.assertEqual([f.slot_value for f in funcs], [BASE + t for t in fx.PRX_THUNKS])

    def test_exports(self):
        mods = cell.parse_exports(self.relocated, cell.library_tables(self.relocated))
        self.assertEqual(len(mods), 1)
        self.assertEqual(mods[0].name, "fixExp")
        self.assertEqual(mods[0].functions, [cell.ExportFunc(
            fx.PRX_EXPORT_FNID, BASE + fx.PRX_OPDS[0], BASE + fx.PRX_FUNCS[0],
            BASE + fx.PRX_TOC)])

    def test_entry_is_the_module_toc(self):
        self.assertEqual(cell.parse_entry(self.relocated),
                         cell.EntryInfo("prx", None, None, BASE + fx.PRX_TOC))

    def test_text_and_code_ranges_exclude_and_include_the_thunks(self):
        self.assertEqual(cell.find_text_range(self.relocated),
                         (BASE, BASE + fx.PRX_THUNKS_END))
        self.assertEqual(cell.find_code_range(self.relocated),
                         (BASE, BASE + fx.PRX_CODE_END))

    def test_ranges_without_imports_are_the_code_segment(self):
        data = bytearray(fx.build_prx_fixture(True, BASE))
        elf = ElfFile.from_bytes(bytes(data))
        info = cell.parse_module_info(elf)
        # imports_end sits 4 words into the module info after the name.
        struct.pack_into(">I", data, elf.va_to_offset(info.addr) + 0x30, info.imports_start)
        bare = ElfFile.from_bytes(bytes(data))
        end = BASE + fx.PRX_SEG0_SIZE
        self.assertEqual(cell.find_text_range(bare), (BASE, end))
        self.assertEqual(cell.find_code_range(bare), (BASE, end))

    def test_segment_for_va_prefers_the_segment_holding_the_file_data(self):
        elf = self.unrelocated
        # Segment 0's p_memsz reaches 0x100 bytes into segment 1.
        self.assertEqual(elf.segment_for_va(fx.PRX_SEG1 - 4).index, 0)
        self.assertEqual(elf.segment_for_va(fx.PRX_SEG1).index, 1)
        self.assertEqual(elf.read_u32_va(fx.PRX_SLOTS[0]), fx.PRX_THUNKS[0])
        self.assertEqual(elf.read_u32_va(fx.PRX_OPDS[0]), fx.PRX_FUNCS[0])
        # Past segment 1's file data, its zero-filled tail still wins over the overlap.
        self.assertEqual(elf.segment_for_va(fx.PRX_SEG1 + fx.PRX_SEG1_SIZE + 0x10).index, 1)
        self.assertEqual(elf.read_u32_va(fx.PRX_SEG1 + fx.PRX_SEG1_SIZE + 0x10), 0)
        # Beyond segment 1 only segment 0's memory size covers the address.
        self.assertEqual(elf.segment_for_va(fx.PRX_SEG1 + 0xF0).index, 0)
        self.assertIsNone(elf.segment_for_va(fx.PRX_SEG1 + 0x200))

    def test_unrelocated_file_reads_through_its_overlap(self):
        info = cell.parse_module_info(self.unrelocated)
        tables = cell.library_tables(self.unrelocated)
        mods = cell.parse_imports(self.unrelocated, tables)
        self.assertEqual(info.name, fx.PRX_NAME)
        self.assertEqual([f.slot_value for f in mods[0].functions], list(fx.PRX_THUNKS))
        self.assertEqual(cell.find_code_range(self.unrelocated), (0, fx.PRX_CODE_END))

    def test_thunks_have_the_libsre_shape(self):
        words = struct.unpack(">8I", self.relocated.read_va(BASE + fx.PRX_THUNKS[0], 32))
        self.assertEqual(words[2], 0xF8410028)          # std r2,40(r1)
        self.assertEqual(words[6], 0x4E800420)          # bctr


class FindTextRangeTest(unittest.TestCase):
    TEXT_VA = 0x10000

    def _elf(self, sections, text_size=0x40, with_sections=True):
        data = build_elf64_be(
            entry=0x10000,
            segments=[FixtureSegment(1, self.TEXT_VA, bytes(text_size), flags=5),
                      FixtureSegment(1, 0x20000, bytes(0x40), flags=6)],
            sections=sections if with_sections else None)
        return ElfFile.from_bytes(data)

    def test_canonical_fixture(self):
        elf = ElfFile.from_bytes(fx.build_ps3_fixture())
        self.assertEqual(cell.find_text_range(elf), (0x10000, 0x10020))

    def test_without_sections_uses_exec_segment(self):
        elf = self._elf(None, text_size=0x22, with_sections=False)
        self.assertEqual(elf.sections, [])
        self.assertEqual(cell.find_text_range(elf), (0x10000, 0x10020))

    def test_largest_section_wins(self):
        elf = self._elf([FixtureSection("", 1, 0x10000, 0x10, flags=4),
                         FixtureSection("", 1, 0x10010, 0x20, flags=6)])
        self.assertEqual(cell.find_text_range(elf), (0x10010, 0x10030))

    def test_tie_prefers_lowest_address(self):
        elf = self._elf([FixtureSection("", 1, 0x10020, 0x10, flags=4),
                         FixtureSection("", 1, 0x10000, 0x10, flags=4)])
        self.assertEqual(cell.find_text_range(elf), (0x10000, 0x10010))

    def test_ignores_non_exec_nobits_and_outside_sections(self):
        elf = self._elf([FixtureSection("", 1, 0x10000, 0x30, flags=2),
                         FixtureSection("", 8, 0x10000, 0x38, flags=6),
                         FixtureSection("", 1, 0x20000, 0x40, flags=6),
                         FixtureSection("", 1, 0x90000, 0x1000, flags=6)])
        # No usable section: falls back to the whole exec segment.
        self.assertEqual(cell.find_text_range(elf), (0x10000, 0x10040))

    def test_no_exec_segment(self):
        data = build_elf64_be(entry=0, segments=[FixtureSegment(1, 0x20000, bytes(8))])
        with self.assertRaises(ElfError):
            cell.find_text_range(ElfFile.from_bytes(data))


class FindCodeRangeTest(unittest.TestCase):
    def _elf(self, sections, text_size=0x80):
        data = build_elf64_be(
            entry=0x10000,
            segments=[FixtureSegment(1, 0x10000, bytes(text_size), flags=5),
                      FixtureSegment(1, 0x20000, bytes(0x40), flags=6)],
            sections=sections)
        return ElfFile.from_bytes(data)

    def test_single_section_is_the_text_range(self):
        elf = ElfFile.from_bytes(fx.build_ps3_fixture())
        self.assertEqual(cell.find_code_range(elf), cell.find_text_range(elf))

    def test_without_sections_is_the_text_range(self):
        elf = self._elf(None)
        self.assertEqual(cell.find_code_range(elf), (0x10000, 0x10080))

    def test_adjoining_sections_are_included(self):
        # The gap before .text is smaller than its alignment, so it still adjoins.
        elf = self._elf([FixtureSection("", 1, 0x10000, 0x2c, flags=6, align=4),
                         FixtureSection("", 1, 0x10030, 0x40, flags=6, align=16),
                         FixtureSection("", 1, 0x10070, 0x10, flags=6, align=4)])
        self.assertEqual(cell.find_text_range(elf), (0x10030, 0x10070))
        self.assertEqual(cell.find_code_range(elf), (0x10000, 0x10080))

    def test_distant_sections_are_not_included(self):
        elf = self._elf([FixtureSection("", 1, 0x10000, 0x8, flags=6, align=4),
                         FixtureSection("", 1, 0x10020, 0x40, flags=6, align=16),
                         FixtureSection("", 1, 0x10068, 0x8, flags=6, align=4)])
        self.assertEqual(cell.find_code_range(elf), (0x10020, 0x10060))

    def test_chain_of_adjoining_sections(self):
        elf = self._elf([FixtureSection("", 1, 0x10000, 0x10, flags=6, align=4),
                         FixtureSection("", 1, 0x10010, 0x10, flags=6, align=4),
                         FixtureSection("", 1, 0x10020, 0x40, flags=6, align=4),
                         FixtureSection("", 1, 0x10060, 0x10, flags=6, align=4),
                         FixtureSection("", 1, 0x10070, 0x10, flags=6, align=4)])
        self.assertEqual(cell.find_code_range(elf), (0x10000, 0x10080))


@unittest.skipUnless(os.environ.get("TOX2_EBOOT_ELF"), "TOX2_EBOOT_ELF is not set")
class RealEbootCellTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ["TOX2_EBOOT_ELF"]
        if not os.path.isfile(path):
            raise unittest.SkipTest("TOX2_EBOOT_ELF does not point to a file")
        cls.elf = ElfFile.from_path(path)

    def test_text_range(self):
        self.assertEqual(cell.find_text_range(self.elf), (0x00010240, 0x00C8F3D4))

    def test_code_range_adds_init_and_fini_but_not_the_stubs(self):
        self.assertEqual(cell.find_code_range(self.elf), (0x00010200, 0x00C8F408))

    def test_entry(self):
        entry = cell.parse_entry(self.elf)
        self.assertEqual(entry.kind, "opd")
        self.assertEqual(entry.opd_addr, 0x0105DDB8)
        self.assertEqual(entry.func, 0x00010240)
        self.assertEqual(entry.toc, 0x010B6330)

    def test_opd_section_absent(self):
        self.assertIsNone(cell.parse_opd_section(self.elf))

    def test_params_and_tls(self):
        self.assertEqual(cell.parse_proc_param(self.elf)["addr"], 0x0104A7C0)
        prx = cell.parse_prx_param(self.elf)
        self.assertEqual(prx["addr"], 0x0104A7E8)
        self.assertEqual((prx["libstub_start"], prx["libstub_end"]),
                         (0x00E53164, 0x00E53450))
        tls = cell.parse_tls(self.elf)
        self.assertEqual((tls["vaddr"], tls["filesz"], tls["memsz"]),
                         (0x010C99A4, 4, 0x1B4))

    def test_imports(self):
        prx = cell.parse_prx_param(self.elf)
        mods = cell.parse_imports(self.elf, prx)
        counts = {m.name: len(m.functions) for m in mods}
        self.assertEqual(len(mods), 17)
        self.assertEqual(sum(counts.values()), 173)
        self.assertEqual(counts["sys_io"], 6)
        self.assertEqual(counts["cellSpurs"], 30)
        self.assertEqual(counts["cellSysutil"], 23)
        self.assertEqual(counts["sysPrxForUser"], 20)
        self.assertFalse(any(m.unknown_layout for m in mods))

    def test_exports_parse(self):
        prx = cell.parse_prx_param(self.elf)
        cell.parse_exports(self.elf, prx)


if __name__ == "__main__":
    unittest.main()

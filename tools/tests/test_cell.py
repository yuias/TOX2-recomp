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

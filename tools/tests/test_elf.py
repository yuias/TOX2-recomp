import struct
import unittest

from ps3recomp import elf
from ps3recomp.elf import ElfError, ElfFile

from tests.elf_fixture import (FixtureSection, FixtureSegment, build_elf32_be,
                               build_elf64_be)

CODE_VA = 0x10000
DATA_VA = 0x100000
CODE = bytes(range(0x40))
DATA = b"hello\0" + b"\xff" * 10 + b"\xc3\xa9\0" + bytes(5)

# Offsets of e_shoff / e_shnum inside the 64-bit ELF header.
_E_SHOFF = 40
_E_SHNUM = 60


def _segments():
    return [
        FixtureSegment(elf.PT_LOAD, CODE_VA, CODE, flags=elf.PF_R | elf.PF_X),
        FixtureSegment(elf.PT_LOAD, DATA_VA, DATA, memsz=0x100,
                       flags=elf.PF_R | elf.PF_W),
    ]


def _sections():
    return [
        FixtureSection(".text", 1, CODE_VA, len(CODE), flags=6),
        FixtureSection("", 1, DATA_VA, 4),
        FixtureSection(".bss", elf.SHT_NOBITS, DATA_VA + 0x80, 0x40),
        FixtureSection(".ghost", 1, 0x900000, 0x20),
    ]


def _build(sections=None):
    return build_elf64_be(entry=0x100100, segments=_segments(), sections=sections)


class HeaderTest(unittest.TestCase):
    def test_fields(self):
        f = ElfFile.from_bytes(_build())
        self.assertEqual(f.ei_class, elf.ELFCLASS64)
        self.assertEqual(f.osabi, 0x66)
        self.assertEqual(f.e_type, 2)
        self.assertEqual(f.e_machine, elf.EM_PPC64)
        self.assertEqual(f.e_entry, 0x100100)
        self.assertEqual(f.sections, [])

    def test_is_self(self):
        self.assertTrue(elf.is_self(b"SCE\0" + bytes(16)))
        self.assertFalse(elf.is_self(_build()))

    def test_bad_magic(self):
        data = bytearray(_build())
        data[0] = 0
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(bytes(data))

    def test_self_input_rejected(self):
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(b"SCE\0" + bytes(64))

    def test_little_endian_rejected(self):
        data = bytearray(_build())
        data[5] = 1
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(bytes(data))

    def test_unknown_class_rejected(self):
        data = bytearray(_build())
        data[4] = 3
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(bytes(data))

    def test_truncated_ehdr(self):
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(_build()[:40])

    def test_empty_input(self):
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(b"")

    def test_from_path(self):
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x.bin")
            with open(path, "wb") as fh:
                fh.write(_build())
            self.assertEqual(ElfFile.from_path(path).e_entry, 0x100100)


class SegmentTest(unittest.TestCase):
    def setUp(self):
        self.f = ElfFile.from_bytes(_build())

    def test_segments(self):
        self.assertEqual(len(self.f.segments), 2)
        code, data = self.f.segments
        self.assertEqual((code.index, code.p_type, code.p_flags), (0, 1, 5))
        self.assertEqual((code.p_vaddr, code.p_filesz, code.p_memsz),
                         (CODE_VA, len(CODE), len(CODE)))
        self.assertEqual((data.p_vaddr, data.p_filesz, data.p_memsz, data.p_flags),
                         (DATA_VA, len(DATA), 0x100, 6))
        self.assertEqual(code.p_offset % 16, 0)
        self.assertEqual(data.p_offset % 16, 0)
        self.assertGreaterEqual(code.p_offset, 64 + 2 * 56)

    def test_loads_and_exec(self):
        self.assertEqual([s.index for s in self.f.loads()], [0, 1])
        self.assertEqual([s.index for s in self.f.exec_segments()], [0])

    def test_non_load_segment_ignored(self):
        data = build_elf64_be(entry=0, segments=[
            FixtureSegment(elf.PT_TLS, 0x2000, b"abcd", flags=elf.PF_R),
            FixtureSegment(elf.PT_LOAD, 0x1000, b"wxyz"),
        ])
        f = ElfFile.from_bytes(data)
        self.assertEqual(len(f.segments), 2)
        self.assertEqual([s.index for s in f.loads()], [1])
        self.assertIsNone(f.segment_for_va(0x2000))

    def test_segment_for_va(self):
        self.assertEqual(self.f.segment_for_va(CODE_VA).index, 0)
        self.assertEqual(self.f.segment_for_va(CODE_VA + len(CODE) - 1).index, 0)
        self.assertIsNone(self.f.segment_for_va(CODE_VA + len(CODE)))
        self.assertEqual(self.f.segment_for_va(DATA_VA + 0xFF).index, 1)
        self.assertIsNone(self.f.segment_for_va(DATA_VA + 0x100))
        self.assertIsNone(self.f.segment_for_va(0))


class ReadTest(unittest.TestCase):
    def setUp(self):
        self.f = ElfFile.from_bytes(_build())

    def test_va_to_offset(self):
        code = self.f.segments[0]
        self.assertEqual(self.f.va_to_offset(CODE_VA), code.p_offset)
        self.assertEqual(self.f.va_to_offset(CODE_VA + 4), code.p_offset + 4)
        self.assertEqual(self.f.data[self.f.va_to_offset(CODE_VA + 5)], 5)

    def test_va_to_offset_bss_and_unmapped(self):
        self.assertIsNone(self.f.va_to_offset(DATA_VA + len(DATA)))  # bss
        self.assertIsNone(self.f.va_to_offset(0x500000))

    def test_read_va(self):
        self.assertEqual(self.f.read_va(CODE_VA + 2, 3), CODE[2:5])
        self.assertEqual(self.f.read_va(CODE_VA, 0), b"")

    def test_read_bss_zeros(self):
        self.assertEqual(self.f.read_va(DATA_VA + 0x80, 8), bytes(8))
        # Straddles filesz into memsz.
        out = self.f.read_va(DATA_VA + len(DATA) - 2, 6)
        self.assertEqual(out, DATA[-2:] + bytes(4))
        self.assertEqual(len(self.f.read_va(DATA_VA, 0x100)), 0x100)

    def test_read_outside(self):
        for va, size in ((0x500000, 4), (CODE_VA + len(CODE) - 2, 4),
                         (DATA_VA + 0xFC, 8), (CODE_VA, -1)):
            with self.subTest(va=va, size=size), self.assertRaises(ElfError):
                self.f.read_va(va, size)

    def test_read_ints(self):
        self.assertEqual(self.f.read_u16_va(CODE_VA + 1), 0x0102)
        self.assertEqual(self.f.read_u32_va(CODE_VA + 4), 0x04050607)

    def test_read_cstr(self):
        self.assertEqual(self.f.read_cstr_va(DATA_VA), "hello")
        self.assertEqual(self.f.read_cstr_va(DATA_VA + 5), "")

    def test_read_cstr_errors(self):
        with self.assertRaises(ElfError):
            self.f.read_cstr_va(DATA_VA, max_len=5)  # NUL is the 6th byte
        self.assertEqual(self.f.read_cstr_va(DATA_VA, max_len=6), "hello")
        with self.assertRaises(ElfError):
            self.f.read_cstr_va(DATA_VA + 16)  # non-ASCII bytes
        with self.assertRaises(ElfError):
            self.f.read_cstr_va(0x500000)

    def test_read_cstr_unterminated_at_segment_end(self):
        data = build_elf64_be(entry=0, segments=[
            FixtureSegment(elf.PT_LOAD, 0x1000, b"abc")])
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(data).read_cstr_va(0x1000)

    def test_truncated_segment_data(self):
        data = _build()
        f = ElfFile.from_bytes(data[:-20])
        with self.assertRaises(ElfError):
            f.read_va(DATA_VA, len(DATA))


class SectionTest(unittest.TestCase):
    def setUp(self):
        self.data = _build(_sections())
        self.f = ElfFile.from_bytes(self.data)

    def test_names(self):
        names = [s.name for s in self.f.sections]
        self.assertEqual(names, ["", ".text", "", ".bss", ".ghost", ".shstrtab"])

    def test_empty_name_is_not_none(self):
        self.assertEqual(self.f.sections[2].name, "")

    def test_fields(self):
        text = self.f.sections[1]
        self.assertEqual((text.index, text.sh_type, text.sh_flags, text.sh_addr,
                          text.sh_size), (1, 1, 6, CODE_VA, len(CODE)))
        self.assertEqual(text.sh_offset, self.f.segments[0].p_offset)
        self.assertTrue(text.data_present)

    def test_nobits_present(self):
        self.assertTrue(self.f.sections[3].data_present)

    def test_missing_data(self):
        ghost = self.f.sections[4]
        self.assertFalse(ghost.data_present)
        self.assertGreater(ghost.sh_offset + ghost.sh_size, len(self.data))

    def test_null_section(self):
        null = self.f.sections[0]
        self.assertEqual((null.sh_type, null.sh_size, null.data_present), (0, 0, True))

    def test_shnum_zero(self):
        data = bytearray(self.data)
        struct.pack_into(">H", data, _E_SHNUM, 0)
        f = ElfFile.from_bytes(bytes(data))
        self.assertEqual(f.sections, [])
        self.assertEqual(len(f.segments), 2)

    def test_shdr_table_truncated(self):
        shoff = struct.unpack_from(">Q", self.data, _E_SHOFF)[0]
        f = ElfFile.from_bytes(self.data[:shoff + 10])
        self.assertEqual(f.sections, [])
        self.assertEqual(len(f.segments), 2)
        self.assertEqual(f.image_extent(), max(s.p_offset + s.p_filesz
                                               for s in f.segments))

    def test_strtab_missing_gives_none_names(self):
        # Cut the file inside .shstrtab's data region by pointing shstrndx at
        # a section whose data is not present.
        data = bytearray(self.data)
        struct.pack_into(">H", data, _E_SHNUM + 2, 4)  # e_shstrndx -> .ghost
        f = ElfFile.from_bytes(bytes(data))
        self.assertEqual(len(f.sections), 6)
        self.assertTrue(all(s.name is None for s in f.sections))

    def test_shstrndx_out_of_range(self):
        data = bytearray(self.data)
        struct.pack_into(">H", data, _E_SHNUM + 2, 99)
        f = ElfFile.from_bytes(bytes(data))
        self.assertTrue(all(s.name is None for s in f.sections))


class RobustnessTest(unittest.TestCase):
    def test_phdr_table_truncated(self):
        data = _build()
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(data[:64 + 56 + 10])

    def test_phentsize_too_small(self):
        data = bytearray(_build())
        struct.pack_into(">H", data, 54, 16)  # e_phentsize
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(bytes(data))

    def test_no_program_headers(self):
        f = ElfFile.from_bytes(build_elf64_be(entry=0, segments=[]))
        self.assertEqual(f.segments, [])
        self.assertEqual(f.image_extent(), 64)

    def test_base_outside_buffer(self):
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(_build(), base=10_000_000)


class Elf32Test(unittest.TestCase):
    def test_parse_at_nonzero_base(self):
        image = build_elf32_be(entry=0x80, segments=[
            FixtureSegment(elf.PT_LOAD, 0x0, b"\x01\x02\x03\x04" * 8,
                           flags=elf.PF_R | elf.PF_X),
            FixtureSegment(elf.PT_LOAD, 0x3000, b"spu\0", memsz=0x20,
                           flags=elf.PF_R | elf.PF_W),
        ])
        prefix = b"\xaa" * 0x123
        buf = prefix + image + b"trailing"
        f = ElfFile.from_bytes(buf, base=len(prefix))

        self.assertEqual(f.ei_class, elf.ELFCLASS32)
        self.assertEqual(f.e_machine, elf.EM_SPU)
        self.assertEqual(f.e_entry, 0x80)
        self.assertEqual(f.osabi, 0)
        self.assertEqual([s.p_flags for s in f.segments], [5, 6])
        self.assertEqual(f.segments[1].p_memsz, 0x20)
        self.assertEqual(f.exec_segments()[0].index, 0)

        self.assertEqual(f.va_to_offset(0), len(prefix) + f.segments[0].p_offset)
        self.assertEqual(f.read_va(0, 4), b"\x01\x02\x03\x04")
        self.assertEqual(f.read_u32_va(4), 0x01020304)
        self.assertEqual(f.read_cstr_va(0x3000), "spu")
        self.assertEqual(f.read_va(0x3010, 4), bytes(4))
        self.assertEqual(f.image_extent(), len(image))

    def test_bad_magic_at_base(self):
        image = build_elf32_be(entry=0, segments=[])
        with self.assertRaises(ElfError):
            ElfFile.from_bytes(b"\0" * 8 + image, base=0)
        self.assertEqual(ElfFile.from_bytes(b"\0" * 8 + image, base=8).e_entry, 0)


class ImageExtentTest(unittest.TestCase):
    def test_without_sections(self):
        data = _build()
        self.assertEqual(ElfFile.from_bytes(data).image_extent(), len(data))

    def test_with_sections(self):
        data = _build(_sections())
        self.assertEqual(ElfFile.from_bytes(data).image_extent(), len(data))

    def test_trailing_data_not_counted(self):
        data = _build(_sections())
        f = ElfFile.from_bytes(data + b"\0" * 100)
        self.assertEqual(f.image_extent(), len(data))

    def test_extent_relative_to_base(self):
        data = _build()
        f = ElfFile.from_bytes(b"\x11" * 32 + data, base=32)
        self.assertEqual(f.image_extent(), len(data))


if __name__ == "__main__":
    unittest.main()

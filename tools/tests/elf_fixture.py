"""Builders for synthetic big-endian ELF images used by the unit tests."""

from __future__ import annotations

import struct
from dataclasses import dataclass

PT_LOAD = 1

_SHT_STRTAB = 3


@dataclass
class FixtureSegment:
    p_type: int
    vaddr: int
    data: bytes
    memsz: int | None = None
    flags: int = 4
    align: int = 0x10


@dataclass
class FixtureSection:
    name: str
    sh_type: int
    addr: int
    size: int
    flags: int = 0
    align: int = 1


def _align(value: int, alignment: int = 16) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def _ident(ei_class: int, osabi: int) -> bytes:
    return bytes([0x7F, 0x45, 0x4C, 0x46, ei_class, 2, 1, osabi]) + bytes(8)


def _layout_segments(segments: list[FixtureSegment], data_start: int) -> list[int]:
    offsets = []
    pos = data_start
    for seg in segments:
        pos = _align(pos)
        offsets.append(pos)
        pos += len(seg.data)
    return offsets


def build_elf64_be(*, entry: int, segments: list[FixtureSegment],
                   sections: list[FixtureSection] | None = None,
                   e_type: int = 2, machine: int = 21, osabi: int = 0x66) -> bytes:
    ehsize, phentsize, shentsize = 64, 56, 64
    phnum = len(segments)
    offsets = _layout_segments(segments, ehsize + phentsize * phnum)

    buf = bytearray(ehsize + phentsize * phnum)
    for seg, off in zip(segments, offsets):
        buf.extend(bytes(off - len(buf)))
        buf.extend(seg.data)

    shoff = shnum = shstrndx = 0
    if sections is not None:
        # Name offsets into .shstrtab; the leading NUL gives the null section "".
        strtab = bytearray(b"\0")
        name_offs = []
        for sec in sections:
            if sec.name:
                name_offs.append(len(strtab))
                strtab.extend(sec.name.encode("ascii") + b"\0")
            else:
                name_offs.append(0)
        strtab_name = len(strtab)
        strtab.extend(b".shstrtab\0")

        strtab_off = _align(len(buf))
        buf.extend(bytes(strtab_off - len(buf)))
        buf.extend(strtab)
        shoff = _align(len(buf))
        buf.extend(bytes(shoff - len(buf)))
        shnum = len(sections) + 2
        shstrndx = shnum - 1
        buf.extend(bytes(shentsize * shnum))
        missing_off = len(buf) + 0x1000

        def section_offset(addr: int) -> int:
            for seg, off in zip(segments, offsets):
                if seg.p_type == PT_LOAD and seg.vaddr <= addr < seg.vaddr + len(seg.data):
                    return off + (addr - seg.vaddr)
            return missing_off

        rows = [struct.pack(">IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
        for sec, name_off in zip(sections, name_offs):
            rows.append(struct.pack(">IIQQQQIIQQ", name_off, sec.sh_type, sec.flags,
                                    sec.addr, section_offset(sec.addr), sec.size,
                                    0, 0, sec.align, 0))
        rows.append(struct.pack(">IIQQQQIIQQ", strtab_name, _SHT_STRTAB, 0, 0,
                                strtab_off, len(strtab), 0, 0, 1, 0))
        buf[shoff:shoff + shentsize * shnum] = b"".join(rows)

    ehdr = struct.pack(">16sHHIQQQIHHHHHH", _ident(2, osabi), e_type, machine, 1, entry,
                       ehsize if phnum else 0, shoff, 0, ehsize, phentsize, phnum,
                       shentsize, shnum, shstrndx)
    buf[0:ehsize] = ehdr
    for i, (seg, off) in enumerate(zip(segments, offsets)):
        memsz = seg.memsz if seg.memsz is not None else len(seg.data)
        phdr = struct.pack(">IIQQQQQQ", seg.p_type, seg.flags, off, seg.vaddr, seg.vaddr,
                           len(seg.data), memsz, seg.align)
        buf[ehsize + i * phentsize:ehsize + (i + 1) * phentsize] = phdr
    return bytes(buf)


PS3_ENTRY = 0x20000
PS3_TEXT_VA = 0x10000
PS3_DATA_VA = 0x20000
PS3_TOC = 0x28000
PS3_OPD0 = 0x20000
PS3_OPD1 = 0x20008
PS3_LIBENT_VA = 0x20010
PS3_LIBSTUB0_VA = 0x20030
PS3_LIBSTUB1_VA = 0x2005C
PS3_LIBSTUB_END = 0x20088
PS3_PROC_PARAM_VA = 0x20300
PS3_PRX_PARAM_VA = 0x20340
PS3_TLS_VA = 0x20180
PS3_SLOT_ADDRS = (0x20150, 0x20154, 0x20158)
PS3_FNIDS = (0x0BAE8772, 0x9D98AFA0, 0xACFC8DBC)
PS3_EXPORT_FNID = 0xBC9A0086

_PS3_TEXT_WORDS = (0x7C0802A6, 0xF821FF91, 0x3960008D, 0x44000002,
                   0x38210070, 0x7C0803A6, 0x4E800020, 0x00000000)


def build_ps3_fixture(with_spu: bool = False) -> bytes:
    """Canonical PS3-style image: OPDs, one export, two imports, param segments, TLS.

    With `with_spu`, the data segment also holds an embedded SPU ELF at vaddr 0x20200
    followed by a truncated ELF header.
    """
    text = struct.pack(">8I", *_PS3_TEXT_WORDS)

    data = bytearray(0x200)

    def put(offset: int, fmt: str, *values) -> None:
        struct.pack_into(fmt, data, offset, *values)

    put(0x000, ">II", 0x10000, PS3_TOC)
    put(0x008, ">II", 0x10018, PS3_TOC)
    put(0x010, ">BBHHHHHBB2sIII", 0x1C, 0, 1, 0x8000, 1, 0, 0, 0, 0, b"\0\0",
        0, 0x20100, 0x20104)
    put(0x030, ">BBHHHHHBB2sIIIIIII", 0x2C, 0, 1, 0x0009, 2, 0, 0, 0, 0, b"\0\0",
        0x20120, 0x20140, 0x20150, 0, 0, 0, 0)
    put(0x05C, ">BBHHHHHBB2sIIIIIII", 0x2C, 0, 1, 0x0009, 1, 0, 0, 0, 0, b"\0\0",
        0x20130, 0x20148, 0x20158, 0, 0, 0, 0)
    put(0x100, ">II", PS3_EXPORT_FNID, 0x00020000)
    data[0x120:0x12C] = b"cellSysutil\0"
    data[0x130:0x13A] = b"cellSpurs\0"
    put(0x140, ">III", *PS3_FNIDS)

    if with_spu:
        spu = build_elf32_be(entry=0x0,
                             segments=[FixtureSegment(1, 0x0, bytes(32), flags=5)])
        data.extend(spu)
        data.extend(bytes(_align(len(data)) - len(data)))
        # A bare magic with an all-zero remainder must count as an invalid hit.
        data.extend(b"\x7fELF\x01\x02\x01\x00" + bytes(8))

    proc_param = struct.pack(">IIIIiIII", 0x20, 0x13BCC5F6, 0x00330000, 0x00440001,
                             1000, 0x10000, 0x100000, 0)
    prx_param = struct.pack(">IIIIIIIIHHI", 0x28, 0x1B434CEC, 4, 0, 0x20010, 0x2002C,
                            0x20030, PS3_LIBSTUB_END, 1, 0, 0)

    return build_elf64_be(
        entry=PS3_ENTRY,
        segments=[
            FixtureSegment(PT_LOAD, PS3_TEXT_VA, text, flags=5),
            FixtureSegment(PT_LOAD, PS3_DATA_VA, bytes(data), memsz=0x10000, flags=6),
            FixtureSegment(0x60000001, PS3_PROC_PARAM_VA, proc_param),
            FixtureSegment(0x60000002, PS3_PRX_PARAM_VA, prx_param),
            FixtureSegment(7, PS3_TLS_VA, b"", memsz=0x10),
        ],
        sections=[
            FixtureSection(".text", 1, PS3_TEXT_VA, 0x20, flags=6),
            FixtureSection(".opd", 1, PS3_DATA_VA, 0x10, flags=3),
            FixtureSection(".missing", 1, 0x90000, 0x40),
        ],
    )


def build_elf32_be(*, entry: int, segments: list[FixtureSegment],
                   e_type: int = 2, machine: int = 23) -> bytes:
    ehsize, phentsize = 52, 32
    phnum = len(segments)
    offsets = _layout_segments(segments, ehsize + phentsize * phnum)

    buf = bytearray(ehsize + phentsize * phnum)
    for seg, off in zip(segments, offsets):
        buf.extend(bytes(off - len(buf)))
        buf.extend(seg.data)

    ehdr = struct.pack(">16sHHIIIIIHHHHHH", _ident(1, 0), e_type, machine, 1, entry,
                       ehsize if phnum else 0, 0, 0, ehsize, phentsize, phnum, 40, 0, 0)
    buf[0:ehsize] = ehdr
    for i, (seg, off) in enumerate(zip(segments, offsets)):
        memsz = seg.memsz if seg.memsz is not None else len(seg.data)
        phdr = struct.pack(">IIIIIIII", seg.p_type, off, seg.vaddr, seg.vaddr,
                           len(seg.data), memsz, seg.flags, seg.align)
        buf[ehsize + i * phentsize:ehsize + (i + 1) * phentsize] = phdr
    return bytes(buf)

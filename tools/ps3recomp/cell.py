"""Parsers for PS3 (CellOS) specific structures in a decrypted PPU ELF."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .elf import SHT_NOBITS, ElfError, ElfFile, Segment

PT_TLS = 7
PT_SCE_PROC_PARAM = 0x60000001
PT_SCE_PRX_PARAM = 0x60000002

SHF_EXECINSTR = 0x4

PROC_PARAM_MAGIC =0x13BCC5F6
PRX_PARAM_MAGIC = 0x1B434CEC

IMPORT_ENTRY_SIZE = 0x2C
EXPORT_ENTRY_SIZE = 0x1C

_OPD = struct.Struct(">II")
_PROC_PARAM = struct.Struct(">IIIIiIII")
_PRX_PARAM = struct.Struct(">IIIIIIIIHHI")
_LIBSTUB = struct.Struct(">BBHHHHHBB2sIIIIIII")
# Same leading 16 bytes as a libstub entry, then name / nids / addrs.
_LIBENT = struct.Struct(">BBHHHHHBB2sIII")


@dataclass(frozen=True)
class EntryInfo:
    kind: str
    opd_addr: int | None
    func: int
    toc: int | None


@dataclass(frozen=True)
class ImportFunc:
    fnid: int
    slot_addr: int
    slot_value: int


@dataclass(frozen=True)
class ImportModule:
    entry_addr: int
    name: str
    version: int
    attributes: int
    functions: list[ImportFunc]
    variables: list[tuple[int, int]]
    num_tlsvar: int
    size: int = IMPORT_ENTRY_SIZE
    unknown_layout: bool = False


@dataclass(frozen=True)
class ExportFunc:
    fnid: int
    opd_addr: int
    func: int | None
    toc: int | None


@dataclass(frozen=True)
class ExportModule:
    entry_addr: int
    name: str | None
    version: int
    attributes: int
    functions: list[ExportFunc]
    variables: list[tuple[int, int]]
    num_tlsvar: int
    size: int = EXPORT_ENTRY_SIZE
    unknown_layout: bool = False


def _find_segment(elf: ElfFile, p_type: int) -> Segment | None:
    for s in elf.segments:
        if s.p_type == p_type:
            return s
    return None


def _read_opd(elf: ElfFile, va: int) -> tuple[int, int]:
    return _OPD.unpack(elf.read_va(va, _OPD.size))


def _in_exec_segment(elf: ElfFile, va: int) -> bool:
    return any(s.p_vaddr <= va < s.p_vaddr + s.p_memsz for s in elf.exec_segments())


def parse_entry(elf: ElfFile) -> EntryInfo:
    try:
        func, toc = _read_opd(elf, elf.e_entry)
    except ElfError:
        func = toc = None
    if func is not None and _in_exec_segment(elf, func):
        return EntryInfo("opd", elf.e_entry, func, toc)
    # e_entry already points at code (non-PS3 style); there is no TOC to report.
    return EntryInfo("code", None, elf.e_entry, None)


def _code_sections(elf: ElfFile) -> list[tuple[int, int, int]]:
    """File-backed executable sections inside an executable segment as `(start, end, align)`."""
    segments = elf.exec_segments()
    found = []
    for sec in elf.sections:
        if (not sec.sh_flags & SHF_EXECINSTR or sec.sh_type == SHT_NOBITS
                or not sec.data_present or sec.sh_size == 0):
            continue
        start, end = sec.sh_addr, sec.sh_addr + sec.sh_size
        if any(s.p_vaddr <= start and end <= s.p_vaddr + s.p_filesz for s in segments):
            found.append((start, end, max(sec.sh_addralign, 1)))
    return found


def find_text_range(elf: ElfFile) -> tuple[int, int]:
    """Return `(start, end_exclusive)` of the main code range.

    Picks the largest executable section backed by file data inside an executable
    segment (ties: lowest address). Sections are unnamed in RPCS3 output, so the name
    is never consulted. Without such a section the first executable segment is used.
    """
    best: tuple[int, int] | None = None
    for start, end, _ in _code_sections(elf):
        if best is None or (end - start, -start) > (best[1] - best[0], -best[0]):
            best = (start, end)
    if best is not None:
        return best
    segments = elf.exec_segments()
    if not segments:
        raise ElfError("no executable segment")
    seg = segments[0]
    return seg.p_vaddr, seg.p_vaddr + seg.p_filesz // 4 * 4


def find_code_range(elf: ElfFile) -> tuple[int, int]:
    """Return `(start, end_exclusive)` of the code that is recovered and emitted.

    The main code range (`find_text_range`) extended over the executable sections that
    directly adjoin it, such as the small initializer and finalizer sections. A gap
    below a section's alignment counts as adjoining. The import stub section is excluded.
    """
    text = find_text_range(elf)
    sections = _code_sections(elf)
    first = next((sec for sec in sections if sec[:2] == text), None)
    if first is None:
        return text
    stubs: list[int] = []
    prx = parse_prx_param(elf)
    if prx is not None:
        stubs = [f.slot_value for m in parse_imports(elf, prx) if not m.unknown_layout
                 for f in m.functions]
    candidates = [sec for sec in sections
                  if not any(sec[0] <= addr < sec[1] for addr in stubs)]
    low = high = first
    grown = True
    while grown:
        grown = False
        for sec in candidates:
            if 0 <= sec[0] - high[1] < sec[2] and sec[1] > high[1]:
                high = sec
                grown = True
            elif 0 <= low[0] - sec[1] < low[2] and sec[0] < low[0]:
                low = sec
                grown = True
    return low[0], high[1]


def parse_opd_section(elf: ElfFile) -> list[tuple[int, int]] | None:
    for sec in elf.sections:
        if sec.name != ".opd":
            continue
        if sec.sh_type == SHT_NOBITS or not sec.data_present or sec.sh_size == 0:
            return None
        start = elf.base + sec.sh_offset
        raw = elf.data[start:start + sec.sh_size]
        return [_OPD.unpack_from(raw, i * _OPD.size)
                for i in range(len(raw) // _OPD.size)]
    return None


def _read_segment_bytes(elf: ElfFile, seg: Segment, size: int, what: str) -> bytes:
    start = elf.base + seg.p_offset
    raw = elf.data[start:start + min(size, seg.p_filesz)]
    if len(raw) < size:
        raise ElfError("%s segment is too small (%d < %d bytes)" % (what, len(raw), size))
    return raw


def parse_proc_param(elf: ElfFile) -> dict | None:
    seg = _find_segment(elf, PT_SCE_PROC_PARAM)
    if seg is None:
        return None
    raw = _read_segment_bytes(elf, seg, _PROC_PARAM.size, "proc_param")
    (size, magic, version, sdk_version, primary_prio, primary_stacksize,
     malloc_pagesize, ppc_seg) = _PROC_PARAM.unpack_from(raw)
    start = elf.base + seg.p_offset
    raw_len = min(size, seg.p_filesz)
    return {
        "addr": seg.p_vaddr,
        "size": size,
        "magic": magic,
        "version": version,
        "sdk_version": sdk_version,
        "primary_prio": primary_prio,
        "primary_stacksize": primary_stacksize,
        "malloc_pagesize": malloc_pagesize,
        "ppc_seg": ppc_seg,
        "raw": elf.data[start:start + raw_len].hex(),
    }


def parse_prx_param(elf: ElfFile) -> dict | None:
    seg = _find_segment(elf, PT_SCE_PRX_PARAM)
    if seg is None:
        return None
    raw = _read_segment_bytes(elf, seg, _PRX_PARAM.size, "prx_param")
    (size, magic, version, unk0, libent_start, libent_end, libstub_start,
     libstub_end, ver, unk1, unk2) = _PRX_PARAM.unpack_from(raw)
    return {
        "addr": seg.p_vaddr,
        "size": size,
        "magic": magic,
        "version": version,
        "unk0": unk0,
        "libent_start": libent_start,
        "libent_end": libent_end,
        "libstub_start": libstub_start,
        "libstub_end": libstub_end,
        "ver": ver,
        "unk1": unk1,
        "unk2": unk2,
    }


def _walk_table(elf: ElfFile, start: int, end: int, known_size: int):
    """Yield (entry_addr, entry_size, is_known_layout) over a libstub/libent range."""
    cursor = start
    while cursor < end:
        size = elf.read_va(cursor, 1)[0]
        if size == 0:
            raise ElfError("zero-sized table entry at 0x%08x" % cursor)
        yield cursor, size, size == known_size
        cursor += size


def parse_imports(elf: ElfFile, prx: dict) -> list[ImportModule]:
    modules = []
    for addr, size, known in _walk_table(elf, prx["libstub_start"], prx["libstub_end"],
                                         IMPORT_ENTRY_SIZE):
        if not known:
            modules.append(ImportModule(addr, "", 0, 0, [], [], 0, size, True))
            continue
        (_size, _unk0, version, attributes, num_func, num_var, num_tlsvar, _info_hash,
         _info_tlshash, _unk1, name, fnids, fstubs, vnids, vstubs, _unk4,
         _unk5) = _LIBSTUB.unpack(elf.read_va(addr, _LIBSTUB.size))
        functions = []
        for i in range(num_func):
            slot_addr = fstubs + 4 * i
            functions.append(ImportFunc(elf.read_u32_va(fnids + 4 * i), slot_addr,
                                        elf.read_u32_va(slot_addr)))
        variables = [(elf.read_u32_va(vnids + 4 * i), elf.read_u32_va(vstubs + 4 * i))
                     for i in range(num_var)]
        modules.append(ImportModule(addr, elf.read_cstr_va(name), version, attributes,
                                    functions, variables, num_tlsvar, size))
    return modules


def parse_exports(elf: ElfFile, prx: dict) -> list[ExportModule]:
    modules = []
    for addr, size, known in _walk_table(elf, prx["libent_start"], prx["libent_end"],
                                         EXPORT_ENTRY_SIZE):
        if not known:
            modules.append(ExportModule(addr, None, 0, 0, [], [], 0, size, True))
            continue
        (_size, _unk0, version, attributes, num_func, num_var, num_tlsvar, _info_hash,
         _info_tlshash, _unk1, name, nids, addrs) = _LIBENT.unpack(
             elf.read_va(addr, _LIBENT.size))
        functions = []
        for i in range(num_func):
            opd_addr = elf.read_u32_va(addrs + 4 * i)
            try:
                func, toc = _read_opd(elf, opd_addr)
            except ElfError:
                func = toc = None
            functions.append(ExportFunc(elf.read_u32_va(nids + 4 * i), opd_addr,
                                        func, toc))
        # Variables follow the functions in the shared nids/addrs arrays.
        variables = [(elf.read_u32_va(nids + 4 * (num_func + i)),
                      elf.read_u32_va(addrs + 4 * (num_func + i)))
                     for i in range(num_var)]
        modules.append(ExportModule(addr, elf.read_cstr_va(name) if name else None,
                                    version, attributes, functions, variables,
                                    num_tlsvar, size))
    return modules


def parse_tls(elf: ElfFile) -> dict | None:
    seg = _find_segment(elf, PT_TLS)
    if seg is None:
        return None
    return {
        "vaddr": seg.p_vaddr,
        "offset": seg.p_offset,
        "filesz": seg.p_filesz,
        "memsz": seg.p_memsz,
        "align": seg.p_align,
    }

"""Minimal big-endian ELF reader for decrypted PS3 executables."""

from __future__ import annotations

import struct
from dataclasses import dataclass

EM_PPC64 = 21
EM_SPU = 23

PT_LOAD = 1
PT_TLS = 7
SHT_NOBITS = 8

PF_X = 1
PF_W = 2
PF_R = 4

ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2MSB = 2
OSABI_PS3 = 0x66

_EHDR64 = struct.Struct(">16sHHIQQQIHHHHHH")
_PHDR64 = struct.Struct(">IIQQQQQQ")
_SHDR64 = struct.Struct(">IIQQQQIIQQ")
_EHDR32 = struct.Struct(">16sHHIIIIIHHHHHH")
_PHDR32 = struct.Struct(">IIIIIIII")
_SHDR32 = struct.Struct(">IIIIIIIIII")


class ElfError(Exception):
    pass


def is_self(data: bytes) -> bool:
    return data[:4] == b"SCE\0"


@dataclass(frozen=True)
class Segment:
    index: int
    p_type: int
    p_flags: int
    p_offset: int
    p_vaddr: int
    p_paddr: int
    p_filesz: int
    p_memsz: int
    p_align: int


@dataclass(frozen=True)
class Section:
    index: int
    name: str | None
    sh_type: int
    sh_flags: int
    sh_addr: int
    sh_offset: int
    sh_size: int
    sh_link: int
    sh_info: int
    sh_addralign: int
    sh_entsize: int
    data_present: bool


class ElfFile:
    """A parsed ELF image located at `base` inside `data`.

    Offsets stored in headers and in `Segment`/`Section` are relative to `base`;
    `va_to_offset` returns an absolute index into `data`.
    """

    def __init__(self) -> None:
        self.data: bytes = b""
        self.base: int = 0
        self.ei_class: int = 0
        self.osabi: int = 0
        self.e_type: int = 0
        self.e_machine: int = 0
        self.e_entry: int = 0
        self.e_flags: int = 0
        self.segments: list[Segment] = []
        self.sections: list[Section] = []
        self._extent: int = 0

    @classmethod
    def from_bytes(cls, data: bytes, base: int = 0) -> "ElfFile":
        if base < 0 or base > len(data):
            raise ElfError("image base outside the buffer")
        if data[base:base + 4] != b"\x7fELF":
            raise ElfError("not an ELF image (bad magic)")
        if len(data) - base < 6:
            raise ElfError("ELF header truncated")
        ei_class = data[base + 4]
        if ei_class == ELFCLASS64:
            ehdr, phdr, shdr = _EHDR64, _PHDR64, _SHDR64
        elif ei_class == ELFCLASS32:
            ehdr, phdr, shdr = _EHDR32, _PHDR32, _SHDR32
        else:
            raise ElfError("unsupported ELF class %d" % ei_class)
        if data[base + 5] != ELFDATA2MSB:
            raise ElfError("not a big-endian ELF image")
        if len(data) - base < ehdr.size:
            raise ElfError("ELF header truncated")

        (ident, e_type, e_machine, _version, e_entry, e_phoff, e_shoff, e_flags,
         _ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
         e_shstrndx) = ehdr.unpack_from(data, base)

        self = cls()
        self.data = data
        self.base = base
        self.ei_class = ei_class
        self.osabi = ident[7]
        self.e_type = e_type
        self.e_machine = e_machine
        self.e_entry = e_entry
        self.e_flags = e_flags

        extent = ehdr.size
        if e_phnum:
            if e_phentsize < phdr.size:
                raise ElfError("program header entry size too small")
            ph_end = e_phoff + e_phnum * e_phentsize
            if base + ph_end > len(data):
                raise ElfError("program header table extends past end of file")
            extent = max(extent, ph_end)
            for i in range(e_phnum):
                fields = phdr.unpack_from(data, base + e_phoff + i * e_phentsize)
                if ei_class == ELFCLASS64:
                    (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz,
                     p_memsz, p_align) = fields
                else:
                    # ELF32 places p_flags after p_memsz.
                    (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
                     p_flags, p_align) = fields
                self.segments.append(Segment(i, p_type, p_flags, p_offset, p_vaddr,
                                             p_paddr, p_filesz, p_memsz, p_align))
                extent = max(extent, p_offset + p_filesz)

        sections_end = self._parse_sections(shdr, e_shoff, e_shentsize, e_shnum,
                                            e_shstrndx)
        self._extent = max(extent, sections_end)
        return self

    @classmethod
    def from_path(cls, path) -> "ElfFile":
        with open(path, "rb") as f:
            return cls.from_bytes(f.read())

    def _parse_sections(self, shdr: struct.Struct, e_shoff: int, e_shentsize: int,
                        e_shnum: int, e_shstrndx: int) -> int:
        # Section headers are optional; any inconsistency yields no sections.
        if e_shnum == 0 or e_shentsize < shdr.size:
            return 0
        sh_end = e_shoff + e_shnum * e_shentsize
        if self.base + sh_end > len(self.data):
            return 0

        raw = [shdr.unpack_from(self.data, self.base + e_shoff + i * e_shentsize)
               for i in range(e_shnum)]

        strtab = None
        if 0 < e_shstrndx < e_shnum:
            st = raw[e_shstrndx]
            st_type, st_off, st_size = st[1], st[4], st[5]
            if st_type != SHT_NOBITS and self.base + st_off + st_size <= len(self.data):
                strtab = self.data[self.base + st_off:self.base + st_off + st_size]

        for i, (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link,
                sh_info, sh_addralign, sh_entsize) in enumerate(raw):
            name = None
            if strtab is not None and sh_name < len(strtab):
                end = strtab.find(b"\0", sh_name)
                if end < 0:
                    end = len(strtab)
                name = strtab[sh_name:end].decode("ascii", errors="replace")
            present = (sh_type == SHT_NOBITS
                       or self.base + sh_offset + sh_size <= len(self.data))
            self.sections.append(Section(i, name, sh_type, sh_flags, sh_addr, sh_offset,
                                         sh_size, sh_link, sh_info, sh_addralign,
                                         sh_entsize, present))
        return sh_end

    def loads(self) -> list[Segment]:
        return [s for s in self.segments if s.p_type == PT_LOAD]

    def exec_segments(self) -> list[Segment]:
        return [s for s in self.loads() if s.p_flags & PF_X]

    def segment_for_va(self, va: int) -> Segment | None:
        # A pre-linked PRX can have a segment whose `p_memsz` reaches into the next one; the
        # segment whose file data holds the address owns it, then the later-starting one.
        # This runs once per read, so it avoids building lists.
        found = None
        found_key = (False, 0)
        for s in self.segments:
            if s.p_type != PT_LOAD or not s.p_vaddr <= va < s.p_vaddr + s.p_memsz:
                continue
            key = (va < s.p_vaddr + s.p_filesz, s.p_vaddr)
            if found is None or key > found_key:
                found, found_key = s, key
        return found

    def va_to_offset(self, va: int) -> int | None:
        s = self.segment_for_va(va)
        if s is None or va - s.p_vaddr >= s.p_filesz:
            return None
        return self.base + s.p_offset + (va - s.p_vaddr)

    def read_va(self, va: int, size: int) -> bytes:
        if size < 0:
            raise ElfError("negative read size")
        if size == 0:
            return b""
        s = self.segment_for_va(va)
        if s is None or va + size > s.p_vaddr + s.p_memsz:
            raise ElfError("read of %d bytes at 0x%x is outside a loaded segment"
                           % (size, va))
        rel = va - s.p_vaddr
        in_file = max(0, min(size, s.p_filesz - rel))
        out = b""
        if in_file:
            start = self.base + s.p_offset + rel
            out = self.data[start:start + in_file]
            if len(out) != in_file:
                raise ElfError("segment data at 0x%x is truncated in the file" % va)
        # Bytes past p_filesz are zero-initialised (bss).
        return out + bytes(size - in_file)

    def read_u16_va(self, va: int) -> int:
        return struct.unpack(">H", self.read_va(va, 2))[0]

    def read_u32_va(self, va: int) -> int:
        return struct.unpack(">I", self.read_va(va, 4))[0]

    def read_cstr_va(self, va: int, max_len: int = 256) -> str:
        out = bytearray()
        for i in range(max_len):
            b = self.read_va(va + i, 1)[0]
            if b == 0:
                try:
                    return out.decode("ascii")
                except UnicodeDecodeError as e:
                    raise ElfError("non-ASCII string at 0x%x" % va) from e
            out.append(b)
        raise ElfError("unterminated string at 0x%x" % va)

    def image_extent(self) -> int:
        return self._extent

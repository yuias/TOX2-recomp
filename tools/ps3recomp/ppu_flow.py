"""Control-flow classification and backward register evaluation over a decoded code range."""

from __future__ import annotations

import struct
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass

from .elf import ElfFile
from .ppu import Insn, decode

_MASK32 = 0xFFFFFFFF

FLOW_KINDS = ("seq", "jump", "cond_jump", "call", "cond_call", "return", "cond_return",
              "indirect_jump", "cond_indirect_jump", "indirect_call", "syscall", "trap",
              "invalid")

_TRAP_FORMS = frozenset({"tw", "td", "twi", "tdi"})
_TRAP_ALWAYS = 31

# Flow kinds after which execution never reaches the next word.
_NO_FALL_THROUGH = frozenset({"jump", "return", "indirect_jump", "trap", "invalid"})
# Kinds that end a straight-line backward walk: the preceding word is not the predecessor of
# the current one (unconditional transfer) or a call clobbers volatile registers.
_WALK_BARRIERS = frozenset({"invalid", "jump", "return", "indirect_jump",
                            "call", "cond_call", "indirect_call"})

_UPDATE_FORMS = frozenset(
    "lwzu lbzu lhzu lhau lfsu lfdu stwu stbu sthu stfsu stfdu ldu stdu "
    "lwzux lbzux lhzux lhaux lwaux ldux stwux stbux sthux stdux lfsux lfdux "
    "stfsux stfdux".split())


@dataclass(frozen=True)
class Flow:
    kind: str
    target: int | None        # direct target for b/bc forms, else None
    falls_through: bool


@dataclass(frozen=True)
class CodeImage:
    start: int
    end: int                  # exclusive
    words: tuple[int, ...]
    insns: tuple[Insn, ...]   # strict decode, one per word

    @classmethod
    def from_words(cls, start: int, words: Sequence[int]) -> "CodeImage":
        words = tuple(words)
        insns = tuple(decode(w, start + 4 * i, strict=True) for i, w in enumerate(words))
        return cls(start, start + 4 * len(words), words, insns)

    @classmethod
    def from_elf(cls, elf: ElfFile, text_range: tuple[int, int]) -> "CodeImage":
        start, end = text_range
        count = (end - start) // 4
        words = struct.unpack(">%dI" % count, elf.read_va(start, count * 4))
        return cls.from_words(start, words)

    def contains(self, addr: int) -> bool:
        return self.start <= addr < self.end and addr % 4 == 0

    def _index(self, addr: int) -> int:
        if not self.contains(addr):
            raise ValueError("address 0x%08x is outside the code image" % addr)
        return (addr - self.start) // 4

    def word(self, addr: int) -> int:
        return self.words[self._index(addr)]

    def insn(self, addr: int) -> Insn:
        return self.insns[self._index(addr)]

    def valid(self, addr: int) -> bool:
        return self.contains(addr) and self.insns[(addr - self.start) // 4].spec is not None


def _always(insn: Insn) -> bool:
    return insn.field("BO") & 0x14 == 0x14


def flow_of(insn: Insn) -> Flow:
    spec = insn.spec
    if spec is None:
        return Flow("invalid", None, False)
    name = spec.name
    if name in ("b", "bc"):
        disp = insn.field("LI" if name == "b" else "BD")
        target = (disp if insn.field("AA") else insn.addr + disp) & _MASK32
        link = insn.field("LK")
        if link and target == (insn.addr + 4) & _MASK32:
            # `bcl 20,31,$+4` reads the PC; it never leaves the instruction stream.
            return Flow("seq", None, True)
        if name == "b":
            return Flow("call", target, True) if link else Flow("jump", target, False)
        if link:
            return Flow("call" if _always(insn) else "cond_call", target, True)
        if _always(insn):
            return Flow("jump", target, False)
        return Flow("cond_jump", target, True)
    if name in ("bclr", "bcctr"):
        if insn.field("LK"):
            return Flow("indirect_call", None, True)
        always = _always(insn)
        if name == "bclr":
            return Flow("return" if always else "cond_return", None, not always)
        return Flow("indirect_jump" if always else "cond_indirect_jump", None, not always)
    if name == "sc":
        return Flow("syscall", None, True)
    if name in _TRAP_FORMS and insn.field("TO") == _TRAP_ALWAYS:
        return Flow("trap", None, False)
    return Flow("seq", None, True)


def written_gprs(insn: Insn) -> frozenset[int]:
    """GPRs an instruction overwrites (rD, logical-op rA, update-form rA)."""
    spec = insn.spec
    written = set()
    if spec.operands[:1] == ("rD",):
        written.add(insn.field("rD"))
        if spec.name == "lmw":
            written.update(range(insn.field("rD"), 32))
    if spec.operands[:2] == ("rA", "rS"):
        written.add(insn.field("rA"))
    if spec.name in _UPDATE_FORMS:
        written.add(insn.field("rA"))
    return frozenset(written)


def walk_back(image: CodeImage, addr: int, limit: int) -> Iterator[Insn]:
    """Yield the instructions before `addr`, nearest first, at most `limit` of them.

    The walk ends before an invalid word, a call (a branch with LK=1), an unconditional
    jump/return, or the image start; those words are not yielded. Conditional non-linking
    branches are yielded and passed.
    """
    # Known limitation: a branch target between a definition and its use is not detected
    # without a CFG, so a join point could in theory mix in another path's value.
    cur = addr - 4
    for _ in range(limit):
        if not image.contains(cur):
            return
        insn = image.insn(cur)
        if flow_of(insn).kind in _WALK_BARRIERS:
            return
        yield insn
        cur -= 4


def find_def(image: CodeImage, addr: int, reg: int, limit: int = 16) -> Insn | None:
    for insn in walk_back(image, addr, limit):
        if reg in written_gprs(insn):
            return insn
    return None


def eval_const(image: CodeImage, addr: int, reg: int, *, toc: int | None = None,
               read_mem: Callable[[int, int], bytes | None] | None = None,
               depth: int = 6) -> int | None:
    """Value of GPR `reg` just before `addr`, or None when it is not a recoverable constant."""
    if reg == 2:
        # r2 is constant in this ELF; any write to it is a TOC restore.
        return toc
    if depth <= 0:
        return None
    d = find_def(image, addr, reg)
    if d is None:
        return None

    def src(r: int) -> int | None:
        # rA == 0 reads as literal zero in the address and addi/addis forms only.
        return eval_const(image, d.addr, r, toc=toc, read_mem=read_mem, depth=depth - 1)

    def base(r: int) -> int | None:
        return 0 if r == 0 else src(r)

    name = d.spec.name
    if name in ("addi", "addis"):
        simm = d.field("SIMM") << (16 if name == "addis" else 0)
        b = base(d.field("rA"))
        return None if b is None else (b + simm) & _MASK32
    if name in ("ori", "oris"):
        uimm = d.field("UIMM") << (16 if name == "oris" else 0)
        v = src(d.field("rS"))
        return None if v is None else (v | uimm) & _MASK32
    if name == "or":
        if d.field("rS") != d.field("rB"):
            return None
        return src(d.field("rS"))
    if name == "rldicl":
        if d.field("SH64") != 0 or d.field("MB64") != 32:
            return None
        v = src(d.field("rS"))
        return None if v is None else v & _MASK32
    if name in ("lwz", "ld"):
        # Update forms are separate mnemonics, so rD is the only register written here.
        if read_mem is None:
            return None
        b = base(d.field("rA"))
        if b is None:
            return None
        if name == "lwz":
            ea, size = (b + d.field("D")) & _MASK32, 4
        else:
            ea, size = (b + d.field("DS")) & _MASK32, 8
        raw = read_mem(ea, size)
        if raw is None or len(raw) != size:
            return None
        return int.from_bytes(raw, "big") & _MASK32
    return None

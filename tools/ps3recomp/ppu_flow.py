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

# `walk_back` stops at every one of these; the call-crossing walk stops at the rest only.
_CALL_KINDS = frozenset({"call", "cond_call", "indirect_call"})
_HARD_BARRIERS = _WALK_BARRIERS - _CALL_KINDS

# Registers a callee preserves (ELF64 PPC ABI): their value survives a `bl`.
_CALLEE_SAVED = frozenset(range(14, 32))
# Words searched back for a definition or a stack-slot store when calls may be crossed; a function
# prologue spills and the code that reloads the value can be this far apart.
_CALL_WINDOW = 64

_STACK_REG = 1
# Byte widths of the D/DS-form stores that can overlap a stack slot.
_STORE_WIDTH = {"stb": 1, "sth": 2, "stw": 4, "std": 8, "stfs": 4, "stfd": 8}

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


def _walk_back_calls(image: CodeImage, addr: int, limit: int, cross_calls: bool) -> Iterator[Insn]:
    """`walk_back` that may pass calls; the other barriers still end the walk."""
    barriers = _HARD_BARRIERS if cross_calls else _WALK_BARRIERS
    cur = addr - 4
    for _ in range(limit):
        if not image.contains(cur):
            return
        insn = image.insn(cur)
        if flow_of(insn).kind in barriers:
            return
        yield insn
        cur -= 4


def find_def(image: CodeImage, addr: int, reg: int, limit: int = 16, *,
             through_calls: bool = False) -> Insn | None:
    """Nearest instruction before `addr` that writes `reg`, within `limit` words.

    With `through_calls`, the search is `_CALL_WINDOW` words long and passes calls when `reg` is
    callee-saved; a volatile register still stops at the first call.
    """
    if through_calls:
        limit = _CALL_WINDOW
    cross = through_calls and reg in _CALLEE_SAVED
    for insn in _walk_back_calls(image, addr, limit, cross):
        if reg in written_gprs(insn):
            return insn
    return None


def find_spill_store(image: CodeImage, load: Insn) -> Insn | None:
    """Store that wrote the stack slot a reload `ld/lwz rD,D(r1)` reads, or None.

    Stack slots persist across calls, so calls are crossed. The search gives up on a write to
    r1, on a store of another width that overlaps the slot, and on an indexed or multi-word
    store through r1, since the slot may have changed.
    """
    if load.spec.name == "ld":
        disp, size, store_name = load.field("DS"), 8, "std"
    elif load.spec.name == "lwz":
        disp, size, store_name = load.field("D"), 4, "stw"
    else:
        return None
    if load.field("rA") != _STACK_REG:
        return None
    for insn in _walk_back_calls(image, load.addr, _CALL_WINDOW, True):
        if _STACK_REG in written_gprs(insn):
            return None
        name = insn.spec.name
        if not name.startswith("st") or insn.field("rA") != _STACK_REG:
            continue
        width = _STORE_WIDTH.get(name)
        if width is None:
            return None
        store_disp = insn.field("DS") if name == "std" else insn.field("D")
        if store_disp + width <= disp or disp + size <= store_disp:
            continue
        if name == store_name and store_disp == disp:
            return insn
        return None
    return None


def eval_const(image: CodeImage, addr: int, reg: int, *, toc: int | None = None,
               read_mem: Callable[[int, int], bytes | None] | None = None,
               depth: int = 6, through_calls: bool = False) -> int | None:
    """Value of GPR `reg` just before `addr`, or None when it is not a recoverable constant.

    `through_calls` lets the search pass calls for callee-saved registers and follow a reload
    from a stack slot to the store that spilled it (see `find_def` and `find_spill_store`). It is
    off by default because passing a call is only sound for the table-base uses of the jump-table
    recovery; the other callers keep the call as a barrier.
    """
    if reg == 2:
        # r2 is constant in this ELF; any write to it is a TOC restore.
        return toc
    if depth <= 0:
        return None
    d = find_def(image, addr, reg, through_calls=through_calls)
    if d is None:
        return None

    def src(r: int) -> int | None:
        # rA == 0 reads as literal zero in the address and addi/addis forms only.
        return eval_const(image, d.addr, r, toc=toc, read_mem=read_mem, depth=depth - 1,
                          through_calls=through_calls)

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
    if name in ("lwz", "ld") and through_calls and d.field("rA") == _STACK_REG:
        store = find_spill_store(image, d)
        if store is None:
            return None
        return eval_const(image, store.addr, store.field("rS"), toc=toc, read_mem=read_mem,
                          depth=depth - 1, through_calls=True)
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

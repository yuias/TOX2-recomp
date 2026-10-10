"""SPU control-flow classification and backward register evaluation over a decoded code image.

An SPU image has several code segments and its constant code pointers may sit in data segments,
so `SpuCode` keeps every segment of the image: instructions for the code ones, plain bytes for
all of them. SPU branches have no delay slots, so the flow of an instruction is local.
"""

from __future__ import annotations

import functools
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass

from .spu import LS_TARGET_MASK, SpuInsn, decode_range

_MASK32 = 0xFFFFFFFF
# lqr/lqa/lqd addresses drop the low 4 bits and wrap at the 256 KiB local store.
_QUAD_MASK = 0x3FFF0

FLOW_KINDS = ("seq", "jump", "cond_jump", "call", "indirect_call", "return", "cond_return",
              "indirect_jump", "cond_indirect_jump", "terminate", "invalid")

# `stop` codes that end the thread; every other code returns control to the next instruction.
STOP_TERMINATE_CODES = frozenset({0x101, 0x102})

# Kinds that end a backward walk: the preceding word is not the predecessor of the current one
# (no fall-through), or a call clobbers registers.
_WALK_BARRIERS = frozenset({"invalid", "jump", "return", "indirect_jump", "terminate",
                            "call", "indirect_call"})

# Instructions whose RT operand is read (or unused) instead of written.
_RT_NOT_WRITTEN = frozenset({"wrch", "mtspr", "nop", "brz", "brnz", "brhz", "brhnz",
                             "biz", "binz", "bihz", "bihnz"})

_HALT_REG = frozenset({"heq", "hgt", "hlgt"})
_HALT_IMM = frozenset({"heqi", "hgti", "hlgti"})


@dataclass(frozen=True)
class Flow:
    kind: str
    target: int | None        # direct target of relative/absolute branches
    falls_through: bool
    link: int | None = None   # register that receives the return address
    reg: int | None = None    # register that holds an indirect target (None for iret)


class SpuCode:
    """Code and data of one SPU image, addressed by LS address."""

    def __init__(self, code_segments: Sequence[tuple[int, bytes]],
                 data_segments: Sequence[tuple[int, bytes]] = ()):
        self._insns: dict[int, SpuInsn] = {}
        for ls, data in code_segments:
            for insn in decode_range(data, ls):
                if insn.addr in self._insns:
                    raise ValueError("code segments overlap at ls 0x%05x" % insn.addr)
                self._insns[insn.addr] = insn
        self._memory = tuple(sorted([*code_segments, *data_segments]))

    @classmethod
    def from_words(cls, start: int, words: Iterable[int]) -> SpuCode:
        return cls([(start, b"".join(w.to_bytes(4, "big") for w in words))])

    @classmethod
    def from_image(cls, image) -> SpuCode:
        """`image` is a `spu_images.SpuImage` with a known LS base."""
        code = [(s.ls, image.segment_bytes(s)) for s in image.segments if s.code]
        data = [(s.ls, image.segment_bytes(s)) for s in image.segments if not s.code]
        return cls(code, data)

    def contains(self, addr: int) -> bool:
        return addr in self._insns

    def valid(self, addr: int) -> bool:
        # An all-zero word is padding far more often than a real `stop 0`; it is never code.
        insn = self._insns.get(addr)
        return insn is not None and insn.spec is not None and insn.word != 0

    def insn(self, addr: int) -> SpuInsn:
        return self._insns[addr]

    def word(self, addr: int) -> int:
        return self._insns[addr].word

    @property
    def addrs(self) -> list[int]:
        return sorted(self._insns)

    def read_word(self, addr: int) -> int | None:
        """Big-endian word at `addr` from the image bytes (code or data), None when not backed."""
        for ls, data in self._memory:
            if ls <= addr and addr + 4 <= ls + len(data):
                return int.from_bytes(data[addr - ls:addr - ls + 4], "big")
        return None

    @functools.cached_property
    def direct_targets(self) -> frozenset[int]:
        """Code addresses that some direct branch (jump, call or conditional) names."""
        out = set()
        for insn in self._insns.values():
            target = insn.branch_target
            if target is None or target not in self._insns:
                continue
            # The PC-read idiom `brsl $x,$+4` does not make its successor a join point.
            if "link" in insn.spec.flags and target == insn.addr + 4:
                continue
            out.add(target)
        return frozenset(out)


def _halt_always(insn: SpuInsn, const_of: Callable[[int], int | None] | None) -> bool:
    """True when a conditional halt is certainly taken."""
    name = insn.spec.mnemonic
    if name in _HALT_REG:
        ra, rb = insn.field("RA"), insn.field("RB")
        if ra == rb:
            return name == "heq"
        if const_of is None:
            return False
        a, b = const_of(ra), const_of(rb)
    else:
        if const_of is None:
            return False
        a, b = const_of(insn.field("RA")), insn.field("I10") & _MASK32
    if a is None or b is None:
        return False
    if name in ("heq", "heqi"):
        return a == b
    if name in ("hlgt", "hlgti"):
        return a > b
    return _signed(a) > _signed(b)


def _signed(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


def flow_of(insn: SpuInsn, const_of: Callable[[int], int | None] | None = None) -> Flow:
    """Classify `insn`. `const_of(reg)` gives the preferred-slot value of a register just before
    the instruction, when known; it only matters for halts with a constant condition."""
    spec = insn.spec
    if spec is None or insn.word == 0:
        return Flow("invalid", None, False)
    name = spec.mnemonic
    flags = spec.flags
    if "branch" in flags:
        link = insn.field("RT") if "link" in flags else None
        if "indirect" in flags:
            ra = insn.field("RA")
            if name == "bi":
                return Flow("return", None, False) if ra == 0 else Flow("indirect_jump", None, False, None, ra)
            if name == "iret":
                return Flow("indirect_jump", None, False)
            if name in ("bisl", "bisled"):
                return Flow("indirect_call", None, True, link, ra)
            # biz/binz/bihz/bihnz test RT and continue at the next word when not taken.
            return Flow("cond_return", None, True) if ra == 0 else Flow("cond_indirect_jump", None, True, None, ra)
        target = insn.branch_target
        if link is not None:
            if target == insn.addr + 4:
                # `brsl $x,$+4` reads the PC; it never leaves the instruction stream.
                return Flow("seq", None, True)
            return Flow("call", target, True, link)
        if "conditional" in flags:
            return Flow("cond_jump", target, True)
        return Flow("jump", target, False)
    if "stop" in flags:
        if name == "stopd" or insn.field("STOP") in STOP_TERMINATE_CODES:
            return Flow("terminate", None, False)
        return Flow("seq", None, True)
    if "halt" in flags and _halt_always(insn, const_of):
        return Flow("terminate", None, False)
    return Flow("seq", None, True)


def written_reg(insn: SpuInsn) -> int | None:
    """The register an instruction overwrites (including the link of branches), or None."""
    spec = insn.spec
    if spec is None or not spec.operands:
        return None
    first = spec.operands[0]
    if first == "RT4":
        return insn.field("RT4")
    if first == "RT" and spec.mnemonic not in _RT_NOT_WRITTEN and "store" not in spec.flags:
        return insn.field("RT")
    return None


def eval_const(code: SpuCode, addr: int, reg: int, *, labels: frozenset[int] | None = None,
               memory: bool = False, limit: int = 64, depth: int = 8) -> int | None:
    """Preferred-slot word of `reg` just before `addr`, or None when it is not a recoverable constant.

    The walk goes backward through straight-line code. It ends at `labels` (addresses other paths
    can enter; default: the direct branch targets of the image), at unconditional transfers and
    at calls. `memory` also follows `lqr`/`lqa`/`lqd` into the image bytes; those hold initial
    values that a running program may overwrite, so only code-pointer detection uses them.
    """
    if labels is None:
        labels = code.direct_targets
    pos = addr
    for _ in range(limit):
        if depth <= 0 or pos in labels:
            return None
        cur = pos - 4
        if not code.contains(cur):
            return None
        insn = code.insn(cur)
        if flow_of(insn).kind in _WALK_BARRIERS:
            return None
        if written_reg(insn) == reg:
            return _eval_def(code, insn, labels, memory, limit, depth)
        pos = cur
    return None


def _eval_def(code: SpuCode, d: SpuInsn, labels: frozenset[int], memory: bool, limit: int,
              depth: int) -> int | None:
    def src(r: int) -> int | None:
        return eval_const(code, d.addr, r, labels=labels, memory=memory, limit=limit, depth=depth - 1)

    name = d.spec.mnemonic
    if name == "il":
        return d.field("I16") & _MASK32
    if name == "ila":
        return d.field("I18")
    if name == "ilhu":
        return d.field("U16") << 16
    if name == "ilh":
        return d.field("U16") * 0x10001
    if name in ("brsl", "brasl"):
        # Only the `$+4` PC read reaches here; real calls are walk barriers.
        return (d.addr + 4) & _MASK32
    if name == "iohl":
        base = src(d.field("RT"))
        return None if base is None else base | d.field("U16")
    if name in ("ai", "ori", "andi", "shli"):
        a = src(d.field("RA"))
        if a is None:
            return None
        if name == "ai":
            return (a + d.field("I10")) & _MASK32
        if name == "ori":
            return a | (d.field("I10") & _MASK32)
        if name == "andi":
            return a & d.field("I10") & _MASK32
        count = d.field("I7") & 0x3F
        return (a << count) & _MASK32 if count < 32 else 0
    if name in ("a", "sf", "and", "or"):
        a = src(d.field("RA"))
        if a is None:
            return None
        b = a if name == "or" and d.field("RA") == d.field("RB") else src(d.field("RB"))
        if b is None:
            return None
        if name == "a":
            return (a + b) & _MASK32
        if name == "sf":
            return (b - a) & _MASK32
        return a & b if name == "and" else a | b
    if memory and name in ("lqr", "lqa", "lqd"):
        if name == "lqr":
            ea = (d.addr + d.field("REL16")) & _QUAD_MASK
        elif name == "lqa":
            ea = d.field("ABS16") & _QUAD_MASK
        else:
            base = src(d.field("RA"))
            if base is None:
                return None
            ea = (base + d.field("I10x16")) & _QUAD_MASK
        return code.read_word(ea)
    return None


def const_target(code: SpuCode, insn: SpuInsn, flow: Flow, *, labels: frozenset[int] | None = None,
                 memory: bool = True) -> int | None:
    """LS address that the register of an indirect branch holds, when it is a constant.

    A value that comes out of the image bytes (`memory`) counts only when it is an address in the
    image's code: data slots of other values are not pointers, and the bytes may change at run time.
    """
    if flow.reg is None:
        return None
    value = eval_const(code, insn.addr, flow.reg, labels=labels)
    if value is None and memory:
        value = eval_const(code, insn.addr, flow.reg, labels=labels, memory=True)
        if value is not None and not code.contains(value & LS_TARGET_MASK):
            return None
    return None if value is None else value & LS_TARGET_MASK

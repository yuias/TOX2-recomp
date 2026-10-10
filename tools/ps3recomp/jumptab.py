"""Recovery of compiler switch tables behind `bctr` (bcctr with LK=0)."""

from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass

from .elf import ElfError, ElfFile
from .ppu import Insn
from .ppu_flow import CodeImage, eval_const, find_def, flow_of, walk_back, written_gprs

UNRESOLVED_REASONS = ("no_mtctr", "ctr_from_load", "unknown_def", "no_table_base", "no_index",
                      "no_bound", "bad_bound", "table_unreadable", "bad_target")

_MASK32 = 0xFFFFFFFF
_SPR_CTR = 9
_WALK_LIMIT = 32
_MAX_COUNT = 4096
_MAX_HOPS = 4
# A table base built with `lis; addi` has its first instruction this far before the spine
# start that a branch into the table block may target.
_BASE_PREFIX = 8
# Distance searched back from a `ble` into the table block for its compare.
_ENTRY_COMPARE_LIMIT = 16

_LOAD_INDEXED = frozenset({"lwzx", "lwax"})
_COMPARES = frozenset({"cmp", "cmpi", "cmpl", "cmpli"})


@dataclass(frozen=True)
class JumpTable:
    branch: int
    table: int
    kind: str                     # "abs32" | "rel32"
    count: int
    targets: tuple[int, ...]      # entry order, may repeat
    table_in_text: bool


@dataclass(frozen=True)
class UnresolvedJump:
    branch: int
    reason: str
    detail: str


class _Unresolved(Exception):
    def __init__(self, reason: str, detail: str):
        super().__init__(reason, detail)
        self.reason = reason
        self.detail = detail


def make_reader(elf: ElfFile) -> Callable[[int, int], bytes | None]:
    """Reader over file-backed PT_LOAD bytes; zero-filled (bss) or unmapped ranges give None."""
    def read_mem(ea: int, size: int) -> bytes | None:
        seg = elf.segment_for_va(ea)
        if seg is None or ea + size > seg.p_vaddr + seg.p_filesz:
            return None
        try:
            return elf.read_va(ea, size)
        except ElfError:
            return None
    return read_mem


def _sext32(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


def _def(image: CodeImage, addr: int, reg: int) -> Insn | None:
    return find_def(image, addr, reg, _WALK_LIMIT)


def _copy_source(insn: Insn, narrowing: bool) -> int | None:
    """Source register when `insn` copies a value (`narrowing` also admits zero-extending forms)."""
    name = insn.spec.name
    if name == "or" and insn.field("rS") == insn.field("rB"):
        return insn.field("rS")
    if name == "extsw":
        return insn.field("rS")
    if not narrowing:
        return None
    # clrlwi / clrldi 32 keep the variable's identity for bound checks on narrowed copies.
    if name == "rlwinm" and insn.field("SH") == 0 and insn.field("ME") == 31:
        return insn.field("rS")
    if name == "rldicl" and insn.field("SH64") == 0 and insn.field("MB64") == 32:
        return insn.field("rS")
    return None


def _skip_copies(image: CodeImage, addr: int, reg: int) -> Insn | None:
    """First definition of `reg` before `addr` that is not a `mr`/`extsw` hop."""
    for _ in range(_MAX_HOPS + 1):
        d = _def(image, addr, reg)
        if d is None:
            return None
        src = _copy_source(d, False)
        if src is None:
            return d
        addr, reg = d.addr, src
    return None


def _root(image: CodeImage, addr: int, reg: int) -> tuple[int | None, int]:
    """Identity of the value in `reg` before `addr`: (address of its last real def, register)."""
    for _ in range(_MAX_HOPS + 1):
        d = _def(image, addr, reg)
        if d is None:
            return None, reg
        src = _copy_source(d, True)
        if src is None:
            return d.addr, reg
        addr, reg = d.addr, src
    return addr, reg


def _chain(image: CodeImage, addr: int, reg: int) -> set[int]:
    """`reg` plus the registers it was copied from (through mr/clrlwi/clrldi/extsw)."""
    regs = {reg}
    for _ in range(_MAX_HOPS):
        d = _def(image, addr, reg)
        src = None if d is None else _copy_source(d, True)
        if src is None:
            break
        regs.add(src)
        addr, reg = d.addr, src
    return regs


def _index_source(insn: Insn) -> int | None:
    """Source register of a `<< 2` index scaling, else None."""
    name = insn.spec.name
    if name == "rlwinm" and insn.field("SH") == 2 and insn.field("ME") == 29:
        return insn.field("rS")        # slwi 2 (MB = 0) and clrlslwi forms
    if name == "rldic" and insn.field("SH64") == 2 and insn.field("MB64") == 30:
        return insn.field("rS")
    if name == "rldicr" and insn.field("SH64") == 2 and insn.field("MB64") == 61:
        return insn.field("rS")        # sldi 2
    return None


def _compare_crf(insn: Insn) -> int | None:
    return insn.field("crfD") if insn.spec.name in _COMPARES else None


def _unsigned_compare(image: CodeImage, insn: Insn, toc: int | None,
                      read_mem) -> tuple[int, int] | None:
    """(register, constant) of a `cmpli`, or a `cmpl` whose second operand is constant."""
    name = insn.spec.name
    if name == "cmpli":
        return insn.field("rA"), insn.field("UIMM")
    if name == "cmpl":
        k = eval_const(image, insn.addr, insn.field("rB"), toc=toc, read_mem=read_mem)
        return None if k is None else (insn.field("rA"), k)
    return None


class _EntryBranches:
    """Lazy map: target -> addresses of `ble` instructions (BO=4, bit GT) jumping there."""

    def __init__(self, image: CodeImage):
        self._image = image
        self._by_target: dict[int, list[int]] | None = None

    def to(self, target: int) -> list[int]:
        if self._by_target is None:
            self._by_target = {}
            for insn in self._image.insns:
                if (insn.spec is not None and insn.spec.name == "bc"
                        and insn.field("BO") & 0x1E == 4 and insn.field("BI") % 4 == 1):
                    flow = flow_of(insn)
                    if flow.kind == "cond_jump":
                        self._by_target.setdefault(flow.target, []).append(insn.addr)
        return self._by_target.get(target, [])


def _bound_by_walk(image: CodeImage, branch: int, spine_start: int, index_src: int,
                   index_addr: int, toc, read_mem) -> int | None:
    # Bound check that leaves the table path: `cmplwi crN,rZ,K` then `bgt crN,default`.
    want = _root(image, index_addr, index_src)
    clobbered: set[int] = set()
    exits: list[Insn] = []          # nearest first
    for insn in walk_back(image, branch, _WALK_LIMIT):
        crf = _compare_crf(insn)
        if crf is None:
            if flow_of(insn).kind == "cond_jump":
                exits.append(insn)
            continue
        operands = _unsigned_compare(image, insn, toc, read_mem)
        if operands is not None and crf not in clobbered:
            rz, k = operands
            if _root(image, insn.addr, rz) == want:
                for bc in exits:
                    if bc.field("BI") // 4 != crf:
                        continue
                    target = flow_of(bc).target
                    if spine_start <= target <= branch:
                        continue     # stays on the table path
                    bo, bit = bc.field("BO") & 0x1E, bc.field("BI") % 4
                    if bo == 12 and bit == 1:      # bgt
                        return k + 1
                    if bo == 4 and bit == 0:       # bge
                        return k
        clobbered.add(crf)
    return None


def _bound_by_entry(image: CodeImage, spine_start: int, index_src: int, enter: _EntryBranches,
                    toc, read_mem) -> int | None:
    # The table block is reached only by `ble crN,L` after `cmplwi crN,rZ,K`; a blr/b/bl
    # just before the block hides that compare from a backward walk from the branch.
    counts: set[int] = set()
    for target in range(spine_start - _BASE_PREFIX, spine_start + 1, 4):
        if not image.contains(target) or any(
                flow_of(image.insn(a)).kind != "seq" for a in range(target, spine_start, 4)):
            continue
        for addr in enter.to(target):
            count = _entry_compare(image, addr, index_src, toc, read_mem)
            if count is not None:
                counts.add(count)
    if len(counts) > 1:
        raise _Unresolved("bad_bound", "conflicting entry bounds %s" % sorted(counts))
    return counts.pop() if counts else None


def _entry_compare(image: CodeImage, ble_addr: int, index_src: int, toc, read_mem) -> int | None:
    crf = image.insn(ble_addr).field("BI") // 4
    passed: list[Insn] = []
    for insn in walk_back(image, ble_addr, _ENTRY_COMPARE_LIMIT):
        found = _compare_crf(insn)
        if found is None:
            passed.append(insn)
            continue
        if found != crf:
            continue
        operands = _unsigned_compare(image, insn, toc, read_mem)
        if operands is None:
            return None
        rz, k = operands
        if index_src not in _chain(image, insn.addr, rz):
            return None
        if any(rz in written_gprs(p) for p in passed):
            return None
        return k + 1
    return None


def _split_add(image: CodeImage, add: Insn, toc, read_mem) -> tuple[Insn, int]:
    """(indexed load, constant base) for `add rE,rA,rB` where one operand is a table load."""
    problem: _Unresolved | None = None
    for loaded, other in ((add.field("rA"), add.field("rB")),
                          (add.field("rB"), add.field("rA"))):
        cand = _skip_copies(image, add.addr, loaded)
        if cand is None or cand.spec.name not in _LOAD_INDEXED:
            continue
        base = eval_const(image, add.addr, other, toc=toc, read_mem=read_mem)
        if base is not None:
            return cand, base
        problem = _Unresolved("no_table_base", "add operand r%d is not constant" % other)
    raise problem or _Unresolved("unknown_def", "add without an lwzx/lwax operand")


def _recover_site(image: CodeImage, branch: int, read_mem, toc, enter: _EntryBranches
                  ) -> JumpTable:
    mtctr = next((i for i in walk_back(image, branch, _WALK_LIMIT)
                  if i.spec.name == "mtspr" and i.field("SPR") == _SPR_CTR), None)
    if mtctr is None:
        raise _Unresolved("no_mtctr", "no mtctr within %d words" % _WALK_LIMIT)
    reg = mtctr.field("rS")
    d = _skip_copies(image, mtctr.addr, reg)
    if d is None:
        raise _Unresolved("unknown_def", "no definition of r%d" % reg)

    name = d.spec.name
    rel_base = None
    if name in _LOAD_INDEXED:
        load, kind = d, "abs32"
    elif name == "add":
        load, rel_base = _split_add(image, d, toc, read_mem)
        kind = "rel32"
    elif name in ("lwz", "ld"):
        ra = d.field("rA")
        const = 0 if ra == 0 else eval_const(image, d.addr, ra, toc=toc, read_mem=read_mem)
        if const is None:
            raise _Unresolved("ctr_from_load", "%s at 0x%08x" % (name, d.addr))
        raise _Unresolved("unknown_def", "%s from a constant address at 0x%08x" % (name, d.addr))
    else:
        raise _Unresolved("unknown_def", "%s at 0x%08x" % (name, d.addr))

    ra, rb = load.field("rA"), load.field("rB")
    if ra == 0:
        raise _Unresolved("no_index", "lwzx with rA = 0")
    const_a = eval_const(image, load.addr, ra, toc=toc, read_mem=read_mem)
    const_b = eval_const(image, load.addr, rb, toc=toc, read_mem=read_mem)
    if const_a is None and const_b is None:
        raise _Unresolved("no_table_base", "neither r%d nor r%d is constant" % (ra, rb))
    if const_a is not None and const_b is not None:
        raise _Unresolved("no_index", "both load operands are constant")
    table, base_reg, index_reg = (const_a, ra, rb) if const_a is not None else (const_b, rb, ra)

    index_def = _def(image, load.addr, index_reg)
    index_src = None if index_def is None else _index_source(index_def)
    if index_src is None:
        raise _Unresolved("no_index", "r%d is not a scaled index" % index_reg)

    starts = [load.addr, index_def.addr]
    base_def = _def(image, load.addr, base_reg)
    if base_def is not None:
        starts.append(base_def.addr)
    spine_start = min(starts)

    count = _bound_by_walk(image, branch, spine_start, index_src, index_def.addr, toc, read_mem)
    if count is None:
        count = _bound_by_entry(image, spine_start, index_src, enter, toc, read_mem)
    if count is None:
        raise _Unresolved("no_bound", "no bound check for r%d" % index_src)
    if not 0 < count <= _MAX_COUNT:
        raise _Unresolved("bad_bound", "count %d" % count)

    raw = read_mem(table, 4 * count)
    if raw is None or len(raw) != 4 * count:
        raise _Unresolved("table_unreadable", "0x%08x, %d entries" % (table, count))
    entries = struct.unpack(">%dI" % count, raw)
    if kind == "abs32":
        targets = entries
    else:
        targets = tuple((rel_base + _sext32(e)) & _MASK32 for e in entries)
    for target in targets:
        if not image.valid(target):
            raise _Unresolved("bad_target", "0x%08x" % target)
    in_text = image.contains(table) and table + 4 * count <= image.end
    return JumpTable(branch, table, kind, count, tuple(targets), in_text)


def recover_jump_tables(image: CodeImage, read_mem: Callable[[int, int], bytes | None],
                        toc: int | None
                        ) -> tuple[dict[int, JumpTable], dict[int, UnresolvedJump]]:
    """Resolve every `bcctr` (LK=0) in the image; each one lands in exactly one result dict."""
    tables: dict[int, JumpTable] = {}
    unresolved: dict[int, UnresolvedJump] = {}
    enter = _EntryBranches(image)
    for insn in image.insns:
        if insn.spec is None or insn.spec.name != "bcctr" or insn.field("LK"):
            continue
        try:
            tables[insn.addr] = _recover_site(image, insn.addr, read_mem, toc, enter)
        except _Unresolved as e:
            unresolved[insn.addr] = UnresolvedJump(insn.addr, e.reason, e.detail)
    return tables, unresolved

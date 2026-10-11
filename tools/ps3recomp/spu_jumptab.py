"""Resolution of computed `bi` targets that are not a single constant: jump tables and scaled offsets.

Two idioms occur in SPU code and both are matched structurally, by walking backward from the branch
through straight-line code:

- Table: the index is scaled by 4 (`shli`), the table quadword is loaded with `lqx` and the wanted
  word is rotated into the preferred slot with `rotqby`, using the same address as the load; a
  `clgti` against the index plus a `brnz` (or a conditional return, `binz $r,$0`) on its result
  bounds the table.
- Computed offset (Duff device): `brsl $r,$+4` reads the PC, an offset taken from `andi` + `rotmi`
  is added to it and the sum is branched to. Every value the mask allows is a target.

The walks ignore join points: the patterns are strict enough that an unrelated path merging in
between would have to produce the very same instruction sequence.
"""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field

from .spu import LS_TARGET_MASK, SpuInsn
from .spu_flow import SpuCode, eval_const, find_def, flow_of, written_reg

_NO_LABELS: frozenset[int] = frozenset()
_MASK32 = 0xFFFFFFFF
# A table is found from its bound; a larger bound is more likely a mismatch than a table.
MAX_TABLE_ENTRIES = 256
_SCAN_LIMIT = 96
# Reason returned by a matcher when the site does not look like its idiom at all; not worth a note.
_NOT_A_TABLE = "no match"


@dataclass(frozen=True)
class JumpTable:
    site: int
    kind: str                    # "table" | "computed"
    table: int | None            # LS address of the word table (None for "computed")
    targets: tuple[int, ...]     # sorted, without duplicates
    count: int                   # entries of the table, or values of the offset


@dataclass(frozen=True)
class JumpTables:
    tables: dict[int, JumpTable] = field(default_factory=dict)
    # Sites that looked like an idiom but could not be resolved, with the reason.
    notes: dict[int, str] = field(default_factory=dict)

    def data_ranges(self) -> list[tuple[int, int]]:
        """LS ranges [start, end) of the word tables, which are data inside code segments."""
        return sorted((t.table, t.table + 4 * t.count) for t in self.tables.values() if t.table is not None)


def resolve_jump_tables(code: SpuCode, sites: Iterable[int] | None = None,
                        zero_regs: frozenset[int] = frozenset()) -> JumpTables:
    """Resolve the indirect jumps (`bi $rX`, X != 0, and the conditional forms) of `code`.

    `sites` limits the search to the given instruction addresses (default: every indirect jump).
    """
    out = JumpTables()
    for addr in (sorted(sites) if sites is not None else code.addrs):
        if not code.contains(addr):
            continue
        insn = code.insn(addr)
        if insn.spec is None or flow_of(insn).kind not in ("indirect_jump", "cond_indirect_jump"):
            continue
        result = _match_table(code, insn, zero_regs)
        if result == _NOT_A_TABLE:
            result = _match_computed(code, insn, zero_regs)
        if isinstance(result, JumpTable):
            out.tables[addr] = result
        elif result != _NOT_A_TABLE:
            out.notes[addr] = result
    return out


def _same_def(code: SpuCode, a: int, b: int, reg: int) -> bool:
    """True when `reg` has the same defining instruction before `a` and before `b`."""
    da = find_def(code, a, reg, labels=_NO_LABELS, limit=_SCAN_LIMIT)
    db = find_def(code, b, reg, labels=_NO_LABELS, limit=_SCAN_LIMIT)
    return (da.addr if da else None) == (db.addr if db else None)


def _def(code: SpuCode, addr: int, reg: int, mnemonic: str) -> SpuInsn | None:
    d = find_def(code, addr, reg, labels=_NO_LABELS, limit=_SCAN_LIMIT)
    return d if d is not None and d.spec.mnemonic == mnemonic else None


def _const(code: SpuCode, addr: int, reg: int, zero_regs: frozenset[int]) -> int | None:
    return eval_const(code, addr, reg, labels=_NO_LABELS, limit=_SCAN_LIMIT, zero_regs=zero_regs)


def _match_table(code: SpuCode, branch: SpuInsn, zero_regs: frozenset[int]) -> JumpTable | str:
    site = branch.addr
    rot = _def(code, site, branch.field("RA"), "rotqby")
    if rot is None:
        return _NOT_A_TABLE
    load = _def(code, rot.addr, rot.field("RA"), "lqx")
    add = _def(code, rot.addr, rot.field("RB"), "a")
    if load is None or add is None:
        return _NOT_A_TABLE
    regs = (load.field("RA"), load.field("RB"))
    if sorted(regs) != sorted((add.field("RA"), add.field("RB"))):
        return _NOT_A_TABLE
    # The rotate amount and the loaded quadword must come from the same address.
    if not all(_same_def(code, load.addr, add.addr, r) for r in regs):
        return _NOT_A_TABLE

    base = scaled = None
    for first, second in (regs, regs[::-1]):
        value = _const(code, load.addr, first, zero_regs)
        if value is not None and _const(code, load.addr, second, zero_regs) is None:
            base, scaled = value, second
            break
    if base is None:
        return "table address is not a constant"
    if base % 4:
        return "table address 0x%x is not word aligned" % base

    shift = _def(code, load.addr, scaled, "shli")
    if shift is None or shift.field("I7") != 2:
        return "table index is not scaled by 4"
    index = shift.field("RA")

    count = _bound(code, site, index, shift.addr)
    if count is None:
        return "no bound check (clgti and brnz or binz) on the table index"

    targets = []
    for i in range(count):
        word = code.read_word(base + 4 * i)
        target = None if word is None else word & LS_TARGET_MASK
        if target is None or not code.valid(target):
            return "table entry %d at 0x%x is not a code address" % (i, base + 4 * i)
        targets.append(target)
    return JumpTable(site, "table", base, tuple(sorted(set(targets))), count)


def _bound(code: SpuCode, site: int, index: int, scaled_at: int) -> int | None:
    """Number of table entries allowed by an unsigned compare of `index` before `site`, or None."""
    pos = site
    for _ in range(_SCAN_LIMIT):
        pos -= 4
        if not code.contains(pos):
            return None
        insn = code.insn(pos)
        if flow_of(insn).kind in ("invalid", "jump", "return", "indirect_jump", "terminate", "call",
                                  "indirect_call"):
            return None
        if insn.spec.mnemonic != "clgti" or insn.field("RA") != index:
            continue
        limit = insn.field("I10")
        guard = insn.field("RT")
        # The index must be the value that was scaled, and the compare result must feed a taken
        # branch that leaves the table.
        if (0 <= limit < MAX_TABLE_ENTRIES and _same_def(code, insn.addr, scaled_at, index)
                and _guard_branch(code, insn.addr, site, guard)):
            return limit + 1
    return None


def _guard_branch(code: SpuCode, start: int, site: int, guard: int) -> bool:
    for addr in range(start + 4, site, 4):
        insn = code.insn(addr)
        name = insn.spec.mnemonic
        # brnz leaves to a default case; binz on the link register returns from the function instead.
        if name in ("brnz", "binz") and insn.field("RT") == guard:
            return True
        # A redefinition of the compare result before the branch breaks the pairing.
        if written_reg(insn) == guard:
            return False
    return False


def _match_computed(code: SpuCode, branch: SpuInsn, zero_regs: frozenset[int]) -> JumpTable | str:
    site = branch.addr
    add = _def(code, site, branch.field("RA"), "a")
    if add is None:
        return _NOT_A_TABLE
    for first, second in ((add.field("RA"), add.field("RB")), (add.field("RB"), add.field("RA"))):
        base = _const(code, add.addr, first, zero_regs)
        if base is None:
            continue
        shr = _def(code, add.addr, second, "rotmi")
        if shr is None:
            continue
        count = -shr.field("I7") & 0x3F
        if not 0 < count < 32:
            return "offset is not a right shift"
        mask = _def(code, shr.addr, shr.field("RA"), "andi")
        if mask is None:
            return "offset is not masked by andi"
        bits = mask.field("I10")
        if bits < 0 or bits.bit_count() > 6:
            return "offset mask 0x%x allows too many values" % bits
        targets = set()
        for v in _subsets(bits):
            target = (base + (v >> count)) & _MASK32
            if target % 4 or not code.valid(target):
                return "computed target 0x%x is not a code address" % target
            targets.add(target)
        return JumpTable(site, "computed", None, tuple(sorted(targets)), 1 << bits.bit_count())
    return _NOT_A_TABLE


def _subsets(mask: int) -> list[int]:
    """Every value v with v & mask == v, in increasing order."""
    out, sub = [], 0
    while True:
        out.append(sub)
        if sub == mask:
            return out
        sub = (sub - mask) & mask

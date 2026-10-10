"""Partition of a code image into functions: seeds are grown by control flow to a fixpoint."""

from __future__ import annotations

from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from .jumptab import JumpTable
from .ppu_flow import CodeImage, flow_of
from .seeds import ImportThunk, Seed

_INDIRECT_KINDS = frozenset({"indirect_jump", "cond_indirect_jump"})
_CALL_KINDS = frozenset({"call", "cond_call"})
_JUMP_KINDS = frozenset({"jump", "cond_jump"})


@dataclass(frozen=True)
class TailEdge:
    site: int
    target: int
    kind: str          # "branch" | "fallthrough" | "import"


@dataclass(frozen=True)
class RecoveredFunction:
    entry: int
    sources: frozenset[str]
    addrs: frozenset[int]                 # owned instruction addresses
    calls: frozenset[int]                 # direct call targets in the image
    import_calls: frozenset[int]          # thunk addresses reached by call kinds
    tail_edges: tuple[TailEdge, ...]
    indirect_sites: tuple[int, ...]       # bcctr LK=0 sites (resolved or not)
    external_targets: frozenset[int]      # direct targets outside the image, not thunks
    # Jump-table words inside the image that belong to this function's switches. They are data
    # and never part of `addrs`.
    data_addrs: frozenset[int] = frozenset()

    def ranges(self) -> list[tuple[int, int]]:
        return _merge(self.addrs)

    def data_ranges(self) -> list[tuple[int, int]]:
        return _merge(self.data_addrs)


@dataclass(frozen=True)
class Promotion:
    addr: int
    round: int
    reached_from: tuple[int, ...]         # starts whose flow reached `addr`
    edge_sites: tuple[int, ...]           # instructions entering `addr` from less-shared code


@dataclass(frozen=True)
class PartitionResult:
    functions: dict[int, RecoveredFunction]
    rounds: int
    converged: bool
    promotions: tuple[Promotion, ...]
    invalid_flow: tuple[tuple[int, int, int], ...]   # (function, from, to): into a non-valid word
    rejected_calls: tuple[tuple[int, int], ...]      # (site, target): call to a non-valid word


@dataclass(frozen=True)
class _Node:
    succ: tuple[tuple[int, str], ...]     # (address, edge kind) of intra-function successors
    call: int | None
    indirect: bool


class _Walk:
    """Result of following the flow from one start."""

    def __init__(self) -> None:
        self.visited: set[int] = set()
        self.tails: set[TailEdge] = set()
        self.calls: set[int] = set()
        self.import_calls: set[int] = set()
        self.external: set[int] = set()
        self.indirect: set[int] = set()
        self.invalid: set[tuple[int, int]] = set()      # (from, to)
        self.rejected: set[tuple[int, int]] = set()     # (site, target)
        self.new_calls: set[int] = set()


def _merge(addrs: Iterable[int]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for addr in sorted(addrs):
        if merged and merged[-1][1] == addr:
            merged[-1][1] = addr + 4
        else:
            merged.append([addr, addr + 4])
    return [(a, b) for a, b in merged]


def _edges(image: CodeImage, addr: int,
           jump_tables: Mapping[int, JumpTable]) -> tuple[tuple[int, str], ...]:
    flow = flow_of(image.insn(addr))
    edges: list[tuple[int, str]] = []
    if flow.falls_through:
        edges.append(((addr + 4) & 0xFFFFFFFF, "fallthrough"))
    if flow.kind in _JUMP_KINDS and flow.target is not None:
        edges.append((flow.target, "branch"))
    table = jump_tables.get(addr)
    if table is not None and flow.kind in _INDIRECT_KINDS:
        edges.extend((t, "branch") for t in sorted(set(table.targets)))
    seen: set[int] = set()
    unique = []
    for target, kind in edges:
        if target not in seen:
            seen.add(target)
            unique.append((target, kind))
    return tuple(unique)


def intra_successors(image: CodeImage, addr: int,
                     jump_tables: Mapping[int, JumpTable]) -> list[int]:
    """Fall-through, direct branch target (not calls) and jump-table targets; may leave the image."""
    return [v for v, _ in _edges(image, addr, jump_tables)]


def partition(image: CodeImage, seeds: Iterable[Seed], jump_tables: Mapping[int, JumpTable],
              thunks: Mapping[int, ImportThunk], max_rounds: int = 64) -> PartitionResult:
    # Words of in-image tables are data: flow into them is an anomaly, never code.
    table_words: set[int] = set()
    table_of: dict[int, frozenset[int]] = {}
    for branch, table in jump_tables.items():
        if not table.table_in_text:
            continue
        words = frozenset(a for a in (table.table + 4 * i for i in range(table.count))
                          if image.contains(a))
        table_of[branch] = words
        table_words |= words

    def usable(addr: int) -> bool:
        return image.valid(addr) and addr not in table_words

    nodes: dict[int, _Node] = {}

    def node(addr: int) -> _Node:
        cached = nodes.get(addr)
        if cached is None:
            flow = flow_of(image.insn(addr))
            cached = nodes[addr] = _Node(
                _edges(image, addr, jump_tables),
                flow.target if flow.kind in _CALL_KINDS else None,
                flow.kind in _INDIRECT_KINDS)
        return cached

    sources: dict[int, set[str]] = defaultdict(set)
    for seed in seeds:
        if usable(seed.addr):
            sources[seed.addr].add(seed.source)

    def walk(start: int) -> _Walk:
        out = _Walk()
        out.visited.add(start)
        stack = [start]
        while stack:
            u = stack.pop()
            info = node(u)
            if info.indirect:
                out.indirect.add(u)
            target = info.call
            if target is not None:
                if image.contains(target):
                    if usable(target):
                        out.calls.add(target)
                        if target not in sources:
                            out.new_calls.add(target)
                    else:
                        out.rejected.add((u, target))
                elif target in thunks:
                    out.import_calls.add(target)
                else:
                    out.external.add(target)
            for v, kind in info.succ:
                if not image.contains(v):
                    if v in thunks:
                        out.tails.add(TailEdge(u, v, "import"))
                    else:
                        out.external.add(v)
                elif v != start and v in sources:
                    out.tails.add(TailEdge(u, v, kind))
                elif v in out.visited:
                    continue
                elif not usable(v):
                    out.invalid.add((u, v))
                else:
                    out.visited.add(v)
                    stack.append(v)
        return out

    promotions: list[Promotion] = []
    rounds = 0
    converged = False
    while True:
        rounds += 1
        walks = {s: walk(s) for s in sorted(sources)}

        count: Counter[int] = Counter()
        for w in walks.values():
            count.update(w.visited)

        # A shared address is an entry of the shared region when some visited edge reaches it
        # from code reached by fewer starts (reach sets only grow along an edge).
        entry_sites: dict[int, set[int]] = defaultdict(set)
        if any(c >= 2 for c in count.values()):
            for w in walks.values():
                for u in w.visited:
                    cu = count[u]
                    for v, _ in node(u).succ:
                        if v in w.visited and count[v] > cu and count[v] >= 2:
                            entry_sites[v].add(u)

        new_calls: set[int] = set()
        for w in walks.values():
            new_calls |= w.new_calls
        added = new_calls | set(entry_sites)
        if not added:
            converged = True
            break
        if rounds >= max_rounds:
            break

        for v in sorted(entry_sites):
            reached = tuple(s for s, w in walks.items() if v in w.visited)
            promotions.append(Promotion(v, rounds, reached, tuple(sorted(entry_sites[v]))))
        for v in new_calls:
            sources[v].add("call")
        for v in entry_sites:
            sources[v].add("promoted")

    functions: dict[int, RecoveredFunction] = {}
    invalid: set[tuple[int, int, int]] = set()
    rejected: set[tuple[int, int]] = set()
    for start, w in walks.items():
        data: set[int] = set()
        for site in w.indirect:
            data |= table_of.get(site, frozenset())
        functions[start] = RecoveredFunction(
            entry=start,
            sources=frozenset(sources[start]),
            addrs=frozenset(w.visited),
            calls=frozenset(w.calls),
            import_calls=frozenset(w.import_calls),
            tail_edges=tuple(sorted(w.tails, key=lambda e: (e.site, e.target, e.kind))),
            indirect_sites=tuple(sorted(w.indirect)),
            external_targets=frozenset(w.external),
            data_addrs=frozenset(data))
        invalid.update((start, u, v) for u, v in w.invalid)
        rejected |= w.rejected

    return PartitionResult(functions, rounds, converged, tuple(promotions),
                           tuple(sorted(invalid)), tuple(sorted(rejected)))

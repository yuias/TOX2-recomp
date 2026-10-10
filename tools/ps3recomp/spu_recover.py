"""SPU function recovery: seeds are grown by control flow to a fixpoint, as `partition.py` does for the PPU.

Besides the function ranges, the result keeps the call/return structure (call sites, return sites,
indirect sites with their resolution) even though the stack-free lowering only needs the set of
dispatchable addresses.

Seed sources: `entry`, `extra` (registry extra entries), `brsl`/`brasl` (direct call targets),
`constant` (a constant code pointer that an indirect branch uses) and `promoted` (code shared by
several functions). Jump-table targets are intra-function edges, as on the PPU side; the tables
themselves are resolved by the caller and passed in.
"""

from __future__ import annotations

from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass

from .spu_flow import Flow, SpuCode, const_target, eval_const, flow_of

DEFAULT_MAX_ROUNDS = 64

_JUMP_INDIRECT = frozenset({"indirect_jump", "cond_indirect_jump"})
_RETURN_KINDS = frozenset({"return", "cond_return"})
_CALL_MNEMONICS = frozenset({"bisl", "bisled"})


@dataclass(frozen=True)
class CallSite:
    site: int
    kind: str              # "brsl" | "brasl"
    target: int
    link: int              # register that receives the return address

    @property
    def return_site(self) -> int:
        return self.site + 4


@dataclass(frozen=True)
class IndirectSite:
    site: int
    kind: str                       # base mnemonic: bi, biz, bisl, iret, ...
    resolution: str                 # "constant" | "jump_table" | "unresolved"
    targets: tuple[int, ...]        # constant or table targets
    link: int | None                # link register of bisl/bisled
    reason: str | None = None       # why a site is unresolved

    @property
    def is_call(self) -> bool:
        return self.kind in _CALL_MNEMONICS


@dataclass(frozen=True)
class ConstantTarget:
    site: int
    target: int
    image: str                      # "self": inside the image's code; "external": anywhere else


@dataclass(frozen=True)
class TailEdge:
    site: int
    target: int
    kind: str                       # "branch" | "fallthrough" | "constant"


@dataclass(frozen=True)
class RecoveredFunction:
    entry: int
    sources: frozenset[str]
    addrs: frozenset[int]                       # owned instruction addresses
    calls: frozenset[int]                       # call targets in the image (direct and constant)
    call_sites: tuple[CallSite, ...]            # direct calls
    returns: tuple[int, ...]                    # `bi $0` sites and conditional forms
    indirect_sites: tuple[IndirectSite, ...]    # every other indirect branch, including calls
    tail_edges: tuple[TailEdge, ...]
    external_targets: frozenset[int]            # targets outside the image's code

    def ranges(self) -> list[tuple[int, int]]:
        return _merge(self.addrs)

    def return_sites(self) -> tuple[int, ...]:
        """Address after each call (direct or indirect): where the callee's return lands."""
        sites = {c.return_site for c in self.call_sites}
        sites.update(i.site + 4 for i in self.indirect_sites if i.is_call)
        return tuple(sorted(sites))


@dataclass(frozen=True)
class Promotion:
    addr: int
    round: int
    reached_from: tuple[int, ...]       # starts whose flow reached `addr`
    edge_sites: tuple[int, ...]         # instructions entering `addr` from less-shared code


@dataclass(frozen=True)
class RecoveryResult:
    functions: dict[int, RecoveredFunction]
    rounds: int
    converged: bool
    promotions: tuple[Promotion, ...]
    invalid_flow: tuple[tuple[int, int, int], ...]      # (function, from, to): into a non-code word
    rejected_calls: tuple[tuple[int, int], ...]         # (site, target): call to a non-code word
    rejected_seeds: tuple[tuple[int, str], ...]         # (addr, source): seed that is not code
    constant_targets: tuple[ConstantTarget, ...]

    @property
    def unresolved_indirect(self) -> tuple[IndirectSite, ...]:
        out = [i for fn in self.functions.values() for i in fn.indirect_sites if i.resolution == "unresolved"]
        return tuple(sorted(out, key=lambda i: i.site))


@dataclass(frozen=True)
class _Node:
    flow: Flow
    succ: tuple[tuple[int, str], ...]       # (address, edge kind) of intra-function successors
    call: tuple[int, str] | None            # (target, seed source) of a call
    jump: int | None                        # constant target of an indirect jump
    call_site: CallSite | None
    indirect: IndirectSite | None


class _Walk:
    """Result of following the flow from one start."""

    def __init__(self) -> None:
        self.visited: set[int] = set()
        self.tails: set[TailEdge] = set()
        self.calls: set[int] = set()
        self.external: set[int] = set()
        self.invalid: set[tuple[int, int]] = set()      # (from, to)
        self.rejected: set[tuple[int, int]] = set()     # (site, target)
        self.new_seeds: dict[int, set[str]] = defaultdict(set)


def _merge(addrs: Iterable[int]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for addr in sorted(addrs):
        if merged and merged[-1][1] == addr:
            merged[-1][1] = addr + 4
        else:
            merged.append([addr, addr + 4])
    return [(a, b) for a, b in merged]


def recover(code: SpuCode, entry: int | None, extra_entries: Iterable[int] = (),
            jump_tables: Mapping[int, Sequence[int]] | None = None,
            data_ranges: Iterable[tuple[int, int]] = (),
            max_rounds: int = DEFAULT_MAX_ROUNDS) -> RecoveryResult:
    """Partition `code` into functions.

    `jump_tables` maps a `bi` site to its resolved targets. `data_ranges` are LS ranges
    [start, end) that hold data inside code segments; flow into them is an anomaly, never code.
    """
    tables = {site: tuple(sorted(set(t))) for site, t in (jump_tables or {}).items()}
    data_words: set[int] = set()
    for start, end in data_ranges:
        data_words.update(range(start & ~3, end, 4))

    def usable(addr: int) -> bool:
        return code.valid(addr) and addr not in data_words

    sources: dict[int, set[str]] = defaultdict(set)
    rejected_seeds: list[tuple[int, str]] = []
    initial = [(a, "extra") for a in extra_entries]
    if entry is not None:
        initial.append((entry, "entry"))
    for addr, source in initial:
        if usable(addr):
            sources[addr].add(source)
        else:
            rejected_seeds.append((addr, source))

    # Addresses that other paths can enter; backward constant evaluation stops there. Entries found
    # later are added to it, and the node cache is dropped whenever it changes.
    base_labels = set(code.direct_targets)
    for targets in tables.values():
        base_labels.update(targets)
    labels = frozenset(base_labels | set(sources))
    nodes: dict[int, _Node] = {}

    def node(addr: int) -> _Node:
        cached = nodes.get(addr)
        if cached is None:
            cached = nodes[addr] = _build_node(code, addr, tables, labels)
        return cached

    def walk(start: int) -> _Walk:
        out = _Walk()
        out.visited.add(start)
        stack = [start]
        while stack:
            u = stack.pop()
            info = node(u)
            if info.call is not None:
                target, source = info.call
                if code.contains(target):
                    if usable(target):
                        out.calls.add(target)
                        if target not in sources:
                            out.new_seeds[target].add(source)
                    else:
                        out.rejected.add((u, target))
                else:
                    out.external.add(target)
            if info.jump is not None:
                target = info.jump
                if code.contains(target):
                    if usable(target):
                        if target not in sources:
                            out.new_seeds[target].add("constant")
                        out.tails.add(TailEdge(u, target, "constant"))
                    else:
                        out.rejected.add((u, target))
                else:
                    out.external.add(target)
            for v, kind in info.succ:
                if not code.contains(v):
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

        new_seeds: dict[int, set[str]] = defaultdict(set)
        for w in walks.values():
            for target, found in w.new_seeds.items():
                new_seeds[target] |= found
        if not new_seeds and not entry_sites:
            converged = True
            break
        if rounds >= max_rounds:
            break

        for v in sorted(entry_sites):
            reached = tuple(s for s, w in walks.items() if v in w.visited)
            promotions.append(Promotion(v, rounds, reached, tuple(sorted(entry_sites[v]))))
        for v, found in new_seeds.items():
            sources[v] |= found
        for v in entry_sites:
            sources[v].add("promoted")
        labels = frozenset(base_labels | set(sources))
        nodes.clear()

    functions: dict[int, RecoveredFunction] = {}
    invalid: set[tuple[int, int, int]] = set()
    rejected: set[tuple[int, int]] = set()
    constants: list[ConstantTarget] = []
    for start, w in walks.items():
        call_sites: list[CallSite] = []
        returns: list[int] = []
        indirect: list[IndirectSite] = []
        for u in sorted(w.visited):
            info = node(u)
            if info.call_site is not None:
                call_sites.append(info.call_site)
            if info.flow.kind in _RETURN_KINDS:
                returns.append(u)
            if info.indirect is not None:
                indirect.append(info.indirect)
                if info.indirect.resolution == "constant":
                    target = info.indirect.targets[0]
                    constants.append(ConstantTarget(u, target, "self" if code.contains(target) else "external"))
        functions[start] = RecoveredFunction(
            entry=start,
            sources=frozenset(sources[start]),
            addrs=frozenset(w.visited),
            calls=frozenset(w.calls),
            call_sites=tuple(call_sites),
            returns=tuple(returns),
            indirect_sites=tuple(indirect),
            tail_edges=tuple(sorted(w.tails, key=lambda e: (e.site, e.target, e.kind))),
            external_targets=frozenset(w.external))
        invalid.update((start, u, v) for u, v in w.invalid)
        rejected |= w.rejected

    return RecoveryResult(functions, rounds, converged, tuple(promotions), tuple(sorted(invalid)),
                          tuple(sorted(rejected)), tuple(sorted(set(rejected_seeds))),
                          tuple(sorted(constants, key=lambda c: (c.site, c.target))))


def _build_node(code: SpuCode, addr: int, tables: Mapping[int, tuple[int, ...]],
                labels: frozenset[int]) -> _Node:
    insn = code.insn(addr)
    # Halts with a constant condition end the flow; memory is not consulted because a running
    # program may have changed the value that the image bytes hold.
    flow = flow_of(insn, lambda reg: _const_before(code, addr, reg, labels))
    name = insn.spec.mnemonic
    succ: list[tuple[int, str]] = []
    if flow.falls_through:
        succ.append((addr + 4, "fallthrough"))
    if flow.kind in ("jump", "cond_jump") and flow.target is not None:
        succ.append((flow.target, "branch"))

    call = jump = call_site = indirect = None
    if flow.kind == "call":
        call = (flow.target, name)
        call_site = CallSite(addr, name, flow.target, flow.link)
    elif flow.kind == "indirect_call" or flow.kind in _JUMP_INDIRECT:
        table = tables.get(addr) if flow.kind in _JUMP_INDIRECT else None
        if table is not None:
            succ.extend((t, "branch") for t in table)
            indirect = IndirectSite(addr, name, "jump_table", table, None)
        else:
            target = const_target(code, insn, flow, labels=labels, memory=True)
            if target is None:
                reason = ("iret: the target is SRR0" if flow.reg is None
                          else "$%d is not a recoverable constant" % flow.reg)
                indirect = IndirectSite(addr, name, "unresolved", (), flow.link, reason)
            else:
                indirect = IndirectSite(addr, name, "constant", (target,), flow.link)
                if flow.kind == "indirect_call":
                    call = (target, "constant")
                else:
                    jump = target

    seen: set[int] = set()
    unique = []
    for target, kind in succ:
        if target not in seen:
            seen.add(target)
            unique.append((target, kind))
    return _Node(flow, tuple(unique), call, jump, call_site, indirect)


def _const_before(code: SpuCode, addr: int, reg: int, labels: frozenset[int]) -> int | None:
    return eval_const(code, addr, reg, labels=labels, memory=False)

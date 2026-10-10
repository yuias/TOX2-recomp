"""SPU function recovery: seeds are grown by control flow to a fixpoint, as `partition.py` does for the PPU.

Besides the function ranges, the result keeps the call/return structure (call sites, return sites,
indirect sites with their resolution) even though the stack-free lowering only needs the set of
dispatchable addresses.

Seed sources: `entry`, `extra` (registry extra entries), `brsl`/`brasl` (direct call targets),
`constant` (a constant code pointer that an indirect branch uses, or that position-independent
code materialises with `a rX,rY,$delta`) and `promoted` (code shared by several functions). Jump-table targets are intra-function edges, as on the PPU side; the tables
themselves are resolved by the caller and passed in.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .info import EXIT_IO, REPO_ROOT, CliError
from .spu_flow import Flow, SpuCode, const_target, eval_const, flow_of
from .spu_images import (SCHEMA, SpuImage, write_if_changed, add_input_arguments, load_containers,
                         load_registry, registry_entry_sha256, resolve_image, select_entries)
from .spu_jumptab import JumpTables, resolve_jump_tables

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
    use: str = "branch"             # "branch": target of an indirect branch; "pointer": materialised only


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
    pointer: int | None                     # code address that position-independent code materialises
    call_site: CallSite | None
    indirect: IndirectSite | None


class _Walk:
    """Result of following the flow from one start."""

    def __init__(self) -> None:
        self.visited: set[int] = set()
        self.tails: set[TailEdge] = set()
        self.calls: set[int] = set()
        self.external: set[int] = set()
        self.pointers: set[tuple[int, int]] = set()     # (site, code address)
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
            max_rounds: int = DEFAULT_MAX_ROUNDS,
            zero_regs: Iterable[int] = (),
            table_notes: Mapping[int, str] | None = None) -> RecoveryResult:
    """Partition `code` into functions.

    `jump_tables` maps a `bi` site to its resolved targets. `data_ranges` are LS ranges
    [start, end) that hold data inside code segments; flow into them is an anomaly, never code.
    `zero_regs` hold 0 at the link address (see `spu_flow.eval_const`). `table_notes` explains why
    an indirect jump that looked like a table stays unresolved.
    """
    zero = frozenset(zero_regs)
    notes = dict(table_notes or {})
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
            cached = nodes[addr] = _build_node(code, addr, tables, labels, zero, notes)
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
            if info.pointer is not None and usable(info.pointer):
                out.pointers.add((u, info.pointer))
                if info.pointer not in sources:
                    out.new_seeds[info.pointer].add("constant")
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
    constants: set[ConstantTarget] = set()
    for start, w in walks.items():
        constants.update(ConstantTarget(site, target, "self", "pointer") for site, target in w.pointers)
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
                    constants.add(ConstantTarget(u, target, "self" if code.contains(target) else "external"))
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
                          tuple(sorted(constants, key=lambda c: (c.site, c.target, c.use))))


def _build_node(code: SpuCode, addr: int, tables: Mapping[int, tuple[int, ...]],
                labels: frozenset[int], zero_regs: frozenset[int], notes: Mapping[int, str]) -> _Node:
    insn = code.insn(addr)
    # Halts with a constant condition end the flow; memory is not consulted because a running
    # program may have changed the value that the image bytes hold.
    flow = flow_of(insn, lambda reg: _const_before(code, addr, reg, labels, zero_regs))
    name = insn.spec.mnemonic
    succ: list[tuple[int, str]] = []
    if flow.falls_through:
        succ.append((addr + 4, "fallthrough"))
    if flow.kind in ("jump", "cond_jump") and flow.target is not None:
        succ.append((flow.target, "branch"))

    call = jump = call_site = indirect = pointer = None
    if zero_regs and name == "a":
        # `a rX,rY,$delta` adds the load-time delta to an address; at the link address the sum is the
        # address itself, and the only things the compiler adds a delta to are pointers.
        for reg, other in ((insn.field("RA"), insn.field("RB")), (insn.field("RB"), insn.field("RA"))):
            if reg in zero_regs and other not in zero_regs:
                value = eval_const(code, addr, other, labels=labels, zero_regs=zero_regs)
                if value is not None and value % 4 == 0 and code.valid(value):
                    pointer = value
                break
    if flow.kind == "call":
        call = (flow.target, name)
        call_site = CallSite(addr, name, flow.target, flow.link)
    elif flow.kind == "indirect_call" or flow.kind in _JUMP_INDIRECT:
        table = tables.get(addr) if flow.kind in _JUMP_INDIRECT else None
        if table is not None:
            succ.extend((t, "branch") for t in table)
            indirect = IndirectSite(addr, name, "jump_table", table, None)
        else:
            target = const_target(code, insn, flow, labels=labels, memory=True, zero_regs=zero_regs)
            if target is None:
                reason = notes.get(addr) or ("iret: the target is SRR0" if flow.reg is None
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
    return _Node(flow, tuple(unique), call, jump, pointer, call_site, indirect)


def _const_before(code: SpuCode, addr: int, reg: int, labels: frozenset[int],
                  zero_regs: frozenset[int]) -> int | None:
    return eval_const(code, addr, reg, labels=labels, memory=False, zero_regs=zero_regs)


# --- Dispatchable blocks, database and report ------------------------------------------------

DEFAULT_DB_DIR = REPO_ROOT / "config" / "spu"
DEFAULT_REPORT = REPO_ROOT / "generated" / "analysis" / "spu_recovery_report.json"
EXIT_NOT_CONVERGED = 1
JSON_WIDTH = 110


@dataclass(frozen=True)
class ImageRecovery:
    image: SpuImage
    container_sha256: str
    code: SpuCode
    tables: JumpTables
    data_ranges: tuple[tuple[int, int], ...]
    result: RecoveryResult


def recover_image(image: SpuImage, container_sha256: str, *,
                  max_rounds: int = DEFAULT_MAX_ROUNDS) -> ImageRecovery:
    """Resolve jump tables, then partition the code of a registry image."""
    entry = image.entry
    code = SpuCode.from_image(image)
    zero = frozenset(entry.pic_delta_regs)
    tables = resolve_jump_tables(code, zero_regs=zero)
    data = tuple(sorted([(r.start, r.end) for r in entry.data_ranges] + tables.data_ranges()))
    result = recover(code, image.entry_ls, [e.ls for e in entry.extra_entries],
                     {site: t.targets for site, t in tables.tables.items()}, data, max_rounds,
                     zero, tables.notes)
    return ImageRecovery(image, container_sha256, code, tables, data, result)


def owned_addrs(result: RecoveryResult) -> set[int]:
    owned: set[int] = set()
    for fn in result.functions.values():
        owned |= fn.addrs
    return owned


def dispatchable_blocks(rec: ImageRecovery) -> dict[int, tuple[str, ...]]:
    """LS address -> why it is a dispatch target: the entry set of the stack-free lowering.

    The image entry, function entries, return sites of calls, resolved jump-table targets, constant
    code pointers and registry extra entries. Return sites that are not owned code (the call never
    returns and padding follows) are left out.
    """
    why: dict[int, set[str]] = defaultdict(set)
    owned = owned_addrs(rec.result)
    for fn in rec.result.functions.values():
        why[fn.entry] |= fn.sources
        for site in fn.return_sites():
            if site in owned:
                why[site].add("return")
    for table in rec.tables.tables.values():
        for target in table.targets:
            if target in owned:
                why[target].add("jump_table")
    for const in rec.result.constant_targets:
        if const.image == "self" and const.target in owned:
            why[const.target].add("constant")
    return {addr: tuple(sorted(found)) for addr, found in sorted(why.items())}


def _hex(value: int) -> str:
    return "0x%x" % value


def _function_doc(fn: RecoveredFunction) -> dict:
    indirect = []
    for site in sorted(fn.indirect_sites, key=lambda i: i.site):
        item: dict = {"site": _hex(site.site), "kind": site.kind, "resolution": site.resolution}
        if site.targets:
            item["targets"] = [_hex(t) for t in site.targets]
        if site.link is not None:
            item["link"] = site.link
        if site.reason:
            item["reason"] = site.reason
        indirect.append(item)
    return {
        "entry": _hex(fn.entry),
        "sources": sorted(fn.sources),
        "ranges": [[_hex(a), _hex(b)] for a, b in fn.ranges()],
        "call_sites": [{"site": _hex(c.site), "kind": c.kind, "target": _hex(c.target), "link": c.link}
                       for c in sorted(fn.call_sites, key=lambda c: c.site)],
        "returns": [_hex(r) for r in sorted(fn.returns)],
        "indirect": indirect,
    }


def build_database(rec: ImageRecovery) -> dict:
    """The function database of one image (addresses and hashes only)."""
    image, result = rec.image, rec.result
    return {
        "schema": SCHEMA,
        "image": image.name,
        "input": {"container_sha256": rec.container_sha256, "image_sha256": image.sha256,
                  "registry_entry_sha256": registry_entry_sha256(image.entry)},
        "segments": [{"ls": _hex(s.ls), "size": _hex(s.filesz), "code": s.code}
                     for s in sorted(image.segments, key=lambda s: s.ls)],
        "entry": _hex(image.entry_ls),
        "functions": [_function_doc(result.functions[e]) for e in sorted(result.functions)],
        "dispatch_seeds": [{"ls": _hex(a), "sources": list(w)} for a, w in dispatchable_blocks(rec).items()],
        "jump_tables": [{"site": _hex(t.site), "kind": t.kind,
                         "table": None if t.table is None else _hex(t.table),
                         "count": t.count, "targets": [_hex(x) for x in t.targets]}
                        for _, t in sorted(rec.tables.tables.items())],
        "constant_targets": [{"site": _hex(c.site), "target": _hex(c.target), "image": c.image, "use": c.use}
                             for c in result.constant_targets],
        "unresolved_indirect": [{"site": _hex(i.site), "kind": i.kind, "reason": i.reason}
                                for i in result.unresolved_indirect],
    }


def format_json(value, indent: int = 0) -> str:
    """Deterministic JSON that keeps short containers on one line, so diffs stay readable."""
    flat = json.dumps(value, ensure_ascii=True)
    if not isinstance(value, (dict, list)) or not value or indent + len(flat) <= JSON_WIDTH:
        return flat
    pad = " " * (indent + 2)
    if isinstance(value, dict):
        items = ["%s%s: %s" % (pad, json.dumps(k), format_json(v, indent + 2)) for k, v in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + " " * indent + "}"
    if all(not isinstance(v, (dict, list)) for v in value):
        lines: list[str] = []
        line = ""
        for v in value:
            text = json.dumps(v) + ","
            if line and len(pad) + len(line) + 1 + len(text) > JSON_WIDTH:
                lines.append(pad + line)
                line = ""
            line = line + " " + text if line else text
        lines.append(pad + line)
        lines[-1] = lines[-1].rstrip(",")
        return "[\n" + "\n".join(lines) + "\n" + " " * indent + "]"
    items = [pad + format_json(v, indent + 2) for v in value]
    return "[\n" + ",\n".join(items) + "\n" + " " * indent + "]"


def database_text(rec: ImageRecovery) -> bytes:
    return (format_json(build_database(rec)) + "\n").encode("ascii")


def build_report(rec: ImageRecovery) -> dict:
    result, code = rec.result, rec.code
    owned = owned_addrs(result)
    data_words: set[int] = set()
    for start, end in rec.data_ranges:
        data_words.update(range(start, end, 4))
    zero = [a for a in code.addrs if code.word(a) == 0 and a not in data_words]
    unowned = [a for a in code.addrs if a not in owned and a not in data_words and code.word(a) != 0]
    external = sorted({c.target for c in result.constant_targets if c.image == "external"}
                      | {t for fn in result.functions.values() for t in fn.external_targets})
    return {
        "image": rec.image.name,
        "gate": rec.image.entry.gate,
        "functions": len(result.functions),
        "rounds": result.rounds,
        "converged": result.converged,
        "code_words": {"owned": len(owned), "unowned": len(unowned), "zero": len(zero),
                       "data": len(data_words)},
        "unowned_ranges": [[_hex(a), _hex(b)] for a, b in _merge(unowned)],
        "promoted": [_hex(p.addr) for p in result.promotions],
        "jump_tables": len(rec.tables.tables),
        "unresolved_indirect": [{"site": _hex(i.site), "kind": i.kind, "reason": i.reason}
                                for i in result.unresolved_indirect],
        "external_targets": [_hex(t) for t in external],
        "invalid_flow": [[_hex(f), _hex(a), _hex(b)] for f, a, b in result.invalid_flow],
        "rejected_calls": [[_hex(a), _hex(b)] for a, b in result.rejected_calls],
        "rejected_seeds": [[_hex(a), s] for a, s in result.rejected_seeds],
        "dispatchable": [_hex(a) for a in dispatchable_blocks(rec)],
    }


def summary_line(rep: dict) -> str:
    words = rep["code_words"]
    return ("%s functions %d rounds %d owned %d unowned %d unresolved %d external %d promoted %d "
            "jump_tables %d dispatchable %d"
            % (rep["image"], rep["functions"], rep["rounds"], words["owned"], words["unowned"],
               len(rep["unresolved_indirect"]), len(rep["external_targets"]), len(rep["promoted"]),
               rep["jump_tables"], len(rep["dispatchable"])))


# --- spu-recover ------------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py spu-recover",
        description="Recover the functions of the SPU images in the registry and write one function "
                    "database per image (addresses only).")
    add_input_arguments(parser)
    parser.add_argument("--out", default=str(DEFAULT_DB_DIR),
                        help="database directory (default: config/spu)")
    parser.add_argument("--report", default=str(DEFAULT_REPORT),
                        help="report file (default: generated/analysis/spu_recovery_report.json)")
    return parser


def _make_dir(path: Path) -> None:
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot create %s: %s" % (path, e.strerror))


def main(argv: list[str]) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        registry = load_registry(args.registry)
        entries = select_entries(registry, args.images, include_position_independent=False)
        containers = load_containers(args.eboot, args.libsre, args.target_config)
        out_dir = Path(args.out)
        reports = []
        written = 0
        for entry in entries:
            image = resolve_image(entry, containers)
            rec = recover_image(image, containers[entry.container].sha256)
            if not rec.result.converged:
                raise CliError(EXIT_NOT_CONVERGED, "%s: recovery did not converge in %d rounds"
                               % (image.name, rec.result.rounds))
            _make_dir(out_dir)
            written += write_if_changed(out_dir / (image.name + ".json"), database_text(rec))
            report = build_report(rec)
            reports.append(report)
            print(summary_line(report))
        report_path = Path(args.report)
        _make_dir(report_path.parent)
        text = format_json({"schema": SCHEMA, "images": reports}) + "\n"
        write_if_changed(report_path, text.encode("ascii"))
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code
    print("%d databases, %d written" % (len(reports), written))
    return 0

"""`recover` command: seed, partition and write the function DB (no Ghidra needed)."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from collections import Counter
from pathlib import Path

from . import cell
from .coverage import build_coverage, coverage_lines
from .elf import ElfError, ElfFile
from .ghidra import _load_default_config
from .ghidra_summary import GhidraSummary, load_summary, write_compact_json
from .info import (EXIT_IO, EXIT_NOT_PPU_ELF, EXIT_PIN_MISMATCH, REPO_ROOT, CliError,
                   parse_ppu_elf, read_input, reject_encrypted, resolve_nid_table)
from .jumptab import JumpTable, UnresolvedJump, make_reader, recover_jump_tables
from .nids import DEFAULT_SYSCALLS, load_syscall_table
from .partition import PartitionResult, partition
from .ppu_flow import CodeImage
from .probe import resolve_syscall_number
from .seeds import (ImportThunk, SeedCensus, check_thunk_slots, collect_recovery_seeds,
                    collect_tocs, find_import_thunks, function_tocs, prologue_candidates)
from .syscall_cfg import count_sites_outside_text, resolve_syscalls

SCHEMA = 1
GENERATOR = "ps3tool recover"

EXIT_NOT_CONVERGED = 8

DEFAULT_SUMMARY = REPO_ROOT / "config" / "ghidra_summary.json"
DEFAULT_EXTRA_SEEDS = REPO_ROOT / "config" / "extra_seeds.json"
DEFAULT_OUTPUT = REPO_ROOT / "generated" / "analysis" / "functions.json"
DEFAULT_REPORT = REPO_ROOT / "generated" / "analysis" / "recovery_report.json"
DEFAULT_SEEDS_OUT = REPO_ROOT / "generated" / "analysis" / "ghidra_seeds.txt"

DEFAULT_MAX_ROUNDS = 64
_EXAMPLE_LIMIT = 50


def _hex(value: int | None) -> str | None:
    return None if value is None else "0x%08x" % value


def _hex_list(values) -> list[str]:
    return [_hex(v) for v in values]


def _capped(items: list) -> dict:
    return {"count": len(items), "examples": items[:_EXAMPLE_LIMIT]}


def _write_text(path: Path, text: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _load_extra_seeds(path: Path) -> list[int]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read extra seeds %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid extra seeds %s: %s" % (path, e))
    try:
        if doc["schema"] != SCHEMA:
            raise ValueError("unsupported schema %r" % doc["schema"])
        return [int(item["addr"], 16) for item in doc["seeds"]]
    except (KeyError, TypeError, ValueError, AttributeError) as e:
        raise CliError(EXIT_IO, "invalid extra seeds %s: %r" % (path, e))


def _load_summary_checked(path: Path, sha256: str,
                          text_range: tuple[int, int]) -> tuple[GhidraSummary, str]:
    summary = load_summary(path)
    if summary.sha256.lower() != sha256:
        raise CliError(EXIT_PIN_MISMATCH,
                       "Ghidra summary was made from a different ELF (SHA-256 %s, expected %s)"
                       % (summary.sha256, sha256))
    if summary.text != text_range:
        raise CliError(EXIT_PIN_MISMATCH,
                       "Ghidra summary text range 0x%08x-0x%08x differs from the ELF's "
                       "0x%08x-0x%08x" % (*summary.text, *text_range))
    try:
        file_sha = hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))
    return summary, file_sha


def _slot_mismatches(elf: ElfFile, thunks: dict[int, ImportThunk]) -> list[int]:
    try:
        return check_thunk_slots(elf, thunks)
    except ElfError:
        # A stub range that is not file-backed cannot load any slot.
        return sorted(thunks)


def _owner_map(result: PartitionResult) -> dict[int, int]:
    return {addr: entry for entry, fn in result.functions.items() for addr in fn.addrs}


def _build_db(elf_sha: str, summary_sha: str | None, extra_count: int,
              text_range: tuple[int, int], code_range: tuple[int, int], toc: int | None, summary: GhidraSummary | None,
              result: PartitionResult, thunks: dict[int, ImportThunk],
              tables: dict[int, JumpTable], unresolved: dict[int, UnresolvedJump],
              owner: dict[int, int]) -> dict:
    names = {} if summary is None else {f.entry: f.name for f in summary.functions}
    functions = []
    for entry in sorted(result.functions):
        fn = result.functions[entry]
        functions.append({
            "entry": _hex(entry),
            "name": names.get(entry),
            "sources": sorted(fn.sources),
            "ranges": [[_hex(a), _hex(b)] for a, b in fn.ranges()],
        })
    return {
        "schema": SCHEMA,
        "generator": GENERATOR,
        "input": {"executable_sha256": elf_sha, "ghidra_summary_sha256": summary_sha,
                  "extra_seeds": extra_count},
        "text": {"start": _hex(text_range[0]), "end": _hex(text_range[1])},
        "code": {"start": _hex(code_range[0]), "end": _hex(code_range[1])},
        "toc": _hex(toc),
        "functions": functions,
        "import_thunks": [
            {"addr": _hex(t.addr), "module": t.module, "fnid": _hex(t.fnid), "name": t.name,
             "slot": _hex(t.slot_addr)} for _, t in sorted(thunks.items())],
        "jump_tables": [
            {"branch": _hex(t.branch), "table": _hex(t.table), "kind": t.kind,
             "count": t.count, "targets": _hex_list(t.targets)}
            for _, t in sorted(tables.items())],
        "unresolved_indirect": [
            {"addr": _hex(u.branch), "function": _hex(owner.get(u.branch)), "reason": u.reason}
            for _, u in sorted(unresolved.items())],
    }


def _seeds_section(census: SeedCensus) -> dict:
    return {
        "counts": census.counts,
        "by_source": dict(sorted(Counter(s.source for s in census.seeds).items())),
        "rejected": [{"addr": _hex(a), "source": s, "reason": r}
                     for a, s, r in census.rejected],
    }


def _thunks_section(thunks: dict[int, ImportThunk], census: SeedCensus,
                    mismatches: list[int]) -> dict:
    never = sorted(set(thunks) - set(census.bl_sites))
    return {
        "count": len(thunks),
        "slot_mismatches": _hex_list(mismatches),
        "never_called": {"count": len(never), "examples": _hex_list(never[:_EXAMPLE_LIMIT])},
    }


def _jump_tables_section(tables: dict[int, JumpTable],
                         unresolved: dict[int, UnresolvedJump],
                         owner: dict[int, int], tocs: tuple[int, ...]) -> dict:
    entries = [t for table in tables.values() for t in table.targets]
    return {
        "tocs": _hex_list(tocs),
        "bcctr_sites": len(tables) + len(unresolved),
        "resolved": len(tables),
        "targets": len(entries),
        "distinct_targets": len(set(entries)),
        "in_text_tables": sum(1 for t in tables.values() if t.table_in_text),
        "unresolved_by_reason": dict(sorted(Counter(u.reason for u in unresolved.values())
                                            .items())),
        "unresolved": [{"addr": _hex(u.branch), "function": _hex(owner.get(u.branch)),
                        "reason": u.reason, "detail": u.detail}
                       for _, u in sorted(unresolved.items())],
    }


def _partition_section(result: PartitionResult, census: SeedCensus,
                       owner: dict[int, int]) -> dict:
    functions = result.functions
    by_source = Counter(src for fn in functions.values() for src in fn.sources)
    tail_kinds = Counter(e.kind for fn in functions.values() for e in fn.tail_edges)
    external = Counter(t for fn in functions.values() for t in fn.external_targets)
    suspect = sorted(
        entry for entry, fn in functions.items()
        if fn.sources == {"bl"}
        and not any(site in owner for site in census.bl_sites.get(entry, ())))
    return {
        "rounds": result.rounds,
        "converged": result.converged,
        "functions": len(functions),
        "by_source": dict(sorted(by_source.items())),
        "promoted": [{"addr": _hex(p.addr), "round": p.round,
                      "reached_from": _hex_list(p.reached_from),
                      "edge_sites": _hex_list(p.edge_sites)} for p in result.promotions],
        "tail_edges_by_kind": dict(sorted(tail_kinds.items())),
        # Fall-through into another start usually means a noreturn callee (not known here).
        "fallthrough_tail_edges": [
            {"function": _hex(fn.entry), "site": _hex(e.site), "target": _hex(e.target)}
            for _, fn in sorted(functions.items()) for e in fn.tail_edges
            if e.kind == "fallthrough"],
        "invalid_flow": _capped([{"function": _hex(f), "from": _hex(a), "to": _hex(b)}
                                 for f, a, b in result.invalid_flow]),
        "rejected_calls": _capped([{"site": _hex(s), "target": _hex(t)}
                                   for s, t in result.rejected_calls]),
        "external_targets": [{"target": _hex(t), "functions": n}
                             for t, n in sorted(external.items())],
        "suspect_bl_seeds": _capped(_hex_list(suspect)),
    }


def _syscalls_section(image: CodeImage, elf: ElfFile, code_range: tuple[int, int],
                      result: PartitionResult, tables: dict[int, JumpTable],
                      names: dict[int, str]) -> dict:
    sites = resolve_syscalls(image, result.functions, tables)
    outside = count_sites_outside_text(elf, code_range)
    resolved = [s for s in sites if s.number is not None]
    disagreements, probe_only, cfg_only = [], [], []
    for site in sites:
        probe = resolve_syscall_number(image.words, (site.addr - image.start) // 4)
        if probe is not None and site.number is not None:
            if probe != site.number:
                disagreements.append({"addr": _hex(site.addr), "probe": probe,
                                      "cfg": site.number})
        elif probe is not None:
            probe_only.append({"addr": _hex(site.addr), "probe": probe,
                               "reason": site.reason})
        elif site.number is not None:
            cfg_only.append(site)
    by_number = Counter(s.number for s in resolved)
    return {
        "sites": len(sites),
        "sites_outside_text": outside,
        "in_functions": sum(1 for s in sites if s.function is not None),
        "resolved": len(resolved),
        "distinct_numbers": len(by_number),
        "by_number": {str(n): {"count": c, "name": names.get(n)}
                      for n, c in sorted(by_number.items())},
        "unresolved": [{"addr": _hex(s.addr), "function": _hex(s.function), "reason": s.reason}
                       for s in sites if s.number is None],
        "probe_disagreements": disagreements,
        "probe_only": probe_only,
        "cfg_only": _hex_list(s.addr for s in cfg_only),
        "names": {_hex(s.addr): {"number": s.number, "name": names.get(s.number)}
                  for s in cfg_only},
    }


def _syscalls_line(doc: dict) -> str:
    return ("syscalls: %d sites in .text, %d outside, %d in functions, %d resolved by CFG, "
            "%d unresolved, %d disagree with probe"
            % (doc["sites"], doc["sites_outside_text"], doc["in_functions"], doc["resolved"],
               len(doc["unresolved"]), len(doc["probe_disagreements"])))


def _stdout_lines(census: SeedCensus, thunks_doc: dict, jt_doc: dict, part_doc: dict,
                  prologue: bool) -> list[str]:
    counts = census.counts
    by_source = Counter(s.source for s in census.seeds)
    prologue_text = "%d" % counts["prologue_seeds"] if prologue else "off"
    call_found = part_doc["by_source"].get("call", 0)
    return [
        "seeds: ghidra %d, opd %d, bl %d (%d not ghidra), entry %d, export %d, data_ptr %d, "
        "ghidra_data_ref %d, prologue %s, manual %d; union(ghidra+bl+opd) %d; rejected %d"
        % (counts["ghidra_functions"], counts["opd"], counts["bl_targets_in_text"],
           counts["bl_targets_not_ghidra"], by_source["entry"], by_source["export"],
           counts["data_ptr_seeds"], counts["ghidra_data_ref_seeds"], prologue_text,
           counts["manual"], counts["union_ghidra_bl_opd"], counts["rejected"]),
        "thunks: %d, slot mismatches %d, never called %d"
        % (thunks_doc["count"], len(thunks_doc["slot_mismatches"]),
           thunks_doc["never_called"]["count"]),
        "jump tables: %d bcctr sites, %d resolved (%d targets), %d unresolved"
        % (jt_doc["bcctr_sites"], jt_doc["resolved"], jt_doc["targets"],
           len(jt_doc["unresolved"])),
        "partition: %d rounds, %s, %d functions, %d promoted, %d call-discovered"
        % (part_doc["rounds"], "converged" if part_doc["converged"] else "NOT CONVERGED",
           part_doc["functions"], len(part_doc["promoted"]), call_found),
    ]


def _run(args: argparse.Namespace) -> int:
    started = time.monotonic()
    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    elf = parse_ppu_elf(data)
    if cell.has_unapplied_relocations(elf):
        raise CliError(EXIT_NOT_PPU_ELF, "PRX with unapplied relocations; pass the file written by "
                       "tox2_decrypt reloc (generated/inputs/libsre.reloc.elf)")

    try:
        text_range = cell.find_text_range(elf)
        code_range = cell.find_code_range(elf)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "no code range found: %s" % e)

    summary = None
    summary_sha = None
    if not args.no_ghidra:
        if args.ghidra_summary is not None:
            summary_path = Path(args.ghidra_summary)
        else:
            summary_path = DEFAULT_SUMMARY
            if not summary_path.is_file():
                raise CliError(EXIT_IO, "Ghidra summary %s not found; create it with "
                               "`ghidra-summary` or pass --no-ghidra" % summary_path)
        summary, summary_sha = _load_summary_checked(summary_path, sha256, text_range)

    extra: list[int] = []
    if args.extra_seeds is not None:
        extra = _load_extra_seeds(Path(args.extra_seeds))
    elif not cell.is_prx(elf) and DEFAULT_EXTRA_SEEDS.is_file():
        # The default seeds name EBOOT addresses; a PRX must not pick them up.
        extra = _load_extra_seeds(DEFAULT_EXTRA_SEEDS)

    nid_table = resolve_nid_table(args.nids)
    # Loaded before the long run so a bad path fails early.
    syscall_names: dict[int, str] = {}
    if args.syscalls is not None:
        syscall_names = load_syscall_table(Path(args.syscalls))
    elif DEFAULT_SYSCALLS.is_file():
        syscall_names = load_syscall_table(DEFAULT_SYSCALLS)
    else:
        print("note: syscall table %s not found; syscall names are not resolved"
              % DEFAULT_SYSCALLS, file=sys.stderr)

    try:
        entry = cell.parse_entry(elf)
        thunks = find_import_thunks(elf, nid_table)
        mismatches = _slot_mismatches(elf, thunks)
        image = CodeImage.from_elf(elf, code_range)
        tocs = collect_tocs(elf, image)
        tables, unresolved = recover_jump_tables(
            image, make_reader(elf), tocs, function_tocs(elf, image, tocs))
        table_targets = {t for table in tables.values() for t in table.targets}
        census = collect_recovery_seeds(
            elf, image, summary, thunks, jump_table_targets=table_targets, extra=extra,
            prologue=args.prologue_seeds, tocs=tocs)
        result = partition(image, census.seeds, tables, thunks, max_rounds=args.max_rounds)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)

    owner = _owner_map(result)
    thunks_doc = _thunks_section(thunks, census, mismatches)
    jt_doc = _jump_tables_section(tables, unresolved, owner, tocs)
    part_doc = _partition_section(result, census, owner)
    report = {
        "seeds": _seeds_section(census),
        "thunks": thunks_doc,
        "jump_tables": jt_doc,
        "partition": part_doc,
    }
    cov_lines: list[str] = []
    if result.converged:
        coverage, ghidra = build_coverage(image, result.functions, tables,
                                          prologue_candidates(image), summary)
        report["coverage"] = coverage
        if ghidra is not None:
            report["ghidra"] = ghidra
        cov_lines = coverage_lines(coverage, ghidra)
        try:
            report["syscalls"] = _syscalls_section(image, elf, code_range, result, tables,
                                                   syscall_names)
        except ElfError as e:
            raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)
        cov_lines.append(_syscalls_line(report["syscalls"]))
    report_path = Path(args.report) if args.report else DEFAULT_REPORT
    _write_text(report_path,
                json.dumps(report, indent=2, sort_keys=False, ensure_ascii=False) + "\n")

    for line in _stdout_lines(census, thunks_doc, jt_doc, part_doc, args.prologue_seeds):
        print(line)
    for line in cov_lines:
        print(line)

    if not result.converged:
        print("elapsed: %.1f s" % (time.monotonic() - started))
        raise CliError(EXIT_NOT_CONVERGED,
                       "partition did not converge within %d rounds; function DB not written"
                       % args.max_rounds)

    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    write_compact_json(out_path, _build_db(
        sha256, summary_sha, len(extra), text_range, code_range, entry.toc, summary, result, thunks,
        tables, unresolved, owner))

    ghidra_starts = set() if summary is None else {f.entry for f in summary.functions}
    own_starts = sorted(set(result.functions) - ghidra_starts)
    seeds_path = Path(args.seeds_out) if args.seeds_out else DEFAULT_SEEDS_OUT
    _write_text(seeds_path, "".join("0x%08x\n" % a for a in own_starts))

    print("elapsed: %.1f s" % (time.monotonic() - started))
    return 0


def _positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py recover",
        description="Recover functions from a decrypted PPU ELF: seed, partition to a fixpoint, "
                    "resolve jump tables and write the function DB and a recovery report.")
    parser.add_argument("elf", help="decrypted EBOOT.elf, or a PRX relocated by tox2_decrypt reloc")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--ghidra-summary", metavar="PATH",
                       help="Ghidra summary (default: config/ghidra_summary.json)")
    group.add_argument("--no-ghidra", action="store_true",
                       help="do not use a Ghidra summary")
    parser.add_argument("--extra-seeds", metavar="PATH",
                        help="extra function starts (default: config/extra_seeds.json if it "
                             "exists)")
    parser.add_argument("--nids", help="FNID -> name table (default: config/nids.json)")
    parser.add_argument("--syscalls", help="lv2 syscall name table "
                        "(default: config/lv2_syscalls.json)")
    parser.add_argument("-o", "--output", help="function DB path "
                        "(default: generated/analysis/functions.json)")
    parser.add_argument("--report", help="report path "
                        "(default: generated/analysis/recovery_report.json)")
    parser.add_argument("--seeds-out", help="list of recovered starts that Ghidra lacks "
                        "(default: generated/analysis/ghidra_seeds.txt)")
    parser.add_argument("--prologue-seeds", action="store_true",
                        help="also seed prologue candidates after a flow boundary")
    parser.add_argument("--max-rounds", type=_positive_int, default=DEFAULT_MAX_ROUNDS,
                        metavar="N", help="partition round limit (default: %(default)s)")
    return parser


def main(argv: list[str]) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return _run(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

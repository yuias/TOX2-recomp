"""`ghidra-export` command: run Ghidra headless on the ELF and validate its JSON export."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from collections import Counter
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

from . import cell
from .elf import ElfError, ElfFile
from .info import (DEFAULT_CONFIG, EXIT_IO, EXIT_NOT_PPU_ELF, EXIT_PIN_MISMATCH, REPO_ROOT,
                   CliError, load_config, parse_ppu_elf, read_input, reject_encrypted)
from .ppu import decode

DEFAULT_LANGUAGE = "PowerPC:BE:64:A2ALT-32addr"
DEFAULT_CSPEC = "default"
DEFAULT_TIMEOUT = 3600
GHIDRA_DIR_ENV = "GHIDRA_INSTALL_DIR"

GHIDRA_DIR = REPO_ROOT / "generated" / "ghidra"
DEFAULT_PROJECT_DIR = GHIDRA_DIR / "project"
DEFAULT_LOG_DIR = GHIDRA_DIR / "logs"
DEFAULT_OUTPUT = GHIDRA_DIR / "eboot_ghidra.json"
SCRIPT_DIR = REPO_ROOT / "tools" / "ghidra"

SCHEMA = 1
PROJECT_NAME = "eboot"

EXIT_BAD_ADDRESS = 5
EXIT_TOOL = 6
EXIT_LANGUAGE_CHECK = 7

# Extra time for Ghidra's own startup, import and script runtime beyond the analysis budget.
_TIMEOUT_SLACK = 300

_REQUIRED_KEYS = ("executable_sha256", "memory_blocks", "text", "functions", "words",
                  "jump_tables", "data_refs")

_PROBES = ("vmaddfp", "vperm", "lvx", "stvx", "lvlx", "lvrx")
# The first of these present in the ELF is the required VMX evidence.
_REQUIRED_ANY = ("vmaddfp", "vperm")
# Must match when present. lvlx/lvrx are Cell-only and merely informational.
_MUST_MATCH = ("vmaddfp", "vperm", "lvx", "stvx")


def resolve_ghidra_dir(arg: str | None, environ: Mapping[str, str]) -> Path:
    value = arg or environ.get(GHIDRA_DIR_ENV)
    if not value:
        raise CliError(EXIT_TOOL, "Ghidra install directory not set; pass --ghidra-dir or "
                       "set %s" % GHIDRA_DIR_ENV)
    return Path(value)


def headless_launcher(ghidra_dir: Path, os_name: str = os.name) -> Path:
    name = "analyzeHeadless.bat" if os_name == "nt" else "analyzeHeadless"
    launcher = ghidra_dir / "support" / name
    if not launcher.is_file():
        raise CliError(EXIT_TOOL, "Ghidra launcher not found: %s" % launcher)
    return launcher


def build_headless_command(*, launcher: Path, project_dir: Path, elf_path: Path,
                           out_path: Path, language: str, cspec: str, script_dir: Path,
                           log_dir: Path, toc: int | None, seeds: list[int],
                           text_range: tuple[int, int], timeout: int,
                           seeds_file: Path | None = None) -> list[str]:
    # A file keeps the command line short: cmd.exe rejects lines over ~8191 characters.
    seeds_args = [] if seeds_file is None else ["@%s" % seeds_file]
    toc_arg = "none" if toc is None else "0x%08x" % toc
    return [
        str(launcher), str(project_dir), PROJECT_NAME,
        "-import", str(elf_path), "-overwrite", "-loader", "ElfLoader",
        "-processor", language, "-cspec", cspec,
        "-scriptPath", str(script_dir),
        "-preScript", "Ps3Prepare.java", toc_arg, *["0x%08x" % s for s in seeds],
        *seeds_args,
        "-postScript", "ExportAnalysis.java", str(out_path),
        "0x%08x" % text_range[0], "0x%08x" % text_range[1],
        "-analysisTimeoutPerFile", str(timeout),
        "-log", str(log_dir / "application.log"),
        "-scriptlog", str(log_dir / "script.log"),
    ]


def collect_seeds(elf: ElfFile) -> tuple[int | None, list[int]]:
    """TOC base and the sorted unique function addresses known from the entry and exports."""
    try:
        entry = cell.parse_entry(elf)
        seeds = {entry.func}
        prx = cell.parse_prx_param(elf)
        if prx is not None:
            for module in cell.parse_exports(elf, prx):
                seeds.update(f.func for f in module.functions if f.func is not None)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)
    return entry.toc, sorted(seeds)


def parse_export_addr(value: object, what: str) -> int:
    if not isinstance(value, str):
        raise CliError(EXIT_IO, "invalid Ghidra export: %s is not an address string" % what)
    try:
        return int(value, 16)
    except ValueError:
        raise CliError(EXIT_IO, "invalid Ghidra export: bad address %r in %s" % (value, what))


def export_text_range(export: dict) -> tuple[int, int]:
    text = export["text"]
    if not isinstance(text, dict) or "start" not in text or "end" not in text:
        raise CliError(EXIT_IO, "invalid Ghidra export: malformed 'text'")
    return (parse_export_addr(text["start"], "text.start"),
            parse_export_addr(text["end"], "text.end"))


def _merged_blocks(export: dict) -> list[tuple[int, int]]:
    blocks = export["memory_blocks"]
    if not isinstance(blocks, list):
        raise CliError(EXIT_IO, "invalid Ghidra export: 'memory_blocks' is not a list")
    spans = []
    for block in blocks:
        if not isinstance(block, dict):
            raise CliError(EXIT_IO, "invalid Ghidra export: malformed memory block")
        spans.append((parse_export_addr(block.get("start"), "memory block start"),
                      parse_export_addr(block.get("end"), "memory block end")))
    spans.sort()
    merged: list[tuple[int, int]] = []
    for start, end in spans:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def _covered(merged: list[tuple[int, int]], start: int, end: int) -> bool:
    return any(s <= start and end <= e for s, e in merged)


def _uncovered(merged: list[tuple[int, int]], start: int, end: int) -> list[tuple[int, int]]:
    holes = []
    cur = start
    for s, e in merged:
        if e <= cur or s >= end:
            continue
        if s > cur:
            holes.append((cur, s))
        cur = max(cur, e)
    if cur < end:
        holes.append((cur, end))
    return holes


def validate_export(export: dict, elf: ElfFile, sha256: str,
                    check_memory_map: bool = True) -> None:
    if not isinstance(export, dict):
        raise CliError(EXIT_IO, "invalid Ghidra export: not a JSON object")
    if export.get("schema") != SCHEMA:
        raise CliError(EXIT_IO, "unsupported Ghidra export schema %r (expected %d)"
                       % (export.get("schema"), SCHEMA))
    missing = [k for k in _REQUIRED_KEYS if k not in export]
    if missing:
        raise CliError(EXIT_IO, "invalid Ghidra export: missing keys: %s" % ", ".join(missing))
    exported_sha = export["executable_sha256"]
    if not isinstance(exported_sha, str) or exported_sha.lower() != sha256:
        raise CliError(EXIT_PIN_MISMATCH,
                       "Ghidra export was made from a different ELF (SHA-256 %s, expected %s)"
                       % (exported_sha, sha256))
    for key in ("functions", "words", "jump_tables", "data_refs"):
        if not isinstance(export[key], list):
            raise CliError(EXIT_IO, "invalid Ghidra export: %r is not a list" % key)
    export_text_range(export)
    if not check_memory_map:
        return

    merged = _merged_blocks(export)
    # Ghidra's ELF loader maps sections, so the alignment padding between them is left
    # unmapped. Only holes with non-zero file content indicate a real mismatch.
    for seg in elf.loads():
        if not seg.p_filesz:
            continue
        for lo, hi in _uncovered(merged, seg.p_vaddr, seg.p_vaddr + seg.p_filesz):
            if any(elf.read_va(lo, hi - lo)):
                raise CliError(EXIT_TOOL, "Ghidra memory map does not match PT_LOAD segments: "
                               "file range 0x%08x-0x%08x is not covered" % (lo, hi))
    for seg in elf.loads():
        if seg.p_memsz > seg.p_filesz:
            lo, hi = seg.p_vaddr + seg.p_filesz, seg.p_vaddr + seg.p_memsz
            print("zero-fill 0x%08x-0x%08x: %s by Ghidra memory blocks"
                  % (lo, hi, "covered" if _covered(merged, lo, hi) else "not covered"))


@dataclass(frozen=True)
class LanguageCheck:
    probe: str
    addr: int | None
    ghidra: str | None
    status: str  # "ok" | "FAIL" | "info" | "absent"


def check_language(elf: ElfFile, export: dict) -> list[LanguageCheck]:
    start, end = export_text_range(export)
    try:
        raw = elf.read_va(start, (end - start) // 4 * 4)
    except ElfError as e:
        raise CliError(EXIT_IO, "Ghidra export text range is not readable from the ELF: %s" % e)

    by_addr = {}
    for entry in export["words"]:
        if isinstance(entry, dict) and "addr" in entry:
            by_addr[parse_export_addr(entry["addr"], "words.addr")] = entry

    # Per probe: first word of any kind, and first word Ghidra analysed as code.
    first_any: dict[str, int] = {}
    first_code: dict[str, int] = {}
    for i, (word,) in enumerate(struct.iter_unpack(">I", raw)):
        insn = decode(word)
        name = insn.spec.name if insn.spec is not None else None
        if name not in _PROBES:
            continue
        addr = start + 4 * i
        first_any.setdefault(name, addr)
        if name not in first_code and by_addr.get(addr, {}).get("kind") == "code":
            first_code[name] = addr

    results = []
    for probe in _PROBES:
        if probe not in first_any:
            results.append(LanguageCheck(probe, None, None, "absent"))
            continue
        addr = first_code.get(probe, first_any[probe])
        mnemonic = by_addr.get(addr, {}).get("mnemonic")
        ghidra = mnemonic.lower() if isinstance(mnemonic, str) else None
        if probe in _MUST_MATCH:
            status = "ok" if ghidra == probe else "FAIL"
        else:
            status = "info"
        results.append(LanguageCheck(probe, addr, ghidra, status))
    return results


def language_failure(checks: list[LanguageCheck]) -> str | None:
    if all(c.status == "absent" for c in checks if c.probe in _REQUIRED_ANY):
        return ("no VMX instruction (%s) found in the text range to check the language"
                % " or ".join(_REQUIRED_ANY))
    failed = [c.probe for c in checks if c.status == "FAIL"]
    if failed:
        return "Ghidra language does not decode VMX like our decoder: %s" % ", ".join(failed)
    return None


def _print_language_checks(checks: list[LanguageCheck]) -> None:
    for c in checks:
        if c.status == "absent":
            print("language check: %s not present in the text range: absent" % c.probe)
        else:
            print("language check: %s at 0x%08x -> ghidra %r: %s"
                  % (c.probe, c.addr, c.ghidra, c.status))


def _print_summary(export: dict, elapsed: float | None) -> None:
    kinds = Counter(w.get("kind") for w in export["words"] if isinstance(w, dict))
    print("functions: %d" % len(export["functions"]))
    print("words: code %d, data %d, undefined %d"
          % (kinds["code"], kinds["data"], kinds["undefined"]))
    print("jump tables: %d" % len(export["jump_tables"]))
    print("data refs: %d" % len(export["data_refs"]))
    if elapsed is not None:
        print("elapsed: %d s" % round(elapsed))


def _run_headless(cmd: list[str], log_dir: Path, timeout: int) -> float:
    console_log = log_dir / "console.log"
    started = time.monotonic()
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        with open(console_log, "wb") as log:
            proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=timeout + _TIMEOUT_SLACK)
    except subprocess.TimeoutExpired:
        raise CliError(EXIT_TOOL, "Ghidra timed out after %d s; see logs in %s"
                       % (timeout + _TIMEOUT_SLACK, log_dir))
    except OSError as e:
        raise CliError(EXIT_TOOL, "cannot run Ghidra: %s" % e.strerror)
    if proc.returncode != 0:
        raise CliError(EXIT_TOOL, "Ghidra exited with status %d; see logs in %s"
                       % (proc.returncode, log_dir))
    return time.monotonic() - started


def _load_default_config() -> dict | None:
    # The pin file is only used to recognise the encrypted EBOOT.BIN by hash;
    # a missing or broken file must not stop the export.
    try:
        return load_config(DEFAULT_CONFIG)
    except CliError:
        return None


def _require_exec_range(elf: ElfFile, start: int, end: int) -> None:
    for seg in elf.exec_segments():
        if seg.p_vaddr <= start and end <= seg.p_vaddr + seg.p_filesz:
            return
    raise CliError(EXIT_BAD_ADDRESS,
                   "range 0x%08x-0x%08x is not inside the file-backed part of an "
                   "executable segment" % (start, end))


def _load_export(path: Path) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid Ghidra export %s: %s" % (path, e))


def _run(args: argparse.Namespace) -> int:
    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    if args.seeds_file and not Path(args.seeds_file).is_file():
        raise CliError(EXIT_IO, "seeds file not found: %s" % args.seeds_file)
    launcher = None
    if not args.check_only:
        ghidra_dir = resolve_ghidra_dir(args.ghidra_dir, os.environ)
        launcher = headless_launcher(ghidra_dir)

    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    elf = parse_ppu_elf(data)

    if args.start is not None:
        text_range = (args.start, args.end)
        _require_exec_range(elf, *text_range)
    else:
        try:
            text_range = cell.find_text_range(elf)
        except ElfError as e:
            raise CliError(EXIT_NOT_PPU_ELF, "no code range found: %s" % e)

    elapsed = None
    if launcher is not None:
        toc, seeds = collect_seeds(elf)
        seeds_file = Path(args.seeds_file).resolve() if args.seeds_file else None
        project_dir = Path(args.project_dir).resolve()
        out_path = out_path.resolve()
        try:
            out_path.unlink(missing_ok=True)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            project_dir.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            raise CliError(EXIT_IO, "cannot prepare %s: %s" % (e.filename, e.strerror))
        cmd = build_headless_command(
            launcher=launcher.resolve(), project_dir=project_dir,
            elf_path=Path(args.elf).resolve(), out_path=out_path, language=args.language,
            cspec=args.cspec, script_dir=SCRIPT_DIR, log_dir=DEFAULT_LOG_DIR,
            toc=toc, seeds=seeds, text_range=text_range, timeout=args.timeout,
            seeds_file=seeds_file)
        elapsed = _run_headless(cmd, DEFAULT_LOG_DIR, args.timeout)
        if not out_path.is_file():
            raise CliError(EXIT_TOOL, "Ghidra finished without writing %s; see logs in %s"
                           % (out_path, DEFAULT_LOG_DIR))

    export = _load_export(out_path)
    validate_export(export, elf, sha256)
    checks = check_language(elf, export)
    _print_language_checks(checks)
    _print_summary(export, elapsed)
    failure = language_failure(checks)
    if failure is not None:
        raise CliError(EXIT_LANGUAGE_CHECK, failure)
    return 0


def _addr_arg(value: str) -> int:
    try:
        number = int(value, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid address: %r" % value)
    if number < 0:
        raise argparse.ArgumentTypeError("negative address: %r" % value)
    return number


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py ghidra-export",
        description="Analyse a decrypted PS3 PPU ELF with Ghidra headless and write a JSON "
                    "export of functions and the per-word listing of the code range.")
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("--ghidra-dir", metavar="DIR",
                        help="Ghidra install directory (default: $%s)" % GHIDRA_DIR_ENV)
    parser.add_argument("--language", default=DEFAULT_LANGUAGE, metavar="ID",
                        help="Ghidra language id (default: %(default)s)")
    parser.add_argument("--cspec", default=DEFAULT_CSPEC, metavar="ID",
                        help="compiler spec id (default: %(default)s)")
    parser.add_argument("--project-dir", default=str(DEFAULT_PROJECT_DIR), metavar="DIR",
                        help="Ghidra project directory (default: generated/ghidra/project)")
    parser.add_argument("-o", "--output", metavar="OUT",
                        help="export JSON path (default: generated/ghidra/eboot_ghidra.json)")
    parser.add_argument("--start", type=_addr_arg, metavar="ADDR",
                        help="first address of the exported code range "
                             "(default: largest executable section)")
    parser.add_argument("--end", type=_addr_arg, metavar="ADDR",
                        help="end address of the exported code range, exclusive")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, metavar="SEC",
                        help="Ghidra analysis timeout in seconds (default: %(default)s)")
    parser.add_argument("--seeds-file", metavar="PATH",
                        help="file of extra function starts, one hex address per line, "
                             "seeded in addition to the entry and exports")
    parser.add_argument("--check-only", action="store_true",
                        help="do not run Ghidra; validate and language-check an existing export")
    return parser


def _validate(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if (args.start is None) != (args.end is None):
        parser.error("--start and --end must be given together")
    for name in ("start", "end"):
        value = getattr(args, name)
        if value is not None and value % 4:
            parser.error("--%s must be 4-byte aligned" % name)
    if args.start is not None and args.end <= args.start:
        parser.error("--end must be greater than --start")
    if args.timeout < 1:
        parser.error("--timeout must be at least 1")


def main(argv: list[str]) -> int:
    parser = _parser()
    try:
        args = parser.parse_args(argv)
        _validate(parser, args)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return _run(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

"""`emit` command: turn the function DB and the decrypted ELF into C source for the runtime."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from dataclasses import replace
from pathlib import Path

from . import cell
from .elf import ElfError
from .emit_core import EmitEnv, EmitStats, emit_function
from .emit_db import census_doc, census_lines, check_db_matches, load_function_db, take_census
from .emit_output import (ExportRow, FunctionRecord, build_report, func_name, import_name,
                          read_thunk_words, render_trampoline, report_lines, write_output)
from .emit_overrides import (DEFAULT_OVERRIDES, check_pin, generated_name, load_overrides,
                             report_section)
from .ghidra import _load_default_config
from .info import (EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError, parse_ppu_elf, read_input,
                   reject_encrypted)
from .ppu_flow import CodeImage

EXIT_USAGE = 2
EXIT_UNHANDLED = 9

PREFIX_PATTERN = re.compile(r"[a-z][a-z0-9]*")

DEFAULT_DB = REPO_ROOT / "config" / "functions.json"
DEFAULT_OUT = REPO_ROOT / "generated" / "ppu"
DEFAULT_REPORT = REPO_ROOT / "generated" / "analysis" / "emit_report.json"
DEFAULT_CENSUS = REPO_ROOT / "generated" / "analysis" / "emit_census.json"

DEFAULT_CHUNKS = 16


def _write_text(path: Path, text: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _default_analysis_path(default: Path, prefix: str) -> Path:
    return default.with_name("%s_%s" % (prefix, default.name)) if prefix else default


def collect_exports(elf, db) -> list[ExportRow]:
    """One row per function export of the module info, sorted by (module, fnid).

    Duplicate FNIDs that share a descriptor stay as separate rows; the runtime decides what a
    duplicate means. Every descriptor must name a recovered function, since the runtime binds
    imports to it and dispatches on its entry.
    """
    tables = cell.library_tables(elf)
    if tables is None:
        return []
    entries = {fn.entry for fn in db.functions}
    rows = []
    try:
        modules = cell.parse_exports(elf, tables)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)
    for module in modules:
        for export in module.functions:
            if module.name is None:
                raise CliError(EXIT_NOT_PPU_ELF, "export fnid 0x%08x (descriptor 0x%08x) belongs "
                               "to a module without a name" % (export.fnid, export.opd_addr))
            if export.func not in entries:
                target = ("an unreadable descriptor" if export.func is None else
                          "0x%08x, which is not a function in the database" % export.func)
                raise CliError(EXIT_NOT_PPU_ELF, "export %s fnid 0x%08x: descriptor 0x%08x names %s"
                               % (module.name, export.fnid, export.opd_addr, target))
            rows.append(ExportRow(export.opd_addr, export.fnid, module.name))
    rows.sort(key=lambda row: (row.module, row.fnid))
    return rows


def _run(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    prefix = args.symbol_prefix or ""
    if prefix and args.overrides:
        raise CliError(EXIT_USAGE, "--overrides cannot be combined with --symbol-prefix; "
                       "overrides apply to the main executable only")
    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    elf = parse_ppu_elf(data)
    # The main executable is always the unprefixed image, so a second image cannot shadow it.
    if cell.is_prx(elf) and not prefix:
        raise CliError(EXIT_USAGE, "a PRX input needs --symbol-prefix")
    if prefix and not cell.is_prx(elf):
        raise CliError(EXIT_USAGE, "--symbol-prefix is only for PRX inputs; the main executable "
                       "is emitted without a prefix")
    if cell.has_unapplied_relocations(elf):
        raise CliError(EXIT_NOT_PPU_ELF, "PRX with unapplied relocations; pass the file written by "
                       "tox2_decrypt reloc (generated/inputs/libsre.reloc.elf)")
    try:
        text_range = cell.find_text_range(elf)
        code_range = cell.find_code_range(elf)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "no code range found: %s" % e)

    db = load_function_db(Path(args.db) if args.db else DEFAULT_DB)
    check_db_matches(db, sha256, text_range, code_range)
    try:
        image = CodeImage.from_elf(elf, code_range)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)

    census = take_census(image, db)
    census_path = (Path(args.census_out) if args.census_out
                   else _default_analysis_path(DEFAULT_CENSUS, prefix))
    _write_text(census_path, json.dumps(census_doc(census), indent=2, ensure_ascii=False) + "\n")
    if args.census:
        for line in census_lines(census):
            print(line)
        return 0

    # A prefixed image never gets the default file, whose addresses belong to the executable.
    overrides = [] if prefix else load_overrides(
        Path(args.overrides) if args.overrides else DEFAULT_OVERRIDES,
        explicit=bool(args.overrides), entries=[fn.entry for fn in db.functions])
    overridden = {o.entry: o for o in overrides}

    env = EmitEnv(
        names={fn.entry: func_name(fn.entry, prefix) for fn in db.functions},
        thunks={t.addr: import_name(t.addr, prefix) for t in db.thunks},
        switch_targets={branch: tuple(sorted(set(table.targets)))
                        for branch, table in db.jump_tables.items()},
        comments=args.comments)
    stats = EmitStats()
    functions = []
    for fn in db.functions:
        addrs = fn.addrs()
        name = func_name(fn.entry, prefix)
        override = overridden.get(fn.entry)
        if override is not None:
            # The pin covers the plain body whatever --comments says. Its stats are thrown away so
            # that the report counts the function once.
            check_pin(override, emit_function(image, fn.entry, addrs, replace(env, comments=False),
                                              EmitStats(), name=name, title=fn.name))
            name = generated_name(name)
        text = emit_function(image, fn.entry, addrs, env, stats, name=name, title=fn.name)
        functions.append(FunctionRecord(fn.entry, len(addrs), text))

    thunk_texts = []
    without_toc_save = []
    for index, thunk in enumerate(db.thunks):
        text, saves = render_trampoline(thunk, index, read_thunk_words(elf, thunk.addr), prefix)
        thunk_texts.append(text)
        if not saves:
            without_toc_save.append(thunk.addr)

    exports = collect_exports(elf, db) if prefix else []
    out_dir = Path(args.out) if args.out else DEFAULT_OUT
    summary = write_output(out_dir, sha256, functions, db.thunks, thunk_texts, args.chunks,
                           prefix, exports)
    report = build_report(sha256=sha256, db_sha256=db.file_sha256, stats=stats,
                          functions=functions, thunks=db.thunks, without_toc_save=without_toc_save,
                          summary=summary, overrides=report_section(overrides),
                          elapsed=time.perf_counter() - started)
    report_path = (Path(args.report) if args.report
                   else _default_analysis_path(DEFAULT_REPORT, prefix))
    _write_text(report_path, json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    for line in report_lines(report, str(out_dir)):
        print(line)
    if args.max_unhandled is not None and report["unhandled"]["total"] > args.max_unhandled:
        return EXIT_UNHANDLED
    return 0


def _positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def _non_negative_int(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be at least 0")
    return number


def _symbol_prefix(value: str) -> str:
    if not PREFIX_PATTERN.fullmatch(value):
        raise argparse.ArgumentTypeError("must match [a-z][a-z0-9]*")
    return value


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py emit",
        description="Emit C source for the recovered functions of a decrypted PPU ELF.")
    parser.add_argument("elf", help="decrypted EBOOT.elf, or a relocated PRX with --symbol-prefix")
    parser.add_argument("--db", metavar="PATH",
                        help="function DB (default: config/functions.json)")
    parser.add_argument("--out", metavar="DIR",
                        help="output directory (default: generated/ppu)")
    parser.add_argument("--chunks", type=_positive_int, default=DEFAULT_CHUNKS, metavar="N",
                        help="number of generated source chunks (default: %(default)s)")
    parser.add_argument("--comments", action="store_true",
                        help="annotate generated code with the guest disassembly")
    parser.add_argument("--report", metavar="PATH",
                        help="report path (default: generated/analysis/emit_report.json)")
    parser.add_argument("--census", action="store_true",
                        help="only count the owned instructions by mnemonic")
    parser.add_argument("--census-out", metavar="PATH",
                        help="census path (default: generated/analysis/emit_census.json)")
    parser.add_argument("--symbol-prefix", type=_symbol_prefix, metavar="NAME",
                        help="prefix of every generated file name and global symbol; required "
                             "for a PRX, refused for the main executable")
    parser.add_argument("--overrides", metavar="PATH",
                        help="guest function override file (default: config/ppu_overrides.json, "
                             "optional); refused with --symbol-prefix")
    parser.add_argument("--max-unhandled", type=_non_negative_int, metavar="N",
                        help="exit 9 if more than N words are emitted as unhandled")
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

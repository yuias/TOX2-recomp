"""`ghidra-summary` command: reduce a Ghidra export to the compact, committable facts."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from .elf import ElfFile
from .ghidra import _load_default_config, _load_export, export_text_range, parse_export_addr, \
    validate_export
from .info import EXIT_IO, REPO_ROOT, CliError, parse_ppu_elf, read_input, reject_encrypted

SCHEMA = 1
GENERATOR = "ps3tool ghidra-summary"
DEFAULT_OUTPUT = REPO_ROOT / "generated" / "ghidra" / "eboot_ghidra_summary.json"

_DEFAULT_NAME = re.compile(r"^(\.opd\.)?FUN_[0-9a-f]{8}$")
_BCTR_MNEMONICS = ("bctr", "bcctr")
_KINDS = ("code", "data", "undefined")


def _hex(value: int) -> str:
    return "0x%08x" % value


def _bad(what: str) -> CliError:
    return CliError(EXIT_IO, "invalid Ghidra export: %s" % what)


def write_compact_json(path: Path, doc: dict) -> None:
    """Top-level keys on their own lines; each array element on one line, no indentation."""
    parts = []
    for key, value in doc.items():
        head = json.dumps(key) + ": "
        if isinstance(value, list) and value:
            body = ",\n".join(json.dumps(el) for el in value)
            parts.append(head + "[\n" + body + "\n]")
        else:
            parts.append(head + json.dumps(value))
    text = "{\n" + ",\n".join(parts) + "\n}\n"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _word_runs(words: list) -> tuple[Counter, dict[str, list[list[str]]]]:
    """Per-kind word counts and the maximal contiguous code/data runs (end-exclusive)."""
    kinds: Counter = Counter()
    runs: dict[str, list[list[int]]] = {"code": [], "data": []}
    prev_addr = None
    prev_kind = None
    for entry in words:
        if not isinstance(entry, dict):
            raise _bad("malformed word entry")
        addr = parse_export_addr(entry.get("addr"), "words.addr")
        kind = entry.get("kind")
        if kind not in _KINDS:
            raise _bad("bad kind %r at 0x%08x" % (kind, addr))
        kinds[kind] += 1
        if kind in runs:
            run = runs[kind]
            if prev_kind == kind and prev_addr is not None and addr == prev_addr + 4:
                run[-1][1] = addr + 4
            else:
                run.append([addr, addr + 4])
        prev_addr, prev_kind = addr, kind
    return kinds, {k: [[_hex(a), _hex(b)] for a, b in v] for k, v in runs.items()}


def _functions(export: dict) -> list[dict]:
    out = []
    for fn in export["functions"]:
        if not isinstance(fn, dict):
            raise _bad("malformed function entry")
        entry = parse_export_addr(fn.get("entry"), "functions.entry")
        name = fn.get("name")
        if not isinstance(name, (str, type(None))):
            raise _bad("bad function name at 0x%08x" % entry)
        if name is not None and _DEFAULT_NAME.match(name):
            name = None
        body = fn.get("body")
        if not isinstance(body, list):
            raise _bad("bad function body at 0x%08x" % entry)
        spans = sorted((parse_export_addr(b[0], "functions.body"),
                        parse_export_addr(b[1], "functions.body"))
                       for b in (_pair(x) for x in body))
        out.append({"entry": _hex(entry), "name": name, "thunk": bool(fn.get("thunk")),
                    "body": [[_hex(a), _hex(b)] for a, b in spans]})
    out.sort(key=lambda f: f["entry"])
    return out


def _pair(value: object) -> list:
    if not isinstance(value, list) or len(value) != 2:
        raise _bad("malformed function body range")
    return value


def _data_refs(export: dict) -> list[dict]:
    out = []
    for ref in export["data_refs"]:
        if not isinstance(ref, dict) or not isinstance(ref.get("type"), str):
            raise _bad("malformed data ref")
        src = parse_export_addr(ref.get("from"), "data_refs.from")
        dst = parse_export_addr(ref.get("to"), "data_refs.to")
        out.append({"from": _hex(src), "to": _hex(dst), "type": ref["type"],
                    "from_exec": bool(ref.get("from_exec"))})
    out.sort(key=lambda r: (r["from"], r["to"], r["type"]))
    return out


def _bctr_sites(export: dict) -> list[dict]:
    tables: dict[int, set[int]] = {}
    for entry in export["jump_tables"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("targets"), list):
            raise _bad("malformed jump table entry")
        branch = parse_export_addr(entry.get("branch"), "jump_tables.branch")
        tables.setdefault(branch, set()).update(
            parse_export_addr(t, "jump_tables.targets") for t in entry["targets"])
    sites = []
    for word in export["words"]:
        if word.get("kind") != "code" or not isinstance(word.get("mnemonic"), str):
            continue
        mnemonic = word["mnemonic"].rstrip("+-")
        if mnemonic not in _BCTR_MNEMONICS:
            continue
        addr = parse_export_addr(word.get("addr"), "words.addr")
        sites.append({"addr": _hex(addr), "mnemonic": mnemonic,
                      "targets": [_hex(t) for t in sorted(tables.get(addr, ()))]})
    sites.sort(key=lambda s: s["addr"])
    return sites


def build_summary(export: dict) -> dict:
    """The summary document for a validated export."""
    text_start, text_end = export_text_range(export)
    kinds, runs = _word_runs(export["words"])
    return {
        "schema": SCHEMA,
        "generator": GENERATOR,
        "source": {"ghidra_version": export.get("ghidra_version"),
                   "language": export.get("language"),
                   "compiler_spec": export.get("compiler_spec"),
                   "executable_sha256": export["executable_sha256"].lower()},
        "text": {"start": _hex(text_start), "end": _hex(text_end)},
        "word_kinds": {k: kinds[k] for k in _KINDS},
        "functions": _functions(export),
        "code_ranges": runs["code"],
        "data_ranges": runs["data"],
        "data_refs": _data_refs(export),
        "bctr_sites": _bctr_sites(export),
    }


@dataclass(frozen=True)
class GhidraFunction:
    entry: int
    name: str | None
    thunk: bool
    body: tuple[tuple[int, int], ...]


@dataclass(frozen=True)
class GhidraSummary:
    sha256: str
    text: tuple[int, int]
    functions: tuple[GhidraFunction, ...]
    code_ranges: tuple[tuple[int, int], ...]
    data_ranges: tuple[tuple[int, int], ...]
    data_refs: tuple[tuple[int, int, str, bool], ...]  # (from, to, type, from_exec)
    bctr_sites: tuple[tuple[int, tuple[int, ...]], ...]


def _int(value: str) -> int:
    return int(value, 16)


def _parse_summary(doc: dict) -> GhidraSummary:
    if doc["schema"] != SCHEMA:
        raise ValueError("unsupported schema %r" % doc["schema"])
    return GhidraSummary(
        sha256=doc["source"]["executable_sha256"],
        text=(_int(doc["text"]["start"]), _int(doc["text"]["end"])),
        functions=tuple(
            GhidraFunction(_int(f["entry"]), f["name"], bool(f["thunk"]),
                           tuple((_int(a), _int(b)) for a, b in f["body"]))
            for f in doc["functions"]),
        code_ranges=tuple((_int(a), _int(b)) for a, b in doc["code_ranges"]),
        data_ranges=tuple((_int(a), _int(b)) for a, b in doc["data_ranges"]),
        data_refs=tuple((_int(r["from"]), _int(r["to"]), r["type"], bool(r["from_exec"]))
                        for r in doc["data_refs"]),
        bctr_sites=tuple((_int(s["addr"]), tuple(_int(t) for t in s["targets"]))
                         for s in doc["bctr_sites"]))


def load_summary(path: Path) -> GhidraSummary:
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid Ghidra summary %s: %s" % (path, e))
    try:
        return _parse_summary(doc)
    except (KeyError, TypeError, ValueError, IndexError) as e:
        raise CliError(EXIT_IO, "invalid Ghidra summary %s: %r" % (path, e))


def _print_summary(doc: dict, size: int) -> None:
    start, end = _int(doc["text"]["start"]), _int(doc["text"]["end"])
    funcs = doc["functions"]
    in_text = sum(1 for f in funcs if start <= _int(f["entry"]) < end)
    named = sum(1 for f in funcs if f["name"] is not None)
    print("functions: %d (%d in .text, %d named)" % (len(funcs), in_text, named))
    print("code ranges: %d" % len(doc["code_ranges"]))
    print("data ranges: %d" % len(doc["data_ranges"]))
    print("data refs: %d" % len(doc["data_refs"]))
    print("bctr sites: %d" % len(doc["bctr_sites"]))
    print("size: %d bytes" % size)


def _run(args: argparse.Namespace) -> int:
    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    elf: ElfFile = parse_ppu_elf(data)
    export = _load_export(Path(args.ghidra))
    # validate_export reports zero-fill coverage on stdout; not part of this command's output.
    with contextlib.redirect_stdout(io.StringIO()):
        validate_export(export, elf, sha256)

    doc = build_summary(export)
    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    write_compact_json(out_path, doc)
    _print_summary(doc, out_path.stat().st_size)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py ghidra-summary",
        description="Reduce a Ghidra export to function bodies, code/data ranges, data "
                    "references and indirect-branch sites in a small, committable file.")
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("--ghidra", required=True, metavar="EXPORT",
                        help="export written by ghidra-export")
    parser.add_argument("-o", "--output", metavar="OUT",
                        help="summary JSON path (default: "
                             "generated/ghidra/eboot_ghidra_summary.json)")
    return parser


def main(argv: list[str]) -> int:
    parser = _parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return _run(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

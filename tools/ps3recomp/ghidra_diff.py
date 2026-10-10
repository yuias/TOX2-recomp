"""`ghidra-diff` command: compare our decoder with a Ghidra export word by word."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from collections import Counter
from pathlib import Path

from . import ghidra_norm
from .disasm import _write_json
from .elf import ElfError, ElfFile
from .ghidra import (EXIT_BAD_ADDRESS, _addr_arg, _load_default_config, _load_export,
                     _require_exec_range, export_text_range, parse_export_addr, validate_export)
from .info import (EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError, parse_ppu_elf, read_input,
                   reject_encrypted)
from .ppu import Insn, decode
from .ppu_format import format_insn

SCHEMA = 1
DEFAULT_OUTPUT = REPO_ROOT / "generated" / "analysis" / "ghidra_diff.json"
DEFAULT_TOP = 30
DEFAULT_EXAMPLES = 5
STDOUT_TOP = 10

KINDS = ("code", "data", "undefined")
CATEGORIES = ("match", "known_difference", "ghidra_code_we_unknown", "mnemonic_mismatch",
              "operand_mismatch", "we_code_ghidra_noncode", "both_noncode_unknown")
PSEUDO_COUNTERS = ("agree", "mnemonic_mismatch", "operand_mismatch", "ghidra_bad", "we_unknown")
BUG_CATEGORIES = ("ghidra_code_we_unknown", "mnemonic_mismatch", "operand_mismatch")

# Keys of the report's "pairs" object, in output order.
PAIR_KEYS = ("known_difference", "ghidra_code_we_unknown", "mnemonic_mismatch",
             "operand_mismatch", "we_code_ghidra_noncode", "noncode_mnemonic_mismatch",
             "noncode_operand_mismatch", "noncode_ghidra_bad")
_PSEUDO_PAIR_KEYS = {"mnemonic_mismatch": "noncode_mnemonic_mismatch",
                     "operand_mismatch": "noncode_operand_mismatch",
                     "ghidra_bad": "noncode_ghidra_bad"}


def _ghidra_text(mnemonic: str | None, operands: str | None) -> str | None:
    if mnemonic is None:
        return None
    return "%s %s" % (mnemonic, operands) if operands else mnemonic


def classify(insn: Insn, kind: str, mnemonic: str | None,
             operands: str | None) -> tuple[str, str | None]:
    """Returns (category, noncode_pseudo counter or None) for one word."""
    decoded = insn.spec is not None
    if kind != "code":
        if mnemonic is None:
            pseudo = "ghidra_bad" if decoded else "agree"
        elif not decoded:
            pseudo = "we_unknown"
        else:
            result = ghidra_norm.compare(insn, mnemonic, operands)
            pseudo = "agree" if result == "match" else result
        return ("we_code_ghidra_noncode" if decoded else "both_noncode_unknown"), pseudo

    if decoded and mnemonic is not None:
        result = ghidra_norm.compare(insn, mnemonic, operands)
        if result == "match":
            return "match", None
    else:
        result = "mnemonic_mismatch"
    key = (insn.spec.name if decoded else None,
           ghidra_norm.normalize_mnemonic(mnemonic) if mnemonic is not None else None)
    if key in ghidra_norm.KNOWN_GHIDRA_DIFFERENCES:
        return "known_difference", None
    if not decoded:
        return "ghidra_code_we_unknown", None
    return result, None


def diff_words(raw: bytes, start: int, entries: list[dict], top: int, examples: int,
               strict: bool = False) -> dict:
    """Compare the ELF words `raw` (loaded at `start`) with the matching export entries."""
    kinds: Counter = Counter()
    categories: Counter = Counter()
    pseudo: Counter = Counter()
    tables: dict[str, dict[tuple, list]] = {key: {} for key in PAIR_KEYS}

    def record(table_key: str, insn: Insn, mnemonic, operands, addr: int) -> None:
        ghidra = (ghidra_norm.normalize_mnemonic(mnemonic) if mnemonic is not None else None)
        slot = tables[table_key].setdefault(
            (insn.spec.name if insn.spec is not None else None, ghidra), [0, []])
        slot[0] += 1
        if len(slot[1]) < examples:
            slot[1].append({"addr": "0x%08x" % addr, "word": "0x%08x" % insn.word,
                            "ours": format_insn(insn, True),
                            "ghidra": _ghidra_text(mnemonic, operands)})

    for i, (word,) in enumerate(struct.iter_unpack(">I", raw)):
        addr = start + 4 * i
        entry = entries[i]
        kind, mnemonic, operands = entry["kind"], entry["mnemonic"], entry["operands"]
        insn = decode(word, addr, strict)
        category, pseudo_counter = classify(insn, kind, mnemonic, operands)
        kinds[kind] += 1
        categories[category] += 1
        if category in tables:
            record(category, insn, mnemonic, operands, addr)
        if pseudo_counter is not None:
            pseudo[pseudo_counter] += 1
            if pseudo_counter in _PSEUDO_PAIR_KEYS:
                record(_PSEUDO_PAIR_KEYS[pseudo_counter], insn, mnemonic, operands, addr)

    pairs = {}
    for key in PAIR_KEYS:
        ordered = sorted(tables[key].items(),
                         key=lambda kv: (-kv[1][0], kv[0][0] or "", kv[0][1] or ""))
        pairs[key] = [{"ours": ours, "ghidra": ghidra, "count": slot[0], "examples": slot[1]}
                      for (ours, ghidra), slot in ordered[:top]]
    return {
        "total_words": len(raw) // 4,
        "ghidra_kinds": {k: kinds[k] for k in KINDS},
        "categories": {k: categories[k] for k in CATEGORIES},
        "noncode_pseudo": {k: pseudo[k] for k in PSEUDO_COUNTERS},
        "pairs": pairs,
    }


def _entry_fields(entry: object, addr: int) -> dict:
    if not isinstance(entry, dict):
        raise CliError(EXIT_IO, "invalid Ghidra export: malformed word entry at 0x%08x" % addr)
    if parse_export_addr(entry.get("addr"), "words.addr") != addr:
        raise CliError(EXIT_IO, "invalid Ghidra export: word entries are not contiguous at "
                       "0x%08x" % addr)
    if entry.get("kind") not in KINDS:
        raise CliError(EXIT_IO, "invalid Ghidra export: bad kind %r at 0x%08x"
                       % (entry.get("kind"), addr))
    for key in ("mnemonic", "operands"):
        if not isinstance(entry.get(key), (str, type(None))):
            raise CliError(EXIT_IO, "invalid Ghidra export: bad %s at 0x%08x" % (key, addr))
    return entry


def select_entries(export: dict, raw: bytes, start: int, end: int) -> list[dict]:
    """The export's word entries for [start, end), checked against the ELF bytes."""
    text_start, text_end = export_text_range(export)
    if start < text_start or end > text_end:
        raise CliError(EXIT_BAD_ADDRESS,
                       "range 0x%08x-0x%08x is not inside the export's text range "
                       "0x%08x-0x%08x" % (start, end, text_start, text_end))
    words = export["words"]
    first = (start - text_start) // 4
    if len(words) != (text_end - text_start) // 4:
        raise CliError(EXIT_IO, "invalid Ghidra export: %d word entries for a text range of "
                       "%d words" % (len(words), (text_end - text_start) // 4))
    entries = []
    for i, (word,) in enumerate(struct.iter_unpack(">I", raw)):
        addr = start + 4 * i
        entry = _entry_fields(words[first + i], addr)
        exported = parse_export_addr(entry.get("word"), "words.word")
        if exported != word:
            raise CliError(EXIT_NOT_PPU_ELF,
                           "Ghidra export disagrees with the ELF at 0x%08x: export 0x%08x, "
                           "ELF 0x%08x" % (addr, exported, word))
        entries.append(entry)
    return entries


def _print_report(report: dict) -> None:
    kinds = report["ghidra_kinds"]
    print("range: %s-%s" % (report["range"]["start"], report["range"]["end"]))
    print("total words: %d" % report["total_words"])
    print("ghidra kinds: code %d, data %d, undefined %d"
          % (kinds["code"], kinds["data"], kinds["undefined"]))
    print("categories:")
    for name in CATEGORIES:
        print("  %-24s %d" % (name, report["categories"][name]))
    print("noncode pseudo-decode:")
    for name in PSEUDO_COUNTERS:
        print("  %-24s %d" % (name, report["noncode_pseudo"][name]))
    for name in BUG_CATEGORIES:
        pairs = report["pairs"][name][:STDOUT_TOP]
        if not pairs:
            continue
        print("top pairs for %s (ours -> ghidra):" % name)
        for pair in pairs:
            ex = pair["examples"][0] if pair["examples"] else None
            line = "  %-10s -> %-10s %7d" % (pair["ours"], pair["ghidra"], pair["count"])
            if ex is not None:
                line += "   e.g. %s %s  ours: %s  ghidra: %s" % (
                    ex["addr"], ex["word"], ex["ours"], ex["ghidra"])
            print(line)


def _run(args: argparse.Namespace) -> int:
    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    elf = parse_ppu_elf(data)
    export = _load_export(Path(args.ghidra))
    validate_export(export, elf, sha256, check_memory_map=False)

    if args.start is not None:
        start, end = args.start, args.end
    else:
        start, end = export_text_range(export)
    _require_exec_range(elf, start, end)
    try:
        raw = elf.read_va(start, end - start)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)
    entries = select_entries(export, raw, start, end)

    report = {
        "schema": SCHEMA,
        "input": {"elf": str(args.elf), "ghidra_json": str(args.ghidra), "sha256": sha256},
        "range": {"start": "0x%08x" % start, "end": "0x%08x" % end},
        "strict": args.strict,
    }
    report.update(diff_words(raw, start, entries, args.top, args.examples, args.strict))
    _print_report(report)
    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    _write_json(out_path, report)
    print("report: %s" % out_path)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py ghidra-diff",
        description="Compare our PPU decoder with a Ghidra export word by word and classify "
                    "the differences.")
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("--ghidra", required=True, metavar="JSON",
                        help="export written by ghidra-export")
    parser.add_argument("--start", type=_addr_arg, metavar="ADDR",
                        help="first address to compare (default: start of the export's text)")
    parser.add_argument("--end", type=_addr_arg, metavar="ADDR",
                        help="end address, exclusive (default: end of the export's text)")
    parser.add_argument("--strict", action="store_true",
                        help="decode with reserved-bit checking (see disasm --strict)")
    parser.add_argument("-o", "--output", metavar="OUT",
                        help="report JSON path (default: generated/analysis/ghidra_diff.json)")
    parser.add_argument("--top", type=int, default=DEFAULT_TOP, metavar="N",
                        help="pairs kept per category in the report (default: %(default)s)")
    parser.add_argument("--examples", type=int, default=DEFAULT_EXAMPLES, metavar="K",
                        help="examples kept per pair (default: %(default)s)")
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
    if args.top < 1:
        parser.error("--top must be at least 1")
    if args.examples < 0:
        parser.error("--examples must not be negative")


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

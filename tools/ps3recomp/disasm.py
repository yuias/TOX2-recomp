"""`disasm` command: PPU disassembly listing and decode-coverage statistics."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path

from . import cell
from .elf import ElfError, ElfFile
from .info import (DEFAULT_CONFIG, EXIT_IO, EXIT_NOT_PPU_ELF, CliError, load_config,
                   parse_ppu_elf, read_input, reject_encrypted)
from .ppu import bits, decode, decode_range
from .ppu_format import format_insn

EXIT_BAD_ADDRESS = 5

DEFAULT_ENTRY_COUNT = 32
SCHEMA = 1
TOP_MNEMONICS = 30
TOP_UNKNOWN = 20
TOP_REJECTED = 20


def _load_default_config() -> dict | None:
    # The pin file is only used to recognise the encrypted EBOOT.BIN by hash;
    # a missing or broken file must not stop the disassembler.
    try:
        return load_config(DEFAULT_CONFIG)
    except CliError:
        return None


def _hex_addr(value: str) -> int:
    try:
        number = int(value, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid address: %r" % value)
    if number < 0:
        raise argparse.ArgumentTypeError("negative address: %r" % value)
    return number


def _check_range(elf: ElfFile, start: int, end: int) -> None:
    """Require [start, end) to lie in the file-backed part of one exec segment."""
    for seg in elf.exec_segments():
        if seg.p_vaddr <= start and end <= seg.p_vaddr + seg.p_filesz:
            return
    raise CliError(EXIT_BAD_ADDRESS,
                   "range 0x%08x-0x%08x is not inside the file-backed part of an "
                   "executable segment" % (start, end))


def _read_range(elf: ElfFile, start: int, end: int) -> bytes:
    _check_range(elf, start, end)
    try:
        return elf.read_va(start, end - start)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)


def list_range(elf: ElfFile, start: int, end: int, raw: bool,
               strict: bool = False) -> list[str]:
    data = _read_range(elf, start, end)
    return ["0x%08x: %08x  %s" % (insn.addr, insn.word, format_insn(insn, simplify=not raw))
            for insn in decode_range(data, start, strict)]


def compute_stats(elf: ElfFile, ranges: list[tuple[int, int]]) -> dict:
    mnemonics: Counter = Counter()
    unknown: Counter = Counter()
    rejected: Counter = Counter()
    total = decoded = 0
    for start, end in ranges:
        for insn in decode_range(_read_range(elf, start, end), start):
            total += 1
            spec = insn.spec
            if spec is None:
                unknown[(bits(insn.word, 0, 5), bits(insn.word, 21, 30))] += 1
                continue
            decoded += 1
            mnemonics[spec.name] += 1
            # The non-strict hit already tells whether strict mode accepts the word; only a
            # rejected word needs a second walk, to see whether a less specific spec takes it.
            if (insn.word & spec.strict_mask != spec.strict_match
                    and decode(insn.word, insn.addr, True).spec is None):
                rejected[spec.name] += 1
    unknown_count = total - decoded
    rejected_count = sum(rejected.values())
    strict_decoded = decoded - rejected_count
    return {
        "schema": SCHEMA,
        "ranges": [{"start": "0x%08x" % s, "end": "0x%08x" % e} for s, e in ranges],
        "total_words": total,
        "decoded": decoded,
        "unknown": unknown_count,
        "decoded_pct": round(100.0 * decoded / total, 4) if total else 0.0,
        "unknown_pct": round(100.0 * unknown_count / total, 4) if total else 0.0,
        "top_mnemonics": [{"mnemonic": name, "count": n}
                          for name, n in mnemonics.most_common(TOP_MNEMONICS)],
        "top_unknown_patterns": [{"primary": p, "xo_21_30": xo, "count": n}
                                 for (p, xo), n in unknown.most_common(TOP_UNKNOWN)],
        "strict": {
            "decoded": strict_decoded,
            "decoded_pct": round(100.0 * strict_decoded / total, 4) if total else 0.0,
            "rejected": rejected_count,
            "top_rejected": [{"mnemonic": name, "count": n}
                             for name, n in rejected.most_common(TOP_REJECTED)],
        },
    }


def _print_stats(stats: dict) -> None:
    total = stats["total_words"]
    print("total words: %d" % total)
    print("decoded:     %d (%.2f%%)" % (stats["decoded"], stats["decoded_pct"]))
    print("unknown:     %d (%.2f%%)" % (stats["unknown"], stats["unknown_pct"]))
    print("top mnemonics:")
    for item in stats["top_mnemonics"]:
        print("  %-12s %d" % (item["mnemonic"], item["count"]))
    print("top unknown patterns (primary, bits 21-30):")
    for item in stats["top_unknown_patterns"]:
        print("  %2d, 0x%03x   %d" % (item["primary"], item["xo_21_30"], item["count"]))
    strict = stats["strict"]
    print("strict decoded: %d (%.2f%%)" % (strict["decoded"], strict["decoded_pct"]))
    print("strict rejected: %d" % strict["rejected"])
    print("top strict-rejected mnemonics:")
    for item in strict["top_rejected"]:
        print("  %-12s %d" % (item["mnemonic"], item["count"]))


def _write_json(path: Path, doc: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2, sort_keys=False, ensure_ascii=False)
            f.write("\n")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _run(args: argparse.Namespace) -> int:
    data = read_input(args.elf)
    reject_encrypted(data, hashlib.sha256(data).hexdigest(), _load_default_config())
    elf = parse_ppu_elf(data)

    if args.stats:
        if args.start is not None:
            ranges = [(args.start, args.end)]
        else:
            ranges = [(s.p_vaddr, s.p_vaddr + s.p_filesz // 4 * 4)
                      for s in elf.exec_segments()]
        stats = compute_stats(elf, ranges)
        _print_stats(stats)
        if args.json:
            _write_json(Path(args.json), stats)
        return 0

    if args.entry:
        try:
            start = cell.parse_entry(elf).func
        except ElfError as e:
            raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)
        if start is None:
            raise CliError(EXIT_NOT_PPU_ELF, "this file has no entry function")
        count = args.count if args.count is not None else DEFAULT_ENTRY_COUNT
        end = start + 4 * count
        if args.count is None:
            # The implicit default may overrun a short segment; clamp it instead of failing.
            seg = next((s for s in elf.exec_segments()
                        if s.p_vaddr <= start < s.p_vaddr + s.p_filesz), None)
            if seg is not None:
                end = min(end, seg.p_vaddr + seg.p_filesz // 4 * 4)
    else:
        start = args.start
        end = args.end if args.end is not None else start + 4 * args.count

    for line in list_range(elf, start, end, args.raw, args.strict):
        print(line)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py disasm",
        description="Disassemble PPU code in a decrypted PS3 ELF, or report decode coverage.",
        usage="%(prog)s ELF (--start ADDR (--count N | --end ADDR) | --entry [--count N] | "
              "--stats [--start ADDR --end ADDR [--json OUT]]) [--raw] [--strict]",
        epilog="modes:\n"
               "  range:  disasm ELF --start ADDR (--count N | --end ADDR) [--raw]\n"
               "  entry:  disasm ELF --entry [--count N]    (default count: %d)\n"
               "  stats:  disasm ELF --stats [--start ADDR --end ADDR] [--json OUT]\n"
               % DEFAULT_ENTRY_COUNT,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("--start", type=_hex_addr, metavar="ADDR",
                        help="first address (range mode; with --stats, start of the range)")
    parser.add_argument("--end", type=_hex_addr, metavar="ADDR",
                        help="end address, exclusive")
    parser.add_argument("--count", type=int, metavar="N", help="number of instructions")
    parser.add_argument("--entry", action="store_true",
                        help="start at the entry function")
    parser.add_argument("--stats", action="store_true",
                        help="decode every word of the executable segments (or of "
                             "[--start, --end)) and print coverage statistics")
    parser.add_argument("--raw", action="store_true",
                        help="print raw mnemonics (no simplified forms)")
    parser.add_argument("--strict", action="store_true",
                        help="print words with reserved bits set as .long (listing modes; "
                             "--stats always reports strict coverage, so the flag is "
                             "redundant there)")
    parser.add_argument("--json", metavar="OUT",
                        help="with --stats, also write the statistics as JSON to OUT")
    return parser


def _validate(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    for name in ("start", "end"):
        value = getattr(args, name)
        if value is not None and value % 4:
            parser.error("--%s must be 4-byte aligned" % name)
    if args.count is not None and args.count < 1:
        parser.error("--count must be at least 1")
    if args.stats:
        if args.entry or args.count is not None or args.raw:
            parser.error("--stats cannot be combined with --entry, --count or --raw")
        if (args.start is None) != (args.end is None):
            parser.error("--stats takes --start and --end together or not at all")
    else:
        if args.json:
            parser.error("--json requires --stats")
        if args.entry:
            if args.start is not None or args.end is not None:
                parser.error("--entry cannot be combined with --start or --end")
        else:
            if args.start is None:
                parser.error("one of --start, --entry or --stats is required")
            if (args.count is None) == (args.end is None):
                parser.error("give exactly one of --count and --end with --start")
    if args.start is not None and args.end is not None and args.end <= args.start:
        parser.error("--end must be greater than --start")


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

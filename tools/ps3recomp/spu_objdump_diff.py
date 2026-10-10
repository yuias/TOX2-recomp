"""`spu-objdump-diff` command: compare our SPU decoder with `objdump` word by word.

The reference is the objdump of an SPU-capable GNU binutils release, run under WSL on the raw code
segments of every image in the registry (`spu_objdump.py`). Our side is the `spu_format` text of each
word, parsed by the same parser as objdump's text. A disagreement is settled by the SPU ISA document, not
by adopting objdump's behaviour.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from .disasm import _write_json
from .info import EXIT_IO, REPO_ROOT, CliError
from .spu import FIELDS, SpuInsn, decode
from .spu_format import format_insn
from .spu_images import SpuImage, SpuSegment, add_input_arguments, open_images, write_if_changed
from .spu_objdump import (DEFAULT_DISTRO, TARGET_MODULUS, CanonInsn, ObjdumpUnavailable, Operand,
                          canon_text, objdump_version, parse_listing, resolve_objdump, run_objdump)

SCHEMA = 1
EXIT_GATE = 9
DEFAULT_REPORT = REPO_ROOT / "generated" / "analysis" / "spu_objdump_diff.json"
DEFAULT_LISTING_DIR = REPO_ROOT / "generated" / "analysis" / "spu_objdump"
DEFAULT_TOP = 20
DEFAULT_EXAMPLES = 5

CATEGORIES = ("match", "known_difference", "objdump_code_we_unknown", "we_code_objdump_unknown",
              "mnemonic_mismatch", "operand_mismatch", "both_unknown")
# Unlike a disassembler with code analysis, `objdump -D` decodes every word it can, so a word that only
# we decode is a disagreement about the encoding too.
DISAGREEMENTS = ("objdump_code_we_unknown", "we_code_objdump_unknown", "mnemonic_mismatch", "operand_mismatch")


@dataclass(frozen=True)
class KnownDifference:
    category: str  # one of DISAGREEMENTS
    ours: str | None  # our base mnemonic, None for ".long"
    objdump: str | None  # objdump's base mnemonic, None for ".long"
    reason: str  # names the ISA section that shows our decoding is right (or that both are valid)


# An entry is added only after the ISA has been checked.
KNOWN_DIFFERENCES: tuple[KnownDifference, ...] = ()


# --- Comparison -----------------------------------------------------------------------------


def _field_modulus(name: str) -> int:
    """2**(field width + scale shift): absorbs signed against unsigned printing of one field."""
    if name.endswith(")"):
        name = name.split("(")[0]
    field_ = FIELDS.get(name)
    return 1 << (field_.width + field_.scale) if field_ is not None else 1 << 32


def operand_equal(name: str, ours: Operand, theirs: Operand) -> bool:
    """`name` is the operand of our spec at this position (for the field width)."""
    if ours.kind != theirs.kind:
        return False
    if ours.kind == "raw":
        return ours.text == theirs.text
    if ours.kind in ("reg", "chan", "spr"):
        return ours.value == theirs.value
    if ours.kind == "target":
        return (ours.value - theirs.value) % TARGET_MODULUS == 0
    modulus = _field_modulus(name)
    if ours.kind == "mem":
        return ours.base == theirs.base and (ours.value - theirs.value) % modulus == 0
    return (ours.value - theirs.value) % modulus == 0


def operands_match(ours: SpuInsn, ours_canon: CanonInsn, theirs: CanonInsn) -> bool:
    if len(ours_canon.operands) != len(theirs.operands):
        return False
    names = ours.spec.operands if ours.spec is not None else ()
    for index, (a, b) in enumerate(zip(ours_canon.operands, theirs.operands)):
        name = names[index] if index < len(names) else ""
        if not operand_equal(name, a, b):
            return False
    return True


def _base(canon: CanonInsn) -> str | None:
    return None if canon.mnemonic == ".long" else canon.mnemonic


def compare(ours: SpuInsn, ours_canon: CanonInsn, theirs: CanonInsn,
            known: tuple[KnownDifference, ...] | None = None) -> str:
    """The category of one word."""
    ours_known = ours.spec is not None
    theirs_known = theirs.mnemonic != ".long"
    if not ours_known and not theirs_known:
        return "both_unknown"
    if not ours_known:
        category = "objdump_code_we_unknown"
    elif not theirs_known:
        category = "we_code_objdump_unknown"
    elif ours_canon.mnemonic != theirs.mnemonic or ours_canon.flags != theirs.flags:
        category = "mnemonic_mismatch"
    elif not operands_match(ours, ours_canon, theirs):
        category = "operand_mismatch"
    else:
        return "match"
    for entry in KNOWN_DIFFERENCES if known is None else known:
        if (entry.category, entry.ours, entry.objdump) == (category, _base(ours_canon), _base(theirs)):
            return "known_difference"
    return category


# --- Collection -----------------------------------------------------------------------------


@dataclass
class Tally:
    counts: Counter = field(default_factory=Counter)
    # (category, ours, objdump) -> count and examples
    triples: dict = field(default_factory=dict)

    def add(self, category: str, ours: str | None, objdump: str | None, example: dict, limit: int) -> None:
        self.counts[category] += 1
        if category == "match":
            return
        entry = self.triples.setdefault((category, ours, objdump), {"count": 0, "examples": []})
        entry["count"] += 1
        if len(entry["examples"]) < limit:
            entry["examples"].append(example)

    def merge(self, other: "Tally", limit: int) -> None:
        self.counts.update(other.counts)
        for key, value in other.triples.items():
            entry = self.triples.setdefault(key, {"count": 0, "examples": []})
            entry["count"] += value["count"]
            entry["examples"].extend(value["examples"][:max(0, limit - len(entry["examples"]))])


def compare_segment(listing: list, limit: int, known: tuple[KnownDifference, ...] | None = None) -> Tally:
    tally = Tally()
    for line in listing:
        ours = decode(line.word, line.addr)
        ours_text = format_insn(ours)
        ours_canon = canon_text(ours_text)
        theirs = canon_text(line.text)
        category = compare(ours, ours_canon, theirs, known)
        example = {"ls": "0x%05x" % line.addr, "word": "0x%08x" % line.word, "ours": ours_text,
                   "objdump": " ".join(line.text.split("\t#")[0].split())}
        tally.add(category, _base(ours_canon), _base(theirs), example, limit)
    return tally


def listing_name(image: SpuImage, seg: SpuSegment) -> str:
    return "%s_%05x.txt" % (image.name, seg.ls)


def segment_code(image: SpuImage, seg: SpuSegment) -> bytes:
    # A trailing partial word would be printed as bytes and put the listing out of step.
    data = image.segment_bytes(seg)
    return data[:len(data) // 4 * 4]


def obtain_listing(image: SpuImage, seg: SpuSegment, code: bytes, *, listing_dir: Path | None,
                   objdump: str, distro: str | None) -> str:
    name = listing_name(image, seg)
    if listing_dir is not None:
        path = listing_dir / name
        try:
            return path.read_text(encoding="utf-8")
        except OSError as e:
            raise CliError(EXIT_IO, "cannot read listing %s: %s" % (path, e.strerror))
        except UnicodeDecodeError:
            raise CliError(EXIT_IO, "listing %s is not UTF-8 text" % path)
    text = run_objdump(code, seg.ls, objdump, distro)
    try:
        DEFAULT_LISTING_DIR.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot create %s: %s" % (DEFAULT_LISTING_DIR, e.strerror))
    write_if_changed(DEFAULT_LISTING_DIR / name, text.encode("utf-8"))
    return text


@dataclass
class ImageResult:
    image: SpuImage
    total: Tally
    segments: list  # (seg, Tally)

    @property
    def disagreements(self) -> Counter:
        return Counter({c: self.total.counts[c] for c in DISAGREEMENTS if self.total.counts[c]})


def diff_image(image: SpuImage, *, listing_dir: Path | None, objdump: str, distro: str | None,
               limit: int, known: tuple[KnownDifference, ...] | None = None) -> ImageResult:
    total = Tally()
    segments = []
    for seg in image.code_segments:
        code = segment_code(image, seg)
        if not code:
            continue
        text = obtain_listing(image, seg, code, listing_dir=listing_dir, objdump=objdump, distro=distro)
        tally = compare_segment(parse_listing(text, seg.ls, code), limit, known)
        total.merge(tally, limit)
        segments.append((seg, tally))
    return ImageResult(image, total, segments)


# --- Output ---------------------------------------------------------------------------------


def count_line(name: str, counts: Counter) -> str:
    return " ".join([name] + ["%s %d" % (c, counts[c]) for c in CATEGORIES])


def _triples_json(tally: Tally, top: int) -> list:
    ordered = sorted(tally.triples.items(), key=lambda kv: (-kv[1]["count"], kv[0][0], str(kv[0][1]), str(kv[0][2])))
    return [{"category": category, "ours": ours, "objdump": theirs, "count": value["count"],
             "examples": value["examples"]} for (category, ours, theirs), value in ordered[:top]]


def build_report(results: list[ImageResult], version: str | None, top: int) -> dict:
    images = []
    for result in results:
        images.append({
            "name": result.image.name,
            "gate": result.image.entry.gate,
            "counts": {c: result.total.counts[c] for c in CATEGORIES},
            "segments": [{"ls": "0x%x" % seg.ls, "size": seg.filesz,
                          "counts": {c: tally.counts[c] for c in CATEGORIES}} for seg, tally in result.segments],
            "triples": _triples_json(result.total, top),
        })
    return {"schema": SCHEMA, "objdump_version": version, "images": images}


def summary_lines(results: list[ImageResult]) -> tuple[list[str], int]:
    """Output lines and the number of unexplained disagreements in gate images."""
    lines = [count_line(r.image.name, r.total.counts) for r in results]
    bad_images, unexplained = [], 0
    for result in results:
        if not result.disagreements:
            continue
        if result.image.entry.gate:
            bad_images.append(result.image.name)
            unexplained += sum(result.disagreements.values())
        else:
            for category in DISAGREEMENTS:
                if result.disagreements[category]:
                    lines.append("ungated disagreement: %s %s %d" % (result.image.name, category,
                                                                      result.disagreements[category]))
    if unexplained:
        lines.append("gate: %d unexplained disagreements in %s" % (unexplained, ", ".join(bad_images)))
    else:
        lines.append("gate: ok")
    return lines, unexplained


# --- Command --------------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py spu-objdump-diff",
        description="Compare the SPU decoder with objdump (SPU-capable GNU binutils, run under WSL) on "
                    "every code segment of the registry images; default: all images.")
    add_input_arguments(parser)
    parser.add_argument("--objdump", metavar="PATH",
                        help="objdump path inside WSL (default: $%s, else the build-spu-objdump.sh install)"
                             % "TOX2_SPU_OBJDUMP")
    parser.add_argument("--wsl-distro", default=DEFAULT_DISTRO, metavar="NAME",
                        help="WSL distribution (default: %(default)s; empty: the WSL default)")
    parser.add_argument("--listing-dir", metavar="DIR",
                        help="read <image>_<ls>.txt listings from DIR instead of running objdump")
    parser.add_argument("--report", default=str(DEFAULT_REPORT),
                        help="JSON report (default: generated/analysis/spu_objdump_diff.json)")
    parser.add_argument("--top", type=int, default=DEFAULT_TOP, help="triples per image in the report")
    parser.add_argument("--examples", type=int, default=DEFAULT_EXAMPLES,
                        help="examples per triple and image in the report")
    return parser


def _run(args: argparse.Namespace) -> int:
    images = open_images(args, include_position_independent=True)
    objdump = resolve_objdump(args.objdump, os.environ)
    distro = args.wsl_distro or None
    listing_dir = Path(args.listing_dir) if args.listing_dir else None
    version = None if listing_dir is not None else objdump_version(objdump, distro)
    results = [diff_image(image, listing_dir=listing_dir, objdump=objdump, distro=distro, limit=args.examples)
               for image in images]
    lines, unexplained = summary_lines(results)
    _write_json(Path(args.report), build_report(results, version, args.top))
    for line in lines:
        print(line)
    return EXIT_GATE if unexplained else 0


def main(argv: list[str]) -> int:
    parser = _parser()
    try:
        args = parser.parse_args(argv)
        if args.top < 0 or args.examples < 0:
            parser.error("--top and --examples must not be negative")
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return _run(args)
    except ObjdumpUnavailable as e:
        print("spu-objdump-diff: %s" % e, file=sys.stderr)
        return e.code
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

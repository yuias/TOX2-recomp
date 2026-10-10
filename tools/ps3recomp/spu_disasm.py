"""`spu-disasm`: list SPU code of a registry image, or print a census of what the code uses.

The census exists to answer questions about an image before any recovery exists: which channels
the code reads and writes, which `stop` codes it raises and which MFC commands it issues.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

from .info import EXIT_IO, CliError
from .spu import SpuInsn, decode_range
from .spu_format import format_line
from .spu_images import SpuImage, SpuSegment, add_input_arguments, open_images

EXIT_BAD_ADDRESS = 5
TOP_MNEMONICS = 20

# Channel numbers of the Cell Broadband Engine Architecture (version 1.02, SPU channel table).
CHANNEL_NAMES = {
    0: "SPU_RdEventStat", 1: "SPU_WrEventMask", 2: "SPU_WrEventAck", 3: "SPU_RdSigNotify1",
    4: "SPU_RdSigNotify2", 7: "SPU_WrDec", 8: "SPU_RdDec", 9: "MFC_WrMSSyncReq",
    11: "SPU_RdEventMask", 12: "MFC_RdTagMask", 13: "SPU_RdMachStat", 14: "SPU_WrSRR0",
    15: "SPU_RdSRR0", 16: "MFC_LSA", 17: "MFC_EAH", 18: "MFC_EAL", 19: "MFC_Size", 20: "MFC_TagID",
    21: "MFC_Cmd", 22: "MFC_WrTagMask", 23: "MFC_WrTagUpdate", 24: "MFC_RdTagStat",
    25: "MFC_RdListStallStat", 26: "MFC_WrListStallAck", 27: "MFC_RdAtomicStat",
    28: "SPU_WrOutMbox", 29: "SPU_RdInMbox", 30: "SPU_WrOutIntrMbox",
}
MFC_CMD_CHANNEL = 21

_CHANNEL_MNEMONICS = ("rdch", "wrch", "rchcnt")
_WORD = 0xFFFFFFFF
# Instructions whose RT operand is read (or unused) instead of written.
_RT_NOT_WRITTEN = frozenset({"wrch", "mtspr", "nop", "brz", "brnz", "brhz", "brhnz",
                             "biz", "binz", "bihz", "bihnz"})


class ImageCensus:
    """Counts over the code segments of one image."""

    def __init__(self, image: SpuImage):
        self.image = image
        self.zero_words = 0
        self.decoded = 0
        self.undecoded = 0
        self.mnemonics: Counter[str] = Counter()
        self.channels: dict[str, Counter[int]] = {m: Counter() for m in _CHANNEL_MNEMONICS}
        self.stop_codes: Counter[int] = Counter()
        self.mfc_commands: Counter[int] = Counter()
        self.mfc_non_constant = 0

    @property
    def words(self) -> int:
        return self.zero_words + self.decoded + self.undecoded


def _dest_register(insn: SpuInsn) -> int | None:
    """Register that `insn` writes (including the link of branches), or None."""
    spec = insn.spec
    if not spec.operands:
        return None
    if spec.operands[0] == "RT4":
        return insn.field("RT4")
    if spec.operands[0] == "RT" and spec.mnemonic not in _RT_NOT_WRITTEN and "store" not in spec.flags:
        return insn.field("RT")
    return None


def _constant_result(insn: SpuInsn, known: dict[int, int]) -> int | None:
    """Preferred-slot word that `insn` leaves in its RT, or None when it is not a known constant."""
    name = insn.spec.mnemonic
    if name == "il":
        return insn.field("I16") & _WORD
    if name == "ila":
        return insn.field("I18")
    if name == "ilhu":
        return insn.field("U16") << 16
    if name == "ilh":
        return insn.field("U16") * 0x10001
    ra = known.get(insn.field("RA")) if "RA" in insn.spec.field_names else None
    if name == "iohl":
        base = known.get(insn.field("RT"))
        return None if base is None else base | insn.field("U16")
    if name == "ai":
        return None if ra is None else (ra + insn.field("I10")) & _WORD
    if name == "ori":
        return None if ra is None else (ra | insn.field("I10")) & _WORD
    if name == "andi":
        return None if ra is None else ra & insn.field("I10") & _WORD
    if name == "or" and insn.field("RA") == insn.field("RB"):
        return ra
    return None


def _scan_segment(census: ImageCensus, image: SpuImage, seg: SpuSegment) -> None:
    # Control flow is ignored on purpose: a register holds the nearest preceding constant definition
    # in address order. Firmware sets its MFC command registers once, far from the `wrch`, so a
    # block local analysis would report almost none of them. This is a census, not a proof.
    known: dict[int, int] = {}
    for insn in decode_range(image.segment_bytes(seg), seg.ls):
        spec = insn.spec
        if insn.word == 0:
            # An all-zero word is padding or data far more often than a real `stop 0`.
            census.zero_words += 1
            continue
        if spec is None:
            census.undecoded += 1
            continue
        census.decoded += 1
        census.mnemonics[spec.mnemonic] += 1
        if spec.mnemonic in _CHANNEL_MNEMONICS:
            channel = insn.field("CA")
            census.channels[spec.mnemonic][channel] += 1
            if spec.mnemonic == "wrch" and channel == MFC_CMD_CHANNEL:
                value = known.get(insn.field("RT"))
                if value is None:
                    census.mfc_non_constant += 1
                else:
                    census.mfc_commands[value] += 1
        elif spec.mnemonic == "stop":
            census.stop_codes[insn.field("STOP")] += 1

        dest = _dest_register(insn)
        if dest is not None:
            result = _constant_result(insn, known)
            if result is None:
                known.pop(dest, None)
            else:
                known[dest] = result


def compute_census(image: SpuImage) -> ImageCensus:
    census = ImageCensus(image)
    for seg in image.code_segments:
        _scan_segment(census, image, seg)
    return census


def _channel_item(channel: int, count: int) -> dict:
    return {"channel": channel, "name": CHANNEL_NAMES.get(channel), "count": count}


def census_json(census: ImageCensus) -> dict:
    image = census.image
    top = sorted(census.mnemonics.items(), key=lambda kv: (-kv[1], kv[0]))[:TOP_MNEMONICS]
    return {
        "name": image.name,
        "ls_base": None if image.ls_start is None else "0x%x" % image.ls_start,
        "code_words": census.words,
        "decoded": census.decoded,
        "long": census.undecoded,
        "zero": census.zero_words,
        "top_mnemonics": [{"mnemonic": m, "count": n} for m, n in top],
        "channels": {m: [_channel_item(c, n) for c, n in sorted(census.channels[m].items())]
                     for m in _CHANNEL_MNEMONICS},
        "stop_codes": [{"code": "0x%x" % c, "count": n} for c, n in sorted(census.stop_codes.items())],
        "mfc_commands": {
            "constants": [{"value": "0x%x" % v, "count": n} for v, n in sorted(census.mfc_commands.items())],
            "non_constant": census.mfc_non_constant,
        },
    }


def print_census(doc: dict) -> None:
    print("image %s" % doc["name"])
    print("  code words %d: decoded %d, .long %d, zero %d"
          % (doc["code_words"], doc["decoded"], doc["long"], doc["zero"]))
    print("  top mnemonics:")
    for item in doc["top_mnemonics"]:
        print("    %-10s %d" % (item["mnemonic"], item["count"]))
    for mnemonic in _CHANNEL_MNEMONICS:
        items = doc["channels"][mnemonic]
        text = ", ".join("%d %s x%d" % (i["channel"], i["name"] or "?", i["count"]) for i in items)
        print("  %s channels: %s" % (mnemonic, text or "none"))
    text = ", ".join("%s x%d" % (i["code"], i["count"]) for i in doc["stop_codes"])
    print("  stop codes: %s" % (text or "none"))
    mfc = doc["mfc_commands"]
    text = ", ".join("%s x%d" % (i["value"], i["count"]) for i in mfc["constants"])
    print("  MFC_Cmd constants: %s (non-constant writes: %d)" % (text or "none", mfc["non_constant"]))


def _write_json(path: Path, doc: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
            f.write("\n")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def list_segment(image: SpuImage, seg: SpuSegment, start: int | None, end: int | None) -> list[str]:
    lo = seg.ls if start is None else start
    hi = seg.ls + seg.filesz // 4 * 4 if end is None else end
    data = image.segment_bytes(seg)[lo - seg.ls:hi - seg.ls]
    return [format_line(insn) for insn in decode_range(data, lo)]


def list_range(image: SpuImage, start: int | None, end: int | None) -> list[str]:
    if start is None:
        lines: list[str] = []
        for seg in image.code_segments:
            lines.extend(list_segment(image, seg, None, None))
        return lines
    stop = start + 4 if end is None else end
    for seg in image.code_segments:
        if seg.ls <= start and stop <= seg.ls + seg.filesz // 4 * 4:
            return list_segment(image, seg, start, stop)
    raise CliError(EXIT_BAD_ADDRESS, "%s: ls 0x%05x..0x%05x is not inside one code segment"
                   % (image.name, start, stop))


def _hex_addr(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("invalid address %r" % text)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py spu-disasm",
        description="Disassemble the code of an SPU image from the registry, or print a census "
                    "(--stats) of its mnemonics, channels, stop codes and MFC commands.",
        epilog="modes:\n"
               "  listing: spu-disasm --image NAME [--start LS [--end LS]]\n"
               "  census:  spu-disasm --stats [--images a,b,...] [--json OUT]\n",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_input_arguments(parser)
    parser.add_argument("--image", metavar="NAME", help="the image to list (same as --images NAME)")
    parser.add_argument("--start", type=_hex_addr, metavar="LS",
                        help="first LS address to list (default: every code segment)")
    parser.add_argument("--end", type=_hex_addr, metavar="LS",
                        help="end LS address, exclusive (default: one instruction)")
    parser.add_argument("--stats", action="store_true",
                        help="print the census of every selected image instead of a listing")
    parser.add_argument("--json", metavar="OUT", help="with --stats, also write the census as JSON")
    return parser


def _validate(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    for name in ("start", "end"):
        value = getattr(args, name)
        if value is not None and (value < 0 or value % 4):
            parser.error("--%s must be a non-negative multiple of 4" % name)
    if args.image is not None:
        if args.images is not None:
            parser.error("give --image or --images, not both")
        args.images = args.image
    if args.stats:
        if args.start is not None or args.end is not None:
            parser.error("--stats cannot be combined with --start or --end")
        return
    if args.json:
        parser.error("--json requires --stats")
    if args.end is not None and args.start is None:
        parser.error("--end needs --start")
    if args.start is not None and args.end is not None and args.end <= args.start:
        parser.error("--end must be greater than --start")
    if args.images is None or "," in args.images:
        parser.error("a listing needs exactly one image: --image NAME")


def _run(args: argparse.Namespace) -> int:
    # A listing may name a position independent blob (its LS base is shown as 0); the census
    # default skips them like every other stage.
    images = open_images(args, include_position_independent=not args.stats)
    if args.stats:
        docs = [census_json(compute_census(image)) for image in images]
        for doc in docs:
            print_census(doc)
        if args.json:
            _write_json(Path(args.json), {"schema": 1, "images": docs})
        return 0
    for line in list_range(images[0], args.start, args.end):
        print(line)
    return 0


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

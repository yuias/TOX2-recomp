"""`spu-emit`: turn the committed SPU function databases into C source for the runtime.

Per image the output is `spu_<image>_NNN.c` (functions in address order, a new chunk once 4096
instructions are reached, a function never split). Shared files are `spu_funcs.h`, `spu_tables.c`
(the dispatch tables with their check words) and `spu_sources.cmake`. The report and the file set
are deterministic, and a file is rewritten only when its content changes.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from . import spu
from .info import EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError
from .spu_emit_core import EmitStats, FunctionRecord, ImageModel, emit_image, get_lowering, u32
from .spu_flow import SpuCode
from .spu_images import (SCHEMA, SpuImage, add_input_arguments, load_containers, load_registry,
                         registry_entry_sha256, resolve_image, select_entries, write_if_changed)

EXIT_USAGE = 2
EXIT_DB_MISMATCH = EXIT_NOT_PPU_ELF
EXIT_UNHANDLED = 9

DEFAULT_DB_DIR = REPO_ROOT / "config" / "spu"
DEFAULT_OUT = REPO_ROOT / "generated" / "ppu" / "spu"
DEFAULT_REPORT = REPO_ROOT / "generated" / "analysis" / "spu_emit_report.json"

CHUNK_INSTRUCTIONS = 4096
MAX_CHECK_WORDS = 4
CENSUS_TOP = 15
EXAMPLE_SITES = 5

HEADER_NAME = "spu_funcs.h"
TABLES_NAME = "spu_tables.c"
SOURCES_NAME = "spu_sources.cmake"

EDGE_KEYS = ("gotos", "jumps", "dispatch_direct", "dispatch_indirect", "fallthrough_jumps",
             "fallthrough_dispatch", "returns", "stops", "leaves", "invalid_flow")


# --- Function database ------------------------------------------------------------------------


@dataclass(frozen=True)
class DbFunction:
    entry: int
    sources: tuple[str, ...]
    addrs: tuple[int, ...]


@dataclass(frozen=True)
class SpuDatabase:
    image: str
    container_sha256: str
    image_sha256: str
    registry_entry_sha256: str
    entry: int
    functions: tuple[DbFunction, ...]
    dispatch: tuple[int, ...]


def _hex(value) -> int:
    if not isinstance(value, str):
        raise ValueError("expected a hex string, got %r" % (value,))
    return int(value, 16)


def parse_database(doc: dict) -> SpuDatabase:
    if doc["schema"] != SCHEMA:
        raise ValueError("unsupported schema %r" % (doc["schema"],))
    inputs = doc["input"]
    functions = []
    for item in doc["functions"]:
        addrs: list[int] = []
        for start, end in item["ranges"]:
            start, end = _hex(start), _hex(end)
            if start >= end or start % 4 or end % 4:
                raise ValueError("bad range 0x%x-0x%x" % (start, end))
            addrs.extend(range(start, end, 4))
        functions.append(DbFunction(_hex(item["entry"]), tuple(item["sources"]), tuple(addrs)))
    return SpuDatabase(doc["image"], inputs["container_sha256"], inputs["image_sha256"],
                       inputs["registry_entry_sha256"], _hex(doc["entry"]),
                       tuple(sorted(functions, key=lambda f: f.entry)),
                       tuple(sorted(_hex(seed["ls"]) for seed in doc["dispatch_seeds"])))


def load_database(path: Path) -> SpuDatabase:
    try:
        raw = Path(path).read_bytes()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read function database %s: %s (run spu-recover first)"
                       % (path, e.strerror))
    try:
        return parse_database(json.loads(raw.decode("utf-8")))
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        detail = "missing key %s" % e if isinstance(e, KeyError) else str(e)
        raise CliError(EXIT_IO, "invalid function database %s: %s" % (path, detail))


def check_database(db: SpuDatabase, image: SpuImage, container_sha256: str) -> None:
    """Refuse a database that was not made from these bytes and this registry entry."""
    for what, have, want in (("image", db.image, image.name),
                             ("container SHA-256", db.container_sha256, container_sha256),
                             ("image SHA-256", db.image_sha256, image.sha256),
                             ("registry entry SHA-256", db.registry_entry_sha256,
                              registry_entry_sha256(image.entry))):
        if have != want:
            raise CliError(EXIT_DB_MISMATCH, "%s: the function database has a different %s "
                           "(%s, expected %s); run spu-recover" % (image.name, what, have, want))


def build_model(db: SpuDatabase, image: SpuImage, *, comments: bool = False) -> ImageModel:
    overrides = dict(image.entry.lowering_overrides)
    default = image.entry.lowering

    def lowering_of(entry: int):
        return get_lowering(overrides.get(entry, default))

    try:
        return ImageModel(image.name, SpuCode.from_image(image), {f.entry: f.addrs for f in db.functions},
                          db.dispatch, lowering_of, comments=comments)
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid function database for %s: %s" % (image.name, e))


# --- Rendering --------------------------------------------------------------------------------


def banner(prefix: str = "//") -> str:
    return "%s Generated by ps3tool spu-emit; do not edit.\n" % prefix


def chunk_name(image: str, index: int) -> str:
    return "spu_%s_%03d.c" % (image, index)


def plan_chunks(records: Sequence[FunctionRecord], limit: int = CHUNK_INSTRUCTIONS) -> list[list[FunctionRecord]]:
    """Contiguous groups; a group closes once it holds `limit` instructions, so no function is split."""
    chunks: list[list[FunctionRecord]] = []
    current: list[FunctionRecord] = []
    count = 0
    for record in records:
        current.append(record)
        count += record.instructions
        if count >= limit:
            chunks.append(current)
            current, count = [], 0
    if current:
        chunks.append(current)
    return chunks


def render_chunk(image: SpuImage, records: Sequence[FunctionRecord]) -> str:
    head = banner() + "// Image %s, SHA-256 %s.\n\n#include \"%s\"\n\n" % (image.name, image.sha256, HEADER_NAME)
    return head + "\n".join(record.text for record in records)


def render_funcs_header(models: Sequence[ImageModel], records: Mapping[str, Sequence[FunctionRecord]]) -> str:
    headers = sorted({get_lowering(name).header for name in _lowering_names(models)})
    lines = [banner().rstrip("\n"), "#pragma once", "", '#include "tox2/spu_context.h"']
    lines += ['#include "%s"' % h for h in headers]
    lines += ['#include "tox2/spu_ops.h"', ""]
    for model in models:
        lines += ["TOX2_SPU_FUNC(%s);" % record.name for record in records[model.name]]
    return "\n".join(lines) + "\n"


def _lowering_names(models: Sequence[ImageModel]) -> set[str]:
    return {model.lowering_of(entry).name for model in models for entry in model.functions}


def check_words(model: ImageModel, addr: int) -> list[int]:
    """Up to four instruction words from `addr` that the same function owns."""
    owner = model.owner[addr]
    words = []
    for i in range(MAX_CHECK_WORDS):
        a = addr + 4 * i
        if model.owner.get(a) != owner:
            break
        words.append(model.code.word(a))
    return words


def c_string(text: str) -> str:
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def render_tables(models: Sequence[ImageModel], images: Sequence[SpuImage]) -> str:
    lines = [banner().rstrip("\n"), "", '#include "%s"' % HEADER_NAME, ""]
    for model, image in zip(models, images):
        segments = sorted(image.segments, key=lambda s: s.ls)
        lines.append("static const Tox2SpuSegment kSegments_%s[] = {" % image.name)
        lines += ["    { %s, %s, %s, %du }," % (u32(s.ea), u32(s.ls), u32(s.filesz), 1 if s.code else 0)
                  for s in segments]
        lines += ["};", "", "static const Tox2SpuEntry kEntries_%s[] = {" % image.name]
        for addr in sorted(model.dispatchable):
            words = check_words(model, addr)
            lines.append("    { %s, %du, { %s }, %s }," % (
                u32(addr), len(words), ", ".join(u32(w) for w in words),
                model.func_name(model.owner[addr])))
        lines += ["};", ""]
    lines.append("const Tox2SpuImageInfo kTox2SpuImages[] = {")
    for model, image in zip(models, images):
        lines.append("    { %s, %s, %s, %s, %s, %du, kSegments_%s, %du, kEntries_%s }," % (
            c_string(image.name), u32(image.ea), u32(image.size), c_string(image.sha256),
            u32(image.entry_ls), len(image.segments), image.name, len(model.dispatchable), image.name))
    lines += ["};", "const uint32_t kTox2SpuImageCount = %du;" % len(images)]
    return "\n".join(lines) + "\n"


def render_sources(files: Sequence[str]) -> str:
    # CMake comments start with '#', so this file cannot use the C-style banner.
    lines = [banner("#").rstrip("\n"), "set(TOX2_SPU_GENERATED_SOURCES"]
    lines += ['    "${CMAKE_CURRENT_LIST_DIR}/%s"' % name for name in files]
    lines.append(")")
    return "\n".join(lines) + "\n"


# --- Per-image result, report and output ------------------------------------------------------


@dataclass
class ImageResult:
    image: SpuImage
    model: ImageModel
    stats: EmitStats
    records: list[FunctionRecord]
    chunks: list[list[FunctionRecord]]

    @property
    def unhandled(self) -> int:
        return sum(self.stats.unhandled.values())


def emit_one(image: SpuImage, db: SpuDatabase, *, comments: bool = False) -> ImageResult:
    model = build_model(db, image, comments=comments)
    stats = EmitStats()
    titles = {f.entry: "function 0x%05x (%s)" % (f.entry, ", ".join(f.sources)) for f in db.functions}
    records = emit_image(model, stats, titles=titles)
    return ImageResult(image, model, stats, records, plan_chunks(records))


def _hexs(addr: int) -> str:
    return "0x%05x" % addr


def _unhandled_section(stats: EmitStats) -> dict:
    by_mnemonic = dict(sorted(stats.unhandled.items(), key=lambda kv: (-kv[1], kv[0])))
    by_group = {group: 0 for group in spu.GROUPS}
    invalid = 0
    for key, count in stats.unhandled.items():
        found = spu.SPECS_BY_NAME.get(key.split(":")[0])
        if found is None:
            invalid += count
        else:
            by_group[found.group] += count
    if invalid:
        by_group["invalid"] = invalid
    return {"total": sum(stats.unhandled.values()),
            "by_mnemonic": by_mnemonic,
            "by_group": by_group,
            "examples": {key: [_hexs(a) for a in stats.unhandled_sites[key][:EXAMPLE_SITES]]
                         for key in sorted(by_mnemonic)}}


def image_report(result: ImageResult, file_bytes: Mapping[str, int]) -> dict:
    stats = result.stats
    return {
        "image": result.image.name,
        "gate": result.image.entry.gate,
        "functions": stats.functions,
        "instructions": stats.instructions,
        "dispatchable": len(result.model.dispatchable),
        "chunks": [{"file": chunk_name(result.image.name, i), "functions": len(chunk),
                    "instructions": sum(r.instructions for r in chunk),
                    "bytes": file_bytes.get(chunk_name(result.image.name, i), 0)}
                   for i, chunk in enumerate(result.chunks)],
        "unhandled": _unhandled_section(stats),
        "edges": {key: stats.edges.get(key, 0) for key in EDGE_KEYS},
        "invalid_flow": [[_hexs(site), _hexs(nxt)] for site, nxt in stats.invalid_flow],
        "unlisted_targets": [[_hexs(site), _hexs(target)] for site, target in stats.unlisted_targets],
        "labels": stats.labels,
        "loop_hooks": stats.loop_hooks,
        "folded_shufb": stats.folded_shufb,
    }


def build_report(results: Sequence[ImageResult], file_bytes: Mapping[str, int]) -> dict:
    gate = sum(r.unhandled for r in results if r.image.entry.gate)
    return {"schema": SCHEMA, "generator": "ps3tool spu-emit",
            "images": [image_report(r, file_bytes) for r in results],
            "gate_unhandled": gate,
            "ungated_unhandled": sum(r.unhandled for r in results if not r.image.entry.gate)}


def output_files(results: Sequence[ImageResult]) -> dict[str, str]:
    models = [r.model for r in results]
    files: dict[str, str] = {}
    sources: list[str] = []
    for result in results:
        for index, chunk in enumerate(result.chunks):
            name = chunk_name(result.image.name, index)
            files[name] = render_chunk(result.image, chunk)
            sources.append(name)
    files[HEADER_NAME] = render_funcs_header(models, {r.image.name: r.records for r in results})
    files[TABLES_NAME] = render_tables(models, [r.image for r in results])
    sources.append(TABLES_NAME)
    files[SOURCES_NAME] = render_sources(sources)
    return files


def write_output(out_dir: Path, files: Mapping[str, str], results: Sequence[ImageResult]) -> int:
    """Write the generated tree; returns the number of files that changed."""
    try:
        for result in results:
            keep = {chunk_name(result.image.name, i) for i in range(len(result.chunks))}
            for path in sorted(out_dir.glob("spu_%s_[0-9][0-9][0-9].c" % result.image.name)):
                if path.name not in keep:
                    path.unlink()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot remove a stale chunk in %s: %s" % (out_dir, e.strerror))
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot create %s: %s" % (out_dir, e.strerror))
    return sum(write_if_changed(out_dir / name, text.encode("utf-8")) for name, text in files.items())


def result_line(result: ImageResult) -> str:
    return "%s: functions %d instructions %d dispatchable %d unhandled %d" % (
        result.image.name, result.stats.functions, result.stats.instructions,
        len(result.model.dispatchable), result.unhandled)


def census_lines(result: ImageResult) -> list[str]:
    stats = result.stats
    top = ", ".join("%s %d" % kv for kv in stats.mnemonics.most_common(CENSUS_TOP))
    ranked = sorted(stats.unhandled.items(), key=lambda kv: (-kv[1], kv[0]))
    unhandled = ", ".join("%s %d" % kv for kv in ranked[:CENSUS_TOP])
    if len(ranked) > CENSUS_TOP:
        unhandled += " (+%d more mnemonics)" % (len(ranked) - CENSUS_TOP)
    return [result_line(result), "  top: " + top, "  unhandled: " + (unhandled or "none")]


# --- Command ----------------------------------------------------------------------------------


def _non_negative_int(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be at least 0")
    return number


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py spu-emit",
        description="Emit C source for the recovered functions of the SPU images in the registry.")
    add_input_arguments(parser)
    parser.add_argument("--db-dir", default=str(DEFAULT_DB_DIR),
                        help="function database directory (default: config/spu)")
    parser.add_argument("--out", default=str(DEFAULT_OUT),
                        help="output directory (default: generated/ppu/spu)")
    parser.add_argument("--report", default=str(DEFAULT_REPORT),
                        help="report path (default: generated/analysis/spu_emit_report.json)")
    parser.add_argument("--comments", action="store_true",
                        help="annotate generated code with the disassembly")
    parser.add_argument("--census", action="store_true",
                        help="only count instructions and unhandled words; write no files")
    parser.add_argument("--max-unhandled", type=_non_negative_int, metavar="N",
                        help="exit 9 if the gate images together have more than N unhandled words")
    return parser


def _run(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    entries = select_entries(registry, args.images, include_position_independent=False)
    containers = load_containers(args.eboot, args.libsre, args.target_config)
    results = []
    for entry in entries:
        image = resolve_image(entry, containers)
        db = load_database(Path(args.db_dir) / (image.name + ".json"))
        check_database(db, image, containers[entry.container].sha256)
        results.append(emit_one(image, db, comments=args.comments))

    if args.census:
        for result in results:
            for line in census_lines(result):
                print(line)
    else:
        files = output_files(results)
        changed = write_output(Path(args.out), files, results)
        report = build_report(results, {name: len(text.encode("utf-8")) for name, text in files.items()})
        report_text = json.dumps(report, indent=2, ensure_ascii=True) + "\n"
        try:
            Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            raise CliError(EXIT_IO, "cannot create %s: %s" % (Path(args.report).parent, e.strerror))
        write_if_changed(Path(args.report), report_text.encode("utf-8"))
        for result in results:
            print(result_line(result))
        print("output: %d files, %d written, in %s" % (len(files), changed, args.out))

    for result in results:
        if not result.image.entry.gate:
            print("ungated unhandled: %s %d" % (result.image.name, result.unhandled))
    gate = sum(r.unhandled for r in results if r.image.entry.gate)
    print("gate unhandled: %d" % gate)
    if args.max_unhandled is not None and gate > args.max_unhandled:
        return EXIT_UNHANDLED
    return 0


def main(argv: list[str]) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else EXIT_USAGE
    try:
        return _run(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

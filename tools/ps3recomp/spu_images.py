"""SPU image registry and `spu-extract`.

The registry (`config/spu_images.json`) names every SPU program embedded in the executable and in
libsre. It holds addresses, sizes, entries and roles only; the bytes are read from the pinned
containers at run time. `resolve_image` turns a registry entry into an `SpuImage`: the image
bytes, its SHA-256 and its segments with absolute guest addresses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .elf import ELFCLASS32, EM_SPU, PF_X, ElfError, ElfFile
from .info import (DEFAULT_CONFIG, EXIT_IO, EXIT_NOT_PPU_ELF, EXIT_PIN_MISMATCH, REPO_ROOT,
                   CliError, load_config, parse_ppu_elf, pinned_sha, read_input,
                   reject_encrypted)

SCHEMA = 1

# Exit codes beyond the ones shared with the other commands (`info.py`).
EXIT_REGISTRY = 2
EXIT_BAD_IMAGE = EXIT_NOT_PPU_ELF

DEFAULT_REGISTRY = REPO_ROOT / "config" / "spu_images.json"
DEFAULT_EBOOT = REPO_ROOT / "generated" / "inputs" / "EBOOT.elf"
DEFAULT_LIBSRE = REPO_ROOT / "generated" / "inputs" / "libsre.reloc.elf"
DEFAULT_EXTRACT_DIR = REPO_ROOT / "generated" / "spu" / "images"

LS_SIZE = 0x40000

# Container name -> key of its pin in the target config.
CONTAINER_PINS = {"eboot": "eboot_elf", "libsre": "libsre_reloc_elf"}

KINDS = ("elf", "raw")
ROLES = ("spurs_kernel", "spurs_system_service", "spurs_policy_module", "spurs_task",
         "raw_thread", "exit_handler", "job_chain_pm")
LOWERINGS = ("stackfree",)

_NAME_RE = re.compile(r"[a-z][a-z0-9_]*\Z")
_HEX_RE = re.compile(r"0x[0-9a-fA-F]+\Z")

_COMMON_KEYS = {"name", "container", "ea", "kind", "role", "lowering", "gate", "extra_entries",
                "lowering_overrides", "data_ranges", "pic_delta_regs"}
_RAW_KEYS = {"size", "ls_base", "entry"}


class RegistryError(CliError):
    def __init__(self, message: str):
        super().__init__(EXIT_REGISTRY, message)


class ImageError(CliError):
    def __init__(self, message: str):
        super().__init__(EXIT_BAD_IMAGE, message)


@dataclass(frozen=True)
class ExtraEntry:
    ls: int
    reason: str


@dataclass(frozen=True)
class DataRange:
    start: int      # LS addresses [start, end) of data inside a code segment
    end: int
    reason: str


@dataclass(frozen=True)
class RegistryEntry:
    name: str
    container: str
    ea: int
    kind: str
    role: str
    lowering: str
    gate: bool
    extra_entries: tuple[ExtraEntry, ...]
    lowering_overrides: tuple[tuple[int, str], ...]
    # Raw blobs only. `ls_base` is None for a position independent blob.
    size: int | None = None
    ls_base: int | None = None
    entry: int | None = None
    # Data inside code segments, and registers that hold 0 at the link address (the delta that
    # position-independent code adds to its addresses).
    data_ranges: tuple[DataRange, ...] = ()
    pic_delta_regs: tuple[int, ...] = ()
    raw: dict = field(default_factory=dict, compare=False, repr=False)


@dataclass(frozen=True)
class Registry:
    images: tuple[RegistryEntry, ...]

    def find(self, name: str) -> RegistryEntry | None:
        for entry in self.images:
            if entry.name == name:
                return entry
        return None


@dataclass(frozen=True)
class SpuSegment:
    ea: int        # absolute guest address of the segment's file bytes
    ls: int
    filesz: int
    memsz: int     # bytes past `filesz` are zero-filled in LS
    code: bool
    offset: int    # position of the bytes inside the image


@dataclass(frozen=True)
class SpuImage:
    entry: RegistryEntry
    data: bytes
    sha256: str
    segments: tuple[SpuSegment, ...]
    ls_start: int | None   # None: position independent
    ls_end: int | None
    entry_ls: int | None

    @property
    def name(self) -> str:
        return self.entry.name

    @property
    def ea(self) -> int:
        return self.entry.ea

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def position_independent(self) -> bool:
        return self.ls_start is None

    @property
    def code_segments(self) -> tuple[SpuSegment, ...]:
        return tuple(s for s in self.segments if s.code)

    def segment_bytes(self, seg: SpuSegment) -> bytes:
        return self.data[seg.offset:seg.offset + seg.filesz]


@dataclass(frozen=True)
class Container:
    name: str
    path: str
    sha256: str
    elf: ElfFile


# --- Registry -------------------------------------------------------------------------------


def _hex_value(value, what: str) -> int:
    if not isinstance(value, str) or not _HEX_RE.match(value):
        raise RegistryError("%s must be a hex string like \"0x1000\", got %r" % (what, value))
    return int(value, 16)


def _ls_value(value, what: str) -> int:
    ls = _hex_value(value, what)
    if ls % 4 or ls >= LS_SIZE:
        raise RegistryError("%s 0x%x is not a word aligned LS address" % (what, ls))
    return ls


def _check_lowering(value, what: str) -> str:
    if value not in LOWERINGS:
        raise RegistryError("lowering not implemented: %s %r (available: %s)"
                            % (what, value, ", ".join(LOWERINGS)))
    return value


def _parse_extra_entries(raw, name: str) -> tuple[ExtraEntry, ...]:
    if not isinstance(raw, list):
        raise RegistryError("%s: extra_entries must be a list" % name)
    out, seen = [], set()
    for item in raw:
        if (not isinstance(item, dict) or set(item) != {"ls", "reason"}
                or not isinstance(item["reason"], str) or not item["reason"].strip()):
            raise RegistryError("%s: extra_entries items need exactly \"ls\" and a non-empty "
                                "\"reason\"" % name)
        ls = _ls_value(item["ls"], "%s: extra_entries ls" % name)
        if ls in seen:
            raise RegistryError("%s: duplicate extra_entries ls 0x%x" % (name, ls))
        seen.add(ls)
        out.append(ExtraEntry(ls, item["reason"]))
    return tuple(out)


def _parse_data_ranges(raw, name: str) -> tuple[DataRange, ...]:
    if not isinstance(raw, list):
        raise RegistryError("%s: data_ranges must be a list" % name)
    out = []
    for item in raw:
        if (not isinstance(item, dict) or set(item) != {"start", "end", "reason"}
                or not isinstance(item["reason"], str) or not item["reason"].strip()):
            raise RegistryError("%s: data_ranges items need exactly \"start\", \"end\" and a "
                                "non-empty \"reason\"" % name)
        start = _ls_value(item["start"], "%s: data_ranges start" % name)
        end = _hex_value(item["end"], "%s: data_ranges end" % name)
        if end % 4 or end > LS_SIZE or end <= start:
            raise RegistryError("%s: data_ranges 0x%x..0x%x is not a non-empty word aligned LS range"
                                % (name, start, end))
        out.append(DataRange(start, end, item["reason"]))
    out.sort(key=lambda r: (r.start, r.end))
    for a, b in zip(out, out[1:]):
        if a.end > b.start:
            raise RegistryError("%s: data_ranges 0x%x..0x%x and 0x%x..0x%x overlap"
                                % (name, a.start, a.end, b.start, b.end))
    return tuple(out)


def _parse_pic_regs(raw, name: str) -> tuple[int, ...]:
    if (not isinstance(raw, list)
            or not all(isinstance(r, int) and not isinstance(r, bool) and 0 <= r < 128 for r in raw)):
        raise RegistryError("%s: pic_delta_regs must be a list of register numbers 0..127" % name)
    return tuple(sorted(set(raw)))


def _parse_overrides(raw, name: str) -> tuple[tuple[int, str], ...]:
    if not isinstance(raw, dict):
        raise RegistryError("%s: lowering_overrides must be an object" % name)
    out = []
    for key, value in raw.items():
        ls = _ls_value(key, "%s: lowering_overrides key" % name)
        out.append((ls, _check_lowering(value, "%s: lowering_overrides 0x%x" % (name, ls))))
    return tuple(sorted(out))


def _parse_entry(raw, index: int) -> RegistryEntry:
    if not isinstance(raw, dict):
        raise RegistryError("images[%d] is not an object" % index)
    name = raw.get("name")
    if not isinstance(name, str) or not _NAME_RE.match(name):
        raise RegistryError("images[%d]: name must match [a-z][a-z0-9_]*, got %r" % (index, name))
    kind = raw.get("kind")
    if kind not in KINDS:
        raise RegistryError("%s: kind must be one of %s, got %r" % (name, "/".join(KINDS), kind))

    allowed = _COMMON_KEYS | (_RAW_KEYS if kind == "raw" else set())
    unknown = sorted(set(raw) - allowed)
    if unknown:
        raise RegistryError("%s: unknown or misplaced keys: %s" % (name, ", ".join(unknown)))
    missing = sorted({"container", "ea", "role", "lowering", "gate", "extra_entries"} - set(raw))
    if kind == "raw":
        missing += sorted({"size", "ls_base"} - set(raw))
    if missing:
        raise RegistryError("%s: missing keys: %s" % (name, ", ".join(missing)))

    container = raw["container"]
    if container not in CONTAINER_PINS:
        raise RegistryError("%s: container must be one of %s, got %r"
                            % (name, "/".join(CONTAINER_PINS), container))
    if raw["role"] not in ROLES:
        raise RegistryError("%s: unknown role %r" % (name, raw["role"]))
    if not isinstance(raw["gate"], bool):
        raise RegistryError("%s: gate must be true or false" % name)
    ea = _hex_value(raw["ea"], "%s: ea" % name)
    if ea % 4:
        raise RegistryError("%s: ea 0x%x is not word aligned" % (name, ea))

    lowering = _check_lowering(raw["lowering"], name)
    extra = _parse_extra_entries(raw["extra_entries"], name)
    overrides = _parse_overrides(raw.get("lowering_overrides", {}), name)
    data_ranges = _parse_data_ranges(raw.get("data_ranges", []), name)
    pic_regs = _parse_pic_regs(raw.get("pic_delta_regs", []), name)

    size = ls_base = entry = None
    if kind == "raw":
        size = _hex_value(raw["size"], "%s: size" % name)
        if size == 0 or size % 4:
            raise RegistryError("%s: size 0x%x must be a non-zero multiple of 4" % (name, size))
        if raw["ls_base"] is not None:
            ls_base = _ls_value(raw["ls_base"], "%s: ls_base" % name)
            if ls_base + size > LS_SIZE:
                raise RegistryError("%s: blob does not fit in local store" % name)
            entry = _ls_value(raw.get("entry", raw["ls_base"]), "%s: entry" % name)
            if not ls_base <= entry < ls_base + size:
                raise RegistryError("%s: entry 0x%x is outside the blob" % (name, entry))
        elif "entry" in raw or extra or overrides or data_ranges or pic_regs:
            raise RegistryError("%s: a position independent blob (ls_base null) has no entry, "
                                "extra_entries, lowering_overrides, data_ranges or pic_delta_regs"
                                % name)
    return RegistryEntry(name, container, ea, kind, raw["role"], lowering, raw["gate"], extra,
                         overrides, size, ls_base, entry, data_ranges, pic_regs, raw)


def parse_registry(doc) -> Registry:
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        raise RegistryError("registry schema must be %d" % SCHEMA)
    if set(doc) - {"schema", "images"}:
        raise RegistryError("registry has unknown top-level keys")
    raw_images = doc.get("images")
    if not isinstance(raw_images, list) or not raw_images:
        raise RegistryError("registry needs a non-empty \"images\" list")
    entries = [_parse_entry(raw, i) for i, raw in enumerate(raw_images)]
    names = [e.name for e in entries]
    for name in sorted(set(names)):
        if names.count(name) > 1:
            raise RegistryError("duplicate image name %s" % name)
    return Registry(tuple(entries))


def load_registry(path: Path | str = DEFAULT_REGISTRY) -> Registry:
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read SPU registry %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise RegistryError("invalid SPU registry %s: %s" % (path, e))
    return parse_registry(doc)


def registry_entry_sha256(entry: RegistryEntry) -> str:
    """Hash of the entry as written in the registry; function databases record it."""
    text = json.dumps(entry.raw, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(text.encode("ascii")).hexdigest()


# --- Containers -----------------------------------------------------------------------------


def load_container(name: str, path: str | Path, config: dict) -> Container:
    key = CONTAINER_PINS[name]
    data = read_input(str(path))
    sha = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha, config)
    pinned = pinned_sha(config, key)
    if pinned is None:
        raise CliError(EXIT_PIN_MISMATCH, "%s: no %s sha256 is pinned in the target config"
                       % (path, key))
    if pinned != sha:
        raise CliError(EXIT_PIN_MISMATCH, "%s: SHA-256 %s does not match the pinned %s value"
                       % (path, sha, key))
    return Container(name, str(path), sha, parse_ppu_elf(data))


def load_containers(eboot: str | Path, libsre: str | Path,
                    config_path: str | Path = DEFAULT_CONFIG) -> dict[str, Container]:
    config = load_config(Path(config_path))
    return {"eboot": load_container("eboot", eboot, config),
            "libsre": load_container("libsre", libsre, config)}


# --- Image extraction -----------------------------------------------------------------------


def _read_guest(container: Container, ea: int, size: int, what: str) -> bytes:
    try:
        return container.elf.read_va(ea, size)
    except ElfError as e:
        raise ImageError("%s: %s" % (what, e))


def _elf_image(entry: RegistryEntry, container: Container) -> tuple[bytes, list[SpuSegment], int]:
    offset = container.elf.va_to_offset(entry.ea)
    if offset is None:
        raise ImageError("%s: address 0x%08x is not backed by file data in %s"
                         % (entry.name, entry.ea, container.name))
    try:
        elf = ElfFile.from_bytes(container.elf.data, base=offset)
    except ElfError as e:
        raise ImageError("%s: no ELF image at 0x%08x: %s" % (entry.name, entry.ea, e))
    if elf.ei_class != ELFCLASS32 or elf.e_machine != EM_SPU:
        raise ImageError("%s: not an SPU ELF (class %d, machine %d)"
                         % (entry.name, elf.ei_class, elf.e_machine))

    # The image runs from the ELF header to the end of the last segment; section headers are
    # not part of what the loader copies.
    extent = 52 + 32 * len(elf.segments)
    for seg in elf.segments:
        extent = max(extent, seg.p_offset + seg.p_filesz)
    segments = []
    for seg in elf.loads():
        if seg.p_filesz > seg.p_memsz:
            raise ImageError("%s: segment %d has p_filesz > p_memsz" % (entry.name, seg.index))
        if seg.p_memsz == 0:
            continue
        if seg.p_vaddr % 4 or seg.p_vaddr + seg.p_memsz > LS_SIZE:
            raise ImageError("%s: segment %d (ls 0x%x, 0x%x bytes) does not fit in local store"
                             % (entry.name, seg.index, seg.p_vaddr, seg.p_memsz))
        segments.append(SpuSegment(entry.ea + seg.p_offset, seg.p_vaddr, seg.p_filesz,
                                   seg.p_memsz, bool(seg.p_flags & PF_X), seg.p_offset))
    data = _read_guest(container, entry.ea, extent, entry.name)
    if data != container.elf.data[offset:offset + extent]:
        raise ImageError("%s: image bytes are not contiguous in the file" % entry.name)
    return data, segments, elf.e_entry


def resolve_image(entry: RegistryEntry, containers: dict[str, Container]) -> SpuImage:
    container = containers[entry.container]
    if entry.kind == "elf":
        data, segments, entry_ls = _elf_image(entry, container)
    else:
        data = _read_guest(container, entry.ea, entry.size, entry.name)
        entry_ls = entry.entry
        segments = [SpuSegment(entry.ea, entry.ls_base if entry.ls_base is not None else 0,
                               entry.size, entry.size, True, 0)]

    if entry.kind == "raw" and entry.ls_base is None:
        return SpuImage(entry, data, hashlib.sha256(data).hexdigest(), tuple(segments), None,
                        None, None)

    ordered = sorted(segments, key=lambda s: s.ls)
    if not ordered or not any(s.code for s in ordered):
        raise ImageError("%s: no code segment" % entry.name)
    for a, b in zip(ordered, ordered[1:]):
        if a.ls + a.memsz > b.ls:
            raise ImageError("%s: segments at ls 0x%x and 0x%x overlap" % (entry.name, a.ls, b.ls))
    if not any(s.ls <= entry_ls < s.ls + s.filesz for s in ordered if s.code):
        raise ImageError("%s: entry 0x%x is not in a code segment" % (entry.name, entry_ls))
    for extra in entry.extra_entries:
        if not any(s.ls <= extra.ls < s.ls + s.filesz for s in ordered if s.code):
            raise RegistryError("%s: extra entry 0x%x is not in a code segment"
                                % (entry.name, extra.ls))
    for rng in entry.data_ranges:
        if not any(s.ls <= rng.start and rng.end <= s.ls + s.filesz for s in ordered if s.code):
            raise RegistryError("%s: data range 0x%x..0x%x is not inside a code segment"
                                % (entry.name, rng.start, rng.end))
    return SpuImage(entry, data, hashlib.sha256(data).hexdigest(), tuple(segments),
                    ordered[0].ls, max(s.ls + s.memsz for s in ordered), entry_ls)


# --- Selection and CLI plumbing -------------------------------------------------------------


def select_entries(registry: Registry, names: str | None, *,
                   include_position_independent: bool) -> list[RegistryEntry]:
    """Entries named in `names` (comma separated), or the default set, in registry order.

    The default set is every image with a known LS base, plus the position independent blobs when
    `include_position_independent` is set (extraction lists them; recovery and emission skip them).
    """
    def position_independent(e: RegistryEntry) -> bool:
        return e.kind == "raw" and e.ls_base is None

    if names is None:
        return [e for e in registry.images
                if include_position_independent or not position_independent(e)]
    wanted = [n.strip() for n in names.split(",") if n.strip()]
    if not wanted:
        raise RegistryError("--images names no image")
    selected = []
    for name in wanted:
        entry = registry.find(name)
        if entry is None:
            raise RegistryError("unknown image %s" % name)
        if position_independent(entry) and not include_position_independent:
            raise RegistryError("image %s is position independent; this command skips it" % name)
        if entry not in selected:
            selected.append(entry)
    return selected


def add_input_arguments(parser: argparse.ArgumentParser) -> None:
    """Options shared by every SPU command."""
    parser.add_argument("--eboot", default=str(DEFAULT_EBOOT),
                        help="decrypted EBOOT.elf (default: generated/inputs/EBOOT.elf)")
    parser.add_argument("--libsre", default=str(DEFAULT_LIBSRE),
                        help="relocated libsre (default: generated/inputs/libsre.reloc.elf)")
    parser.add_argument("--images", help="comma separated image names")
    parser.add_argument("--registry", default=str(DEFAULT_REGISTRY),
                        help="SPU image registry (default: config/spu_images.json)")
    parser.add_argument("--target-config", default=str(DEFAULT_CONFIG),
                        help="target pin file (default: config/target.json)")


def open_images(args: argparse.Namespace, *,
                include_position_independent: bool) -> list[SpuImage]:
    registry = load_registry(args.registry)
    entries = select_entries(registry, args.images,
                             include_position_independent=include_position_independent)
    containers = load_containers(args.eboot, args.libsre, args.target_config)
    return [resolve_image(e, containers) for e in entries]


# --- spu-extract ----------------------------------------------------------------------------


def image_filename(image: SpuImage) -> str:
    return image.name + (".elf" if image.entry.kind == "elf" else ".bin")


def _hex(value: int | None) -> str | None:
    return None if value is None else "0x%x" % value


def manifest_entry(image: SpuImage) -> dict:
    return {
        "name": image.name,
        "file": image_filename(image),
        "kind": image.entry.kind,
        "role": image.entry.role,
        "ea": "0x%08x" % image.ea,
        "size": image.size,
        "sha256": image.sha256,
        "ls_base": _hex(image.ls_start),
        "ls_end": _hex(image.ls_end),
        "entry": _hex(image.entry_ls),
        "segments": [{"ea": "0x%08x" % s.ea, "ls": _hex(s.ls), "size": s.filesz,
                      "memsz": s.memsz, "code": s.code, "offset": s.offset}
                     for s in image.segments],
    }


def describe(image: SpuImage) -> str:
    if image.position_independent:
        ls, entry = "-", "-"
    else:
        ls, entry = "0x%x-0x%x" % (image.ls_start, image.ls_end), "0x%x" % image.entry_ls
    return "%s %s ea 0x%08x ls %s entry %s code segments %d" % (
        image.name, image.entry.kind, image.ea, ls, entry, len(image.code_segments))


def write_if_changed(path: Path, content: bytes) -> bool:
    """Write `content` unless the file already holds it; True when the file was written."""
    try:
        if path.is_file() and path.read_bytes() == content:
            return False
        path.write_bytes(content)
        return True
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def extract_images(images: list[SpuImage], out_dir: Path) -> dict:
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot create %s: %s" % (out_dir, e.strerror))
    for image in images:
        write_if_changed(out_dir / image_filename(image), image.data)
    manifest = {"schema": SCHEMA, "images": [manifest_entry(i) for i in images]}
    text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    write_if_changed(out_dir / "manifest.json", text.encode("utf-8"))
    return manifest


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py spu-extract",
        description="Write the SPU images named in the registry as standalone files, with a "
                    "manifest of their LS layout.")
    add_input_arguments(parser)
    parser.add_argument("--out", default=str(DEFAULT_EXTRACT_DIR),
                        help="output directory (default: generated/spu/images)")
    return parser


def main(argv: list[str]) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        # Extraction also lists the position independent blobs; later stages skip them.
        images = open_images(args, include_position_independent=True)
        extract_images(images, Path(args.out))
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code
    for image in images:
        print(describe(image))
    return 0

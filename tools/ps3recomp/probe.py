"""`probe` command: look for embedded SPU images and SPU-related imports in a PPU ELF."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from pathlib import Path

from . import cell
from .elf import EM_SPU, ELFCLASS32, PF_R, PF_W, PF_X, ElfError, ElfFile
from .info import (DEFAULT_CONFIG, EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError,
                   load_config, parse_ppu_elf, read_input, reject_encrypted)
from .nids import DEFAULT_SYSCALLS, load_syscall_table
from .ppu import decode
from .ppu_flow import written_gprs

DEFAULT_OUTPUT = REPO_ROOT / "generated" / "analysis" / "probe.json"

SCHEMA = 1

SPU_RELATED_MODULES = frozenset({
    "cellSpurs", "cellSpursJq", "cellSync", "cellSync2", "cellFiber", "cellDaisy",
    "cellSpudll",
})

_ELF_MAGIC = b"\x7fELF"
# ET_REL..ET_CORE. Rejects zero-filled headers that follow a stray magic.
_VALID_E_TYPES = range(1, 5)


def _hex(value: int | None) -> str | None:
    return None if value is None else "0x%08x" % value


def _flags_str(flags: int) -> str:
    return "".join(ch if flags & bit else "-"
                   for ch, bit in (("R", PF_R), ("W", PF_W), ("X", PF_X)))


def _file_offset_to_va(main: ElfFile, offset: int) -> int | None:
    for seg in main.loads():
        start = main.base + seg.p_offset
        if start <= offset < start + seg.p_filesz:
            return seg.p_vaddr + (offset - start)
    return None


def _try_parse_at(data: bytes, hit: int) -> ElfFile | None:
    try:
        elf = ElfFile.from_bytes(data, base=hit)
    except ElfError:
        return None
    return elf if elf.e_type in _VALID_E_TYPES else None


def _image_json(main: ElfFile, data: bytes, hit: int, elf: ElfFile) -> dict:
    size = elf.image_extent()
    return {
        "file_offset": _hex(hit),
        "vaddr": _hex(_file_offset_to_va(main, hit)),
        "class": 32 if elf.ei_class == ELFCLASS32 else 64,
        "machine": elf.e_machine,
        "e_type": elf.e_type,
        "entry": _hex(elf.e_entry),
        "segments": [{"type": _hex(s.p_type), "vaddr": _hex(s.p_vaddr),
                      "filesz": s.p_filesz, "memsz": s.p_memsz,
                      "flags": _flags_str(s.p_flags)} for s in elf.segments],
        "size": size,
        "sha256": hashlib.sha256(data[hit:hit + size]).hexdigest(),
    }


_SC_LOOKBACK = 24
_BRANCHES = frozenset({"b", "bc", "bclr", "bcctr"})
# `sc` with LEV = 0, the lv2 syscall form. The decoder's `sc` entry matches the primary
# opcode only, so it also hits float constants and ASCII data; compare the whole word.
_SC_WORD = 0x44000002


def _is_unconditional_branch(insn) -> bool:
    name = insn.spec.name
    if name == "b":
        return True
    return name in ("bc", "bclr", "bcctr") and insn.field("BO") & 0x14 == 0x14


def resolve_syscall_number(words: tuple[int, ...], index: int) -> int | None:
    """Find the `li r11, N` that feeds the `sc` at `words[index]`, scanning backwards."""
    # Known limitation: a branch target between the `li` and the `sc` is not detected
    # without a CFG, so a join point could in theory mix in another path's r11.
    for j in range(index - 1, max(index - 1 - _SC_LOOKBACK, -1), -1):
        insn = decode(words[j], strict=True)
        spec = insn.spec
        if spec is None:
            return None
        if spec.name == "addi" and insn.field("rD") == 11 and insn.field("rA") == 0:
            return insn.field("SIMM")
        if 11 in written_gprs(insn):
            return None
        if spec.name in _BRANCHES:
            # A call clobbers r11; an unconditional jump leaves the fall-through path.
            if "lk" in spec.flags and insn.field("LK"):
                return None
            if _is_unconditional_branch(insn):
                return None
    return None


def census_syscalls(main: ElfFile) -> dict:
    """Count `sc` sites and resolve the lv2 number from a preceding `li r11, N`."""
    sites = unresolved = 0
    by_number: Counter = Counter()
    for seg in main.exec_segments():
        size = seg.p_filesz // 4 * 4
        words = struct.unpack(">%dI" % (size // 4), main.read_va(seg.p_vaddr, size))
        for i, word in enumerate(words):
            if word != _SC_WORD:
                continue
            sites += 1
            number = resolve_syscall_number(words, i)
            if number is None:
                unresolved += 1
            else:
                by_number[number] += 1
    return {
        "sites": sites,
        "unresolved": unresolved,
        "by_number": {str(n): by_number[n] for n in sorted(by_number)},
    }


def build_probe(main: ElfFile, data: bytes, path: str, sha256: str,
                syscall_names: dict[int, str] | None = None) -> dict:
    spu_images, other_elfs = [], []
    invalid = 0
    hit = data.find(_ELF_MAGIC, 1)
    while hit >= 0:
        elf = _try_parse_at(data, hit)
        if elf is None:
            invalid += 1
        else:
            image = _image_json(main, data, hit, elf)
            if elf.ei_class == ELFCLASS32 and elf.e_machine == EM_SPU:
                spu_images.append(image)
            else:
                other_elfs.append(image)
        hit = data.find(_ELF_MAGIC, hit + 1)

    syscalls = census_syscalls(main)
    names = syscall_names or {}
    syscalls["names"] = {n: names[int(n)] for n in syscalls["by_number"] if int(n) in names}

    prx = cell.parse_prx_param(main)
    modules = cell.parse_imports(main, prx) if prx is not None else []
    imports = [{"name": None if m.unknown_layout else m.name,
                "num_func": len(m.functions),
                "spu_related": not m.unknown_layout and m.name in SPU_RELATED_MODULES}
               for m in modules]
    return {
        "schema": SCHEMA,
        "input": {"path": path, "sha256": sha256},
        "spu_images": spu_images,
        "other_elfs": other_elfs,
        "invalid_magic_hits": invalid,
        "imports": imports,
        "syscalls": syscalls,
        "summary": {
            "spu_image_count": len(spu_images),
            "spu_related_imports": [i["name"] for i in imports if i["spu_related"]],
        },
    }


def _load_default_config() -> dict | None:
    # The pin file is only used to recognise the encrypted EBOOT.BIN by hash;
    # a missing or broken file must not stop the probe.
    try:
        return load_config(DEFAULT_CONFIG)
    except CliError:
        return None


def _resolve_syscall_names(syscalls_arg: str | None) -> dict[int, str]:
    if syscalls_arg is not None:
        return load_syscall_table(Path(syscalls_arg))
    if not DEFAULT_SYSCALLS.is_file():
        print("note: syscall table %s not found; syscall names are not resolved"
              % DEFAULT_SYSCALLS, file=sys.stderr)
        return {}
    return load_syscall_table(DEFAULT_SYSCALLS)


def _run(args: argparse.Namespace) -> int:
    data = read_input(args.elf)
    sha256 = hashlib.sha256(data).hexdigest()
    reject_encrypted(data, sha256, _load_default_config())
    main_elf = parse_ppu_elf(data)
    syscall_names = _resolve_syscall_names(args.syscalls)

    try:
        report = build_probe(main_elf, data, args.elf, sha256, syscall_names)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)

    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    try:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(report, f, indent=2, sort_keys=False, ensure_ascii=False)
            f.write("\n")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (out_path, e.strerror))

    for img in report["spu_images"]:
        print("spu file_off=%s vaddr=%s size=0x%08x entry=%s sha256=%s"
              % (img["file_offset"], img["vaddr"], img["size"], img["entry"],
                 img["sha256"][:16]))
    summary = report["summary"]
    print("spu images: %d" % summary["spu_image_count"])
    print("spu-related imports: %s" % (", ".join(summary["spu_related_imports"]) or "none"))
    sc = report["syscalls"]
    print("syscalls: %d sites, %d distinct, %d unresolved"
          % (sc["sites"], len(sc["by_number"]), sc["unresolved"]))
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py probe",
        description="Search a decrypted PS3 PPU ELF for embedded SPU images "
                    "and SPU-related imports.")
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("-o", "--output", help="JSON output path "
                        "(default: generated/analysis/probe.json)")
    parser.add_argument("--syscalls", help="lv2 syscall name table "
                        "(default: config/lv2_syscalls.json)")
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

"""`info` command: JSON summary of a decrypted PS3 PPU ELF and SHA-256 pin check."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from . import cell
from .elf import EM_PPC64, ELFCLASS64, PF_R, PF_W, PF_X, ElfError, ElfFile, is_self
from .nids import DEFAULT_NIDS, NidTable, load_nid_table

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / "config" / "target.json"
DEFAULT_OUTPUT = REPO_ROOT / "generated" / "analysis" / "eboot_info.json"

SCHEMA = 1

EXIT_OK = 0
EXIT_IO = 1
EXIT_NOT_PPU_ELF = 3
EXIT_PIN_MISMATCH = 4

SELF_HINT = ("encrypted SELF; decrypt it with RPCS3 (Utilities > Decrypt PS3 Binaries) "
             "and pass the .elf")

_SEGMENT_TYPE_NAMES = {
    1: "LOAD",
    2: "DYNAMIC",
    4: "NOTE",
    7: "TLS",
    0x60000001: "SCE_PROC_PARAM",
    0x60000002: "SCE_PRX_PARAM",
    0x700000A4: "SCE_PPURELA",
}


class CliError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _hex(value: int | None) -> str | None:
    return None if value is None else "0x%08x" % value


def _segment_json(seg) -> dict:
    flags = "".join(ch if seg.p_flags & bit else "-"
                    for ch, bit in (("R", PF_R), ("W", PF_W), ("X", PF_X)))
    return {
        "index": seg.index,
        "type": _hex(seg.p_type),
        "type_name": _SEGMENT_TYPE_NAMES.get(seg.p_type),
        "offset": _hex(seg.p_offset),
        "vaddr": _hex(seg.p_vaddr),
        "paddr": _hex(seg.p_paddr),
        "filesz": seg.p_filesz,
        "memsz": seg.p_memsz,
        "flags": flags,
        "align": _hex(seg.p_align),
    }


def _section_json(sec) -> dict:
    return {
        "index": sec.index,
        "name": sec.name,
        "type": _hex(sec.sh_type),
        "flags": _hex(sec.sh_flags),
        "addr": _hex(sec.sh_addr),
        "offset": _hex(sec.sh_offset),
        "size": sec.sh_size,
        "data_present": sec.data_present,
    }


def _tls_json(tls: dict | None) -> dict | None:
    if tls is None:
        return None
    return {
        "vaddr": _hex(tls["vaddr"]),
        "offset": _hex(tls["offset"]),
        "filesz": tls["filesz"],
        "memsz": tls["memsz"],
        "align": _hex(tls["align"]),
    }


def _proc_param_json(pp: dict | None) -> dict | None:
    if pp is None:
        return None
    return {
        "addr": _hex(pp["addr"]),
        "size": pp["size"],
        "magic": _hex(pp["magic"]),
        "version": _hex(pp["version"]),
        "sdk_version": _hex(pp["sdk_version"]),
        "primary_prio": pp["primary_prio"],
        "primary_stacksize": _hex(pp["primary_stacksize"]),
        "malloc_pagesize": _hex(pp["malloc_pagesize"]),
        "ppc_seg": _hex(pp["ppc_seg"]),
        "raw": pp["raw"],
    }


def _prx_param_json(prx: dict | None) -> dict | None:
    if prx is None:
        return None
    return {
        "addr": _hex(prx["addr"]),
        "size": prx["size"],
        "magic": _hex(prx["magic"]),
        "version": _hex(prx["version"]),
        "libent_start": _hex(prx["libent_start"]),
        "libent_end": _hex(prx["libent_end"]),
        "libstub_start": _hex(prx["libstub_start"]),
        "libstub_end": _hex(prx["libstub_end"]),
        "ver": _hex(prx["ver"]),
    }


def _import_json(mod: cell.ImportModule, nid_table: NidTable | None) -> dict:
    module_name = None if mod.unknown_layout else mod.name

    def func_name(fnid: int) -> str | None:
        return None if nid_table is None else nid_table.lookup(module_name or "", fnid)

    out = {
        "entry_addr": _hex(mod.entry_addr),
        "name": module_name,
        "version": _hex(mod.version),
        "attributes": _hex(mod.attributes),
        "functions": [{"fnid": _hex(f.fnid), "name": func_name(f.fnid),
                       "slot_addr": _hex(f.slot_addr), "slot_value": _hex(f.slot_value)}
                      for f in mod.functions],
        "variables": [{"vnid": _hex(vnid), "vstub": _hex(vstub)}
                      for vnid, vstub in mod.variables],
        "num_tlsvar": mod.num_tlsvar,
    }
    if mod.unknown_layout:
        out["size"] = mod.size
        out["unknown_layout"] = True
    return out


def _export_json(mod: cell.ExportModule) -> dict:
    out = {
        "entry_addr": _hex(mod.entry_addr),
        "name": mod.name,
        "version": _hex(mod.version),
        "attributes": _hex(mod.attributes),
        "functions": [{"fnid": _hex(f.fnid), "opd_addr": _hex(f.opd_addr),
                       "func": _hex(f.func), "toc": _hex(f.toc)}
                      for f in mod.functions],
        "variables": [{"vnid": _hex(vnid), "addr": _hex(addr)}
                      for vnid, addr in mod.variables],
        "num_tlsvar": mod.num_tlsvar,
    }
    if mod.unknown_layout:
        out["size"] = mod.size
        out["unknown_layout"] = True
    return out


def _range_json(rng: tuple[int, int]) -> dict:
    return {"start": _hex(rng[0]), "end": _hex(rng[1])}


def _module_info_json(elf: ElfFile, info: cell.ModuleInfo) -> dict:
    return {
        "addr": _hex(info.addr),
        "name": info.name,
        "version": "%d.%d" % (info.version_major, info.version_minor),
        "attributes": _hex(info.attributes),
        "toc": _hex(info.toc),
        "exports": _range_json((info.exports_start, info.exports_end)),
        "imports": _range_json((info.imports_start, info.imports_end)),
        "text_range": _range_json(cell.find_text_range(elf)),
        "code_range": _range_json(cell.find_code_range(elf)),
    }


def pin_key(elf: ElfFile) -> str:
    """The target config entry that pins this input."""
    if cell.is_prx(elf):
        return "libsre_prx" if cell.has_unapplied_relocations(elf) else "libsre_reloc_elf"
    return "eboot_elf"


def build_summary(elf: ElfFile, path: str, size: int, sha256: str, pin: str,
                  nid_table: NidTable | None = None) -> dict:
    entry = cell.parse_entry(elf)
    prx = cell.parse_prx_param(elf)
    module_info = cell.parse_module_info(elf)
    tables = cell.library_tables(elf)
    imports = cell.parse_imports(elf, tables) if tables is not None else []
    exports = cell.parse_exports(elf, tables) if tables is not None else []
    opd = cell.parse_opd_section(elf)
    summary = {
        "schema": SCHEMA,
        "input": {"path": path, "size": size, "sha256": sha256, "pin": pin},
        "elf": {
            "class": 64 if elf.ei_class == ELFCLASS64 else 32,
            "osabi": _hex(elf.osabi),
            "type": elf.e_type,
            "machine": elf.e_machine,
            "entry": _hex(elf.e_entry),
            "flags": _hex(elf.e_flags),
        },
        "entry": {
            "kind": entry.kind,
            "opd_addr": _hex(entry.opd_addr),
            "func": _hex(entry.func),
            "toc": _hex(entry.toc),
        },
        "segments": [_segment_json(s) for s in elf.segments],
        "sections": [_section_json(s) for s in elf.sections],
        "tls": _tls_json(cell.parse_tls(elf)),
        "process_param": _proc_param_json(cell.parse_proc_param(elf)),
        "prx_param": _prx_param_json(prx),
        "opd": None if opd is None else {
            "count": len(opd),
            "toc_values": [_hex(t) for t in sorted({toc for _func, toc in opd})],
        },
        "imports": [_import_json(m, nid_table) for m in imports],
        "exports": [_export_json(m) for m in exports],
    }
    if module_info is not None:
        summary["module_info"] = _module_info_json(elf, module_info)
    return summary


def load_config(path: Path) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            config = json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read target config %s: %s" % (path, e.strerror))
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid target config %s: %s" % (path, e))
    if not isinstance(config, dict):
        raise CliError(EXIT_IO, "invalid target config %s: not an object" % path)
    return config


def pinned_sha(config: dict, key: str) -> str | None:
    section = config.get(key)
    sha = section.get("sha256") if isinstance(section, dict) else None
    return sha.lower() if isinstance(sha, str) else None


def read_input(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))


def reject_encrypted(data: bytes, sha256: str, config: dict | None) -> None:
    if is_self(data) or (config is not None and sha256 == pinned_sha(config, "eboot_bin")):
        raise CliError(EXIT_NOT_PPU_ELF, SELF_HINT)


def parse_ppu_elf(data: bytes) -> ElfFile:
    try:
        elf = ElfFile.from_bytes(data)
        if elf.ei_class != ELFCLASS64 or elf.e_machine != EM_PPC64:
            raise ElfError("not a 64-bit PPC ELF (class %d, machine %d)"
                           % (elf.ei_class, elf.e_machine))
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "not a decrypted PPU ELF: %s" % e)
    return elf


def resolve_nid_table(nids_arg: str | None) -> NidTable | None:
    if nids_arg is not None:
        return load_nid_table(Path(nids_arg))
    if not DEFAULT_NIDS.is_file():
        print("note: NID table %s not found; import names are not resolved" % DEFAULT_NIDS,
              file=sys.stderr)
        return None
    return load_nid_table(DEFAULT_NIDS)


def _run(args: argparse.Namespace) -> int:
    data = read_input(args.elf)

    sha256 = hashlib.sha256(data).hexdigest()
    config = None if args.no_verify else load_config(Path(args.target_config))

    reject_encrypted(data, sha256, config)
    elf = parse_ppu_elf(data)
    nid_table = resolve_nid_table(args.nids)

    pin = "skipped"
    if config is not None:
        key = pin_key(elf)
        pinned = pinned_sha(config, key)
        if pinned is None:
            pin = "unpinned"
            print("note: no %s sha256 is pinned; size=%d sha256=%s\n"
                  "      record them under %s in the target config to pin this input"
                  % (key, len(data), sha256, key), file=sys.stderr)
        else:
            pin = "verified" if pinned == sha256 else "mismatch"

    try:
        summary = build_summary(elf, args.elf, len(data), sha256, pin, nid_table)
    except ElfError as e:
        raise CliError(EXIT_NOT_PPU_ELF, "malformed PS3 structures: %s" % e)

    out_path = Path(args.output) if args.output else DEFAULT_OUTPUT
    try:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(summary, f, indent=2, sort_keys=False, ensure_ascii=False)
            f.write("\n")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (out_path, e.strerror))

    entry = summary["entry"]
    print("sha256:   %s" % sha256)
    print("pin:      %s" % pin)
    print("entry:    func=%s toc=%s (%s)" % (entry["func"], entry["toc"], entry["kind"]))
    if "module_info" in summary:
        module = summary["module_info"]
        print("module:   %s %s toc %s" % (module["name"], module["version"], module["toc"]))
    print("segments: %d" % len(summary["segments"]))
    print("sections: %d" % len(summary["sections"]))
    import_funcs = [(m["name"], f) for m in summary["imports"] for f in m["functions"]]
    print("imports:  %d modules, %d functions, %d named"
          % (len(summary["imports"]), len(import_funcs),
             sum(1 for _module, f in import_funcs if f["name"] is not None)))
    for module, f in import_funcs:
        if f["name"] is None:
            print("unresolved import: %s %s" % (module or "<unknown>", f["fnid"]))
    print("exports:  %d functions"
          % sum(len(m["functions"]) for m in summary["exports"]))
    print("output:   %s" % out_path)

    if pin == "mismatch":
        raise CliError(EXIT_PIN_MISMATCH,
                        "SHA-256 %s does not match the pinned %s value" % (sha256, key))
    return EXIT_OK


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py info",
        description="Summarise a decrypted PS3 PPU ELF as JSON and check its SHA-256 pin.")
    parser.add_argument("elf", help="decrypted EBOOT.elf")
    parser.add_argument("-o", "--output", help="JSON output path "
                        "(default: generated/analysis/eboot_info.json)")
    parser.add_argument("--target-config", default=str(DEFAULT_CONFIG),
                        help="target pin file (default: config/target.json)")
    parser.add_argument("--nids", help="FNID -> name table "
                        "(default: config/nids.json; names stay null if it is missing)")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the SHA-256 pin check")
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

"""Command-line interface of the save data tool: decrypt, inspect, patch and install Tales of Xillia 2 saves."""

from __future__ import annotations

import argparse
import errno
import json
import os
import re
import sys
from pathlib import Path

from . import EXIT_FORMAT, EXIT_IO, EXIT_USAGE, EXIT_VERIFY, CliError
from . import codes, pfd, save, sfo

_PFD_FILE = "PARAM.PFD"
_SECURE_ID_RE = re.compile(r"[0-9A-Fa-f]{32}")
_SFO_FILE = "PARAM.SFO"
_SAVE_ROOT = Path("dev_hdd0") / "home" / "00000001" / "savedata"
_SLOT_PREFIX = "BLJS10188_SAVE1_"
_MAX_SLOT = 999
# Same rule as PFD entry names: the file names end up in the hdd tree.
_SAFE_FILE_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")


def _read(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, os.strerror(e.errno or errno.EIO))) from e


def _write(path: Path, data: bytes) -> None:
    try:
        path.write_bytes(data)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, os.strerror(e.errno or errno.EIO))) from e


def _mkdir(path: Path) -> None:
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, os.strerror(e.errno or errno.EIO))) from e


def _is_inside_or_same(path: Path, root: Path) -> bool:
    path, root = path.resolve(), root.resolve()
    return path == root or root in path.parents


def _list_dir(path: Path) -> list[Path]:
    try:
        return sorted(path.iterdir())
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, os.strerror(e.errno or errno.EIO))) from e


def _is_non_empty(path: Path) -> bool:
    if not path.exists():
        return False
    if not path.is_dir():
        return True
    return bool(_list_dir(path))


def _decrypt(args: argparse.Namespace) -> int:
    save_dir: Path = args.save_dir
    out_dir: Path = args.output
    if not _SECURE_ID_RE.fullmatch(args.secure_file_id):
        raise CliError(EXIT_USAGE, "--secure-file-id must be 32 hex digits")
    secure_id = bytes.fromhex(args.secure_file_id)
    if not save_dir.is_dir():
        raise CliError(EXIT_IO, "%s is not a directory" % save_dir)
    if _is_inside_or_same(out_dir, save_dir):
        raise CliError(EXIT_USAGE, "-o %s is the save directory or inside it" % out_dir)
    if _is_non_empty(out_dir):
        raise CliError(EXIT_IO, "refusing to write into non-empty %s" % out_dir)

    pfd_path = save_dir / _PFD_FILE
    if not pfd_path.is_file():
        raise CliError(
            EXIT_FORMAT, "%s has no %s; it is not an encrypted PS3 save (recomp saves are already plain)" %
            (save_dir, _PFD_FILE))
    table = pfd.parse_pfd(_read(pfd_path))

    encrypted = [e for e in table.entries if not pfd.is_plain_entry(e.name)]
    # The key file is only needed when something is encrypted.
    syscon_key = pfd.load_syscon_key(args.keys if args.keys is not None else pfd.default_keys_dir()) \
        if encrypted else b""

    # Matched by exact name: on a case-insensitive file system `save / "save"` would also find SAVE.
    present = {p.name for p in _list_dir(save_dir) if p.is_file()}
    outputs: dict[str, bytes] = {}
    decrypted: set[str] = set()
    for entry in encrypted:
        path = save_dir / entry.name
        if entry.name not in present:
            raise CliError(EXIT_FORMAT, "%s lists '%s' but %s does not exist" % (_PFD_FILE, entry.name, path))
        plain = pfd.decrypt_entry(entry, _read(path), syscon_key, secure_id)
        if entry.name == "SAVE":
            try:
                save.parse_save(plain)
            except CliError:
                raise CliError(
                    EXIT_VERIFY,
                    "decrypted SAVE has no valid header; is %s the right key?" % pfd.SYSCON_KEY_NAME) from None
        outputs[entry.name] = plain
        decrypted.add(entry.name)

    for path in _list_dir(save_dir):
        if path.name == _PFD_FILE or path.name in decrypted:
            continue
        if path.is_dir():
            print("note: skipping subdirectory %s" % path.name, file=sys.stderr)
        elif path.is_file():
            outputs[path.name] = _read(path)

    # Everything is validated and in memory; only now touch the output location.
    _mkdir(out_dir)
    for name, data in outputs.items():
        _write(out_dir / name, data)
    for name in sorted(outputs):
        if name in decrypted:
            print("%s: decrypted (%d bytes)" % (name, len(outputs[name])))
        else:
            print("%s: copied" % name)
    print("output: %s" % out_dir)
    return 0


def _info_text(doc: dict) -> str:
    lines = [
        "size:       %d (total size field %d)" % (doc["size"], doc["total_size"]),
        "sections:   %d" % len(doc["sections"]),
    ]
    for s in doc["sections"]:
        # The game's SAVE has a few unnamed sections; a placeholder keeps the line from ending in a space.
        lines.append("section %2d offset %s size 0x%08x %s" % (s["index"], s["offset"], s["size"], s["name"]
                                                              or "(unnamed)"))
    lines.append("gald:       %d" % doc["gald"])
    frames = doc["play_time_frames"]
    seconds = frames // 60
    lines.append("play time:  %d:%02d:%02d (%d frames)" % (seconds // 3600, seconds // 60 % 60, seconds % 60, frames))
    for c in doc["characters"]:
        lines.append("character %d %s: hp %d tp %d exp %d ac %d sp %d str %d int %d vit %d dex %d agi %d spi %d" %
                     (c["index"], c["name"], c["hp"], c["tp"], c["exp"], c["ac"], c["sp"], c["strength"],
                      c["intelligence"], c["vitality"], c["dexterity"], c["agility"], c["spirit"]))
    return "\n".join(lines)


def _info(args: argparse.Namespace) -> int:
    doc = save.describe(save.parse_save(_read(args.save)))
    if args.json:
        print(json.dumps(doc, indent=2))
    else:
        print(_info_text(doc))
    return 0


def _load_savepatch(path: Path) -> codes.Savepatch:
    try:
        text = _read(path).decode("utf-8-sig")
    except UnicodeDecodeError as e:
        raise CliError(EXIT_FORMAT, "%s is not valid UTF-8 (%s)" % (path, e.reason)) from e
    return codes.parse_savepatch(text, str(path))


def _patch_list(args: argparse.Namespace) -> int:
    group = None
    for code in _load_savepatch(args.patch).codes:
        if code.group != group:
            group = code.group
            print("[GROUP:%s]" % group)
        print(code.name)
    return 0


def _patch(args: argparse.Namespace) -> int:
    # Option rules first, so that a bad combination never touches a file.
    if args.list:
        if args.patch is None:
            raise CliError(EXIT_USAGE, "--list needs --patch")
        if args.save is not None or args.code or args.raw or args.output is not None or args.in_place:
            raise CliError(EXIT_USAGE, "--list cannot be combined with SAVE, --code, --raw, -o or --in-place")
        return _patch_list(args)
    if args.save is None:
        raise CliError(EXIT_USAGE, "SAVE is required (or use --patch FILE --list)")
    if not args.code and not args.raw:
        raise CliError(EXIT_USAGE, "give at least one --code or --raw")
    if args.code and args.patch is None:
        raise CliError(EXIT_USAGE, "--code needs --patch")
    if args.in_place == (args.output is not None):
        raise CliError(EXIT_USAGE, "give exactly one of -o OUT and --in-place")
    if args.output is not None:
        if args.output.resolve() == args.save.resolve():
            raise CliError(EXIT_USAGE, "-o names the input file; use --in-place")
        if os.path.lexists(args.output):
            raise CliError(EXIT_IO, "refusing to overwrite existing %s" % args.output)

    plain = save.parse_save(_read(args.save)).data
    summary: list[str] = []
    writes: list[codes.Write] = []
    if args.code:
        patchfile = _load_savepatch(args.patch)
        for name in args.code:
            code = patchfile.find(name)
            if code.target not in (None, "SAVE"):
                raise CliError(
                    EXIT_FORMAT, "%s:%d: code '%s' targets '%s'; only SAVE is supported" %
                    (patchfile.source, code.line_no, code.name, code.target))
            compiled = codes.compile_code(code.lines, patchfile.source)
            writes += compiled
            summary.append("code '%s': %d write(s)" % (name, len(compiled)))
    for i, text in enumerate(args.raw, 1):
        compiled = codes.compile_code(codes.parse_raw(text, i), "--raw %d" % i)
        writes += compiled
        summary.append("raw %d: %d write(s)" % (i, len(compiled)))

    patched = bytearray(plain)
    codes.apply_writes(patched, writes)
    target = args.save if args.in_place else args.output
    _write(target, bytes(patched))
    for line in summary:
        print(line)
    print("output: %s" % target)
    return 0


def _install(args: argparse.Namespace) -> int:
    plain_dir: Path = args.plain_dir
    hdd: Path = args.hdd
    if args.slot is not None and not 1 <= args.slot <= _MAX_SLOT:
        raise CliError(EXIT_USAGE, "--slot must be between 1 and %d" % _MAX_SLOT)
    if not plain_dir.is_dir():
        raise CliError(EXIT_IO, "%s is not a directory" % plain_dir)
    if (plain_dir / _PFD_FILE).exists():
        raise CliError(EXIT_FORMAT, "%s has %s; it is encrypted, run 'decrypt' first" % (plain_dir, _PFD_FILE))

    present = {p.name for p in _list_dir(plain_dir) if p.is_file()}
    for name in (_SFO_FILE, "SAVE"):
        if name not in present:
            raise CliError(EXIT_FORMAT, "%s has no %s" % (plain_dir, name))
    source_sfo = sfo.parse_sfo(_read(plain_dir / _SFO_FILE))
    save.parse_save(_read(plain_dir / "SAVE"))

    if not hdd.is_dir():
        raise CliError(EXIT_IO, "%s is not a directory" % hdd)
    # The save root is created only after every check has passed, so a failed run leaves --hdd as it was.
    save_root = hdd / _SAVE_ROOT
    if args.slot is not None:
        slot_name = "%s%03d" % (_SLOT_PREFIX, args.slot)
        if os.path.lexists(save_root / slot_name):
            raise CliError(EXIT_IO, "%s already exists" % (save_root / slot_name))
    else:
        # The game numbers its slots from 1.
        free = (n for n in ("%s%03d" % (_SLOT_PREFIX, i) for i in range(1, _MAX_SLOT + 1))
                if not os.path.lexists(save_root / n))
        slot_name = next(free, None)
        if slot_name is None:
            raise CliError(EXIT_IO, "no free slot (001..%d) in %s" % (_MAX_SLOT, save_root))

    files: dict[str, bytes] = {}
    for path in _list_dir(plain_dir):
        if path.name == _SFO_FILE:
            continue
        if path.is_dir():
            print("note: skipping subdirectory %s" % path.name, file=sys.stderr)
        elif path.is_file():
            if not _SAFE_FILE_RE.fullmatch(path.name):
                print("note: skipping %s: unsupported file name" % path.name, file=sys.stderr)
                continue
            files[path.name] = _read(path)
    files[_SFO_FILE] = sfo.serialize_sfo(sfo.build_install_sfo(source_sfo, slot_name, files))

    slot_dir = save_root / slot_name
    _mkdir(save_root)
    try:
        slot_dir.mkdir()
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (slot_dir, os.strerror(e.errno or errno.EIO))) from e
    for name, data in files.items():
        _write(slot_dir / name, data)
    print("slot:   %s" % slot_name)
    print("files:  %s" % ", ".join(sorted(files)))
    print("output: %s" % slot_dir)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="tox2_savetool.py",
                                     description="Decrypt and inspect Tales of Xillia 2 save data.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("decrypt", help="decrypt a real-PS3 save directory into a plain directory")
    p.add_argument("save_dir", type=Path, help="save directory with PARAM.PFD and an encrypted SAVE")
    p.add_argument("-o", "--output", type=Path, required=True, metavar="OUT_DIR",
                   help="output directory; must not exist or must be empty")
    p.add_argument("--keys", type=Path, metavar="DIR", help="directory holding key_vault.h (default: the build's)")
    p.add_argument("--secure-file-id", metavar="HEX", default=pfd.TOX2_SECURE_FILE_ID.hex(),
                   help="32 hex digits (default: the id of Tales of Xillia 2)")
    p.set_defaults(func=_decrypt)

    p = sub.add_parser("info", help="print the section table and known fields of a plain SAVE")
    p.add_argument("save", type=Path, help="decrypted SAVE file")
    p.add_argument("--json", action="store_true", help="print JSON instead of text")
    p.set_defaults(func=_info)

    p = sub.add_parser("patch", help="apply Game Genie codes to a plain SAVE")
    p.add_argument("save", type=Path, nargs="?", help="decrypted SAVE file")
    p.add_argument("--patch", type=Path, metavar="FILE", help="Apollo .savepatch file")
    p.add_argument("--list", action="store_true", help="list the codes of --patch and exit")
    p.add_argument("--code", action="append", default=[], metavar="NAME",
                   help="name of a code in --patch; may be repeated")
    p.add_argument("--raw", action="append", default=[], metavar="CODE",
                   help="Game Genie lines as pairs of 8 hex digits; may be repeated")
    p.add_argument("-o", "--output", type=Path, metavar="OUT", help="output file; must not exist")
    p.add_argument("--in-place", action="store_true", help="overwrite SAVE")
    p.set_defaults(func=_patch)

    p = sub.add_parser("install", help="copy a plain save into the runtime's hdd tree")
    p.add_argument("plain_dir", type=Path, help="plain save directory (SAVE, PARAM.SFO, ...), e.g. from decrypt")
    p.add_argument("--hdd", type=Path, required=True, metavar="DIR", help="hdd directory given to tox2_game --hdd-dir")
    p.add_argument("--slot", type=int, metavar="N",
                   help="slot number 1..%d (default: the lowest free one)" % _MAX_SLOT)
    p.set_defaults(func=_install)
    return parser


def main(argv: list[str]) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return args.func(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code

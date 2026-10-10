"""`nidgen` command: build the FNID and lv2 syscall name tables from RPCS3 sources.

The sources are only read as text. The registration macros are the ones defined in
`PPUModule.h`: `REG_FUNC(module, func)` (name and hashed string are `func`),
`REG_FNID(module, nid, func)` (`nid` is a string literal that is hashed and also becomes the
name, or an integer literal that is used as is with `func` as the name), `REG_VAR` and
`REG_VNID` (the same for variables). `REG_HIDDEN_FUNC*` and `.flag(MFF_HIDDEN)` entries are
HLE-internal helpers and are skipped.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

from .info import CliError
from .nids import DEFAULT_NIDS, DEFAULT_SYSCALLS, NID_SUFFIX, compute_nid

EXIT_OK = 0
EXIT_IO = 1
EXIT_BAD_SOURCE = 3

SCHEMA = 1
RPCS3_REPOSITORY = "https://github.com/RPCS3/rpcs3"

_HEADER = Path("rpcs3/Emu/Cell/PPUModule.h")
_LV2 = Path("rpcs3/Emu/Cell/lv2/lv2.cpp")
_CELL_DIR = Path("rpcs3/Emu/Cell")

_FUNC_MACROS = ("REG_FUNC", "REG_FNID")
_VAR_MACROS = ("REG_VAR", "REG_VNID")
_HIDDEN_MACROS = ("REG_HIDDEN_FUNC", "REG_HIDDEN_FUNC_PURE")

_IDENT = r"[A-Za-z_]\w*"
_STR = r'"(?:[^"\\\n]|\\.)*"'
_INT = r"0[xX][0-9A-Fa-f]+"
_HIDDEN_FLAG = r"(?:\s*\.\s*flag\s*\([^)]*\bMFF_HIDDEN\b[^)]*\))?"
_ARGS = {
    "REG_FUNC": r"\(\s*(%s)\s*,\s*(%s)\s*\)" % (_IDENT, _IDENT),
    "REG_VAR": r"\(\s*(%s)\s*,\s*(%s)\s*\)" % (_IDENT, _IDENT),
    "REG_FNID": r"\(\s*(%s)\s*,\s*(%s|%s)\s*,\s*(%s)\s*\)" % (_IDENT, _STR, _INT, _IDENT),
    "REG_VNID": r"\(\s*(%s)\s*,\s*(%s|%s)\s*,\s*(%s)\s*\)" % (_IDENT, _STR, _INT, _IDENT),
}
_REG_CALL_RES = {name: re.compile(name + r"\b\s*" + args + "(" + _HIDDEN_FLAG + ")")
                 for name, args in _ARGS.items()}
_REG_WORD_RE = re.compile(r"\b(REG_[A-Z0-9_]+)\b")
_DECLARE_RE = re.compile(r"\bDECLARE\s*\(\s*ppu_module_manager::(%s)\s*\)\s*\(\s*(%s)"
                         % (_IDENT, _STR))
_DECLARE_WORD_RE = re.compile(r"\bDECLARE\s*\(\s*ppu_module_manager\b")

_SYSC_TABLE_NAME = "g_ppu_syscall_table"
_SYSC_ELEMENT_RE = re.compile(
    r"\s*(?:(BIND_SYSC|NULL_FUNC)\s*\(\s*(%s)\s*\)|(null_func|uns_func))\s*(?:,|$)" % _IDENT)
_SYSC_COMMENT_RE = re.compile(r"^\s*(\d+)(?![\d-])(?!\s*-)\s*(?:\(\s*0[xX]([0-9A-Fa-f]+)\s*\))?")


def strip_comments(text: str) -> str:
    """Remove `//` and `/* */` comments, keeping string literals and line numbers intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


class _Registry:
    def __init__(self):
        self.counts = Counter()
        self.hidden = Counter()
        self.unhandled: list[str] = []
        self.libraries: dict[str, str] = {}   # module variable -> library string
        # module variable -> nid -> set of names
        self.functions: dict[str, dict[int, set[str]]] = {}
        self.variables: dict[str, dict[int, set[str]]] = {}


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _scan_file(path: Path, display: str, reg: _Registry) -> None:
    text = strip_comments(_read_text(path))

    declared = 0
    for m in _DECLARE_RE.finditer(text):
        reg.libraries[m.group(1)] = m.group(2)[1:-1]
        declared += 1
    for m in _DECLARE_WORD_RE.finditer(text):
        # DECLARE(ppu_module_manager::x) in a form the regex does not understand.
        if declared == 0 or not _DECLARE_RE.match(text, m.start()):
            reg.unhandled.append("%s:%d: module declaration" % (display, _line_of(text, m.start())))

    for m in _REG_WORD_RE.finditer(text):
        word = m.group(1)
        where = "%s:%d: %s" % (display, _line_of(text, m.start()), word)
        line_start = text.rfind("\n", 0, m.start()) + 1
        prefix = text[line_start:m.start()]
        if prefix.lstrip().startswith("#"):
            continue   # a macro definition or conditional, not a registration
        if prefix.count('"') % 2:
            continue   # inside a string literal
        if word in _HIDDEN_MACROS:
            reg.hidden[word] += 1
            continue
        call_re = _REG_CALL_RES.get(word)
        call = call_re.match(text, m.start()) if call_re else None
        if call is None:
            reg.unhandled.append(where)
            continue

        is_func = word in _FUNC_MACROS
        module = call.group(1)
        if word in ("REG_FUNC", "REG_VAR"):
            name = call.group(2)
            nid = compute_nid(name)
            symbol_flags = call.group(3)
        else:
            nid_text, name = call.group(2), call.group(3)
            if nid_text.startswith('"'):
                name = nid_text[1:-1]
                nid = compute_nid(name)
            else:
                nid = int(nid_text, 16)
            symbol_flags = call.group(4)
        if "MFF_HIDDEN" in symbol_flags:
            reg.hidden[word] += 1
            continue
        reg.counts[word] += 1
        table = reg.functions if is_func else reg.variables
        table.setdefault(module, {}).setdefault(nid, set()).add(name)


def _scan_registrations(root: Path) -> _Registry:
    reg = _Registry()
    cell_dir = root / _CELL_DIR
    for path in sorted(cell_dir.rglob("*.cpp")):
        _scan_file(path, path.relative_to(root).as_posix(), reg)
    return reg


def _build_modules(raw: dict[str, dict[int, set[str]]], libraries: dict[str, str],
                   collisions: list[dict] | None) -> dict[str, dict[int, str]]:
    merged: dict[str, dict[int, set[str]]] = {}
    for module_var, entries in raw.items():
        key = libraries.get(module_var, module_var)
        target = merged.setdefault(key, {})
        for nid, names in entries.items():
            target.setdefault(nid, set()).update(names)
    modules: dict[str, dict[int, str]] = {}
    for key in sorted(merged):
        table = {}
        for nid in sorted(merged[key]):
            names = sorted(merged[key][nid])
            table[nid] = names[0]
            if len(names) > 1 and collisions is not None:
                collisions.append({"module": key, "nid": "0x%08x" % nid, "names": names})
        modules[key] = table
    return modules


def _modules_json(modules: dict[str, dict[int, str]]) -> dict:
    return {key: {"0x%08x" % nid: name for nid, name in table.items()}
            for key, table in modules.items()}


def parse_syscall_table(text: str, warnings: list[str] | None = None) -> dict[int, str]:
    """Return {number: name} from the lv2 syscall table initializer (named elements only)."""
    if warnings is None:
        warnings = []
    start = text.find(_SYSC_TABLE_NAME)
    brace = text.find("{", start) if start >= 0 else -1
    if brace < 0:
        raise CliError(EXIT_BAD_SOURCE, "syscall table %s not found" % _SYSC_TABLE_NAME)
    end = re.compile(r"^\};", re.M).search(text, brace)
    if end is None:
        raise CliError(EXIT_BAD_SOURCE, "syscall table %s is not terminated" % _SYSC_TABLE_NAME)

    names: dict[int, str] = {}
    index = 0
    first_line = text.count("\n", 0, brace) + 2
    for offset, line in enumerate(text[brace + 1:end.start()].split("\n")):
        line_no = first_line + offset - 1
        if "/*" in line:
            raise CliError(EXIT_BAD_SOURCE,
                           "lv2.cpp:%d: unsupported block comment in syscall table" % line_no)
        code, _sep, comment = line.partition("//")
        first_index = index
        pos = 0
        code = code.strip()
        while pos < len(code):
            m = _SYSC_ELEMENT_RE.match(code, pos)
            if m is None or m.end() == pos:
                raise CliError(EXIT_BAD_SOURCE, "lv2.cpp:%d: unrecognised syscall table element: %s"
                               % (line_no, code[pos:].strip()))
            if m.group(2):
                names[index] = m.group(2)
            index += 1
            pos = m.end()
        if index == first_index:
            continue
        # A single-number comment ("//141 (0x08D)") names the last element of its line (the
        # table has lines of several unused slots). Ranges and free text are not checked.
        c = _SYSC_COMMENT_RE.match(comment)
        if c is not None:
            number = int(c.group(1))
            if number != index - 1:
                raise CliError(EXIT_BAD_SOURCE,
                               "lv2.cpp:%d: comment says syscall %d but the element index is %d"
                               % (line_no, number, index - 1))
            # The hex part is only a typo-prone annotation of the decimal number; the decimal
            # comment and the element index already agree, so just report the mismatch.
            if c.group(2) and int(c.group(2), 16) != number:
                warnings.append("lv2.cpp:%d: comment hex 0x%s does not match syscall %d"
                                % (line_no, c.group(2), number))
    if index > 1024:
        raise CliError(EXIT_BAD_SOURCE, "syscall table has %d elements, expected at most 1024"
                       % index)
    return names


def read_source_commit(root: Path) -> str | None:
    git = root / ".git"
    try:
        head = (git / "HEAD").read_text(encoding="ascii").strip()
        if not head.startswith("ref:"):
            return head if re.fullmatch(r"[0-9a-f]{40}", head) else None
        ref = head[4:].strip()
        ref_file = git / ref
        if ref_file.is_file():
            sha = ref_file.read_text(encoding="ascii").strip()
            return sha if re.fullmatch(r"[0-9a-f]{40}", sha) else None
        packed = git / "packed-refs"
        if packed.is_file():
            for line in packed.read_text(encoding="ascii").splitlines():
                parts = line.split()
                if len(parts) == 2 and parts[1] == ref and re.fullmatch(r"[0-9a-f]{40}", parts[0]):
                    return parts[0]
    except (OSError, UnicodeDecodeError):
        pass
    return None


def _write_json(path: Path, doc: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
            f.write("\n")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _run(args: argparse.Namespace) -> int:
    root = Path(args.rpcs3)
    for rel in (_HEADER, _LV2):
        if not (root / rel).is_file():
            raise CliError(EXIT_IO, "not an RPCS3 checkout: %s is missing" % (root / rel))

    # The macros are implemented from the header; refuse a tree where they changed shape.
    header = strip_comments(_read_text(root / _HEADER))
    for macro in _FUNC_MACROS + _VAR_MACROS + _HIDDEN_MACROS:
        if re.search(r"#\s*define\s+%s\b" % macro, header) is None:
            raise CliError(EXIT_BAD_SOURCE, "%s does not define %s" % (_HEADER, macro))

    commit = args.source_commit if args.source_commit else read_source_commit(root)

    reg = _scan_registrations(root)
    collisions: list[dict] = []
    modules = _build_modules(reg.functions, reg.libraries, collisions)
    variables = _build_modules(reg.variables, reg.libraries, None)
    collisions.sort(key=lambda c: (c["module"], c["nid"]))
    sysc_warnings: list[str] = []
    syscalls = parse_syscall_table(_read_text(root / _LV2), sysc_warnings)

    source = {"project": "RPCS3", "repository": RPCS3_REPOSITORY, "commit": commit}
    _write_json(Path(args.nids_out), {
        "schema": SCHEMA,
        "source": source,
        "nid_suffix": NID_SUFFIX.hex(),
        "modules": _modules_json(modules),
        "variables": _modules_json(variables),
        "collisions": collisions,
    })
    _write_json(Path(args.syscalls_out), {
        "schema": SCHEMA,
        "source": source,
        "syscalls": {str(number): syscalls[number] for number in sorted(syscalls)},
    })

    print("source commit: %s" % commit)
    for macro in _FUNC_MACROS + _VAR_MACROS:
        print("%-22s %d" % (macro + ":", reg.counts[macro]))
    for macro in _HIDDEN_MACROS:
        print("%-22s %d (skipped)" % (macro + ":", reg.hidden[macro]))
    print("hidden via flag: %d (skipped)" % sum(
        n for macro, n in reg.hidden.items() if macro not in _HIDDEN_MACROS))
    print("modules:         %d" % len(modules))
    print("functions:       %d" % sum(len(t) for t in modules.values()))
    print("variables:       %d" % sum(len(t) for t in variables.values()))
    print("syscalls:        %d" % len(syscalls))
    print("collisions:      %d" % len(collisions))
    for c in collisions:
        print("  %s %s: %s" % (c["module"], c["nid"], ", ".join(c["names"])))
    print("syscall comment warnings: %d" % len(sysc_warnings))
    for item in sysc_warnings:
        print("  %s" % item)
    print("unhandled constructs: %d" % len(reg.unhandled))
    for item in reg.unhandled:
        print("  %s" % item)
    print("nids:     %s" % args.nids_out)
    print("syscalls: %s" % args.syscalls_out)
    return EXIT_OK


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py nidgen",
        description="Build the FNID and lv2 syscall name tables from an RPCS3 checkout.")
    parser.add_argument("--rpcs3", required=True, help="RPCS3 checkout root")
    parser.add_argument("--source-commit",
                        help="commit to record (default: read from the checkout's .git)")
    parser.add_argument("--nids-out", default=str(DEFAULT_NIDS),
                        help="NID table output (default: config/nids.json)")
    parser.add_argument("--syscalls-out", default=str(DEFAULT_SYSCALLS),
                        help="syscall table output (default: config/lv2_syscalls.json)")
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

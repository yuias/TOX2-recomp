"""Running an SPU-capable `objdump` under WSL on raw LS bytes, and reading its listing.

The objdump is a separate GNU binutils program (built by `tools/build-spu-objdump.sh`); it is executed
and its text output is parsed, nothing of it is linked or copied. The code bytes of a segment go in
through stdin, so WSL needs no access to the repository drive.

Both sides of the cross-check (our `spu_format` text and objdump's text) go through `canon_text`, so the
formatter is checked too and the normalisation is symmetric.
"""

from __future__ import annotations

import itertools
import re
import struct
import subprocess
from dataclasses import dataclass

from .info import CliError
from .spu import SPECS_BY_NAME

EXIT_OBJDUMP_UNAVAILABLE = 6
EXIT_LISTING = 1

# Environment variable that names the objdump inside WSL when --objdump is not given.
OBJDUMP_ENV = "TOX2_SPU_OBJDUMP"
DEFAULT_DISTRO = "Ubuntu-26.04"
# Resolved inside WSL: a positional argument is not tilde-expanded, so the default stays in the script.
_DEFAULT_OBJDUMP_SH = "${1:-$HOME/.local/opt/tox2-spu-binutils/bin/spu-elf-objdump}"

# The objdump path ($1) and VMA ($2) are positional arguments, never interpolated into the script text.
# `-z` prints runs of zero words as instructions instead of "..."; `-EB` is not needed with `-b binary`.
RUN_SCRIPT = (
    'f=$(mktemp) || exit 1\n'
    'cat >"$f"\n'
    '"' + _DEFAULT_OBJDUMP_SH + '" -D -z -b binary -m spu --adjust-vma="$2" "$f"\n'
    's=$?\n'
    'rm -f "$f"\n'
    'exit $s\n'
)
VERSION_SCRIPT = 'exec "' + _DEFAULT_OBJDUMP_SH + '" --version\n'

BUILD_HINT = "build it with: wsl.exe -e bash -s < tools/build-spu-objdump.sh"
TIMEOUT_SECONDS = 300
# Branch and hint targets are LS addresses; objdump prints them without the LSLR wrap in some cases.
TARGET_MODULUS = 0x40000


class ObjdumpUnavailable(CliError):
    def __init__(self, detail: str):
        super().__init__(EXIT_OBJDUMP_UNAVAILABLE, "objdump not available: %s; %s" % (detail, BUILD_HINT))


class ListingError(CliError):
    def __init__(self, message: str):
        super().__init__(EXIT_LISTING, message)


# --- Invocation -----------------------------------------------------------------------------


def resolve_objdump(option: str | None, environ: dict[str, str]) -> str:
    """Path of the objdump inside WSL: the option, else the environment variable, else "" (script default)."""
    if option:
        return option
    return environ.get(OBJDUMP_ENV, "")


def wsl_command(distro: str | None, script: str, *args: str) -> list[str]:
    command = ["wsl.exe"]
    if distro:
        command += ["-d", distro]
    return command + ["-e", "sh", "-c", script, "sh", *args]


def _decode(data: bytes) -> str:
    # `wsl.exe` itself reports failures in UTF-16; the tools run through `-e` write plain text.
    if b"\0" in data:
        try:
            return data.decode("utf-16-le").strip()
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", errors="replace")


def _run(command: list[str], data: bytes) -> str:
    try:
        result = subprocess.run(command, input=data, capture_output=True, timeout=TIMEOUT_SECONDS)
    except FileNotFoundError as e:
        raise ObjdumpUnavailable("cannot run wsl.exe (%s)" % (e.strerror or e))
    except subprocess.TimeoutExpired:
        raise ListingError("objdump did not finish within %d s" % TIMEOUT_SECONDS)
    stderr = _decode(result.stderr).strip()
    if result.returncode == 127:
        raise ObjdumpUnavailable(stderr or "exit status 127")
    if result.returncode != 0:
        raise ListingError("objdump failed with status %d: %s" % (result.returncode, stderr or _decode(result.stdout).strip()))
    return _decode(result.stdout)


def run_objdump(code: bytes, vma: int, objdump: str = "", distro: str | None = DEFAULT_DISTRO) -> str:
    """Disassemble raw SPU code that is loaded at LS address `vma`; returns objdump's listing."""
    return _run(wsl_command(distro, RUN_SCRIPT, objdump, "0x%x" % vma), code)


def objdump_version(objdump: str = "", distro: str | None = DEFAULT_DISTRO) -> str:
    """First line of `objdump --version`."""
    text = _run(wsl_command(distro, VERSION_SCRIPT, objdump), b"")
    lines = text.splitlines()
    return lines[0].strip() if lines else ""


# --- Canonical form -------------------------------------------------------------------------


@dataclass(frozen=True)
class Operand:
    kind: str  # "reg", "chan", "spr", "imm", "mem", "target", "raw" (text that is not understood)
    value: int
    base: int = -1  # base register of "mem"
    text: str = ""  # only for "raw"


@dataclass(frozen=True)
class CanonInsn:
    mnemonic: str  # base mnemonic; ".long" for an undecoded word
    flags: frozenset[str]  # "e", "d", "p", "c" suffix bits split off the mnemonic
    operands: tuple[Operand, ...]


UNDECODED = (".long", ".word", "(bad)")

# Operand names of our decoder table that hold LS addresses (branch, hint and lqa/lqr targets).
_TARGET_FIELDS = frozenset({"REL16", "ABS16", "RO", "RO_IND"})


def _suffix_table() -> dict[str, tuple[str, frozenset[str]]]:
    """Printed mnemonic -> (base mnemonic, flag bits). Whole mnemonics win over suffixed ones: `bisled` is
    its own instruction, not `bisl` with two flags."""
    table: dict[str, tuple[str, frozenset[str]]] = {}
    for spec in SPECS_BY_NAME.values():
        table[spec.mnemonic] = (spec.mnemonic, frozenset())
    for spec in SPECS_BY_NAME.values():
        letters = [f.lower() for f in spec.flag_fields]
        for count in range(1, len(letters) + 1):
            for order in itertools.permutations(letters, count):
                table.setdefault(spec.mnemonic + "".join(order), (spec.mnemonic, frozenset(order)))
    return table


_SUFFIXES = _suffix_table()

_COMMENT_RE = re.compile(r"\s+#.*\Z")
_SYMBOL_RE = re.compile(r"\s*<[^<>]*>\Z")
_REG_RE = re.compile(r"\$(\d+)\Z")
_CHAN_RE = re.compile(r"\$ch(\d+)\Z")
# objdump spells the SPR operand `$sp<N>`; `spu_format` prints `$spr<N>`.
_SPR_RE = re.compile(r"\$spr?(\d+)\Z")
_MEM_RE = re.compile(r"([+-]?\w+)\(\$(\d+)\)\Z")
_NUM_RE = re.compile(r"([+-]?)(0[xX][0-9a-fA-F]+|\d+|[0-9a-fA-F]+)\Z")


def parse_number(text: str) -> int | None:
    """Decimal or hex, with or without `0x`, with an optional sign; None when not a number."""
    match = _NUM_RE.match(text.strip())
    if not match:
        return None
    sign, digits = match.groups()
    if digits[:2] in ("0x", "0X"):
        value = int(digits, 16)
    elif digits.isdigit():
        value = int(digits, 10)
    else:
        value = int(digits, 16)
    return -value if sign == "-" else value


def _parse_operand(text: str, is_target: bool) -> Operand:
    text = text.strip()
    for regex, kind in ((_REG_RE, "reg"), (_CHAN_RE, "chan"), (_SPR_RE, "spr")):
        match = regex.match(text)
        if match:
            return Operand(kind, int(match.group(1)))
    match = _MEM_RE.match(text)
    if match:
        disp = parse_number(match.group(1))
        if disp is not None:
            return Operand("mem", disp, int(match.group(2)))
    # A branch target may carry a `<symbol+offset>` label.
    value = parse_number(_SYMBOL_RE.sub("", text))
    if value is not None:
        return Operand("target" if is_target else "imm", value)
    return Operand("raw", 0, text=text)


def canon_text(text: str) -> CanonInsn:
    """Canonical form of `mnemonic operands` text from either `spu_format` or objdump."""
    text = _COMMENT_RE.sub("", text.strip())
    if not text:
        return CanonInsn(".long", frozenset(), ())
    parts = text.split(None, 1)
    printed, rest = parts[0], parts[1] if len(parts) > 1 else ""
    if printed in UNDECODED:
        return CanonInsn(".long", frozenset(), ())
    base, flags = _SUFFIXES.get(printed, (printed, frozenset()))
    spec = SPECS_BY_NAME.get(base)
    names = spec.operands if spec is not None else ()
    raw_operands = [o for o in rest.split(",") if o.strip()] if rest else []
    if base == "stop":
        # objdump prints no stop code; the code carries no decode information.
        raw_operands = []
    operands = []
    for index, raw in enumerate(raw_operands):
        name = names[index] if index < len(names) else ""
        operands.append(_parse_operand(raw, name in _TARGET_FIELDS))
    return CanonInsn(base, flags, tuple(operands))


# --- Listing --------------------------------------------------------------------------------

_LINE_RE = re.compile(r"\s*([0-9a-f]+):\t((?:[0-9a-f]{2} ){4})\s*(.*)\Z")


@dataclass(frozen=True)
class ListingLine:
    addr: int
    word: int
    text: str  # mnemonic and operands, as printed after the raw bytes


def parse_listing(text: str, ls: int, code: bytes) -> list[ListingLine]:
    """Instruction lines of a listing of `code` at LS address `ls`.

    Header, section and label lines are skipped. Every word must appear exactly once at the expected
    address with the bytes of `code`; otherwise the listing is out of step and every category would be
    meaningless.
    """
    words = len(code) // 4
    out: list[ListingLine] = []
    for line in text.splitlines():
        match = _LINE_RE.match(line)
        if not match:
            continue
        addr = int(match.group(1), 16)
        raw = bytes(int(h, 16) for h in match.group(2).split())
        index = len(out)
        expected = ls + 4 * index
        if index >= words or addr != expected or raw != code[4 * index:4 * index + 4]:
            raise ListingError("listing out of step at ls 0x%05x" % (expected if index < words else addr))
        out.append(ListingLine(addr, struct.unpack(">I", raw)[0], match.group(3)))
    if len(out) != words:
        raise ListingError("listing out of step at ls 0x%05x" % (ls + 4 * len(out)))
    return out

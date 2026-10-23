"""Game Genie style codes and Apollo `.savepatch` parsing, applied to a plain SAVE buffer."""

from __future__ import annotations

import re
from collections.abc import Sequence
from dataclasses import dataclass

from . import EXIT_FORMAT, EXIT_USAGE, CliError

_CODE_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})$")
_WORD = re.compile(r"^[0-9A-Fa-f]{8}$")
_WIDTH_BY_TYPE = {0: 1, 1: 2, 2: 4}
_UNSUPPORTED = "unsupported code type {type:X} T={t:X} (only 0, 1, 2 with T=0 and 4 with T=0..2)"


@dataclass(frozen=True)
class Code:
    name: str
    group: str | None
    target: str | None  # last ':' line before the header, None if none
    line_no: int  # header line
    lines: tuple[tuple[int, str], ...]  # (line number, stripped text) of the body lines


@dataclass(frozen=True)
class Savepatch:
    source: str  # file path, for messages
    codes: tuple[Code, ...]

    def find(self, name: str) -> Code:
        matches = [c for c in self.codes if c.name == name]
        if not matches:
            raise CliError(EXIT_USAGE, f"no code named '{name}' in {self.source} (--list shows the names)")
        if len(matches) > 1:
            where = ", ".join(str(c.line_no) for c in matches)
            raise CliError(EXIT_FORMAT, f"code name '{name}' appears more than once in {self.source} (lines {where})")
        return matches[0]


@dataclass(frozen=True)
class Write:
    offset: int
    width: int  # 1, 2 or 4
    value: int
    where: str  # "<source>:<line>" for error messages


def parse_savepatch(text: str, source: str) -> Savepatch:
    codes: list[Code] = []
    group: str | None = None
    target: str | None = None
    # Open code: (name, group, target, header line, body lines)
    current: tuple[str, str | None, str | None, int, list[tuple[int, str]]] | None = None

    def close() -> None:
        nonlocal current
        if current is not None:
            name, cur_group, cur_target, line_no, body = current
            codes.append(Code(name, cur_group, cur_target, line_no, tuple(body)))
            current = None

    # Split on "\n" only: splitlines() would also break on form feeds and Unicode separators, which would
    # shift the line numbers reported to the user. strip() removes the "\r" of CRLF files.
    for line_no, raw in enumerate(text.removeprefix("\ufeff").split("\n"), 1):
        s = raw.strip()
        if not s or s.startswith(";"):
            continue
        if s.startswith(":"):
            close()
            target = s[1:].strip()
            if not target:
                raise CliError(EXIT_FORMAT, f"{source}:{line_no}: empty target name")
        elif s.startswith("[") and s.endswith("]"):
            close()
            inner = s[1:-1]
            if inner.startswith("GROUP:"):
                group = inner[len("GROUP:") :]
            else:
                current = (inner, group, target, line_no, [])
        elif current is None:
            raise CliError(EXIT_FORMAT, f"{source}:{line_no}: line outside a code")
        else:
            current[4].append((line_no, s))
    close()
    return Savepatch(source, tuple(codes))


def parse_raw(text: str, index: int) -> list[tuple[int, str]]:
    source = f"--raw {index}"
    tokens = text.split()
    if len(tokens) % 2 != 0:
        raise CliError(EXIT_FORMAT, f"{source}: expected pairs of 8 hex digits, got {len(tokens)} tokens")
    for token in tokens:
        if not _WORD.match(token):
            raise CliError(EXIT_FORMAT, f"{source}: '{token}' is not 8 hex digits")
    return [(i + 1, f"{tokens[2 * i]} {tokens[2 * i + 1]}") for i in range(len(tokens) // 2)]


def _split_line(line_no: int, text: str, source: str) -> tuple[int, int, str]:
    where = f"{source}:{line_no}"
    m = _CODE_LINE.match(text)
    if not m:
        raise CliError(EXIT_FORMAT, f"{where}: not a code line")
    return int(m.group(1), 16), int(m.group(2), 16), where


def compile_code(lines: Sequence[tuple[int, str]], source: str) -> list[Write]:
    if not lines:
        raise CliError(EXIT_FORMAT, f"{source}: code has no lines")
    writes: list[Write] = []
    i = 0
    while i < len(lines):
        addr_word, value, where = _split_line(*lines[i], source)
        i += 1
        kind, t, addr = addr_word >> 28, (addr_word >> 24) & 0xF, addr_word & 0xFFFFFF
        if kind in _WIDTH_BY_TYPE and t == 0:
            width = _WIDTH_BY_TYPE[kind]
            _check_fits(value, width, where)
            writes.append(Write(addr, width, value, where))
        elif kind == 4 and t in (0, 1, 2):
            width = 1 << t
            _check_fits(value, width, where)
            if i >= len(lines):
                raise CliError(EXIT_FORMAT, f"{where}: type 4 code needs a second line")
            addr2, increment, where2 = _split_line(*lines[i], source)
            i += 1
            if addr2 >> 28 != 4:
                raise CliError(EXIT_FORMAT, f"{where2}: second line of a type 4 code must start with 4")
            count, step = (addr2 >> 16) & 0xFFF, addr2 & 0xFFFF
            if count == 0:
                raise CliError(EXIT_FORMAT, f"{where2}: type 4 code writes 0 values")
            mask = (1 << (8 * width)) - 1
            for n in range(count):
                writes.append(Write(addr + n * step, width, (value + n * increment) & mask, where))
        else:
            raise CliError(EXIT_FORMAT, f"{where}: " + _UNSUPPORTED.format(type=kind, t=t))
    return writes


def _check_fits(value: int, width: int, where: str) -> None:
    # Apollo masks the extra bits, but a value that has them set means a misread code.
    if value >> (8 * width):
        raise CliError(EXIT_FORMAT, f"{where}: value 0x{value:08X} does not fit in {width} byte(s)")


def apply_writes(buf: bytearray, writes: Sequence[Write]) -> None:
    # Check every write first so that a failure leaves the buffer untouched.
    for w in writes:
        if w.offset + w.width > len(buf):
            raise CliError(
                EXIT_FORMAT, f"{w.where}: offset 0x{w.offset:06x} + {w.width} is outside the {len(buf)}-byte SAVE"
            )
    for w in writes:
        buf[w.offset : w.offset + w.width] = w.value.to_bytes(w.width, "big")

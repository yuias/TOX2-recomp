"""Syscon key loading and PARAM.PFD handling for real-PS3 Tales of Xillia 2 saves."""

from __future__ import annotations

import errno
import os
import re
from pathlib import Path

from . import EXIT_IO, CliError

REPO_ROOT = Path(__file__).resolve().parents[2]
KEYS_CMAKE = REPO_ROOT / "cmake" / "rpcs3_keys.cmake"
SYSCON_KEY_NAME = "SC_ISO_SERIES_KEY_1"
# A per-title identifier published in pfdtool/Apollo games.conf, not a console key.
TOX2_SECURE_FILE_ID = bytes.fromhex("2E7F0BC9F5383AB3DC4E8EA303F04D8A")

_KEYS_COMMIT_RE = re.compile(r'^\s*set\(\s*TOX2_RPCS3_KEYS_COMMIT\s+"([0-9A-Fa-f]{40})"\s*\)', re.MULTILINE)
_HEX_BYTE_RE = re.compile(r"0[xX][0-9A-Fa-f]{2}")
_KEY_LEN = 16


def default_keys_dir(cmake_file: Path = KEYS_CMAKE) -> Path:
    # The build downloads key_vault.h into a directory named after the pinned RPCS3 commit.
    try:
        text = cmake_file.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (cmake_file, os.strerror(e.errno or errno.EIO))) from e
    match = _KEYS_COMMIT_RE.search(text)
    if not match:
        raise CliError(EXIT_IO, 'no set(TOX2_RPCS3_KEYS_COMMIT "<commit>") line in %s' % cmake_file)
    return REPO_ROOT / "generated" / "deps" / "rpcs3" / match.group(1).lower()


def _is_ident_char(c: str) -> bool:
    return c.isascii() and (c.isalnum() or c == "_")


def _strip_comments(text: str) -> str:
    # Same rules as tox2_decrypt/keys.cpp: comments inside double-quoted strings stay; a block comment
    # becomes one space so that it still separates tokens.
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                s = text[i]
                i += 1
                out.append(s)
                if s == "\\" and i < n:
                    out.append(text[i])
                    i += 1
                elif s == '"':
                    break
        elif c == "/" and text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _skip_space(text: str, pos: int) -> int:
    while pos < len(text) and text[pos] in " \t\r\n\f\v":
        pos += 1
    return pos


def parse_syscon_key(header_text: str) -> bytes:
    text = _strip_comments(header_text)
    name = SYSCON_KEY_NAME
    pos = 0
    while True:
        begin = text.find(name, pos)
        if begin < 0:
            raise CliError(EXIT_IO, "%s: not found in key_vault.h" % name)
        end = begin + len(name)
        pos = end
        whole = (begin == 0 or not _is_ident_char(text[begin - 1])) and (end >= len(text) or
                                                                         not _is_ident_char(text[end]))
        bracket = _skip_space(text, end)
        if not whole or bracket >= len(text) or text[bracket] != "[":
            continue
        open_brace = text.find("{", bracket)
        semicolon = text.find(";", bracket)
        # A use such as `NAME[i];` is not the initializer.
        if open_brace < 0 or (0 <= semicolon < open_brace):
            continue
        close = text.find("}", open_brace)
        if close < 0:
            raise CliError(EXIT_IO, "%s: initializer is not closed" % name)
        # Tokens of any other shape are ignored, so a wrong count shows up as a size error.
        tokens = [t for t in re.findall(r"[A-Za-z0-9_]+", text[open_brace + 1:close])
                  if len(t) == 4 and _HEX_BYTE_RE.fullmatch(t)]
        if len(tokens) != _KEY_LEN:
            raise CliError(EXIT_IO, "%s: found %d bytes, expected %d" % (name, len(tokens), _KEY_LEN))
        return bytes(int(t, 16) for t in tokens)


def load_syscon_key(keys_dir: Path) -> bytes:
    path = keys_dir / "key_vault.h"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise CliError(
            EXIT_IO,
            "cannot read %s (%s); configure the build with -EbootBin or -Pup (build.sh --eboot-bin/--pup) "
            'to download it, or pass --keys DIR; see docs/building.md, "Keys"' %
            (path, os.strerror(e.errno or errno.EIO))) from e
    return parse_syscon_key(text)

"""Syscon key loading and PARAM.PFD handling for real-PS3 Tales of Xillia 2 saves."""

from __future__ import annotations

import errno
import hmac
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path

from . import EXIT_FORMAT, EXIT_IO, EXIT_VERIFY, CliError
from .aes import Aes128, cbc_decrypt

REPO_ROOT = Path(__file__).resolve().parents[2]
KEYS_CMAKE = REPO_ROOT / "cmake" / "rpcs3_keys.cmake"
SYSCON_KEY_NAME = "SC_ISO_SERIES_KEY_1"
# A per-title identifier published in pfdtool/Apollo games.conf, not a console key.
TOX2_SECURE_FILE_ID = bytes.fromhex("2E7F0BC9F5383AB3DC4E8EA303F04D8A")

_KEYS_COMMIT_RE = re.compile(r'^\s*set\(\s*TOX2_RPCS3_KEYS_COMMIT\s+"([0-9A-Fa-f]{40})"\s*\)', re.MULTILINE)
_HEX_BYTE_RE = re.compile(r"0[xX][0-9A-Fa-f]{2}")
_KEY_LEN = 16

_PFD_MAGIC = 0x50464442
_PFD_VERSIONS = (3, 4)
_HASH_TABLE_OFFSET = 0x60
_ENTRY_SIZE = 272
_NAME_OFFSET = 0x08
_NAME_FIELD = 65
_KEY_OFFSET = 0x50
_PFD_KEY_LEN = 64
_HASHES_OFFSET = 0x90
_FILE_SIZE_OFFSET = 0x108
# Bounds the table-size multiplications below; real saves use a handful of entries.
_MAX_TABLE = 4096
_SAFE_NAME_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")
_PLAIN_ENTRY = "PARAM.SFO"
# Bytes of the hash key that are fixed, as position -> value; the secure file id fills the rest.
_HASH_KEY_FIXED = {1: 11, 2: 15, 5: 14, 8: 10}


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


@dataclass(frozen=True)
class PfdEntry:
    index: int
    name: str
    key: bytes  # 64 bytes, encrypted entry key
    file_hashes: tuple[bytes, bytes, bytes, bytes]  # 4 x 20 bytes; [0] = file hash
    file_size: int


@dataclass(frozen=True)
class Pfd:
    version: int
    capacity: int
    num_reserved: int
    entries: tuple[PfdEntry, ...]


def _align16(n: int) -> int:
    return (n + 15) & ~15


def _pfd_error(reason: str) -> CliError:
    return CliError(EXIT_FORMAT, "PARAM.PFD: %s" % reason)


def parse_pfd(data: bytes) -> Pfd:
    if len(data) < _HASH_TABLE_OFFSET + 24:
        raise _pfd_error("file is too short")
    magic, version = struct.unpack_from(">QQ", data, 0)
    if magic != _PFD_MAGIC:
        raise _pfd_error("bad magic")
    if version not in _PFD_VERSIONS:
        raise _pfd_error("unsupported version %d" % version)
    capacity, num_reserved, num_used = struct.unpack_from(">QQQ", data, _HASH_TABLE_OFFSET)
    if capacity > _MAX_TABLE or num_reserved > _MAX_TABLE:
        raise _pfd_error("table size is out of range")
    if num_used > num_reserved:
        raise _pfd_error("%d entries in use but only %d reserved" % (num_used, num_reserved))
    table = _HASH_TABLE_OFFSET + 24 + capacity * 8
    if table + num_reserved * _ENTRY_SIZE > len(data):
        raise _pfd_error("entry table reaches past the end of the file")

    entries: list[PfdEntry] = []
    seen: set[str] = set()
    for i in range(num_used):
        base = table + i * _ENTRY_SIZE
        raw_name = data[base + _NAME_OFFSET:base + _NAME_OFFSET + _NAME_FIELD]
        end = raw_name.find(b"\0")
        if end < 0:
            raise _pfd_error("entry %d: name is not terminated" % i)
        try:
            name = raw_name[:end].decode("ascii")
        except UnicodeDecodeError:
            raise _pfd_error("entry %d: name is not ASCII" % i) from None
        if not _SAFE_NAME_RE.fullmatch(name):
            raise _pfd_error("entry %d: unsafe file name" % i)
        if name in seen:
            raise _pfd_error("duplicate entry '%s'" % name)
        seen.add(name)
        h = base + _HASHES_OFFSET
        hashes = (bytes(data[h:h + 20]), bytes(data[h + 20:h + 40]), bytes(data[h + 40:h + 60]),
                  bytes(data[h + 60:h + 80]))
        (file_size,) = struct.unpack_from(">Q", data, base + _FILE_SIZE_OFFSET)
        entries.append(
            PfdEntry(i, name, bytes(data[base + _KEY_OFFSET:base + _KEY_OFFSET + _PFD_KEY_LEN]), hashes,
                     file_size))
    return Pfd(version, capacity, num_reserved, tuple(entries))


def is_plain_entry(name: str) -> bool:
    # pfd.c compares with strncasecmp.
    return name.upper() == _PLAIN_ENTRY


def hash_key(secure_file_id: bytes) -> bytes:
    if len(secure_file_id) != 16:
        raise ValueError("secure file id must be 16 bytes")
    ids = iter(secure_file_id)
    return bytes(_HASH_KEY_FIXED[i] if i in _HASH_KEY_FIXED else next(ids) for i in range(20))


def file_hash(hash_key: bytes, stored: bytes) -> bytes:
    return hmac.digest(hash_key, stored, "sha1")


def entry_key(entry: PfdEntry, syscon_key: bytes, hash_key: bytes) -> bytes:
    return cbc_decrypt(syscon_key, hash_key[:16], entry.key)[:16]


def _counter_block(i: int) -> bytes:
    return i.to_bytes(8, "big") + bytes(8)


def decrypt_data(entry_key: bytes, stored: bytes) -> bytes:
    if len(stored) % 16:
        raise ValueError("data length must be a multiple of 16")
    cipher = Aes128(entry_key)
    out = bytearray()
    for i in range(0, len(stored), 16):
        mask = int.from_bytes(cipher.encrypt_block(_counter_block(i // 16)), "big")
        plain = int.from_bytes(cipher.decrypt_block(stored[i:i + 16]), "big") ^ mask
        out += plain.to_bytes(16, "big")
    return bytes(out)


def encrypt_data(entry_key: bytes, plain: bytes) -> bytes:
    cipher = Aes128(entry_key)
    padded = plain + bytes(-len(plain) % 16)
    out = bytearray()
    for i in range(0, len(padded), 16):
        mask = int.from_bytes(cipher.encrypt_block(_counter_block(i // 16)), "big")
        block = int.from_bytes(padded[i:i + 16], "big") ^ mask
        out += cipher.encrypt_block(block.to_bytes(16, "big"))
    return bytes(out)


def decrypt_entry(entry: PfdEntry, stored: bytes, syscon_key: bytes, secure_file_id: bytes) -> bytes:
    hkey = hash_key(secure_file_id)
    size = _align16(entry.file_size)
    if len(stored) < size:
        raise CliError(
            EXIT_FORMAT, "'%s' is %d bytes but PARAM.PFD says %d" % (entry.name, len(stored), entry.file_size))
    if not hmac.compare_digest(file_hash(hkey, stored), entry.file_hashes[0]):
        raise CliError(
            EXIT_VERIFY,
            "file hash of '%s' does not match PARAM.PFD; wrong secure file id (--secure-file-id)?" % entry.name)
    return decrypt_data(entry_key(entry, syscon_key, hkey), stored[:size])[:entry.file_size]

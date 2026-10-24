"""PARAM.SFO reader and writer, matching the runtime's `parseParamSfo` and `serializeParamSfo`."""

from __future__ import annotations

import re
import struct
from collections.abc import Iterable
from dataclasses import dataclass, field

from . import EXIT_FORMAT, CliError

_MAGIC = b"\x00PSF"
_HEADER_SIZE = 20
_INDEX_SIZE = 16
_FORMAT_BINARY = 0x0004
_FORMAT_UTF8 = 0x0204
_FORMAT_U32 = 0x0404
_VERSION = 0x101

# Media files are not secure by default; the runtime marks every other file (SAVE) as secure.
SECURE_DEFAULT_ZERO = ("ICON0.PNG", "ICON1.PAM", "PIC1.PNG", "SND0.AT3")

# Minimum capacities of text values, as in `textCapacity` of the runtime.
_MIN_CAPACITY = {"TITLE": 128, "TITLE_ID": 16, "VERSION": 8, "CATEGORY": 4}
_NOT_FILES = ("PARAM.SFO", "PARAM.PFD")
# The game starts SUB_TITLE with the number of the slot it saved to, and its load list shows that text.
_SUB_TITLE_SLOT_RE = re.compile(r"^\d{3}(?= )")


@dataclass
class Sfo:
    ints: dict[str, int] = field(default_factory=dict)  # format 0x0404
    strings: dict[str, str] = field(default_factory=dict)  # format 0x0204
    binary: dict[str, bytes] = field(default_factory=dict)  # format 0x0004 (US PARAMS, PARAMS2, ...)


def _align4(n: int) -> int:
    return (n + 3) & ~3


def parse_sfo(data: bytes) -> Sfo:
    if len(data) < _HEADER_SIZE or data[:4] != _MAGIC:
        raise CliError(EXIT_FORMAT, "not a PARAM.SFO (bad magic)")
    _, key_table, data_table, count = struct.unpack_from("<IIII", data, 4)
    if count > (len(data) - _HEADER_SIZE) // _INDEX_SIZE or key_table > len(data) or data_table > len(data):
        raise CliError(EXIT_FORMAT, "PARAM.SFO is truncated")

    sfo = Sfo()
    seen: set[str] = set()
    for i in range(count):
        key_off, fmt, length, _capacity, data_off = struct.unpack_from("<HHIII", data, _HEADER_SIZE + i * _INDEX_SIZE)
        start = key_table + key_off
        end = data.find(b"\0", start) if start < len(data) else -1
        if end < 0:
            raise CliError(EXIT_FORMAT, "PARAM.SFO entry %d has an unterminated key or a key outside the file" % i)
        try:
            key = data[start:end].decode("utf-8")
        except UnicodeDecodeError as e:
            raise CliError(EXIT_FORMAT, "PARAM.SFO entry %d has a key that is not valid UTF-8" % i) from e
        if key in seen:
            raise CliError(EXIT_FORMAT, "PARAM.SFO has the key '%s' more than once" % key)
        seen.add(key)

        if fmt not in (_FORMAT_U32, _FORMAT_UTF8, _FORMAT_BINARY):
            continue
        begin = data_table + data_off
        used = 4 if fmt == _FORMAT_U32 else length
        if begin + used > len(data) or (fmt == _FORMAT_U32 and length < 4):
            raise CliError(EXIT_FORMAT, "PARAM.SFO entry '%s' has its data outside the file" % key)
        if fmt == _FORMAT_U32:
            sfo.ints[key] = struct.unpack_from("<I", data, begin)[0]
        elif fmt == _FORMAT_BINARY:
            sfo.binary[key] = data[begin:begin + length]
        else:
            raw = data[begin:begin + length]
            nul = raw.find(b"\0")
            try:
                sfo.strings[key] = (raw if nul < 0 else raw[:nul]).decode("utf-8")
            except UnicodeDecodeError as e:
                raise CliError(EXIT_FORMAT, "PARAM.SFO entry '%s' is not valid UTF-8" % key) from e
    return sfo


def serialize_sfo(sfo: Sfo) -> bytes:
    if sfo.binary:
        raise ValueError("binary entries cannot be serialized: the runtime never writes them")
    both = sfo.ints.keys() & sfo.strings.keys()
    if both:
        raise ValueError("key '%s' is both an int and a string" % min(both))

    # (key bytes, format, length, capacity, data); std::map orders by the key's bytes.
    entries = []
    for key, value in sfo.ints.items():
        entries.append((key.encode("utf-8"), _FORMAT_U32, 4, 4, struct.pack("<I", value)))
    for key, text in sfo.strings.items():
        raw = text.encode("utf-8")
        capacity = _align4(max(_MIN_CAPACITY.get(key, 0), len(raw) + 1))
        entries.append((key.encode("utf-8"), _FORMAT_UTF8, len(raw) + 1, capacity, raw))
    entries.sort(key=lambda e: e[0])

    key_table = _HEADER_SIZE + _INDEX_SIZE * len(entries)
    data_table = key_table + _align4(sum(len(e[0]) + 1 for e in entries))
    out = bytearray(struct.pack("<4sIIII", _MAGIC, _VERSION, key_table, data_table, len(entries)))
    key_off = data_off = 0
    for key, fmt, length, capacity, _ in entries:
        out += struct.pack("<HHIII", key_off, fmt, length, capacity, data_off)
        key_off += len(key) + 1
        data_off += capacity
    for key, *_ in entries:
        out += key + b"\0"
    out += bytes(data_table - len(out))
    for _, _, _, capacity, payload in entries:
        out += payload.ljust(capacity, b"\0")
    return bytes(out)


def build_install_sfo(source: Sfo, dir_name: str, file_names: Iterable[str]) -> Sfo:
    sfo = Sfo()
    sfo.strings["ACCOUNT_ID"] = "0000000000000000"
    sfo.strings["CATEGORY"] = "SD"
    sfo.strings["SAVEDATA_DIRECTORY"] = dir_name
    for key in ("DETAIL", "SAVEDATA_LIST_PARAM", "SUB_TITLE", "TITLE"):
        sfo.strings[key] = source.strings.get(key, "")
    sfo.strings["SUB_TITLE"] = _SUB_TITLE_SLOT_RE.sub(dir_name[-3:], sfo.strings["SUB_TITLE"], count=1)
    for key in ("ATTRIBUTE", "PARENTAL_LEVEL"):
        sfo.ints[key] = source.ints.get(key, 0)
    for name in file_names:
        if name in _NOT_FILES:
            continue
        key = "*" + name
        default = 0 if name in SECURE_DEFAULT_ZERO else 1
        sfo.ints[key] = source.ints.get(key, default)
    return sfo

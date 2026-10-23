"""Builders for synthetic save data. Every key here is made up; no real key bytes belong in this file."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path
from typing import Iterable, Sequence

from tox2_save import aes, pfd

FAKE_SYSCON_KEY = bytes(range(0xA0, 0xB0))
# Differs from pfd.TOX2_SECURE_FILE_ID, so a run that relies on the built-in id fails the hash check.
FAKE_SECURE_ID = bytes(range(0x30, 0x40))


def _c_array(name: str, key: bytes, decl: str = "16") -> str:
    body = ", ".join("0x%02X" % b for b in key)
    return "constexpr u8 %s[%s] = {\n    %s\n};\n" % (name, decl, body)


def fake_key_vault_h(key: bytes, *, crlf: bool = False, omit: bool = False, short: bool = False) -> str:
    decoy = bytes(range(0x11, 0x21))
    lines = [
        "#pragma once",
        "",
        "// SC_ISO_SERIES_KEY_1[16] = { 0x00 }",
        "/* SC_ISO_SERIES_KEY_1[16] = { 0x00 } */",
        "constexpr int PASSPHRASE_KEY_LEN = 16;",
        _c_array("SC_ISO_SERIES_KEY_10", decoy),
        _c_array("SC_ISO_SERIES_KEY_2", decoy[::-1]),
        # The block-comment opener inside a string must not start a comment that swallows the real array.
        'const char* kNote = "SC_ISO_SERIES_KEY_1 /* not a comment";',
        # A subscript use before the real initializer is not the array.
        "u8 first = SC_ISO_SERIES_KEY_1[0];",
    ]
    if not omit:
        real = key[:15] if short else key
        body = ", ".join("0x%02X" % b for b in real[:8]) + ",\n    // 0xEE\n    " + \
            ", ".join("0x%02X" % b for b in real[8:])
        lines.append("constexpr u8 SC_ISO_SERIES_KEY_1[PASSPHRASE_KEY_LEN] = {\n    %s\n};" % body)
    lines.append("")
    text = "\n".join(lines)
    return text.replace("\n", "\r\n") if crlf else text


def build_pfd_dir(files: dict[str, bytes], encrypted: Iterable[str], *, syscon_key: bytes, secure_file_id: bytes,
                  version: int = 3, capacity: int = 7, num_reserved: int = 4) -> dict[str, bytes]:
    encrypted = set(encrypted)
    hkey = pfd.hash_key(secure_file_id)
    out: dict[str, bytes] = {}
    records: list[bytes] = []
    for name, plain in files.items():
        if name in encrypted:
            # Made-up per-file key, stored the way the console does: CBC-encrypted under the syscon key.
            entry_plain = hashlib.sha256(name.encode()).digest() * 2
            stored_key = aes.cbc_encrypt(syscon_key, hkey[:16], entry_plain)
            stored = pfd.encrypt_data(entry_plain[:16], plain)
            hashes = [pfd.file_hash(hkey, stored), bytes(20), bytes(20), bytes(20)]
            out[name] = stored
        elif pfd.is_plain_entry(name):
            stored_key, hashes = bytes(64), [bytes(20)] * 4
            out[name] = plain
        else:
            out[name] = plain
            continue
        record = bytearray(272)
        record[8:8 + len(name)] = name.encode()
        record[0x50:0x90] = stored_key
        for k, h in enumerate(hashes):
            record[0x90 + k * 20:0xA4 + k * 20] = h
        struct.pack_into(">Q", record, 0x108, len(plain))
        records.append(bytes(record))

    header = bytearray(0x78 + capacity * 8)
    struct.pack_into(">QQ", header, 0, 0x50464442, version)
    struct.pack_into(">QQQ", header, 0x60, capacity, num_reserved, len(records))
    table = b"".join(records) + bytes(max(0, num_reserved - len(records)) * 272)
    out["PARAM.PFD"] = bytes(header) + table
    return out


def write_dir(path: Path, files: dict[str, bytes]) -> None:
    path.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (path / name).write_bytes(data)


def tree_digest(path: Path) -> dict[str, str]:
    return {
        str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(path.rglob("*")) if p.is_file()
    }


_DEFAULT_SECTIONS = (
    ("SAVE_DATA_COMMON", 0x2D0, 0x100),
    ("PLAYER_3_STATUS_SAVE_DATA", 0x8890, 0xA000),
    ("SAVE_DATA_ID_ITEM_DATA_MANAGER", 0x8A170, 0x100),
)


def build_save(size: int = 0xACD00, sections: Sequence[tuple[str, int, int]] | None = None) -> bytearray:
    if sections is None:
        sections = [s for s in _DEFAULT_SECTIONS if s[1] + s[2] <= size]
    buf = bytearray(size)
    table_end = 0x10 + len(sections) * 16
    struct.pack_into(">HHI", buf, 0, 0x0064, len(sections), size)
    name_offset = table_end
    for i, (name, offset, length) in enumerate(sections):
        struct.pack_into(">IIII", buf, 0x10 + i * 16, name_offset, offset, length, 0)
        raw = name.encode("ascii") + b"\0"
        buf[name_offset:name_offset + len(raw)] = raw
        name_offset += len(raw)
    return buf

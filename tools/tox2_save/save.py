"""Section table and known fields of a plain (decrypted) Tales of Xillia 2 SAVE file."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from . import EXIT_FORMAT, CliError

SAVE_MAGIC = 0x0064
SAVE_SIZE = 0xACD00  # size of the game's SAVE; informational, not enforced
GALD_OFFSET = 0x2E8  # u32
PLAY_TIME_OFFSET = 0x2DC  # u32 frames at 60 fps; matches the hh:mm in PARAM.SFO
CHARACTER_BASE = 0x8890
CHARACTER_STRIDE = 0xA000
# Order taken from the nine "All Stats MAX" community codes, which write at 0x88D6 + k * 0xA000.
CHARACTERS = ("Alvin", "Jude", "Milla", "Elize", "Rowen", "Leia", "Gaius", "Muzet", "Ludger")
# Offsets inside a character block: u32, u32, u32, u8, u32.
HP, TP, EXP, AC, SP = 0x20, 0x24, 0x30, 0x3F, 0x40
STATS, STAT_STRIDE = 0x46, 4  # six u16 values
STAT_NAMES = ("strength", "intelligence", "vitality", "dexterity", "agility", "spirit")

_HEADER_SIZE = 0x10
_ENTRY_SIZE = 16
_MAX_NAME = 64
_MIN_DESCRIBE_SIZE = CHARACTER_BASE + (len(CHARACTERS) - 1) * CHARACTER_STRIDE + STATS + len(STAT_NAMES) * STAT_STRIDE


@dataclass(frozen=True)
class Section:
    index: int
    name: str
    offset: int
    size: int


@dataclass(frozen=True)
class SaveFile:
    data: bytes
    total_size: int
    sections: tuple[Section, ...]


def _not_a_save(reason: str) -> CliError:
    return CliError(
        EXIT_FORMAT, "%s; not a decrypted TOX2 SAVE (an encrypted SAVE must go through 'decrypt' first)" % reason)


def parse_save(data: bytes) -> SaveFile:
    if len(data) < _HEADER_SIZE:
        raise _not_a_save("file is too short")
    magic, count, total_size = struct.unpack_from(">HHI", data, 0)
    if magic != SAVE_MAGIC:
        raise _not_a_save("bad magic 0x%04x" % magic)
    if total_size > len(data):
        raise _not_a_save("total size 0x%x is larger than the file" % total_size)
    if _HEADER_SIZE + count * _ENTRY_SIZE > len(data):
        raise _not_a_save("section table reaches past the end of the file")

    sections: list[Section] = []
    for i in range(count):
        name_offset, offset, size, _ = struct.unpack_from(">IIII", data, _HEADER_SIZE + i * _ENTRY_SIZE)
        if name_offset >= len(data):
            raise _not_a_save("section %d: name offset is past the end of the file" % i)
        end = data.find(b"\0", name_offset)
        if end < 0:
            raise _not_a_save("section %d: name is not terminated" % i)
        raw = data[name_offset:end]
        # The game's own SAVE has unnamed sections (four of its 44 entries), so an empty name is valid.
        if len(raw) > _MAX_NAME or any(not 0x20 <= b <= 0x7E for b in raw):
            raise _not_a_save("section %d: invalid name" % i)
        if offset + size > len(data):
            raise _not_a_save("section %d: data reaches past the end of the file" % i)
        sections.append(Section(i, raw.decode("ascii"), offset, size))
    return SaveFile(data, total_size, tuple(sections))


def describe(save: SaveFile) -> dict:
    data = save.data
    if len(data) < _MIN_DESCRIBE_SIZE:
        raise CliError(
            EXIT_FORMAT, "SAVE is %d bytes; the known fields need at least %d" % (len(data), _MIN_DESCRIBE_SIZE))
    characters = []
    for k, name in enumerate(CHARACTERS):
        base = CHARACTER_BASE + k * CHARACTER_STRIDE
        entry = {
            "index": k,
            "name": name,
            "offset": "0x%08x" % base,
            "hp": struct.unpack_from(">I", data, base + HP)[0],
            "tp": struct.unpack_from(">I", data, base + TP)[0],
            "exp": struct.unpack_from(">I", data, base + EXP)[0],
            "ac": data[base + AC],
            "sp": struct.unpack_from(">I", data, base + SP)[0],
        }
        for s, stat in enumerate(STAT_NAMES):
            entry[stat] = struct.unpack_from(">H", data, base + STATS + s * STAT_STRIDE)[0]
        characters.append(entry)
    return {
        "schema": 1,
        "size": len(data),
        "total_size": save.total_size,
        "sections": [{
            "index": s.index,
            "name": s.name,
            "offset": "0x%08x" % s.offset,
            "size": s.size
        } for s in save.sections],
        "gald": struct.unpack_from(">I", data, GALD_OFFSET)[0],
        "play_time_frames": struct.unpack_from(">I", data, PLAY_TIME_OFFSET)[0],
        "characters": characters,
    }

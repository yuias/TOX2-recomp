"""Notifying stores of the `emit` command.

A plain guest store (`tox2WriteN`) does not touch the reservation table, so an SPU that waits for its
reservation on that line to be lost only sees the store at its next poll. The notify file lists the
store instructions that must go through `tox2NotifyWriteN` instead, keyed by the function that owns
them.
"""

from __future__ import annotations

import json
import re
from collections.abc import Collection, Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from .emit_db import DbFunction
from .emit_output import hexs
from .info import EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError
from .ppu_flow import CodeImage

DEFAULT_NOTIFY_STORES = REPO_ROOT / "config" / "ppu_notify_stores.json"
MAIN_IMAGE = "main"

ADDRESS_PATTERN = re.compile(r"0x[0-9a-fA-F]{1,8}")
IMAGE_PATTERN = re.compile(r"main|[a-z][a-z0-9]*")
ENTRY_KEYS = {"image", "function", "stores", "reason"}

# Integer and byte-reversed stores, the only ones that have a tox2NotifyWriteN form. Kept in step with
# the store table of emit_ops_mem by a unit test.
NOTIFY_MNEMONICS = frozenset(
    root + suffix
    for root in ("stb", "sth", "stw", "std")
    for suffix in ("", "u", "x", "ux")) | {"sthbrx", "stwbrx", "stdbrx"}


@dataclass(frozen=True)
class NotifyEntry:
    image: str
    function: int
    stores: tuple[int, ...]     # sorted
    reason: str


def _invalid(path: Path, detail: str) -> CliError:
    return CliError(EXIT_NOT_PPU_ELF, "invalid notify-stores file %s: %s" % (path, detail))


def _parse_address(path: Path, where: str, value: object) -> int:
    if not isinstance(value, str) or not ADDRESS_PATTERN.fullmatch(value):
        raise _invalid(path, "%s: expected a hex address such as 0x011d97e4" % where)
    return int(value, 16)


def image_name_of(prefix: str) -> str:
    """Value of the `image` key that selects the entries of the image emitted with `prefix`."""
    return prefix or MAIN_IMAGE


def load_notify_stores(path: Path, *, explicit: bool, image_name: str, image: CodeImage,
                       functions: Iterable[DbFunction]) -> list[NotifyEntry]:
    """Read and check the notify file; returns the entries of `image_name`, sorted by function.

    A missing file means no entries unless the user named it. Entries of other images are checked
    for their shape only: the functions and addresses they name belong to an image that is not
    loaded here.
    """
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        if explicit:
            raise CliError(EXIT_IO, "cannot read notify-stores file %s: no such file" % path)
        return []
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read notify-stores file %s: %s" % (path, e.strerror))
    except UnicodeDecodeError as e:
        raise _invalid(path, "not UTF-8 (%s)" % e.reason)
    try:
        doc = json.loads(raw)
    except ValueError as e:
        raise _invalid(path, "not JSON (%s)" % e)
    if (not isinstance(doc, dict) or set(doc) != {"notify_stores"}
            or not isinstance(doc["notify_stores"], list)):
        raise _invalid(path, 'expected an object with the single key "notify_stores" (a list)')

    by_entry = {fn.entry: fn for fn in functions}
    seen_functions: set[int] = set()
    seen_stores: set[int] = set()
    result = []
    for index, item in enumerate(doc["notify_stores"]):
        where = "notify_stores[%d]" % index
        if not isinstance(item, dict) or set(item) != ENTRY_KEYS:
            raise _invalid(path, "%s: expected the keys %s" % (where, sorted(ENTRY_KEYS)))
        image_value = item["image"]
        if not isinstance(image_value, str) or not IMAGE_PATTERN.fullmatch(image_value):
            raise _invalid(path, '%s: image must be "main" or a symbol prefix' % where)
        function = _parse_address(path, where + ".function", item["function"])
        reason = item["reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise _invalid(path, "%s: reason must be a non-empty string" % where)
        raw_stores = item["stores"]
        if not isinstance(raw_stores, list) or not raw_stores:
            raise _invalid(path, "%s: stores must be a non-empty list of hex addresses" % where)
        stores = [_parse_address(path, "%s.stores[%d]" % (where, k), value)
                  for k, value in enumerate(raw_stores)]
        if image_value != image_name:
            continue

        fn = by_entry.get(function)
        if fn is None:
            raise _invalid(path, "%s is not a function of the database" % hexs(function))
        if function in seen_functions:
            raise _invalid(path, "%s: function listed twice" % hexs(function))
        seen_functions.add(function)
        owned = set(fn.addrs())
        for addr in stores:
            if addr in seen_stores:
                raise _invalid(path, "%s: store listed twice" % hexs(addr))
            seen_stores.add(addr)
            if addr not in owned:
                raise _invalid(path, "%s: store is not owned by function %s"
                               % (hexs(addr), hexs(function)))
            if not image.valid(addr):
                raise _invalid(path, "%s: store does not decode" % hexs(addr))
            mnemonic = image.insn(addr).spec.name
            if mnemonic not in NOTIFY_MNEMONICS:
                raise _invalid(path, "%s: %s cannot be routed (only integer and byte-reversed "
                               "stores can)" % (hexs(addr), mnemonic))
        result.append(NotifyEntry(image_value, function, tuple(sorted(stores)), reason))
    result.sort(key=lambda e: e.function)
    return result


def store_addresses(entries: Iterable[NotifyEntry]) -> frozenset[int]:
    return frozenset(addr for entry in entries for addr in entry.stores)


def check_emitted(entries: Sequence[NotifyEntry], notified: Collection[int]) -> None:
    """Fail unless every listed store was emitted as a notifying store."""
    for entry in entries:
        for addr in entry.stores:
            if addr not in notified:
                raise CliError(
                    EXIT_NOT_PPU_ELF,
                    "notify-stores: %s in function %s was not emitted as a store (stale list: "
                    "the ELF, the database or the emitter changed, or the instruction is not "
                    "emitted as a plain store)" % (hexs(addr), hexs(entry.function)))


def report_section(entries: Sequence[NotifyEntry]) -> list[dict]:
    return [{"image": e.image, "function": hexs(e.function),
             "stores": [hexs(a) for a in e.stores], "reason": e.reason} for e in entries]

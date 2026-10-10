"""NID hashing and lookup tables (FNID -> name, lv2 syscall number -> name)."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_NIDS = REPO_ROOT / "config" / "nids.json"
DEFAULT_SYSCALLS = REPO_ROOT / "config" / "lv2_syscalls.json"

NID_SUFFIX = bytes.fromhex("6759659904250490566427499489741a")

EXIT_IO = 1


def compute_nid(name: str) -> int:
    digest = hashlib.sha1(name.encode("ascii") + NID_SUFFIX).digest()
    return int.from_bytes(digest[:4], "little")


@dataclass(frozen=True)
class NidTable:
    modules: dict[str, dict[int, str]]
    # Variable NIDs live in a separate namespace from function NIDs.
    variables: dict[str, dict[int, str]] = field(default_factory=dict)

    def lookup(self, module: str, nid: int) -> str | None:
        # The module's own entry wins; otherwise accept a name only if every module that
        # knows this NID agrees on it (some libraries re-export each other's functions).
        entry = self.modules.get(module)
        if entry is not None and nid in entry:
            return entry[nid]
        names = {table[nid] for table in self.modules.values() if nid in table}
        return next(iter(names)) if len(names) == 1 else None


def _load_json(path: Path, what: str) -> dict:
    # Imported here: info imports this module for the `info` command.
    from .info import CliError
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s %s: %s" % (what, path, e.strerror))
    except ValueError as e:
        raise CliError(EXIT_IO, "invalid %s %s: %s" % (what, path, e))
    if not isinstance(doc, dict):
        raise CliError(EXIT_IO, "invalid %s %s: not an object" % (what, path))
    return doc


def _parse_module_map(doc: dict, key: str, path: Path) -> dict[str, dict[int, str]]:
    from .info import CliError
    raw = doc.get(key, {})
    try:
        if not isinstance(raw, dict):
            raise ValueError("'%s' is not an object" % key)
        return {module: {int(nid, 16): str(name) for nid, name in entries.items()}
                for module, entries in raw.items()}
    except (ValueError, AttributeError, TypeError) as e:
        raise CliError(EXIT_IO, "invalid NID table %s: %s" % (path, e))


def load_nid_table(path: Path) -> NidTable:
    doc = _load_json(path, "NID table")
    return NidTable(_parse_module_map(doc, "modules", path),
                    _parse_module_map(doc, "variables", path))


def load_syscall_table(path: Path) -> dict[int, str]:
    from .info import CliError
    doc = _load_json(path, "syscall table")
    raw = doc.get("syscalls")
    try:
        if not isinstance(raw, dict):
            raise ValueError("'syscalls' is not an object")
        return {int(number): str(name) for number, name in raw.items()}
    except (ValueError, TypeError) as e:
        raise CliError(EXIT_IO, "invalid syscall table %s: %s" % (path, e))

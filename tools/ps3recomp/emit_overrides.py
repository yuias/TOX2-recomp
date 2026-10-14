"""Guest function overrides of the `emit` command.

An override file names guest functions whose `fn_<address>` is defined by a hand-written C source
instead of the emitter. The emitter still writes the generated body, under another name, so a review
can diff the two.
"""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Collection, Sequence
from dataclasses import dataclass
from pathlib import Path

from .emit_output import hexs
from .info import EXIT_IO, EXIT_NOT_PPU_ELF, REPO_ROOT, CliError

DEFAULT_OVERRIDES = REPO_ROOT / "config" / "ppu_overrides.json"
GENERATED_SUFFIX = "_generated"

ENTRY_PATTERN = re.compile(r"0x[0-9a-fA-F]{1,8}")
SHA256_PATTERN = re.compile(r"[0-9a-fA-F]{64}")
ENTRY_KEYS = {"entry", "generated_sha256", "reason"}


@dataclass(frozen=True)
class Override:
    entry: int
    generated_sha256: str       # lower case
    reason: str


def generated_name(name: str) -> str:
    """C name under which the emitter keeps the body of an overridden function."""
    return name + GENERATED_SUFFIX


def body_sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _invalid(path: Path, detail: str) -> CliError:
    return CliError(EXIT_NOT_PPU_ELF, "invalid override file %s: %s" % (path, detail))


def _parse_entry(path: Path, index: int, value: object) -> int:
    if not isinstance(value, str) or not ENTRY_PATTERN.fullmatch(value):
        raise _invalid(path, "overrides[%d]: entry must be a hex address such as 0x00956a00" % index)
    return int(value, 16)


def load_overrides(path: Path, *, explicit: bool, entries: Collection[int]) -> list[Override]:
    """Read and check the override file; returns the overrides sorted by entry.

    A missing file means no overrides unless the user named it. `entries` are the functions of the
    database. The hash pin is checked later, when the body has been emitted (`check_pin`).
    """
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        if explicit:
            raise CliError(EXIT_IO, "cannot read override file %s: no such file" % path)
        return []
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read override file %s: %s" % (path, e.strerror))
    except UnicodeDecodeError as e:
        raise _invalid(path, "not UTF-8 (%s)" % e.reason)
    try:
        doc = json.loads(raw)
    except ValueError as e:
        raise _invalid(path, "not JSON (%s)" % e)
    if not isinstance(doc, dict) or set(doc) != {"overrides"} or not isinstance(doc["overrides"], list):
        raise _invalid(path, 'expected an object with the single key "overrides" (a list)')

    known = set(entries)
    seen: set[int] = set()
    result = []
    for index, item in enumerate(doc["overrides"]):
        if not isinstance(item, dict) or set(item) != ENTRY_KEYS:
            raise _invalid(path, "overrides[%d]: expected the keys %s" % (index, sorted(ENTRY_KEYS)))
        entry = _parse_entry(path, index, item["entry"])
        sha = item["generated_sha256"]
        if not isinstance(sha, str) or not SHA256_PATTERN.fullmatch(sha):
            raise _invalid(path, "%s: generated_sha256 must be 64 hex digits" % hexs(entry))
        reason = item["reason"]
        if not isinstance(reason, str) or not reason.strip():
            raise _invalid(path, "%s: reason must be a non-empty string" % hexs(entry))
        if entry in seen:
            raise _invalid(path, "%s: listed twice" % hexs(entry))
        if entry not in known:
            raise _invalid(path, "%s is not a function of the database" % hexs(entry))
        seen.add(entry)
        result.append(Override(entry, sha.lower(), reason))
    result.sort(key=lambda o: o.entry)
    return result


def check_pin(override: Override, text: str) -> None:
    """Fail unless `text` (the plain `fn_<address>` body) is the code the override was derived from."""
    actual = body_sha256(text)
    if actual != override.generated_sha256:
        raise CliError(
            EXIT_NOT_PPU_ELF,
            "override %s: generated_sha256 does not match the emitted function (file: %s, actual: %s); "
            "the ELF, the database or the emitter changed, so derive the override again"
            % (hexs(override.entry), override.generated_sha256, actual))


def report_section(overrides: Sequence[Override]) -> list[dict]:
    return [{"entry": hexs(o.entry), "reason": o.reason} for o in overrides]

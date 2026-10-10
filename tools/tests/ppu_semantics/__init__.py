"""Semantics harness: hand-written PPU cases, compiled through the emitter and run natively.

`gen` turns the `CASES` of every `cases_<group>` module into C (see `gen.py`); the C driver
runs them against the expected register and memory state.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field

CODE_BASE = 0x10000     # entry of every case
CALLEE_BASE = 0x18000   # callees live in [CALLEE_BASE, DATA_START)
DATA_START = 0x20000    # guest data window of mem_init / mem_expect
DATA_END = 0xF0000
MEM_SIZE = 0x100000


@dataclass(frozen=True)
class Case:
    name: str                       # [a-z0-9_]+, unique across all case modules
    ref: str                        # e.g. "RPCS3 PPUInterpreter.cpp ADDIC"
    words: tuple[int, ...]          # placed at CODE_BASE; gen appends blr if the last word falls through
    init: Mapping[str, object] = field(default_factory=dict)
    expect: Mapping[str, object] = field(default_factory=dict)
    mem_init: Mapping[int, bytes] = field(default_factory=dict)
    mem_expect: Mapping[int, bytes] = field(default_factory=dict)
    # Extra functions, address in [CALLEE_BASE, DATA_START).
    callees: Mapping[int, tuple[int, ...]] = field(default_factory=dict)
    switch_targets: Mapping[int, tuple[int, ...]] = field(default_factory=dict)

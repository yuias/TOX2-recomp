"""SPU semantics harness: hand-written cases, compiled through the emitter and run natively.

`gen` turns the `CASES` of every `cases_<group>` module into C (see `gen.py`); the C driver
(`runtime/tests/spu_semantics_main.c`) runs each case against the expected register, local-store and
channel state. Expected values come from the SPU ISA documents, never from running the emitter.
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field

CODE_BASE = 0x01000     # entry of every case; code occupies [CODE_BASE, DATA_START)
DATA_START = 0x10000    # LS window of ls_init / ls_expect
DATA_END = 0x3E000
EXIT_LS = 0x3F000       # a dispatch to this address ends the case (outside code and data)


@dataclass(frozen=True)
class Case:
    name: str                       # [a-z0-9_]+, unique across all case modules
    ref: str                        # the SPU ISA 1.2 / CBEA rule the expected values come from
    words: tuple[int, ...]          # placed at CODE_BASE; gen appends `stop 0x102` if the last word falls through
    init: Mapping[str, object] = field(default_factory=dict)
    expect: Mapping[str, object] = field(default_factory=dict)
    ls_init: Mapping[int, bytes] = field(default_factory=dict)
    ls_expect: Mapping[int, bytes] = field(default_factory=dict)
    # Extra function entries: code addresses that a computed branch reaches (they become recovery seeds).
    entries: tuple[int, ...] = ()
    # Scripted and recorded channel traffic:
    #   "reads":      {channel: (value, ...)}   consumed in order by rdch
    #   "counts":     {channel: n}              rchcnt / bisled event count (default 0)
    #   "writes":     [(channel, value), ...]   every wrch, in order, expected exactly
    #   "stops":      [code, ...]               every stop executed (the appended terminator included)
    #   "stop_leave": (code, ...)               stop codes for which the hook asks the thread to leave
    #   "halts":      [site, ...]               taken heq/hgt/hlgt and stopd; the case ends at the first
    #                                           (default: none)
    #   "unhandled":  [(site, word), ...]       tox2SpuUnhandled calls (a word the emitter declined, the
    #                                           double-precision rounding-mode guard); the case ends at the
    #                                           first (default: none)
    #   "leave_channels": (channel, ...)        rdch/wrch of these channels make the hook ask the thread to
    #                                           leave (a write is still recorded; a read yields no value)
    channels: Mapping[str, object] = field(default_factory=dict)

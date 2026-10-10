"""Group `vmx_int`: integer vector add, subtract, average, min/max and compare.

Vector registers are byte-reversed in the host (see `runtime/include/tox2/ppu_context.h`), which is
invisible to lane-wise operations: they are emitted as one expression on the matching element view
(`u8`/`s8`, `u16`/`s16`, `u32`/`s32`). The saturating forms, the averages, the carry forms and the
CR6 update live in `runtime/include/tox2/ppu_vmx.h`.
"""

from __future__ import annotations

import re

from .emit_core import Op, handler
from .emit_db import GROUPS

# Element size letter -> (unsigned view, signed view, unsigned vector type).
_VIEWS = {
    "b": ("u8", "s8", "Tox2U8x16"),
    "h": ("u16", "s16", "Tox2U16x8"),
    "w": ("u32", "s32", "Tox2U32x4"),
}


def _names(pattern: str) -> list[str]:
    return [name for name in GROUPS["vmx_int"] if re.fullmatch(pattern, name)]


def _view(letter: str, signed: bool) -> str:
    return _VIEWS[letter][1 if signed else 0]


def _regs(op: Op) -> tuple[str, str, str]:
    return op.vr(op.f("vD")), op.vr(op.f("vA")), op.vr(op.f("vB"))


@handler(*_names(r"v(add|sub)u[bhw]m"))
def emit_modulo(op: Op) -> list[str] | None:
    # RPCS3 VADDUBM, VADDUHM, VADDUWM, VSUBUBM, VSUBUHM, VSUBUWM: wrapping lane arithmetic, done on
    # the unsigned view because signed vector overflow is not defined in C.
    d, a, b = _regs(op)
    view = _view(op.name[-2], False)
    sign = "+" if op.name.startswith("vadd") else "-"
    return ["%s.%s = %s.%s %s %s.%s;" % (d, view, a, view, sign, b, view)]


@handler(*_names(r"v(add|sub)[us][bhw]s"))
def emit_saturate(op: Op) -> list[str] | None:
    # RPCS3 VADDUBS ... VSUBSWS: clamp each lane and set SAT when any lane clamped.
    d, a, b = _regs(op)
    return ["%s = tox2%s(ctx, %s, %s);" % (d, op.name.capitalize(), a, b)]


@handler("vaddcuw", "vsubcuw")
def emit_carry(op: Op) -> list[str] | None:
    # RPCS3 VADDCUW, VSUBCUW
    d, a, b = _regs(op)
    return ["%s = tox2%s(%s, %s);" % (d, op.name.capitalize(), a, b)]


@handler(*_names(r"vavg[us][bhw]"))
def emit_average(op: Op) -> list[str] | None:
    # RPCS3 VAVGSB ... VAVGUW: (a + b + 1) >> 1 without losing the carry-out bit.
    d, a, b = _regs(op)
    return ["%s = tox2%s(%s, %s);" % (d, op.name.capitalize(), a, b)]


@handler(*_names(r"v(max|min)[us][bhw]"))
def emit_min_max(op: Op) -> list[str] | None:
    # RPCS3 VMAXSB ... VMINUW
    d, a, b = _regs(op)
    match = re.fullmatch(r"v(max|min)([us])([bhw])", op.name)
    view = _view(match.group(3), match.group(2) == "s")
    return ["%s.%s = __builtin_elementwise_%s(%s.%s, %s.%s);" % (d, view, match.group(1), a, view,
                                                                 b, view)]


@handler(*_names(r"vcmp(equ|gtu|gts)[bhw]"))
def emit_compare(op: Op) -> list[str] | None:
    # RPCS3 VCMPEQUB ... VCMPGTUW: all-ones or all-zeros per lane; with Rc, CR6 = {all lanes true,
    # 0, no lane true, 0} (the `has_oe` flag in the handler is the Rc bit).
    d, a, b = _regs(op)
    match = re.fullmatch(r"vcmp(equ|gtu|gts)([bhw])", op.name)
    kind, letter = match.groups()
    view = _view(letter, kind == "gts")
    comparison = "==" if kind == "equ" else ">"
    lines = ["%s.%s = (%s)(%s.%s %s %s.%s);" % (d, _view(letter, False), _VIEWS[letter][2], a, view,
                                                comparison, b, view)]
    if op.rc:
        lines.append("tox2SetCr6(ctx, %s);" % d)
    return lines

"""Group `vmx_mul`: per-element shifts and rotates, widening multiplies, multiply-sum and sum-across.

All of these are helpers in `runtime/include/tox2/ppu_vmx.h`. The ones that can clamp receive the
context so they can set VSCR[SAT]; the modulo forms do not.
"""

from __future__ import annotations

from .emit_core import Op, handler
from .emit_db import GROUPS

_SATURATING = frozenset(["vmhaddshs", "vmhraddshs", "vmsumuhs", "vmsumshs", "vsum4ubs", "vsum4sbs",
                         "vsum4shs", "vsum2sws", "vsumsws"])
_THREE_SOURCE = frozenset(["vmhaddshs", "vmhraddshs", "vmladduhm", "vmsumubm", "vmsummbm", "vmsumuhm",
                           "vmsumuhs", "vmsumshm", "vmsumshs"])


@handler(*GROUPS["vmx_mul"])
def emit_vmx_mul(op: Op) -> list[str] | None:
    # RPCS3 VRLB ... VSUMSWS: every form is `vD = tox2<Name>([ctx,] vA, vB[, vC])`. The element-wise
    # shifts take their counts from the matching element of vB.
    args = [op.vr(op.f("vA")), op.vr(op.f("vB"))]
    if op.name in _THREE_SOURCE:
        args.append(op.vr(op.f("vC")))
    if op.name in _SATURATING:
        args.insert(0, "ctx")
    return ["%s = tox2%s(%s);" % (op.vr(op.f("vD")), op.name.capitalize(), ", ".join(args))]

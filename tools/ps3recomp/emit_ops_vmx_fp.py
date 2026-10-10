"""Group `vmx_fp`: single-precision vector arithmetic, rounding, conversions and compares.

Every operation is a helper in `runtime/include/tox2/ppu_vmx.h`; the helpers that flush denormals
read VSCR[NJ] from the context and the saturating conversions set VSCR[SAT]. Only the compares are
expressed inline, on the `f32` view, like the integer compares.
"""

from __future__ import annotations

from .emit_core import Op, handler
from .emit_db import GROUPS

_TWO_SOURCE = frozenset(["vaddfp", "vsubfp", "vmaxfp", "vminfp"])
_THREE_SOURCE = frozenset(["vmaddfp", "vnmsubfp"])
_ONE_SOURCE_NJ = frozenset(["vrefp", "vrsqrtefp", "vrfin", "vrfiz", "vrfip", "vrfim"])
_ONE_SOURCE = frozenset(["vexptefp", "vlogefp"])
_CONVERT = frozenset(["vcfsx", "vcfux"])
_CONVERT_SAT = frozenset(["vctsxs", "vctuxs"])
_COMPARE = {"vcmpeqfp": "==", "vcmpgefp": ">=", "vcmpgtfp": ">"}


@handler(*GROUPS["vmx_fp"])
def emit_vmx_fp(op: Op) -> list[str] | None:
    # RPCS3 VADDFP ... VCMPBFP; the helper comments say which flags each handler uses.
    name = op.name
    d = op.vr(op.f("vD"))
    if name in _TWO_SOURCE:
        args = ["ctx", op.vr(op.f("vA")), op.vr(op.f("vB"))]
    elif name in _THREE_SOURCE:
        # d = a * c + b: vB is the addend even though the assembly order puts vC before it.
        args = ["ctx", op.vr(op.f("vA")), op.vr(op.f("vB")), op.vr(op.f("vC"))]
    elif name in _ONE_SOURCE_NJ:
        args = ["ctx", op.vr(op.f("vB"))]
    elif name in _ONE_SOURCE:
        args = [op.vr(op.f("vB"))]
    elif name in _CONVERT:
        args = [op.vr(op.f("vB")), str(op.f("UIMM5"))]
    elif name in _CONVERT_SAT:
        args = ["ctx", op.vr(op.f("vB")), str(op.f("UIMM5"))]
    else:
        a, b = op.vr(op.f("vA")), op.vr(op.f("vB"))
        if name == "vcmpbfp":
            lines = ["%s = tox2Vcmpbfp(%s, %s);" % (d, a, b)]
        else:
            lines = ["%s.u32 = (Tox2U32x4)(%s.f32 %s %s.f32);" % (d, a, _COMPARE[name], b)]
        if op.rc:
            lines.append("tox2SetCr6(ctx, %s);" % d)
        return lines
    return ["%s = tox2%s(%s);" % (d, name.capitalize(), ", ".join(args))]

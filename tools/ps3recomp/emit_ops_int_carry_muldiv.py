"""Group `int_carry_muldiv`: carrying add/subtract, multiply and divide.

PS3 code runs in 64-bit mode (RPCS3 `PPUInterpreter.cpp`): CA is the carry out of the 64-bit add,
OE forms set OV (and sticky SO) from 64-bit arithmetic except where the handler says otherwise,
and Rc=1 records the signed 64-bit result.
"""

from __future__ import annotations

from .emit_core import Op, handler, u64


def _binary(op: Op, dest: int, a: str, b: str, call: str, overflow: str | None = None,
            *, record: bool | None = None) -> list[str]:
    """Write `call` (a template over `{a}`/`{b}`) to GPR `dest`, with OV when the op is an OE form.

    With OE the operands are copied to t0/t1 first because rD may alias them; `overflow` is a
    template over `{a}`, `{b}` and the result `{r}`.
    """
    rc = op.rc if record is None else record
    if overflow is None or not op.oe:
        expr = call.format(a=a, b=b)
        if not rc:
            return ["%s = %s;" % (op.gpr(dest), expr)]
        return ["uint64_t t0 = %s;" % expr, "%s = t0;" % op.gpr(dest), op.record("t0")]
    lines = ["uint64_t t0 = %s;" % a, "uint64_t t1 = %s;" % b,
             "uint64_t t2 = %s;" % call.format(a="t0", b="t1"),
             "%s = t2;" % op.gpr(dest),
             "dalSetOv(ctx, %s);" % overflow.format(a="t0", b="t1", r="t2")]
    if rc:
        lines.append(op.record("t2"))
    return lines


# name -> (operand a is ~rA, operand b, carry-in). RPCS3 ADDC ADDE ADDZE ADDME SUBFC SUBFE SUBFZE
# SUBFME: subtraction is ~a + b + carry-in with carry-in 1 (subfc) or CA.
_CA = "ctx->xer_ca"
_CARRYING = {
    "addc": (False, "rB", "0"),
    "adde": (False, "rB", _CA),
    "addze": (False, "0", _CA),
    "addme": (False, "-1", _CA),
    "subfc": (True, "rB", "1"),
    "subfe": (True, "rB", _CA),
    "subfze": (True, "0", _CA),
    "subfme": (True, "-1", _CA),
}


@handler(*_CARRYING)
def emit_carrying(op: Op) -> list[str] | None:
    invert, b_kind, carry_in = _CARRYING[op.name]
    a = ("~%s" if invert else "%s") % op.gpr(op.f("rA"))
    b = op.gpr(op.f("rB")) if b_kind == "rB" else u64(int(b_kind))
    # Signed overflow of a + b + CA is that of a + b for any carry-in (0 or 1).
    return _binary(op, op.f("rD"), a, b, "dalAddCarry(ctx, {a}, {b}, %s)" % carry_in,
                   "dalAddOverflows({a}, {b}, {r})")


@handler("addic", "addic.", "subfic")
def emit_carrying_immediate(op: Op) -> list[str] | None:
    # RPCS3 ADDIC, SUBFIC: CA from the 64-bit add; addic. records CR0.
    imm = u64(op.f("SIMM"))
    ra = op.gpr(op.f("rA"))
    if op.name == "subfic":
        return _binary(op, op.f("rD"), "~" + ra, imm, "dalAddCarry(ctx, {a}, {b}, 1)")
    return _binary(op, op.f("rD"), ra, imm, "dalAddCarry(ctx, {a}, {b}, 0)",
                   record=op.name == "addic." or op.rc)


@handler("mulli")
def emit_mulli(op: Op) -> list[str] | None:
    # RPCS3 MULLI: the low 64 bits of the product, computed unsigned to avoid signed overflow.
    return _binary(op, op.f("rD"), op.gpr(op.f("rA")), u64(op.f("SIMM")), "{a} * {b}")


@handler("mullw")
def emit_mullw(op: Op) -> list[str] | None:
    # RPCS3 MULLW
    return _binary(op, op.f("rD"), op.gpr(op.f("rA")), op.gpr(op.f("rB")), "dalMullw({a}, {b})",
                   "dalMullwOverflows({r})")


@handler("mulld")
def emit_mulld(op: Op) -> list[str] | None:
    # RPCS3 MULLD
    return _binary(op, op.f("rD"), op.gpr(op.f("rA")), op.gpr(op.f("rB")), "{a} * {b}",
                   "dalMulldOverflows({a}, {b}, {r})")


_HIGH_MULTIPLY = {"mulhw": "dalMulhw", "mulhwu": "dalMulhwu", "mulhd": "dalMulhd",
                  "mulhdu": "dalMulhdu"}


@handler(*_HIGH_MULTIPLY)
def emit_high_multiply(op: Op) -> list[str] | None:
    # RPCS3 MULHW, MULHWU, MULHD, MULHDU: no OE form.
    return _binary(op, op.f("rD"), op.gpr(op.f("rA")), op.gpr(op.f("rB")),
                   _HIGH_MULTIPLY[op.name] + "({a}, {b})")


# name -> (helper, overflow template). RPCS3 DIVW DIVWU DIVD DIVDU
_DIVIDE = {
    "divw": ("dalDivw", "dalDivwOverflows({a}, {b})"),
    "divwu": ("dalDivwu", "dalDivwuOverflows({b})"),
    "divd": ("dalDivd", "dalDivdOverflows({a}, {b})"),
    "divdu": ("dalDivdu", "{b} == 0"),
}


@handler(*_DIVIDE)
def emit_divide(op: Op) -> list[str] | None:
    helper, overflow = _DIVIDE[op.name]
    return _binary(op, op.f("rD"), op.gpr(op.f("rA")), op.gpr(op.f("rB")),
                   helper + "({a}, {b})", overflow)

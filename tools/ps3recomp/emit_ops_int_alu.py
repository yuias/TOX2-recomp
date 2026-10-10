"""Group `int_alu`: add/subtract, logical, sign extension, count leading zeros and compares.

PS3 code runs in 64-bit mode: Rc=1 records the signed 64-bit result, OE overflow is 64-bit, and
only `cmp*` with L=0 look at the low words (RPCS3 `PPUInterpreter.cpp`).
"""

from __future__ import annotations

from .emit_core import Op, handler, u32, u64

_NOP_WORD = 0x60000000          # ori r0,r0,0
_INT64_MIN = 1 << 63


def _store(op: Op, dest: int, expr: str, *, record: bool | None = None) -> list[str]:
    """Write `expr` to GPR `dest`; record CR0 from the written value when Rc=1 (or `record`)."""
    if not (op.rc if record is None else record):
        return ["%s = %s;" % (op.gpr(dest), expr)]
    return ["uint64_t t0 = %s;" % expr, "%s = t0;" % op.gpr(dest), op.record("t0")]


@handler("addi", "addis")
def emit_add_immediate(op: Op) -> list[str] | None:
    # RPCS3 ADDI, ADDIS: rA = 0 means the literal 0.
    imm = op.f("SIMM") << (16 if op.name == "addis" else 0)
    ra = op.f("rA")
    expr = u64(imm) if ra == 0 else "%s + %s" % (op.gpr(ra), u64(imm))
    return _store(op, op.f("rD"), expr)


def _overflow_form(op: Op, result: str, overflow: str, *, unary: bool = False) -> list[str]:
    """Shared shape of `add`, `subf` and `neg`.

    `{a}`/`{b}` in `result` name rA/rB; with OE they are copied to t0/t1 first because rD may
    alias them, and `overflow` is written in terms of t0, t1 and the result t2.
    """
    ra, rb = op.gpr(op.f("rA")), "" if unary else op.gpr(op.f("rB"))
    if not op.oe:
        return _store(op, op.f("rD"), result.format(a=ra, b=rb))
    lines = ["uint64_t t0 = %s;" % ra]
    if not unary:
        lines.append("uint64_t t1 = %s;" % rb)
    lines.append("uint64_t t2 = %s;" % result.format(a="t0", b="t1"))
    lines.append("%s = t2;" % op.gpr(op.f("rD")))
    lines.append("dalSetOv(ctx, %s);" % overflow)
    if op.rc:
        lines.append(op.record("t2"))
    return lines


@handler("add")
def emit_add(op: Op) -> list[str] | None:
    # RPCS3 ADD
    return _overflow_form(op, "{a} + {b}", "dalAddOverflows(t0, t1, t2)")


@handler("subf")
def emit_subf(op: Op) -> list[str] | None:
    # RPCS3 SUBF: rB - rA, which overflows like ~rA + rB.
    return _overflow_form(op, "{b} - {a}", "dalAddOverflows(~t0, t1, t2)")


@handler("neg")
def emit_neg(op: Op) -> list[str] | None:
    # RPCS3 NEG: OV only for INT64_MIN.
    return _overflow_form(op, "0 - {a}", "t0 == %s" % u64(_INT64_MIN), unary=True)


_LOGICAL = {
    "and": "{s} & {b}",
    "andc": "{s} & ~{b}",
    "or": "{s} | {b}",
    "orc": "{s} | ~{b}",
    "xor": "{s} ^ {b}",
    "nor": "~({s} | {b})",
    "nand": "~({s} & {b})",
    "eqv": "~({s} ^ {b})",
}


@handler(*_LOGICAL)
def emit_logical(op: Op) -> list[str] | None:
    # RPCS3 AND, ANDC, OR, ORC, XOR, NOR, NAND, EQV
    expr = _LOGICAL[op.name].format(s=op.gpr(op.f("rS")), b=op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), expr)


_LOGICAL_IMMEDIATE = {
    "ori": ("{s} | {i}", 0, False),
    "oris": ("{s} | {i}", 16, False),
    "xori": ("{s} ^ {i}", 0, False),
    "xoris": ("{s} ^ {i}", 16, False),
    "andi.": ("{s} & {i}", 0, True),
    "andis.": ("{s} & {i}", 16, True),
}


@handler(*_LOGICAL_IMMEDIATE)
def emit_logical_immediate(op: Op) -> list[str] | None:
    # RPCS3 ORI, ORIS, XORI, XORIS, ANDI, ANDIS: UIMM is zero-extended; the andi forms always
    # record.
    if op.word == _NOP_WORD:
        return []
    template, shift, always_record = _LOGICAL_IMMEDIATE[op.name]
    expr = template.format(s=op.gpr(op.f("rS")), i=u64(op.f("UIMM") << shift))
    return _store(op, op.f("rA"), expr, record=always_record or op.rc)


_EXTEND = {"extsb": "int8_t", "extsh": "int16_t", "extsw": "int32_t"}


@handler(*_EXTEND)
def emit_extend(op: Op) -> list[str] | None:
    # RPCS3 EXTSB, EXTSH, EXTSW
    expr = "(uint64_t)(int64_t)(%s)%s" % (_EXTEND[op.name], op.gpr(op.f("rS")))
    return _store(op, op.f("rA"), expr)


@handler("cntlzw", "cntlzd")
def emit_cntlz(op: Op) -> list[str] | None:
    # RPCS3 CNTLZW, CNTLZD
    helper = "dalCntlzw" if op.name == "cntlzw" else "dalCntlzd"
    return _store(op, op.f("rA"), "%s(%s)" % (helper, op.gpr(op.f("rS"))))


@handler("cmp", "cmpl", "cmpi", "cmpli")
def emit_compare(op: Op) -> list[str] | None:
    # RPCS3 CMP, CMPL, CMPI, CMPLI: L = 0 compares the low words, L = 1 all 64 bits.
    signed = op.name in ("cmp", "cmpi")
    wide = bool(op.f("L"))
    cast = ("int64_t" if signed else "uint64_t") if wide else ("int32_t" if signed else "uint32_t")
    lhs = "(%s)%s" % (cast, op.gpr(op.f("rA")))
    if op.name == "cmp" or op.name == "cmpl":
        rhs = "(%s)%s" % (cast, op.gpr(op.f("rB")))
    elif signed:
        rhs = str(op.f("SIMM"))
    else:
        rhs = u64(op.f("UIMM")) if wide else u32(op.f("UIMM"))
    return ["%s(ctx, %d, %s, %s);" % ("dalCmpS" if signed else "dalCmpU", op.f("crfD"), lhs, rhs)]

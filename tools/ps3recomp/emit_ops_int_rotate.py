"""Group `int_rotate`: rotate-and-mask, shifts and algebraic shifts.

PS3 code runs in 64-bit mode (RPCS3 `PPUInterpreter.cpp`): the rlw* forms rotate the low word
duplicated into both halves and apply a 64-bit mask, so a wrapping mask (MB > ME) can set high
bits. Rc=1 records the signed 64-bit result.
"""

from __future__ import annotations

from .emit_core import Op, handler, u64

_ALL_ONES = 0xFFFFFFFFFFFFFFFF


def _mask(mb: int, me: int) -> int:
    """64-bit mask of IBM bits mb..me (bit 0 is the MSB); wraps around when mb > me."""
    from_mb = _ALL_ONES >> mb
    to_me = (_ALL_ONES << (63 - me)) & _ALL_ONES
    return from_mb & to_me if mb <= me else from_mb | to_me


def _store(op: Op, dest: int, expr: str) -> list[str]:
    """Write `expr` to GPR `dest`; record CR0 from the written value when Rc=1."""
    if not op.rc:
        return ["%s = %s;" % (op.gpr(dest), expr)]
    return ["uint64_t t0 = %s;" % expr, "%s = t0;" % op.gpr(dest), op.record("t0")]


def _and_mask(expr: str, mask: int) -> str:
    return expr if mask == _ALL_ONES else "%s & %s" % (expr, u64(mask))


def _rotl64(op: Op, rs: int, sh: int) -> str:
    return op.gpr(rs) if sh == 0 else "tox2Rotl64(%s, %d)" % (op.gpr(rs), sh)


def _insert(op: Op, rotated: str, mask: int) -> list[str]:
    ra = op.gpr(op.f("rA"))
    return _store(op, op.f("rA"), "(%s & ~%s) | (%s & %s)" % (ra, u64(mask), rotated, u64(mask)))


@handler("rlwinm")
def emit_rlwinm(op: Op) -> list[str] | None:
    # RPCS3 RLWINM
    mask = _mask(op.f("MB") + 32, op.f("ME") + 32)
    rotated = "tox2Rotl32Dup(%s, %d)" % (op.gpr(op.f("rS")), op.f("SH"))
    return _store(op, op.f("rA"), _and_mask(rotated, mask))


@handler("rlwnm")
def emit_rlwnm(op: Op) -> list[str] | None:
    # RPCS3 RLWNM: the count is the low 5 bits of rB.
    mask = _mask(op.f("MB") + 32, op.f("ME") + 32)
    rotated = "tox2Rotl32Dup(%s, (uint32_t)%s)" % (op.gpr(op.f("rS")), op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), _and_mask(rotated, mask))


@handler("rlwimi")
def emit_rlwimi(op: Op) -> list[str] | None:
    # RPCS3 RLWIMI
    mask = _mask(op.f("MB") + 32, op.f("ME") + 32)
    return _insert(op, "tox2Rotl32Dup(%s, %d)" % (op.gpr(op.f("rS")), op.f("SH")), mask)


@handler("rldicl")
def emit_rldicl(op: Op) -> list[str] | None:
    # RPCS3 RLDICL: MASK(mb, 63)
    rotated = _rotl64(op, op.f("rS"), op.f("SH64"))
    return _store(op, op.f("rA"), _and_mask(rotated, _mask(op.f("MB64"), 63)))


@handler("rldicr")
def emit_rldicr(op: Op) -> list[str] | None:
    # RPCS3 RLDICR: the MB64 field holds me; MASK(0, me)
    rotated = _rotl64(op, op.f("rS"), op.f("SH64"))
    return _store(op, op.f("rA"), _and_mask(rotated, _mask(0, op.f("MB64"))))


@handler("rldic")
def emit_rldic(op: Op) -> list[str] | None:
    # RPCS3 RLDIC: MASK(mb, 63 - sh)
    sh = op.f("SH64")
    rotated = _rotl64(op, op.f("rS"), sh)
    return _store(op, op.f("rA"), _and_mask(rotated, _mask(op.f("MB64"), 63 - sh)))


@handler("rldimi")
def emit_rldimi(op: Op) -> list[str] | None:
    # RPCS3 RLDIMI: insert under MASK(mb, 63 - sh)
    sh = op.f("SH64")
    return _insert(op, _rotl64(op, op.f("rS"), sh), _mask(op.f("MB64"), 63 - sh))


@handler("rldcl")
def emit_rldcl(op: Op) -> list[str] | None:
    # RPCS3 RLDCL: the count is the low 6 bits of rB.
    rotated = "tox2Rotl64(%s, (uint32_t)%s)" % (op.gpr(op.f("rS")), op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), _and_mask(rotated, _mask(op.f("MB64"), 63)))


@handler("rldcr")
def emit_rldcr(op: Op) -> list[str] | None:
    # RPCS3 RLDCR: the MB64 field holds me.
    rotated = "tox2Rotl64(%s, (uint32_t)%s)" % (op.gpr(op.f("rS")), op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), _and_mask(rotated, _mask(0, op.f("MB64"))))


_SHIFT_HELPER = {"slw": "tox2Slw", "srw": "tox2Srw", "sld": "tox2Sld", "srd": "tox2Srd"}


@handler(*_SHIFT_HELPER)
def emit_shift(op: Op) -> list[str] | None:
    # RPCS3 SLW, SRW, SLD, SRD
    expr = "%s(%s, %s)" % (_SHIFT_HELPER[op.name], op.gpr(op.f("rS")), op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), expr)


@handler("sraw", "srad")
def emit_algebraic_shift(op: Op) -> list[str] | None:
    # RPCS3 SRAW, SRAD: both write XER.CA.
    helper = "tox2Sraw" if op.name == "sraw" else "tox2Srad"
    expr = "%s(ctx, %s, %s)" % (helper, op.gpr(op.f("rS")), op.gpr(op.f("rB")))
    return _store(op, op.f("rA"), expr)


@handler("srawi", "sradi")
def emit_algebraic_shift_immediate(op: Op) -> list[str] | None:
    # RPCS3 SRAWI, SRADI
    if op.name == "srawi":
        helper, amount = "tox2Sraw", op.f("SH")
    else:
        helper, amount = "tox2Srad", op.f("SH64")
    return _store(op, op.f("rA"), "%s(ctx, %s, %d)" % (helper, op.gpr(op.f("rS")), amount))

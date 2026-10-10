"""Group `fp`: scalar floating-point arithmetic, conversions, compares and FPSCR moves.

FPRs hold doubles. Single-precision forms compute in double and round the result to float
(RPCS3 `f32`). The multiply-add family uses `__builtin_fma` (RPCS3 `FMADD` etc. with accurate
DFMA); sign-bit operations work on the bit pattern. FPSCR is only partly modelled, see
`runtime/include/tox2/ppu_fp.h`. With Rc=1 every instruction ends by copying FPSCR[0:3] to CR1.
"""

from __future__ import annotations

from .emit_core import Op, handler

_SIGN = "0x8000000000000000ull"
_NOT_SIGN = "0x7fffffffffffffffull"


def _finish(op: Op, lines: list[str]) -> list[str]:
    if op.rc:
        lines.append("tox2RecordFp(ctx);")
    return lines


def _set_double(op: Op, expr: str, *, single: bool = False) -> list[str]:
    """frD = `expr` (a double expression), rounded to float first for the single-precision forms."""
    if single:
        expr = "(double)(float)(%s)" % expr
    return _finish(op, ["%s.f64 = %s;" % (op.fpr(op.f("frD")), expr)])


def _set_bits(op: Op, expr: str) -> list[str]:
    return _finish(op, ["%s.u64 = %s;" % (op.fpr(op.f("frD")), expr)])


def _f64(op: Op, name: str) -> str:
    return "%s.f64" % op.fpr(op.f(name))


def _u64(op: Op, name: str) -> str:
    return "%s.u64" % op.fpr(op.f(name))


_BINARY = {"fadd": "+", "fsub": "-", "fdiv": "/"}


@handler(*_BINARY, *(name + "s" for name in _BINARY))
def emit_binary(op: Op) -> list[str] | None:
    # RPCS3 FADD, FSUB, FDIV and the FADDS, FSUBS, FDIVS forms
    single = op.name.endswith("s")
    symbol = _BINARY[op.name[:-1] if single else op.name]
    return _set_double(op, "%s %s %s" % (_f64(op, "frA"), symbol, _f64(op, "frB")), single=single)


@handler("fmul", "fmuls")
def emit_multiply(op: Op) -> list[str] | None:
    # RPCS3 FMUL, FMULS: the multiplier is frC.
    return _set_double(op, "%s * %s" % (_f64(op, "frA"), _f64(op, "frC")),
                       single=op.name == "fmuls")


@handler("fsqrt", "fsqrts")
def emit_sqrt(op: Op) -> list[str] | None:
    # RPCS3 FSQRT, FSQRTS
    return _set_double(op, "__builtin_sqrt(%s)" % _f64(op, "frB"), single=op.name == "fsqrts")


@handler("fres")
def emit_fres(op: Op) -> list[str] | None:
    # RPCS3 FRES: an estimate on hardware; the exact quotient is within its tolerance.
    return _set_double(op, "1.0 / %s" % _f64(op, "frB"), single=True)


@handler("frsqrte")
def emit_frsqrte(op: Op) -> list[str] | None:
    # RPCS3 FRSQRTE: estimate; the exact value is within tolerance.
    return _set_double(op, "1.0 / __builtin_sqrt(%s)" % _f64(op, "frB"))


# name -> (negate the result, negate the addend)
_FUSED = {"fmadd": (False, False), "fmsub": (False, True),
          "fnmadd": (True, False), "fnmsub": (True, True)}


@handler(*_FUSED, *(name + "s" for name in _FUSED))
def emit_fused(op: Op) -> list[str] | None:
    # RPCS3 FMADD, FMSUB, FNMADD, FNMSUB and the S forms: a * c + b with one rounding. The result
    # sign flips also for a NaN result, as in RPCS3.
    single = op.name.endswith("s")
    negate_result, negate_addend = _FUSED[op.name[:-1] if single else op.name]
    expr = "__builtin_fma(%s, %s, %s%s)" % (_f64(op, "frA"), _f64(op, "frC"),
                                            "-" if negate_addend else "", _f64(op, "frB"))
    if negate_result:
        expr = "-" + expr
    return _set_double(op, expr, single=single)


@handler("fsel")
def emit_fsel(op: Op) -> list[str] | None:
    # RPCS3 FSEL: frA >= 0 (either zero) picks frC; a NaN compares false and picks frB.
    return _set_double(op, "%s >= 0.0 ? %s : %s" % (_f64(op, "frA"), _f64(op, "frC"),
                                                    _f64(op, "frB")))


@handler("frsp")
def emit_frsp(op: Op) -> list[str] | None:
    # RPCS3 FRSP: out-of-range values become infinity (IEEE conversion).
    return _set_double(op, _f64(op, "frB"), single=True)


@handler("fctiw", "fctiwz", "fctid", "fctidz")
def emit_convert_to_integer(op: Op) -> list[str] | None:
    # RPCS3 FCTIW, FCTIWZ, FCTID, FCTIDZ: saturating; the 32-bit forms store the sign-extended word.
    helper = {"fctiw": "tox2Fctiw", "fctiwz": "tox2Fctiwz", "fctid": "tox2Fctid",
              "fctidz": "tox2Fctidz"}[op.name]
    return _set_bits(op, "%s(%s)" % (helper, _f64(op, "frB")))


@handler("fcfid")
def emit_fcfid(op: Op) -> list[str] | None:
    # RPCS3 FCFID: the register bits are a signed 64-bit integer.
    return _set_double(op, "(double)(int64_t)%s" % _u64(op, "frB"))


@handler("fneg", "fabs", "fnabs", "fmr")
def emit_sign_ops(op: Op) -> list[str] | None:
    # RPCS3 FNEG, FABS, FNABS, FMR: bit operations, so NaN payloads are kept.
    source = _u64(op, "frB")
    expr = {"fneg": "%s ^ %s" % (source, _SIGN), "fabs": "%s & %s" % (source, _NOT_SIGN),
            "fnabs": "%s | %s" % (source, _SIGN), "fmr": source}[op.name]
    return _set_bits(op, expr)


@handler("fcmpu", "fcmpo")
def emit_compare(op: Op) -> list[str] | None:
    # RPCS3 FCMPU, FCMPO (which forwards to FCMPU)
    return ["tox2Fcmp(ctx, %d, %s, %s);" % (op.f("crfD"), _f64(op, "frA"), _f64(op, "frB"))]


@handler("mffs")
def emit_mffs(op: Op) -> list[str] | None:
    # RPCS3 MFFS: the FPSCR image goes to the low 32 bits of frD.
    return _set_bits(op, "tox2Mffs(ctx)")


@handler("mtfsf")
def emit_mtfsf(op: Op) -> list[str] | None:
    # Architecture: the low word of frB goes to the FPSCR fields selected by FM. RPCS3 `MTFSF` is
    # an empty stub that only records CR1.
    return _finish(op, ["tox2Mtfsf(ctx, 0x%02xu, %s);" % (op.f("FM"), _u64(op, "frB"))])


@handler("mtfsfi")
def emit_mtfsfi(op: Op) -> list[str] | None:
    # RPCS3 MTFSFI (which only handles the FPCC field; the other fields are written here too).
    field = op.f("crfD")
    return _finish(op, ["tox2Mtfsf(ctx, 0x%02xu, (uint64_t)%du << %d);"
                        % (0x80 >> field, op.f("IMM_fp"), 28 - 4 * field)])


@handler("mtfsb0", "mtfsb1")
def emit_mtfsb(op: Op) -> list[str] | None:
    # RPCS3 MTFSB0, MTFSB1
    return _finish(op, ["tox2Mtfsb(ctx, %d, %d);" % (op.f("crbD"), op.name == "mtfsb1")])


@handler("mcrfs")
def emit_mcrfs(op: Op) -> list[str] | None:
    # RPCS3 MCRFS: copies the FPSCR field; the architectural clearing of the copied exception
    # bits is not modelled.
    return ["tox2SetCrField(ctx, %d, (uint32_t)(tox2Mffs(ctx) >> %d) & 15u);"
            % (op.f("crfD"), 28 - 4 * op.f("crfS"))]

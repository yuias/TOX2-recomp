"""Group `fp_single`: single-precision arithmetic (`fa`, `fs`, `fm`, `fma`, `fms`, `fnms`), compares
(`fceq`, `fcmeq`, `fcgt`, `fcmgt`) and estimates (`frest`, `frsqest`, `fi`).

The lane semantics live in `runtime/include/tox2/spu_fp.h`; a handler only picks the helper. SPU ISA 1.2,
9.1 (extended-range single precision, p.195-196) and the instruction pages for `fa` .. `fnms`:

- A word with exponent field 0 is zero whatever its sign and fraction (denormals are zero); an exponent
  field of 255 is an ordinary exponent, there are no Inf or NaN.
- A result is zero when it is zero or below the smallest normal number (always +0), saturates to
  `sign | 0x7fffffff` above the largest, and is otherwise truncated toward zero to 24 bits.
- `fma`, `fms` and `fnms` compute RA * RB + RC, RA * RB - RC and RC - RA * RB with an exact product; only
  the final value is rounded.
- The compares (p.231-234) always run in extended-range mode: a word with exponent field 0 counts as +0,
  two zeros are equal and never greater. `fceq`/`fcgt` order by sign and magnitude, `fcmeq`/`fcmgt` by
  magnitude alone; true is all ones in the word.
- The estimates (p.215-219) are a recorded deviation, because the ISA does not publish the hardware's base
  and step tables: `frest` and `frsqest` give the exact reciprocal (square root) truncated to 24 bits,
  `fi` returns RB. `frest` of a zero exponent is `sign | 0x7fffffff`, of `|x| >= 2^126` is +0;
  `frsqest` of a zero exponent is `0x7fffffff` and its sign is always 0.

The status register is not touched: the single-precision flags are never set (recorded deviation).
`fscrrd` and `fscrwr` belong to the `chan` module.
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler

_RR = {"fa": "tox2SpuFa", "fs": "tox2SpuFs", "fm": "tox2SpuFm", "fceq": "tox2SpuFceq", "fcmeq": "tox2SpuFcmeq",
       "fcgt": "tox2SpuFcgt", "fcmgt": "tox2SpuFcmgt", "fi": "tox2SpuFi"}

# One source operand.
_R = {"frest": "tox2SpuFrest", "frsqest": "tox2SpuFrsqest"}

# The RRR forms name their fields RT4, RA, RB, RC.
_RRR = {"fma": "tox2SpuFma", "fms": "tox2SpuFms", "fnms": "tox2SpuFnms"}


def _make_rr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s, %s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")))]
    handler(name)(emit)


def _make_r(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")))]
    handler(name)(emit)


def _make_rrr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s, %s, %s);" % (op.gpr(op.f("RT4")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")),
                                          op.gpr(op.f("RC")))]
    handler(name)(emit)


for _name, _fn in _RR.items():
    _make_rr(_name, _fn)
for _name, _fn in _R.items():
    _make_r(_name, _fn)
for _name, _fn in _RRR.items():
    _make_rrr(_name, _fn)

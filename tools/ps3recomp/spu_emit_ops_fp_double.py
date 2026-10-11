"""Group `fp_double`: double-precision arithmetic (`dfa`, `dfs`, `dfm`, `dfma`, `dfms`, `dfnms`, `dfnma`),
the conversions `fesd` and `frds`, and the ISA 1.2 optional compares (`dfceq`, `dfcmeq`, `dfcgt`, `dfcmgt`) and
`dftsv`.

The lane semantics live in `runtime/include/tox2/spu_fp.h`; a handler only picks the helper. SPU ISA 1.2, 9.2
(p.197-199) and the instruction pages (dfa p.203, dfs p.205, dfm p.207, dfma p.209, dfnms p.211, dfms p.213,
dfnma p.214, frds p.224, fesd p.225, dfceq p.226, dfcmeq p.227, dfcgt p.228, dfcmgt p.229, dftsv p.230):

- Each doubleword of the register is an independent IEEE double operation in round to nearest. `dfma`,
  `dfms`, `dfnms` and `dfnma` use RA * RB and the old value of RT with one rounding; `dfnms` and `dfnma` negate
  the rounded result unless it is a NaN.
- Recorded choices: a denormal operand is forced to zero of the same sign (the optional behaviour of
  p.198-199), denormal results are kept, and every NaN result is the default QNaN.
- `fesd` and `frds` convert between IEEE singles (word 0 and word 2) and doubles; `frds` rounds to nearest and
  puts zeros into words 1 and 3.
- The compares treat a NaN as unordered (false) and zeros as equal; `dftsv` tests the raw operand.

Only round to nearest is implemented. FPSCR RN0 (slice 0) and RN1 (slice 1) are the bits `0x00000c00` and
`0x00000300` of word 0 of the status register; the operations that round (`dfa dfs dfm dfma dfms dfnms dfnma
frds`) emit a check that stops the run with `tox2SpuUnhandled` at that instruction when either field is not 0,
instead of silently computing another value. `fesd`, the compares and `dftsv` are exact and need no check. The
status flags are never set (recorded deviation). `fscrrd` and `fscrwr` belong to the `chan` module.
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler, ls_lit, u32

# Word 0 of FPSCR holds both rounding-mode fields (ISA bits 20-23).
_ROUNDING_MODE_BITS = 0x00000F00

_RR = {"dfa": "tox2SpuDfa", "dfs": "tox2SpuDfs", "dfm": "tox2SpuDfm"}

# RT is also the accumulator.
_RR_ACC = {"dfma": "tox2SpuDfma", "dfms": "tox2SpuDfms", "dfnms": "tox2SpuDfnms", "dfnma": "tox2SpuDfnma"}

_COMPARE = {"dfceq": "tox2SpuDfceq", "dfcmeq": "tox2SpuDfcmeq", "dfcgt": "tox2SpuDfcgt", "dfcmgt": "tox2SpuDfcmgt"}


def _guard(op: SpuOp) -> str:
    return ("if ((TOX2_VR_W(ctx->fpscr, 0) & 0x%08xu) != 0u) tox2SpuUnhandled(ctx, ls, %s, %s);"
            % (_ROUNDING_MODE_BITS, ls_lit(op.addr), u32(op.word)))


def _make_rr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return [_guard(op),
                "%s = %s(%s, %s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")))]
    handler(name)(emit)


def _make_rr_acc(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        rt = op.gpr(op.f("RT"))
        return [_guard(op), "%s = %s(%s, %s, %s);" % (rt, fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")), rt)]
    handler(name)(emit)


def _make_compare(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return ["%s = %s(%s, %s);" % (op.gpr(op.f("RT")), fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")))]
    handler(name)(emit)


for _name, _fn in _RR.items():
    _make_rr(_name, _fn)
for _name, _fn in _RR_ACC.items():
    _make_rr_acc(_name, _fn)
for _name, _fn in _COMPARE.items():
    _make_compare(_name, _fn)


@handler("fesd")
def _fesd(op: SpuOp) -> list[str]:
    return ["%s = tox2SpuFesd(%s);" % (op.gpr(op.f("RT")), op.gpr(op.f("RA")))]


@handler("frds")
def _frds(op: SpuOp) -> list[str]:
    return [_guard(op), "%s = tox2SpuFrds(%s);" % (op.gpr(op.f("RT")), op.gpr(op.f("RA")))]


@handler("dftsv")
def _dftsv(op: SpuOp) -> list[str]:
    # The decoder names the 7-bit immediate U7; the ISA calls it I7.
    return ["%s = tox2SpuDftsv(%s, %du);" % (op.gpr(op.f("RT")), op.gpr(op.f("RA")), op.f("U7"))]

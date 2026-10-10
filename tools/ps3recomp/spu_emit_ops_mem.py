"""Group `mem`: quadword loads and stores of the local store.

`tox2SpuLoadQ` and `tox2SpuStoreQ` wrap the address at the local-store size and ignore its low four
bits (LSLR & 0xFFFFFFF0, SPU ISA 1.2 chapter "Memory-Load/Store"). The d-form and x-form addresses are
computed with 32-bit wrap-around from the preferred words; the a-form and r-form addresses are
constants, so the masking happens here.
"""

from __future__ import annotations

from .spu import LS_TARGET_MASK
from .spu_emit_core import SpuOp, handler, u32

# LSLR is 0x3ffff for the 256 KiB local store; the quadword address also drops its low four bits.
_QUADWORD_MASK = LS_TARGET_MASK & ~0xF


def _load(op: SpuOp, addr: str) -> list[str]:
    return ["%s = tox2SpuLoadQ(ls, %s);" % (op.gpr(op.f("RT")), addr)]


def _store(op: SpuOp, addr: str) -> list[str]:
    return ["tox2SpuStoreQ(ls, %s, %s);" % (addr, op.gpr(op.f("RT")))]


def _d_form(op: SpuOp) -> str:
    return "%s + %s" % (op.pref(op.f("RA")), u32(op.f("I10x16")))


def _x_form(op: SpuOp) -> str:
    return "%s + %s" % (op.pref(op.f("RA")), op.pref(op.f("RB")))


def _a_form(op: SpuOp) -> str:
    # The field holds the signed byte address; its low 18 bits are the LS address.
    return "0x%05xu" % (op.f("ABS16") & _QUADWORD_MASK)


def _r_form(op: SpuOp) -> str:
    # The offset is relative to the address of the instruction itself.
    return "0x%05xu" % ((op.addr + op.f("REL16")) & _QUADWORD_MASK)


@handler("lqd")
def emit_lqd(op: SpuOp) -> list[str]:
    return _load(op, _d_form(op))


@handler("lqx")
def emit_lqx(op: SpuOp) -> list[str]:
    return _load(op, _x_form(op))


@handler("lqa")
def emit_lqa(op: SpuOp) -> list[str]:
    return _load(op, _a_form(op))


@handler("lqr")
def emit_lqr(op: SpuOp) -> list[str]:
    return _load(op, _r_form(op))


@handler("stqd")
def emit_stqd(op: SpuOp) -> list[str]:
    return _store(op, _d_form(op))


@handler("stqx")
def emit_stqx(op: SpuOp) -> list[str]:
    return _store(op, _x_form(op))


@handler("stqa")
def emit_stqa(op: SpuOp) -> list[str]:
    return _store(op, _a_form(op))


@handler("stqr")
def emit_stqr(op: SpuOp) -> list[str]:
    return _store(op, _r_form(op))

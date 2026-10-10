"""Group `chan`: channel access, `stopd`, the special-purpose register moves and the FPSCR moves.

Every channel access goes through a runtime hook, so which channels exist, what blocks and what the
channel value means (RdMachStat, WrEventMask, ...) is decided there, not here. A hook that returns
false means the thread must leave; the handler then emits the lowering's halt statement, so nothing
after the instruction runs. SPU ISA 1.2, chapter "Channel Instructions" (rdch, rchcnt, wrch), chapter
"Control Instructions" (stopd, mfspr, mtspr) and "Floating-Point Status and Control Register"
(fscrrd, fscrwr).

The channel hooks work on 32-bit values: the value of a read goes to the preferred word of RT and the
other words become 0 (ISA note on 32-bit channels), and a write sends the preferred word of RT.
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler, ls_lit


def _channel(op: SpuOp) -> int:
    return op.f("CA")


@handler("rdch")
def emit_rdch(op: SpuOp) -> list[str]:
    return [
        "uint32_t t0;",
        "if (!tox2SpuReadChannel(ctx, ls, %s, %du, &t0)) %s" % (ls_lit(op.addr), _channel(op), op.halt()),
        "%s = tox2VrFromBeWords(t0, 0u, 0u, 0u);" % op.gpr(op.f("RT")),
    ]


@handler("rchcnt")
def emit_rchcnt(op: SpuOp) -> list[str]:
    return ["%s = tox2VrFromBeWords(tox2SpuChannelCount(ctx, ls, %du), 0u, 0u, 0u);"
            % (op.gpr(op.f("RT")), _channel(op))]


@handler("wrch")
def emit_wrch(op: SpuOp) -> list[str]:
    return ["if (!tox2SpuWriteChannel(ctx, ls, %s, %du, %s)) %s"
            % (ls_lit(op.addr), _channel(op), op.pref(op.f("RT")), op.halt())]


# stopd ends the thread like a stop that has no code to report. The gate images only use it as an
# assertion that must not be reached, so it is a fatal stop at its site.
@handler("stopd")
def emit_stopd(op: SpuOp) -> list[str]:
    return ["tox2SpuHalt(ctx, ls, %s);" % ls_lit(op.addr)]


# No SPR is implemented (ISA: an undefined SPR reads as zero and a write does nothing).
@handler("mfspr")
def emit_mfspr(op: SpuOp) -> list[str]:
    return ["%s = tox2VrFromBeWords(0u, 0u, 0u, 0u);" % op.gpr(op.f("RT"))]


@handler("mtspr")
def emit_mtspr(op: SpuOp) -> list[str]:
    return []


@handler("fscrrd")
def emit_fscrrd(op: SpuOp) -> list[str]:
    return ["%s = tox2SpuFscrRead(ctx->fpscr);" % op.gpr(op.f("RT"))]


# RT of fscrwr is a false target and is not written.
@handler("fscrwr")
def emit_fscrwr(op: SpuOp) -> list[str]:
    return ["ctx->fpscr = %s;" % op.gpr(op.f("RA"))]

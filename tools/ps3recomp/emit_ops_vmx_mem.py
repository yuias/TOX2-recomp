"""Group `vmx_mem`: vector loads and stores, bitwise logic, vsel and the VSCR moves.

Vector registers are byte-reversed in the host (see `runtime/include/tox2/ppu_context.h`); the
partial loads and stores live in `runtime/include/tox2/ppu_vmx.h`. The effective address is the
32-bit `(rA|0) + rB`; `lvx`/`stvx` and the element loads ignore its low four bits.
"""

from __future__ import annotations

from .emit_core import Op, handler


@handler("lvx", "lvxl", "lvebx", "lvehx", "lvewx")
def emit_load_block(op: Op) -> list[str] | None:
    # RPCS3 LVX, LVXL, LVEBX, LVEHX, LVEWX: all five load the whole aligned block.
    return ["%s = tox2ReadVr(base, %s);" % (op.vr(op.f("vD")), op.ea_x())]


@handler("stvx", "stvxl")
def emit_store_block(op: Op) -> list[str] | None:
    # RPCS3 STVX, STVXL
    return ["tox2WriteVr(base, %s, %s);" % (op.ea_x(), op.vr(op.f("vS")))]


@handler("lvlx", "lvlxl")
def emit_lvlx(op: Op) -> list[str] | None:
    # RPCS3 LVLX, LVLXL
    return ["%s = tox2Lvlx(base, %s);" % (op.vr(op.f("vD")), op.ea_x())]


@handler("lvrx", "lvrxl")
def emit_lvrx(op: Op) -> list[str] | None:
    # RPCS3 LVRX, LVRXL
    return ["%s = tox2Lvrx(base, %s);" % (op.vr(op.f("vD")), op.ea_x())]


_PARTIAL_STORES = {
    "stvlx": "tox2Stvlx", "stvlxl": "tox2Stvlx", "stvrx": "tox2Stvrx", "stvrxl": "tox2Stvrx",
    "stvebx": "tox2Stvebx", "stvehx": "tox2Stvehx", "stvewx": "tox2Stvewx",
}


@handler(*_PARTIAL_STORES)
def emit_partial_store(op: Op) -> list[str] | None:
    # RPCS3 STVLX, STVLXL, STVRX, STVRXL, STVEBX, STVEHX, STVEWX
    return ["%s(base, %s, %s);" % (_PARTIAL_STORES[op.name], op.ea_x(), op.vr(op.f("vS")))]


@handler("lvsl", "lvsr")
def emit_permute_control(op: Op) -> list[str] | None:
    # RPCS3 LVSL, LVSR
    helper = "tox2Lvsl" if op.name == "lvsl" else "tox2Lvsr"
    return ["%s = %s(%s);" % (op.vr(op.f("vD")), helper, op.ea_x())]


_LOGIC = {
    "vand": "%(a)s & %(b)s",
    "vandc": "%(a)s & ~%(b)s",
    "vor": "%(a)s | %(b)s",
    "vxor": "%(a)s ^ %(b)s",
    "vnor": "~(%(a)s | %(b)s)",
}


@handler(*_LOGIC)
def emit_logic(op: Op) -> list[str] | None:
    # RPCS3 VAND, VANDC, VOR, VXOR, VNOR
    operands = {"a": "%s.u32" % op.vr(op.f("vA")), "b": "%s.u32" % op.vr(op.f("vB"))}
    return ["%s.u32 = %s;" % (op.vr(op.f("vD")), _LOGIC[op.name] % operands)]


@handler("vsel")
def emit_vsel(op: Op) -> list[str] | None:
    # RPCS3 VSEL: bits of vB where vC is set, bits of vA elsewhere.
    return ["%s = tox2Vsel(%s, %s, %s);" % (op.vr(op.f("vD")), op.vr(op.f("vA")),
                                          op.vr(op.f("vB")), op.vr(op.f("vC")))]


@handler("mfvscr")
def emit_mfvscr(op: Op) -> list[str] | None:
    # RPCS3 MFVSCR
    return ["%s = tox2Mfvscr(ctx);" % op.vr(op.f("vD"))]


@handler("mtvscr")
def emit_mtvscr(op: Op) -> list[str] | None:
    # RPCS3 MTVSCR
    return ["tox2Mtvscr(ctx, %s);" % op.vr(op.f("vB"))]

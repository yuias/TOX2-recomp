"""Group `vmx_perm`: permutes, merges, splats, packs, unpacks and whole-register shifts.

Vector registers are byte-reversed in the host (see `runtime/include/dal/ppu_context.h`): host lane
i of an n-lane view holds big-endian element n-1-i. Merges, `vsldoi`, splats and the truncating
packs are emitted as `__builtin_shufflevector` calls; `be_shuffle` converts a result described in
big-endian element numbers into the host lane indices. The rest lives in
`runtime/include/dal/ppu_vmx.h`.
"""

from __future__ import annotations

from .emit_core import Op, handler

# View name, lane count, for each element size in bytes.
_VIEWS = {1: ("u8", 16), 2: ("u16", 8), 4: ("u32", 4)}


def be_shuffle(lanes: int, be_sources: list[int]) -> list[int]:
    """Host shuffle indices for `__builtin_shufflevector(a, b, ...)`.

    `be_sources[j]` says where big-endian result element j comes from: 0 .. lanes-1 is big-endian
    element k of `a`, lanes .. 2*lanes-1 is element k - lanes of `b`. Both operands and the result
    are in the host (byte-reversed) lane order.
    """
    host = []
    for k in be_sources:
        host.append(lanes - 1 - k if k < lanes else 3 * lanes - 1 - k)
    return host[::-1]


def _shuffle(op: Op, size: int, a: int, b: int, be_sources: list[int]) -> str:
    view, lanes = _VIEWS[size]
    indices = ", ".join(str(i) for i in be_shuffle(lanes, be_sources))
    return "%s.%s = __builtin_shufflevector(%s.%s, %s.%s, %s);" % (
        op.vr(op.f("vD")), view, op.vr(a), view, op.vr(b), view, indices)


@handler("vperm")
def emit_vperm(op: Op) -> list[str] | None:
    # RPCS3 VPERM
    return ["%s = dalVperm(%s, %s, %s);" % (op.vr(op.f("vD")), op.vr(op.f("vA")),
                                          op.vr(op.f("vB")), op.vr(op.f("vC")))]


@handler("vsldoi")
def emit_vsldoi(op: Op) -> list[str] | None:
    # RPCS3 VSLDOI: big-endian bytes SHB .. SHB+15 of concat(vA, vB).
    sh = op.f("SHB")
    return [_shuffle(op, 1, op.f("vA"), op.f("vB"), [sh + j for j in range(16)])]


_MERGES = {
    "vmrghb": (1, 0), "vmrghh": (2, 0), "vmrghw": (4, 0),
    "vmrglb": (1, 1), "vmrglh": (2, 1), "vmrglw": (4, 1),
}


@handler(*_MERGES)
def emit_merge(op: Op) -> list[str] | None:
    # RPCS3 VMRGHB, VMRGHH, VMRGHW, VMRGLB, VMRGLH, VMRGLW: a0 b0 a1 b1 ... from the first
    # (high) or second (low) half of the big-endian elements.
    size, low = _MERGES[op.name]
    lanes = _VIEWS[size][1]
    start = low * (lanes // 2)
    sources = []
    for m in range(lanes // 2):
        sources += [start + m, lanes + start + m]
    return [_shuffle(op, size, op.f("vA"), op.f("vB"), sources)]


_SPLATS = {"vspltb": 1, "vsplth": 2, "vspltw": 4}


@handler(*_SPLATS)
def emit_splat(op: Op) -> list[str] | None:
    # RPCS3 VSPLTB, VSPLTH, VSPLTW: big-endian element UIMM5 modulo the lane count.
    size = _SPLATS[op.name]
    lanes = _VIEWS[size][1]
    element = op.f("UIMM5") % lanes
    return [_shuffle(op, size, op.f("vB"), op.f("vB"), [element] * lanes)]


_SPLAT_IMMEDIATES = {"vspltisb": "dalVspltisb", "vspltish": "dalVspltish", "vspltisw": "dalVspltisw"}


@handler(*_SPLAT_IMMEDIATES)
def emit_splat_immediate(op: Op) -> list[str] | None:
    # RPCS3 VSPLTISB, VSPLTISH, VSPLTISW: SIMM5 sign-extended.
    return ["%s = %s(%d);" % (op.vr(op.f("vD")), _SPLAT_IMMEDIATES[op.name], op.f("SIMM5"))]


@handler("vpkuhum", "vpkuwum")
def emit_pack_modulo(op: Op) -> list[str] | None:
    # RPCS3 VPKUHUM, VPKUWUM: low half of each element of concat(vA, vB).
    size = 1 if op.name == "vpkuhum" else 2
    lanes = _VIEWS[size][1]
    return [_shuffle(op, size, op.f("vA"), op.f("vB"), [2 * j + 1 for j in range(lanes)])]


_SATURATING_PACKS = {
    "vpkshss": "dalVpkshss", "vpkshus": "dalVpkshus", "vpkuhus": "dalVpkuhus",
    "vpkswss": "dalVpkswss", "vpkswus": "dalVpkswus", "vpkuwus": "dalVpkuwus",
}


@handler(*_SATURATING_PACKS)
def emit_pack_saturate(op: Op) -> list[str] | None:
    # RPCS3 VPKSHSS, VPKSHUS, VPKUHUS, VPKSWSS, VPKSWUS, VPKUWUS
    return ["%s = %s(ctx, %s, %s);" % (op.vr(op.f("vD")), _SATURATING_PACKS[op.name],
                                       op.vr(op.f("vA")), op.vr(op.f("vB")))]


@handler("vpkpx")
def emit_vpkpx(op: Op) -> list[str] | None:
    # RPCS3 VPKPX
    return ["%s = dalVpkpx(%s, %s);" % (op.vr(op.f("vD")), op.vr(op.f("vA")),
                                        op.vr(op.f("vB")))]


_UNPACKS = {
    "vupkhsb": "dalVupkhsb", "vupklsb": "dalVupklsb", "vupkhsh": "dalVupkhsh",
    "vupklsh": "dalVupklsh", "vupkhpx": "dalVupkhpx", "vupklpx": "dalVupklpx",
}


@handler(*_UNPACKS)
def emit_unpack(op: Op) -> list[str] | None:
    # RPCS3 VUPKHSB, VUPKLSB, VUPKHSH, VUPKLSH, VUPKHPX, VUPKLPX
    return ["%s = %s(%s);" % (op.vr(op.f("vD")), _UNPACKS[op.name], op.vr(op.f("vB")))]


_SHIFTS = {"vsl": "dalVsl", "vsr": "dalVsr", "vslo": "dalVslo", "vsro": "dalVsro"}


@handler(*_SHIFTS)
def emit_shift(op: Op) -> list[str] | None:
    # RPCS3 VSL, VSR, VSLO, VSRO
    return ["%s = %s(%s, %s);" % (op.vr(op.f("vD")), _SHIFTS[op.name], op.vr(op.f("vA")),
                                  op.vr(op.f("vB")))]

"""Group `sys`: SPR/CR moves, CR logical ops, barriers, reservations and cache hints.

CR bits live one per byte (0 or 1), so CR logical instructions are byte operations; only the
packing forms (`mfcr`, `mtcrf`, `mfocrf`, `mtocrf`) convert. Unknown SPRs stay unhandled, with the
register number in the key.
"""

from __future__ import annotations

from .emit_core import Op, handler

_SPR_XER = 1
_SPR_LR = 8
_SPR_CTR = 9
_SPR_VRSAVE = 256
_TBR_LOWER = 268
_TBR_UPPER = 269


def _timebase(op: Op, spr: int) -> list[str] | None:
    """`rD` = lower or upper half of the time base, or None for any other number."""
    if spr == _TBR_LOWER:
        return ["%s = dalMftb(ctx);" % op.gpr(op.f("rD"))]
    if spr == _TBR_UPPER:
        return ["%s = dalMftb(ctx) >> 32;" % op.gpr(op.f("rD"))]
    return None


@handler("mfspr")
def emit_mfspr(op: Op) -> list[str] | None:
    # RPCS3 MFSPR (it also serves the time base through SPR 268/269)
    spr, rd = op.f("SPR"), op.gpr(op.f("rD"))
    simple = {_SPR_XER: "dalMfxer(ctx)", _SPR_LR: "ctx->lr", _SPR_CTR: "ctx->ctr",
              _SPR_VRSAVE: "ctx->vrsave"}
    if spr in simple:
        return ["%s = %s;" % (rd, simple[spr])]
    return _timebase(op, spr) or op.unsupported("spr=%d" % spr)


@handler("mtspr")
def emit_mtspr(op: Op) -> list[str] | None:
    # RPCS3 MTSPR
    spr, rs = op.f("SPR"), op.gpr(op.f("rS"))
    if spr == _SPR_XER:
        return ["dalMtxer(ctx, %s);" % rs]
    if spr in (_SPR_LR, _SPR_CTR):
        return ["ctx->%s = %s;" % ("lr" if spr == _SPR_LR else "ctr", rs)]
    if spr == _SPR_VRSAVE:
        return ["ctx->vrsave = (uint32_t)%s;" % rs]
    return op.unsupported("spr=%d" % spr)


@handler("mftb")
def emit_mftb(op: Op) -> list[str] | None:
    # RPCS3 MFTB: 268 = TBL, 269 = TBU (the 64-bit time base shifted down).
    spr = op.f("TBR")
    return _timebase(op, spr) or op.unsupported("tbr=%d" % spr)


def _first_field(mask: int) -> int:
    """Field selected by CRM: the leftmost set bit (RPCS3 `std::countl_zero`); `mask` is nonzero."""
    return 8 - mask.bit_length()


@handler("mfcr")
def emit_mfcr(op: Op) -> list[str] | None:
    # RPCS3 MFCR
    return ["%s = dalMfcr(ctx);" % op.gpr(op.f("rD"))]


@handler("mfocrf")
def emit_mfocrf(op: Op) -> list[str] | None:
    # RPCS3 MFOCRF: only the selected field's bits, all others 0.
    crm = op.f("FXM")
    if crm == 0:
        return op.unsupported("crm=0")
    field = _first_field(crm)
    return ["%s = (uint64_t)dalGetCrField(ctx, %d) << %d;" % (op.gpr(op.f("rD")), field,
                                                               28 - 4 * field)]


@handler("mtcrf")
def emit_mtcrf(op: Op) -> list[str] | None:
    # RPCS3 MTCRF: every field selected by CRM takes its nibble of the low word.
    crm, rs = op.f("CRM"), op.gpr(op.f("rS"))
    if crm == 0:
        return []
    return ["dalMtcrf(ctx, 0x%02xu, (uint32_t)%s);" % (crm, rs)]


@handler("mtocrf")
def emit_mtocrf(op: Op) -> list[str] | None:
    # RPCS3 MTOCRF: one field, chosen by the leftmost CRM bit.
    crm = op.f("CRM")
    if crm == 0:
        return op.unsupported("crm=0")
    field = _first_field(crm)
    return ["dalSetCrField(ctx, %d, (uint32_t)(%s >> %d) & 15u);" % (field, op.gpr(op.f("rS")),
                                                                    28 - 4 * field)]


@handler("mcrf")
def emit_mcrf(op: Op) -> list[str] | None:
    # RPCS3 MCRF
    return ["dalSetCrField(ctx, %d, dalGetCrField(ctx, %d));" % (op.f("crfD"), op.f("crfS"))]


# Operands are 0/1 bytes, so `^ 1` is logical not.
_CR_LOGICAL = {
    "crand": "{a} & {b}",
    "crandc": "{a} & ({b} ^ 1)",
    "creqv": "({a} ^ {b}) ^ 1",
    "crnand": "({a} & {b}) ^ 1",
    "crnor": "({a} | {b}) ^ 1",
    "cror": "{a} | {b}",
    "crorc": "{a} | ({b} ^ 1)",
    "crxor": "{a} ^ {b}",
}


@handler(*_CR_LOGICAL)
def emit_cr_logical(op: Op) -> list[str] | None:
    # RPCS3 CRAND, CRANDC, CREQV, CRNAND, CRNOR, CROR, CRORC, CRXOR
    d, a, b = op.f("crbD"), op.f("crbA"), op.f("crbB")
    dest = "ctx->cr[%d]" % d
    if a == b == d and op.name in ("crxor", "creqv"):
        # crclr / crset idioms: the result does not depend on the old bit.
        return ["%s = %d;" % (dest, 0 if op.name == "crxor" else 1)]
    expr = _CR_LOGICAL[op.name].format(a="ctx->cr[%d]" % a, b="ctx->cr[%d]" % b)
    return ["%s = (uint8_t)(%s);" % (dest, expr)]


@handler("sync")
def emit_sync(op: Op) -> list[str] | None:
    # RPCS3 SYNC: L = 1 is lwsync; L = 0 and the reserved/ptesync values are full barriers.
    kind = "kDalSyncLight" if op.f("L_sync") == 1 else "kDalSyncHeavy"
    return ["dalSync(ctx, %s);" % kind]


@handler("eieio")
def emit_eieio(op: Op) -> list[str] | None:
    return ["dalSync(ctx, kDalSyncEieio);"]


@handler("isync")
def emit_isync(op: Op) -> list[str] | None:
    # RPCS3 ISYNC
    return ["dalSync(ctx, kDalSyncIsync);"]


@handler("lwarx", "ldarx")
def emit_load_reserved(op: Op) -> list[str] | None:
    # RPCS3 LWARX, LDARX: the runtime records the reservation; lwarx zero-extends.
    helper = "dalLwarx" if op.name == "lwarx" else "dalLdarx"
    return ["%s = %s(ctx, base, %s);" % (op.gpr(op.f("rD")), helper, op.ea_x())]


@handler("stwcx.", "stdcx.")
def emit_store_conditional(op: Op) -> list[str] | None:
    # RPCS3 STWCX, STDCX: CR0 = {lt 0, gt 0, eq success, so XER.SO}.
    word = op.name == "stwcx."
    value = "(uint32_t)%s" % op.gpr(op.f("rS")) if word else op.gpr(op.f("rS"))
    return ["bool t0 = %s(ctx, base, %s, %s);" % ("dalStwcx" if word else "dalStdcx",
                                                  op.ea_x(), value),
            "dalSetCr0Conditional(ctx, t0);"]


@handler("dcbz")
def emit_dcbz(op: Op) -> list[str] | None:
    # RPCS3 DCBZ: clears the whole 128-byte line containing the address.
    return ["dalDcbz(ctx, base, %s);" % op.ea_x()]


# Prefetch and cache-maintenance hints have no architectural effect on guest-visible state here.
@handler("dcbt", "dcbtst", "dcbf", "dcbst", "icbi", "dst", "dstst", "dss")
def emit_cache_hint(op: Op) -> list[str] | None:
    return []

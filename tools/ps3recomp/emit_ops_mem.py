"""Group `mem`: integer and floating-point loads and stores, byte-reversed forms, lmw/stmw, strings.

The effective address is computed in 64 bits and truncated to the 32-bit guest address (RPCS3
`vm::cast`). Update forms write the untruncated sum to rA after the access, so a load with
rD == rA leaves the address in rA (RPCS3 `LWZU`). Update forms with rA = 0 are invalid and stay
unhandled.
"""

from __future__ import annotations

from dataclasses import dataclass

from .emit_core import Op, handler


@dataclass(frozen=True)
class _Access:
    store: bool
    width: int                  # bytes: 1, 2, 4, 8
    kind: str                   # "int", "algebraic", "reversed", "float", "single", "word"
    update: bool
    indexed: bool
    ds: bool = False            # DS-form displacement (ld, std, lwa families)


def _build_table() -> dict[str, _Access]:
    table: dict[str, _Access] = {}

    def add_family(load: str, store: str | None, width: int, kind: str, *,
                   ds: bool = False) -> None:
        """Register `<root>`, `<root>u` (D/DS-form) and `<root>x`, `<root>ux` (X-form)."""
        for update in (False, True):
            for name, is_store in ((load, False), (store, True)):
                if name is None:
                    continue
                table[name + ("u" if update else "")] = _Access(is_store, width, kind, update,
                                                                False, ds)
                table[name + ("ux" if update else "x")] = _Access(is_store, width, kind, update,
                                                                  True)

    add_family("lbz", "stb", 1, "int")
    add_family("lhz", "sth", 2, "int")
    add_family("lwz", "stw", 4, "int")
    add_family("ld", "std", 8, "int", ds=True)
    add_family("lha", None, 2, "algebraic")
    add_family("lwa", None, 4, "algebraic", ds=True)
    del table["lwau"]       # no such instruction: lwa is DS-form without an update variant
    add_family("lfs", "stfs", 4, "single")
    add_family("lfd", "stfd", 8, "float")
    for width, letter in ((2, "h"), (4, "w"), (8, "d")):
        table["l%sbrx" % letter] = _Access(False, width, "reversed", False, True)
        table["st%sbrx" % letter] = _Access(True, width, "reversed", False, True)
    table["stfiwx"] = _Access(True, 4, "word", False, True)
    return table


_TABLE = _build_table()
_READ = {1: "tox2Read8", 2: "tox2Read16", 4: "tox2Read32", 8: "tox2Read64"}
_WRITE = {1: "tox2Write8", 2: "tox2Write16", 4: "tox2Write32", 8: "tox2Write64"}
_NOTIFY_WRITE = {1: "tox2NotifyWrite8", 2: "tox2NotifyWrite16", 4: "tox2NotifyWrite32",
                 8: "tox2NotifyWrite64"}
_UNSIGNED = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
_SIGNED = {2: "int16_t", 4: "int32_t"}


def _load_value(access: _Access, ea: str) -> str:
    """Expression for the value a load puts in its destination register."""
    kind, width = access.kind, access.width
    if kind == "int":
        return "%s(base, %s)" % (_READ[width], ea)
    if kind == "algebraic":
        return "(uint64_t)(int64_t)(%s)%s(base, %s)" % (_SIGNED[width], _READ[width], ea)
    if kind == "reversed":
        # Memory is big-endian, so the reversed value is the little-endian read.
        if width == 2:
            return "(uint64_t)__builtin_bswap16(tox2Read16(base, %s))" % ea
        return "(uint64_t)__builtin_bswap%d(%s(base, %s))" % (width * 8, _READ[width], ea)
    raise AssertionError(kind)


def _store_value(access: _Access, source: str) -> str:
    """Expression for the value a store writes; `source` is the 64-bit register value."""
    kind, width = access.kind, access.width
    if kind == "int":
        return "(%s)%s" % (_UNSIGNED[width], source)
    if kind == "reversed":
        return "__builtin_bswap%d((%s)%s)" % (width * 8, _UNSIGNED[width], source)
    raise AssertionError(kind)


def _access_statement(op: Op, access: _Access, ea: str) -> str:
    """The single statement that performs the memory access at the 32-bit address `ea`."""
    kind = access.kind
    if not access.store:
        if kind == "single":
            return "%s.f64 = tox2ReadF32(base, %s);" % (op.fpr(op.f("frD")), ea)
        if kind == "float":
            return "%s.u64 = tox2Read64(base, %s);" % (op.fpr(op.f("frD")), ea)
        return "%s = %s;" % (op.gpr(op.f("rD")), _load_value(access, ea))
    if kind == "single":
        return "tox2WriteF32(base, %s, %s.f64);" % (ea, op.fpr(op.f("frS")))
    if kind == "float":
        return "tox2Write64(base, %s, %s.u64);" % (ea, op.fpr(op.f("frS")))
    if kind == "word":
        # stfiwx stores the low word of the FPR bits (RPCS3 `STFIWX`).
        return "tox2Write32(base, %s, (uint32_t)%s.u64);" % (ea, op.fpr(op.f("frS")))
    # Only integer and byte-reversed stores can be routed; the notify-file loader refuses the rest.
    write = _NOTIFY_WRITE if op.notify_store else _WRITE
    return "%s(base, %s, %s);" % (write[access.width],
                                  ea, _store_value(access, op.gpr(op.f("rS"))))


@handler(*_TABLE)
def emit_access(op: Op) -> list[str] | None:
    # RPCS3 LBZ ... LWZUX, LHA*, LWA*, LD*, LWBRX, STB ... STDUX, STWBRX, LFS*, LFD*, STFS*, STFD*,
    # STFIWX
    access = _TABLE[op.name]
    field = "DS" if access.ds else "D"
    if not access.update:
        ea = op.ea_x() if access.indexed else op.ea_d(field)
        return [_access_statement(op, access, ea)]
    if op.f("rA") == 0:
        return op.unsupported("ra0")
    ea64 = op.ea64_x() if access.indexed else op.ea64_d(field)
    return ["uint64_t t0 = %s;" % ea64,
            _access_statement(op, access, "(uint32_t)t0"),
            "%s = t0;" % op.gpr(op.f("rA"))]


@handler("lmw", "stmw")
def emit_multiple(op: Op) -> list[str] | None:
    # RPCS3 LMW, STMW: low words of rD..r31 (rS..r31), addresses truncated one by one.
    first = op.f("rD") if op.name == "lmw" else op.f("rS")
    lines = ["uint64_t t0 = %s;" % op.ea64_d()]
    for k, reg in enumerate(range(first, 32)):
        ea = "(uint32_t)(t0 + %d)" % (4 * k) if k else "(uint32_t)t0"
        if op.name == "lmw":
            lines.append("%s = tox2Read32(base, %s);" % (op.gpr(reg), ea))
        else:
            lines.append("tox2Write32(base, %s, (uint32_t)%s);" % (ea, op.gpr(reg)))
    return lines


def _string_ea_and_count(op: Op, indexed: bool) -> tuple[str, str]:
    """(guest address, byte count) of a string instruction."""
    if indexed:
        return op.ea_x(), "ctx->xer_count & 0x7f"
    ra = op.f("rA")
    nb = op.f("NB") or 32
    return ("(uint32_t)%s" % op.gpr(ra) if ra else "0u"), "%du" % nb


@handler("lswi", "lswx")
def emit_load_string(op: Op) -> list[str] | None:
    # RPCS3 LSWI, LSWX
    ea, count = _string_ea_and_count(op, op.name == "lswx")
    return ["tox2LoadString(ctx, base, %s, %d, %s);" % (ea, op.f("rD"), count)]


@handler("stswi", "stswx")
def emit_store_string(op: Op) -> list[str] | None:
    # RPCS3 STSWI, STSWX
    ea, count = _string_ea_and_count(op, op.name == "stswx")
    return ["tox2StoreString(ctx, base, %s, %d, %s);" % (ea, op.f("rS"), count)]

"""Tiny PPC encoder for tests: each function returns one big-endian instruction word.

Operand order follows assembler syntax (`lwz(rd, d, ra)` is `lwz rD,d(rA)`). Branch helpers
take the instruction address and the target, and compute the displacement. `encode` builds any
word of the decoder's opcode table from named fields.
"""

from ps3recomp import ppu


def _d_form(primary: int, rt: int, ra: int, imm: int) -> int:
    return primary << 26 | rt << 21 | ra << 16 | imm & 0xFFFF


def _x_form(xo: int, rt: int, ra: int, rb: int, rc: int = 0) -> int:
    return 31 << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc


def _ds_form(primary: int, rt: int, ra: int, ds: int, xo: int) -> int:
    return primary << 26 | rt << 21 | ra << 16 | ds & 0xFFFC | xo


def _md_form(xo: int, ra: int, rs: int, sh: int, mb: int) -> int:
    # The 6-bit mb/me field is stored rotated: low five bits first, high bit last.
    return (30 << 26 | rs << 21 | ra << 16 | (sh & 31) << 11 | (mb & 31) << 6
            | (mb >> 5) << 5 | xo << 2 | (sh >> 5) << 1)


def _spr_form(xo: int, rt: int, spr: int) -> int:
    # The two five-bit halves of the SPR number are swapped in the encoding.
    return 31 << 26 | rt << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | xo << 1


def b(addr: int, target: int, lk: bool = False, aa: bool = False) -> int:
    disp = target if aa else target - addr
    return 18 << 26 | disp & 0x03FFFFFC | int(aa) << 1 | int(lk)


def bc(bo: int, bi: int, addr: int, target: int, lk: bool = False, aa: bool = False) -> int:
    disp = target if aa else target - addr
    return 16 << 26 | bo << 21 | bi << 16 | disp & 0xFFFC | int(aa) << 1 | int(lk)


def bclr(bo: int, bi: int, lk: bool = False) -> int:
    return 19 << 26 | bo << 21 | bi << 16 | 16 << 1 | int(lk)


def bcctr(bo: int, bi: int, lk: bool = False) -> int:
    return 19 << 26 | bo << 21 | bi << 16 | 528 << 1 | int(lk)


def bgt(crf: int, addr: int, target: int) -> int:
    return bc(12, 4 * crf + 1, addr, target)


def ble(crf: int, addr: int, target: int) -> int:
    return bc(4, 4 * crf + 1, addr, target)


def bge(crf: int, addr: int, target: int) -> int:
    return bc(4, 4 * crf, addr, target)


def blr() -> int:
    return bclr(20, 0)


def bl(addr: int, target: int) -> int:
    return b(addr, target, lk=True)


def blrl() -> int:
    return bclr(20, 0, lk=True)


def bctr() -> int:
    return bcctr(20, 0)


def bctrl() -> int:
    return bcctr(20, 0, lk=True)


def mtctr(rs: int) -> int:
    return _spr_form(467, rs, 9)


def mflr(rd: int) -> int:
    return _spr_form(339, rd, 8)


def nop() -> int:
    return ori(0, 0, 0)


def addi(rd: int, ra: int, simm: int) -> int:
    return _d_form(14, rd, ra, simm)


def addis(rd: int, ra: int, simm: int) -> int:
    return _d_form(15, rd, ra, simm)


def li(rd: int, simm: int) -> int:
    return addi(rd, 0, simm)


def lis(rd: int, simm: int) -> int:
    return addis(rd, 0, simm)


def ori(ra: int, rs: int, uimm: int) -> int:
    return _d_form(24, rs, ra, uimm)


def oris(ra: int, rs: int, uimm: int) -> int:
    return _d_form(25, rs, ra, uimm)


def mr(ra: int, rs: int) -> int:
    return _x_form(444, rs, ra, rs)


def lwz(rd: int, d: int, ra: int) -> int:
    return _d_form(32, rd, ra, d)


def ld(rd: int, ds: int, ra: int) -> int:
    return _ds_form(58, rd, ra, ds, 0)


def stdu(rs: int, ds: int, ra: int) -> int:
    return _ds_form(62, rs, ra, ds, 1)


def lwzx(rd: int, ra: int, rb: int) -> int:
    return _x_form(23, rd, ra, rb)


def lwax(rd: int, ra: int, rb: int) -> int:
    return _x_form(341, rd, ra, rb)


def add(rd: int, ra: int, rb: int) -> int:
    return _x_form(266, rd, ra, rb)


def slwi(ra: int, rs: int, n: int) -> int:
    # rlwinm ra,rs,n,0,31-n
    return 21 << 26 | rs << 21 | ra << 16 | n << 11 | 0 << 6 | (31 - n) << 1


def sldi(ra: int, rs: int, n: int) -> int:
    # rldicr ra,rs,n,63-n
    return _md_form(1, ra, rs, n, 63 - n)


def clrldi(ra: int, rs: int, n: int) -> int:
    # rldicl ra,rs,0,n
    return _md_form(0, ra, rs, 0, n)


def rlwinm(ra: int, rs: int, sh: int, mb: int, me: int) -> int:
    return 21 << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1


def clrlwi(ra: int, rs: int, n: int) -> int:
    # rlwinm ra,rs,0,n,31
    return rlwinm(ra, rs, 0, n, 31)


def rldic(ra: int, rs: int, sh: int, mb: int) -> int:
    return _md_form(2, ra, rs, sh, mb)


def extsw(ra: int, rs: int) -> int:
    return _x_form(986, rs, ra, 0)


def cmplwi(crf: int, ra: int, uimm: int) -> int:
    return 10 << 26 | crf << 23 | ra << 16 | uimm & 0xFFFF


def cmplw(crf: int, ra: int, rb: int) -> int:
    return 31 << 26 | crf << 23 | ra << 16 | rb << 11 | 32 << 1


def sc() -> int:
    return 0x44000002


def tw(to: int, ra: int, rb: int) -> int:
    return _x_form(4, to, ra, rb)


def twi(to: int, ra: int, simm: int) -> int:
    return _d_form(3, to, ra, simm)


def td(to: int, ra: int, rb: int) -> int:
    return _x_form(68, to, ra, rb)


def tdi(to: int, ra: int, simm: int) -> int:
    return _d_form(2, to, ra, simm)


def bdnz(addr: int, target: int) -> int:
    return bc(16, 0, addr, target)


def trap() -> int:
    return tw(31, 0, 0)


# Cell-only vector loads/stores (not in stock AltiVec).
def lvlx(vd: int, ra: int, rb: int) -> int:
    return _x_form(519, vd, ra, rb)


def lvlxl(vd: int, ra: int, rb: int) -> int:
    return _x_form(775, vd, ra, rb)


def lvrx(vd: int, ra: int, rb: int) -> int:
    return _x_form(551, vd, ra, rb)


def lvrxl(vd: int, ra: int, rb: int) -> int:
    return _x_form(807, vd, ra, rb)


def stvlx(vs: int, ra: int, rb: int) -> int:
    return _x_form(647, vs, ra, rb)


def stvlxl(vs: int, ra: int, rb: int) -> int:
    return _x_form(903, vs, ra, rb)


def stvrx(vs: int, ra: int, rb: int) -> int:
    return _x_form(679, vs, ra, rb)


def stvrxl(vs: int, ra: int, rb: int) -> int:
    return _x_form(935, vs, ra, rb)


# Fields whose value is stored as separate bit pieces: (value bit offset, IBM first bit, last bit).
_PIECES: dict[str, tuple[tuple[int, int, int], ...]] = {
    "SH64": ((0, 16, 20), (5, 30, 30)),
    "MB64": ((0, 21, 25), (5, 26, 26)),
    # The two five-bit halves of the SPR/TBR number are swapped in the encoding.
    "SPR": ((0, 11, 15), (5, 16, 20)),
    "TBR": ((0, 11, 15), (5, 16, 20)),
}
_SIGNED_FIELDS = frozenset({"SIMM", "D", "DS", "BD", "LI", "SIMM5"})
_BYTE_OFFSET_FIELDS = frozenset({"DS", "BD", "LI"})      # given in bytes, stored divided by 4
_FLAG_FIELDS = {"rc": "Rc", "oe": "OE", "lk": "LK", "aa": "AA", "rc_vc": "Rc_vc"}
_FLAG_IBM_BIT = {"Rc": 31, "LK": 31, "AA": 30, "OE": 21, "Rc_vc": 21}

_SPECS: dict[str, ppu.OpSpec] = {}


def _spec_named(name: str) -> ppu.OpSpec:
    if not _SPECS:
        for spec in ppu.ALL_OPS:
            if spec.name in _SPECS:
                raise ValueError("duplicate mnemonic %r in the opcode table" % spec.name)
            _SPECS[spec.name] = spec
    try:
        return _SPECS[name]
    except KeyError:
        raise ValueError("unknown mnemonic %r" % name) from None


def operand_fields(spec: ppu.OpSpec) -> tuple[str, ...]:
    """Field names of a spec's operands; pseudo-operands such as `D(rA)` count as two."""
    names: list[str] = []
    for operand in spec.operands:
        names.extend(operand.replace(")", "").split("("))
    return tuple(names)


def field_range(name: str) -> tuple[int, int]:
    """Inclusive range of values `encode` accepts for a field (byte offsets for DS/BD/LI)."""
    width = sum(b - a + 1 for _, a, b in _PIECES.get(name) or ((0, *ppu.FIELD_BITS[name][0]),))
    if name in _SIGNED_FIELDS:
        low, high = -(1 << (width - 1)), (1 << (width - 1)) - 1
        if name in _BYTE_OFFSET_FIELDS:
            return low * 4, high * 4
        return low, high
    return 0, (1 << width) - 1


def encode(name: str, **fields: int) -> int:
    """Build the word of mnemonic `name`; omitted operand fields are 0.

    Operands use the decoder's values: sign-extended fields (`SIMM`, `D`, `SIMM5`) may be negative,
    `DS`/`BD`/`LI` are byte offsets (multiples of 4), `SPR`/`TBR` are the plain register numbers.
    Flags are `Rc`, `OE`, `LK`, `AA` and `Rc_vc`, allowed only where the mnemonic has them.
    """
    spec = _spec_named(name)
    allowed = set(operand_fields(spec))
    allowed.update(_FLAG_FIELDS[flag] for flag in spec.flags)
    # strict_match, not match: it also carries the fixed bits of sc, stwcx. and stdcx.
    word = spec.strict_match
    used = spec.strict_mask
    for field_name, value in fields.items():
        if field_name not in allowed:
            raise ValueError("%s has no field %r" % (name, field_name))
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError("%s.%s: value must be an int" % (name, field_name))
        if field_name in _FLAG_IBM_BIT:
            low, high = 0, 1
            pieces = ((0, _FLAG_IBM_BIT[field_name], _FLAG_IBM_BIT[field_name]),)
        else:
            low, high = field_range(field_name)
            pieces = _PIECES.get(field_name) or ((0, *ppu.FIELD_BITS[field_name][0]),)
        if not low <= value <= high:
            raise ValueError("%s.%s: %d is outside %d..%d" % (name, field_name, value, low, high))
        if field_name in _BYTE_OFFSET_FIELDS:
            if value % 4:
                raise ValueError("%s.%s: %d is not a multiple of 4" % (name, field_name, value))
            value //= 4
        for shift, first, last in pieces:
            mask = ((1 << (last - first + 1)) - 1) << (31 - last)
            if used & mask & ~spec.strict_mask and field_name not in _FLAG_IBM_BIT:
                raise ValueError("%s.%s overlaps another field" % (name, field_name))
            used |= mask
            word = word & ~mask | ((value >> shift) << (31 - last)) & mask
    return word

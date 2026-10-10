"""PPU (PowerPC 64, Cell) instruction decoder: field extraction, opcode lookup, `Insn`."""

from __future__ import annotations

import struct
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field


def bits(word: int, a: int, b: int) -> int:
    """Inclusive IBM bit range (bit 0 is the MSB of the 32-bit word)."""
    return (word >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def _sext(value: int, width: int) -> int:
    return value - (1 << width) if value & (1 << (width - 1)) else value


def _f(a: int, b: int) -> Callable[[int], int]:
    return lambda w: bits(w, a, b)


def _fs(a: int, b: int, shift: int = 0) -> Callable[[int], int]:
    return lambda w: _sext(bits(w, a, b), b - a + 1) << shift


FIELDS: dict[str, Callable[[int], int]] = {}
# Inclusive IBM bit ranges that make up each FIELDS entry; strict decoding treats every bit
# outside the opcode mask, the operand fields and the flag bits as reserved.
FIELD_BITS: dict[str, tuple[tuple[int, int], ...]] = {}
for _names, _range in (
    ("rD rS frD frS vD vS crbD BO TO", (6, 10)),
    ("rA frA vA crbA BI", (11, 15)),
    ("rB frB vB crbB SH NB", (16, 20)),
    ("frC vC MB", (21, 25)),
    ("ME", (26, 30)),
    ("crfD", (6, 8)),
    ("crfS", (11, 13)),
    ("L", (10, 10)),
    ("L_sync", (9, 10)),
    ("UIMM", (16, 31)),
    ("AA", (30, 30)),
    ("LK Rc", (31, 31)),
    ("OE Rc_vc", (21, 21)),
    ("CRM FXM", (12, 19)),
    ("FM", (7, 14)),
    ("IMM_fp", (16, 19)),
    ("UIMM5", (11, 15)),
    ("SHB", (22, 25)),
    ("STRM", (9, 10)),
):
    for _n in _names.split():
        FIELDS[_n] = _f(*_range)
        FIELD_BITS[_n] = (_range,)
FIELDS["SIMM"] = FIELDS["D"] = _fs(16, 31)
FIELD_BITS["SIMM"] = FIELD_BITS["D"] = ((16, 31),)
FIELDS["DS"] = _fs(16, 29, 2)
FIELD_BITS["DS"] = ((16, 29),)
FIELDS["BD"] = _fs(16, 29, 2)
FIELD_BITS["BD"] = ((16, 29),)
FIELDS["LI"] = _fs(6, 29, 2)
FIELD_BITS["LI"] = ((6, 29),)
FIELDS["SIMM5"] = _fs(11, 15)
FIELD_BITS["SIMM5"] = ((11, 15),)
FIELDS["SH64"] = lambda w: bits(w, 16, 20) | bits(w, 30, 30) << 5
FIELD_BITS["SH64"] = ((16, 20), (30, 30))
# The high bit of mb/me is stored in bit 26 (MD/MDS forms).
FIELDS["MB64"] = lambda w: bits(w, 21, 25) | bits(w, 26, 26) << 5
FIELD_BITS["MB64"] = ((21, 26),)
# SPR/TBR halves are swapped in the encoding.
FIELDS["SPR"] = FIELDS["TBR"] = lambda w: bits(w, 16, 20) << 5 | bits(w, 11, 15)
FIELD_BITS["SPR"] = FIELD_BITS["TBR"] = ((11, 20),)

# Form -> inclusive IBM bit range of the extended opcode, None when the form has none.
FORMS: dict[str, tuple[int, int] | None] = {
    "D": None, "I": None, "B": None, "M": None, "SC": None,
    "DS": (30, 31),
    "X": (21, 30), "XL": (21, 30), "XFX": (21, 30), "XFL": (21, 30),
    "XO": (22, 30),
    "XS": (21, 29),
    "A": (26, 30),
    "MD": (27, 29),
    "MDS": (27, 30),
    "VA": (26, 31),
    "VX": (21, 31),
    "VC": (22, 31),
}

_FLAGS = frozenset({"rc", "oe", "lk", "aa", "rc_vc"})
_FLAG_BITS = {"rc": (31, 31), "lk": (31, 31), "aa": (30, 30), "oe": (21, 21), "rc_vc": (21, 21)}


def _ibm_mask(a: int, b: int) -> int:
    return ((1 << (b - a + 1)) - 1) << (31 - b)


# Spec name -> (allow_mask, ones_mask). Allowed bits may take any value in strict mode; ones
# bits must be 1. Everything else outside the opcode, operands and flags must be 0.
# Not listed on purpose: mfcr/mtcrf. Words with bit 11 set belong to the more specific
# mfocrf/mtocrf rows, and a strict-rejected one must not decode as plain mfcr/mtcrf.
# BO values with a set "z" bit (e.g. 21-23, 28-31) are invalid operand values, not reserved
# encoding bits, so strict mode does not check them.
STRICT_EXCEPTIONS: dict[str, tuple[int, int]] = {
    # LEV (bits 20-26) stays reserved: strict sc is exactly 0x44000002 (lv2 calls only).
    "sc": (0, _ibm_mask(30, 30)),
    # The dot is part of the encoding, not an rc flag.
    "stwcx.": (0, _ibm_mask(31, 31)),
    "stdcx.": (0, _ibm_mask(31, 31)),
    # BH branch hint.
    "bclr": (_ibm_mask(19, 20), 0),
    "bcctr": (_ibm_mask(19, 20), 0),
    # TH (touch hint).
    "dcbt": (_ibm_mask(6, 10), 0),
    "dcbtst": (_ibm_mask(6, 10), 0),
    # T (transient) and A (dssall) bits.
    "dst": (_ibm_mask(6, 6), 0),
    "dstst": (_ibm_mask(6, 6), 0),
    "dss": (_ibm_mask(6, 6), 0),
}


def _operand_bits(operands: tuple[str, ...]) -> int:
    out = 0
    for operand in operands:
        # Pseudo-operands such as "D(rA)" stand for two fields.
        for name in operand.replace(")", "").split("("):
            for a, b in FIELD_BITS[name]:
                out |= _ibm_mask(a, b)
    return out


@dataclass(frozen=True)
class OpSpec:
    name: str
    form: str
    primary: int
    xo: int | None
    operands: tuple[str, ...]
    flags: frozenset[str]
    mask: int
    match: int
    # Derived in __post_init__ (so dataclasses.replace() of mask/match stays consistent):
    # a word is strictly valid when `word & strict_mask == strict_match`.
    strict_mask: int = field(init=False, compare=False)
    strict_match: int = field(init=False, compare=False)

    def __post_init__(self) -> None:
        allow, ones = STRICT_EXCEPTIONS.get(self.name, (0, 0))
        used = self.mask | _operand_bits(self.operands) | allow
        for flag in self.flags:
            used |= _ibm_mask(*_FLAG_BITS[flag])
        reserved = 0xFFFFFFFF & ~used
        object.__setattr__(self, "strict_mask", self.mask | reserved | ones)
        object.__setattr__(self, "strict_match", self.match | ones)


def op(name: str, form: str, primary: int, xo: int | None = None,
       operands: tuple[str, ...] = (), flags: tuple[str, ...] | frozenset[str] = ()) -> OpSpec:
    if form not in FORMS:
        raise ValueError(f"{name}: unknown form {form!r}")
    if not 0 <= primary < 64:
        raise ValueError(f"{name}: primary {primary} out of range")
    flag_set = frozenset(flags)
    if not flag_set <= _FLAGS:
        raise ValueError(f"{name}: unknown flags {sorted(flag_set - _FLAGS)}")
    span = FORMS[form]
    mask = 0xFC000000
    match = primary << 26
    if span is None:
        if xo is not None:
            raise ValueError(f"{name}: form {form} has no extended opcode")
    else:
        start, end = span
        if xo is None or not 0 <= xo < 1 << (end - start + 1):
            raise ValueError(f"{name}: bad extended opcode {xo!r} for form {form}")
        shift = 31 - end
        mask |= ((1 << (end - start + 1)) - 1) << shift
        match |= xo << shift
    return OpSpec(name, form, primary, xo, tuple(operands), flag_set, mask, match)


@dataclass(frozen=True)
class Insn:
    addr: int
    word: int
    spec: OpSpec | None

    def field(self, name: str) -> int:
        return FIELDS[name](self.word)

    @property
    def mnemonic(self) -> str | None:
        spec = self.spec
        if spec is None:
            return None
        flags = spec.flags
        out = spec.name
        if "oe" in flags and self.field("OE"):
            out += "o"
        if ("rc" in flags and self.field("Rc")) or ("rc_vc" in flags and self.field("Rc_vc")):
            out += "."
        if "lk" in flags and self.field("LK"):
            out += "l"
        if "aa" in flags and self.field("AA"):
            out += "a"
        return out


def _build_lookup(specs: tuple[OpSpec, ...]) -> dict[int, list[tuple[int, dict[int, OpSpec]]]]:
    by_primary: dict[int, dict[int, dict[int, OpSpec]]] = {}
    for spec in specs:
        groups = by_primary.setdefault(spec.primary, {})
        group = groups.setdefault(spec.mask, {})
        if spec.match in group:
            raise ValueError(
                f"duplicate opcode (mask {spec.mask:#010x}, match {spec.match:#010x}): "
                f"{group[spec.match].name} / {spec.name}")
        group[spec.match] = spec
    # Most specific masks first so a wider-mask entry never shadows a narrower one.
    return {
        primary: sorted(groups.items(), key=lambda kv: kv[0].bit_count(), reverse=True)
        for primary, groups in by_primary.items()
    }


# Imported here because the table modules use `op` from this module; import ppu first.
from . import ppu_ops, ppu_vmx  # noqa: E402

ALL_OPS: tuple[OpSpec, ...] = tuple(ppu_ops.OPS + ppu_vmx.OPS)
_LOOKUP = _build_lookup(ALL_OPS)


def decode(word: int, addr: int = 0, strict: bool = False) -> Insn:
    for mask, group in _LOOKUP.get(word >> 26, ()):
        spec = group.get(word & mask)
        # In strict mode a spec whose reserved bits are set is skipped, not returned.
        if spec is not None and (not strict or word & spec.strict_mask == spec.strict_match):
            return Insn(addr, word, spec)
    return Insn(addr, word, None)


def decode_range(data: bytes, base_addr: int, strict: bool = False) -> Iterator[Insn]:
    # A trailing partial word is ignored.
    for offset in range(0, len(data) - 3, 4):
        yield decode(struct.unpack_from(">I", data, offset)[0], base_addr + offset, strict)

"""SPU (Cell Synergistic Processor) instruction decoder: field extraction, opcode lookup, `SpuInsn`.

Layouts follow the public "Synergistic Processor Unit Instruction Set Architecture" (version 1.2),
chapter "Instruction formats". Bit 0 is the most significant bit of the 32-bit word; fields below
are stored as (shift, width) pieces counted from the least significant bit.

Opcodes have no fixed primary field. Their width depends on the format (RRR 4, RI18 and hint 7,
RI10 8, RI16 9, RI8 10, RR/RI7 and the special forms 11 bits), and the set of opcodes is a prefix
code, so decoding tries each width in turn.

Reserved bits are ignored by `decode`, as the hardware does.
"""

from __future__ import annotations

import struct
from collections.abc import Iterator
from dataclasses import dataclass

# LSLR mask with the two low bits cleared: every LS branch target is word aligned.
LS_TARGET_MASK = 0x3FFFC


@dataclass(frozen=True)
class Field:
    """A (possibly split) bit field. `scale` is a left shift that turns the stored count into bytes."""

    pieces: tuple[tuple[int, int], ...]  # (shift, width), most significant piece first
    signed: bool = False
    scale: int = 0

    @property
    def width(self) -> int:
        return sum(width for _, width in self.pieces)

    @property
    def mask(self) -> int:
        out = 0
        for shift, width in self.pieces:
            out |= ((1 << width) - 1) << shift
        return out

    @property
    def range(self) -> tuple[int, int]:
        """Inclusive range of the values `extract` can return (a multiple of 1 << scale)."""
        width = self.width
        low, high = (-(1 << (width - 1)), (1 << (width - 1)) - 1) if self.signed else (0, (1 << width) - 1)
        return low << self.scale, high << self.scale

    def extract(self, word: int) -> int:
        value = 0
        for shift, width in self.pieces:
            value = value << width | (word >> shift) & ((1 << width) - 1)
        width = self.width
        if self.signed and value & (1 << (width - 1)):
            value -= 1 << width
        return value << self.scale

    def insert(self, value: int) -> int:
        """Bits to OR into a word; raises ValueError when `value` is not representable."""
        low, high = self.range
        if not low <= value <= high:
            raise ValueError(f"{value} is outside {low}..{high}")
        if value & ((1 << self.scale) - 1):
            raise ValueError(f"{value} is not a multiple of {1 << self.scale}")
        raw = (value >> self.scale) & ((1 << self.width) - 1)
        out = 0
        for shift, width in reversed(self.pieces):
            out |= (raw & ((1 << width) - 1)) << shift
            raw >>= width
        return out


def _reg(shift: int) -> Field:
    return Field(((shift, 7),))


FIELDS: dict[str, Field] = {
    # RR/RI forms: RT is the low field; stopd calls the same bits RC.
    "RT": _reg(0),
    "RC": _reg(0),
    "RA": _reg(7),
    "RB": _reg(14),
    # RRR form: the destination sits in bits 4-10 and RC in the low field.
    "RT4": _reg(21),
    "I7": Field(((14, 7),), signed=True),
    "U7": Field(((14, 7),)),
    "I8": Field(((14, 8),)),
    "I10": Field(((14, 10),), signed=True),
    # lqd/stqd displacements are in quadwords.
    "I10x16": Field(((14, 10),), signed=True, scale=4),
    "I16": Field(((7, 16),), signed=True),
    "U16": Field(((7, 16),)),
    # Word counts in the encoding, byte values here.
    "REL16": Field(((7, 16),), signed=True, scale=2),
    "ABS16": Field(((7, 16),), signed=True, scale=2),
    "I18": Field(((7, 18),)),
    "CA": _reg(7),
    "SA": _reg(7),
    "STOP": Field(((0, 14),)),
    # Branch hint offset (in bytes) of the hinted branch from the hint instruction: ROH || ROL.
    "RO": Field(((23, 2), (0, 7)), signed=True, scale=2),
    "RO_IND": Field(((14, 2), (0, 7)), signed=True, scale=2),
    # Flag bits: indirect branches have D and E, sync has C, hbr has P.
    "D": Field(((19, 1),)),
    "E": Field(((18, 1),)),
    "C": Field(((20, 1),)),
    "P": Field(((20, 1),)),
}

# Form -> opcode width in bits.
FORMS: dict[str, int] = {
    "RRR": 4,
    "RI18": 7,
    "HBR_REL": 7,
    "HBR_ABS": 7,
    "RI10": 8,
    "RI16": 9,
    "RI8": 10,
    "RI7": 11,
    "RR": 11,
    "CH": 11,
    "BI": 11,
    "HBR": 11,
    "STOP": 11,
    "SYNC": 11,
    "NOP": 11,
}

# Instruction groups; one emitter module per group.
GROUPS = ("int", "perm", "mem", "control", "chan", "fp_single", "fp_double")

# Flow flags: "branch" is any transfer of control; "relative"/"absolute"/"indirect" tell how the target
# is given; "link" writes the return address to RT; "conditional" may fall through; "stop" is stop/stopd;
# "halt" is the conditional halt group; "hint" has no architectural effect; "load"/"store" access LS.
FLOW_FLAGS = frozenset({
    "branch", "relative", "absolute", "indirect", "conditional", "link", "stop", "halt", "hint", "load", "store",
})


@dataclass(frozen=True)
class SpuSpec:
    mnemonic: str
    form: str
    width: int
    opcode: int
    operands: tuple[str, ...]
    group: str
    flags: frozenset[str]
    mask: int
    match: int

    @property
    def field_names(self) -> tuple[str, ...]:
        """Operand fields in assembler order; the "I10x16(RA)" pseudo-operand stands for two fields."""
        out: list[str] = []
        for operand in self.operands:
            out.extend(operand.replace(")", "").split("("))
        return tuple(out)

    @property
    def flag_fields(self) -> tuple[str, ...]:
        """Mnemonic-modifying bit fields (D/E, C, P) the spec allows."""
        if "indirect" in self.flags and "branch" in self.flags:
            return ("D", "E")
        if self.mnemonic == "sync":
            return ("C",)
        if self.mnemonic == "hbr":
            return ("P",)
        return ()


def _op(mnemonic: str, form: str, opcode: int, operands: tuple[str, ...], group: str,
        flags: tuple[str, ...] = ()) -> SpuSpec:
    width = FORMS[form]
    if not 0 <= opcode < 1 << width:
        raise ValueError(f"{mnemonic}: opcode {opcode:#x} does not fit {width} bits")
    if group not in GROUPS:
        raise ValueError(f"{mnemonic}: unknown group {group!r}")
    flag_set = frozenset(flags)
    if not flag_set <= FLOW_FLAGS:
        raise ValueError(f"{mnemonic}: unknown flags {sorted(flag_set - FLOW_FLAGS)}")
    shift = 32 - width
    return SpuSpec(mnemonic, form, width, opcode, operands, group, flag_set,
                   ((1 << width) - 1) << shift, opcode << shift)


_RRR = ("RT4", "RA", "RB", "RC")
_RR3 = ("RT", "RA", "RB")
_RR2 = ("RT", "RA")
_RI7 = ("RT", "RA", "I7")
_RI10 = ("RT", "RA", "I10")
_MEMD = ("RT", "I10x16(RA)")
_MEMX = ("RT", "I7(RA)")


def _table() -> list[SpuSpec]:
    t: list[SpuSpec] = []

    def add(form: str, group: str, operands: tuple[str, ...], entries: dict[str, int],
            flags: tuple[str, ...] = ()) -> None:
        for mnemonic, opcode in entries.items():
            t.append(_op(mnemonic, form, opcode, operands, group, flags))

    # RRR (4-bit opcodes).
    add("RRR", "perm", _RRR, {"shufb": 0xB})
    add("RRR", "int", _RRR, {"selb": 0x8, "mpya": 0xC})
    add("RRR", "fp_single", _RRR, {"fnms": 0xD, "fma": 0xE, "fms": 0xF})

    # RI18, hint forms (7-bit opcodes).
    add("RI18", "int", ("RT", "I18"), {"ila": 0x21})
    add("HBR_ABS", "control", ("RO", "ABS16"), {"hbra": 0x08}, ("hint",))
    add("HBR_REL", "control", ("RO", "REL16"), {"hbrr": 0x09}, ("hint",))

    # RI10 (8-bit opcodes).
    add("RI10", "mem", _MEMD, {"lqd": 0x34}, ("load",))
    add("RI10", "mem", _MEMD, {"stqd": 0x24}, ("store",))
    add("RI10", "int", _RI10, {
        "ai": 0x1C, "ahi": 0x1D, "sfi": 0x0C, "sfhi": 0x0D,
        "andi": 0x14, "andhi": 0x15, "andbi": 0x16, "ori": 0x04, "orhi": 0x05, "orbi": 0x06,
        "xori": 0x44, "xorhi": 0x45, "xorbi": 0x46,
        "ceqi": 0x7C, "ceqhi": 0x7D, "ceqbi": 0x7E, "cgti": 0x4C, "cgthi": 0x4D, "cgtbi": 0x4E,
        "clgti": 0x5C, "clgthi": 0x5D, "clgtbi": 0x5E, "mpyi": 0x74, "mpyui": 0x75,
    })
    add("RI10", "control", ("RA", "I10"), {"heqi": 0x7F, "hgti": 0x4F, "hlgti": 0x5F}, ("halt",))

    # RI16 (9-bit opcodes).
    add("RI16", "mem", ("RT", "ABS16"), {"lqa": 0x061}, ("load",))
    add("RI16", "mem", ("RT", "REL16"), {"lqr": 0x067}, ("load",))
    add("RI16", "mem", ("RT", "ABS16"), {"stqa": 0x041}, ("store",))
    add("RI16", "mem", ("RT", "REL16"), {"stqr": 0x047}, ("store",))
    add("RI16", "int", ("RT", "U16"), {"ilh": 0x083, "ilhu": 0x082, "iohl": 0x0C1})
    add("RI16", "int", ("RT", "I16"), {"il": 0x081})
    add("RI16", "perm", ("RT", "U16"), {"fsmbi": 0x065})
    add("RI16", "control", ("REL16",), {"br": 0x064}, ("branch", "relative"))
    add("RI16", "control", ("ABS16",), {"bra": 0x060}, ("branch", "absolute"))
    add("RI16", "control", ("RT", "REL16"), {"brsl": 0x066}, ("branch", "relative", "link"))
    add("RI16", "control", ("RT", "ABS16"), {"brasl": 0x062}, ("branch", "absolute", "link"))
    add("RI16", "control", ("RT", "REL16"),
        {"brz": 0x040, "brnz": 0x042, "brhz": 0x044, "brhnz": 0x046}, ("branch", "relative", "conditional"))

    # RI8 (10-bit opcodes): float/integer conversion with a scale factor.
    add("RI8", "fp_single", ("RT", "RA", "I8"), {"cflts": 0x1D8, "cfltu": 0x1D9, "csflt": 0x1DA, "cuflt": 0x1DB})

    # RR and RI7 (11-bit opcodes).
    add("RR", "int", _RR3, {
        "ah": 0x0C8, "a": 0x0C0, "sfh": 0x048, "sf": 0x040,
        "addx": 0x340, "cg": 0x0C2, "cgx": 0x342, "sfx": 0x341, "bg": 0x042, "bgx": 0x343,
        "mpy": 0x3C4, "mpyu": 0x3CC, "mpyh": 0x3C5, "mpys": 0x3C7, "mpyhh": 0x3C6, "mpyhha": 0x346,
        "mpyhhu": 0x3CE, "mpyhhau": 0x34E,
        "avgb": 0x0D3, "absdb": 0x053, "sumb": 0x253,
        "and": 0x0C1, "andc": 0x2C1, "or": 0x041, "orc": 0x2C9, "xor": 0x241,
        "nand": 0x0C9, "nor": 0x049, "eqv": 0x249,
        "shlh": 0x05F, "shl": 0x05B, "roth": 0x05C, "rot": 0x058,
        "rothm": 0x05D, "rotm": 0x059, "rotmah": 0x05E, "rotma": 0x05A,
        "ceqb": 0x3D0, "ceqh": 0x3C8, "ceq": 0x3C0, "cgtb": 0x250, "cgth": 0x248, "cgt": 0x240,
        "clgtb": 0x2D0, "clgth": 0x2C8, "clgt": 0x2C0,
    })
    add("RR", "int", _RR2, {"clz": 0x2A5, "cntb": 0x2B4, "xsbh": 0x2B6, "xshw": 0x2AE, "xswd": 0x2A6})
    # Perm group: everything that moves bytes or bits across lanes (ISA chapter "Shift and rotate
    # quadword" and the control/select/gather instructions).
    add("RR", "perm", _RR3, {
        "shlqbi": 0x1DB, "shlqby": 0x1DF, "shlqbybi": 0x1CF,
        "rotqbi": 0x1D8, "rotqby": 0x1DC, "rotqbybi": 0x1CC,
        "rotqmbi": 0x1D9, "rotqmby": 0x1DD, "rotqmbybi": 0x1CD,
        "cbx": 0x1D4, "chx": 0x1D5, "cwx": 0x1D6, "cdx": 0x1D7,
    })
    add("RR", "perm", _RR2, {"fsmb": 0x1B6, "fsmh": 0x1B5, "fsm": 0x1B4, "gbb": 0x1B2, "gbh": 0x1B1,
                             "gb": 0x1B0, "orx": 0x1F0})
    add("RR", "mem", _RR3, {"lqx": 0x1C4}, ("load",))
    add("RR", "mem", _RR3, {"stqx": 0x144}, ("store",))
    add("RR", "control", ("RA", "RB"), {"heq": 0x3D8, "hgt": 0x258, "hlgt": 0x2D8}, ("halt",))
    add("RR", "control", ("RC", "RA", "RB"), {"stopd": 0x140}, ("stop",))
    add("RR", "fp_single", _RR3, {
        "fa": 0x2C4, "fs": 0x2C5, "fm": 0x2C6, "fceq": 0x3C2, "fcmeq": 0x3CA, "fcgt": 0x2C2, "fcmgt": 0x2CA,
        "fi": 0x3D4,
    })
    add("RR", "fp_single", _RR2, {"frest": 0x1B8, "frsqest": 0x1B9})
    add("RR", "fp_single", ("RT",), {"fscrrd": 0x398})
    add("RR", "fp_single", _RR2, {"fscrwr": 0x3BA})
    add("RR", "fp_double", _RR3, {
        "dfa": 0x2CC, "dfs": 0x2CD, "dfm": 0x2CE, "dfma": 0x35C, "dfms": 0x35D, "dfnms": 0x35E, "dfnma": 0x35F,
        "dfceq": 0x3C3, "dfcmeq": 0x3CB, "dfcgt": 0x2C3, "dfcmgt": 0x2CB,
    })
    add("RR", "fp_double", _RR2, {"fesd": 0x3B8, "frds": 0x3B9})
    add("RI7", "fp_double", ("RT", "RA", "U7"), {"dftsv": 0x3BF})
    add("RI7", "int", _RI7, {
        "shlhi": 0x07F, "shli": 0x07B, "rothi": 0x07C, "roti": 0x078,
        "rothmi": 0x07D, "rotmi": 0x079, "rotmahi": 0x07E, "rotmai": 0x07A,
    })
    add("RI7", "perm", _RI7, {
        "shlqbii": 0x1FB, "shlqbyi": 0x1FF, "rotqbii": 0x1F8, "rotqbyi": 0x1FC,
        "rotqmbii": 0x1F9, "rotqmbyi": 0x1FD,
    })
    add("RI7", "perm", _MEMX, {"cbd": 0x1F4, "chd": 0x1F5, "cwd": 0x1F6, "cdd": 0x1F7})

    # Indirect branches: D (bit 12) disables and E (bit 13) enables interrupts around the transfer.
    add("BI", "control", ("RA",), {"bi": 0x1A8}, ("branch", "indirect"))
    add("BI", "control", ("RA",), {"iret": 0x1AA}, ("branch", "indirect"))
    add("BI", "control", ("RT", "RA"), {"bisl": 0x1A9}, ("branch", "indirect", "link"))
    add("BI", "control", ("RT", "RA"), {"bisled": 0x1AB}, ("branch", "indirect", "link", "conditional"))
    add("BI", "control", ("RT", "RA"),
        {"biz": 0x128, "binz": 0x129, "bihz": 0x12A, "bihnz": 0x12B}, ("branch", "indirect", "conditional"))

    # Special encodings.
    add("HBR", "control", ("RO_IND", "RA"), {"hbr": 0x1AC}, ("hint",))
    add("STOP", "control", ("STOP",), {"stop": 0x000}, ("stop",))
    add("NOP", "control", ("RT",), {"nop": 0x201})
    add("NOP", "control", (), {"lnop": 0x001, "dsync": 0x003})
    add("SYNC", "control", (), {"sync": 0x002})
    add("CH", "chan", ("RT", "CA"), {"rdch": 0x00D, "rchcnt": 0x00F})
    add("CH", "chan", ("CA", "RT"), {"wrch": 0x10D})
    # No SPRs are implemented (ISA, mfspr/mtspr), but the encodings exist.
    add("CH", "control", ("RT", "SA"), {"mfspr": 0x00C})
    add("CH", "control", ("SA", "RT"), {"mtspr": 0x10C})
    return t


ALL_OPS: tuple[SpuSpec, ...] = tuple(_table())
SPECS_BY_NAME: dict[str, SpuSpec] = {spec.mnemonic: spec for spec in ALL_OPS}


def _build_lookup(specs: tuple[SpuSpec, ...]) -> dict[int, dict[int, SpuSpec]]:
    by_width: dict[int, dict[int, SpuSpec]] = {}
    for spec in specs:
        group = by_width.setdefault(spec.width, {})
        if spec.opcode in group:
            raise ValueError(f"duplicate opcode {spec.opcode:#x}: {group[spec.opcode].mnemonic} / {spec.mnemonic}")
        group[spec.opcode] = spec
    # The opcode space is a prefix code: a short opcode must not be the start of a longer one.
    widths = sorted(by_width)
    for i, short in enumerate(widths):
        for long in widths[i + 1:]:
            for opcode, spec in by_width[long].items():
                other = by_width[short].get(opcode >> (long - short))
                if other is not None:
                    raise ValueError(f"opcode of {spec.mnemonic} starts with the opcode of {other.mnemonic}")
    return {width: by_width[width] for width in widths}


if len(SPECS_BY_NAME) != len(ALL_OPS):
    raise ValueError("duplicate mnemonic in the SPU table")
_LOOKUP = _build_lookup(ALL_OPS)


@dataclass(frozen=True)
class SpuInsn:
    addr: int
    word: int
    spec: SpuSpec | None

    def field(self, name: str) -> int:
        return FIELDS[name].extract(self.word)

    def fields(self) -> dict[str, int]:
        """Operand fields by name (empty for an unknown word)."""
        if self.spec is None:
            return {}
        return {name: self.field(name) for name in self.spec.field_names}

    @property
    def mnemonic(self) -> str | None:
        """Assembler mnemonic, with the D/E (`bid`, `binze`), C (`syncc`) and P (`hbrp`) suffixes."""
        spec = self.spec
        if spec is None:
            return None
        out = spec.mnemonic
        if "D" in spec.flag_fields and self.field("D"):
            out += "d"
        if "E" in spec.flag_fields and self.field("E"):
            out += "e"
        if "C" in spec.flag_fields and self.field("C"):
            out += "c"
        if "P" in spec.flag_fields and self.field("P"):
            out += "p"
        return out

    @property
    def branch_target(self) -> int | None:
        """LS target of a direct branch (br, bra, brsl, brasl, brz...), else None."""
        spec = self.spec
        if spec is None or "branch" not in spec.flags:
            return None
        if "relative" in spec.flags:
            return (self.addr + self.field("REL16")) & LS_TARGET_MASK
        if "absolute" in spec.flags:
            return self.field("ABS16") & LS_TARGET_MASK
        return None

    @property
    def hint_site(self) -> int | None:
        """LS address of the branch that an hbr/hbra/hbrr instruction hints."""
        spec = self.spec
        if spec is None or "hint" not in spec.flags:
            return None
        return (self.addr + self.field("RO_IND" if spec.mnemonic == "hbr" else "RO")) & LS_TARGET_MASK

    @property
    def hint_target(self) -> int | None:
        """Target given by hbra/hbrr (hbr takes its target from a register)."""
        spec = self.spec
        if spec is None:
            return None
        if spec.mnemonic == "hbra":
            return self.field("ABS16") & LS_TARGET_MASK
        if spec.mnemonic == "hbrr":
            return (self.addr + self.field("REL16")) & LS_TARGET_MASK
        return None


def decode(word: int, addr: int = 0) -> SpuInsn:
    for width, group in _LOOKUP.items():
        spec = group.get(word >> (32 - width))
        if spec is not None:
            return SpuInsn(addr, word, spec)
    return SpuInsn(addr, word, None)


def decode_range(data: bytes, base_addr: int) -> Iterator[SpuInsn]:
    # A trailing partial word is ignored.
    for offset in range(0, len(data) - 3, 4):
        yield decode(struct.unpack_from(">I", data, offset)[0], base_addr + offset)

"""Group `int`: arithmetic, logical, compare, select, multiply, byte, shift and rotate instructions of
the integer lanes, plus the conditional halts (`heq`, `hgt`, `hlgt` and their immediate forms).

The lane semantics live in `runtime/include/tox2/spu_ops.h`; a handler only picks the helper and
turns the immediate into a splatted register, so every immediate form shares the code of its register
form. SPU ISA 1.2, chapters "Constant-formation", "Integer and logical", "Compare, branch, and halt"
and "Shift and rotate".
"""

from __future__ import annotations

from .spu_emit_core import SpuOp, handler, ls_lit, u32

_MASK32 = 0xFFFFFFFF

# Register forms: RT, RA, RB.
_RR = {
    "a": "tox2SpuA", "ah": "tox2SpuAh", "sf": "tox2SpuSf", "sfh": "tox2SpuSfh",
    "cg": "tox2SpuCg", "bg": "tox2SpuBg",
    "mpy": "tox2SpuMpy", "mpyu": "tox2SpuMpyu", "mpyh": "tox2SpuMpyh", "mpys": "tox2SpuMpys",
    "mpyhh": "tox2SpuMpyhh", "mpyhhu": "tox2SpuMpyhhu",
    "avgb": "tox2SpuAvgb", "absdb": "tox2SpuAbsdb", "sumb": "tox2SpuSumb",
    "and": "tox2SpuAnd", "andc": "tox2SpuAndc", "or": "tox2SpuOr", "orc": "tox2SpuOrc", "xor": "tox2SpuXor",
    "nand": "tox2SpuNand", "nor": "tox2SpuNor", "eqv": "tox2SpuEqv",
    "ceqb": "tox2SpuCeqb", "ceqh": "tox2SpuCeqh", "ceq": "tox2SpuCeq",
    "cgtb": "tox2SpuCgtb", "cgth": "tox2SpuCgth", "cgt": "tox2SpuCgt",
    "clgtb": "tox2SpuClgtb", "clgth": "tox2SpuClgth", "clgt": "tox2SpuClgt",
    "shl": "tox2SpuShl", "shlh": "tox2SpuShlh", "rot": "tox2SpuRot", "roth": "tox2SpuRoth",
    "rotm": "tox2SpuRotm", "rothm": "tox2SpuRothm", "rotma": "tox2SpuRotma", "rotmah": "tox2SpuRotmah",
}

# Forms whose result also depends on the old RT (carry-in, accumulator).
_RR_ACC = {
    "addx": "tox2SpuAddx", "cgx": "tox2SpuCgx", "sfx": "tox2SpuSfx", "bgx": "tox2SpuBgx",
    "mpyhha": "tox2SpuMpyhha", "mpyhhau": "tox2SpuMpyhhau",
}

_RRR = {"selb": "tox2SpuSelb", "mpya": "tox2SpuMpya"}

_UNARY = {
    "clz": "tox2SpuClz", "cntb": "tox2SpuCntb", "xsbh": "tox2SpuXsbh", "xshw": "tox2SpuXshw", "xswd": "tox2SpuXswd",
}

# Immediate forms: mnemonic -> (register-form helper, element width of the splat). RI10 forms
# sign-extend I10 to the element width; the byte forms use its low 8 bits; the shifts take the
# sign-extended I7 as the count, of which the helper keeps the low bits. `sfi` computes imm - RA, which
# is `sf` with the immediate in the RB position.
_IMM = {
    "ai": ("tox2SpuA", "W"), "ahi": ("tox2SpuAh", "H"), "sfi": ("tox2SpuSf", "W"), "sfhi": ("tox2SpuSfh", "H"),
    "andi": ("tox2SpuAnd", "W"), "andhi": ("tox2SpuAnd", "H"), "andbi": ("tox2SpuAnd", "B"),
    "ori": ("tox2SpuOr", "W"), "orhi": ("tox2SpuOr", "H"), "orbi": ("tox2SpuOr", "B"),
    "xori": ("tox2SpuXor", "W"), "xorhi": ("tox2SpuXor", "H"), "xorbi": ("tox2SpuXor", "B"),
    "ceqi": ("tox2SpuCeq", "W"), "ceqhi": ("tox2SpuCeqh", "H"), "ceqbi": ("tox2SpuCeqb", "B"),
    "cgti": ("tox2SpuCgt", "W"), "cgthi": ("tox2SpuCgth", "H"), "cgtbi": ("tox2SpuCgtb", "B"),
    # The unsigned compares use the sign-extended immediate, then compare as unsigned.
    "clgti": ("tox2SpuClgt", "W"), "clgthi": ("tox2SpuClgth", "H"), "clgtbi": ("tox2SpuClgtb", "B"),
    "mpyi": ("tox2SpuMpy", "W"),
    # Zero-extended from the sign-extended 16-bit immediate.
    "mpyui": ("tox2SpuMpyu", "U"),
    "shlhi": ("tox2SpuShlh", "H"), "shli": ("tox2SpuShl", "W"), "rothi": ("tox2SpuRoth", "H"),
    "roti": ("tox2SpuRot", "W"), "rothmi": ("tox2SpuRothm", "H"), "rotmi": ("tox2SpuRotm", "W"),
    "rotmahi": ("tox2SpuRotmah", "H"), "rotmai": ("tox2SpuRotma", "W"),
}

_SPLAT = {
    "W": ("tox2SpuSplatW", _MASK32, "0x%08xu"),
    "H": ("tox2SpuSplatH", 0xFFFF, "0x%04xu"),
    "B": ("tox2SpuSplatB", 0xFF, "0x%02xu"),
    "U": ("tox2SpuSplatW", 0xFFFF, "0x%08xu"),
}


def _set(op: SpuOp, expr: str, reg: str = "RT") -> list[str]:
    return ["%s = %s;" % (op.gpr(op.f(reg)), expr)]


def _splat(kind: str, value: int) -> str:
    fn, mask, fmt = _SPLAT[kind]
    return "%s(%s)" % (fn, fmt % (value & mask))


def _make_rr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB"))))
    handler(name)(emit)


def _make_rr_acc(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")), op.gpr(op.f("RT"))))
    handler(name)(emit)


def _make_rrr(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s, %s)" % (fn, op.gpr(op.f("RA")), op.gpr(op.f("RB")), op.gpr(op.f("RC"))), "RT4")
    handler(name)(emit)


def _make_unary(name: str, fn: str):
    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s)" % (fn, op.gpr(op.f("RA"))))
    handler(name)(emit)


def _make_imm(name: str, fn: str, kind: str):
    imm_field = "I7" if name.startswith(("shl", "rot")) else "I10"

    def emit(op: SpuOp) -> list[str]:
        return _set(op, "%s(%s, %s)" % (fn, op.gpr(op.f("RA")), _splat(kind, op.f(imm_field))))
    handler(name)(emit)


for _name, _fn in _RR.items():
    _make_rr(_name, _fn)
for _name, _fn in _RR_ACC.items():
    _make_rr_acc(_name, _fn)
for _name, _fn in _RRR.items():
    _make_rrr(_name, _fn)
for _name, _fn in _UNARY.items():
    _make_unary(_name, _fn)
for _name, (_fn, _kind) in _IMM.items():
    _make_imm(_name, _fn, _kind)


# --- Immediate loads --------------------------------------------------------------------------

@handler("il")
def emit_il(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("I16")))


@handler("ilh")
def emit_ilh(op: SpuOp) -> list[str]:
    return _set(op, _splat("H", op.f("U16")))


@handler("ilhu")
def emit_ilhu(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("U16") << 16))


@handler("ila")
def emit_ila(op: SpuOp) -> list[str]:
    return _set(op, _splat("W", op.f("I18")))


@handler("iohl")
def emit_iohl(op: SpuOp) -> list[str]:
    rt = op.gpr(op.f("RT"))
    return _set(op, "tox2SpuOr(%s, %s)" % (rt, _splat("W", op.f("U16"))))


# --- Conditional halts ------------------------------------------------------------------------

@handler("heq", "hgt", "hlgt", "heqi", "hgti", "hlgti")
def emit_halt(op: SpuOp) -> list[str]:
    # Compares the preferred words; the immediate forms use the sign-extended I10.
    a = op.pref(op.f("RA"))
    if op.name.endswith("i"):
        b = u32(op.f("I10"))
    else:
        b = op.pref(op.f("RB"))
    if op.name.startswith("heq"):
        cond = "%s == %s" % (a, b)
    elif op.name.startswith("hgt"):
        cond = "(int32_t)%s > (int32_t)%s" % (a, b)
    else:
        cond = "%s > %s" % (a, b)
    return ["if (%s) tox2SpuHalt(ctx, ls, %s);" % (cond, ls_lit(op.addr))]

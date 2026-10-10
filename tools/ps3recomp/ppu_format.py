"""Text formatting of decoded PPU instructions, with optional simplified mnemonics."""

from __future__ import annotations

from .ppu import Insn

_GPR = frozenset({"rD", "rS", "rA", "rB"})
_FPR = frozenset({"frD", "frS", "frA", "frB", "frC"})
_VR = frozenset({"vD", "vS", "vA", "vB", "vC"})
_CRF = frozenset({"crfD", "crfS"})
_SIGNED_HEX = frozenset({"SIMM", "D", "DS"})
_BRANCH_TARGET = frozenset({"LI", "BD"})

_CC_TRUE = ("lt", "gt", "eq", "so")
_CC_FALSE = ("ge", "le", "ne", "ns")
_BRANCH_SUFFIX = {"bc": "", "bclr": "lr", "bcctr": "ctr"}


def _hex(value: int) -> str:
    return f"-{-value:#x}" if value < 0 else f"{value:#x}"


def _target(insn: Insn, field: str) -> str:
    offset = insn.field(field)
    return f"{(offset if insn.field('AA') else insn.addr + offset) & 0xFFFFFFFF:#x}"


def _operand(insn: Insn, name: str) -> str:
    if name.endswith("(rA)"):
        return f"{_hex(insn.field(name[:-4]))}(r{insn.field('rA')})"
    if name in _GPR:
        return f"r{insn.field(name)}"
    if name in _FPR:
        return f"f{insn.field(name)}"
    if name in _VR:
        return f"v{insn.field(name)}"
    if name in _CRF:
        return f"cr{insn.field(name)}"
    if name in _SIGNED_HEX:
        return _hex(insn.field(name))
    if name == "UIMM":
        return f"{insn.field(name):#x}"
    if name == "FXM":
        # mfocrf reads one CR field; print it as crN when exactly one mask bit is set.
        fxm = insn.field(name)
        return f"cr{8 - fxm.bit_length()}" if fxm.bit_count() == 1 else str(fxm)
    if name in _BRANCH_TARGET:
        return _target(insn, name)
    return str(insn.field(name))


def _join(mnemonic: str, operands: list[str]) -> str:
    return f"{mnemonic} {','.join(operands)}" if operands else mnemonic


def _format_raw(insn: Insn) -> str:
    spec = insn.spec
    return _join(insn.mnemonic, [_operand(insn, name) for name in spec.operands])


def _simplify_branch(insn: Insn) -> str | None:
    name = insn.spec.name
    bo = insn.field("BO")
    bi = insn.field("BI")
    link = "l" if insn.field("LK") else ""
    absolute = "a" if name == "bc" and insn.field("AA") else ""
    operands: list[str] = []

    if bo == 20:
        base = {"bc": "b", "bclr": "blr", "bcctr": "bctr"}[name] + link + absolute
    elif (bo & 0x1C) in (0x0C, 0x04):
        cc = (_CC_TRUE if (bo & 0x1C) == 0x0C else _CC_FALSE)[bi % 4]
        base = "b" + cc + _BRANCH_SUFFIX[name] + link + absolute
        if bi // 4:
            operands.append(f"cr{bi // 4}")
    elif name == "bc" and (bo & 0x16) in (0x10, 0x12):
        base = ("bdnz" if (bo & 0x16) == 0x10 else "bdz") + link + absolute
    else:
        return None
    if name == "bc":
        operands.append(_target(insn, "BD"))
    return _join(base, operands)


def _simplify_rlwinm(insn: Insn, dot: str) -> str | None:
    sh, mb, me = insn.field("SH"), insn.field("MB"), insn.field("ME")
    ra, rs = f"r{insn.field('rA')}", f"r{insn.field('rS')}"
    if mb == 0 and me == 31:
        name, arg = "rotlwi", sh
    elif mb == 0 and me == 31 - sh and sh > 0:
        name, arg = "slwi", sh
    elif me == 31 and sh == 32 - mb and mb > 0:
        name, arg = "srwi", mb
    elif sh == 0 and me == 31:
        name, arg = "clrlwi", mb
    else:
        return None
    return _join(name + dot, [ra, rs, str(arg)])


def _simplify_rldic(insn: Insn, dot: str) -> str | None:
    sh, mb = insn.field("SH64"), insn.field("MB64")
    ra, rs = f"r{insn.field('rA')}", f"r{insn.field('rS')}"
    if insn.spec.name == "rldicl":
        if sh == 0:
            name, arg = "clrldi", mb
        elif sh + mb == 64 and mb > 0:
            name, arg = "srdi", mb
        elif mb == 0:
            name, arg = "rotldi", sh
        else:
            return None
    elif sh + mb == 63:
        name, arg = "sldi", sh
    else:
        return None
    return _join(name + dot, [ra, rs, str(arg)])


_SPR_MFSPR = {1: "mfxer", 8: "mflr", 9: "mfctr"}
_SPR_MTSPR = {1: "mtxer", 8: "mtlr", 9: "mtctr"}
_CMP_NAMES = {
    "cmpi": ("cmpwi", "cmpdi"), "cmpli": ("cmplwi", "cmpldi"),
    "cmp": ("cmpw", "cmpd"), "cmpl": ("cmplw", "cmpld"),
}


def _simplify(insn: Insn) -> str | None:
    name = insn.spec.name
    dot = "." if insn.mnemonic.endswith(".") else ""

    if name == "ori":
        if insn.field("rS") == 0 and insn.field("rA") == 0 and insn.field("UIMM") == 0:
            return "nop"
    elif name in ("addi", "addis"):
        if insn.field("rA") == 0:
            return _join("li" if name == "addi" else "lis",
                         [_operand(insn, "rD"), _operand(insn, "SIMM")])
    elif name in ("or", "nor"):
        if insn.field("rS") == insn.field("rB"):
            return _join(("mr" if name == "or" else "not") + dot,
                         [_operand(insn, "rA"), _operand(insn, "rS")])
    elif name == "vor":
        if insn.field("vA") == insn.field("vB"):
            return _join("vmr", [_operand(insn, "vD"), _operand(insn, "vA")])
    elif name == "mfspr":
        mnemonic = _SPR_MFSPR.get(insn.field("SPR"))
        if mnemonic:
            return _join(mnemonic, [_operand(insn, "rD")])
    elif name == "mtspr":
        mnemonic = _SPR_MTSPR.get(insn.field("SPR"))
        if mnemonic:
            return _join(mnemonic, [_operand(insn, "rS")])
    elif name == "mtcrf":
        if insn.field("CRM") == 0xFF:
            return _join("mtcr", [_operand(insn, "rS")])
    elif name == "sync":
        return {0: "sync", 1: "lwsync"}.get(insn.field("L_sync"))
    elif name in _CMP_NAMES:
        mnemonic = _CMP_NAMES[name][insn.field("L")]
        operands = [] if insn.field("crfD") == 0 else [_operand(insn, "crfD")]
        operands += [_operand(insn, n) for n in insn.spec.operands[2:]]
        return _join(mnemonic, operands)
    elif name == "rlwinm":
        return _simplify_rlwinm(insn, dot)
    elif name in ("rldicl", "rldicr"):
        return _simplify_rldic(insn, dot)
    elif name in _BRANCH_SUFFIX:
        return _simplify_branch(insn)
    return None


def format_insn(insn: Insn, simplify: bool = True) -> str:
    if insn.spec is None:
        return f".long 0x{insn.word:08x}"
    if simplify:
        text = _simplify(insn)
        if text is not None:
            return text
    return _format_raw(insn)

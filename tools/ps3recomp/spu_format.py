"""Text formatting of decoded SPU instructions.

Registers print as `$0..$127` (no `$lr`/`$sp` aliases), channels as `$ch<N>`, branch and hint
targets as absolute LS addresses (`0x%05x`). The output is `mnemonic op,op,...` with one space after
the mnemonic and no spaces between operands, so a listing parser can split it by comma.
"""

from __future__ import annotations

from .spu import SpuInsn

_REGISTERS = frozenset({"RT", "RT4", "RA", "RB", "RC"})
_SIGNED = frozenset({"I7", "I10", "I10x16", "I16"})
_UNSIGNED_HEX = frozenset({"U7", "U16", "I18", "STOP"})
# The assembler operand of the float/integer conversions is `scale`, not the I8 field (ISA 1.2, p.220-223):
# scale = 173 - I8 for cflts/cfltu and 155 - I8 for csflt/cuflt, as an 8-bit unsigned value.
_SCALE_BASE = {"cflts": 173, "cfltu": 173, "csflt": 155, "cuflt": 155}


def _operand(insn: SpuInsn, name: str) -> str:
    if name.endswith("(RA)"):
        return f"{insn.field(name[:-4])}(${insn.field('RA')})"
    if name in _REGISTERS:
        return f"${insn.field(name)}"
    if name in _SIGNED:
        return str(insn.field(name))
    if name == "I8":
        return str((_SCALE_BASE[insn.spec.mnemonic] - insn.field(name)) & 0xFF)
    if name in _UNSIGNED_HEX:
        return f"{insn.field(name):#x}"
    if name == "CA":
        return f"$ch{insn.field(name)}"
    if name == "SA":
        return f"$spr{insn.field(name)}"
    if name in ("REL16", "ABS16"):
        # lqa/stqa/lqr/stqr use the same address computation as the branches.
        target = insn.addr + insn.field(name) if name == "REL16" else insn.field(name)
        return f"{target & 0x3FFFC:#07x}"
    if name in ("RO", "RO_IND"):
        return f"{insn.hint_site:#07x}"
    raise ValueError(f"no formatting for operand {name!r}")


def format_insn(insn: SpuInsn) -> str:
    if insn.spec is None:
        return f".long {insn.word:#010x}"
    operands = ",".join(_operand(insn, name) for name in insn.spec.operands)
    return f"{insn.mnemonic} {operands}" if operands else insn.mnemonic


def format_line(insn: SpuInsn) -> str:
    """Listing line: LS address, raw word, text."""
    return f"{insn.addr:05x}: {insn.word:08x}  {format_insn(insn)}"

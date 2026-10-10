"""Syntax normalisation between Ghidra's listing text and our decoder's text."""

from __future__ import annotations

import re
from collections.abc import Callable
from functools import lru_cache

from .ppu import ALL_OPS, Insn
from .ppu_format import format_insn

Token = str | int

# Ghidra prints a zero base register (rA = 0 in an rA|0 slot) as a bare "0".
_MEM_OPERAND = re.compile(r"^(.*)\((r\d+|0)\)$")
_INT = re.compile(r"^-?(?:0x[0-9a-f]+|\d+)$")
_INT_MODULI = tuple(1 << k for k in (16, 32, 64))

Rewrite = Callable[[tuple[Token, ...]], tuple[str, tuple[Token, ...]]]

# Ghidra prints addi/addic/addic./addis with a negative immediate as subi/... with the magnitude.
_SUB_TO_ADD = {"subi": "addi", "subic": "addic", "subic.": "addic.", "subis": "addis"}

# Ghidra names the SPRs of mfspr/mtspr/mftb; we print the number.
_SPR_NUMBERS = {"lr": 8, "ctr": 9, "tblr": 268}

_CR_BIT_NAMES = ("lt", "gt", "eq", "so")
_CR_BIT = re.compile(r"^(?:4\*cr(\d)\+)?(lt|gt|eq|so)$")
_CR_LOGICAL = ("crand", "crandc", "creqv", "crnand", "crnor", "cror", "crorc", "crxor")

_RA_OR_ZERO_FIRST = frozenset({"rD", "rS", "frD", "frS", "vD", "vS"})


def _sub_to_add(name: str) -> Rewrite:
    def rewrite(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
        *head, imm = tokens
        if not isinstance(imm, int):
            return name, tokens
        return _SUB_TO_ADD[name], (*head, -imm)
    return rewrite


def _spr_number(name: str) -> Rewrite:
    def rewrite(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
        return name, tuple(_SPR_NUMBERS.get(t, t) if isinstance(t, str) else t for t in tokens)
    return rewrite


def _cr_bit_number(token: Token) -> Token:
    m = _CR_BIT.match(token) if isinstance(token, str) else None
    return 4 * int(m.group(1) or 0) + _CR_BIT_NAMES.index(m.group(2)) if m else token


def _cr_logical(name: str) -> Rewrite:
    def rewrite(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
        return name, tuple(_cr_bit_number(t) for t in tokens)
    return rewrite


def _zero_base(name: str) -> Rewrite:
    # rA is the second-to-last operand of the indexed forms "rA,rB" and "rT,rA,rB".
    def rewrite(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
        if len(tokens) >= 2 and tokens[-2] == 0:
            return name, (*tokens[:-2], "r0", tokens[-1])
        return name, tokens
    return rewrite


def _sc_level(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
    # We omit the LEV operand of `sc`, Ghidra prints it. A non-zero level stays a mismatch.
    return "sc", () if tokens == (0,) else tokens


def _trap_to_zero(name: str) -> Rewrite:
    # Ghidra omits the TO operand when it is 0.
    def rewrite(tokens: tuple[Token, ...]) -> tuple[str, tuple[Token, ...]]:
        return name, (0, *tokens) if len(tokens) == 2 else tokens
    return rewrite


def _indexed_rewrites() -> dict[str, Rewrite]:
    rewrites: dict[str, Rewrite] = {}
    for spec in ALL_OPS:
        ops = spec.operands
        indexed = ops == ("rA", "rB") or (
            len(ops) == 3 and ops[0] in _RA_OR_ZERO_FIRST and ops[1:] == ("rA", "rB"))
        if spec.form == "X" and spec.primary == 31 and indexed:
            rewrites[spec.name] = _zero_base(spec.name)
    return rewrites


# Per normalised Ghidra mnemonic: rewrite into the form we print. Each entry must be a pure
# syntax change; the operands still have to equal ours afterwards.
GHIDRA_REWRITES: dict[str, Rewrite] = {
    **{name: _sub_to_add(name) for name in _SUB_TO_ADD},
    **{name: _spr_number(name) for name in ("mfspr", "mtspr", "mftb")},
    **{name: _cr_logical(name) for name in _CR_LOGICAL},
    **_indexed_rewrites(),
    "sc": _sc_level,
    "twi": _trap_to_zero("twi"),
    "tdi": _trap_to_zero("tdi"),
}

# (our raw spec name or None if we cannot decode, normalised Ghidra mnemonic or None if Ghidra
# cannot decode) -> reason. Ghidra-side limitations only, never our bugs.
KNOWN_GHIDRA_DIFFERENCES: dict[tuple[str | None, str | None], str] = {}


def normalize_mnemonic(mnemonic: str) -> str:
    mnemonic = mnemonic.lower()
    # Branch prediction hint, e.g. "beq+".
    if mnemonic.endswith(("+", "-")):
        mnemonic = mnemonic[:-1]
    return mnemonic


def _parse_int(text: str) -> int:
    negative = text.startswith("-")
    body = text[1:] if negative else text
    value = int(body, 16) if body.startswith("0x") else int(body, 10)
    return -value if negative else value


def _token(text: str) -> Token:
    return _parse_int(text) if _INT.match(text) else text


def _drop_cr0(tokens: tuple[Token, ...]) -> tuple[Token, ...]:
    return tokens[1:] if tokens and tokens[0] == "cr0" else tokens


@lru_cache(maxsize=1 << 16)
def normalize(mnemonic: str, operands: str | None) -> tuple[str, tuple[Token, ...]]:
    tokens: list[Token] = []
    text = "".join((operands or "").lower().split())
    for part in text.split(",") if text else ():
        m = _MEM_OPERAND.match(part)
        if m:
            if m.group(1):
                tokens.append(_token(m.group(1)))
            tokens.append("r0" if m.group(2) == "0" else m.group(2))
        else:
            tokens.append(_token(part))
    return normalize_mnemonic(mnemonic), _drop_cr0(tuple(tokens))


def _ints_equal(a: int, b: int) -> bool:
    # Ghidra prints some fields sign-extended or zero-extended, we print them as encoded.
    return a == b or any(a % m == b % m for m in _INT_MODULI)


def _tokens_equal(a: tuple[Token, ...], b: tuple[Token, ...]) -> bool:
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if isinstance(x, int) and isinstance(y, int):
            if not _ints_equal(x, y):
                return False
        elif x != y:
            return False
    return True


def _normalize_text(text: str) -> tuple[str, tuple[Token, ...]]:
    mnemonic, _, operands = text.partition(" ")
    return normalize(mnemonic, operands)


def ghidra_form(mnemonic: str, operands: str | None) -> tuple[str, tuple[Token, ...]]:
    name, tokens = normalize(mnemonic, operands)
    rewrite = GHIDRA_REWRITES.get(name)
    if rewrite is not None:
        name, tokens = rewrite(tokens)
        tokens = _drop_cr0(tokens)
    return name, tokens


def compare(insn: Insn, ghidra_mnemonic: str, ghidra_operands: str | None) -> str:
    """Returns "match", "mnemonic_mismatch" or "operand_mismatch" for a decoded `insn`."""
    g_name, g_tokens = ghidra_form(ghidra_mnemonic, ghidra_operands)
    same_mnemonic = False
    for simplify in (True, False):
        name, tokens = _normalize_text(format_insn(insn, simplify))
        if name != g_name:
            continue
        if _tokens_equal(tokens, g_tokens):
            return "match"
        same_mnemonic = True
    return "operand_mismatch" if same_mnemonic else "mnemonic_mismatch"

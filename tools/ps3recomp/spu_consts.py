"""Forward constant propagation over one emitted SPU function.

The emitter writes one instruction at a time, so a handler cannot see that a register holds the same
constant on every path that reaches it. This pass computes that, on decoded instructions, with the
successor rules of the lowering (`spu_lower_stackfree.py`, `spu_emit_core.py`): a direct branch to an
owned address is a `goto`; anything else leaves the function and the register file is not tracked.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping

from .spu import SpuInsn
from .spu_flow import WORD_OPS, SpuCode, flow_of, word_result, written_reg

# Four words of one register in big-endian element order: index 0 is the preferred word.
Words = tuple[int, int, int, int]

# Emitted code calls a runtime hook that can change any register (channels, stop and signal, SPR moves)
# or tests a channel (bisled): nothing is known after them.
KILL_ALL = frozenset({"stop", "stopd", "rdch", "rchcnt", "wrch", "mfspr", "mtspr", "fscrrd", "fscrwr", "bisled"})

_NONE: Mapping[int, Words] = {}


def fsmbi_words(mask: int) -> Words:
    """fsmbi: architected byte k is 0xff when bit 15 - k of the 16-bit mask is set (tox2SpuFsmb)."""
    out = []
    for word in range(4):
        value = 0
        for k in range(4 * word, 4 * word + 4):
            value = (value << 8) | (0xFF if (mask >> (15 - k)) & 1 else 0)
        out.append(value)
    return (out[0], out[1], out[2], out[3])


class FunctionConstants:
    """Registers with a known full value before each instruction of one function."""

    def __init__(self, states: dict[int, dict[int, Words]]) -> None:
        self._states = states

    def at(self, addr: int) -> Mapping[int, Words]:
        """Registers that hold the same full value on every path that reaches `addr`, before the
        instruction at `addr` runs. Empty for an address that no seed reaches."""
        return self._states.get(addr, _NONE)


def _transfer(insn: SpuInsn, state: Mapping[int, Words], handled: Callable[[str], bool]) -> dict[int, Words]:
    name = insn.spec.mnemonic
    # An unhandled mnemonic emits a noreturn tox2SpuUnhandled, so nothing carries past it.
    if name in KILL_ALL or not handled(name):
        return {}
    reg = written_reg(insn)
    if reg is None:
        return dict(state)
    out = {r: w for r, w in state.items() if r != reg}
    if name == "fsmbi":
        out[reg] = fsmbi_words(insn.field("U16"))
    elif name in WORD_OPS:
        words = [word_result(insn, lambda r, i=i: state[r][i] if r in state else None) for i in range(4)]
        if None not in words:
            out[reg] = (words[0], words[1], words[2], words[3])
    return out


def _successors(code: SpuCode, owned: frozenset[int], addr: int) -> list[int]:
    if not code.valid(addr):
        return []
    insn = code.insn(addr)
    flow = flow_of(insn)
    out = []
    if flow.kind in ("jump", "cond_jump", "call") and flow.target in owned:
        out.append(flow.target)
    # bisled is the one call that continues at the next word when it is not taken.
    is_call = flow.kind in ("call", "indirect_call")
    if flow.falls_through and (not is_call or insn.spec.mnemonic == "bisled") and addr + 4 in owned:
        out.append(addr + 4)
    return out


def function_constants(code: SpuCode, owned: frozenset[int], seeds: Iterable[int],
                       handled: Callable[[str], bool]) -> FunctionConstants:
    """Constants that hold on every path to each instruction of a function that owns `owned`.

    A function is entered only at its dispatchable blocks (`seeds`), where the register file is
    unknown, so a seed never gains a register. Return sites, indirect jumps and transfers to words
    that another function owns have no successor here.
    """
    states: dict[int, dict[int, Words]] = {}
    work = []
    for seed in seeds:
        if seed in owned:
            states[seed] = {}
            work.append(seed)
    while work:
        addr = work.pop()
        after = _transfer(code.insn(addr), states[addr], handled) if code.valid(addr) else {}
        for succ in _successors(code, owned, addr):
            old = states.get(succ)
            if old is None:
                states[succ] = dict(after)
                work.append(succ)
                continue
            kept = {r: w for r, w in old.items() if after.get(r) == w}
            if len(kept) != len(old):
                states[succ] = kept
                work.append(succ)
    return FunctionConstants(states)

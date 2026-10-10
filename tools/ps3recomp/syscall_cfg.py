"""lv2 syscall number resolution over the recovered control-flow graph."""

from __future__ import annotations

import struct
from collections import defaultdict
from collections.abc import Mapping
from dataclasses import dataclass

from .elf import ElfFile
from .jumptab import JumpTable
from .partition import RecoveredFunction, intra_successors
from .ppu_flow import CodeImage, flow_of, written_gprs

# `sc` with LEV = 0, the lv2 syscall form. The decoder's `sc` entry matches the primary opcode
# only, so it also hits float constants and ASCII data; compare the whole word.
SC_WORD = 0x44000002

_SYSCALL_REG = 11
_NODE_LIMIT = 512
_CALL_KINDS = frozenset({"call", "cond_call", "indirect_call"})
# Order in which a non-numeric outcome is reported when several paths fail.
_REASON_ORDER = ("limit", "call", "r11_written", "entry")


@dataclass(frozen=True)
class SyscallSite:
    addr: int
    function: int | None
    number: int | None
    reason: str | None


def _predecessors(image: CodeImage, fn: RecoveredFunction,
                  jump_tables: Mapping[int, JumpTable]) -> dict[int, list[int]]:
    preds: dict[int, list[int]] = defaultdict(list)
    for addr in sorted(fn.addrs):
        for succ in intra_successors(image, addr, jump_tables):
            if succ in fn.addrs:
                preds[succ].append(addr)
    return preds


def _resolve(image: CodeImage, fn: RecoveredFunction, site: int,
             preds: Mapping[int, list[int]]) -> tuple[int | None, str | None]:
    numbers: set[int] = set()
    reasons: set[str] = set()
    # Nodes are instructions whose r11 effect is still to be examined; the site itself is not.
    visited: set[int] = set()
    stack = list(preds.get(site, ()))
    if site == fn.entry or not stack:
        reasons.add("entry")
    while stack:
        addr = stack.pop()
        if addr in visited:
            continue
        if len(visited) >= _NODE_LIMIT:
            return None, "limit"
        visited.add(addr)
        insn = image.insn(addr)
        if insn.spec.name == "addi" and insn.field("rD") == _SYSCALL_REG \
                and insn.field("rA") == 0:
            numbers.add(insn.field("SIMM"))
            continue
        if _SYSCALL_REG in written_gprs(insn):
            reasons.add("r11_written")
            continue
        if flow_of(insn).kind in _CALL_KINDS:
            reasons.add("call")
            continue
        if addr == fn.entry:
            reasons.add("entry")
        stack.extend(preds.get(addr, ()))
        if addr != fn.entry and not preds.get(addr):
            # Reached only from outside the owned set (e.g. through a shared tail).
            reasons.add("entry")
    if reasons:
        return None, next(r for r in _REASON_ORDER if r in reasons)
    if len(numbers) == 1:
        return next(iter(numbers)), None
    return None, "conflict"


def resolve_syscalls(image: CodeImage, functions: Mapping[int, RecoveredFunction],
                     jump_tables: Mapping[int, JumpTable]) -> list[SyscallSite]:
    owner = {addr: entry for entry, fn in functions.items() for addr in fn.addrs}
    pred_cache: dict[int, dict[int, list[int]]] = {}
    sites: list[SyscallSite] = []
    for index, word in enumerate(image.words):
        if word != SC_WORD:
            continue
        addr = image.start + 4 * index
        entry = owner.get(addr)
        if entry is None:
            sites.append(SyscallSite(addr, None, None, "unowned"))
            continue
        fn = functions[entry]
        if entry not in pred_cache:
            pred_cache[entry] = _predecessors(image, fn, jump_tables)
        number, reason = _resolve(image, fn, addr, pred_cache[entry])
        sites.append(SyscallSite(addr, entry, number, reason))
    return sites


def count_sites_outside_text(elf: ElfFile, text_range: tuple[int, int]) -> int:
    """`sc` words in executable segments outside the recovered code range (counted only)."""
    start, end = text_range
    count = 0
    for seg in elf.exec_segments():
        size = seg.p_filesz // 4 * 4
        words = struct.unpack(">%dI" % (size // 4), elf.read_va(seg.p_vaddr, size))
        for i, word in enumerate(words):
            if word == SC_WORD and not start <= seg.p_vaddr + 4 * i < end:
                count += 1
    return count

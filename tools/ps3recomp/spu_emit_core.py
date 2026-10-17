"""SPU emitter core: handler registry, `SpuOp`, per-function emission and the control group.

The control group decides *what* a branch does (condition, target, link register, interrupt bits);
the lowering module (`spu_lower_stackfree.py`) decides *how* the transfer is made in C. Nothing in
this file names a TOX2_SPU_SF_* macro, so a second lowering only adds a module and a header.

Semantics handlers of the other groups live in `spu_emit_ops_<group>.py`. They register by mnemonic,
return C statements that use `ctx`, `ls` and the helpers of `spu_ops.h`, and never transfer control;
a hook that asks the thread to leave is followed by `op.halt()`.
"""

from __future__ import annotations

import functools
import importlib
import importlib.util
from collections import Counter
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field

from . import spu
from .spu import LS_TARGET_MASK, SpuInsn
from .spu_consts import FunctionConstants, Words, function_constants
from .spu_flow import STOP_TERMINATE_CODES, SpuCode, flow_of
from .spu_format import format_insn

_INDENT = "    "
_SITE_EXAMPLES = 5
_MASK32 = 0xFFFFFFFF

# Control-group instructions the core lowers itself. Every other mnemonic needs a registered handler.
_DIRECT_BRANCHES = frozenset({"br", "bra", "brsl", "brasl", "brz", "brnz", "brhz", "brhnz"})
_INDIRECT_BRANCHES = frozenset({"bi", "iret", "bisl", "bisled", "biz", "binz", "bihz", "bihnz"})
# No architectural effect on a private local store: hints, no-ops and the synchronisation instructions.
_NO_CODE = frozenset({"hbr", "hbra", "hbrr", "nop", "lnop", "sync", "dsync"})
CORE_MNEMONICS = _DIRECT_BRANCHES | _INDIRECT_BRANCHES | _NO_CODE | {"stop"}

_FIELD_TESTS = {
    "brz": "== 0", "brnz": "!= 0", "bihz": "== 0", "bihnz": "!= 0", "biz": "== 0", "binz": "!= 0",
    "brhz": "== 0", "brhnz": "!= 0",
}
_HALFWORD_TESTS = frozenset({"brhz", "brhnz", "bihz", "bihnz"})


def u32(value: int) -> str:
    return "0x%08xu" % (value & _MASK32)


def ls_lit(addr: int) -> str:
    """An LS address as a C literal (18 bits)."""
    return "0x%05xu" % addr


@dataclass
class EmitStats:
    edges: Counter = field(default_factory=Counter)
    unhandled: Counter = field(default_factory=Counter)
    unhandled_sites: dict[str, list[int]] = field(default_factory=dict)
    # Flow that runs into a word that is not code: (site, next address).
    invalid_flow: list[tuple[int, int]] = field(default_factory=list)
    # Direct transfers to code that another function owns without it being a dispatchable block.
    unlisted_targets: list[tuple[int, int]] = field(default_factory=list)
    mnemonics: Counter = field(default_factory=Counter)
    labels: int = 0
    loop_hooks: int = 0
    # Shuffles whose control was written as a literal (set by the handler through `SpuOp.folded_control`).
    folded_shufb: int = 0
    functions: int = 0
    instructions: int = 0


@dataclass(frozen=True)
class FunctionRecord:
    entry: int
    name: str
    instructions: int
    text: str


class ImageModel:
    """What emission needs to know about one image: its code, its functions and the dispatchable set.

    `functions` maps a function entry to the addresses it owns; `dispatchable` lists the blocks the
    runtime can enter (function entries are always added). `lowering_of(entry)` selects the lowering
    of a function, which is how a per-image or per-function override takes effect.
    """

    def __init__(self, name: str, code: SpuCode, functions: Mapping[int, Iterable[int]],
                 dispatchable: Iterable[int], lowering_of: Callable[[int], "Lowering"],
                 *, comments: bool = False) -> None:
        self.name = name
        self.code = code
        self.comments = comments
        self.lowering_of = lowering_of
        self.functions = {entry: tuple(sorted(addrs)) for entry, addrs in sorted(functions.items())}
        self.owner: dict[int, int] = {}
        for entry, addrs in self.functions.items():
            if entry not in addrs:
                raise ValueError("%s: function 0x%05x does not own its entry" % (name, entry))
            for addr in addrs:
                if addr in self.owner:
                    raise ValueError("%s: 0x%05x is owned by functions 0x%05x and 0x%05x"
                                     % (name, addr, self.owner[addr], entry))
                self.owner[addr] = entry
        entries = set(self.functions)
        for addr in dispatchable:
            if addr not in self.owner:
                raise ValueError("%s: dispatchable block 0x%05x is not owned code" % (name, addr))
            entries.add(addr)
        self.dispatchable = frozenset(entries)

    def func_name(self, entry: int) -> str:
        return "spu_%s_%05x" % (self.name, entry)

    def entry_cases(self, entry: int) -> list[int]:
        """Dispatchable blocks that function `entry` owns, ascending."""
        return sorted(a for a in self.dispatchable if self.owner[a] == entry)


@dataclass
class FunctionView:
    """The function being emitted, as the lowering sees it."""

    model: ImageModel
    entry: int
    owned: frozenset[int]
    stats: EmitStats
    targeted: set[int] = field(default_factory=set)


class SpuOp:
    """What a handler sees: the decoded instruction plus C-expression helpers."""

    def __init__(self, insn: SpuInsn, lowering: "Lowering", known: Mapping[int, Words] | None = None) -> None:
        self.insn = insn
        self.addr = insn.addr
        self.word = insn.word
        self.name = insn.spec.mnemonic
        self.lowering = lowering
        self.unhandled_key: str | None = None
        self.folded_control = False
        self._known = {} if known is None else known

    def known(self, reg: int) -> Words | None:
        """Full value of `reg` before this instruction when it is the same emit-time constant on every path
        that reaches it (spu_consts); None otherwise."""
        return self._known.get(reg)

    def f(self, field_name: str) -> int:
        return self.insn.field(field_name)

    def gpr(self, n: int) -> str:
        return "ctx->gpr[%d]" % n

    def pref(self, n: int) -> str:
        """Preferred-slot word of a register."""
        return "tox2SpuPreferred(ctx->gpr[%d])" % n

    def halt(self) -> str:
        """Statement that makes the thread leave (after a hook returned false)."""
        return self.lowering.halt_stmt()

    def unsupported(self, reason: str) -> None:
        self.unhandled_key = "%s:%s" % (self.name, reason)
        return None


class Lowering:
    """Interface of a control-flow lowering. `spu_lower_stackfree.StackFreeLowering` is the only one."""

    name = ""
    header = ""

    def link_stmt(self, reg: int, ret: int) -> str:
        raise NotImplementedError

    def halt_stmt(self) -> str:
        raise NotImplementedError

    def direct(self, view: FunctionView, site: int, target: int, link: int | None = None,
               *, fall_through: bool = False) -> list[str]:
        """Transfer to a target known at emit time; writes the link register first when given."""
        raise NotImplementedError

    def computed(self, view: FunctionView, site: int, target_expr: str, link: int | None = None) -> list[str]:
        """Transfer to a target held in a C expression that was evaluated before the link write."""
        raise NotImplementedError

    def render_function(self, view: FunctionView, name: str, title: str | None, entry: int,
                        cases: Sequence[int], blocks: Sequence[tuple[int, str | None, list[str]]]) -> str:
        raise NotImplementedError


_LOWERING_MODULES = {"stackfree": "spu_lower_stackfree"}


def lowering_names() -> tuple[str, ...]:
    return tuple(_LOWERING_MODULES)


@functools.lru_cache(maxsize=None)
def get_lowering(name: str) -> Lowering:
    module_name = _LOWERING_MODULES.get(name)
    if module_name is None:
        raise ValueError("lowering not implemented: %s" % name)
    module = importlib.import_module("%s.%s" % (__package__ or "ps3recomp", module_name))
    return module.LOWERING


Handler = Callable[[SpuOp], "list[str] | None"]

_REGISTRY: dict[str, Handler] = {}

# Groups that have (or will have) a handler module. A module may register mnemonics of other groups
# (the channel module takes `mfspr`, `mtspr` and the FPSCR moves), so only unknown names and the
# mnemonics the core lowers are rejected.
HANDLER_GROUPS = ("int", "perm", "mem", "chan", "fp_single", "fp_double")


def handler(*names: str) -> Callable[[Handler], Handler]:
    for name in names:
        if name not in spu.SPECS_BY_NAME:
            raise ValueError("unknown mnemonic %r" % name)
        if name in CORE_MNEMONICS:
            raise ValueError("%r is lowered by the emitter core" % name)
    if len(set(names)) != len(names):
        raise ValueError("mnemonic listed twice in one registration: %s" % ", ".join(names))

    def register(fn: Handler) -> Handler:
        for name in names:
            if name in _REGISTRY:
                raise ValueError("duplicate handler for %r" % name)
        for name in names:
            _REGISTRY[name] = fn
        return fn

    return register


def load_handlers() -> dict[str, Handler]:
    """Import every existing `spu_emit_ops_<group>` module and return the registry."""
    package = __package__ or "ps3recomp"
    for group in HANDLER_GROUPS:
        module_name = "%s.spu_emit_ops_%s" % (package, group)
        if importlib.util.find_spec(module_name) is not None:
            importlib.import_module(module_name)
    return dict(_REGISTRY)


@functools.lru_cache(maxsize=1)
def _default_handlers() -> dict[str, Handler]:
    return load_handlers()


def is_handled(name: str, handlers: Mapping[str, Handler] | None = None) -> bool:
    return name in CORE_MNEMONICS or name in (_default_handlers() if handlers is None else handlers)


def _comment_text(text: str) -> str:
    # A trailing backslash would splice the next line into the comment.
    return "".join(c if 0x20 <= ord(c) < 0x7F and c != "\\" else "?" for c in text)


def _braced(lines: list[str]) -> list[str]:
    if len(lines) <= 1:
        return lines
    return ["{"] + [_INDENT + line for line in lines] + ["}"]


def _guarded(cond: str | None, lines: list[str]) -> list[str]:
    if cond is None:
        return lines
    if len(lines) == 1:
        return ["if (%s) %s" % (cond, lines[0])]
    return ["if (%s) {" % cond] + [_INDENT + line for line in lines] + ["}"]


class _FunctionEmitter:
    def __init__(self, model: ImageModel, view: FunctionView, lowering: Lowering,
                 handlers: Mapping[str, Handler], consts: FunctionConstants) -> None:
        self.model = model
        self.view = view
        self.lowering = lowering
        self.handlers = handlers
        self.consts = consts
        self.stats = view.stats

    def unhandled_stmt(self, key: str, site: int, word: int) -> list[str]:
        self.stats.unhandled[key] += 1
        sites = self.stats.unhandled_sites.setdefault(key, [])
        if len(sites) < _SITE_EXAMPLES:
            sites.append(site)
        return ["tox2SpuUnhandled(ctx, ls, %s, %s);" % (ls_lit(site), u32(word))]

    # --- control group ----------------------------------------------------------------------

    def direct_branch(self, op: SpuOp) -> list[str]:
        insn, site, name = op.insn, op.addr, op.name
        flags = insn.spec.flags
        link = insn.field("RT") if "link" in flags else None
        target = insn.branch_target
        if link is not None and target == site + 4:
            # `brsl $x,$+4` reads the PC; it never leaves the instruction stream.
            return [self.lowering.link_stmt(link, site + 4)]
        cond = None
        if "conditional" in flags:
            value = op.pref(insn.field("RT"))
            if name in _HALFWORD_TESTS:
                value = "(%s & 0xffffu)" % value
            cond = "%s %s" % (value, _FIELD_TESTS[name])
        return _guarded(cond, self.lowering.direct(self.view, site, target, link))

    def indirect_branch(self, op: SpuOp) -> list[str]:
        insn, site, name = op.insn, op.addr, op.name
        flags = insn.spec.flags
        link = insn.field("RT") if "link" in flags else None
        mask = "0x%05xu" % LS_TARGET_MASK
        if name == "iret":
            target = "ctx->srr0 & %s" % mask
        else:
            target = "%s & %s" % (op.pref(insn.field("RA")), mask)
        cond = None
        if name == "bisled":
            # Taken when an event is pending (channel 0 is SPU_RdEventStat).
            cond = "tox2SpuChannelCount(ctx, ls, 0) != 0"
        elif "conditional" in flags:
            value = op.pref(insn.field("RT"))
            if name in _HALFWORD_TESTS:
                value = "(%s & 0xffffu)" % value
            cond = "%s %s" % (value, _FIELD_TESTS[name])
        if name == "bi" and insn.field("RA") == 0 and not insn.field("D") and not insn.field("E"):
            self.stats.edges["returns"] += 1
        body = ["uint32_t target = %s;" % target]
        # bisled sets its link on both paths (ISA: RT receives the next address whether or not the
        # branch is taken), so the write sits outside the condition, after the target was read.
        pre_link = link is not None and name == "bisled"
        if pre_link:
            body.append(self.lowering.link_stmt(link, site + 4))
        taken = []
        # The D and E bits change the interrupt state together with the transfer. iret has no extra
        # rule here: no interrupt is ever delivered, so only RdMachStat could tell.
        if insn.field("D"):
            taken.append("ctx->int_enabled = 0;")
        elif insn.field("E"):
            taken.append("ctx->int_enabled = 1;")
        taken += self.lowering.computed(self.view, site, "target", None if pre_link else link)
        if cond is None:
            return ["{"] + [_INDENT + line for line in body + taken] + ["}"]
        return (["{"] + [_INDENT + line for line in body] + [_INDENT + "if (%s) {" % cond]
                + [_INDENT * 2 + line for line in taken] + [_INDENT + "}", "}"])

    def stop(self, op: SpuOp) -> list[str]:
        code = op.f("STOP")
        call = "tox2SpuStop(ctx, ls, %s, 0x%xu)" % (ls_lit(op.addr), code)
        self.stats.edges["stops"] += 1
        if code in STOP_TERMINATE_CODES:
            self.stats.edges["leaves"] += 1
            return [call + ";", self.lowering.halt_stmt()]
        return ["if (!%s) %s" % (call, self.lowering.halt_stmt())]

    def control(self, op: SpuOp) -> list[str] | None:
        if op.name in _DIRECT_BRANCHES:
            return self.direct_branch(op)
        if op.name in _INDIRECT_BRANCHES:
            return self.indirect_branch(op)
        if op.name == "stop":
            return self.stop(op)
        return []

    # --- instructions -----------------------------------------------------------------------

    def instruction(self, addr: int) -> list[str]:
        code = self.model.code
        if not code.valid(addr):
            return self.unhandled_stmt("invalid", addr, code.word(addr) if code.contains(addr) else 0)
        insn = code.insn(addr)
        op = SpuOp(insn, self.lowering, self.consts.at(addr))
        self.stats.mnemonics[op.name] += 1
        if op.name in CORE_MNEMONICS:
            lines = self.control(op)
        else:
            fn = self.handlers.get(op.name)
            lines = None if fn is None else fn(op)
            if lines is not None and len(lines) > 1:
                # Handler temporaries (t0, t1, ...) stay local to the instruction.
                lines = _braced(lines)
        if lines is None:
            return self.unhandled_stmt(op.unhandled_key or op.name, addr, op.word)
        if op.folded_control:
            self.stats.folded_shufb += 1
        return lines

    def fall_through(self, addr: int) -> list[str]:
        code = self.model.code
        if not code.valid(addr):
            return []
        insn = code.insn(addr)
        flow = flow_of(insn)
        # bisled is the one call that continues at the next word when it is not taken; every other
        # call leaves through its target and the return site is a dispatchable block of its own.
        if not flow.falls_through or (flow.kind in ("call", "indirect_call") and insn.spec.mnemonic != "bisled"):
            return []
        nxt = addr + 4
        if nxt in self.view.owned:
            return []
        if not code.valid(nxt):
            self.stats.edges["invalid_flow"] += 1
            self.stats.invalid_flow.append((addr, nxt))
            word = code.word(nxt) if code.contains(nxt) else 0
            return ["tox2SpuUnhandled(ctx, ls, %s, %s);" % (ls_lit(nxt), u32(word))]
        return self.lowering.direct(self.view, addr, nxt, fall_through=True)


def emit_function(model: ImageModel, entry: int, stats: EmitStats,
                  handlers: Mapping[str, Handler] | None = None, *, title: str | None = None) -> FunctionRecord:
    lowering = model.lowering_of(entry)
    addrs = model.functions[entry]
    view = FunctionView(model, entry, frozenset(addrs), stats)
    resolved = _default_handlers() if handlers is None else handlers
    consts = function_constants(model.code, view.owned, model.entry_cases(entry),
                                lambda name: is_handled(name, resolved))
    em = _FunctionEmitter(model, view, lowering, resolved, consts)
    blocks: list[tuple[int, str | None, list[str]]] = []
    for addr in addrs:
        lines = em.instruction(addr) + em.fall_through(addr)
        comment = None
        if model.comments:
            insn = model.code.insn(addr)
            comment = "%05x: %08x  %s" % (addr, insn.word, _comment_text(format_insn(insn)))
        blocks.append((addr, comment, lines))
    name = model.func_name(entry)
    text = lowering.render_function(view, name, None if title is None else _comment_text(title), entry,
                                    model.entry_cases(entry), blocks)
    stats.functions += 1
    stats.instructions += len(addrs)
    stats.labels += len(view.targeted)
    return FunctionRecord(entry, name, len(addrs), text)


def emit_image(model: ImageModel, stats: EmitStats, handlers: Mapping[str, Handler] | None = None,
               titles: Mapping[int, str] | None = None) -> list[FunctionRecord]:
    """Every function of the image in address order."""
    return [emit_function(model, entry, stats, handlers, title=(titles or {}).get(entry))
            for entry in model.functions]

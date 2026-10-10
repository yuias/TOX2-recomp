"""Emitter core: handler registry, `Op` helper, per-function emission and control flow."""

from __future__ import annotations

import functools
import importlib
import importlib.util
from collections import Counter
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field

from .emit_db import GROUP_OF, GROUPS
from .ppu import Insn
from .ppu_flow import CodeImage, flow_of
from .ppu_format import format_insn

_MASK32 = 0xFFFFFFFF
_MASK64 = 0xFFFFFFFFFFFFFFFF
_SITE_EXAMPLES = 5
_INDENT = "    "


def u32(v: int) -> str:
    return "0x%08xu" % (v & _MASK32)


def u64(v: int) -> str:
    return "0x%016xull" % (v & _MASK64)


@dataclass(frozen=True)
class EmitEnv:
    names: Mapping[int, str]                        # function entry -> C name ("fn_%08x")
    thunks: Mapping[int, str]                       # import thunk addr -> trampoline ("imp_%08x")
    switch_targets: Mapping[int, tuple[int, ...]]   # bcctr site -> sorted distinct targets
    comments: bool = False


@dataclass
class EmitStats:
    edges: Counter = field(default_factory=Counter)
    unhandled: Counter = field(default_factory=Counter)
    unhandled_sites: dict[str, list[int]] = field(default_factory=dict)
    dynamic_targets: list[tuple[int, int, str]] = field(default_factory=list)
    labels: int = 0
    loop_hooks: int = 0
    functions: int = 0
    instructions: int = 0


class Op:
    """What a handler sees: the decoded instruction plus C-expression helpers."""

    def __init__(self, insn: Insn) -> None:
        spec = insn.spec
        self.insn = insn
        self.addr = insn.addr
        self.word = insn.word
        self.name = spec.name
        self.rc = bool(("rc" in spec.flags and insn.field("Rc"))
                       or ("rc_vc" in spec.flags and insn.field("Rc_vc")))
        self.oe = bool("oe" in spec.flags and insn.field("OE"))
        self.unhandled_key: str | None = None

    def f(self, field_name: str) -> int:
        return self.insn.field(field_name)

    def gpr(self, n: int) -> str:
        return "ctx->gpr[%d]" % n

    def fpr(self, n: int) -> str:
        return "ctx->fpr[%d]" % n

    def vr(self, n: int) -> str:
        return "ctx->vr[%d]" % n

    def ea_d(self, field_name: str = "D") -> str:
        ra, disp = self.f("rA"), self.f(field_name)
        if ra == 0:
            return u32(disp)
        return "(uint32_t)(%s + %s)" % (self.gpr(ra), u64(disp))

    def ea_x(self) -> str:
        ra, rb = self.f("rA"), self.f("rB")
        if ra == 0:
            return "(uint32_t)%s" % self.gpr(rb)
        return "(uint32_t)(%s + %s)" % (self.gpr(ra), self.gpr(rb))

    def ea64_d(self, field_name: str = "D") -> str:
        ra, disp = self.f("rA"), self.f(field_name)
        if ra == 0:
            return u64(disp)
        return "(%s + %s)" % (self.gpr(ra), u64(disp))

    def ea64_x(self) -> str:
        ra, rb = self.f("rA"), self.f("rB")
        if ra == 0:
            return self.gpr(rb)
        return "(%s + %s)" % (self.gpr(ra), self.gpr(rb))

    def record(self, value: str) -> str:
        return "tox2Record(ctx, %s);" % value

    def unsupported(self, reason: str) -> None:
        self.unhandled_key = "%s:%s" % (self.name, reason)
        return None


Handler = Callable[[Op], "list[str] | None"]

_REGISTRY: dict[str, Handler] = {}


def handler(*names: str) -> Callable[[Handler], Handler]:
    for name in names:
        if name not in GROUP_OF:
            raise ValueError("unknown mnemonic %r" % name)
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
    """Import every existing `emit_ops_<group>` module and return the registry."""
    package = __package__ or "ps3recomp"
    for group in GROUPS:
        if group == "control":
            continue
        module_name = "%s.emit_ops_%s" % (package, group)
        if importlib.util.find_spec(module_name) is None:
            continue
        importlib.import_module(module_name)
        for name, fn in _REGISTRY.items():
            if fn.__module__ == module_name and GROUP_OF[name] != group:
                raise ValueError("%s registers %r, which belongs to group %s"
                                 % (module_name, name, GROUP_OF[name]))
    return dict(_REGISTRY)


@functools.lru_cache(maxsize=1)
def _default_handlers() -> dict[str, Handler]:
    return load_handlers()


class _FunctionEmitter:
    """Collects the statements of one function and the labels they reference."""

    def __init__(self, image: CodeImage, addrs: list[int], env: EmitEnv, stats: EmitStats,
                 handlers: Mapping[str, Handler]) -> None:
        self.image = image
        self.addrs = addrs
        self.owned = frozenset(addrs)
        self.env = env
        self.stats = stats
        self.handlers = handlers
        self.targeted: set[int] = set()

    def count_unhandled(self, key: str, site: int) -> None:
        self.stats.unhandled[key] += 1
        sites = self.stats.unhandled_sites.setdefault(key, [])
        if len(sites) < _SITE_EXAMPLES:
            sites.append(site)

    def unhandled_stmt(self, key: str, site: int, word: int) -> list[str]:
        self.count_unhandled(key, site)
        return ["TOX2_UNHANDLED(%s, %s);" % (u32(site), u32(word))]

    # Direct transfer targets are classified one of: intra, function, import, dynamic.
    def classify(self, target: int, *, allow_intra: bool) -> str:
        if allow_intra and target in self.owned:
            return "intra"
        if target in self.env.names:
            return "function"
        if target in self.env.thunks:
            return "import"
        return "dynamic"

    def callee(self, kind: str, target: int) -> str:
        return (self.env.names if kind == "function" else self.env.thunks)[target]

    def tail_stmt(self, site: int, target: int, edge: str, dynamic_kind: str) -> list[str]:
        """Transfer to `target` that never returns here (b, bc, fall-through, switch case)."""
        kind = self.classify(target, allow_intra=edge != "fallthrough_tails")
        if kind == "intra":
            self.targeted.add(target)
            stmt = "goto L_%08x;" % target
            if target <= site:
                self.stats.loop_hooks += 1
                return ["{ TOX2_LOOP(%s); %s }" % (u32(site), stmt)]
            return [stmt]
        if kind == "dynamic":
            self.stats.dynamic_targets.append((site, target, dynamic_kind))
            self.stats.edges["fallthrough_tails" if edge == "fallthrough_tails"
                             else "tails_dynamic"] += 1
            return ["TOX2_TAIL_DYN(%s, %s);" % (u32(site), u32(target))]
        if edge == "fallthrough_tails":
            self.stats.edges[edge] += 1
        else:
            self.stats.edges["tails_direct" if kind == "function" else "tails_import"] += 1
        return ["TOX2_TAIL(%s);" % self.callee(kind, target)]

    def call_stmt(self, site: int, target: int) -> list[str]:
        ret = u32(site + 4)
        kind = self.classify(target, allow_intra=False)
        if kind == "dynamic":
            self.stats.dynamic_targets.append((site, target, "call"))
            self.stats.edges["calls_dynamic_const"] += 1
            return ["TOX2_CALL_DYN(%s, %s, %s);" % (u32(site), u32(target), ret)]
        self.stats.edges["calls_direct" if kind == "function" else "calls_import"] += 1
        return ["TOX2_CALL(%s, %s);" % (self.callee(kind, target), ret)]

    def indirect_stmt(self, site: int, reg: str, link: bool) -> list[str]:
        if link:
            self.stats.edges["calls_indirect"] += 1
            return ["TOX2_CALL_DYN(%s, ctx->%s, %s);" % (u32(site), reg, u32(site + 4))]
        self.stats.edges["tails_dynamic"] += 1
        return ["TOX2_TAIL_DYN(%s, ctx->%s);" % (u32(site), reg)]

    def switch_stmt(self, site: int, targets: Iterable[int]) -> list[str]:
        """`bcctr` over a resolved jump table: switch on the CTR value (no index register is known)."""
        cases = sorted(set(targets))
        lines: list[str] = []
        body: list[str] = []
        hook = False
        for target in cases:
            if self.classify(target, allow_intra=True) == "intra":
                self.targeted.add(target)
                hook = hook or target <= site
                body.append("    case %s: goto L_%08x;" % (u32(target), target))
            else:
                body.append("    case %s: %s" % (u32(target),
                                                 self.tail_stmt(site, target, "tails", "tail")[0]))
        if hook:
            # One hook for the whole switch instead of one per backward case.
            self.stats.loop_hooks += 1
            lines.append("TOX2_LOOP(%s);" % u32(site))
        self.stats.edges["switches"] += 1
        self.stats.edges["switch_cases"] += len(cases)
        lines.append("switch ((uint32_t)ctx->ctr) {")
        lines.extend(body)
        lines.append("    default: %s" % self.indirect_stmt(site, "ctr", False)[0])
        lines.append("}")
        return lines

    @staticmethod
    def condition(insn: Insn) -> str | None:
        """C condition of a conditional branch, or None when it is always taken (BO, BI)."""
        bo = insn.field("BO")
        parts = []
        if not bo & 0x04:
            parts.append("ctx->ctr == 0" if bo & 0x02 else "ctx->ctr != 0")
        if not bo & 0x10:
            bit = "ctx->cr[%d]" % insn.field("BI")
            parts.append(bit if bo & 0x08 else "!" + bit)
        return " && ".join(parts) or None

    @staticmethod
    def guarded(cond: str | None, lines: list[str]) -> list[str]:
        if cond is None:
            return lines
        if len(lines) == 1:
            return ["if (%s) %s" % (cond, lines[0])]
        return ["if (%s) {" % cond] + [_INDENT + line for line in lines] + ["}"]

    def branch(self, op: Op) -> list[str] | None:
        """b, bc, bclr and bcctr. Semantics follow RPCS3 `BC`, `BCLR` and `BCCTR`."""
        insn, site, name = op.insn, op.addr, op.name
        link = bool(insn.field("LK"))
        ret = u32(site + 4)
        cond = None if name == "b" else self.condition(insn)
        ctr_ignored = bool(insn.field("BO") & 0x04) if name != "b" else True
        if name == "bcctr" and not ctr_ignored:
            return op.unsupported("ctr")    # CTR decrement is not defined for bcctr
        # CTR is decremented whether or not the branch is taken.
        pre = [] if ctr_ignored else ["ctx->ctr -= 1;"]
        # LR is written whether or not a conditional branch is taken.
        set_lr = ["ctx->lr = %s;" % ret] if link and cond is not None else []

        if name in ("b", "bc"):
            flow = flow_of(insn)
            if flow.kind == "seq":
                # `bcl 20,31,$+4` get-PC idiom (and its conditional forms): both outcomes
                # continue at the next word.
                return pre + ["ctx->lr = %s;" % ret]
            if link:
                transfer = self.call_stmt(site, flow.target)
            else:
                transfer = self.tail_stmt(site, flow.target, "tails", "tail")
            return pre + set_lr + self.guarded(cond, transfer)

        if name == "bclr":
            if cond is None:
                if link:
                    return self.indirect_stmt(site, "lr", True)
                self.stats.edges["returns"] += 1
                return ["return;"]
            if link:
                # The target is read before LR is overwritten.
                self.stats.edges["calls_indirect"] += 1
                call = "TOX2_CALL_DYN(%s, t0, %s);" % (u32(site), ret)
                return (["{", _INDENT + "uint32_t t0 = (uint32_t)ctx->lr;"]
                        + [_INDENT + line for line in pre + set_lr + self.guarded(cond, [call])]
                        + ["}"])
            self.stats.edges["returns"] += 1
            return pre + self.guarded(cond, ["return;"])

        # bcctr
        if link:
            return set_lr + self.guarded(cond, self.indirect_stmt(site, "ctr", True))
        targets = self.env.switch_targets.get(site)
        transfer = (self.switch_stmt(site, targets) if targets
                    else self.indirect_stmt(site, "ctr", False))
        return self.guarded(cond, transfer)

    def trap(self, op: Op) -> list[str]:
        """tw, twi, td, tdi. Semantics follow RPCS3 `TW`, `TWI`, `TD` and `TDI`."""
        insn, site = op.insn, op.addr
        to = insn.field("TO")
        if to == 0:
            return []
        self.stats.edges["traps"] += 1
        trap = ["TOX2_TRAP(%s);" % u32(site)]
        if to == 31:
            return trap
        bits = 64 if op.name in ("td", "tdi") else 32
        reg_a = op.gpr(insn.field("rA"))
        signed_a = "(int%d_t)%s" % (bits, reg_a)
        unsigned_a = "(uint%d_t)%s" % (bits, reg_a)
        if op.name in ("twi", "tdi"):
            simm = insn.field("SIMM")
            signed_b = str(simm)
            unsigned_b = u64(simm) if bits == 64 else u32(simm)
        else:
            reg_b = op.gpr(insn.field("rB"))
            signed_b = "(int%d_t)%s" % (bits, reg_b)
            unsigned_b = "(uint%d_t)%s" % (bits, reg_b)
        terms = [(0x10, "%s < %s" % (signed_a, signed_b)),
                 (0x08, "%s > %s" % (signed_a, signed_b)),
                 (0x04, "%s == %s" % (unsigned_a, unsigned_b)),
                 (0x02, "%s < %s" % (unsigned_a, unsigned_b)),
                 (0x01, "%s > %s" % (unsigned_a, unsigned_b))]
        cond = " || ".join(text for bit, text in terms if to & bit)
        return self.guarded(cond, trap)

    def control(self, op: Op) -> list[str] | None:
        """Statements for a control-group instruction, or None when it is not supported."""
        if op.name in ("b", "bc", "bclr", "bcctr"):
            return self.branch(op)
        if op.name == "sc":
            self.stats.edges["syscalls"] += 1
            return ["TOX2_SYSCALL(%s);" % u32(op.addr)]
        return self.trap(op)

    def instruction(self, addr: int) -> list[str]:
        image = self.image
        if not image.valid(addr):
            return self.unhandled_stmt("invalid", addr, image.word(addr))
        insn = image.insn(addr)
        op = Op(insn)
        if op.name in ("b", "bc", "bclr", "bcctr", "sc", "tw", "twi", "td", "tdi"):
            lines = self.control(op)
            if lines is None:
                return self.unhandled_stmt(op.unhandled_key or op.name, addr, op.word)
            return lines
        fn = self.handlers.get(op.name)
        lines = None if fn is None else fn(op)
        if lines is None:
            return self.unhandled_stmt(op.unhandled_key or op.name, addr, op.word)
        if len(lines) > 1:
            # Handler temporaries (t0, t1, ...) stay local to the instruction.
            return ["{"] + [_INDENT + line for line in lines] + ["}"]
        return lines

    def fall_through(self, addr: int) -> list[str]:
        image = self.image
        if not image.valid(addr) or not flow_of(image.insn(addr)).falls_through:
            return []
        nxt = addr + 4
        if nxt in self.owned:
            return []
        return self.tail_stmt(addr, nxt, "fallthrough_tails", "fallthrough")


def _comment_text(text: str) -> str:
    # A trailing backslash would splice the next line into the comment.
    return "".join(c if 0x20 <= ord(c) < 0x7F and c != "\\" else "?" for c in text)


def emit_function(image: CodeImage, entry: int, addrs: Iterable[int], env: EmitEnv,
                  stats: EmitStats, *, name: str, title: str | None = None,
                  handlers: Mapping[str, Handler] | None = None) -> str:
    addr_list = sorted(addrs)
    em = _FunctionEmitter(image, addr_list, env, stats,
                          _default_handlers() if handlers is None else handlers)
    blocks: list[tuple[int, list[str]]] = []
    for addr in addr_list:
        blocks.append((addr, em.instruction(addr) + em.fall_through(addr)))

    out: list[str] = []
    if title:
        out.append("// %s" % _comment_text(title))
    out.append("TOX2_PPU_FUNC(%s) {" % name)
    out.append(_INDENT + "TOX2_ENTER(%s);" % u32(entry))
    for addr, lines in blocks:
        if addr in em.targeted:
            out.append("L_%08x:" % addr)
        if env.comments:
            out.append(_INDENT + "// %s: %08x  %s"
                       % (u32(addr)[:-1], image.word(addr), format_insn(image.insn(addr), True)))
        out.extend(_INDENT + line for line in lines)
    out.append("}")

    stats.functions += 1
    stats.instructions += len(addr_list)
    stats.labels += len(em.targeted)
    return "\n".join(out) + "\n"

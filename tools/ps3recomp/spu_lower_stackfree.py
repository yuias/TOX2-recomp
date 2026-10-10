"""The host-stack-free lowering of SPU control flow.

This is the only emitter module that knows the TOX2_SPU_SF_* macros of `spu_lower_stackfree.h`.
A transfer to another function or to a computed address is a tail call, so a call writes the link
register and jumps, a return is a dispatch to the register value, and the return site is an entry
of the function that owns it (see `render_function`). Control never needs the host stack.
"""

from __future__ import annotations

from collections.abc import Sequence

from .spu_emit_core import FunctionView, Lowering, ls_lit, u32

_INDENT = "    "

NAME = "stackfree"
HEADER = "tox2/spu_lower_stackfree.h"


class StackFreeLowering(Lowering):
    name = NAME
    header = HEADER

    def link_stmt(self, reg: int, ret: int) -> str:
        return "ctx->gpr[%d] = tox2VrFromBeWords(%s, 0u, 0u, 0u);" % (reg, u32(ret))

    def halt_stmt(self) -> str:
        return "TOX2_SPU_SF_LEAVE();"

    def direct(self, view: FunctionView, site: int, target: int, link: int | None = None,
               *, fall_through: bool = False) -> list[str]:
        out = [self.link_stmt(link, site + 4)] if link is not None else []
        stats, model = view.stats, view.model
        if not fall_through and target in view.owned:
            view.targeted.add(target)
            stats.edges["gotos"] += 1
            if target <= site:
                # A thread stuck in a local loop notices a halt request here.
                stats.loop_hooks += 1
                out.append("TOX2_SPU_SF_LOOP(%s);" % ls_lit(site))
            out.append("goto L_%05x;" % target)
            return out
        owner = model.owner.get(target)
        if owner is not None and target in model.dispatchable:
            stats.edges["fallthrough_jumps" if fall_through else "jumps"] += 1
            out.append("TOX2_SPU_SF_JUMP(%s, %s);" % (model.func_name(owner), ls_lit(target)))
            return out
        if owner is not None:
            # Owned by another function but not an entry: the runtime stops with an unknown target.
            stats.unlisted_targets.append((site, target))
        stats.edges["fallthrough_dispatch" if fall_through else "dispatch_direct"] += 1
        out.append("TOX2_SPU_SF_DISPATCH(%s, %s);" % (ls_lit(site), ls_lit(target)))
        return out

    def computed(self, view: FunctionView, site: int, target_expr: str, link: int | None = None) -> list[str]:
        # The caller evaluated the target already; `bisl $0,$0` branches to the old $0 and then
        # overwrites it, so the link write must come after that evaluation.
        out = [self.link_stmt(link, site + 4)] if link is not None else []
        view.stats.edges["dispatch_indirect"] += 1
        out.append("TOX2_SPU_SF_DISPATCH(%s, %s);" % (ls_lit(site), target_expr))
        return out

    def render_function(self, view: FunctionView, name: str, title: str | None, entry: int,
                        cases: Sequence[int], blocks: Sequence[tuple[int, str | None, list[str]]]) -> str:
        labels = set(view.targeted)
        out: list[str] = []
        if title:
            out.append("// %s" % title)
        out.append("TOX2_SPU_FUNC(%s) {" % name)
        # Not every function reads the local store, and -Wextra rejects an unused parameter.
        out.append(_INDENT + "(void)ls;")
        if len(cases) > 1:
            # One C function per recovered function: a transfer to any dispatchable block of it
            # arrives at the top with its address in jump_target.
            out.append(_INDENT + "switch (ctx->jump_target) {")
            for addr in cases:
                labels.add(addr)
                out.append(_INDENT + "case %s: TOX2_SPU_SF_ENTER(%s); goto L_%05x;"
                           % (ls_lit(addr), ls_lit(addr), addr))
            # Only the dispatcher and generated jumps set jump_target, so this is an emitter or table bug.
            out.append(_INDENT + "default: tox2SpuUnhandled(ctx, ls, ctx->jump_site, ctx->jump_target);")
            out.append(_INDENT + "}")
        else:
            out.append(_INDENT + "TOX2_SPU_SF_ENTER(%s);" % ls_lit(entry))
            if blocks[0][0] != entry:
                labels.add(entry)
                out.append(_INDENT + "goto L_%05x;" % entry)
        for addr, comment, lines in blocks:
            if addr in labels:
                out.append("L_%05x:" % addr)
            if comment:
                out.append(_INDENT + "// " + comment)
            for line in lines:
                out.append(_INDENT + line)
        out.append("}")
        return "\n".join(out) + "\n"


LOWERING = StackFreeLowering()

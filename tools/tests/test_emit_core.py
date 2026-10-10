"""Tests for the emitter core: control flow, labels, unhandled counting and the handler registry."""

import sys
import types
import unittest
from importlib.machinery import ModuleSpec
from unittest import mock

from ps3recomp import emit_core
from ps3recomp.emit_core import EmitEnv, EmitStats, Op, emit_function, handler, u32, u64
from ps3recomp.ppu import decode
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm

BASE = 0x10000
OTHER = 0x20000           # a known function outside the function under test
STUB = 0x30000            # an import trampoline


def set_r3(op: Op) -> list[str]:
    return ["ctx->gpr[3] = %d;" % op.f("SIMM")]


# `ori` stands in for nop and `addi` for a plain statement; every other mnemonic is unhandled.
HANDLERS = {"ori": lambda op: [], "addi": set_r3}


def make_env(names=None, thunks=None, switches=None, comments=False) -> EmitEnv:
    return EmitEnv({OTHER: "fn_%08x" % OTHER} if names is None else names,
                   {STUB: "imp_%08x" % STUB} if thunks is None else thunks,
                   switches or {}, comments)


def emit(words, *, env=None, stats=None, start=BASE, entry=None, handlers=HANDLERS, title=None,
         addrs=None):
    image = CodeImage.from_words(start, words)
    entry = start if entry is None else entry
    stats = EmitStats() if stats is None else stats
    text = emit_function(image, entry, addrs or [start + 4 * i for i in range(len(words))],
                         env or make_env(), stats, name="fn_%08x" % entry, handlers=handlers,
                         title=title)
    return text, stats


def lines_of(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines()]


class HelpersTest(unittest.TestCase):
    def test_literals(self):
        self.assertEqual(u32(0x7484C), "0x0007484cu")
        self.assertEqual(u32(-4), "0xfffffffcu")
        self.assertEqual(u64(-8), "0xfffffffffffffff8ull")

    def test_op_expressions(self):
        lwz = Op(decode(asm.lwz(9, -4, 10), BASE, strict=True))
        self.assertEqual(lwz.ea_d(), "(uint32_t)(ctx->gpr[10] + 0xfffffffffffffffcull)")
        self.assertEqual(lwz.ea64_d(), "(ctx->gpr[10] + 0xfffffffffffffffcull)")
        lwz0 = Op(decode(asm.lwz(9, -4, 0), BASE, strict=True))
        self.assertEqual(lwz0.ea_d(), "0xfffffffcu")
        ld = Op(decode(asm.ld(9, -8, 2), BASE, strict=True))
        self.assertEqual(ld.ea_d("DS"), "(uint32_t)(ctx->gpr[2] + 0xfffffffffffffff8ull)")
        lwzx = Op(decode(asm.lwzx(3, 4, 5), BASE, strict=True))
        self.assertEqual(lwzx.ea_x(), "(uint32_t)(ctx->gpr[4] + ctx->gpr[5])")
        self.assertEqual(lwzx.ea64_x(), "(ctx->gpr[4] + ctx->gpr[5])")
        lwzx0 = Op(decode(asm.lwzx(3, 0, 5), BASE, strict=True))
        self.assertEqual(lwzx0.ea_x(), "(uint32_t)ctx->gpr[5]")
        self.assertEqual(lwzx0.record("ctx->gpr[3]"), "dalRecord(ctx, ctx->gpr[3]);")
        self.assertEqual((lwzx0.gpr(7), lwzx0.fpr(1), lwzx0.vr(2)),
                         ("ctx->gpr[7]", "ctx->fpr[1]", "ctx->vr[2]"))

    def test_op_flags(self):
        add_o = Op(decode(asm.add(3, 4, 5) | 1 << 10 | 1, BASE, strict=True))
        self.assertEqual((add_o.name, add_o.rc, add_o.oe), ("add", True, True))
        plain = Op(decode(asm.add(3, 4, 5), BASE, strict=True))
        self.assertEqual((plain.rc, plain.oe), (False, False))

    def test_unsupported_sets_the_key(self):
        op = Op(decode(asm.lwz(9, 0, 0), BASE, strict=True))
        self.assertIsNone(op.unsupported("ra0"))
        self.assertEqual(op.unhandled_key, "lwz:ra0")


class FunctionShapeTest(unittest.TestCase):
    def test_empty_body_function_with_a_return(self):
        text, stats = emit([asm.blr()])
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.functions, stats.instructions, stats.labels), (1, 1, 0))
        self.assertEqual(stats.edges["returns"], 1)

    def test_title_is_a_sanitised_comment(self):
        text, _ = emit([asm.blr()], title="foo\\\nbar\u00e9")
        self.assertEqual(text.splitlines()[0], "// foo??bar?")

    def test_comments_option(self):
        text, _ = emit([asm.li(3, 5), asm.blr()], env=make_env(comments=True))
        self.assertIn("    // 0x00010000: 38600005  li r3,0x5\n", text)
        self.assertIn("    // 0x00010004: 4e800020  blr\n", text)

    def test_no_comments_by_default(self):
        text, _ = emit([asm.li(3, 5), asm.blr()])
        self.assertNotIn("//", text)

    def test_output_is_deterministic(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        self.assertEqual(emit(words)[0], emit(words)[0])

    def test_multi_line_handler_result_gets_a_block(self):
        def two(op):
            return ["uint64_t t0 = ctx->gpr[3];", "ctx->gpr[4] = t0;"]

        text, _ = emit([asm.li(3, 1), asm.blr()], handlers={"addi": two})
        self.assertEqual(lines_of(text)[2:7], ["{", "uint64_t t0 = ctx->gpr[3];",
                                               "ctx->gpr[4] = t0;", "}", "return;"])


class BranchTest(unittest.TestCase):
    def test_forward_goto_has_no_loop_hook(self):
        words = [asm.b(BASE, BASE + 8), asm.li(3, 1), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "    goto L_00010008;\n"
                               "    ctx->gpr[3] = 1;\n"
                               "L_00010008:\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.labels, stats.loop_hooks), (1, 0))

    def test_backward_branch_text(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        text, stats = emit(words)
        print("\n" + text)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "L_00010000:\n"
                               "    ctx->gpr[3] = 1;\n"
                               "    { DAL_LOOP(0x00010004u); goto L_00010000; }\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.labels, stats.loop_hooks), (1, 1))

    def test_branch_to_self_is_a_backward_branch(self):
        text, stats = emit([asm.b(BASE, BASE)])
        self.assertIn("    { DAL_LOOP(0x00010000u); goto L_00010000; }\n", text)
        self.assertEqual(stats.loop_hooks, 1)

    def test_branch_to_own_entry_labels_the_entry(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        text, _ = emit(words, env=make_env(names={BASE: "fn_00010000"}))
        self.assertIn("DAL_ENTER(0x00010000u);\nL_00010000:\n", text)
        self.assertEqual(text.count("L_00010000:"), 1)

    def test_labels_only_where_targeted(self):
        words = [asm.li(3, 1), asm.li(3, 2), asm.b(BASE + 8, BASE + 12), asm.li(3, 3), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(text.count(" L_") + text.count("\nL_"), 2)   # one goto, one label
        self.assertIn("L_0001000c:\n", text)
        self.assertEqual(stats.labels, 1)

    def test_always_taken_bc_acts_like_b(self):
        words = [asm.bc(20, 0, BASE, BASE + 8), asm.li(3, 1), asm.blr()]
        text, _ = emit(words)
        self.assertIn("    goto L_00010008;\n", text)

    def test_absolute_branch_target(self):
        text, stats = emit([asm.b(BASE, OTHER, aa=True)])
        self.assertIn("    DAL_TAIL(fn_00020000);\n", text)
        self.assertEqual(stats.edges["tails_direct"], 1)


class TailTest(unittest.TestCase):
    def test_tail_to_function(self):
        text, stats = emit([asm.b(BASE, OTHER)])
        self.assertIn("    DAL_TAIL(fn_00020000);\n", text)
        self.assertEqual(stats.edges["tails_direct"], 1)

    def test_tail_to_import(self):
        text, stats = emit([asm.b(BASE, STUB)])
        self.assertIn("    DAL_TAIL(imp_00030000);\n", text)
        self.assertEqual(stats.edges["tails_import"], 1)

    def test_tail_to_dynamic_target_is_recorded(self):
        text, stats = emit([asm.b(BASE, 0x40000)])
        self.assertIn("    DAL_TAIL_DYN(0x00010000u, 0x00040000u);\n", text)
        self.assertEqual(stats.edges["tails_dynamic"], 1)
        self.assertEqual(stats.dynamic_targets, [(BASE, 0x40000, "tail")])

    def test_bctr_is_a_dynamic_tail(self):
        text, stats = emit([asm.bctr()])
        self.assertIn("    DAL_TAIL_DYN(0x00010000u, ctx->ctr);\n", text)
        self.assertEqual(stats.edges["tails_dynamic"], 1)
        self.assertEqual(stats.dynamic_targets, [])

    def test_tail_to_a_target_inside_the_function_when_it_is_also_a_function(self):
        # Entry classification wins for calls; branches stay local.
        env = make_env(names={BASE + 8: "fn_00010008"})
        text, _ = emit([asm.b(BASE, BASE + 8), asm.li(3, 1), asm.blr()], env=env)
        self.assertIn("    goto L_00010008;\n", text)


class CallTest(unittest.TestCase):
    def test_call_kinds(self):
        words = [asm.bl(BASE, OTHER), asm.bl(BASE + 4, STUB), asm.bl(BASE + 8, 0x40000),
                 asm.bctrl(), asm.blrl(), asm.blr()]
        text, stats = emit(words)
        body = lines_of(text)[2:-1]
        self.assertEqual(body, [
            "DAL_CALL(fn_00020000, 0x00010004u);",
            "DAL_CALL(imp_00030000, 0x00010008u);",
            "DAL_CALL_DYN(0x00010008u, 0x00040000u, 0x0001000cu);",
            "DAL_CALL_DYN(0x0001000cu, ctx->ctr, 0x00010010u);",
            "DAL_CALL_DYN(0x00010010u, ctx->lr, 0x00010014u);",
            "return;"])
        self.assertEqual(stats.edges["calls_direct"], 1)
        self.assertEqual(stats.edges["calls_import"], 1)
        self.assertEqual(stats.edges["calls_dynamic_const"], 1)
        self.assertEqual(stats.edges["calls_indirect"], 2)
        self.assertEqual(stats.dynamic_targets, [(BASE + 8, 0x40000, "call")])

    def test_call_to_own_entry_is_a_call_not_a_goto(self):
        text, stats = emit([asm.bl(BASE, BASE), asm.blr()], env=make_env(names={BASE: "fn_00010000"}))
        self.assertIn("    DAL_CALL(fn_00010000, 0x00010004u);\n", text)
        self.assertNotRegex(text, r"(^|[ \n])L_")
        self.assertEqual(stats.labels, 0)

    def test_call_to_a_mid_function_address_is_dynamic(self):
        text, stats = emit([asm.bl(BASE, BASE + 8), asm.blr(), asm.blr()])
        self.assertIn("    DAL_CALL_DYN(0x00010000u, 0x00010008u, 0x00010004u);\n", text)
        self.assertEqual(stats.dynamic_targets, [(BASE, BASE + 8, "call")])

    def test_get_pc_idiom_only_writes_lr(self):
        words = [asm.bc(20, 31, BASE, BASE + 4, lk=True), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(lines_of(text)[2:-1], ["ctx->lr = 0x00010004u;", "return;"])
        self.assertEqual(stats.edges["calls_direct"] + stats.edges["calls_dynamic_const"], 0)
        self.assertEqual(sum(stats.unhandled.values()), 0)

    def test_get_pc_via_b_lk(self):
        text, _ = emit([asm.b(BASE, BASE + 4, lk=True), asm.blr()])
        self.assertEqual(lines_of(text)[2], "ctx->lr = 0x00010004u;")

    def test_conditional_get_pc_keeps_the_ctr_decrement_and_lr_write(self):
        text, stats = emit([asm.bc(16, 0, BASE, BASE + 4, lk=True), asm.blr()])
        self.assertEqual(lines_of(text)[2:-1],
                         ["ctx->ctr -= 1;", "ctx->lr = 0x00010004u;", "return;"])
        self.assertEqual(sum(stats.unhandled.values()), 0)


class FallThroughTest(unittest.TestCase):
    def test_call_then_fall_through_out_of_the_function(self):
        env = make_env(names={OTHER: "fn_00020000", BASE + 4: "fn_00010004"})
        text, stats = emit([asm.bl(BASE, OTHER)], env=env)
        self.assertEqual(lines_of(text)[2:-1],
                         ["DAL_CALL(fn_00020000, 0x00010004u);", "DAL_TAIL(fn_00010004);"])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        self.assertEqual(stats.edges["tails_direct"], 0)

    def test_fall_through_to_an_import_and_to_a_dynamic_target(self):
        _, stats = emit([asm.li(3, 1)], env=make_env(thunks={BASE + 4: "imp_00010004"}))
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        text, stats = emit([asm.li(3, 1)])
        self.assertEqual(lines_of(text)[3], "DAL_TAIL_DYN(0x00010000u, 0x00010004u);")
        self.assertEqual(stats.dynamic_targets, [(BASE, BASE + 4, "fallthrough")])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        self.assertEqual(stats.edges["tails_dynamic"], 0)

    def test_no_tail_after_transfers_or_inside_the_function(self):
        for word in (asm.blr(), asm.bctr(), asm.b(BASE, OTHER), asm.trap()):
            _, stats = emit([word])
            self.assertEqual(stats.edges["fallthrough_tails"], 0)
        _, stats = emit([asm.li(3, 1), asm.blr()])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_gap_between_ranges_counts_as_not_owned(self):
        words = [asm.li(3, 1), asm.nop(), asm.blr()]
        text, stats = emit(words, addrs=[BASE, BASE + 8])
        self.assertEqual(lines_of(text)[2:-1],
                         ["ctx->gpr[3] = 1;", "DAL_TAIL_DYN(0x00010000u, 0x00010004u);", "return;"])
        self.assertEqual(stats.instructions, 2)


class SyscallAndTrapTest(unittest.TestCase):
    def test_sc_falls_through(self):
        text, stats = emit([asm.sc(), asm.blr()])
        self.assertEqual(lines_of(text)[2:-1], ["DAL_SYSCALL(0x00010000u);", "return;"])
        self.assertEqual(stats.edges["syscalls"], 1)

    def test_unconditional_trap_has_no_fall_through_tail(self):
        for word in (asm.trap(), asm.tw(31, 3, 4), asm.twi(31, 3, 5)):
            text, stats = emit([word])
            self.assertEqual(lines_of(text)[2:-1], ["DAL_TRAP(0x00010000u);"])
            self.assertEqual(stats.edges["traps"], 1)
            self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_trap_with_no_condition_bits_emits_nothing(self):
        for word in (asm.tw(0, 3, 4), asm.twi(0, 3, 4), asm.td(0, 3, 4), asm.tdi(0, 3, 4)):
            text, stats = emit([word, asm.blr()])
            self.assertEqual(lines_of(text)[2:-1], ["return;"])
            self.assertEqual(stats.edges["traps"], 0)
            self.assertEqual(sum(stats.unhandled.values()), 0)

    def test_conditional_trap_forms(self):
        cases = [
            (asm.tw(4, 3, 4), "if ((uint32_t)ctx->gpr[3] == (uint32_t)ctx->gpr[4]) "),
            (asm.tw(0x10 | 0x01, 3, 4), "if ((int32_t)ctx->gpr[3] < (int32_t)ctx->gpr[4]"
                                        " || (uint32_t)ctx->gpr[3] > (uint32_t)ctx->gpr[4]) "),
            (asm.td(0x08, 5, 6), "if ((int64_t)ctx->gpr[5] > (int64_t)ctx->gpr[6]) "),
            (asm.td(0x02, 5, 6), "if ((uint64_t)ctx->gpr[5] < (uint64_t)ctx->gpr[6]) "),
            (asm.twi(0x10, 3, -5), "if ((int32_t)ctx->gpr[3] < -5) "),
            (asm.twi(0x03, 3, -5), "if ((uint32_t)ctx->gpr[3] < 0xfffffffbu"
                                   " || (uint32_t)ctx->gpr[3] > 0xfffffffbu) "),
            (asm.tdi(0x0C, 3, -5), "if ((int64_t)ctx->gpr[3] > -5"
                                   " || (uint64_t)ctx->gpr[3] == 0xfffffffffffffffbull) "),
            (asm.twi(0x04, 3, 7), "if ((uint32_t)ctx->gpr[3] == 0x00000007u) "),
        ]
        for word, prefix in cases:
            text, stats = emit([word, asm.blr()])
            self.assertEqual(lines_of(text)[2], prefix + "DAL_TRAP(0x00010000u);", hex(word))
            self.assertEqual(stats.edges["traps"], 1)
            # A conditional trap falls through.
            self.assertEqual(lines_of(text)[3], "return;")

    def test_conditional_trap_at_the_end_has_a_fall_through_tail(self):
        _, stats = emit([asm.tw(4, 3, 4)])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)


class ConditionalBranchTest(unittest.TestCase):
    # bc displacements are 16 bits, so the targets must be close to BASE (or absolute below 32K).
    FN = BASE + 0x2000
    IMP = BASE + 0x2100
    DYN = BASE + 0x2200
    ABS_FN = 0x4000

    def body(self, word, **kwargs):
        kwargs.setdefault("env", make_env(
            names={self.FN: "fn_%08x" % self.FN, self.ABS_FN: "fn_%08x" % self.ABS_FN},
            thunks={self.IMP: "imp_%08x" % self.IMP}))
        text, stats = emit([word] + kwargs.pop("rest", [asm.blr()]), **kwargs)
        return lines_of(text)[2:-1], stats

    def test_cr_condition_classes(self):
        # BO bit 0x01 is a prediction hint and must not change the result.
        for bo, cond in ((12, "ctx->cr[6]"), (13, "ctx->cr[6]"), (4, "!ctx->cr[6]"),
                         (5, "!ctx->cr[6]")):
            body, _ = self.body(asm.bc(bo, 6, BASE, BASE + 8), rest=[asm.nop(), asm.blr()])
            self.assertEqual(body[0], "if (%s) goto L_00010008;" % cond, bo)

    def test_ctr_condition_classes(self):
        cases = [(16, ["ctx->ctr -= 1;", "if (ctx->ctr != 0) goto L_00010008;"]),
                 (18, ["ctx->ctr -= 1;", "if (ctx->ctr == 0) goto L_00010008;"]),
                 (8, ["ctx->ctr -= 1;", "if (ctx->ctr != 0 && ctx->cr[5]) goto L_00010008;"]),
                 (0, ["ctx->ctr -= 1;", "if (ctx->ctr != 0 && !ctx->cr[5]) goto L_00010008;"]),
                 (10, ["ctx->ctr -= 1;", "if (ctx->ctr == 0 && ctx->cr[5]) goto L_00010008;"]),
                 (2, ["ctx->ctr -= 1;", "if (ctx->ctr == 0 && !ctx->cr[5]) goto L_00010008;"])]
        for bo, expected in cases:
            body, _ = self.body(asm.bc(bo, 5, BASE, BASE + 8), rest=[asm.nop(), asm.blr()])
            self.assertEqual(body[:2], expected, bo)

    def test_ignored_ctr_with_reserved_bit_set_does_not_decrement(self):
        # BO = 0b01110: ignore CTR, branch on CR true; bit 0x02 is ignored with it.
        body, _ = self.body(asm.bc(14, 5, BASE, BASE + 8), rest=[asm.nop(), asm.blr()])
        self.assertEqual(body[0], "if (ctx->cr[5]) goto L_00010008;")

    def test_bdnz_loop_text(self):
        words = [asm.li(3, 1), asm.bdnz(BASE + 4, BASE), asm.blr()]
        text, stats = emit(words)
        print("\n" + text)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "L_00010000:\n"
                               "    ctx->gpr[3] = 1;\n"
                               "    ctx->ctr -= 1;\n"
                               "    if (ctx->ctr != 0) { DAL_LOOP(0x00010004u); goto L_00010000; }\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.labels, stats.loop_hooks), (1, 1))

    def test_forward_conditional_goto_has_no_hook(self):
        body, stats = self.body(asm.bgt(0, BASE, BASE + 8), rest=[asm.nop(), asm.blr()])
        self.assertEqual(body[0], "if (ctx->cr[1]) goto L_00010008;")
        self.assertEqual(stats.loop_hooks, 0)

    def test_conditional_branch_to_itself_is_a_loop(self):
        text, stats = emit([asm.bge(0, BASE, BASE), asm.blr()])
        self.assertIn("    if (!ctx->cr[0]) { DAL_LOOP(0x00010000u); goto L_00010000; }\n", text)
        self.assertEqual(stats.loop_hooks, 1)

    def test_conditional_tails_by_target_kind(self):
        words = [asm.bgt(0, BASE, self.FN), asm.bgt(0, BASE + 4, self.IMP),
                 asm.bgt(0, BASE + 8, self.DYN), asm.blr()]
        body, stats = self.body(words[0], rest=words[1:])
        self.assertEqual(body, ["if (ctx->cr[1]) DAL_TAIL(fn_00012000);",
                                "if (ctx->cr[1]) DAL_TAIL(imp_00012100);",
                                "if (ctx->cr[1]) DAL_TAIL_DYN(0x00010008u, 0x00012200u);",
                                "return;"])
        self.assertEqual((stats.edges["tails_direct"], stats.edges["tails_import"],
                          stats.edges["tails_dynamic"]), (1, 1, 1))
        self.assertEqual(stats.dynamic_targets, [(BASE + 8, self.DYN, "tail")])

    def test_conditional_absolute_branch(self):
        body, _ = self.body(asm.bc(12, 2, BASE, self.ABS_FN, aa=True))
        self.assertEqual(body[0], "if (ctx->cr[2]) DAL_TAIL(fn_00004000);")

    def test_conditional_branch_at_the_end_has_a_fall_through_tail(self):
        body, stats = self.body(asm.bgt(0, BASE, self.FN), rest=[])
        self.assertEqual(body, ["if (ctx->cr[1]) DAL_TAIL(fn_00012000);",
                        "DAL_TAIL_DYN(0x00010000u, 0x00010004u);"])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)

    def test_conditional_call_writes_lr_before_the_test(self):
        words = [asm.bc(12, 2, BASE, self.FN, lk=True), asm.bc(16, 0, BASE + 4, self.IMP, lk=True),
                 asm.bc(4, 2, BASE + 8, self.DYN, lk=True), asm.blr()]
        body, stats = self.body(words[0], rest=words[1:])
        self.assertEqual(body, [
            "ctx->lr = 0x00010004u;",
            "if (ctx->cr[2]) DAL_CALL(fn_00012000, 0x00010004u);",
            "ctx->ctr -= 1;",
            "ctx->lr = 0x00010008u;",
            "if (ctx->ctr != 0) DAL_CALL(imp_00012100, 0x00010008u);",
            "ctx->lr = 0x0001000cu;",
            "if (!ctx->cr[2]) DAL_CALL_DYN(0x00010008u, 0x00012200u, 0x0001000cu);",
            "return;"])
        self.assertEqual((stats.edges["calls_direct"], stats.edges["calls_import"],
                          stats.edges["calls_dynamic_const"]), (1, 1, 1))
        self.assertEqual(stats.dynamic_targets, [(BASE + 8, self.DYN, "call")])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_conditional_bclr(self):
        body, stats = self.body(asm.bclr(12, 2))
        self.assertEqual(body, ["if (ctx->cr[2]) return;", "return;"])
        self.assertEqual(stats.edges["returns"], 2)
        body, _ = self.body(asm.bclr(16, 0))
        self.assertEqual(body[:2], ["ctx->ctr -= 1;", "if (ctx->ctr != 0) return;"])
        body, _ = self.body(asm.bclr(2, 3))
        self.assertEqual(body[:2], ["ctx->ctr -= 1;", "if (ctx->ctr == 0 && !ctx->cr[3]) return;"])

    def test_conditional_bclr_has_a_fall_through_tail_at_the_end(self):
        text, stats = emit([asm.bclr(12, 2)])
        self.assertEqual(lines_of(text)[2:-1], ["if (ctx->cr[2]) return;",
                                                "DAL_TAIL_DYN(0x00010000u, 0x00010004u);"])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)

    def test_conditional_bclrl_reads_the_target_before_writing_lr(self):
        body, stats = self.body(asm.bclr(16, 0, lk=True))
        self.assertEqual(body, ["{",
                                "uint32_t t0 = (uint32_t)ctx->lr;",
                                "ctx->ctr -= 1;",
                                "ctx->lr = 0x00010004u;",
                                "if (ctx->ctr != 0) DAL_CALL_DYN(0x00010000u, t0, 0x00010004u);",
                                "}",
                                "return;"])
        self.assertEqual(stats.edges["calls_indirect"], 1)

    def test_conditional_bcctr(self):
        body, stats = self.body(asm.bcctr(12, 2))
        self.assertEqual(body, ["if (ctx->cr[2]) DAL_TAIL_DYN(0x00010000u, ctx->ctr);", "return;"])
        self.assertEqual(stats.edges["tails_dynamic"], 1)
        self.assertEqual(sum(stats.unhandled.values()), 0)

    def test_conditional_bcctrl(self):
        body, stats = self.body(asm.bcctr(4, 2, lk=True))
        self.assertEqual(body, ["ctx->lr = 0x00010004u;",
                                "if (!ctx->cr[2]) DAL_CALL_DYN(0x00010000u, ctx->ctr, 0x00010004u);",
                                "return;"])
        self.assertEqual(stats.edges["calls_indirect"], 1)

    def test_every_conditional_form_is_handled(self):
        tail = [asm.bclr(bo, 3, lk) for bo in range(32) for lk in (False, True)]
        # bcctr is only defined with BO bit 0x04 set.
        tail += [asm.bcctr(bo, 3, lk) for bo in range(4, 32, 8) for lk in (False, True)]
        tail.append(asm.blr())
        end = BASE + 4 * (32 + len(tail) - 1)
        words = [asm.bc(bo, 3, BASE + 4 * bo, end) for bo in range(32)] + tail
        _, stats = emit(words)
        self.assertEqual(sum(stats.unhandled.values()), 0)


class SwitchTest(unittest.TestCase):
    SITE = BASE + 8

    def switch_text(self, targets, words=None, **kwargs):
        words = words or [asm.nop(), asm.nop(), asm.bctr(), asm.nop(), asm.nop(), asm.blr()]
        env = make_env(switches={self.SITE: tuple(targets)})
        return emit(words, env=env, **kwargs)

    def test_forward_switch(self):
        text, stats = self.switch_text([BASE + 12, BASE + 16])
        print("\n" + text)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "    switch ((uint32_t)ctx->ctr) {\n"
                               "        case 0x0001000cu: goto L_0001000c;\n"
                               "        case 0x00010010u: goto L_00010010;\n"
                               "        default: DAL_TAIL_DYN(0x00010008u, ctx->ctr);\n"
                               "    }\n"
                               "L_0001000c:\n"
                               "L_00010010:\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.edges["switches"], stats.edges["switch_cases"]), (1, 2))
        self.assertEqual((stats.labels, stats.loop_hooks), (2, 0))
        self.assertEqual(stats.edges["tails_dynamic"], 1)    # the default

    def test_duplicates_collapse_and_cases_are_sorted(self):
        text, stats = self.switch_text([BASE + 16, BASE + 12, BASE + 16, BASE + 12])
        cases = [line for line in lines_of(text) if line.startswith("case ")]
        self.assertEqual(cases, ["case 0x0001000cu: goto L_0001000c;",
                                 "case 0x00010010u: goto L_00010010;"])
        self.assertEqual(stats.edges["switch_cases"], 2)

    def test_backward_target_gets_one_loop_hook_before_the_switch(self):
        text, stats = self.switch_text([BASE, BASE + 12, BASE + 4, BASE + 12])
        lines = lines_of(text)
        at = lines.index("DAL_LOOP(0x00010008u);")
        self.assertEqual(lines[at + 1], "switch ((uint32_t)ctx->ctr) {")
        self.assertEqual(text.count("DAL_LOOP"), 1)
        self.assertEqual(stats.loop_hooks, 1)
        self.assertIn("case 0x00010000u: goto L_00010000;", lines)

    def test_target_equal_to_the_site_is_backward(self):
        text, stats = self.switch_text([self.SITE])
        self.assertIn("DAL_LOOP(0x00010008u);", lines_of(text))
        self.assertEqual(stats.loop_hooks, 1)

    def test_non_intra_cases_use_the_tail_forms(self):
        text, stats = self.switch_text([BASE + 12, OTHER, STUB, 0x40000])
        lines = lines_of(text)
        self.assertIn("case 0x00020000u: DAL_TAIL(fn_00020000);", lines)
        self.assertIn("case 0x00030000u: DAL_TAIL(imp_00030000);", lines)
        self.assertIn("case 0x00040000u: DAL_TAIL_DYN(0x00010008u, 0x00040000u);", lines)
        self.assertNotIn("DAL_LOOP(0x00010008u);", lines)
        self.assertEqual(stats.edges["switch_cases"], 4)
        self.assertEqual(stats.dynamic_targets, [(self.SITE, 0x40000, "tail")])

    def test_switch_site_has_no_fall_through_tail(self):
        _, stats = self.switch_text([BASE + 12], words=[asm.nop(), asm.nop(), asm.bctr()])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_bcctr_that_is_not_a_switch_site_stays_a_dynamic_tail(self):
        text, _ = emit([asm.nop(), asm.nop(), asm.bctr(), asm.blr()],
                       env=make_env(switches={BASE + 100: (BASE,)}))
        self.assertIn("    DAL_TAIL_DYN(0x00010008u, ctx->ctr);\n", text)
        self.assertNotIn("switch", text)

    def test_bcctrl_at_a_switch_site_is_still_a_call(self):
        words = [asm.nop(), asm.nop(), asm.bctrl(), asm.blr()]
        text, _ = emit(words, env=make_env(switches={self.SITE: (BASE,)}))
        self.assertIn("DAL_CALL_DYN(0x00010008u, ctx->ctr, 0x0001000cu);", text)
        self.assertNotIn("switch", text)

    def test_conditional_bcctr_at_a_switch_site_guards_the_switch(self):
        words = [asm.nop(), asm.nop(), asm.bcctr(12, 2), asm.nop(), asm.blr()]
        text, _ = emit(words, env=make_env(switches={self.SITE: (BASE + 12,)}))
        self.assertEqual(lines_of(text)[2:7],
                         ["if (ctx->cr[2]) {", "switch ((uint32_t)ctx->ctr) {",
                          "case 0x0001000cu: goto L_0001000c;",
                          "default: DAL_TAIL_DYN(0x00010008u, ctx->ctr);", "}"])

    def test_empty_target_set_falls_back_to_a_dynamic_tail(self):
        text, stats = self.switch_text([])
        self.assertNotIn("switch", text)
        self.assertEqual(stats.edges["switches"], 0)


class UnhandledTest(unittest.TestCase):
    def test_missing_handler_counts_by_name(self):
        words = [asm.mflr(0), asm.mflr(0), asm.lwz(3, 0, 4), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(dict(stats.unhandled), {"mfspr": 2, "lwz": 1})
        self.assertEqual(stats.unhandled_sites, {"mfspr": [BASE, BASE + 4], "lwz": [BASE + 8]})
        self.assertIn("    DAL_UNHANDLED(0x00010000u, 0x7c0802a6u);\n", text)

    def test_invalid_word(self):
        text, stats = emit([0x00000000, asm.blr()])
        self.assertIn("    DAL_UNHANDLED(0x00010000u, 0x00000000u);\n", text)
        self.assertEqual(dict(stats.unhandled), {"invalid": 1})
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_invalid_last_word_has_no_fall_through(self):
        _, stats = emit([0x00000000])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_nop_is_unhandled_without_a_handler(self):
        _, stats = emit([asm.nop(), asm.blr()], handlers={})
        self.assertEqual(stats.unhandled["ori"], 1)

    def test_handler_returning_empty_list_is_handled(self):
        text, stats = emit([asm.nop(), asm.blr()])
        self.assertEqual(sum(stats.unhandled.values()), 0)
        self.assertEqual(lines_of(text)[2:-1], ["return;"])

    def test_unsupported_key_and_example_limit(self):
        def ra0(op):
            return op.unsupported("ra0")

        words = [asm.li(3, i) for i in range(7)] + [asm.blr()]
        _, stats = emit(words, handlers={"addi": ra0})
        self.assertEqual(stats.unhandled["addi:ra0"], 7)
        self.assertEqual(stats.unhandled_sites["addi:ra0"], [BASE + 4 * i for i in range(5)])
        self.assertNotIn("addi", stats.unhandled)

    def test_bcctr_with_ctr_decrement_is_invalid(self):
        for word in (asm.bcctr(0, 2), asm.bcctr(8, 2, lk=True), asm.bcctr(16, 0)):
            text, stats = emit([word, asm.blr()])
            self.assertEqual(dict(stats.unhandled), {"bcctr:ctr": 1})
            self.assertIn("DAL_UNHANDLED(0x00010000u, ", text)
            self.assertEqual(stats.edges["tails_dynamic"] + stats.edges["calls_indirect"], 0)


class RegistryTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(emit_core._REGISTRY, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_register_and_return_the_function(self):
        @handler("addi", "addis")
        def fn(op):
            return []

        self.assertIs(emit_core._REGISTRY["addis"], fn)

    def test_duplicate_name(self):
        handler("addi")(lambda op: [])
        with self.assertRaises(ValueError):
            handler("addi")(lambda op: [])
        # A failed registration leaves the other names untouched.
        with self.assertRaises(ValueError):
            handler("addis", "addi")(lambda op: [])
        self.assertNotIn("addis", emit_core._REGISTRY)

    def test_unknown_name(self):
        with self.assertRaises(ValueError):
            handler("nosuchop")

    def test_repeated_name_in_one_call(self):
        with self.assertRaises(ValueError):
            handler("addi", "addi")

    def test_load_handlers_rejects_a_name_from_the_wrong_group(self):
        module_name = "ps3recomp.emit_ops_int_alu"
        fake = types.ModuleType(module_name)
        fake.__spec__ = ModuleSpec(module_name, None)

        def fn(op):
            return []

        fn.__module__ = module_name
        handler("lwz")(fn)                       # `lwz` belongs to group `mem`
        with mock.patch.dict(sys.modules, {module_name: fake}):
            with self.assertRaises(ValueError) as ctx:
                emit_core.load_handlers()
        self.assertIn("lwz", str(ctx.exception))

    def test_load_handlers_accepts_a_name_from_its_own_group(self):
        module_name = "ps3recomp.emit_ops_int_alu"
        fake = types.ModuleType(module_name)
        fake.__spec__ = ModuleSpec(module_name, None)

        def fn(op):
            return []

        fn.__module__ = module_name
        handler("addi")(fn)
        with mock.patch.dict(sys.modules, {module_name: fake}):
            self.assertIs(emit_core.load_handlers()["addi"], fn)


if __name__ == "__main__":
    unittest.main()

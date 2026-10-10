"""Tests for the semantics harness: `ppc_asm.encode`, the C generator and the case rules."""

import contextlib
import io
import os
import unittest
from unittest import mock

from ps3recomp import emit_core, ppu
from ps3recomp.emit_db import GROUPS

from tests import ppc_asm as asm
from tests.ppu_semantics import CODE_BASE, DATA_START, Case, gen
from tests.test_emit_db import TempDirTestCase


def small_value(field: str, index: int) -> int:
    _, high = asm.field_range(field)
    if field in ("DS", "BD", "LI", "D", "SIMM", "SIMM5"):
        return min(4, high)
    return min(1 + index, high)


class EncodeTest(unittest.TestCase):
    def test_round_trip_for_every_spec(self):
        for spec in ppu.ALL_OPS:
            with self.subTest(spec=spec.name):
                fields = {f: small_value(f, i)
                          for i, f in enumerate(asm.operand_fields(spec))}
                fields.update({asm._FLAG_FIELDS[flag]: 1 for flag in spec.flags})
                insn = ppu.decode(asm.encode(spec.name, **fields), 0, strict=True)
                self.assertIs(insn.spec, spec)
                for name, value in fields.items():
                    self.assertEqual(insn.field(name), value, name)

    def test_matches_the_hand_written_helpers(self):
        self.assertEqual(asm.encode("addi", rD=3, rA=1, SIMM=-4), asm.addi(3, 1, -4))
        self.assertEqual(asm.encode("lwz", rD=9, rA=10, D=-4), asm.lwz(9, -4, 10))
        self.assertEqual(asm.encode("ld", rD=3, rA=1, DS=-8), asm.ld(3, -8, 1))
        self.assertEqual(asm.encode("mfspr", rD=3, SPR=8), asm.mflr(3))
        self.assertEqual(asm.encode("mtspr", rS=5, SPR=9), asm.mtctr(5))
        self.assertEqual(asm.encode("rldic", rA=3, rS=4, SH64=33, MB64=40), asm.rldic(3, 4, 33, 40))
        self.assertEqual(asm.encode("rlwinm", rA=3, rS=4, SH=2, MB=0, ME=29),
                         asm.slwi(3, 4, 2))
        self.assertEqual(asm.encode("b", LI=-16, LK=1), asm.b(0x100, 0xF0, lk=True))
        self.assertEqual(asm.encode("bc", BO=16, BI=0, BD=-8), asm.bdnz(0x100, 0xF8))
        self.assertEqual(asm.encode("bclr", BO=20, BI=0), asm.blr())
        self.assertEqual(asm.encode("sc"), asm.sc())
        self.assertEqual(asm.encode("tw", TO=31, rA=0, rB=0), asm.trap())

    def test_split_fields(self):
        insn = ppu.decode(asm.encode("rldicl", rA=1, rS=2, SH64=63, MB64=63), 0, strict=True)
        self.assertEqual((insn.field("SH64"), insn.field("MB64")), (63, 63))
        insn = ppu.decode(asm.encode("mfspr", rD=1, SPR=0x3FF), 0, strict=True)
        self.assertEqual(insn.field("SPR"), 0x3FF)

    def test_rejects_bad_input(self):
        for name, fields in (
                ("nosuchop", {}),
                ("addi", {"Rc": 1}),               # addi has no Rc
                ("addi", {"rD": 32}),
                ("addi", {"SIMM": 0x8000}),
                ("addi", {"nope": 1}),
                ("b", {"LI": 2}),                  # not a multiple of 4
                ("addi", {"rD": True}),
                ("addi", {"rD": 1.0})):
            with self.subTest(name=name, fields=fields), self.assertRaises(ValueError):
                asm.encode(name, **fields)


def run_case(case: Case) -> str:
    """Generate `case` and return the source text."""
    return gen.render([case])[1]


def simple(name="x", words=(asm.blr(),), **kwargs) -> Case:
    return Case(name, "RPCS3 PPUInterpreter.cpp BCLR", tuple(words), **kwargs)


class GeneratorTest(TempDirTestCase):
    def test_writes_both_files_with_one_row_per_case(self):
        cases = gen.load_cases()
        self.assertGreater(len(cases), 0)
        self.assertTrue(gen.generate(cases, self.tmp))
        header = (self.tmp / gen.HEADER_NAME).read_text(encoding="utf-8")
        source = (self.tmp / gen.SOURCE_NAME).read_text(encoding="utf-8")
        self.assertEqual(source.count('    { .name = "'), len(cases))
        self.assertIn("const uint32_t kTox2SemanticsCaseCount = %d;" % len(cases), source)
        for case in cases:
            self.assertIn("TOX2_PPU_FUNC(case_%s);" % case.name, header)
            self.assertIn("init_%s(" % case.name, source)
            self.assertIn("check_%s(" % case.name, source)
        self.assertNotIn("\r", header + source)
        self.assertNotIn(str(self.tmp), header + source)

    def test_rewrite_only_when_changed(self):
        cases = gen.load_cases()
        gen.generate(cases, self.tmp)
        stamps = [(self.tmp / n).stat().st_mtime_ns for n in (gen.HEADER_NAME, gen.SOURCE_NAME)]
        self.assertFalse(gen.generate(cases, self.tmp))
        self.assertEqual(stamps, [(self.tmp / n).stat().st_mtime_ns
                                  for n in (gen.HEADER_NAME, gen.SOURCE_NAME)])

    def test_output_is_deterministic(self):
        cases = gen.load_cases()
        self.assertEqual(gen.render(cases), gen.render(cases))

    def test_main_reports_one_line_errors(self):
        err = io.StringIO()
        bad = [simple(words=(0,))]
        with mock.patch.object(gen, "load_cases", return_value=bad), \
                contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(gen.main(["--out", str(self.tmp)]), 1)
        self.assertEqual(len(err.getvalue().splitlines()), 1)
        self.assertTrue(err.getvalue().startswith("error: "))
        self.assertEqual(os.listdir(self.tmp), [])     # nothing written on error

    def test_main_succeeds(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(gen.main(["--out", str(self.tmp)]), 0)
        self.assertIn("ppu semantics: ", out.getvalue())

    def test_rejections(self):
        reject = {
            "unhandled word": simple(words=(0x00000000,)),
            "unhandled bcctr form": simple(words=(asm.bcctr(16, 0),)),
            "invalid name": simple(name="Bad-Name"),
            "empty name": simple(name=""),
            "missing ref": Case("x", " ", (asm.blr(),)),
            "unknown key": simple(init={"r32": 1}),
            "unknown expect key": simple(expect={"xer": 1}),
            "bad cr field": simple(init={"cr8": 1}),
            "bad flag value": simple(init={"xer_ca": 2}),
            "bad vector": simple(init={"v0": (1, 2, 3)}),
            "bad float": simple(init={"f0": "1.0"}),
            "callee below the callee area": simple(callees={0x10000 + 0x100: (asm.blr(),)}),
            "callee in the data area": simple(callees={DATA_START: (asm.blr(),)}),
            "unaligned callee": simple(callees={0x18002: (asm.blr(),)}),
            "overlapping callees": simple(callees={0x18000: (asm.blr(), asm.blr()),
                                                   0x18004: (asm.blr(),)}),
            "memory below the window": simple(mem_init={DATA_START - 4: b"\0" * 8}),
            "memory above the window": simple(mem_expect={0xEFFFF: b"\0\0"}),
            "empty memory": simple(mem_init={DATA_START: b""}),
            "code into the callee area": simple(words=(asm.blr(),) * 0x2001),
            "switch site is no bcctr": simple(switch_targets={CODE_BASE: (CODE_BASE,)}),
            "unaligned switch target": simple(words=(asm.bctr(),),
                                              switch_targets={CODE_BASE: (CODE_BASE + 2,)}),
        }
        for label, case in reject.items():
            with self.subTest(label), self.assertRaises(gen.CaseError) as ctx:
                gen.render([case])
            self.assertNotIn("\n", str(ctx.exception))

    def test_duplicate_names(self):
        with self.assertRaises(gen.CaseError):
            gen.render([simple("a"), simple("a")])
        # The callee of `a` is named like a case `a_00018000`.
        with self.assertRaises(gen.CaseError):
            gen.render([simple("a", callees={0x18000: (asm.blr(),)}), simple("a_00018000")])

    def test_state_is_rendered(self):
        text = run_case(simple(
            init={"r3": -1, "f1": 1.0, "f2": 0x4000000000000000, "v4": (1, 2, 3, 4), "cr": 0x20000000,
                  "cr7": 2, "lr": 0x100, "vscr_sat": 1, "vrsave": 5, "fpscr": 6},
            expect={"r4": 7, "v5": (0, 0, 0, 0x80000000), "cr3": 8, "xer_ca": 1, "ctr": 3},
            mem_init={DATA_START: b"\x01\x02"}, mem_expect={DATA_START + 4: b"\xff"}))
        for fragment in (
                "ctx->gpr[3] = 0xffffffffffffffffull;",
                "ctx->fpr[1].u64 = 0x3ff0000000000000ull;",
                "ctx->fpr[2].u64 = 0x4000000000000000ull;",
                "ctx->vr[4] = tox2VrFromBeWords(0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u);",
                "tox2Mtcrf(ctx, 0xffu, 0x20000000u);",
                "tox2SetCrField(ctx, 7, 2u);",
                "ctx->lr = 0x0000000000000100ull;",
                "ctx->vscr_sat = 1;",
                "ctx->vrsave = 0x00000005u;",
                "ctx->fpscr = 0x00000006u;",
                '"r4", 16, ctx->gpr[4], 0x0000000000000007ull',
                '"v5", ctx->vr[5], 0x00000000u, 0x00000000u, 0x00000000u, 0x80000000u',
                '"cr3", 1, tox2GetCrField(ctx, 3), 0x8u',
                '"xer_ca", 1, ctx->xer_ca, 0x1u',
                '"ctr", 16, ctx->ctr, 0x0000000000000003ull',
                "__builtin_memcpy(base + 0x00020000u, (const uint8_t[]){0x01, 0x02}, 2u);",
                "checkMem(msg, msg_size, n, base, 0x00020004u, (const uint8_t[]){0xff}, 1u)"):
            self.assertIn(fragment, text)

    def test_cr_is_applied_before_its_fields(self):
        text = run_case(simple(init={"cr7": 1, "cr": 0}))
        self.assertLess(text.index("tox2Mtcrf"), text.index("tox2SetCrField(ctx, 7"))

    def test_return_is_appended_after_fall_through(self):
        # `sc` falls through, so a `blr` follows it; `b` does not.
        sc_text = run_case(simple(words=(asm.sc(),)))
        self.assertIn("TOX2_SYSCALL(0x00010000u);\n    return;\n}", sc_text)
        b_text = run_case(simple(words=(asm.b(CODE_BASE, CODE_BASE + 8), asm.blr(), asm.blr())))
        self.assertIn("goto L_00010008;", b_text)

    def test_callees_and_switch_targets_reach_the_emitter(self):
        words = (asm.bctr(), asm.bl(CODE_BASE + 4, 0x18000), asm.blr())
        text = run_case(simple(words=words, callees={0x18000: (asm.blr(),)},
                               switch_targets={CODE_BASE: (CODE_BASE + 4,)}))
        self.assertIn("TOX2_PPU_FUNC(case_x_00018000)", text)
        self.assertIn("TOX2_CALL(case_x_00018000, 0x00010008u);", text)
        self.assertIn("case 0x00010004u: goto L_00010004;", text)
        self.assertIn("{ 0x00018000u, case_x_00018000 },", text)


def used_names(cases) -> set[str]:
    names = set()
    for case in cases:
        for words in (case.words, *case.callees.values()):
            for word in words:
                spec = ppu.decode(word, 0, strict=True).spec
                if spec is not None:
                    names.add(spec.name)
    return names


class CoverageTest(unittest.TestCase):
    def test_every_handler_has_a_case(self):
        registered = set(GROUPS["control"]) | set(emit_core.load_handlers())
        missing = sorted(registered - used_names(gen.load_cases()))
        self.assertEqual(missing, [], "handlers without a semantics case word")

    def test_case_names_are_unique_and_valid(self):
        names = [c.name for c in gen.load_cases()]
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(gen._NAME_RE.fullmatch(n) for n in names))

    def test_refs_cite_a_reference(self):
        for case in gen.load_cases():
            self.assertTrue(case.ref.startswith(("RPCS3 ", "XenonRecomp ", "PowerPC ")), case.name)


if __name__ == "__main__":
    unittest.main()

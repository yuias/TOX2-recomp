"""Tests for the SPU emitter: control group, stack-free lowering, tables, output and `spu-emit`."""

import contextlib
import io
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import ps3tool
from ps3recomp import spu_emit, spu_emit_core, spu_recover
from ps3recomp.spu_emit_core import EmitStats, emit_image

from tests import spu_asm
from tests.test_spu_images import CONTAINER_VA, RAW_AT, SpuFixtureTest, container_elf, sha
from tests.test_spu_recover import BASE, at, br, brsl, nop, ret, synthetic_recovery, table_program

REPO_ROOT = Path(__file__).resolve().parents[2]


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def stop(code: int) -> int:
    return enc("stop", STOP=code)


def emit(words, handlers=None, **kwargs):
    """Recover `words`, emit them and return (result, text of all functions)."""
    rec = synthetic_recovery(words, **kwargs)
    db = spu_emit.parse_database(spu_recover.build_database(rec))
    result = spu_emit.emit_one(rec.image, db)
    if handlers is not None:
        stats = EmitStats()
        result.records = emit_image(result.model, stats, handlers)
        result.stats = stats
    return result, "".join(r.text for r in result.records)


def function_text(result, entry: int) -> str:
    (record,) = [r for r in result.records if r.entry == entry]
    return record.text


def lines_of(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines()]


class EntrySwitchTest(unittest.TestCase):
    def test_single_dispatchable_block_has_no_switch(self):
        result, text = emit([nop(), nop(), stop(0x102)])
        self.assertNotIn("switch", text)
        self.assertIn("TOX2_SPU_SF_ENTER(0x01000u);", text)
        self.assertIn("(void)ls;", text)
        self.assertEqual(result.stats.functions, 1)

    def test_return_sites_become_cases_of_the_calling_function(self):
        # The call at word 0 leaves through a tail jump; word 1 is entered by the callee's return.
        result, _ = emit([brsl(0, 3), nop(), stop(0x102), ret()])
        text = function_text(result, at(0))
        self.assertIn("switch (ctx->jump_target) {", text)
        self.assertIn("case 0x01000u: TOX2_SPU_SF_ENTER(0x01000u); goto L_01000;", text)
        self.assertIn("case 0x01004u: TOX2_SPU_SF_ENTER(0x01004u); goto L_01004;", text)
        self.assertIn("default: tox2SpuUnhandled(", text)
        # The callee has only its entry.
        self.assertNotIn("switch", function_text(result, at(3)))

    def test_labels_exist_only_where_something_jumps(self):
        result, text = emit([nop(), br(1, 3), nop(), nop(), stop(0x102)])
        self.assertEqual(re.findall(r"^L_\w+:", text, re.M), ["L_0100c:"])
        self.assertEqual(result.stats.labels, 1)

    def test_entry_that_is_not_the_first_block_is_reached_by_goto(self):
        # Word 0 is owned through a backward branch from the entry at word 2.
        words = [nop(), stop(0x102), br(2, 0)]
        result, text = emit(words, entry=2)
        self.assertIn("goto L_01008;", text)
        self.assertEqual(lines_of(text).index("L_01000:") > lines_of(text).index("goto L_01008;"), True)


class TransferTest(unittest.TestCase):
    def test_call_writes_the_link_then_tail_jumps(self):
        result, _ = emit([brsl(0, 3, link=5), nop(), stop(0x102), ret()])
        lines = lines_of(function_text(result, at(0)))
        link = lines.index("ctx->gpr[5] = tox2VrFromBeWords(0x00001004u, 0u, 0u, 0u);")
        self.assertEqual(lines[link + 1], "TOX2_SPU_SF_JUMP(spu_t_raw_0100c, 0x0100cu);")
        self.assertEqual(result.stats.edges["jumps"], 1)

    def test_call_does_not_fall_through(self):
        result, _ = emit([brsl(0, 3), nop(), stop(0x102), ret()])
        text = function_text(result, at(0))
        # The code after the jump is the return site's own block, entered through the switch only.
        self.assertEqual(text.count("TOX2_SPU_SF_JUMP"), 1)
        self.assertEqual(result.stats.edges["fallthrough_jumps"], 0)

    def test_bisl_to_its_own_link_register_reads_the_target_first(self):
        result, _ = emit([enc("bisl", RT=0, RA=0), stop(0x102)])
        lines = lines_of(function_text(result, at(0)))
        read = lines.index("uint32_t target = tox2SpuPreferred(ctx->gpr[0]) & 0x3fffcu;")
        self.assertEqual(lines[read + 1], "ctx->gpr[0] = tox2VrFromBeWords(0x00001004u, 0u, 0u, 0u);")
        self.assertEqual(lines[read + 2], "TOX2_SPU_SF_DISPATCH(0x01000u, target);")

    def test_return_is_a_dispatch_to_the_register(self):
        result, _ = emit([ret()])
        lines = lines_of(function_text(result, at(0)))
        self.assertIn("uint32_t target = tox2SpuPreferred(ctx->gpr[0]) & 0x3fffcu;", lines)
        self.assertIn("TOX2_SPU_SF_DISPATCH(0x01000u, target);", lines)
        self.assertEqual(result.stats.edges["returns"], 1)
        self.assertEqual(result.stats.edges["dispatch_indirect"], 1)

    def test_backward_edge_gets_a_loop_hook_and_forward_edge_does_not(self):
        words = [nop(), enc("brnz", RT=3, addr=at(1), target=at(4)), nop(), br(3, 0), stop(0x102)]
        result, text = emit(words)
        lines = lines_of(text)
        back = lines.index("goto L_01000;")
        self.assertEqual(lines[back - 1], "TOX2_SPU_SF_LOOP(0x0100cu);")
        self.assertIn("if (tox2SpuPreferred(ctx->gpr[3]) != 0) goto L_01010;", lines)
        self.assertEqual(result.stats.loop_hooks, 1)

    def test_conditional_forms_test_the_right_slot(self):
        words = [enc("brz", RT=2, addr=at(0), target=at(5)), enc("brhnz", RT=3, addr=at(1), target=at(5)),
                 enc("biz", RT=4, RA=9), enc("bihnz", RT=6, RA=9), nop(), stop(0x102)]
        _, text = emit(words)
        self.assertIn("if (tox2SpuPreferred(ctx->gpr[2]) == 0) goto L_01014;", text)
        self.assertIn("if ((tox2SpuPreferred(ctx->gpr[3]) & 0xffffu) != 0) goto L_01014;", text)
        self.assertIn("if (tox2SpuPreferred(ctx->gpr[4]) == 0) {", text)
        self.assertIn("if ((tox2SpuPreferred(ctx->gpr[6]) & 0xffffu) != 0) {", text)

    def test_interrupt_bits_change_the_state_before_the_transfer(self):
        words = [enc("bi", RA=0, E=1), enc("bi", RA=0, D=1), enc("iret", RA=0, E=1), enc("iret", RA=0), stop(0x102)]
        result, _ = emit(words, extra=[1, 2, 3])
        for entry, value in ((at(0), 1), (at(1), 0), (at(2), 1)):
            lines = lines_of(function_text(result, entry))
            state = lines.index("ctx->int_enabled = %d;" % value)
            self.assertTrue(lines[state + 1].startswith("TOX2_SPU_SF_DISPATCH"))
        self.assertIn("ctx->srr0 & 0x3fffcu", function_text(result, at(2)))
        self.assertNotIn("int_enabled", function_text(result, at(3)))

    def test_bisled_is_taken_on_a_pending_event_and_falls_through_otherwise(self):
        words = [enc("bisled", RT=0, RA=7), nop(), stop(0x102)]
        result, text = emit(words)
        self.assertIn("if (tox2SpuChannelCount(ctx, ls, 0) != 0) {", text)
        self.assertEqual(result.stats.edges["fallthrough_jumps"], 0)
        # The not-taken path continues inside the function; the next word is also a return site.
        self.assertEqual(result.stats.edges["dispatch_indirect"], 1)
        self.assertIn("case 0x01004u:", text)

    def test_pc_read_idiom_only_writes_the_link(self):
        result, text = emit([enc("brsl", RT=3, addr=at(0), target=at(1)), nop(), stop(0x102)])
        self.assertIn("ctx->gpr[3] = tox2VrFromBeWords(0x00001004u, 0u, 0u, 0u);", text)
        self.assertNotIn("TOX2_SPU_SF_JUMP", text)
        self.assertNotIn("goto", text)

    def test_branch_outside_the_image_is_a_dispatch_to_the_constant(self):
        outside = enc("brasl", RT=0, ABS16=0x2000)
        _, text = emit([outside, nop(), stop(0x102)])
        lines = lines_of(text)
        self.assertIn("TOX2_SPU_SF_DISPATCH(0x01000u, 0x02000u);", lines)
        self.assertLess(lines.index("ctx->gpr[0] = tox2VrFromBeWords(0x00001004u, 0u, 0u, 0u);"),
                        lines.index("TOX2_SPU_SF_DISPATCH(0x01000u, 0x02000u);"))

    def test_fall_through_into_the_next_function_is_a_tail_jump(self):
        # Word 1 is both a call target and the continuation of word 0.
        words = [nop(), nop(), stop(0x102), brsl(3, 1)]
        result, _ = emit(words, extra=[3])
        text = function_text(result, at(0))
        self.assertIn("TOX2_SPU_SF_JUMP(spu_t_raw_01004, 0x01004u);", text)
        self.assertEqual(result.stats.edges["fallthrough_jumps"], 1)

    def test_run_into_padding_is_reported_and_stops_the_run(self):
        result, text = emit([nop(), nop()])
        self.assertEqual(result.stats.invalid_flow, [(at(1), at(2))])
        self.assertIn("tox2SpuUnhandled(ctx, ls, 0x01008u, 0x00000000u);", text)
        self.assertEqual(result.unhandled, 0)


class StopAndNoCodeTest(unittest.TestCase):
    def test_continuing_stop_asks_the_hook(self):
        result, text = emit([stop(0x100), stop(0x102)])
        self.assertIn("if (!tox2SpuStop(ctx, ls, 0x01000u, 0x100u)) TOX2_SPU_SF_LEAVE();", text)
        self.assertIn("tox2SpuStop(ctx, ls, 0x01004u, 0x102u);\n    TOX2_SPU_SF_LEAVE();", text)
        self.assertEqual(result.stats.edges["leaves"], 1)

    def test_thread_ending_stop_does_not_fall_through(self):
        result, _ = emit([stop(0x102), nop()], extra=[1])
        self.assertEqual(result.stats.invalid_flow, [(at(1), at(2))])
        self.assertNotIn("goto", function_text(result, at(0)))

    def test_hints_and_synchronisation_emit_nothing(self):
        words = [nop(), enc("lnop"), enc("sync"), enc("dsync"),
                 enc("hbrr", addr=at(4), site=at(5), target=at(6)), enc("hbr", RA=3, addr=at(5), site=at(7)),
                 nop(), stop(0x102)]
        result, text = emit(words)
        body = [line for line in lines_of(text) if line and not line.startswith("//")]
        self.assertEqual(body, ["TOX2_SPU_FUNC(spu_t_raw_01000) {", "(void)ls;", "TOX2_SPU_SF_ENTER(0x01000u);",
                                "tox2SpuStop(ctx, ls, 0x0101cu, 0x102u);", "TOX2_SPU_SF_LEAVE();", "}"])


class HandlerTest(unittest.TestCase):
    def test_unhandled_words_are_counted_and_emitted_as_hooks(self):
        ai = enc("ai", RT=3, RA=3, I10=1)
        # No handlers: the integer group has real ones by default.
        result, text = emit([ai, ai, enc("a", RT=3, RA=3, RB=3), stop(0x102)], handlers={})
        self.assertEqual(result.stats.unhandled, {"ai": 2, "a": 1})
        self.assertEqual(result.stats.unhandled_sites["ai"], [at(0), at(1)])
        self.assertIn("tox2SpuUnhandled(ctx, ls, 0x01000u, 0x%08xu);" % ai, text)
        self.assertEqual(result.unhandled, 3)

    def test_registered_handler_replaces_the_hook_and_multi_line_output_is_braced(self):
        calls = []

        def ai(op):
            calls.append(op.addr)
            return ["uint32_t t0 = %s;" % op.pref(op.f("RA")), "%s = t0;" % op.gpr(op.f("RT"))]

        result, text = emit([enc("ai", RT=3, RA=4, I10=1), stop(0x102)], handlers={"ai": ai})
        self.assertEqual(calls, [at(0)])
        self.assertIn("{\n        uint32_t t0 = tox2SpuPreferred(ctx->gpr[4]);\n        ctx->gpr[3] = t0;\n    }", text)
        self.assertEqual(result.unhandled, 0)

    def test_handler_can_decline_with_a_reason(self):
        def ai(op):
            return op.unsupported("imm")

        result, _ = emit([enc("ai", RT=3, RA=4, I10=1), stop(0x102)], handlers={"ai": ai})
        self.assertEqual(result.stats.unhandled, {"ai:imm": 1})

    def test_registry_rejects_unknown_core_and_duplicate_names(self):
        with self.assertRaises(ValueError):
            spu_emit_core.handler("nonsense")
        with self.assertRaises(ValueError):
            spu_emit_core.handler("brsl")
        with self.assertRaises(ValueError):
            spu_emit_core.handler("ai", "ai")

    def test_per_function_lowering_override_is_honoured(self):
        rec = synthetic_recovery([brsl(0, 3), nop(), stop(0x102), ret()],
                                 lowering_overrides={"0x%x" % at(3): "stackfree"})
        db = spu_emit.parse_database(spu_recover.build_database(rec))
        result = spu_emit.emit_one(rec.image, db)
        self.assertEqual(result.stats.functions, 2)
        self.assertIn("TOX2_SPU_SF_DISPATCH", function_text(result, at(3)))

    def test_lowering_selection(self):
        self.assertEqual(spu_emit_core.get_lowering("stackfree").name, "stackfree")
        with self.assertRaisesRegex(ValueError, "lowering not implemented"):
            spu_emit_core.get_lowering("hostcall")


class ModelTest(unittest.TestCase):
    def test_overlapping_functions_are_rejected(self):
        code = synthetic_recovery([nop(), stop(0x102)]).code
        lowering = lambda entry: spu_emit_core.get_lowering("stackfree")
        with self.assertRaisesRegex(ValueError, "owned by functions"):
            spu_emit_core.ImageModel("t", code, {at(0): [at(0), at(1)], at(1): [at(1)]}, [], lowering)
        with self.assertRaisesRegex(ValueError, "not owned code"):
            spu_emit_core.ImageModel("t", code, {at(0): [at(0)]}, [at(1)], lowering)

    def test_function_entries_are_always_dispatchable(self):
        result, _ = emit([brsl(0, 3), nop(), stop(0x102), ret()])
        self.assertEqual(sorted(result.model.dispatchable), [at(0), at(1), at(3)])


class OutputTest(unittest.TestCase):
    PROGRAM = [brsl(0, 3), nop(), stop(0x102), ret()]

    def test_tables_are_sorted_and_check_words_stay_inside_the_function(self):
        result, _ = emit(self.PROGRAM)
        text = spu_emit.render_tables([result.model], [result.image])
        rows = re.findall(r"\{ 0x(\w+)u, (\d)u, \{ ([^}]*) \}, (\w+) \}", text)
        self.assertEqual([int(r[0], 16) for r in rows], [at(0), at(1), at(3)])
        counts = {int(r[0], 16): int(r[1]) for r in rows}
        self.assertEqual(counts, {at(0): 3, at(1): 2, at(3): 1})
        first = [w for w in rows[0][2].split(", ")]
        self.assertEqual(first, ["0x%08xu" % w for w in self.PROGRAM[:3]])
        self.assertIn("const uint32_t kTox2SpuImageCount = 1u;", text)
        self.assertIn("1u, kSegments_t_raw, 3u, kEntries_t_raw", text)

    def test_header_declares_every_function_and_the_lowering_header(self):
        result, _ = emit(self.PROGRAM)
        text = spu_emit.render_funcs_header([result.model], {"t_raw": result.records})
        self.assertIn('#include "tox2/spu_lower_stackfree.h"', text)
        self.assertIn("TOX2_SPU_FUNC(spu_t_raw_01000);", text)
        self.assertIn("TOX2_SPU_FUNC(spu_t_raw_0100c);", text)

    def test_output_is_deterministic(self):
        first = spu_emit.output_files([emit(self.PROGRAM)[0]])
        second = spu_emit.output_files([emit(self.PROGRAM)[0]])
        self.assertEqual(first, second)
        for text in first.values():
            self.assertNotIn("\r", text)
            text.encode("ascii")
        self.assertEqual(sorted(first), ["spu_funcs.h", "spu_sources.cmake", "spu_t_raw_000.c", "spu_tables.c"])

    def test_chunks_close_after_the_limit_and_never_split_a_function(self):
        records = [spu_emit_core.FunctionRecord(i, "f%d" % i, n, "") for i, n in enumerate([3, 3, 2, 5, 1])]
        chunks = spu_emit.plan_chunks(records, 5)
        self.assertEqual([[r.name for r in c] for c in chunks], [["f0", "f1"], ["f2", "f3"], ["f4"]])
        self.assertEqual(spu_emit.plan_chunks([], 5), [])

    def test_comments_add_the_disassembly(self):
        rec = synthetic_recovery([enc("ai", RT=3, RA=4, I10=1), stop(0x102)])
        db = spu_emit.parse_database(spu_recover.build_database(rec))
        result = spu_emit.emit_one(rec.image, db, comments=True)
        self.assertIn("// 01000: ", result.records[0].text)
        self.assertIn("ai $3,$4,1", result.records[0].text)


@unittest.skipUnless(shutil.which("clang"), "clang is not installed")
class GeneratedCodeCompilesTest(unittest.TestCase):
    def test_table_program_compiles_with_the_codegen_warnings_as_errors(self):
        result, _ = emit(table_program())
        files = spu_emit.output_files([result])
        with tempfile.TemporaryDirectory() as tmp:
            for name, text in files.items():
                (Path(tmp) / name).write_text(text, newline="\n")
            for name in files:
                if name.endswith(".c"):
                    proc = subprocess.run(
                        ["clang", "-c", "-o", str(Path(tmp) / "out.o"), "-std=c23", "-O1", "-march=x86-64-v3",
                         "-ffp-contract=off", "-fpatchable-function-entry=8", "-Wall", "-Wextra", "-Werror",
                         "-I", str(REPO_ROOT / "runtime" / "include"), "-I", tmp, str(Path(tmp) / name)],
                        capture_output=True, text=True)
                    self.assertEqual(proc.returncode, 0, proc.stderr)


class CommandTest(SpuFixtureTest):
    """`spu-recover` then `spu-emit` on a synthetic libsre that holds one jump-table program."""

    def setUp(self):
        super().setUp()
        self.words = table_program()
        blob = b"".join(w.to_bytes(4, "big") for w in self.words)
        self.libsre = container_elf({RAW_AT: blob})
        self.libsre_path = self.write("libsre.reloc.elf", self.libsre)
        self.config_path = self.write_json("target.json", {"eboot_elf": {"sha256": sha(self.eboot)},
                                                           "libsre_reloc_elf": {"sha256": sha(self.libsre)}})
        self.entry = {"name": "t_table", "container": "libsre", "ea": "0x%x" % (CONTAINER_VA + RAW_AT),
                      "kind": "raw", "size": "0x%x" % len(blob), "ls_base": "0x%x" % BASE,
                      "entry": "0x%x" % BASE, "role": "spurs_policy_module", "lowering": "stackfree",
                      "gate": True, "extra_entries": []}
        self.registry = self.write_json("registry.json", {"schema": 1, "images": [self.entry]})
        self.db = self.tmp / "db"
        self.out = self.tmp / "out"
        self.report = self.tmp / "analysis" / "report.json"
        self.assertEqual(self.tool("spu-recover", "--out", str(self.db),
                                   "--report", str(self.tmp / "recover.json"))[0], 0)

    def tool(self, command, *extra):
        argv = [command, "--eboot", str(self.eboot_path), "--libsre", str(self.libsre_path),
                "--registry", str(self.registry), "--target-config", str(self.config_path), *extra]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ps3tool.main(argv)
        return code, out.getvalue(), err.getvalue()

    def emit_cli(self, *extra):
        return self.tool("spu-emit", "--db-dir", str(self.db), "--out", str(self.out),
                         "--report", str(self.report), *extra)

    def test_writes_chunks_tables_sources_and_a_report(self):
        code, out, err = self.emit_cli()
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(sorted(p.name for p in self.out.iterdir()),
                         ["spu_funcs.h", "spu_sources.cmake", "spu_t_table_000.c", "spu_tables.c"])
        self.assertIn("t_table: functions 1 ", out)
        self.assertIn("output: 4 files, 4 written", out)
        report = json.loads(self.report.read_text())
        (image,) = report["images"]
        self.assertEqual(image["image"], "t_table")
        self.assertEqual(image["dispatchable"], 5)
        self.assertGreater(image["unhandled"]["total"], 0)
        self.assertEqual(report["gate_unhandled"], image["unhandled"]["total"])
        self.assertEqual(image["edges"]["dispatch_indirect"], 2)

    def test_second_run_rewrites_nothing(self):
        self.emit_cli()
        files = [*self.out.iterdir(), self.report]
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in files}
        code, out, _ = self.emit_cli()
        self.assertEqual(code, 0)
        self.assertIn("0 written", out)
        self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in files})

    def test_gate_exits_9_when_the_gate_images_have_unhandled_words(self):
        code, out, _ = self.emit_cli("--max-unhandled", "0")
        self.assertEqual(code, 9)
        self.assertIn("gate unhandled:", out)
        code, _, _ = self.emit_cli("--max-unhandled", "1000")
        self.assertEqual(code, 0)

    def test_ungated_image_does_not_count_and_is_listed(self):
        self.entry["gate"] = False
        self.registry = self.write_json("registry.json", {"schema": 1, "images": [self.entry]})
        self.tool("spu-recover", "--out", str(self.db), "--report", str(self.tmp / "recover.json"))
        code, out, _ = self.emit_cli("--max-unhandled", "0")
        self.assertEqual(code, 0)
        self.assertRegex(out, r"ungated unhandled: t_table [1-9]\d*")

    def test_census_writes_no_files_but_prints_counts(self):
        code, out, _ = self.emit_cli("--census")
        self.assertEqual(code, 0)
        self.assertFalse(self.out.exists())
        self.assertFalse(self.report.exists())
        self.assertRegex(out, r"t_table: functions 1 instructions \d+ dispatchable 5 unhandled \d+")
        self.assertIn("  top: ", out)

    def test_database_of_other_bytes_exits_3(self):
        path = self.db / "t_table.json"
        doc = json.loads(path.read_text())
        doc["input"]["image_sha256"] = "0" * 64
        path.write_text(json.dumps(doc))
        code, _, err = self.emit_cli()
        self.assertEqual(code, 3)
        self.assertIn("image SHA-256", err)
        self.assertFalse(self.out.exists())

    def test_missing_database_exits_1(self):
        (self.db / "t_table.json").unlink()
        code, _, err = self.emit_cli()
        self.assertEqual(code, 1)
        self.assertIn("spu-recover", err)

    def test_registry_lowering_that_does_not_exist_is_refused(self):
        self.entry["lowering"] = "hostcall"
        self.registry = self.write_json("registry.json", {"schema": 1, "images": [self.entry]})
        code, _, err = self.emit_cli()
        self.assertEqual(code, 2)
        self.assertIn("lowering not implemented", err)

    def test_stale_chunk_of_the_image_is_removed(self):
        self.emit_cli()
        stale = self.out / "spu_t_table_007.c"
        stale.write_text("// old\n")
        other = self.out / "spu_other_000.c"
        other.write_text("// kept\n")
        self.emit_cli()
        self.assertFalse(stale.exists())
        self.assertTrue(other.exists())


if __name__ == "__main__":
    unittest.main()

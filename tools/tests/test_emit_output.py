"""Tests for the `emit` output writer: files, determinism, chunking, trampolines, report."""

import contextlib
import hashlib
import io
import json
import os
import re
import unittest
from pathlib import Path

from ps3recomp import cell, emit, emit_output
from ps3recomp.elf import ElfFile
from ps3recomp.emit_db import GROUPS, DbThunk

from tests import ppc_asm as asm
from tests.elf_fixture import PS3_TEXT_VA, build_ps3_fixture
from tests.test_emit_db import TempDirTestCase, db_doc, hexs

TEXT = PS3_TEXT_VA
STD_R2_40_R1 = 62 << 26 | 2 << 21 | 1 << 16 | 40        # std r2,40(r1)
FILES = ["ppu_chunk_000.c", "ppu_funcs.h", "ppu_imports.c", "ppu_tables.c", "sources.cmake"]
REPORT_KEYS = ["schema", "generator", "input", "functions", "instructions", "import_trampolines",
               "thunks_without_toc_save", "chunks", "largest_functions", "unhandled", "edges",
               "dynamic_targets", "labels", "loop_hooks", "output_bytes", "elapsed_s"]


class PlanChunksTest(unittest.TestCase):
    def test_balanced_split(self):
        self.assertEqual(emit_output.plan_chunks([3, 2, 2], 3), [(0, 1), (1, 2), (2, 3)])

    def test_single_chunk(self):
        self.assertEqual(emit_output.plan_chunks([3, 2, 2], 1), [(0, 3)])

    def test_fewer_chunks_than_requested(self):
        self.assertEqual(emit_output.plan_chunks([5, 5], 16), [(0, 1), (1, 2)])

    def test_big_function_does_not_create_empty_chunks(self):
        plan = emit_output.plan_chunks([100, 1, 1, 1], 4)
        self.assertEqual(plan[0], (0, 1))
        self.assertTrue(all(start < end for start, end in plan))
        self.assertLessEqual(len(plan), 4)
        self.assertEqual(plan[-1][1], 4)

    def test_no_functions(self):
        self.assertEqual(emit_output.plan_chunks([], 4), [])


class TrampolineTest(unittest.TestCase):
    THUNK = DbThunk(0x000DCF50, "sys_io", 0x1CF98800, "cellPadInit", 0x1004A4)
    WITH_SAVE = [asm.li(12, 0), asm.oris(12, 12, 0x10), asm.lwz(12, 0x4A4, 12), STD_R2_40_R1,
                 asm.lwz(0, 0, 12), asm.lwz(2, 4, 12), asm.mtctr(0), asm.bctr()]

    def test_with_toc_save(self):
        text, saves = emit_output.render_trampoline(self.THUNK, 7, self.WITH_SAVE)
        self.assertTrue(saves)
        self.assertEqual(text.splitlines(), [
            "// sys_io cellPadInit 0x1cf98800",
            "TOX2_PPU_FUNC(imp_000dcf50) {",
            "    TOX2_ENTER(0x000dcf50u);",
            "    tox2Write64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull), ctx->gpr[2]);",
            "    TOX2_HLE(7);",
            "}"])

    def test_without_toc_save(self):
        words = [w for w in self.WITH_SAVE if w != STD_R2_40_R1] + [asm.nop()]
        text, saves = emit_output.render_trampoline(self.THUNK, 0, words)
        self.assertFalse(saves)
        self.assertNotIn("tox2Write64", text)
        self.assertIn("TOX2_HLE(0);", text)

    def test_other_store_is_not_a_toc_save(self):
        other = 62 << 26 | 3 << 21 | 1 << 16 | 40            # std r3,40(r1)
        self.assertIsNone(emit_output.toc_save_statement([other], 0))
        self.assertIsNone(emit_output.toc_save_statement([62 << 26 | 2 << 21 | 4 << 16], 0))

    def test_negative_displacement(self):
        word = 62 << 26 | 2 << 21 | 1 << 16 | (0x10000 - 8)  # std r2,-8(r1)
        self.assertIn("0xfffffffffffffff8ull", emit_output.toc_save_statement([word], 0))

    def test_unreadable_thunk(self):
        text, saves = emit_output.render_trampoline(self.THUNK, 1, None)
        self.assertFalse(saves)
        self.assertIn("TOX2_HLE(1);", text)

    def test_unnamed_import_and_string_escaping(self):
        thunk = DbThunk(0x1000, "m", 5, None, 0)
        text, _ = emit_output.render_trampoline(thunk, 0, None)
        self.assertTrue(text.startswith("// m - 0x00000005\n"))
        self.assertEqual(emit_output.c_string(None), "nullptr")
        self.assertEqual(emit_output.c_string('a"b\\c'), '"a\\042b\\134c"')


class EmitCliTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        data = build_ps3_fixture()
        self.elf_path = self.tmp / "fixture.elf"
        self.elf_path.write_bytes(data)
        self.sha = hashlib.sha256(data).hexdigest()
        self.text = cell.find_text_range(ElfFile.from_bytes(data))
        self.out = self.tmp / "out"
        self.report = self.tmp / "report.json"

    def make_db(self, functions=None):
        return self.write_db(db_doc(sha=self.sha, text=self.text, functions=functions))

    def run_cli(self, db_path, *extra):
        out, err = io.StringIO(), io.StringIO()
        argv = [str(self.elf_path), "--db", str(db_path), "--out", str(self.out),
                "--report", str(self.report), "--census-out", str(self.tmp / "census.json")]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = emit.main(argv + list(extra))
        return code, out.getvalue(), err.getvalue()

    def snapshot(self):
        return {p.name: p.read_bytes() for p in sorted(self.out.iterdir())}

    def three_functions(self):
        return self.make_db([(TEXT, [(TEXT, TEXT + 0xC)]),
                             (TEXT + 0xC, [(TEXT + 0xC, TEXT + 0x14)]),
                             (TEXT + 0x14, [(TEXT + 0x14, TEXT + 0x1C)])])

    def test_files_and_stdout(self):
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x1C)])])
        code, out, err = self.run_cli(db)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), FILES)
        lines = out.splitlines()
        self.assertEqual(lines[0], "emit: 1 functions (7 insns) in 1 chunks, "
                                   "1 import trampolines (1 without TOC save)")
        self.assertTrue(lines[1].startswith("unhandled: "))
        self.assertTrue(lines[2].startswith("calls: direct 0, "))
        self.assertTrue(lines[3].startswith("output: "))
        self.assertTrue(lines[4].startswith("elapsed: "))
        for name, data in self.snapshot().items():
            self.assertTrue(data.startswith(
                (b"# " if name == "sources.cmake" else b"// ") + b"Generated by ps3tool emit "
                b"from executable " + self.sha.encode() + b"; do not edit.\n"), name)
            self.assertNotIn(b"\r", data)
            self.assertNotIn(str(self.tmp).encode(), data)

    def test_sources_cmake_is_relative(self):
        self.run_cli(self.three_functions(), "--chunks", "3")
        text = (self.out / "sources.cmake").read_text(encoding="utf-8")
        entries = re.findall(r'"([^"]+)"', text)
        self.assertEqual(entries, ["${CMAKE_CURRENT_LIST_DIR}/ppu_chunk_00%d.c" % i
                                   for i in range(3)]
                         + ["${CMAKE_CURRENT_LIST_DIR}/ppu_imports.c",
                            "${CMAKE_CURRENT_LIST_DIR}/ppu_tables.c"])
        self.assertNotRegex(text, r"[A-Za-z]:[\\/]")

    def test_rerun_is_byte_identical_and_keeps_mtimes(self):
        db = self.three_functions()
        self.run_cli(db)
        first = self.snapshot()
        stamp = 1_000_000_000
        for path in self.out.iterdir():
            os.utime(path, (stamp, stamp))
        self.run_cli(db)
        self.assertEqual(self.snapshot(), first)
        self.assertEqual({p.stat().st_mtime for p in self.out.iterdir()}, {float(stamp)})

    def test_fewer_chunks_remove_stale_files(self):
        db = self.three_functions()
        self.run_cli(db, "--chunks", "3")
        self.assertEqual([p.name for p in sorted(self.out.glob("ppu_chunk_*.c"))],
                         ["ppu_chunk_000.c", "ppu_chunk_001.c", "ppu_chunk_002.c"])
        self.run_cli(db, "--chunks", "1")
        self.assertEqual([p.name for p in sorted(self.out.glob("ppu_chunk_*.c"))],
                         ["ppu_chunk_000.c"])
        self.assertNotIn("ppu_chunk_001.c", (self.out / "sources.cmake").read_text())

    def test_mapping_table_is_sorted_and_complete(self):
        self.run_cli(self.three_functions())
        text = (self.out / "ppu_tables.c").read_text(encoding="utf-8")
        guests = [int(m, 16) for m in re.findall(r"^    \{ 0x([0-9a-f]{8})u, (?:fn|imp)_", text,
                                                 re.M)]
        self.assertEqual(guests, sorted(guests))
        self.assertEqual(len(guests), 4)
        self.assertIn("kTox2PpuFuncMappingCount = 4;", text)
        self.assertIn("kTox2PpuImportCount = 1;", text)
        self.assertIn('{ 0x00030000u, 0x00001234u, "cellFoo", nullptr },', text)
        self.assertIn('kTox2PpuExecutableSha256[] = "%s";' % self.sha, text)

    def test_max_unhandled_exits_9_after_writing(self):
        # The all-zero word after blr is invalid code, so it stays unhandled whatever the handlers cover.
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x20)])])
        code, _, _ = self.run_cli(db, "--max-unhandled", "0")
        self.assertEqual(code, 9)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), FILES)
        self.assertTrue(self.report.is_file())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        total = report["unhandled"]["total"]
        self.assertGreater(total, 0)
        code, _, _ = self.run_cli(db, "--max-unhandled", str(total))
        self.assertEqual(code, 0)

    def test_report(self):
        code, _, _ = self.run_cli(self.three_functions())
        self.assertEqual(code, 0)
        raw = self.report.read_bytes()
        self.assertTrue(raw.endswith(b"}\n"))
        report = json.loads(raw.decode("utf-8"))
        self.assertEqual(list(report), REPORT_KEYS)
        self.assertEqual((report["schema"], report["generator"]), (1, "ps3tool emit"))
        self.assertEqual(report["input"]["executable_sha256"], self.sha)
        self.assertEqual(report["input"]["functions_db_sha256"],
                         hashlib.sha256(self.tmp.joinpath("functions.json").read_bytes())
                         .hexdigest())
        self.assertEqual((report["functions"], report["instructions"],
                          report["import_trampolines"]), (3, 7, 1))
        self.assertEqual(report["thunks_without_toc_save"], [hexs(0x30000)])
        self.assertEqual([c["functions"] for c in report["chunks"]], [1, 1, 1])
        self.assertEqual(report["largest_functions"][0], {"entry": hexs(TEXT), "instructions": 3})
        self.assertEqual(list(report["edges"]), list(emit_output.EDGE_KEYS))
        self.assertEqual(report["edges"]["syscalls"], 1)
        self.assertEqual(report["edges"]["returns"], 1)
        self.assertEqual(list(report["unhandled"]["by_group"]),
                         list(GROUPS))
        self.assertEqual(report["unhandled"]["total"],
                         sum(report["unhandled"]["by_mnemonic"].values()))
        self.assertEqual(report["output_bytes"],
                         sum(len(d) for d in self.snapshot().values()))

    def test_comments_option(self):
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x1C)])])
        self.run_cli(db, "--comments")
        text = (self.out / "ppu_chunk_000.c").read_text(encoding="utf-8")
        self.assertIn("// 0x00010000: 7c0802a6", text)


if __name__ == "__main__":
    unittest.main()

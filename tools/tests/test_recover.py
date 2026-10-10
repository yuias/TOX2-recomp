"""Tests for the `recover` command: pipeline wiring, function DB, report and exit codes."""

import contextlib
import hashlib
import io
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import cell, recover
from ps3recomp.elf import ElfFile
from ps3recomp.ghidra_summary import write_compact_json

from tests import ppc_asm as asm
from tests.elf_fixture import (PS3_DATA_VA, PS3_TEXT_VA, FixtureSegment, build_elf64_be,
                               build_ps3_fixture)

TEXT = PS3_TEXT_VA
DATA = PS3_DATA_VA
TOC = 0x28000


def at(index: int) -> int:
    return TEXT + 4 * index


def build_code_elf(words) -> bytes:
    """Image with `.text` = the first executable segment and an entry OPD for its first word."""
    text = struct.pack(">%dI" % len(words), *words)
    data = struct.pack(">II", TEXT, TOC)
    return build_elf64_be(entry=DATA, segments=[
        FixtureSegment(1, TEXT, text, flags=5),
        FixtureSegment(1, DATA, data, memsz=0x10000, flags=6),
    ])


def shared_tail_words():
    # f0: nop; b T   f1 (index 3): nop; b T   T (index 6): nop; nop; blr
    return [asm.nop(), asm.b(at(1), at(6)), asm.blr(),
            asm.nop(), asm.b(at(4), at(6)), asm.blr(),
            asm.nop(), asm.nop(), asm.blr()]


def hexs(value: int) -> str:
    return "0x%08x" % value


def summary_doc(elf_sha: str, text: tuple[int, int], functions) -> dict:
    return {
        "schema": 1,
        "generator": "ps3tool ghidra-summary",
        "source": {"ghidra_version": "test", "language": "test", "compiler_spec": "default",
                   "executable_sha256": elf_sha},
        "text": {"start": hexs(text[0]), "end": hexs(text[1])},
        "word_kinds": {"code": 0, "data": 0, "undefined": 0},
        "functions": [{"entry": hexs(entry), "name": name, "thunk": False,
                       "body": [[hexs(entry), hexs(entry + 4)]]} for entry, name in functions],
        "code_ranges": [],
        "data_ranges": [],
        "data_refs": [],
        "bctr_sites": [],
    }


class RecoverTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        # Keep every default path inside the temp dir: nothing may reach the repository.
        for name, leaf in (("DEFAULT_SUMMARY", "default_summary.json"),
                           ("DEFAULT_EXTRA_SEEDS", "default_extra.json"),
                           ("DEFAULT_OUTPUT", "default_functions.json"),
                           ("DEFAULT_REPORT", "default_report.json"),
                           ("DEFAULT_SEEDS_OUT", "default_seeds.txt")):
            patcher = mock.patch.object(recover, name, self.tmp / leaf)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.out_path = self.tmp / "out" / "functions.json"
        self.report_path = self.tmp / "out" / "report.json"
        self.seeds_path = self.tmp / "out" / "seeds.txt"

    def write_elf(self, raw: bytes) -> Path:
        path = self.tmp / "input.elf"
        path.write_bytes(raw)
        self.elf_path = path
        self.elf_sha = hashlib.sha256(raw).hexdigest()
        elf = ElfFile.from_bytes(raw)
        self.text = cell.find_text_range(elf)
        return path

    def write_summary(self, functions, name="summary.json", **overrides) -> Path:
        doc = summary_doc(self.elf_sha, self.text, functions)
        doc.update(overrides)
        path = self.tmp / name
        write_compact_json(path, doc)
        return path

    def run_recover(self, *args, plain=False):
        argv = [str(self.elf_path)]
        if not plain:
            argv += ["-o", str(self.out_path), "--report", str(self.report_path),
                     "--seeds-out", str(self.seeds_path)]
        argv += [str(a) for a in args]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = recover.main(argv)
        return code, out.getvalue(), err.getvalue()

    def db(self) -> dict:
        return json.loads(self.out_path.read_text(encoding="utf-8"))

    def report(self) -> dict:
        return json.loads(self.report_path.read_text(encoding="utf-8"))


class CanonicalFixtureTest(RecoverTestBase):
    def setUp(self):
        super().setUp()
        self.write_elf(build_ps3_fixture())

    def test_no_ghidra_database(self):
        code, out, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 0, err)
        db = self.db()
        self.assertEqual(list(db), ["schema", "generator", "input", "text", "code", "toc",
                                    "functions",
                                    "import_thunks", "jump_tables", "unresolved_indirect"])
        self.assertEqual((db["schema"], db["generator"]), (1, "ps3tool recover"))
        self.assertEqual(db["input"], {"executable_sha256": self.elf_sha,
                                       "ghidra_summary_sha256": None, "extra_seeds": 0})
        self.assertEqual(db["text"], {"start": hexs(TEXT), "end": hexs(TEXT + 0x20)})
        self.assertEqual(db["code"], db["text"])
        self.assertEqual(db["toc"], hexs(TOC))
        # The first function falls through into the second one's OPD start.
        self.assertEqual(db["functions"], [
            {"entry": hexs(at(0)), "name": None, "sources": ["entry", "export", "opd"],
             "ranges": [[hexs(at(0)), hexs(at(6))]]},
            {"entry": hexs(at(6)), "name": None, "sources": ["opd"],
             "ranges": [[hexs(at(6)), hexs(at(7))]]},
        ])
        self.assertEqual(db["jump_tables"], [])
        self.assertEqual(db["unresolved_indirect"], [])
        self.assertEqual(len(db["import_thunks"]), 1)
        thunk = db["import_thunks"][0]
        self.assertEqual(set(thunk), {"addr", "module", "fnid", "name", "slot"})
        self.assertEqual((thunk["module"], thunk["slot"]), ("cellSysutil", hexs(0x20150)))

    def test_stdout_lines(self):
        code, out, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 0, err)
        lines = out.splitlines()
        self.assertEqual([ln.split(":")[0] for ln in lines],
                         ["seeds", "thunks", "jump tables", "partition", "coverage",
                          "syscalls", "elapsed"])
        self.assertRegex(lines[0], re.compile(
            r"^seeds: ghidra \d+, opd \d+, bl \d+ \(\d+ not ghidra\), entry \d+, export \d+, "
            r"data_ptr \d+, ghidra_data_ref \d+, prologue (\d+|off), manual \d+; "
            r"union\(ghidra\+bl\+opd\) \d+; rejected \d+$"))
        self.assertIn("ghidra 0, opd 2,", lines[0])
        self.assertIn("prologue off", lines[0])
        self.assertEqual(lines[3],
                         "partition: 1 rounds, converged, 2 functions, 0 promoted, "
                         "0 call-discovered")
        self.assertRegex(lines[4], r"^coverage: owned \d+ / \d+ words \(\d+\.\d%\); "
                         r"uncovered \d+ words: zero \d+, padding \d+, jump_table \d+, "
                         r"data \d+, code \d+$")
        self.assertEqual(lines[5], "syscalls: 1 sites in .text, 0 outside, 1 in functions, "
                         "1 resolved by CFG, 0 unresolved, 0 disagree with probe")
        self.assertRegex(lines[6], r"^elapsed: \d+\.\d s$")

    def test_ghidra_lines_and_section_need_a_summary(self):
        summary = self.write_summary([(at(0), None), (at(6), None)])
        code, out, err = self.run_recover("--ghidra-summary", summary)
        self.assertEqual(code, 0, err)
        lines = out.splitlines()
        self.assertEqual([ln.split(":")[0] for ln in lines],
                         ["seeds", "thunks", "jump tables", "partition", "coverage",
                          "ghidra extents", "ghidra code not owned", "syscalls", "elapsed"])
        self.assertEqual(lines[5], "ghidra extents: equal 1, ours_larger 1, ours_smaller 0, "
                         "different 0, missing 0; cell-truncated 0, extended 0")
        report = self.report()
        self.assertEqual(list(report)[-3:], ["coverage", "ghidra", "syscalls"])
        self.assertEqual(report["ghidra"]["extents"]["ours_larger"]["count"], 1)
        self.assertEqual(report["coverage"]["owned_words"], 7)
        self.run_recover("--no-ghidra")
        self.assertNotIn("ghidra", self.report())

    def test_unreadable_thunk_stub_counts_as_slot_mismatch(self):
        # The fixture's slot values are zero, so the single thunk lies outside any segment.
        self.run_recover("--no-ghidra")
        report = self.report()
        self.assertEqual(report["thunks"]["count"], 1)
        self.assertEqual(report["thunks"]["slot_mismatches"], [hexs(0)])

    def test_report_sections(self):
        self.run_recover("--no-ghidra")
        report = self.report()
        self.assertEqual(list(report),
                         ["seeds", "thunks", "jump_tables", "partition", "coverage", "syscalls"])
        seeds = report["seeds"]
        self.assertEqual(seeds["counts"]["opd"], 2)
        self.assertEqual(seeds["by_source"], {"entry": 1, "export": 1, "opd": 2})
        self.assertEqual(report["jump_tables"]["bcctr_sites"], 0)
        part = report["partition"]
        self.assertEqual((part["rounds"], part["converged"], part["functions"]), (1, True, 2))
        self.assertEqual(part["by_source"], {"entry": 1, "export": 1, "opd": 2})
        self.assertEqual(part["tail_edges_by_kind"], {"fallthrough": 1})
        self.assertEqual(part["fallthrough_tail_edges"],
                         [{"function": hexs(at(0)), "site": hexs(at(5)), "target": hexs(at(6))}])
        self.assertEqual(part["promoted"], [])

    def test_report_is_indented_json_with_trailing_newline(self):
        self.run_recover("--no-ghidra")
        text = self.report_path.read_text(encoding="utf-8")
        self.assertTrue(text.startswith('{\n  "seeds"'))
        self.assertTrue(text.endswith("}\n"))
        self.assertNotIn("\r", text)

    def test_rerun_is_byte_identical(self):
        summary = self.write_summary([(at(0), "main"), (at(6), None)])
        self.assertEqual(self.run_recover("--ghidra-summary", summary)[0], 0)
        first = self.out_path.read_bytes()
        self.out_path.unlink()
        self.assertEqual(self.run_recover("--ghidra-summary", summary)[0], 0)
        self.assertEqual(self.out_path.read_bytes(), first)
        self.assertTrue(first.endswith(b"}\n"))
        self.assertNotIn(b"\r", first)

    def test_with_summary_names_sources_and_hash(self):
        summary = self.write_summary(
            [(at(6), None), (at(0), "main"), (0x10, "outside_text")])
        code, out, err = self.run_recover("--ghidra-summary", summary)
        self.assertEqual(code, 0, err)
        db = self.db()
        self.assertEqual(db["input"]["ghidra_summary_sha256"],
                         hashlib.sha256(summary.read_bytes()).hexdigest())
        self.assertEqual([(f["entry"], f["name"], f["sources"]) for f in db["functions"]], [
            (hexs(at(0)), "main", ["entry", "export", "ghidra", "opd"]),
            (hexs(at(6)), None, ["ghidra", "opd"]),
        ])
        rejected = self.report()["seeds"]["rejected"]
        self.assertEqual(rejected, [{"addr": hexs(0x10), "source": "ghidra",
                                     "reason": "outside_text"}])
        self.assertIn("ghidra 3, opd 2,", out)
        self.assertIn("rejected 1", out.splitlines()[0])

    def test_seeds_out_lists_starts_ghidra_lacks(self):
        summary = self.write_summary([(at(0), None)])
        self.assertEqual(self.run_recover("--ghidra-summary", summary)[0], 0)
        self.assertEqual(self.seeds_path.read_text(encoding="utf-8"), hexs(at(6)) + "\n")

    def test_seeds_out_without_ghidra_lists_every_function(self):
        self.assertEqual(self.run_recover("--no-ghidra")[0], 0)
        self.assertEqual(self.seeds_path.read_bytes(),
                         ("%s\n%s\n" % (hexs(at(0)), hexs(at(6)))).encode("ascii"))

    def test_seeds_out_is_empty_when_ghidra_has_everything(self):
        summary = self.write_summary([(at(0), None), (at(6), None)])
        self.assertEqual(self.run_recover("--ghidra-summary", summary)[0], 0)
        self.assertEqual(self.seeds_path.read_bytes(), b"")

    def test_default_outputs_are_used_without_options(self):
        code, _, err = self.run_recover("--no-ghidra", plain=True)
        self.assertEqual(code, 0, err)
        for name in ("default_functions.json", "default_report.json", "default_seeds.txt"):
            self.assertTrue((self.tmp / name).is_file(), name)

    def test_extra_seeds_file_adds_manual_source(self):
        extra = self.tmp / "extra.json"
        extra.write_text(json.dumps({"schema": 1, "seeds": [
            {"addr": hexs(at(3)), "reason": "test"}]}), encoding="utf-8")
        code, out, err = self.run_recover("--no-ghidra", "--extra-seeds", extra)
        self.assertEqual(code, 0, err)
        db = self.db()
        self.assertEqual(db["input"]["extra_seeds"], 1)
        entries = {f["entry"]: f for f in db["functions"]}
        self.assertEqual(entries[hexs(at(3))]["sources"], ["manual"])
        self.assertIn("manual 1;", out)

    def test_default_extra_seeds_file_is_used_when_present(self):
        recover.DEFAULT_EXTRA_SEEDS.write_text(json.dumps({"schema": 1, "seeds": [
            {"addr": hexs(at(3)), "reason": "test"}]}), encoding="utf-8")
        self.assertEqual(self.run_recover("--no-ghidra")[0], 0)
        self.assertEqual(self.db()["input"]["extra_seeds"], 1)


class ErrorTest(RecoverTestBase):
    def setUp(self):
        super().setUp()
        self.write_elf(build_ps3_fixture())

    def assert_no_db(self):
        self.assertFalse(self.out_path.exists())

    def test_explicit_missing_summary_is_io_error(self):
        code, out, err = self.run_recover("--ghidra-summary", self.tmp / "nope.json")
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "), err)
        self.assertNotIn("Traceback", err)
        self.assert_no_db()

    def test_default_missing_summary_hints_at_the_alternatives(self):
        code, out, err = self.run_recover()
        self.assertEqual(code, 1)
        self.assertIn("ghidra-summary", err)
        self.assertIn("--no-ghidra", err)
        self.assertEqual(err.count("\n"), 1)
        self.assert_no_db()

    def test_invalid_summary_is_io_error(self):
        bad = self.tmp / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        self.assertEqual(self.run_recover("--ghidra-summary", bad)[0], 1)

    def test_explicit_missing_extra_seeds_is_io_error(self):
        code, _, err = self.run_recover("--no-ghidra", "--extra-seeds", self.tmp / "nope.json")
        self.assertEqual(code, 1)
        self.assertIn("extra seeds", err)

    def test_invalid_extra_seeds_is_io_error(self):
        bad = self.tmp / "extra.json"
        bad.write_text(json.dumps({"schema": 1, "seeds": [{"addr": "xyz"}]}), encoding="utf-8")
        self.assertEqual(self.run_recover("--no-ghidra", "--extra-seeds", bad)[0], 1)
        bad.write_text(json.dumps({"schema": 2, "seeds": []}), encoding="utf-8")
        self.assertEqual(self.run_recover("--no-ghidra", "--extra-seeds", bad)[0], 1)

    def test_summary_from_another_elf_is_exit_4(self):
        summary = self.tmp / "other.json"
        write_compact_json(summary, summary_doc("0" * 64, self.text, [(at(0), None)]))
        code, _, err = self.run_recover("--ghidra-summary", summary)
        self.assertEqual(code, 4)
        self.assertIn("different ELF", err)
        self.assert_no_db()

    def test_summary_with_other_text_range_is_exit_4(self):
        doc = summary_doc(self.elf_sha, (self.text[0], self.text[1] - 4), [(at(0), None)])
        summary = self.tmp / "text.json"
        write_compact_json(summary, doc)
        code, _, err = self.run_recover("--ghidra-summary", summary)
        self.assertEqual(code, 4)
        self.assertIn("text range", err)
        self.assert_no_db()

    def test_summary_and_no_ghidra_are_exclusive(self):
        summary = self.write_summary([(at(0), None)])
        code, _, err = self.run_recover("--ghidra-summary", summary, "--no-ghidra")
        self.assertEqual(code, 2)

    def test_max_rounds_must_be_positive(self):
        self.assertEqual(self.run_recover("--no-ghidra", "--max-rounds", "0")[0], 2)

    def test_not_an_elf_is_exit_3(self):
        self.elf_path.write_bytes(b"\x00" * 64)
        code, _, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 3)
        self.assertTrue(err.startswith("error: "))

    def test_missing_input_is_exit_1(self):
        self.elf_path = self.tmp / "missing.elf"
        self.assertEqual(self.run_recover("--no-ghidra")[0], 1)


class PromotionTest(RecoverTestBase):
    def setUp(self):
        super().setUp()
        self.write_elf(build_code_elf(shared_tail_words()))
        self.extra = self.tmp / "extra.json"
        self.extra.write_text(json.dumps({"schema": 1, "seeds": [
            {"addr": hexs(at(3)), "reason": "test"}]}), encoding="utf-8")

    def test_shared_tail_is_promoted_and_recorded(self):
        code, out, err = self.run_recover("--no-ghidra", "--extra-seeds", self.extra)
        self.assertEqual(code, 0, err)
        db = self.db()
        self.assertEqual([(f["entry"], f["sources"]) for f in db["functions"]], [
            (hexs(at(0)), ["entry", "opd"]),
            (hexs(at(3)), ["manual"]),
            (hexs(at(6)), ["promoted"]),
        ])
        part = self.report()["partition"]
        self.assertEqual(part["rounds"], 2)
        self.assertEqual(part["promoted"], [{
            "addr": hexs(at(6)), "round": 1, "reached_from": [hexs(at(0)), hexs(at(3))],
            "edge_sites": [hexs(at(1)), hexs(at(4))]}])
        self.assertEqual(part["tail_edges_by_kind"], {"branch": 2})
        self.assertIn("partition: 2 rounds, converged, 3 functions, 1 promoted, "
                      "0 call-discovered", out)

    def test_round_limit_exits_8_without_writing_the_database(self):
        code, out, err = self.run_recover("--no-ghidra", "--extra-seeds", self.extra,
                                          "--max-rounds", "1")
        self.assertEqual(code, 8)
        self.assertIn("did not converge", err)
        self.assertFalse(self.out_path.exists())
        self.assertFalse(self.seeds_path.exists())
        self.assertFalse(self.report()["partition"]["converged"])
        self.assertIn("NOT CONVERGED", out)

    def test_call_discovery_is_counted(self):
        # A conditional call is not found by the `bl` scan, so only the partition discovers it.
        words = [asm.bc(12, 2, TEXT, at(3), lk=True), asm.blr(), asm.blr(), asm.nop(),
                 asm.blr()]
        self.write_elf(build_code_elf(words))
        code, out, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 0, err)
        self.assertIn("2 functions, 0 promoted, 1 call-discovered", out)
        self.assertEqual([f["sources"] for f in self.db()["functions"]],
                         [["entry", "opd"], ["call"]])


class UnresolvedIndirectTest(RecoverTestBase):
    def test_unresolved_bcctr_is_listed_with_its_function(self):
        self.write_elf(build_code_elf([asm.nop(), asm.bctr()]))
        code, out, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.db()["unresolved_indirect"], [
            {"addr": hexs(at(1)), "function": hexs(at(0)), "reason": "no_mtctr"}])
        jt = self.report()["jump_tables"]
        self.assertEqual((jt["bcctr_sites"], jt["resolved"], jt["targets"]), (1, 0, 0))
        self.assertEqual(jt["unresolved_by_reason"], {"no_mtctr": 1})
        self.assertEqual(jt["unresolved"][0]["function"], hexs(at(0)))
        self.assertIn("jump tables: 1 bcctr sites, 0 resolved (0 targets), 1 unresolved", out)


class SuspectBlSeedTest(RecoverTestBase):
    def test_bl_target_called_only_from_unowned_code_is_suspect(self):
        # The `bl` at index 3 is never reached from a function start; its target is a "bl"-only
        # function.
        words = [asm.blr(), asm.blr(), asm.blr(), asm.b(at(3), at(5), lk=True),
                 asm.blr(), asm.blr()]
        self.write_elf(build_code_elf(words))
        code, _, err = self.run_recover("--no-ghidra")
        self.assertEqual(code, 0, err)
        part = self.report()["partition"]
        self.assertEqual(part["suspect_bl_seeds"], {"count": 1, "examples": [hexs(at(5))]})


if __name__ == "__main__":
    unittest.main()

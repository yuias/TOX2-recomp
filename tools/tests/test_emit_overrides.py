"""Tests for guest function overrides of the `emit` command."""

import contextlib
import hashlib
import io
import json
import unittest
from unittest import mock

from ps3recomp import cell, emit
from ps3recomp.elf import ElfFile
from ps3recomp.emit_overrides import body_sha256, generated_name

from tests.elf_fixture import PS3_TEXT_VA, build_ps3_fixture
from tests.test_emit_db import TempDirTestCase, db_doc, hexs

TEXT = PS3_TEXT_VA
FIRST = TEXT
SECOND = TEXT + 0xC
THIRD = TEXT + 0x14


class EmitOverridesTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        data = build_ps3_fixture()
        self.elf_path = self.tmp / "fixture.elf"
        self.elf_path.write_bytes(data)
        self.sha = hashlib.sha256(data).hexdigest()
        self.text = cell.find_text_range(ElfFile.from_bytes(data))
        self.report = self.tmp / "report.json"
        self.default_overrides = self.tmp / "default_overrides.json"
        # The real default file must never leak into a unit test.
        patcher = mock.patch.object(emit, "DEFAULT_OVERRIDES", self.default_overrides)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.db = self.write_db(db_doc(sha=self.sha, text=self.text, functions=[
            (FIRST, [(FIRST, FIRST + 0xC)]),
            (SECOND, [(SECOND, SECOND + 8)]),
            (THIRD, [(THIRD, THIRD + 8)])]))

    def run_cli(self, out_name, *extra):
        out, err = io.StringIO(), io.StringIO()
        argv = [str(self.elf_path), "--db", str(self.db), "--out", str(self.tmp / out_name),
                "--report", str(self.report), "--census-out", str(self.tmp / "census.json"),
                "--chunks", "3"]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = emit.main(argv + list(extra))
        return code, out.getvalue(), err.getvalue()

    def snapshot(self, out_name):
        return {p.name: p.read_text(encoding="utf-8")
                for p in sorted((self.tmp / out_name).iterdir())}

    def plain_body(self, entry):
        """The text of `fn_<entry>` in an emit without overrides, with the chunk banner removed."""
        self.assertEqual(self.run_cli("plain")[0], 0)
        for text in self.snapshot("plain").values():
            if "TOX2_PPU_FUNC(fn_%08x) {" % entry in text:
                return text.split("\n\n", 2)[2]
        self.fail("no chunk holds fn_%08x" % entry)

    def write_overrides(self, *items, name="overrides.json"):
        path = self.tmp / name
        path.write_text(json.dumps({"overrides": list(items)}), encoding="utf-8")
        return path

    def item(self, entry=SECOND, sha=None, reason="why not"):
        return {"entry": hexs(entry), "generated_sha256": sha or body_sha256(self.plain_body(entry)),
                "reason": reason}

    def test_override_renames_the_body_and_keeps_header_and_table(self):
        path = self.write_overrides(self.item())
        code, _, err = self.run_cli("over", "--overrides", str(path))
        self.assertEqual((code, err), (0, ""))
        plain, over = self.snapshot("plain"), self.snapshot("over")
        self.assertEqual(sorted(over), sorted(plain))
        chunks = {name: text for name, text in over.items() if name.startswith("ppu_chunk_")}
        holder = [text for text in chunks.values() if "fn_0001000c_generated" in text]
        self.assertEqual(len(holder), 1)
        self.assertIn("TOX2_PPU_FUNC(fn_0001000c_generated) {", holder[0])
        self.assertNotIn("TOX2_PPU_FUNC(fn_0001000c) {", holder[0])
        # The renamed body is the plain one with only its name changed.
        self.assertEqual(self.plain_body(SECOND).replace("fn_0001000c)", "fn_0001000c_generated)"),
                         holder[0].split("\n\n", 2)[2])
        # The header declaration and the table row still bind fn_<addr>.
        self.assertIn("TOX2_PPU_FUNC(fn_0001000c);", over["ppu_funcs.h"])
        self.assertNotIn("_generated", over["ppu_funcs.h"])
        self.assertIn("{ 0x0001000cu, fn_0001000c },", over["ppu_tables.c"])
        self.assertNotIn("_generated", over["ppu_tables.c"])
        # Nothing else changes.
        for name in plain:
            if "fn_0001000c_generated" not in over[name]:
                self.assertEqual(over[name], plain[name], name)

    def test_chunk_plan_still_counts_the_overridden_function(self):
        self.run_cli("plain")
        self.run_cli("over", "--overrides", str(self.write_overrides(self.item())))
        self.assertEqual(self.snapshot("over")["sources.cmake"], self.snapshot("plain")["sources.cmake"])

    def test_no_file_and_empty_file_give_identical_output(self):
        self.assertEqual(self.run_cli("plain")[0], 0)
        self.assertFalse(self.default_overrides.exists())
        self.default_overrides.write_text('{"overrides": []}', encoding="utf-8")
        self.assertEqual(self.run_cli("default_empty")[0], 0)
        self.assertEqual(self.run_cli("explicit_empty", "--overrides",
                                      str(self.write_overrides()))[0], 0)
        self.assertEqual(self.snapshot("default_empty"), self.snapshot("plain"))
        self.assertEqual(self.snapshot("explicit_empty"), self.snapshot("plain"))

    def test_default_file_is_used_when_present(self):
        item = self.item()
        self.default_overrides.write_text(json.dumps({"overrides": [item]}), encoding="utf-8")
        code, out, _ = self.run_cli("over")
        self.assertEqual(code, 0)
        self.assertIn("fn_0001000c_generated", "".join(self.snapshot("over").values()))
        self.assertEqual(out.splitlines()[-1], "overrides: 1 (0x0001000c)")

    def test_report_and_stdout_list_the_overrides_sorted(self):
        path = self.write_overrides(self.item(THIRD, reason="third"),
                                    self.item(SECOND, reason="second"))
        code, out, _ = self.run_cli("over", "--overrides", str(path))
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-1], "overrides: 2 (0x0001000c, 0x00010014)")
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["overrides"], [{"entry": "0x0001000c", "reason": "second"},
                                               {"entry": "0x00010014", "reason": "third"}])

    def test_no_overrides_reports_zero(self):
        code, out, _ = self.run_cli("plain")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-1], "overrides: 0")
        self.assertEqual(json.loads(self.report.read_text(encoding="utf-8"))["overrides"], [])

    def test_comments_keep_the_pin_on_the_plain_body_and_counts_once(self):
        self.run_cli("plain")
        plain_report = json.loads(self.report.read_text(encoding="utf-8"))
        path = self.write_overrides(self.item())
        code, _, err = self.run_cli("over", "--overrides", str(path), "--comments")
        self.assertEqual((code, err), (0, ""))
        report = json.loads(self.report.read_text(encoding="utf-8"))
        for key in ("functions", "instructions", "labels", "loop_hooks", "edges"):
            self.assertEqual(report[key], plain_report[key], key)
        text = "".join(self.snapshot("over").values())
        self.assertIn("TOX2_PPU_FUNC(fn_0001000c_generated) {", text)
        self.assertIn("// 0x0001000c: ", text)

    def test_hash_mismatch_exits_3_and_prints_the_actual_hash(self):
        actual = body_sha256(self.plain_body(SECOND))
        path = self.write_overrides(self.item(sha="0" * 64))
        code, _, err = self.run_cli("over", "--overrides", str(path))
        self.assertEqual(code, 3)
        self.assertIn(actual, err)
        self.assertIn("0x0001000c", err)
        self.assertFalse((self.tmp / "over").exists())

    def test_pin_is_a_hex_digest_of_the_emitted_text(self):
        # Upper case digits are accepted and compared case-insensitively.
        path = self.write_overrides(self.item(sha=body_sha256(self.plain_body(SECOND)).upper()))
        self.assertEqual(self.run_cli("over", "--overrides", str(path))[0], 0)

    def test_validation_failures_exit_3(self):
        good = self.item()
        cases = {
            "bad hex": dict(good, entry="0xzz"),
            "no prefix": dict(good, entry="0001000c"),
            "too wide": dict(good, entry="0x123456789"),
            "not a function": dict(good, entry=hexs(TEXT + 4)),
            "empty reason": dict(good, reason=""),
            "blank reason": dict(good, reason="  "),
            "short sha": dict(good, generated_sha256="abcd"),
            "missing key": {"entry": good["entry"], "reason": "x"},
            "extra key": dict(good, source="x.c"),
        }
        for label, item in cases.items():
            with self.subTest(label):
                code, _, err = self.run_cli("over", "--overrides", str(self.write_overrides(item)))
                self.assertEqual(code, 3, err)
                self.assertFalse((self.tmp / "over").exists())

    def test_duplicate_entry_exits_3(self):
        item = self.item()
        code, _, err = self.run_cli("over", "--overrides",
                                    str(self.write_overrides(item, dict(item, reason="again"))))
        self.assertEqual(code, 3)
        self.assertIn("twice", err)

    def test_malformed_file_exits_3(self):
        for label, content in (("not json", "{"), ("not an object", "[]"),
                               ("wrong key", '{"override": []}'),
                               ("not a list", '{"overrides": {}}')):
            with self.subTest(label):
                path = self.tmp / "bad.json"
                path.write_text(content, encoding="utf-8")
                self.assertEqual(self.run_cli("over", "--overrides", str(path))[0], 3)

    def test_explicit_missing_file_is_an_io_error(self):
        code, _, err = self.run_cli("over", "--overrides", str(self.tmp / "missing.json"))
        self.assertEqual(code, 1)
        self.assertIn("missing.json", err)

    def test_overrides_with_symbol_prefix_exits_2(self):
        path = self.write_overrides()
        code, _, err = self.run_cli("over", "--overrides", str(path), "--symbol-prefix", "libsre")
        self.assertEqual(code, 2)
        self.assertIn("--overrides", err)
        self.assertFalse((self.tmp / "over").exists())

    def test_generated_name(self):
        self.assertEqual(generated_name("fn_00956a00"), "fn_00956a00_generated")


if __name__ == "__main__":
    unittest.main()

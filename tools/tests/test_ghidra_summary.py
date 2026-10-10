import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from ps3recomp import ghidra_summary
from ps3recomp.info import CliError

from tests.test_ghidra import TEXT_VA, build_vmx_elf, make_export

MFLR = 0x7C0802A6
BCTR = 0x4E800420
BCTRL = 0x4E800421
NOP = 0x60000000

# Words 0-3 code, 4-5 data, 6 code, 7 undefined, 8-9 code.
WORDS = (MFLR, BCTR, BCTR, BCTRL, NOP, NOP, NOP, NOP, NOP, NOP)


def _addr(index: int) -> int:
    return TEXT_VA + 4 * index


def _export(data: bytes) -> dict:
    return make_export(
        data, words=WORDS,
        mnemonics={_addr(1): "bctr+", _addr(2): "bcctr-", _addr(3): "bctrl"},
        kinds={_addr(4): "data", _addr(5): "data", _addr(7): "undefined"},
        functions=[
            {"entry": "0x00010010", "name": "FUN_00010010", "thunk": False,
             "body": [["0x00010020", "0x00010028"], ["0x00010010", "0x00010018"]]},
            {"entry": "0x00010000", "name": ".opd.FUN_00010000", "thunk": False,
             "body": [["0x00010000", "0x0001000c"]]},
            {"entry": "0x0001000c", "name": "main", "thunk": True,
             "body": [["0x0001000c", "0x00010010"]]},
        ],
        jump_tables=[{"branch": "0x00010004", "targets": ["0x00010018", "0x00010010"]}],
        data_refs=[
            {"from": "0x00010008", "to": "0x00010000", "type": "READ", "from_exec": True},
            {"from": "0x00010004", "to": "0x00010004", "type": "DATA", "from_exec": False},
        ])


class SummaryTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.elf_data = build_vmx_elf(WORDS)
        self.elf_path = self.tmp / "input.elf"
        self.elf_path.write_bytes(self.elf_data)
        self.json_path = self.tmp / "export.json"
        self.out_path = self.tmp / "out" / "summary.json"

    def run_summary(self, export, *extra):
        self.json_path.write_text(json.dumps(export), encoding="utf-8")
        return self.run_raw(str(self.elf_path), "--ghidra", str(self.json_path),
                            "-o", str(self.out_path), *extra)

    def run_raw(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ghidra_summary.main(list(args))
        return code, out.getvalue(), err.getvalue()

    def summary_doc(self):
        return json.loads(self.out_path.read_text(encoding="utf-8"))


class BuildTest(SummaryTestBase):
    def setUp(self):
        super().setUp()
        code, out, err = self.run_summary(_export(self.elf_data))
        self.assertEqual((code, err), (0, ""), out)
        self.stdout = out
        self.doc = self.summary_doc()

    def test_header(self):
        self.assertEqual(self.doc["schema"], 1)
        self.assertEqual(self.doc["generator"], "ps3tool ghidra-summary")
        self.assertEqual(self.doc["text"], {"start": "0x00010000", "end": "0x00010028"})
        self.assertEqual(self.doc["source"]["ghidra_version"], "test")
        self.assertEqual(len(self.doc["source"]["executable_sha256"]), 64)

    def test_default_names_become_null_and_functions_are_sorted(self):
        funcs = self.doc["functions"]
        self.assertEqual([f["entry"] for f in funcs],
                         ["0x00010000", "0x0001000c", "0x00010010"])
        self.assertEqual([f["name"] for f in funcs], [None, "main", None])
        self.assertEqual([f["thunk"] for f in funcs], [False, True, False])
        self.assertEqual(funcs[2]["body"], [["0x00010010", "0x00010018"],
                                            ["0x00010020", "0x00010028"]])

    def test_ranges_and_word_kinds(self):
        self.assertEqual(self.doc["word_kinds"], {"code": 7, "data": 2, "undefined": 1})
        self.assertEqual(self.doc["code_ranges"],
                         [["0x00010000", "0x00010010"], ["0x00010018", "0x0001001c"],
                          ["0x00010020", "0x00010028"]])
        self.assertEqual(self.doc["data_ranges"], [["0x00010010", "0x00010018"]])

    def test_bctr_sites_strip_hints_and_carry_targets(self):
        self.assertEqual(self.doc["bctr_sites"], [
            {"addr": "0x00010004", "mnemonic": "bctr",
             "targets": ["0x00010010", "0x00010018"]},
            {"addr": "0x00010008", "mnemonic": "bcctr", "targets": []},
        ])

    def test_data_refs_sorted(self):
        self.assertEqual([r["from"] for r in self.doc["data_refs"]],
                         ["0x00010004", "0x00010008"])
        self.assertEqual(self.doc["data_refs"][1],
                         {"from": "0x00010008", "to": "0x00010000", "type": "READ",
                          "from_exec": True})

    def test_stdout(self):
        lines = self.stdout.splitlines()
        self.assertEqual(lines[0], "functions: 3 (3 in .text, 1 named)")
        self.assertIn("code ranges: 3", lines)
        self.assertIn("data ranges: 1", lines)
        self.assertIn("data refs: 2", lines)
        self.assertIn("bctr sites: 2", lines)
        self.assertEqual(lines[-1], "size: %d bytes" % self.out_path.stat().st_size)

    def test_load_summary_round_trip(self):
        summary = ghidra_summary.load_summary(self.out_path)
        self.assertEqual(summary.text, (0x10000, 0x10028))
        self.assertEqual(summary.sha256, self.doc["source"]["executable_sha256"])
        self.assertEqual([f.entry for f in summary.functions], [0x10000, 0x1000C, 0x10010])
        self.assertEqual(summary.functions[1].name, "main")
        self.assertTrue(summary.functions[1].thunk)
        self.assertEqual(summary.functions[2].body, ((0x10010, 0x10018), (0x10020, 0x10028)))
        self.assertEqual(summary.data_ranges, ((0x10010, 0x10018),))
        self.assertEqual(summary.data_refs[1], (0x10008, 0x10000, "READ", True))
        self.assertEqual(summary.bctr_sites[0], (0x10004, (0x10010, 0x10018)))


class CompactFormatTest(SummaryTestBase):
    def test_one_element_per_line_and_parses_back(self):
        self.run_summary(_export(self.elf_data))
        raw = self.out_path.read_bytes()
        self.assertNotIn(b"\r", raw)
        self.assertTrue(raw.endswith(b"}\n"))
        lines = raw.decode("utf-8").split("\n")
        self.assertEqual(lines[0], "{")
        self.assertIn('"functions": [', lines)
        index = lines.index('"functions": [')
        for offset in range(1, 4):
            line = lines[index + offset]
            self.assertTrue(line.startswith("{") and line.rstrip(",").endswith("}"), line)
        self.assertEqual(lines[index + 4], "],")
        self.assertEqual(lines[index + 5], '"code_ranges": [')
        self.assertEqual(list(json.loads(raw)), [
            "schema", "generator", "source", "text", "word_kinds", "functions",
            "code_ranges", "data_ranges", "data_refs", "bctr_sites"])

    def test_write_compact_json_round_trips(self):
        doc = {"a": 1, "b": [], "c": [[1, 2], {"x": [3]}], "d": {"k": None}}
        path = self.tmp / "compact.json"
        ghidra_summary.write_compact_json(path, doc)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8")), doc)
        self.assertEqual(path.read_text(encoding="utf-8"),
                         '{\n"a": 1,\n"b": [],\n"c": [\n[1, 2],\n{"x": [3]}\n],\n'
                         '"d": {"k": null}\n}\n')

    def test_deterministic(self):
        self.run_summary(_export(self.elf_data))
        first = self.out_path.read_bytes()
        self.run_summary(_export(self.elf_data))
        self.assertEqual(self.out_path.read_bytes(), first)


class ErrorTest(SummaryTestBase):
    def test_sha_mismatch_is_exit_4(self):
        export = _export(self.elf_data)
        export["executable_sha256"] = "0" * 64
        code, _, err = self.run_summary(export)
        self.assertEqual(code, 4)
        self.assertTrue(err.startswith("error: "), err)
        self.assertFalse(self.out_path.exists())

    def test_missing_export_is_exit_1(self):
        code, _, err = self.run_raw(str(self.elf_path), "--ghidra",
                                    str(self.tmp / "missing.json"), "-o", str(self.out_path))
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "), err)

    def test_invalid_json_is_exit_1(self):
        self.json_path.write_text("{not json", encoding="utf-8")
        code, _, _ = self.run_raw(str(self.elf_path), "--ghidra", str(self.json_path),
                                  "-o", str(self.out_path))
        self.assertEqual(code, 1)

    def test_bad_kind_is_exit_1(self):
        export = _export(self.elf_data)
        export["words"][0]["kind"] = "bogus"
        code, _, _ = self.run_summary(export)
        self.assertEqual(code, 1)

    def test_missing_ghidra_option_is_usage_error(self):
        code, _, _ = self.run_raw(str(self.elf_path))
        self.assertEqual(code, 2)

    def test_load_summary_missing_and_invalid(self):
        with self.assertRaises(CliError) as cm:
            ghidra_summary.load_summary(self.tmp / "missing.json")
        self.assertEqual(cm.exception.code, 1)
        bad = self.tmp / "bad.json"
        for text in ("{nope", "{}", '{"schema": 2}'):
            bad.write_text(text, encoding="utf-8")
            with self.assertRaises(CliError) as cm:
                ghidra_summary.load_summary(bad)
            self.assertEqual(cm.exception.code, 1)


if __name__ == "__main__":
    unittest.main()

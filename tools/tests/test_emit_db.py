"""Tests for the emitter's function DB loader, mnemonic groups, census and `emit --census`."""

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from ps3recomp import cell, emit, emit_db
from ps3recomp.elf import ElfFile
from ps3recomp.emit_db import (DbFunction, FunctionDb, check_db_matches, load_function_db,
                               take_census)
from ps3recomp.info import CliError
from ps3recomp.ppu import ALL_OPS
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm
from tests.elf_fixture import PS3_TEXT_VA, build_ps3_fixture

TEXT = PS3_TEXT_VA
SHA = "ab" * 32


def hexs(value: int) -> str:
    return "0x%08x" % value


def db_doc(sha: str = SHA, text=(TEXT, TEXT + 0x20), functions=None, code=None) -> dict:
    code = text if code is None else code
    if functions is None:
        functions = [(TEXT, [(TEXT, TEXT + 0x10)]), (TEXT + 0x10, [(TEXT + 0x10, TEXT + 0x1C)])]
    return {
        "schema": 1,
        "generator": "ps3tool recover",
        "input": {"executable_sha256": sha},
        "text": {"start": hexs(text[0]), "end": hexs(text[1])},
        "code": {"start": hexs(code[0]), "end": hexs(code[1])},
        "toc": hexs(0x28000),
        "functions": [{"entry": hexs(entry), "name": None, "sources": ["ghidra"],
                       "ranges": [[hexs(a), hexs(b)] for a, b in ranges]}
                      for entry, ranges in functions],
        "import_thunks": [{"addr": hexs(0x30000), "module": "cellFoo", "fnid": hexs(0x1234),
                           "name": None, "slot": hexs(0x40000)}],
        "jump_tables": [{"branch": hexs(TEXT + 8), "table": hexs(0x50000), "kind": "abs",
                         "count": 2, "targets": [hexs(TEXT), hexs(TEXT + 4)]}],
        "unresolved_indirect": [],
    }


class TempDirTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def write_db(self, doc, name="functions.json") -> Path:
        path = self.tmp / name
        path.write_text(json.dumps(doc), encoding="utf-8", newline="\n")
        return path


class GroupsTest(unittest.TestCase):
    def test_every_spec_name_is_in_exactly_one_group(self):
        names = [spec.name for spec in ALL_OPS]
        for name in names:
            owners = [g for g, members in emit_db.GROUPS.items() if name in members]
            self.assertEqual(len(owners), 1, name)
            self.assertEqual(emit_db.GROUP_OF[name], owners[0])

    def test_groups_contain_no_unknown_or_repeated_names(self):
        known = {spec.name for spec in ALL_OPS}
        listed = [n for members in emit_db.GROUPS.values() for n in members]
        self.assertEqual(sorted(set(listed) - known), [])
        self.assertEqual(len(listed), len(set(listed)))
        self.assertEqual(len(emit_db.GROUP_OF), len(listed))

    def test_group_order(self):
        self.assertEqual(list(emit_db.GROUPS), [
            "control", "int_alu", "int_carry_muldiv", "int_rotate", "mem", "sys", "fp",
            "vmx_mem", "vmx_perm", "vmx_int", "vmx_mul", "vmx_fp"])


class LoadFunctionDbTest(TempDirTestCase):
    def test_valid_db(self):
        path = self.write_db(db_doc(functions=[
            (TEXT + 0x10, [(TEXT + 0x10, TEXT + 0x18), (TEXT + 0x1C, TEXT + 0x20)]),
            (TEXT, [(TEXT, TEXT + 0x10)])]))
        db = load_function_db(path)
        self.assertEqual(db.sha256, SHA)
        self.assertEqual(db.file_sha256, hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(db.text, (TEXT, TEXT + 0x20))
        self.assertEqual(db.toc, 0x28000)
        self.assertEqual([f.entry for f in db.functions], [TEXT, TEXT + 0x10])
        self.assertEqual(db.functions[1].addrs(), [TEXT + 0x10, TEXT + 0x14, TEXT + 0x1C])
        self.assertEqual(db.thunks[0].addr, 0x30000)
        self.assertEqual((db.thunks[0].module, db.thunks[0].fnid, db.thunks[0].slot),
                         ("cellFoo", 0x1234, 0x40000))
        table = db.jump_tables[TEXT + 8]
        self.assertEqual((table.table, table.kind, table.count, table.targets),
                         (0x50000, "abs", 2, (TEXT, TEXT + 4)))

    def test_null_toc(self):
        doc = db_doc()
        doc["toc"] = None
        self.assertIsNone(load_function_db(self.write_db(doc)).toc)

    def test_missing_file(self):
        with self.assertRaises(CliError) as ctx:
            load_function_db(self.tmp / "absent.json")
        self.assertEqual(ctx.exception.code, 1)

    def test_bad_json(self):
        path = self.tmp / "bad.json"
        path.write_text("{not json", encoding="utf-8")
        with self.assertRaises(CliError) as ctx:
            load_function_db(path)
        self.assertEqual(ctx.exception.code, 1)

    def test_schema_2(self):
        doc = db_doc()
        doc["schema"] = 2
        with self.assertRaises(CliError) as ctx:
            load_function_db(self.write_db(doc))
        self.assertEqual(ctx.exception.code, 1)

    def test_missing_key(self):
        doc = db_doc()
        del doc["functions"]
        with self.assertRaises(CliError) as ctx:
            load_function_db(self.write_db(doc))
        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("functions", str(ctx.exception))

    def test_overlapping_functions(self):
        doc = db_doc(functions=[(TEXT, [(TEXT, TEXT + 0x10)]),
                                (TEXT + 0x8, [(TEXT + 0x8, TEXT + 0x14)])])
        with self.assertRaises(CliError) as ctx:
            load_function_db(self.write_db(doc))
        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("overlapping", str(ctx.exception))


class CheckDbMatchesTest(unittest.TestCase):
    def make_db(self) -> FunctionDb:
        span = (TEXT, TEXT + 0x20)
        return FunctionDb(SHA, "00" * 32, span, span, None, (), (), {})

    def test_match(self):
        check_db_matches(self.make_db(), SHA.upper(), (TEXT, TEXT + 0x20),
                         (TEXT, TEXT + 0x20))

    def test_sha_mismatch(self):
        with self.assertRaises(CliError) as ctx:
            check_db_matches(self.make_db(), "cd" * 32, (TEXT, TEXT + 0x20),
                             (TEXT, TEXT + 0x20))
        self.assertEqual(ctx.exception.code, 4)

    def test_text_mismatch(self):
        with self.assertRaises(CliError) as ctx:
            check_db_matches(self.make_db(), SHA, (TEXT, TEXT + 0x24), (TEXT, TEXT + 0x20))
        self.assertEqual(ctx.exception.code, 4)

    def test_code_mismatch(self):
        with self.assertRaises(CliError) as ctx:
            check_db_matches(self.make_db(), SHA, (TEXT, TEXT + 0x20), (TEXT - 4, TEXT + 0x20))
        self.assertEqual(ctx.exception.code, 4)


class CensusTest(unittest.TestCase):
    def test_counts_and_invalid_owned_word(self):
        words = [asm.addi(3, 3, 1), asm.addi(4, 4, 2), asm.ori(0, 0, 0), asm.blr(),
                 0x00000000,                    # owned but not a strictly valid instruction
                 asm.nop(),                     # not owned
                 asm.blr()]
        image = CodeImage.from_words(TEXT, words)
        db = FunctionDb(SHA, "00" * 32, (TEXT, TEXT + 4 * len(words)),
                        (TEXT, TEXT + 4 * len(words)), None, (
            DbFunction(TEXT, None, ((TEXT, TEXT + 0x14),)),
            DbFunction(TEXT + 0x18, None, ((TEXT + 0x18, TEXT + 0x1C),))), (), {})
        census = take_census(image, db)
        self.assertEqual(census.words, 6)
        self.assertEqual(census.by_name, {"addi": 2, "ori": 1, "bclr": 2})
        self.assertEqual(census.invalid_owned, (TEXT + 0x10,))

    def test_census_doc_and_lines(self):
        census = emit_db.Census(10, {"ori": 5, "addi": 3, "b": 2}, ())
        doc = emit_db.census_doc(census)
        self.assertEqual((doc["words"], doc["distinct"], doc["invalid_owned"]), (10, 3, []))
        self.assertEqual([(m["name"], m["group"], m["count"], m["pct"], m["cum_pct"])
                          for m in doc["mnemonics"]],
                         [("ori", "int_alu", 5, 50.0, 50.0), ("addi", "int_alu", 3, 30.0, 80.0),
                          ("b", "control", 2, 20.0, 100.0)])
        self.assertEqual(list(doc["by_group"]), list(emit_db.GROUPS))
        self.assertEqual(doc["by_group"]["int_alu"],
                         {"words": 8, "mnemonics": [["ori", 5], ["addi", 3]]})
        self.assertEqual(doc["by_group"]["fp"], {"words": 0, "mnemonics": []})
        lines = emit_db.census_lines(census)
        self.assertEqual(lines[0], "census: 10 words, 3 distinct mnemonics, 0 invalid owned")
        self.assertEqual(lines[1], "top: ori 5 (50.0%), addi 3 (30.0%), b 2 (20.0%)")
        self.assertEqual(lines[2], "group control: 2 words, 1 mnemonics: b 2")
        self.assertEqual(len(lines), 2 + len(emit_db.GROUPS))


class EmitCensusCliTest(TempDirTestCase):
    def run_cli(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = emit.main(argv)
        return code, out.getvalue(), err.getvalue()

    def fixture(self):
        data = build_ps3_fixture()
        elf_path = self.tmp / "fixture.elf"
        elf_path.write_bytes(data)
        text = cell.find_text_range(ElfFile.from_bytes(data))
        return elf_path, hashlib.sha256(data).hexdigest(), text

    def test_census_on_the_fixture(self):
        elf_path, sha, text = self.fixture()
        self.assertEqual(text, (TEXT, TEXT + 0x20))
        # The last fixture word (0) is not an instruction; leave it unowned.
        db_path = self.write_db(db_doc(sha=sha, text=text, functions=[
            (TEXT, [(TEXT, TEXT + 0x1C)])]))
        census_path = self.tmp / "out" / "census.json"
        code, out, err = self.run_cli([str(elf_path), "--census", "--db", str(db_path),
                                       "--census-out", str(census_path)])
        self.assertEqual((code, err), (0, ""))
        lines = out.splitlines()
        self.assertEqual(lines[0], "census: 7 words, 6 distinct mnemonics, 0 invalid owned")
        self.assertEqual(lines[1],
                         "top: addi 2 (28.6%), bclr 1 (14.3%), mfspr 1 (14.3%), "
                         "mtspr 1 (14.3%), sc 1 (14.3%), stdu 1 (14.3%)")
        self.assertIn("group control: 2 words, 2 mnemonics: bclr 1, sc 1", lines)
        doc = json.loads(census_path.read_text(encoding="utf-8"))
        self.assertEqual((doc["schema"], doc["words"], doc["distinct"]), (1, 7, 6))
        self.assertEqual(census_path.read_bytes()[-2:], b"}\n")
        self.assertNotIn(b"\r", census_path.read_bytes())

    def test_invalid_owned_word_is_reported(self):
        elf_path, sha, text = self.fixture()
        db_path = self.write_db(db_doc(sha=sha, text=text, functions=[
            (TEXT, [(TEXT, TEXT + 0x20)])]))
        code, out, _ = self.run_cli([str(elf_path), "--census", "--db", str(db_path),
                                     "--census-out", str(self.tmp / "c.json")])
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[0],
                         "census: 8 words, 6 distinct mnemonics, 1 invalid owned")
        doc = json.loads((self.tmp / "c.json").read_text(encoding="utf-8"))
        self.assertEqual(doc["invalid_owned"], [hexs(TEXT + 0x1C)])

    def test_missing_elf_argument_is_a_usage_error(self):
        code, _, _ = self.run_cli(["--census"])
        self.assertEqual(code, 2)

    def test_bad_option_is_a_usage_error(self):
        code, _, _ = self.run_cli(["x.elf", "--chunks", "0"])
        self.assertEqual(code, 2)

    def test_sha_mismatch_exits_4(self):
        elf_path, _, text = self.fixture()
        db_path = self.write_db(db_doc(sha="cd" * 32, text=text))
        code, _, err = self.run_cli([str(elf_path), "--census", "--db", str(db_path),
                                     "--census-out", str(self.tmp / "c.json")])
        self.assertEqual(code, 4)
        self.assertTrue(err.startswith("error: "))

    def test_text_mismatch_exits_4(self):
        elf_path, sha, text = self.fixture()
        db_path = self.write_db(db_doc(sha=sha, text=(text[0], text[1] - 4)))
        code, _, _ = self.run_cli([str(elf_path), "--census", "--db", str(db_path),
                                   "--census-out", str(self.tmp / "c.json")])
        self.assertEqual(code, 4)

    def test_code_mismatch_exits_4(self):
        elf_path, sha, text = self.fixture()
        db_path = self.write_db(db_doc(sha=sha, text=text, code=(text[0] - 4, text[1])))
        code, _, _ = self.run_cli([str(elf_path), "--census", "--db", str(db_path),
                                   "--census-out", str(self.tmp / "c.json")])
        self.assertEqual(code, 4)

    def test_missing_db_exits_1(self):
        elf_path, _, _ = self.fixture()
        code, _, err = self.run_cli([str(elf_path), "--census", "--db", str(self.tmp / "no.json"),
                                     "--census-out", str(self.tmp / "c.json")])
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "))

    def test_not_an_elf_exits_3(self):
        bogus = self.tmp / "bogus.elf"
        bogus.write_bytes(b"not an elf at all" * 4)
        code, _, _ = self.run_cli([str(bogus), "--census", "--db", str(self.tmp / "no.json")])
        self.assertEqual(code, 3)


if __name__ == "__main__":
    unittest.main()

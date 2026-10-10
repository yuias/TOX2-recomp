import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from ps3recomp import nidgen, nids
from ps3recomp.info import CliError

_SHA = "0123456789abcdef0123456789abcdef01234567"

_HEADER = """\
#define REG_FNID(_module, nid, func) register_static_function(#_module, nid, func)
#define REG_FUNC(_module, func) REG_FNID(_module, #func, func)
#define REG_VNID(_module, nid, var) register_static_variable(#_module, nid, var)
#define REG_VAR(_module, var) REG_VNID(_module, #var, var)
#define REG_HIDDEN_FUNC(func) register_function(func)
#define REG_HIDDEN_FUNC_PURE(func) register_function(func)
"""

_MODULE_A = """\
// REG_FUNC(modA, ignoredLine) in a comment must not count
DECLARE(ppu_module_manager::modA)("libA", []()
{
	REG_FUNC(modA, funcOne);
	REG_FUNC(modA, funcTwo).flag(MFF_PERFECT);
	/* REG_FUNC(modA, blockCommented); */
	// REG_FUNC(modA, lineCommented);
	REG_FNID(modA, "fnidName", funcThree);
	REG_FNID(modA, 0x12345678, funcFour);
	REG_FNID(modA, "zzzCollide", funcFive);
	REG_FNID(modA, "aaaCollide", funcSix);
	REG_VAR(modA, varOne);
	REG_VNID(modA, 0x0badf00d, varTwo);
	REG_VAR(modA, hiddenVar).flag(MFF_HIDDEN);
	REG_HIDDEN_FUNC(helper);
	const char* url = "http://example.invalid/REG_FUNC(modA, inString)";
});
DECLARE(ppu_module_manager::modB)("libB", []()
{
	REG_FUNC(modB, funcOne);
	REG_FUNC(undeclared, orphan);
});
"""

_SYSCALLS = """\
static int unrelated = 0;
const std::array<std::pair<int, std::string_view>, 1024> g_ppu_syscall_table
{
	null_func,
	BIND_SYSC(sys_first),                                   //1   (0x001)
	NULL_FUNC(sys_second),                                  //2   (0x002)  ROOT
	uns_func, uns_func, uns_func,                           //3-5  UNS
	BIND_SYSC(sys_sixth),                                   //6 (0x006)
	null_func, uns_func,                                    //8  UNS
	null_func,//BIND_SYSC(sys_hidden),                      //9  (0x009)
	BIND_SYSC(sys_tenth) , BIND_SYSC(sys_eleventh),         //11
};
"""


def _build_tree(root: Path, module_text=_MODULE_A, syscalls=_SYSCALLS, header=_HEADER):
    cell = root / "rpcs3" / "Emu" / "Cell"
    (cell / "Modules").mkdir(parents=True)
    (cell / "lv2").mkdir()
    (cell / "PPUModule.h").write_text(header, encoding="utf-8")
    (cell / "Modules" / "a.cpp").write_text(module_text, encoding="utf-8")
    (cell / "lv2" / "lv2.cpp").write_text(syscalls, encoding="utf-8")


class NidHashTest(unittest.TestCase):
    def test_known_answer(self):
        self.assertEqual(nids.compute_nid("cellSpursInitializeWithAttribute2"), 0x30AA96C4)
        self.assertEqual(nids.compute_nid("cellSailPlayerBoot"), 0xBDF21B0F)
        self.assertEqual(nids.compute_nid("cellAtracDecode"), 0x8EB0E65F)


class NidTableTest(unittest.TestCase):
    def test_lookup_module_first_then_unique_global(self):
        table = nids.NidTable({
            "a": {1: "a_one", 2: "shared", 3: "ambiguous_a"},
            "b": {1: "b_one", 2: "shared", 3: "ambiguous_b", 4: "only_b"},
            "c": {},
        })
        self.assertEqual(table.lookup("a", 1), "a_one")
        self.assertEqual(table.lookup("b", 1), "b_one")
        self.assertEqual(table.lookup("c", 2), "shared")
        self.assertEqual(table.lookup("missing", 4), "only_b")
        self.assertIsNone(table.lookup("c", 3))
        self.assertIsNone(table.lookup("a", 99))

    def test_load_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.json"
            bad.write_text("[1]", encoding="utf-8")
            for loader in (nids.load_nid_table, nids.load_syscall_table):
                for path in (Path(tmp) / "missing.json", bad):
                    with self.assertRaises(CliError) as ctx:
                        loader(path)
                    self.assertEqual(ctx.exception.code, 1)


class NidgenTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.root = self.tmp / "rpcs3"
        self.nids_out = self.tmp / "out" / "nids.json"
        self.sysc_out = self.tmp / "out" / "sysc.json"

    def run_tool(self, *extra, root=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = nidgen.main(["--rpcs3", str(root or self.root),
                                "--nids-out", str(self.nids_out),
                                "--syscalls-out", str(self.sysc_out), *extra])
        return code, out.getvalue(), err.getvalue()

    def test_generates_tables(self):
        _build_tree(self.root)
        code, out, err = self.run_tool("--source-commit", _SHA)
        self.assertEqual((code, err), (0, ""))
        raw = self.nids_out.read_bytes()
        self.assertNotIn(b"\r", raw)
        self.assertTrue(raw.endswith(b"\n"))
        doc = json.loads(raw)
        self.assertEqual(doc["schema"], 1)
        self.assertEqual(doc["source"]["commit"], _SHA)
        self.assertEqual(doc["nid_suffix"], "6759659904250490566427499489741a")

        lib_a = doc["modules"]["libA"]
        self.assertEqual(lib_a["0x%08x" % nids.compute_nid("funcOne")], "funcOne")
        self.assertEqual(lib_a["0x%08x" % nids.compute_nid("funcTwo")], "funcTwo")
        self.assertEqual(lib_a["0x%08x" % nids.compute_nid("fnidName")], "fnidName")
        self.assertEqual(lib_a["0x12345678"], "funcFour")
        self.assertEqual(len(lib_a), 6)
        self.assertEqual(list(lib_a), sorted(lib_a))
        for hidden in ("lineCommented", "blockCommented", "ignoredLine", "inString"):
            self.assertNotIn("0x%08x" % nids.compute_nid(hidden), lib_a)
        # A module variable without a declaration falls back to its own name.
        self.assertEqual(list(doc["modules"]), ["libA", "libB", "undeclared"])
        self.assertEqual(doc["modules"]["undeclared"],
                         {"0x%08x" % nids.compute_nid("orphan"): "orphan"})
        self.assertEqual(doc["variables"]["libA"],
                         {"0x%08x" % nids.compute_nid("varOne"): "varOne",
                          "0x0badf00d": "varTwo"})

        self.assertEqual(doc["collisions"], [])
        self.assertIn("unhandled constructs: 0", out)
        self.assertIn(_SHA, out)

    def test_collision_keeps_lexicographically_first_name(self):
        text = _MODULE_A + """\
DECLARE(ppu_module_manager::modC)("libC", []()
{
	REG_FNID(modC, 0x00000042, zebra);
	REG_FNID(modC, 0x00000042, apple);
});
"""
        _build_tree(self.root, module_text=text)
        code, out, _err = self.run_tool()
        self.assertEqual(code, 0)
        doc = json.loads(self.nids_out.read_text(encoding="utf-8"))
        self.assertEqual(doc["modules"]["libC"], {"0x00000042": "apple"})
        self.assertEqual(doc["collisions"],
                         [{"module": "libC", "nid": "0x00000042", "names": ["apple", "zebra"]}])
        self.assertIn("collisions:      1", out)

    def test_unhandled_constructs_are_reported(self):
        text = _MODULE_A + "void f() { REG_FUNC(modA, a + b); REG_UNKNOWN(x); }\n"
        _build_tree(self.root, module_text=text)
        code, out, _err = self.run_tool()
        self.assertEqual(code, 0)
        self.assertIn("unhandled constructs: 2", out)
        self.assertIn("a.cpp:", out)
        self.assertIn("REG_UNKNOWN", out)

    def test_syscalls(self):
        _build_tree(self.root)
        self.assertEqual(self.run_tool()[0], 0)
        doc = json.loads(self.sysc_out.read_text(encoding="utf-8"))
        self.assertEqual(doc["syscalls"], {
            "1": "sys_first", "2": "sys_second", "6": "sys_sixth",
            "10": "sys_tenth", "11": "sys_eleventh"})
        self.assertEqual(nids.load_syscall_table(self.sysc_out)[2], "sys_second")

    def test_wrong_syscall_comment_exits_3(self):
        bad = _SYSCALLS.replace("//6 (0x006)", "//7 (0x007)")
        _build_tree(self.root, syscalls=bad)
        code, _out, err = self.run_tool()
        self.assertEqual(code, 3)
        self.assertTrue(err.startswith("error: "))
        self.assertFalse(self.sysc_out.exists())

    def test_unrecognised_syscall_element_exits_3(self):
        bad = _SYSCALLS.replace("uns_func, uns_func, uns_func,", "mystery(3),")
        _build_tree(self.root, syscalls=bad)
        self.assertEqual(self.run_tool()[0], 3)

    def test_header_without_macros_exits_3(self):
        _build_tree(self.root, header="// nothing\n")
        self.assertEqual(self.run_tool()[0], 3)

    def test_missing_dirs_exit_1(self):
        code, _out, err = self.run_tool(root=self.tmp / "nope")
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "))
        (self.tmp / "empty").mkdir()
        self.assertEqual(self.run_tool(root=self.tmp / "empty")[0], 1)

    def test_commit_from_git_head_ref(self):
        _build_tree(self.root)
        git = self.root / ".git"
        (git / "refs" / "heads").mkdir(parents=True)
        (git / "HEAD").write_text("ref: refs/heads/main\n", encoding="ascii")
        (git / "refs" / "heads" / "main").write_text(_SHA + "\n", encoding="ascii")
        self.assertEqual(nidgen.read_source_commit(self.root), _SHA)
        self.assertEqual(self.run_tool()[0], 0)
        doc = json.loads(self.nids_out.read_text(encoding="utf-8"))
        self.assertEqual(doc["source"]["commit"], _SHA)

    def test_commit_from_packed_refs_and_detached_head(self):
        _build_tree(self.root)
        git = self.root / ".git"
        git.mkdir()
        (git / "HEAD").write_text("ref: refs/heads/main\n", encoding="ascii")
        (git / "packed-refs").write_text(
            "# pack-refs\n%s refs/heads/other\n%s refs/heads/main\n" % ("1" * 40, _SHA),
            encoding="ascii")
        self.assertEqual(nidgen.read_source_commit(self.root), _SHA)
        (git / "HEAD").write_text(_SHA + "\n", encoding="ascii")
        self.assertEqual(nidgen.read_source_commit(self.root), _SHA)

    def test_commit_unknown_is_null(self):
        _build_tree(self.root)
        self.assertIsNone(nidgen.read_source_commit(self.root))
        self.assertEqual(self.run_tool()[0], 0)
        doc = json.loads(self.nids_out.read_text(encoding="utf-8"))
        self.assertIsNone(doc["source"]["commit"])


if __name__ == "__main__":
    unittest.main()

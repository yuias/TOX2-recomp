import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from tox2_save import cli

from tests import save_fixture as fx

PATCHFILE = """\
; comment
[GROUP:Money]
:SAVE
[Gald 10M]
200002E8 00989680
[GROUP:Other]
[Byte a]
00000010 000000AB
[Step 2,3]
41000100 0000FFFF
40030002 00000001
:OTHER
[Elsewhere]
00000020 00000001
:SAVE
[Past the end]
200FFFFF 00000001
[Past the end 2]
20FFFFFC 00000001
"""

# Valid, whitespace-separated code lines for the tests below.
GALD = "200002E8 00000064"


def run_cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class PatchTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.original = bytes(fx.build_save())
        self.save = self.tmp / "SAVE"
        self.save.write_bytes(self.original)
        self.out = self.tmp / "SAVE.out"
        self.patch = self.tmp / "x.savepatch"
        self.patch.write_text(PATCHFILE, encoding="utf-8")

    def assertFormatted(self, err):
        self.assertTrue(err.startswith("error: "), err)
        self.assertEqual(len(err.splitlines()), 1, err)
        self.assertNotIn("Traceback", err)

    def test_list(self):
        code, out, err = run_cli("patch", "--patch", self.patch, "--list")
        self.assertEqual(code, 0, err)
        self.assertEqual(
            out.splitlines(),
            ["[GROUP:Money]", "Gald 10M", "[GROUP:Other]", "Byte a", "Step 2,3", "Elsewhere", "Past the end",
             "Past the end 2"])

    def test_code_to_output(self):
        code, out, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Gald 10M", "--code",
                                 "Byte a", "-o", self.out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines(),
                         ["code 'Gald 10M': 1 write(s)", "code 'Byte a': 1 write(s)", "output: %s" % self.out])
        expected = bytearray(self.original)
        expected[0x2E8:0x2EC] = (0x989680).to_bytes(4, "big")
        expected[0x10] = 0xAB
        self.assertEqual(self.out.read_bytes(), bytes(expected))
        self.assertEqual(self.save.read_bytes(), self.original)

    def test_raw_alone(self):
        code, out, err = run_cli("patch", self.save, "--raw", GALD, "-o", self.out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines(), ["raw 1: 1 write(s)", "output: %s" % self.out])
        self.assertEqual(self.out.read_bytes()[0x2E8:0x2EC], bytes([0, 0, 0, 100]))

    def test_raw_type4_two_pairs(self):
        code, out, err = run_cli("patch", self.save, "--raw", "41000100 0000FFFF 40030002 00000001", "-o", self.out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines()[0], "raw 1: 3 write(s)")
        self.assertEqual(self.out.read_bytes()[0x100:0x106], bytes([0xFF, 0xFF, 0, 0, 0, 1]))

    def test_code_then_raw_in_order(self):
        # The raw value comes last, so it wins over the code at the same offset.
        code, out, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Gald 10M", "--raw", GALD,
                                 "-o", self.out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines()[:2], ["code 'Gald 10M': 1 write(s)", "raw 1: 1 write(s)"])
        self.assertEqual(self.out.read_bytes()[0x2E8:0x2EC], bytes([0, 0, 0, 100]))

    def test_in_place(self):
        code, out, err = run_cli("patch", self.save, "--raw", GALD, "--in-place")
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines()[-1], "output: %s" % self.save)
        self.assertEqual(self.save.read_bytes()[0x2E8:0x2EC], bytes([0, 0, 0, 100]))

    def test_other_target_is_format_error(self):
        code, _, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Elsewhere", "-o", self.out)
        self.assertEqual(code, 3)
        self.assertIn("targets 'OTHER'", err)
        self.assertIn("only SAVE is supported", err)
        self.assertIn(":%d:" % (PATCHFILE.splitlines().index("[Elsewhere]") + 1), err)
        self.assertFormatted(err)
        self.assertFalse(self.out.exists())

    def test_unknown_code_is_usage_error(self):
        code, _, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Nope", "-o", self.out)
        self.assertEqual(code, 2)
        self.assertFormatted(err)
        self.assertFalse(self.out.exists())

    def test_out_of_range_writes_nothing(self):
        for name in ("Past the end", "Past the end 2"):
            code, _, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Gald 10M", "--code", name,
                                   "-o", self.out)
            self.assertEqual(code, 3)
            self.assertIn("outside", err)
            self.assertFormatted(err)
            self.assertFalse(self.out.exists())
            self.assertEqual(self.save.read_bytes(), self.original)
        code, _, err = run_cli("patch", self.save, "--patch", self.patch, "--code", "Past the end", "--in-place")
        self.assertEqual(code, 3)
        self.assertEqual(self.save.read_bytes(), self.original)

    def test_bad_raw_is_format_error(self):
        for raw in ("000002E8", "0000000G 00000000", "30000000 00000000"):
            code, _, err = run_cli("patch", self.save, "--raw", raw, "-o", self.out)
            self.assertEqual(code, 3, raw)
            self.assertFormatted(err)
            self.assertFalse(self.out.exists())

    def test_existing_output_is_io_error(self):
        self.out.write_bytes(b"keep")
        code, _, err = run_cli("patch", self.save, "--raw", GALD, "-o", self.out)
        self.assertEqual(code, 1)
        self.assertFormatted(err)
        self.assertEqual(self.out.read_bytes(), b"keep")

    def test_output_equal_to_input_is_usage_error(self):
        code, _, err = run_cli("patch", self.save, "--raw", GALD, "-o", self.tmp / "." / "SAVE")
        self.assertEqual(code, 2)
        self.assertIn("--in-place", err)
        self.assertEqual(self.save.read_bytes(), self.original)

    def test_option_rule_violations(self):
        cases = [
            ("--code without --patch", ["patch", self.save, "--code", "x", "-o", self.out]),
            ("neither -o nor --in-place", ["patch", self.save, "--raw", GALD]),
            ("both", ["patch", self.save, "--raw", GALD, "-o", self.out, "--in-place"]),
            ("--list with --code", ["patch", "--patch", self.patch, "--list", "--code", "Byte a"]),
            ("--list without --patch", ["patch", "--list"]),
            ("--list with SAVE", ["patch", self.save, "--patch", self.patch, "--list"]),
            ("--list with -o", ["patch", "--patch", self.patch, "--list", "-o", self.out]),
            ("no SAVE", ["patch", "--raw", GALD, "-o", self.out]),
            ("no code", ["patch", self.save, "-o", self.out]),
        ]
        for label, argv in cases:
            code, _, err = run_cli(*argv)
            self.assertEqual(code, 2, label)
            self.assertNotIn("Traceback", err)
        self.assertFalse(self.out.exists())
        self.assertEqual(self.save.read_bytes(), self.original)

    def test_encrypted_save_is_format_error(self):
        plain = bytes(fx.build_save(0x400))
        files = fx.build_pfd_dir({"SAVE": plain}, ["SAVE"], syscon_key=fx.FAKE_SYSCON_KEY,
                                 secure_file_id=fx.FAKE_SECURE_ID)
        self.save.write_bytes(files["SAVE"])
        code, _, err = run_cli("patch", self.save, "--raw", GALD, "-o", self.out)
        self.assertEqual(code, 3)
        self.assertIn("decrypt", err)
        self.assertFalse(self.out.exists())

    def test_missing_files_are_io_errors(self):
        code, _, err = run_cli("patch", self.tmp / "missing", "--raw", GALD, "-o", self.out)
        self.assertEqual(code, 1)
        code, _, err = run_cli("patch", self.save, "--patch", self.tmp / "missing", "--code", "x", "-o", self.out)
        self.assertEqual(code, 1)
        code, _, err = run_cli("patch", "--patch", self.tmp / "missing", "--list")
        self.assertEqual(code, 1)
        self.assertFormatted(err)

    def test_invalid_utf8_savepatch(self):
        self.patch.write_bytes(b"[x]\n\xff\xfe\n")
        code, _, err = run_cli("patch", "--patch", self.patch, "--list")
        self.assertEqual(code, 3)
        self.assertFormatted(err)


if __name__ == "__main__":
    unittest.main()

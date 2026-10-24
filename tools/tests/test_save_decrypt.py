import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tox2_save import cli
from tox2_save.save import parse_save

from tests import save_fixture as fx

TOOLS_DIR = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS_DIR / "tox2_savetool.py"


def run_cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class DecryptTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.plain = {
            "SAVE": bytes(fx.build_save(0x400)),
            "ICON0.PNG": b"\x89PNG fake icon",
            "PARAM.SFO": b"\0PSF fake sfo",
        }
        self.save_dir = self.tmp / "BLUS31397_SAVE1_000"
        fx.write_dir(
            self.save_dir,
            fx.build_pfd_dir(self.plain, ["SAVE"], syscon_key=fx.FAKE_SYSCON_KEY, secure_file_id=fx.FAKE_SECURE_ID))
        self.keys = self.tmp / "keys"
        self.keys.mkdir()
        (self.keys / "key_vault.h").write_text(fx.fake_key_vault_h(fx.FAKE_SYSCON_KEY), encoding="utf-8")
        self.out_dir = self.tmp / "out"

    def decrypt(self, *extra, out_dir=None, save_dir=None, secure_id=True, keys=True):
        argv = ["decrypt", save_dir or self.save_dir, "-o", out_dir or self.out_dir]
        if keys:
            argv += ["--keys", self.keys]
        if secure_id:
            argv += ["--secure-file-id", fx.FAKE_SECURE_ID.hex()]
        return run_cli(*argv, *extra)

    def assert_key_not_leaked(self, *texts):
        for text in texts:
            for key_hex in (fx.FAKE_SYSCON_KEY.hex(), fx.FAKE_SYSCON_KEY.hex().upper()):
                self.assertFalse(key_hex in text, "output contains key bytes")


class DecryptSuccessTest(DecryptTestBase):
    def test_output_equals_plain_files(self):
        before = fx.tree_digest(self.save_dir)
        code, out, err = self.decrypt()
        self.assertEqual(code, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(sorted(p.name for p in self.out_dir.iterdir()), sorted(self.plain))
        for name, data in self.plain.items():
            self.assertEqual((self.out_dir / name).read_bytes(), data, name)
        self.assertFalse((self.out_dir / "PARAM.PFD").exists())
        self.assertEqual(fx.tree_digest(self.save_dir), before)
        parse_save((self.out_dir / "SAVE").read_bytes())

    def test_output_lines(self):
        _, out, _ = self.decrypt()
        self.assertEqual(out.splitlines(), [
            "ICON0.PNG: copied",
            "PARAM.SFO: copied",
            "SAVE: decrypted (%d bytes)" % len(self.plain["SAVE"]),
            "output: %s" % self.out_dir,
        ])

    def test_existing_empty_out_dir_is_accepted(self):
        self.out_dir.mkdir()
        code, _, err = self.decrypt()
        self.assertEqual(code, 0, err)
        self.assertTrue((self.out_dir / "SAVE").is_file())

    def test_parents_of_out_dir_are_created(self):
        out_dir = self.tmp / "a" / "b" / "out"
        code, _, err = self.decrypt(out_dir=out_dir)
        self.assertEqual(code, 0, err)
        self.assertTrue((out_dir / "SAVE").is_file())

    def test_subdirectory_is_skipped_with_note(self):
        (self.save_dir / "sub").mkdir()
        code, _, err = self.decrypt()
        self.assertEqual(code, 0, err)
        self.assertIn("note: skipping subdirectory sub", err)
        self.assertFalse((self.out_dir / "sub").exists())

    def test_key_is_not_loaded_when_nothing_is_encrypted(self):
        files = fx.build_pfd_dir({"PARAM.SFO": b"x", "ICON0.PNG": b"y"}, [],
                                 syscon_key=fx.FAKE_SYSCON_KEY, secure_file_id=fx.FAKE_SECURE_ID)
        save_dir = self.tmp / "plain_with_pfd"
        fx.write_dir(save_dir, files)
        code, _, err = run_cli("decrypt", save_dir, "-o", self.out_dir, "--keys", self.tmp / "no-keys")
        self.assertEqual(code, 0, err)

    def test_no_key_bytes_in_output(self):
        for kwargs in ({}, {"secure_id": False}):
            _, out, err = self.decrypt(out_dir=self.tmp / ("o%d" % len(kwargs)), **kwargs)
            self.assert_key_not_leaked(out, err)


class DecryptFailureTest(DecryptTestBase):
    def test_builtin_secure_id_differs_from_fixture(self):
        code, _, err = self.decrypt(secure_id=False)
        self.assertEqual(code, 4)
        self.assertIn("--secure-file-id", err)
        self.assertFalse(self.out_dir.exists())

    def test_non_empty_out_dir(self):
        self.out_dir.mkdir()
        (self.out_dir / "keep").write_bytes(b"x")
        code, _, err = self.decrypt()
        self.assertEqual(code, 1)
        self.assertIn("non-empty", err)
        self.assertEqual([p.name for p in self.out_dir.iterdir()], ["keep"])

    def test_out_dir_is_save_dir(self):
        code, _, err = self.decrypt(out_dir=self.save_dir)
        self.assertEqual(code, 2, err)

    def test_out_dir_inside_save_dir(self):
        code, _, err = self.decrypt(out_dir=self.save_dir / "plain")
        self.assertEqual(code, 2, err)
        self.assertFalse((self.save_dir / "plain").exists())

    def test_missing_pfd(self):
        plain_dir = self.tmp / "plain"
        fx.write_dir(plain_dir, self.plain)
        code, _, err = self.decrypt(save_dir=plain_dir)
        self.assertEqual(code, 3)
        self.assertIn("PARAM.PFD", err)
        self.assertFalse(self.out_dir.exists())

    def test_missing_key_file(self):
        code, _, err = run_cli("decrypt", self.save_dir, "-o", self.out_dir, "--keys", self.tmp / "no-keys",
                               "--secure-file-id", fx.FAKE_SECURE_ID.hex())
        self.assertEqual(code, 1)
        self.assertIn("docs/building.md", err)
        self.assertFalse(self.out_dir.exists())

    def test_plain_bytes_not_a_save(self):
        plain = dict(self.plain, SAVE=b"\x01" * 0x400)
        fx.write_dir(
            self.save_dir,
            fx.build_pfd_dir(plain, ["SAVE"], syscon_key=fx.FAKE_SYSCON_KEY, secure_file_id=fx.FAKE_SECURE_ID))
        code, _, err = self.decrypt()
        self.assertEqual(code, 4)
        self.assertIn("SC_ISO_SERIES_KEY_1", err)
        self.assertFalse(self.out_dir.exists())

    def test_wrong_key_gives_verify_error(self):
        wrong = bytes(range(0x50, 0x60))
        (self.keys / "key_vault.h").write_text(fx.fake_key_vault_h(wrong), encoding="utf-8")
        code, _, err = self.decrypt()
        self.assertEqual(code, 4)
        self.assertFalse(wrong.hex() in err, "output contains key bytes")

    def test_missing_encrypted_file(self):
        (self.save_dir / "SAVE").unlink()
        code, _, err = self.decrypt()
        self.assertEqual(code, 3)
        self.assertIn("SAVE", err)

    def test_bad_secure_file_id(self):
        for bad in ("", "zz" * 16, "00" * 15, "00" * 17):
            code, _, err = run_cli("decrypt", self.save_dir, "-o", self.out_dir, "--keys", self.keys,
                                   "--secure-file-id", bad)
            self.assertEqual(code, 2, bad)
            self.assertIn("--secure-file-id", err)
        self.assertFalse(self.out_dir.exists())

    def test_save_dir_is_not_a_directory(self):
        code, _, err = self.decrypt(save_dir=self.tmp / "missing")
        self.assertEqual(code, 1)
        self.assertEqual(len(err.splitlines()), 1)

    def test_no_key_bytes_in_errors(self):
        _, out, err = self.decrypt(secure_id=False)
        self.assert_key_not_leaked(out, err)


class SubprocessTest(unittest.TestCase):
    def run_script(self, *argv):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), *argv], capture_output=True, text=True,
                              check=False)

    def test_help(self):
        proc = self.run_script("--help")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("decrypt", proc.stdout)

    def test_no_arguments_is_usage_error(self):
        proc = self.run_script()
        self.assertEqual(proc.returncode, 2)
        self.assertNotIn("Traceback", proc.stderr)

    def test_missing_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            proc = self.run_script("decrypt", str(Path(tmp) / "missing"), "-o", str(Path(tmp) / "out"))
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(len(proc.stderr.splitlines()), 1)
        self.assertTrue(proc.stderr.startswith("error: "))
        self.assertNotIn("Traceback", proc.stderr)


if __name__ == "__main__":
    unittest.main()

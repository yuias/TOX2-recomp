import re
import tempfile
import unittest
from pathlib import Path

from tox2_save import EXIT_IO, CliError, pfd

from tests.save_fixture import FAKE_SYSCON_KEY, fake_key_vault_h

REPO_ROOT = Path(__file__).resolve().parents[2]
KEY_HEX = FAKE_SYSCON_KEY.hex()


class ParseSysconKeyTest(unittest.TestCase):
    def test_lf_and_crlf(self):
        for crlf in (False, True):
            with self.subTest(crlf=crlf):
                text = fake_key_vault_h(FAKE_SYSCON_KEY, crlf=crlf)
                self.assertEqual(pfd.parse_syscon_key(text), FAKE_SYSCON_KEY)

    def test_not_found(self):
        with self.assertRaises(CliError) as ctx:
            pfd.parse_syscon_key(fake_key_vault_h(FAKE_SYSCON_KEY, omit=True))
        self.assertEqual(ctx.exception.code, EXIT_IO)
        message = str(ctx.exception)
        self.assertEqual(message, "SC_ISO_SERIES_KEY_1: not found in key_vault.h")
        self.assertFalse(KEY_HEX in message.lower(), "message contains key bytes")

    def test_short_key(self):
        with self.assertRaises(CliError) as ctx:
            pfd.parse_syscon_key(fake_key_vault_h(FAKE_SYSCON_KEY, short=True))
        self.assertEqual(ctx.exception.code, EXIT_IO)
        message = str(ctx.exception)
        self.assertEqual(message, "SC_ISO_SERIES_KEY_1: found 15 bytes, expected 16")
        self.assertFalse(KEY_HEX in message.lower(), "message contains key bytes")

    def test_unclosed_initializer(self):
        with self.assertRaises(CliError) as ctx:
            pfd.parse_syscon_key("u8 SC_ISO_SERIES_KEY_1[16] = { 0x01, 0x02")
        self.assertEqual(ctx.exception.code, EXIT_IO)


class LoadSysconKeyTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def test_loads_from_directory(self):
        (self.tmp / "key_vault.h").write_text(fake_key_vault_h(FAKE_SYSCON_KEY), encoding="utf-8")
        self.assertEqual(pfd.load_syscon_key(self.tmp), FAKE_SYSCON_KEY)

    def test_missing_file(self):
        with self.assertRaises(CliError) as ctx:
            pfd.load_syscon_key(self.tmp)
        self.assertEqual(ctx.exception.code, EXIT_IO)
        message = str(ctx.exception)
        self.assertIn("docs/building.md", message)
        self.assertIn("Keys", message)
        self.assertIn(str(self.tmp / "key_vault.h"), message)


class DefaultKeysDirTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def test_uses_pinned_commit(self):
        text = (REPO_ROOT / "cmake" / "rpcs3_keys.cmake").read_text(encoding="utf-8")
        commit = re.search(r'set\(TOX2_RPCS3_KEYS_COMMIT "([0-9a-f]{40})"\)', text).group(1)
        path = pfd.default_keys_dir()
        self.assertEqual(path.parts[-4:], ("generated", "deps", "rpcs3", commit))
        self.assertEqual(path, REPO_ROOT / "generated" / "deps" / "rpcs3" / commit)

    def test_cmake_without_commit_line(self):
        cmake = self.tmp / "keys.cmake"
        cmake.write_text('set(OTHER "1")\n', encoding="utf-8")
        with self.assertRaises(CliError) as ctx:
            pfd.default_keys_dir(cmake)
        self.assertEqual(ctx.exception.code, EXIT_IO)

    def test_missing_cmake_file(self):
        with self.assertRaises(CliError) as ctx:
            pfd.default_keys_dir(self.tmp / "absent.cmake")
        self.assertEqual(ctx.exception.code, EXIT_IO)

    def test_custom_cmake_file(self):
        cmake = self.tmp / "keys.cmake"
        commit = "0123456789abcdef0123456789abcdef01234567"
        cmake.write_text('set(TOX2_RPCS3_KEYS_COMMIT "%s")\n' % commit, encoding="utf-8")
        self.assertEqual(pfd.default_keys_dir(cmake).name, commit)


if __name__ == "__main__":
    unittest.main()

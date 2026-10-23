"""Tests against real save data. Skipped unless TOX2_US_SAVE_DIR and TOX2_JP_SAVE_DIR are set.

TOX2_US_SAVE_DIR is a US save directory (BLUS31397_SAVE1_<NNN>), TOX2_JP_SAVE_DIR the recomp save
(BLJS10188_SAVE1_<NNN>). The key comes from TOX2_SAVE_KEYS_DIR, or from the build's default directory.
"""

import contextlib
import io
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from tox2_save import cli, pfd, sfo

from tests import save_fixture as fx

_US_DIR = os.environ.get("TOX2_US_SAVE_DIR")
_JP_DIR = os.environ.get("TOX2_JP_SAVE_DIR")
_KEYS_ENV = os.environ.get("TOX2_SAVE_KEYS_DIR")

_SECTION_COUNT = 44


def _keys_dir() -> Path:
    return Path(_KEYS_ENV) if _KEYS_ENV else pfd.default_keys_dir()


def _have_keys() -> bool:
    try:
        return (_keys_dir() / "key_vault.h").is_file()
    except Exception:
        return False


def _run_cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


_cache: dict = {}


def _decrypted_us() -> dict:
    # Decrypting 0xACD00 bytes in pure Python takes seconds, so it runs once per module.
    if not _cache:
        tmp = tempfile.TemporaryDirectory()
        out_dir = Path(tmp.name) / "us"
        # Taken before the first run so that test_source_is_unchanged sees every run of the module.
        source_before = fx.tree_digest(Path(_US_DIR))
        start = time.monotonic()
        code, out, err = _run_cli("decrypt", _US_DIR, "-o", out_dir, "--keys", _keys_dir())
        _cache.update(tmp=tmp, out_dir=out_dir, source_before=source_before, code=code, stdout=out, stderr=err,
                      seconds=time.monotonic() - start)
    return _cache


def tearDownModule():
    tmp = _cache.get("tmp")
    if tmp is not None:
        tmp.cleanup()
    _cache.clear()


@unittest.skipUnless(_US_DIR, "TOX2_US_SAVE_DIR is not set")
@unittest.skipUnless(_have_keys(), "key_vault.h is not available")
class RealDecryptTest(unittest.TestCase):
    def test_decrypt_succeeds(self):
        result = _decrypted_us()
        self.assertEqual(result["code"], 0, result["stderr"])

    def test_output_names(self):
        result = _decrypted_us()
        source = {p.name for p in Path(_US_DIR).iterdir() if p.is_file()} - {"PARAM.PFD"}
        self.assertEqual({p.name for p in result["out_dir"].iterdir()}, source)

    def test_save_size(self):
        result = _decrypted_us()
        self.assertEqual((result["out_dir"] / "SAVE").stat().st_size, 0xACD00)

    @unittest.skipUnless(_JP_DIR, "TOX2_JP_SAVE_DIR is not set")
    def test_header_matches_jp_save(self):
        # The first 0x2D0 bytes are the section table of the game's SAVE; both regions share it.
        us = (_decrypted_us()["out_dir"] / "SAVE").read_bytes()
        jp = (Path(_JP_DIR) / "SAVE").read_bytes()
        self.assertTrue(us[:0x2D0] == jp[:0x2D0], "SAVE header differs from the JP SAVE")

    def test_wrong_secure_file_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "out"
            code, _, err = _run_cli("decrypt", _US_DIR, "-o", out_dir, "--keys", _keys_dir(),
                                    "--secure-file-id", "00" * 16)
            self.assertEqual(code, 4, err)
            self.assertFalse(out_dir.exists())

    def test_source_is_unchanged(self):
        before = _decrypted_us()["source_before"]
        self.assertEqual(fx.tree_digest(Path(_US_DIR)), before)

    def test_output_does_not_contain_the_key(self):
        result = _decrypted_us()
        key = pfd.load_syscon_key(_keys_dir())
        key_hex = key.hex()
        for text in (result["stdout"], result["stderr"]):
            for variant in (key_hex, key_hex.upper()):
                self.assertFalse(variant in text, "output contains key bytes")
        for path in result["out_dir"].iterdir():
            self.assertFalse(key in path.read_bytes(), "decrypted file contains key bytes")


def _info_json(save_path: Path) -> dict:
    code, out, err = _run_cli("info", save_path, "--json")
    assert code == 0, err
    return json.loads(out)


class _RealInfoMixin:
    def save_path(self) -> Path:
        raise NotImplementedError

    def test_info_json(self):
        code, out, err = _run_cli("info", self.save_path(), "--json")
        self.assertEqual(code, 0, err)
        doc = json.loads(out)
        self.assertEqual(len(doc["sections"]), _SECTION_COUNT)
        self.assertEqual(len(doc["characters"]), 9)

    def test_named_sections(self):
        by_name = {s["name"]: s for s in _info_json(self.save_path())["sections"]}
        self.assertEqual(by_name["PLAYER_3_STATUS_SAVE_DATA"]["offset"], "0x00008890")
        self.assertEqual(by_name["SAVE_DATA_ID_ITEM_DATA_MANAGER"]["offset"], "0x0008a170")

    def test_info_text(self):
        code, out, err = _run_cli("info", self.save_path())
        self.assertEqual(code, 0, err)
        self.assertEqual(err, "")
        self.assertIn("sections:   %d" % _SECTION_COUNT, out.splitlines())
        for line in out.splitlines():
            self.assertEqual(line, line.rstrip())


@unittest.skipUnless(_JP_DIR, "TOX2_JP_SAVE_DIR is not set")
class RealInfoJpTest(_RealInfoMixin, unittest.TestCase):
    def save_path(self) -> Path:
        return Path(_JP_DIR) / "SAVE"


@unittest.skipUnless(_US_DIR, "TOX2_US_SAVE_DIR is not set")
@unittest.skipUnless(_have_keys(), "key_vault.h is not available")
class RealInfoUsTest(_RealInfoMixin, unittest.TestCase):
    def save_path(self) -> Path:
        result = _decrypted_us()
        self.assertEqual(result["code"], 0, result["stderr"])
        return result["out_dir"] / "SAVE"


@unittest.skipUnless(_JP_DIR, "TOX2_JP_SAVE_DIR is not set")
class RealSfoJpTest(unittest.TestCase):
    def test_round_trip_is_identical(self):
        # The runtime wrote this file with serializeParamSfo, so the Python writer must reproduce it.
        data = (Path(_JP_DIR) / "PARAM.SFO").read_bytes()
        self.assertTrue(sfo.serialize_sfo(sfo.parse_sfo(data)) == data, "re-serialized PARAM.SFO differs")

    def test_save_is_secure(self):
        parsed = sfo.parse_sfo((Path(_JP_DIR) / "PARAM.SFO").read_bytes())
        self.assertEqual(parsed.ints["*SAVE"], 1)


@unittest.skipUnless(_US_DIR, "TOX2_US_SAVE_DIR is not set")
class RealSfoUsTest(unittest.TestCase):
    def test_parses_with_params(self):
        parsed = sfo.parse_sfo((Path(_US_DIR) / "PARAM.SFO").read_bytes())
        self.assertIn("PARAMS", parsed.binary)


if __name__ == "__main__":
    unittest.main()

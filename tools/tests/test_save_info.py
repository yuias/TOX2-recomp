import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tox2_save import cli, save

from tests import save_fixture as fx


def run_cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class InfoTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        buf = fx.build_save()
        struct.pack_into(">I", buf, save.GALD_OFFSET, 123456)
        struct.pack_into(">I", buf, save.PLAY_TIME_OFFSET, 7890)
        base = save.CHARACTER_BASE + 8 * save.CHARACTER_STRIDE
        struct.pack_into(">I", buf, base + save.HP, 1111)
        struct.pack_into(">I", buf, base + save.TP, 222)
        struct.pack_into(">I", buf, base + save.EXP, 33333)
        buf[base + save.AC] = 4
        struct.pack_into(">I", buf, base + save.SP, 55)
        for i in range(6):
            struct.pack_into(">H", buf, base + save.STATS + i * save.STAT_STRIDE, 101 + i)
        self.path = self.tmp / "SAVE"
        self.path.write_bytes(buf)

    def test_text_lines(self):
        code, out, err = run_cli("info", self.path)
        self.assertEqual(code, 0, err)
        lines = out.splitlines()
        self.assertEqual(lines[0], "size:       %d (total size field %d)" % (0xACD00, 0xACD00))
        self.assertEqual(lines[1], "sections:   3")
        self.assertIn("section  1 offset 0x00008890 size 0x0000a000 PLAYER_3_STATUS_SAVE_DATA", lines)
        self.assertIn("gald:       123456", lines)
        self.assertIn("play time:  0:02:11 (7890 frames)", lines)
        self.assertIn("character 8 Ludger: hp 1111 tp 222 exp 33333 ac 4 sp 55 str 101 int 102 vit 103 dex 104 "
                      "agi 105 spi 106", lines)
        self.assertEqual(len([ln for ln in lines if ln.startswith("character ")]), 9)

    def test_unnamed_section(self):
        # The game's own SAVE has unnamed sections; they print as a placeholder, not a trailing space.
        self.path.write_bytes(fx.build_save(0xACD00, [("", 0x2D0, 0x100), ("NAMED", 0x8890, 0x10)]))
        code, out, err = run_cli("info", self.path)
        self.assertEqual(code, 0, err)
        lines = out.splitlines()
        self.assertIn("section  0 offset 0x000002d0 size 0x00000100 (unnamed)", lines)
        self.assertIn("section  1 offset 0x00008890 size 0x00000010 NAMED", lines)
        code, out, _ = run_cli("info", self.path, "--json")
        self.assertEqual(json.loads(out)["sections"][0]["name"], "")

    def test_json(self):
        code, out, err = run_cli("info", self.path, "--json")
        self.assertEqual(code, 0, err)
        doc = json.loads(out)
        self.assertEqual(
            set(doc), {"schema", "size", "total_size", "sections", "gald", "play_time_frames", "characters"})
        self.assertEqual(doc["schema"], 1)
        self.assertEqual(doc["gald"], 123456)
        self.assertEqual([c["name"] for c in doc["characters"]], list(save.CHARACTERS))
        self.assertEqual(doc["characters"][8]["hp"], 1111)
        self.assertEqual(doc["sections"][1]["name"], "PLAYER_3_STATUS_SAVE_DATA")
        self.assertEqual(doc["sections"][1]["offset"], "0x00008890")

    def test_encrypted_save_is_format_error(self):
        plain = bytes(fx.build_save(0x400))
        files = fx.build_pfd_dir({"SAVE": plain}, ["SAVE"], syscon_key=fx.FAKE_SYSCON_KEY,
                                 secure_file_id=fx.FAKE_SECURE_ID)
        self.path.write_bytes(files["SAVE"])
        code, _, err = run_cli("info", self.path)
        self.assertEqual(code, 3)
        self.assertIn("decrypt", err)
        self.assertNotIn("Traceback", err)

    def test_missing_file(self):
        code, _, err = run_cli("info", self.tmp / "missing")
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "))

    def test_small_save_parses_but_cannot_be_described(self):
        self.path.write_bytes(fx.build_save(0x400))
        code, _, err = run_cli("info", self.path)
        self.assertEqual(code, 3)
        self.assertEqual(len(err.splitlines()), 1)


if __name__ == "__main__":
    unittest.main()

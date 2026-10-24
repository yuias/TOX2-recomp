import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from tox2_save import cli, save, sfo

from tests import save_fixture as fx

SAVE_ROOT = Path("dev_hdd0") / "home" / "00000001" / "savedata"


def run_cli(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


def name_offset(k: int) -> int:
    return save.CHARACTER_BASE + k * save.CHARACTER_STRIDE + save.NAME


def set_names(data: bytearray, names) -> None:
    for k, name in enumerate(names):
        raw = name.encode("utf-8")
        data[name_offset(k):name_offset(k) + len(raw) + 1] = raw + b"\0"


def stored_names(data: bytes) -> list[str]:
    names = []
    for k in range(len(save.CHARACTERS)):
        field = data[name_offset(k):name_offset(k) - save.NAME + save.NAME_END]
        names.append(field[:field.find(b"\0")].decode("utf-8"))
    return names


def us_like_sfo(sub_title: str = "000 Sub") -> bytes:
    # Mirrors the US save: no '*' keys, binary PARAMS/PARAMS2 and a foreign account id.
    return sfo.serialize_sfo(
        sfo.Sfo(
            ints={
                "ATTRIBUTE": 5,
                "PARENTAL_LEVEL": 3
            },
            strings={
                "ACCOUNT_ID": "1234567890abcdef",
                "TITLE": "Title",
                "SUB_TITLE": sub_title,
                "DETAIL": "Detail",
                "SAVEDATA_LIST_PARAM": "list",
                "SAVEDATA_DIRECTORY": "BLUS31397_SAVE1_000",
                "CATEGORY": "SD",
            },
        ))


class InstallTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.plain = self.tmp / "plain"
        self.files = {"SAVE": bytes(fx.build_save()), "ICON0.PNG": b"\x89PNG-fake", "PARAM.SFO": us_like_sfo()}
        fx.write_dir(self.plain, self.files)
        self.hdd = self.tmp / "hdd"
        self.hdd.mkdir()
        self.root = self.hdd / SAVE_ROOT

    def assertFormatted(self, err):
        self.assertTrue(err.startswith("error: "), err)
        self.assertEqual(len(err.splitlines()), 1, err)
        self.assertNotIn("Traceback", err)

    def test_slots_are_numbered_from_one(self):
        before = fx.tree_digest(self.plain)
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        self.assertTrue((self.root / "BLJS10188_SAVE1_001").is_dir())
        self.assertEqual(
            out.splitlines(),
            ["slot:   BLJS10188_SAVE1_001", "files:  ICON0.PNG, PARAM.SFO, SAVE",
             "output: %s" % (self.root / "BLJS10188_SAVE1_001")])
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        self.assertTrue((self.root / "BLJS10188_SAVE1_002").is_dir())
        self.assertEqual(fx.tree_digest(self.plain), before)

    def test_scan_takes_the_lowest_free_slot(self):
        for n in ("001", "003"):
            (self.root / ("BLJS10188_SAVE1_" + n)).mkdir(parents=True)
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.splitlines()[0], "slot:   BLJS10188_SAVE1_002")

    def test_explicit_slot_and_repeat(self):
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd, "--slot", "7")
        self.assertEqual(code, 0, err)
        self.assertTrue((self.root / "BLJS10188_SAVE1_007").is_dir())
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd, "--slot", "7")
        self.assertEqual(code, 1)
        self.assertFormatted(err)
        self.assertIn("already exists", err)

    def test_slot_999_is_accepted(self):
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd, "--slot", "999")
        self.assertEqual(code, 0, err)
        self.assertTrue((self.root / "BLJS10188_SAVE1_999").is_dir())

    def test_files_and_sfo(self):
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        slot = self.root / "BLJS10188_SAVE1_001"
        self.assertEqual({p.name for p in slot.iterdir()}, set(self.files))
        self.assertEqual((slot / "SAVE").read_bytes(), self.files["SAVE"])
        self.assertEqual((slot / "ICON0.PNG").read_bytes(), self.files["ICON0.PNG"])
        parsed = sfo.parse_sfo((slot / "PARAM.SFO").read_bytes())
        self.assertEqual(
            parsed.strings, {
                "ACCOUNT_ID": "0000000000000000",
                "CATEGORY": "SD",
                "SAVEDATA_DIRECTORY": "BLJS10188_SAVE1_001",
                "DETAIL": "Detail",
                "SAVEDATA_LIST_PARAM": "list",
                "SUB_TITLE": "001 Sub",
                "TITLE": "Title",
            })
        self.assertEqual(parsed.ints, {"ATTRIBUTE": 5, "PARENTAL_LEVEL": 3, "*SAVE": 1, "*ICON0.PNG": 0})
        self.assertEqual(parsed.binary, {})

    def installed_sfo(self, slot: str) -> sfo.Sfo:
        return sfo.parse_sfo((self.root / ("BLJS10188_SAVE1_" + slot) / "PARAM.SFO").read_bytes())

    def test_sub_title_takes_the_slot_number(self):
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd, "--slot", "42")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.installed_sfo("042").strings["SUB_TITLE"], "042 Sub")

    def test_sub_title_without_a_number_is_kept(self):
        for sub_title in ("Sub", "12 Sub", "1234 Sub", "123Sub"):
            with self.subTest(sub_title=sub_title):
                (self.plain / "PARAM.SFO").write_bytes(us_like_sfo(sub_title))
                code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
                self.assertEqual(code, 0, err)
                slot = out.splitlines()[0][-3:]
                self.assertEqual(self.installed_sfo(slot).strings["SUB_TITLE"], sub_title)

    def test_names_are_left_alone_by_default(self):
        data = bytearray(self.files["SAVE"])
        set_names(data, save.DEFAULT_NAMES["en"])
        (self.plain / "SAVE").write_bytes(data)
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        self.assertEqual((self.root / "BLJS10188_SAVE1_001" / "SAVE").read_bytes(), data)
        self.assertNotIn("names:", out)

    def test_names_both_directions(self):
        for source, target in (("en", "ja"), ("ja", "en")):
            with self.subTest(target=target):
                data = bytearray(self.files["SAVE"])
                set_names(data, save.DEFAULT_NAMES[source])
                (self.plain / "SAVE").write_bytes(data)
                code, out, err = run_cli("install", self.plain, "--hdd", self.hdd, "--names", target)
                self.assertEqual(code, 0, err)
                self.assertIn("names:  9 of 9 set to %s" % target, out.splitlines())
                installed = (self.root / out.splitlines()[0].split()[-1] / "SAVE").read_bytes()
                self.assertEqual(stored_names(installed), list(save.DEFAULT_NAMES[target]))
                expected = bytearray(self.files["SAVE"])
                set_names(expected, save.DEFAULT_NAMES[target])
                # Only the name fields change, and no tail of a longer name is left behind.
                self.assertEqual(installed, expected)

    def test_unknown_and_target_names_are_kept(self):
        data = bytearray(self.files["SAVE"])
        names = list(save.DEFAULT_NAMES["en"])
        names[0] = "Custom"
        names[1] = save.DEFAULT_NAMES["ja"][1]
        names[7] = "Muzet"  # without the accent it is not the US default
        set_names(data, names)
        (self.plain / "SAVE").write_bytes(data)
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd, "--names", "ja")
        self.assertEqual(code, 0, err)
        self.assertIn("names:  6 of 9 set to ja", out.splitlines())
        installed = (self.root / "BLJS10188_SAVE1_001" / "SAVE").read_bytes()
        expected = list(save.DEFAULT_NAMES["ja"])
        expected[0], expected[7] = "Custom", "Muzet"
        self.assertEqual(stored_names(installed), expected)

    def test_names_need_a_full_size_save(self):
        (self.plain / "SAVE").write_bytes(bytes(fx.build_save(size=0x10000)))
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd, "--names", "ja")
        self.assertEqual(code, 3)
        self.assertFormatted(err)
        self.assertEqual(list(self.hdd.iterdir()), [])

    def test_unknown_names_value(self):
        code, _, _ = run_cli("install", self.plain, "--hdd", self.hdd, "--names", "fr")
        self.assertEqual(code, 2)
        self.assertEqual(list(self.hdd.iterdir()), [])

    def test_encrypted_dir(self):
        (self.plain / "PARAM.PFD").write_bytes(b"x")
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 3)
        self.assertFormatted(err)
        self.assertIn("decrypt", err)
        self.assertEqual(list(self.hdd.iterdir()), [])

    def test_invalid_save(self):
        (self.plain / "SAVE").write_bytes(bytes(0x400))
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 3)
        self.assertFormatted(err)
        self.assertEqual(list(self.hdd.iterdir()), [])

    def test_missing_files(self):
        for name in ("PARAM.SFO", "SAVE"):
            with self.subTest(name=name):
                data = (self.plain / name).read_bytes()
                (self.plain / name).unlink()
                code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
                self.assertEqual(code, 3)
                self.assertFormatted(err)
                self.assertIn(name, err)
                self.assertEqual(list(self.hdd.iterdir()), [])
                (self.plain / name).write_bytes(data)

    def test_invalid_sfo(self):
        (self.plain / "PARAM.SFO").write_bytes(b"not an sfo")
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 3)
        self.assertFormatted(err)
        self.assertEqual(list(self.hdd.iterdir()), [])

    def test_missing_hdd(self):
        code, _, err = run_cli("install", self.plain, "--hdd", self.tmp / "nope")
        self.assertEqual(code, 1)
        self.assertFormatted(err)
        self.assertFalse((self.tmp / "nope").exists())

    def test_missing_plain_dir(self):
        code, _, err = run_cli("install", self.tmp / "nope", "--hdd", self.hdd)
        self.assertEqual(code, 1)
        self.assertFormatted(err)

    def test_slot_out_of_range(self):
        for slot in ("0", "1000", "-1"):
            with self.subTest(slot=slot):
                code, _, err = run_cli("install", self.plain, "--hdd", self.hdd, "--slot", slot)
                self.assertEqual(code, 2)
                self.assertEqual(list(self.hdd.iterdir()), [])

    def test_all_slots_taken(self):
        for i in range(1, 1000):
            (self.root / ("BLJS10188_SAVE1_%03d" % i)).mkdir(parents=True, exist_ok=True)
        code, _, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 1)
        self.assertFormatted(err)
        self.assertIn("no free slot", err)

    def test_subdirectory_and_unsafe_names_are_skipped(self):
        (self.plain / "sub").mkdir()
        (self.plain / "bad name").write_bytes(b"x")
        code, out, err = run_cli("install", self.plain, "--hdd", self.hdd)
        self.assertEqual(code, 0, err)
        self.assertIn("note: skipping subdirectory sub", err)
        self.assertIn("note: skipping bad name", err)
        self.assertEqual(out.splitlines()[1], "files:  ICON0.PNG, PARAM.SFO, SAVE")


if __name__ == "__main__":
    unittest.main()

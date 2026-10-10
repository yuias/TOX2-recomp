"""Tests for `spu-disasm` on a synthetic raw image with known channel, stop and MFC usage."""

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import ps3tool
from ps3recomp import spu_disasm
from ps3recomp.spu import decode
from ps3recomp.spu_format import format_line

from tests import elf_fixture as fx
from tests import spu_asm
from tests.elf_fixture import FixtureSegment

CONTAINER_VA = 0x10000
BLOB_AT = 0x100
LS_BASE = 0xA00
UNDECODED = 0x90000000


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def sample_code() -> list[int]:
    return [
        enc("il", RT=3, I16=0xD0),
        enc("wrch", CA=21, RT=3),                 # MFC_Cmd = 0xd0
        enc("ilhu", RT=4, U16=0x1),
        enc("iohl", RT=4, U16=0xB4),
        enc("wrch", CA=21, RT=4),                 # MFC_Cmd = 0x100b4
        enc("ila", RT=5, I18=0x40),
        enc("a", RT=5, RA=5, RB=3),               # no longer a constant
        enc("wrch", CA=21, RT=5),
        enc("ai", RT=6, RA=3, I10=-0x10),
        enc("br", addr=LS_BASE + 36, target=LS_BASE + 44),
        enc("wrch", CA=21, RT=6),                 # 0xc0; flow is ignored on purpose
        enc("rdch", RT=7, CA=13),
        enc("rdch", RT=8, CA=24),
        enc("rchcnt", RT=9, CA=24),
        enc("wrch", CA=28, RT=3),
        enc("stop", STOP=0x101),
        enc("stop", STOP=0x102),
        enc("stop", STOP=0x101),
        0,
        0,
        UNDECODED,
        enc("lnop"),
    ]


def words_bytes(words: list[int]) -> bytes:
    return b"".join(w.to_bytes(4, "big") for w in words)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class SpuDisasmTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.code = sample_code()
        body = bytearray(0x400)
        blob = words_bytes(self.code)
        body[BLOB_AT:BLOB_AT + len(blob)] = blob
        self.size = len(blob)

        def container() -> bytes:
            return fx.build_elf64_be(entry=0,
                                     segments=[FixtureSegment(1, CONTAINER_VA, bytes(body), flags=5)])

        eboot, libsre = container(), container()
        self.eboot_path = self.write("EBOOT.elf", eboot)
        self.libsre_path = self.write("libsre.reloc.elf", libsre)
        self.config_path = self.write("target.json", json.dumps({
            "eboot_elf": {"sha256": sha(eboot)},
            "libsre_reloc_elf": {"sha256": sha(libsre)}}).encode())
        self.registry_path = self.write("registry.json", json.dumps({"schema": 1, "images": [
            self.raw_entry("t_main"), self.raw_entry("t_other")]}).encode())

    def raw_entry(self, name: str) -> dict:
        return {"name": name, "container": "libsre", "ea": "0x%x" % (CONTAINER_VA + BLOB_AT),
                "kind": "raw", "size": "0x%x" % self.size, "ls_base": "0x%x" % LS_BASE,
                "role": "spurs_policy_module", "lowering": "stackfree", "gate": False,
                "extra_entries": []}

    def write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def run_tool(self, *argv):
        base = ["--eboot", str(self.eboot_path), "--libsre", str(self.libsre_path),
                "--registry", str(self.registry_path), "--target-config", str(self.config_path)]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ps3tool.main(["spu-disasm", *base, *argv])
        return code, out.getvalue(), err.getvalue()

    def census(self) -> dict:
        out_path = self.tmp / "census.json"
        code, _, err = self.run_tool("--stats", "--images", "t_main", "--json", str(out_path))
        self.assertEqual((code, err), (0, ""))
        doc = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(doc["schema"], 1)
        (image,) = doc["images"]
        return image


class CensusTest(SpuDisasmTest):
    def test_counts(self):
        image = self.census()
        self.assertEqual(image["name"], "t_main")
        self.assertEqual(image["ls_base"], "0xa00")
        # Zero words are padding, not `stop 0`; they are counted apart from decoded words.
        self.assertEqual((image["code_words"], image["decoded"], image["long"], image["zero"]),
                         (len(self.code), len(self.code) - 3, 1, 2))

    def test_channels(self):
        channels = self.census()["channels"]

        def numbers(mnemonic):
            return [(c["channel"], c["count"]) for c in channels[mnemonic]]

        self.assertEqual(numbers("rdch"), [(13, 1), (24, 1)])
        self.assertEqual(numbers("wrch"), [(21, 4), (28, 1)])
        self.assertEqual(numbers("rchcnt"), [(24, 1)])
        self.assertEqual(channels["rdch"][0]["name"], "SPU_RdMachStat")
        self.assertEqual(channels["wrch"][0]["name"], "MFC_Cmd")

    def test_stop_codes(self):
        stops = [(s["code"], s["count"]) for s in self.census()["stop_codes"]]
        self.assertEqual(stops, [("0x101", 2), ("0x102", 1)])

    def test_mfc_command_constants(self):
        mfc = self.census()["mfc_commands"]
        constants = [(c["value"], c["count"]) for c in mfc["constants"]]
        self.assertEqual(constants, [("0xc0", 1), ("0xd0", 1), ("0x100b4", 1)])
        # `a $5,$5,$3` made the third command register unknown.
        self.assertEqual(mfc["non_constant"], 1)

    def test_top_mnemonics_are_sorted_by_count_then_name(self):
        top = self.census()["top_mnemonics"]
        pairs = [(t["mnemonic"], t["count"]) for t in top]
        self.assertEqual(pairs[:3], [("wrch", 5), ("stop", 3), ("rdch", 2)])
        self.assertEqual(pairs, sorted(pairs, key=lambda p: (-p[1], p[0])))
        self.assertLessEqual(len(pairs), spu_disasm.TOP_MNEMONICS)

    def test_text_output_names_each_image(self):
        code, out, _ = self.run_tool("--stats", "--images", "t_main,t_other")
        self.assertEqual(code, 0)
        self.assertEqual([l for l in out.splitlines() if l.startswith("image ")],
                         ["image t_main", "image t_other"])
        self.assertIn("MFC_Cmd constants: 0xc0 x1, 0xd0 x1, 0x100b4 x1 (non-constant writes: 1)", out)
        self.assertIn("rdch channels: 13 SPU_RdMachStat x1, 24 MFC_RdTagStat x1", out)
        self.assertIn("stop codes: 0x101 x2, 0x102 x1", out)

    def test_json_is_deterministic(self):
        self.census()
        first = (self.tmp / "census.json").read_bytes()
        self.census()
        self.assertEqual((self.tmp / "census.json").read_bytes(), first)
        self.assertNotIn(b"\r", first)

    def test_default_images_are_all_with_a_known_ls_base(self):
        code, out, _ = self.run_tool("--stats")
        self.assertEqual(code, 0)
        self.assertEqual(out.count("\nimage ") + out.startswith("image "), 2)


class ListingTest(SpuDisasmTest):
    def test_whole_image(self):
        code, out, _ = self.run_tool("--image", "t_main")
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertEqual(len(lines), len(self.code))
        self.assertEqual(lines[0], format_line(decode(self.code[0], LS_BASE)))
        self.assertEqual(lines[0], "00a00: 40806803  il $3,208")
        self.assertEqual(lines[-2], "%05x: 90000000  .long 0x90000000" % (LS_BASE + 4 * (len(self.code) - 2)))

    def test_range(self):
        code, out, _ = self.run_tool("--image", "t_main", "--start", "0xa04", "--end", "0xa10")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(),
                         [format_line(decode(self.code[i], LS_BASE + 4 * i)) for i in (1, 2, 3)])

    def test_start_alone_lists_one_instruction(self):
        code, out, _ = self.run_tool("--images", "t_main", "--start", "0xa00")
        self.assertEqual((code, len(out.splitlines())), (0, 1))

    def test_range_outside_the_image(self):
        code, out, err = self.run_tool("--image", "t_main", "--start", "0x3000", "--end", "0x3010")
        self.assertEqual((code, out), (spu_disasm.EXIT_BAD_ADDRESS, ""))
        self.assertIn("not inside one code segment", err)
        code, _, _ = self.run_tool("--image", "t_main", "--start", "0xa00",
                                   "--end", "0x%x" % (LS_BASE + self.size + 4))
        self.assertEqual(code, spu_disasm.EXIT_BAD_ADDRESS)


class UsageTest(SpuDisasmTest):
    def test_errors(self):
        for argv in (
            ["--json", str(self.tmp / "x.json"), "--image", "t_main"],   # --json needs --stats
            ["--images", "t_main,t_other"],                              # a listing is one image
            [],                                                          # no image
            ["--image", "t_main", "--images", "t_main"],
            ["--stats", "--start", "0xa00"],
            ["--image", "t_main", "--start", "0xa02"],
            ["--image", "t_main", "--start", "0xa10", "--end", "0xa04"],
            ["--image", "t_main", "--end", "0xa10"],
        ):
            with self.subTest(argv=argv):
                self.assertEqual(self.run_tool(*argv)[0], 2)

    def test_unknown_image(self):
        code, _, err = self.run_tool("--image", "nope")
        self.assertEqual(code, 2)
        self.assertIn("unknown image nope", err)

    def test_container_hash_mismatch(self):
        self.write("target.json", json.dumps({
            "eboot_elf": {"sha256": "0" * 64}, "libsre_reloc_elf": {"sha256": "0" * 64}}).encode())
        self.assertEqual(self.run_tool("--stats")[0], 4)


class ChannelNameTest(unittest.TestCase):
    def test_mfc_command_channel_is_21(self):
        self.assertEqual(spu_disasm.CHANNEL_NAMES[spu_disasm.MFC_CMD_CHANNEL], "MFC_Cmd")


if __name__ == "__main__":
    unittest.main()

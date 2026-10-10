import contextlib
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ps3recomp import disasm

from tests import elf_fixture as fx

TOOLS_DIR = Path(__file__).resolve().parents[1]


class DisasmTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.elf_path = self.tmp / "input.elf"
        self.elf_path.write_bytes(fx.build_ps3_fixture())

    def run_disasm(self, *args, data: bytes | None = None):
        if data is not None:
            self.elf_path.write_bytes(data)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = disasm.main([str(self.elf_path), *args])
        return code, out.getvalue(), err.getvalue()


class ListingTest(DisasmTestBase):
    def test_entry_listing(self):
        code, out, _ = self.run_disasm("--entry", "--count", "7")
        lines = out.splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 7)
        self.assertEqual(lines[0], "0x00010000: 7c0802a6  mflr r0")
        self.assertEqual(lines[-1], "0x00010018: 4e800020  blr")

    def test_entry_default_count_is_clamped_to_segment(self):
        code, out, _ = self.run_disasm("--entry")
        self.assertEqual(code, 0)
        self.assertEqual(len(out.splitlines()), 8)

    def test_raw_mode(self):
        code, out, _ = self.run_disasm("--start", "0x10000", "--end", "0x10008", "--raw")
        lines = out.splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "0x00010000: 7c0802a6  mfspr r0,8")

    def test_start_count(self):
        code, out, _ = self.run_disasm("--start", "0x10004", "--count", "1")
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), ["0x00010004: f821ff91  stdu r1,-0x70(r1)"])

    def test_unknown_word_is_long(self):
        _, out, _ = self.run_disasm("--start", "0x1001c", "--count", "1")
        self.assertEqual(out.splitlines(), ["0x0001001c: 00000000  .long 0x00000000"])


def reserved_bit_elf() -> bytes:
    # mtspr with the reserved bit 31 set: decodes non-strictly, rejected by strict mode.
    elf = fx.build_ps3_fixture()
    original = struct.pack(">I", 0x7C0803A6)
    assert elf.count(original) == 1
    return elf.replace(original, struct.pack(">I", 0x7C0803A7))


class StrictListingTest(DisasmTestBase):
    def test_strict_listing_prints_long(self):
        data = reserved_bit_elf()
        _, lax, _ = self.run_disasm("--start", "0x10014", "--count", "1", "--raw", data=data)
        self.assertEqual(lax.splitlines(), ["0x00010014: 7c0803a7  mtspr 8,r0"])
        code, strict, _ = self.run_disasm("--start", "0x10014", "--count", "1", "--strict")
        self.assertEqual(code, 0)
        self.assertEqual(strict.splitlines(), ["0x00010014: 7c0803a7  .long 0x7c0803a7"])

    def test_strict_listing_keeps_valid_words(self):
        code, out, _ = self.run_disasm("--entry", "--count", "7", "--strict")
        self.assertEqual(code, 0)
        self.assertNotIn(".long", out)

    def test_strict_with_stats_is_accepted(self):
        _, plain, _ = self.run_disasm("--stats")
        code, out, _ = self.run_disasm("--stats", "--strict")
        self.assertEqual(code, 0)
        self.assertEqual(out, plain)


class RangeErrorTest(DisasmTestBase):
    def test_outside_exec_segment(self):
        code, _, err = self.run_disasm("--start", "0x20000", "--count", "1")
        self.assertEqual(code, 5)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_range_past_segment_end(self):
        code, _, _ = self.run_disasm("--start", "0x10000", "--count", "9")
        self.assertEqual(code, 5)

    def test_stats_range_outside(self):
        code, _, _ = self.run_disasm("--stats", "--start", "0x20000", "--end", "0x20010")
        self.assertEqual(code, 5)


class UsageErrorTest(DisasmTestBase):
    def test_usage_errors_exit_2(self):
        for args in (
            [],
            ["--start", "0x10000"],
            ["--start", "0x10000", "--count", "1", "--end", "0x10004"],
            ["--start", "0x10002", "--count", "1"],
            ["--start", "0x10008", "--end", "0x10000"],
            ["--entry", "--start", "0x10000"],
            ["--stats", "--count", "3"],
            ["--stats", "--start", "0x10000"],
            ["--start", "0x10000", "--count", "1", "--json", "x.json"],
            ["--start", "zzz", "--count", "1"],
        ):
            with self.subTest(args=args):
                code, _, _ = self.run_disasm(*args)
                self.assertEqual(code, 2)


class StatsTest(DisasmTestBase):
    def test_whole_segment(self):
        code, out, _ = self.run_disasm("--stats")
        self.assertEqual(code, 0)
        self.assertRegex(out, r"total words: 8\n")
        self.assertRegex(out, r"decoded:\s+7 ")
        self.assertRegex(out, r"unknown:\s+1 ")

    def test_subrange(self):
        code, out, _ = self.run_disasm("--stats", "--start", "0x10000", "--end", "0x10010")
        self.assertEqual(code, 0)
        self.assertRegex(out, r"total words: 4\n")
        self.assertRegex(out, r"decoded:\s+4 ")
        self.assertRegex(out, r"unknown:\s+0 ")

    def test_json_output(self):
        out_path = self.tmp / "nested" / "stats.json"
        code, _, _ = self.run_disasm("--stats", "--json", str(out_path))
        self.assertEqual(code, 0)
        doc = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(doc["schema"], 1)
        self.assertEqual((doc["total_words"], doc["decoded"], doc["unknown"]), (8, 7, 1))
        self.assertEqual(doc["ranges"], [{"start": "0x00010000", "end": "0x00010020"}])
        self.assertEqual(doc["top_unknown_patterns"],
                         [{"primary": 0, "xo_21_30": 0, "count": 1}])
        self.assertIn("stdu", [m["mnemonic"] for m in doc["top_mnemonics"]])

    def test_strict_block_canonical_fixture(self):
        out_path = self.tmp / "stats.json"
        code, out, _ = self.run_disasm("--stats", "--json", str(out_path))
        self.assertEqual(code, 0)
        strict = json.loads(out_path.read_text(encoding="utf-8"))["strict"]
        self.assertEqual(strict, {"decoded": 7, "decoded_pct": 87.5, "rejected": 0,
                                  "top_rejected": []})
        self.assertIn("strict decoded: 7 (87.50%)\n", out)

    def test_strict_block_counts_reserved_bit_word(self):
        data = reserved_bit_elf()
        out_path = self.tmp / "stats.json"
        code, out, _ = self.run_disasm("--stats", "--json", str(out_path), data=data)
        self.assertEqual(code, 0)
        doc = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(doc["decoded"], 7)
        self.assertEqual(doc["strict"]["decoded"], 6)
        self.assertEqual(doc["strict"]["rejected"], 1)
        self.assertEqual(doc["strict"]["top_rejected"], [{"mnemonic": "mtspr", "count": 1}])
        self.assertRegex(out, r"strict decoded: 6 ")
        self.assertRegex(out, r"  mtspr\s+1\n")


class ErrorTest(DisasmTestBase):
    def test_missing_file(self):
        self.elf_path = self.tmp / "missing.elf"
        code, _, err = self.run_disasm("--entry")
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_self_input(self):
        code, _, err = self.run_disasm("--entry", data=b"SCE\0" + bytes(64))
        self.assertEqual(code, 3)
        self.assertIn("encrypted SELF", err)


class CliTest(DisasmTestBase):
    def test_help_shows_three_modes(self):
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "disasm", "--help"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0)
        for flag in ("--start", "--entry", "--stats"):
            self.assertRegex(proc.stdout, re.escape(flag))

    def test_subprocess_run(self):
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "disasm",
                               str(self.elf_path), "--entry", "--count", "1"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), "0x00010000: 7c0802a6  mflr r0")


if __name__ == "__main__":
    unittest.main()

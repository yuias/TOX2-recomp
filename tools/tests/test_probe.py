import contextlib
import hashlib
import io
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ps3recomp import probe
from ps3recomp.elf import ElfFile

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSegment, build_elf32_be

TOOLS_DIR = Path(__file__).resolve().parents[1]
SPU_VA = 0x20200


class ProbeTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.out_path = self.tmp / "out" / "probe.json"

    def run_probe(self, data: bytes, *extra):
        elf_path = self.tmp / "input.elf"
        elf_path.write_bytes(data)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = probe.main([str(elf_path), "-o", str(self.out_path), *extra])
        return code, out.getvalue(), err.getvalue()

    def load_output(self) -> dict:
        return json.loads(self.out_path.read_text(encoding="utf-8"))


class SpuFixtureTest(ProbeTestBase):
    def setUp(self):
        super().setUp()
        self.data = fx.build_ps3_fixture(with_spu=True)
        self.code, self.stdout, _ = self.run_probe(self.data)
        self.doc = self.load_output()

    def test_one_spu_image(self):
        self.assertEqual(self.code, 0)
        self.assertEqual(self.doc["schema"], 1)
        self.assertEqual(len(self.doc["spu_images"]), 1)
        img = self.doc["spu_images"][0]
        offset = ElfFile.from_bytes(self.data).va_to_offset(SPU_VA)
        spu = build_elf32_be(entry=0x0, segments=[FixtureSegment(1, 0x0, bytes(32), flags=5)])
        self.assertEqual(img["file_offset"], "0x%08x" % offset)
        self.assertEqual(img["vaddr"], "0x%08x" % SPU_VA)
        self.assertEqual(img["size"], len(spu))
        self.assertEqual(img["sha256"], hashlib.sha256(spu).hexdigest())
        self.assertEqual((img["class"], img["machine"], img["e_type"]), (32, 23, 2))
        self.assertEqual(img["entry"], "0x00000000")
        self.assertEqual(img["segments"], [{"type": "0x00000001", "vaddr": "0x00000000",
                                            "filesz": 32, "memsz": 32, "flags": "R-X"}])

    def test_invalid_hit_and_no_other_elfs(self):
        self.assertGreaterEqual(self.doc["invalid_magic_hits"], 1)
        self.assertEqual(self.doc["other_elfs"], [])

    def test_imports(self):
        self.assertEqual(self.doc["summary"]["spu_related_imports"], ["cellSpurs"])
        self.assertEqual(self.doc["summary"]["spu_image_count"], 1)
        by_name = {i["name"]: i for i in self.doc["imports"]}
        self.assertFalse(by_name["cellSysutil"]["spu_related"])
        self.assertTrue(by_name["cellSpurs"]["spu_related"])
        self.assertEqual(by_name["cellSysutil"]["num_func"], 2)
        self.assertEqual(by_name["cellSpurs"]["num_func"], 1)

    def test_stdout(self):
        lines = self.stdout.splitlines()
        self.assertRegex(lines[0], r"^spu file_off=0x[0-9a-f]{8} vaddr=0x00020200 "
                                   r"size=0x[0-9a-f]{8} entry=0x00000000 sha256=[0-9a-f]{16}$")
        self.assertEqual(lines[1:], ["spu images: 1", "spu-related imports: cellSpurs",
                                     "syscalls: 1 sites, 1 distinct, 0 unresolved"])


class NoSpuFixtureTest(ProbeTestBase):
    def test_no_spu(self):
        code, stdout, _ = self.run_probe(fx.build_ps3_fixture())
        self.assertEqual(code, 0)
        doc = self.load_output()
        self.assertEqual(doc["spu_images"], [])
        self.assertEqual(doc["other_elfs"], [])
        self.assertEqual(doc["invalid_magic_hits"], 0)
        self.assertEqual(doc["summary"]["spu_image_count"], 0)
        self.assertIn("spu images: 0", stdout)

    def test_other_elf_is_not_spu(self):
        data = bytearray(fx.build_ps3_fixture())
        ppc = build_elf32_be(entry=0, segments=[], machine=20)
        data.extend(bytes(-len(data) % 16))
        data.extend(ppc)
        code, _, _ = self.run_probe(bytes(data))
        self.assertEqual(code, 0)
        doc = self.load_output()
        self.assertEqual(doc["spu_images"], [])
        self.assertEqual([e["machine"] for e in doc["other_elfs"]], [20])


class SyscallCensusTest(ProbeTestBase):
    TEXT_VA = 0x10000

    def patched_fixture(self, words: dict[int, int]) -> bytes:
        # `words` maps instruction index in the text segment to a replacement word.
        data = bytearray(fx.build_ps3_fixture())
        base = ElfFile.from_bytes(bytes(data)).va_to_offset(self.TEXT_VA)
        for index, word in words.items():
            struct.pack_into(">I", data, base + 4 * index, word)
        return bytes(data)

    def census(self, data: bytes) -> dict:
        code, _, _ = self.run_probe(data)
        self.assertEqual(code, 0)
        return self.load_output()["syscalls"]

    def test_fixture(self):
        census = self.census(fx.build_ps3_fixture())
        self.assertEqual({k: census[k] for k in ("sites", "unresolved", "by_number")},
                         {"sites": 1, "unresolved": 0, "by_number": {"141": 1}})

    def test_branch_between_li_and_sc_is_unresolved(self):
        # li r11,141 ; blr ; sc
        data = self.patched_fixture({3: 0x4E800020, 4: 0x44000002})
        census = self.census(data)
        self.assertEqual((census["sites"], census["unresolved"], census["by_number"]),
                         (1, 1, {}))

    @staticmethod
    def census_of(words: list[int]) -> dict:
        image = fx.build_elf64_be(
            entry=SyscallCensusTest.TEXT_VA,
            segments=[FixtureSegment(fx.PT_LOAD, SyscallCensusTest.TEXT_VA,
                                     struct.pack(">%dI" % len(words), *words), flags=5)])
        return probe.census_syscalls(ElfFile.from_bytes(image))

    def assert_resolved(self, between: list[int]):
        self.assertEqual(self.census_of([self.LI_141, *between, self.SC]),
                         {"sites": 1, "unresolved": 0, "by_number": {"141": 1}}, between)

    def assert_unresolved(self, between: list[int]):
        self.assertEqual(self.census_of([self.LI_141, *between, self.SC]),
                         {"sites": 1, "unresolved": 1, "by_number": {}}, between)

    LI_141 = 0x3960008D
    SC = 0x44000002
    NOP = 0x60000000

    def test_conditional_branches_are_scanned_through(self):
        # bne cr0,<fwd>
        self.assert_resolved([0x40820008])
        # beqlr
        self.assert_resolved([0x4D820020])

    def test_blocking_instructions_leave_the_site_unresolved(self):
        cases = {
            "b <fwd>": 0x48000008,
            "bl <fwd>": 0x48000009,
            "mr r11,r3": 0x7C6B1B78,
            "addi r11,r11,1": 0x396B0001,
            "lwzu r3,4(r11)": 0x866B0004,
            ".long 0": 0x00000000,
            "blr": 0x4E800020,
            "bdnzl <fwd>": 0x42000009,
        }
        for label, word in cases.items():
            with self.subTest(label):
                self.assert_unresolved([word])

    def test_unrelated_writes_do_not_block(self):
        # lwz r3,0(r4) ; mr r12,r3 ; cmpwi cr7,r3,0
        self.assert_resolved([0x80640000, 0x7C6C1B78, 0x2F830000])

    def test_lookback_limit(self):
        # 20 words back is within the window, 30 is not; the window is 24 words.
        self.assert_resolved([self.NOP] * 19)
        self.assert_resolved([self.NOP] * 23)
        self.assert_unresolved([self.NOP] * 24)
        self.assert_unresolved([self.NOP] * 29)

    def test_by_number_sorted_numerically(self):
        words = {0: 0x3960000A, 1: 0x44000002, 2: 0x39600064, 3: 0x44000002,
                 4: 0x3960000A, 5: 0x44000002}
        census = self.census(self.patched_fixture(words))
        self.assertEqual(census["sites"], 3)
        self.assertEqual(list(census["by_number"].items()), [("10", 2), ("100", 1)])


class SyscallNamesTest(ProbeTestBase):
    def write_table(self, syscalls: dict) -> str:
        path = self.tmp / "syscalls.json"
        path.write_text(json.dumps({"schema": 1, "syscalls": syscalls}), encoding="utf-8")
        return str(path)

    def test_names_filled_from_table(self):
        table = self.write_table({"141": "sys_timer_usleep", "999": "unused"})
        code, _, _ = self.run_probe(fx.build_ps3_fixture(), "--syscalls", table)
        self.assertEqual(code, 0)
        self.assertEqual(self.load_output()["syscalls"]["names"],
                         {"141": "sys_timer_usleep"})

    def test_unknown_number_has_no_name(self):
        table = self.write_table({"1": "sys_process_getpid"})
        code, _, _ = self.run_probe(fx.build_ps3_fixture(), "--syscalls", table)
        self.assertEqual(code, 0)
        self.assertEqual(self.load_output()["syscalls"]["names"], {})

    def test_explicit_missing_table_is_an_error(self):
        code, _, err = self.run_probe(fx.build_ps3_fixture(),
                                      "--syscalls", str(self.tmp / "missing.json"))
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)


class ErrorTest(ProbeTestBase):
    def test_missing_file(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = probe.main([str(self.tmp / "missing.elf"), "-o", str(self.out_path)])
        self.assertEqual(code, 1)
        self.assertEqual(len(err.getvalue().strip().splitlines()), 1)

    def test_self_input(self):
        code, _, err = self.run_probe(b"SCE\0" + bytes(64))
        self.assertEqual(code, 3)
        self.assertIn("encrypted SELF", err)

    def test_elf32_input(self):
        spu = build_elf32_be(entry=0, segments=[])
        code, _, _ = self.run_probe(spu)
        self.assertEqual(code, 3)


class CliTest(ProbeTestBase):
    def test_help_lists_output_option(self):
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "probe", "--help"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0)
        self.assertRegex(proc.stdout, re.compile(r"-o\b"))

    def test_subprocess_run(self):
        elf_path = self.tmp / "input.elf"
        elf_path.write_bytes(fx.build_ps3_fixture(with_spu=True))
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "probe",
                               str(elf_path), "-o", str(self.out_path)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("spu images: 1", proc.stdout)


if __name__ == "__main__":
    unittest.main()

import contextlib
import hashlib
import io
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import cell, ghidra
from ps3recomp.elf import ElfFile
from ps3recomp.info import CliError
from ps3recomp.ppu import decode

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSegment, build_elf64_be

TEXT_VA = 0x10000
DATA_VA = 0x20000
TOC = 0x28000

VMADDFP = 0x100220EE  # vmaddfp v0,v2,v3,v4


def _xform(vd: int, ra: int, rb: int, xo: int) -> int:
    return 31 << 26 | vd << 21 | ra << 16 | rb << 11 | xo << 1


LVX = _xform(1, 0, 4, 103)
STVX = _xform(1, 0, 4, 231)
LVLX = _xform(2, 0, 5, 519)
LVRX = _xform(2, 0, 5, 551)
VPERM = 4 << 26 | 1 << 21 | 2 << 16 | 3 << 11 | 4 << 6 | 43

VMX_TEXT = (0x7C0802A6, VMADDFP, VPERM, LVX, STVX, LVLX, 0x4E800020)


def build_vmx_elf(words=VMX_TEXT) -> bytes:
    text = b"".join(w.to_bytes(4, "big") for w in words)
    data = bytearray(0x20)
    data[0:8] = TEXT_VA.to_bytes(4, "big") + TOC.to_bytes(4, "big")
    return build_elf64_be(entry=DATA_VA, segments=[
        FixtureSegment(1, TEXT_VA, text, flags=5),
        FixtureSegment(1, DATA_VA, bytes(data), memsz=0x1000, flags=6),
    ])


def make_export(data: bytes, *, words=VMX_TEXT, mnemonics=None, kinds=None,
                cover_memsz=True, **overrides) -> dict:
    """Synthetic export whose mnemonics come from our decoder unless overridden."""
    elf = ElfFile.from_bytes(data)
    mnemonics = mnemonics or {}
    kinds = kinds or {}
    entries = []
    for i, word in enumerate(words):
        addr = TEXT_VA + 4 * i
        insn = decode(word)
        name = mnemonics.get(addr, insn.spec.name if insn.spec else None)
        entries.append({"addr": "0x%08x" % addr, "word": "0x%08x" % word,
                        "kind": kinds.get(addr, "code"), "mnemonic": name,
                        "operands": "" if name else None})
    blocks = [{"name": "b%d" % s.index, "start": "0x%08x" % s.p_vaddr,
               "end": "0x%08x" % (s.p_vaddr + (s.p_memsz if cover_memsz else s.p_filesz)),
               "initialized": True, "read": True, "write": False,
               "execute": bool(s.p_flags & 1)} for s in elf.loads()]
    export = {
        "schema": 1,
        "generator": "ExportAnalysis.java",
        "ghidra_version": "test",
        "language": ghidra.DEFAULT_LANGUAGE,
        "compiler_spec": "default",
        "executable_sha256": hashlib.sha256(data).hexdigest(),
        "memory_blocks": blocks,
        "text": {"start": "0x%08x" % TEXT_VA, "end": "0x%08x" % (TEXT_VA + 4 * len(words))},
        "functions": [{"entry": "0x00010000", "name": "FUN_00010000", "thunk": False,
                       "body": [["0x00010000", "0x0001001c"]]}],
        "words": entries,
        "jump_tables": [],
        "data_refs": [],
    }
    export.update(overrides)
    return export


class GhidraTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.elf_path = self.tmp / "input.elf"
        self.json_path = self.tmp / "export.json"

    def check_only(self, data: bytes, export: dict, *extra):
        self.elf_path.write_bytes(data)
        self.json_path.write_text(json.dumps(export), encoding="utf-8")
        return self.run_main(str(self.elf_path), "--check-only", "-o", str(self.json_path),
                             *extra)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ghidra.main(list(args))
        return code, out.getvalue(), err.getvalue()


class DirResolutionTest(unittest.TestCase):
    def test_arg_beats_env(self):
        env = {"GHIDRA_INSTALL_DIR": "/from/env"}
        self.assertEqual(ghidra.resolve_ghidra_dir("/from/arg", env), Path("/from/arg"))

    def test_env_used(self):
        env = {"GHIDRA_INSTALL_DIR": "/from/env"}
        self.assertEqual(ghidra.resolve_ghidra_dir(None, env), Path("/from/env"))

    def test_neither_is_exit_6(self):
        for env in ({}, {"GHIDRA_INSTALL_DIR": ""}):
            with self.assertRaises(CliError) as cm:
                ghidra.resolve_ghidra_dir(None, env)
            self.assertEqual(cm.exception.code, 6)

    def test_launcher_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            support = Path(tmp) / "support"
            support.mkdir()
            (support / "analyzeHeadless.bat").write_text("")
            (support / "analyzeHeadless").write_text("")
            self.assertEqual(ghidra.headless_launcher(Path(tmp), "nt").name,
                             "analyzeHeadless.bat")
            self.assertEqual(ghidra.headless_launcher(Path(tmp), "posix").name,
                             "analyzeHeadless")

    def test_missing_launcher_is_exit_6(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(CliError) as cm:
                ghidra.headless_launcher(Path(tmp), "posix")
            self.assertEqual(cm.exception.code, 6)


class CommandTest(unittest.TestCase):
    def _command(self, toc, seeds, seeds_file=None):
        return ghidra.build_headless_command(
            launcher=Path("/g/support/analyzeHeadless"), project_dir=Path("/p"),
            elf_path=Path("/e/EBOOT.elf"), out_path=Path("/o/out.json"),
            language="PowerPC:BE:64:A2ALT-32addr", cspec="default", script_dir=Path("/s"),
            log_dir=Path("/l"), toc=toc, seeds=seeds, text_range=(0x10230, 0xDCF2C),
            timeout=3600, seeds_file=seeds_file)

    def test_exact_command(self):
        self.assertEqual(self._command(TOC, [0x10000, 0x20000]), [
            str(Path("/g/support/analyzeHeadless")), str(Path("/p")), "eboot",
            "-import", str(Path("/e/EBOOT.elf")), "-overwrite", "-loader", "ElfLoader",
            "-processor", "PowerPC:BE:64:A2ALT-32addr", "-cspec", "default",
            "-scriptPath", str(Path("/s")),
            "-preScript", "Ps3Prepare.java", "0x00028000", "0x00010000", "0x00020000",
            "-postScript", "ExportAnalysis.java", str(Path("/o/out.json")),
            "0x00010230", "0x000dcf2c",
            "-analysisTimeoutPerFile", "3600",
            "-log", str(Path("/l/application.log")),
            "-scriptlog", str(Path("/l/script.log")),
        ])

    def test_exact_command_with_seeds_file(self):
        seeds_file = Path("/s/seeds.txt")
        base = self._command(TOC, [0x10000, 0x20000])
        cmd = self._command(TOC, [0x10000, 0x20000], seeds_file)
        i = base.index("0x00020000") + 1
        self.assertEqual(cmd, base[:i] + ["@%s" % seeds_file] + base[i:])
        self.assertEqual(cmd[i + 1], "-postScript")

    def test_toc_none(self):
        cmd = self._command(None, [0x10000])
        i = cmd.index("Ps3Prepare.java")
        self.assertEqual(cmd[i + 1:i + 3], ["none", "0x00010000"])

    def test_seeds_from_fixture_are_unique(self):
        elf = ElfFile.from_bytes(fx.build_ps3_fixture())
        # The entry function and the export both resolve to 0x10000.
        self.assertEqual(ghidra.collect_seeds(elf), (TOC, [0x10000]))


class ExitOrderTest(GhidraTestBase):
    def test_no_ghidra_dir_is_exit_6_before_reading_elf(self):
        env = {k: v for k, v in os.environ.items() if k != "GHIDRA_INSTALL_DIR"}
        with mock.patch.dict(os.environ, env, clear=True):
            code, _, err = self.run_main(str(self.tmp / "missing.elf"))
        self.assertEqual(code, 6)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_missing_launcher_is_exit_6(self):
        code, _, err = self.run_main(str(self.tmp / "missing.elf"),
                                     "--ghidra-dir", str(self.tmp))
        self.assertEqual(code, 6)
        self.assertTrue(err.startswith("error: "))

    def test_missing_seeds_file_is_exit_1(self):
        code, _, err = self.run_main(str(self.elf_path), "--seeds-file",
                                     str(self.tmp / "missing.txt"))
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "))

    def test_usage_errors(self):
        self.assertEqual(self.run_main(str(self.elf_path), "--start", "0x10000")[0], 2)
        self.assertEqual(self.run_main(str(self.elf_path), "--start", "0x10002",
                                       "--end", "0x10010")[0], 2)

    def test_unreadable_elf_is_exit_1(self):
        code, _, _ = self.run_main(str(self.tmp / "missing.elf"), "--check-only")
        self.assertEqual(code, 1)

    def test_not_an_elf_is_exit_3(self):
        self.elf_path.write_bytes(b"not an elf")
        self.assertEqual(self.run_main(str(self.elf_path), "--check-only")[0], 3)

    def test_range_outside_exec_segment_is_exit_5(self):
        data = build_vmx_elf()
        code, _, _ = self.check_only(data, make_export(data), "--start", "0x20000",
                                     "--end", "0x20010")
        self.assertEqual(code, 5)


class CheckOnlyTest(GhidraTestBase):
    def setUp(self):
        super().setUp()
        self.data = build_vmx_elf()

    def test_fixture_decoder_agrees_on_probes(self):
        names = [decode(w).spec.name for w in VMX_TEXT]
        self.assertEqual(names, ["mfspr", "vmaddfp", "vperm", "lvx", "stvx", "lvlx", "bclr"])

    def test_valid_export(self):
        code, out, err = self.check_only(self.data, make_export(self.data))
        self.assertEqual((code, err), (0, ""))
        self.assertIn("language check: vmaddfp at 0x00010004 -> ghidra 'vmaddfp': ok", out)
        self.assertIn("language check: lvlx at 0x00010014 -> ghidra 'lvlx': info", out)
        self.assertIn("language check: lvrx not present in the text range: absent", out)
        self.assertIn("functions: 1\n", out)
        self.assertIn("words: code 7, data 0, undefined 0\n", out)
        self.assertIn("jump tables: 0\n", out)
        self.assertIn("data refs: 0\n", out)
        self.assertNotIn("elapsed", out)

    def test_wrong_sha256_is_exit_4(self):
        export = make_export(self.data, executable_sha256="0" * 64)
        self.assertEqual(self.check_only(self.data, export)[0], 4)

    def test_wrong_schema_is_exit_1(self):
        self.assertEqual(self.check_only(self.data, make_export(self.data, schema=2))[0], 1)

    def test_missing_key_is_exit_1(self):
        export = make_export(self.data)
        del export["words"]
        self.assertEqual(self.check_only(self.data, export)[0], 1)

    def test_invalid_json_is_exit_1(self):
        self.elf_path.write_bytes(self.data)
        self.json_path.write_text("{", encoding="utf-8")
        code, _, err = self.run_main(str(self.elf_path), "--check-only", "-o",
                                     str(self.json_path))
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_missing_export_is_exit_1(self):
        self.elf_path.write_bytes(self.data)
        code, _, _ = self.run_main(str(self.elf_path), "--check-only", "-o",
                                   str(self.tmp / "none.json"))
        self.assertEqual(code, 1)

    def test_memory_blocks_not_covering_file_range_is_exit_6(self):
        export = make_export(self.data)
        export["memory_blocks"][0]["end"] = "0x00010010"
        code, _, err = self.check_only(self.data, export)
        self.assertEqual(code, 6)
        self.assertIn("memory map", err)

    def test_missing_segment_block_is_exit_6(self):
        export = make_export(self.data)
        del export["memory_blocks"][1]
        self.assertEqual(self.check_only(self.data, export)[0], 6)

    def test_adjacent_blocks_are_merged(self):
        export = make_export(self.data)
        export["memory_blocks"][0] = {"start": "0x00010000", "end": "0x00010008"}
        export["memory_blocks"].insert(1, {"start": "0x00010008", "end": "0x00010020"})
        self.assertEqual(self.check_only(self.data, export)[0], 0)

    def _export_with_text_gap(self, words, gap):
        """Export whose text block is split in two, leaving [gap[0], gap[1]) unmapped."""
        data = build_vmx_elf(words)
        export = make_export(data, words=words)
        text_end = TEXT_VA + 4 * len(words)
        export["memory_blocks"][0] = {"start": "0x%08x" % TEXT_VA, "end": "0x%08x" % gap[0]}
        export["memory_blocks"].insert(1, {"start": "0x%08x" % gap[1],
                                           "end": "0x%08x" % text_end})
        return data, export

    def test_zero_filled_hole_between_blocks_is_tolerated(self):
        # Section padding that Ghidra leaves unmapped is all zero in the file.
        words = (0x7C0802A6, VMADDFP, 0, VPERM, LVX, STVX, LVLX, 0x4E800020)
        data, export = self._export_with_text_gap(words, (0x10008, 0x1000C))
        self.assertEqual(self.check_only(data, export)[0], 0)

    def test_nonzero_hole_between_blocks_is_exit_6(self):
        words = (0x7C0802A6, VMADDFP, 0x60000000, VPERM, LVX, STVX, LVLX, 0x4E800020)
        data, export = self._export_with_text_gap(words, (0x10008, 0x1000C))
        code, _, err = self.check_only(data, export)
        self.assertEqual(code, 6)
        self.assertIn("0x00010008-0x0001000c is not covered", err)

    def test_hole_with_one_nonzero_byte_is_exit_6(self):
        words = (0x7C0802A6, VMADDFP, 0x00000100, VPERM, LVX, STVX, LVLX, 0x4E800020)
        data, export = self._export_with_text_gap(words, (0x10008, 0x1000C))
        self.assertEqual(self.check_only(data, export)[0], 6)

    def test_zero_filled_hole_at_segment_end_is_tolerated(self):
        words = VMX_TEXT + (0,)
        data = build_vmx_elf(words)
        export = make_export(data, words=words)
        export["memory_blocks"][0]["end"] = "0x%08x" % (TEXT_VA + 4 * len(VMX_TEXT))
        self.assertEqual(self.check_only(data, export)[0], 0)

    def test_covering_filesz_but_not_memsz_is_ok(self):
        code, out, _ = self.check_only(self.data, make_export(self.data, cover_memsz=False))
        self.assertEqual(code, 0)
        self.assertIn("zero-fill 0x00020020-0x00021000: not covered", out)

    def test_vmx_mismatch_is_exit_7(self):
        export = make_export(self.data, mnemonics={0x10004: "vaddfp"})
        code, out, err = self.check_only(self.data, export)
        self.assertEqual(code, 7)
        self.assertIn("-> ghidra 'vaddfp': FAIL", out)
        self.assertIn("vmaddfp", err)

    def test_undecoded_vmx_word_is_exit_7(self):
        export = make_export(self.data, mnemonics={0x10004: None})
        self.assertEqual(self.check_only(self.data, export)[0], 7)

    def test_lvlx_mismatch_alone_is_ok(self):
        export = make_export(self.data, mnemonics={0x10014: None})
        code, out, _ = self.check_only(self.data, export)
        self.assertEqual(code, 0)
        self.assertIn("language check: lvlx at 0x00010014 -> ghidra None: info", out)

    def test_code_word_preferred_over_data_word(self):
        words = (VMADDFP, 0x100220EE, 0x4E800020)
        data = build_vmx_elf(words)
        # The first vmaddfp word is data that Ghidra cannot decode; the second is code.
        export = make_export(data, words=words, kinds={0x10000: "data"},
                             mnemonics={0x10000: None})
        code, out, _ = self.check_only(data, export)
        self.assertEqual(code, 0)
        self.assertIn("language check: vmaddfp at 0x00010004 -> ghidra 'vmaddfp': ok", out)

    def test_vperm_is_required_when_vmaddfp_is_absent(self):
        words = (0x7C0802A6, VPERM, 0x4E800020)
        data = build_vmx_elf(words)
        self.assertEqual(self.check_only(data, make_export(data, words=words))[0], 0)
        export = make_export(data, words=words, mnemonics={0x10004: "vsel"})
        self.assertEqual(self.check_only(data, export)[0], 7)

    def test_no_vmx_word_is_exit_7(self):
        words = (0x7C0802A6, LVX, STVX, LVLX, 0x4E800020)
        data = build_vmx_elf(words)
        code, _, err = self.check_only(data, make_export(data, words=words))
        self.assertEqual(code, 7)
        self.assertIn("no VMX instruction", err)


class RunTest(GhidraTestBase):
    """Wrapper flow with a stubbed `subprocess.run`."""

    def setUp(self):
        super().setUp()
        self.data = build_vmx_elf()
        self.elf_path.write_bytes(self.data)
        self.ghidra_dir = self.tmp / "ghidra"
        support = self.ghidra_dir / "support"
        support.mkdir(parents=True)
        name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
        (support / name).write_text("")
        self.out_path = self.tmp / "out" / "export.json"
        self.logs = self.tmp / "logs"
        patcher = mock.patch.object(ghidra, "DEFAULT_LOG_DIR", self.logs)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_wrapper(self, run):
        with mock.patch.object(ghidra.subprocess, "run", side_effect=run) as m:
            result = self.run_main(str(self.elf_path), "--ghidra-dir", str(self.ghidra_dir),
                                   "-o", str(self.out_path),
                                   "--project-dir", str(self.tmp / "proj"))
        return result, m

    def test_success(self):
        def fake_run(cmd, **kwargs):
            self.assertFalse(self.out_path.exists())
            self.out_path.write_text(json.dumps(make_export(self.data)), encoding="utf-8")
            return subprocess.CompletedProcess(cmd, 0)

        self.out_path.parent.mkdir()
        self.out_path.write_text("stale", encoding="utf-8")
        (code, out, err), run = self.run_wrapper(fake_run)
        self.assertEqual((code, err), (0, ""))
        self.assertIn("elapsed: ", out)
        cmd = run.call_args.args[0]
        self.assertEqual(run.call_args.kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(run.call_args.kwargs["timeout"], 3600 + 300)
        self.assertEqual(cmd[cmd.index("-preScript") + 2:cmd.index("-preScript") + 4],
                         ["0x00028000", "0x00010000"])
        self.assertEqual(cmd[cmd.index("-postScript") + 3:cmd.index("-postScript") + 5],
                         ["0x00010000", "0x0001001c"])

    def test_nonzero_exit_is_exit_6(self):
        (code, _, err), _ = self.run_wrapper(
            lambda cmd, **kw: subprocess.CompletedProcess(cmd, 1))
        self.assertEqual(code, 6)
        self.assertIn(str(self.logs), err)

    def test_missing_output_is_exit_6(self):
        (code, _, err), _ = self.run_wrapper(
            lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0))
        self.assertEqual(code, 6)
        self.assertIn(str(self.logs), err)

    def test_timeout_is_exit_6(self):
        def fake_run(cmd, **kwargs):
            raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])

        (code, _, err), _ = self.run_wrapper(fake_run)
        self.assertEqual(code, 6)
        self.assertEqual(len(err.strip().splitlines()), 1)


@unittest.skipUnless(os.environ.get("TOX2_EBOOT_ELF"), "TOX2_EBOOT_ELF is not set")
class RealEbootSeedsTest(unittest.TestCase):
    def test_seeds(self):
        path = os.environ["TOX2_EBOOT_ELF"]
        if not os.path.isfile(path):
            self.skipTest("TOX2_EBOOT_ELF does not point to a file")
        elf = ElfFile.from_path(path)
        toc, seeds = ghidra.collect_seeds(elf)
        self.assertEqual(toc, 0x010B6330)
        self.assertEqual(seeds, sorted(set(seeds)))
        self.assertIn(0x00010240, seeds)
        self.assertEqual(cell.find_text_range(elf), (0x00010240, 0x00C8F3D4))


if __name__ == "__main__":
    unittest.main()

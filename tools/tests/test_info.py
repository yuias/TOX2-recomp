import contextlib
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import info, nids

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSegment, build_elf32_be

TOOLS_DIR = Path(__file__).resolve().parents[1]
HEX_RE = re.compile(r"^0x[0-9a-f]{8}$")


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class InfoTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.data = fx.build_ps3_fixture()
        self.elf_path = self.tmp / "input.elf"
        self.elf_path.write_bytes(self.data)
        self.out_path = self.tmp / "out" / "info.json"

    def write_config(self, elf_sha, bin_sha="0" * 64) -> Path:
        path = self.tmp / "target.json"
        path.write_text(json.dumps({
            "title_id": "TEST00000",
            "eboot_bin": {"size": 1, "sha256": bin_sha},
            "eboot_elf": {"size": len(self.data), "sha256": elf_sha},
        }), encoding="utf-8")
        return path

    def run_info(self, *extra, elf_path=None):
        argv = [str(elf_path or self.elf_path), "-o", str(self.out_path), *extra]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = info.main(argv)
        return code, out.getvalue(), err.getvalue()

    def load_output(self) -> dict:
        return json.loads(self.out_path.read_text(encoding="utf-8"))


class SummaryTest(InfoTestBase):
    def test_schema_and_values(self):
        code, stdout, _ = self.run_info("--no-verify")
        self.assertEqual(code, 0)
        self.assertIn("sha256:", stdout)
        doc = self.load_output()
        self.assertEqual(list(doc), ["schema", "input", "elf", "entry", "segments",
                                     "sections", "tls", "process_param", "prx_param",
                                     "opd", "imports", "exports"])
        self.assertEqual(doc["schema"], 1)
        self.assertEqual(doc["input"]["size"], len(self.data))
        self.assertEqual(doc["input"]["sha256"], _sha(self.data))
        self.assertEqual(doc["input"]["pin"], "skipped")
        self.assertEqual(doc["elf"], {"class": 64, "osabi": "0x00000066", "type": 2,
                                      "machine": 21, "entry": "0x00020000",
                                      "flags": "0x00000000"})
        self.assertEqual(doc["entry"], {"kind": "opd", "opd_addr": "0x00020000",
                                        "func": "0x00010000", "toc": "0x00028000"})

    def test_segments_and_sections(self):
        self.run_info("--no-verify")
        doc = self.load_output()
        self.assertEqual(len(doc["segments"]), 5)
        text = doc["segments"][0]
        self.assertEqual((text["type_name"], text["flags"], text["vaddr"]),
                         ("LOAD", "R-X", "0x00010000"))
        self.assertEqual(doc["segments"][1]["flags"], "RW-")
        self.assertEqual(doc["segments"][1]["memsz"], 0x10000)
        self.assertEqual([s["type_name"] for s in doc["segments"][2:]],
                         ["SCE_PROC_PARAM", "SCE_PRX_PARAM", "TLS"])
        names = [s["name"] for s in doc["sections"]]
        self.assertEqual(names, ["", ".text", ".opd", ".missing", ".shstrtab"])
        missing = doc["sections"][3]
        self.assertFalse(missing["data_present"])
        self.assertTrue(doc["sections"][1]["data_present"])

    def test_ps3_structures(self):
        self.run_info("--no-verify")
        doc = self.load_output()
        self.assertEqual(doc["tls"]["vaddr"], "0x00020180")
        self.assertEqual(doc["tls"]["memsz"], 0x10)
        pp = doc["process_param"]
        self.assertEqual((pp["addr"], pp["magic"], pp["primary_prio"]),
                         ("0x00020300", "0x13bcc5f6", 1000))
        self.assertEqual(len(pp["raw"]), 0x20 * 2)
        prx = doc["prx_param"]
        self.assertEqual((prx["magic"], prx["libstub_start"], prx["libstub_end"]),
                         ("0x1b434cec", "0x00020030", "0x00020088"))
        self.assertEqual(doc["opd"], {"count": 2, "toc_values": ["0x00028000"]})
        self.assertEqual([m["name"] for m in doc["imports"]], ["cellSysutil", "cellSpurs"])
        slots = [f["slot_addr"] for m in doc["imports"] for f in m["functions"]]
        self.assertEqual(slots, ["0x00020150", "0x00020154", "0x00020158"])
        self.assertEqual(len(doc["exports"]), 1)
        export = doc["exports"][0]
        self.assertIsNone(export["name"])
        self.assertEqual(export["functions"], [{"fnid": "0xbc9a0086",
                                                "opd_addr": "0x00020000",
                                                "func": "0x00010000",
                                                "toc": "0x00028000"}])

    def test_hex_formatting(self):
        self.run_info("--no-verify")
        doc = self.load_output()

        def check(node, key=None):
            if isinstance(node, dict):
                for k, v in node.items():
                    check(v, k)
            elif isinstance(node, list):
                for v in node:
                    check(v, key)
            elif isinstance(node, str) and node.startswith("0x"):
                self.assertRegex(node, HEX_RE, key)
            elif key in ("filesz", "memsz", "size", "count", "num_tlsvar"):
                self.assertIsInstance(node, int)
                self.assertNotIsInstance(node, bool)

        check(doc)

    def test_output_format(self):
        self.run_info("--no-verify")
        text = self.out_path.read_bytes().decode("utf-8")
        self.assertTrue(text.endswith("}\n"))
        self.assertIn('\n  "schema": 1,', text)
        self.assertNotIn("\r", text)

    def test_no_opd_section_is_null(self):
        data = fx.build_elf64_be(entry=0x10000, segments=[
            FixtureSegment(1, 0x10000, bytes(0x20), flags=5)])
        path = self.tmp / "plain.elf"
        path.write_bytes(data)
        code, _, _ = self.run_info("--no-verify", elf_path=path)
        self.assertEqual(code, 0)
        doc = self.load_output()
        for key in ("tls", "process_param", "prx_param", "opd"):
            self.assertIsNone(doc[key], key)
        self.assertEqual((doc["imports"], doc["exports"]), ([], []))
        self.assertEqual(doc["entry"]["kind"], "code")
        self.assertIsNone(doc["entry"]["toc"])

    def test_subprocess(self):
        proc = subprocess.run(
            [sys.executable, "-B", str(TOOLS_DIR / "ps3tool.py"), "info",
             str(self.elf_path), "-o", str(self.out_path), "--no-verify"],
            capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.load_output()["schema"], 1)

    def test_unknown_subcommand_is_usage_error(self):
        proc = subprocess.run([sys.executable, "-B", str(TOOLS_DIR / "ps3tool.py"), "bogus"],
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 2)


class PinTest(InfoTestBase):
    def test_unpinned(self):
        config = self.write_config(None)
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 0)
        self.assertEqual(self.load_output()["input"]["pin"], "unpinned")
        self.assertIn(_sha(self.data), err)
        self.assertIn(str(len(self.data)), err)

    def test_verified(self):
        config = self.write_config(_sha(self.data))
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.load_output()["input"]["pin"], "verified")

    def test_mismatch_writes_json_then_exits_4(self):
        config = self.write_config("f" * 64)
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 4)
        self.assertEqual(self.load_output()["input"]["pin"], "mismatch")
        self.assertIn("error:", err)

    def test_skipped_ignores_config(self):
        code, _, _ = self.run_info("--no-verify",
                                   "--target-config", str(self.tmp / "missing.json"))
        self.assertEqual(code, 0)
        self.assertEqual(self.load_output()["input"]["pin"], "skipped")

    def test_missing_config_is_io_error(self):
        code, _, err = self.run_info("--target-config", str(self.tmp / "missing.json"))
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_default_config_is_repo_target(self):
        config = json.loads(info.DEFAULT_CONFIG.read_text(encoding="utf-8"))
        self.assertEqual(config["title_id"], "BLJS10188")
        for key in ("eboot_bin", "eboot_elf"):
            self.assertRegex(config[key]["sha256"], r"^[0-9a-f]{64}$")
        self.assertRegex(config["libsre_prx"]["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(config["libsre_prx"]["size"], 239344)
        self.assertEqual(config["libsre_prx"]["firmware"], "4.93")
        self.assertEqual(config["libsre_reloc_elf"]["base"], "0x011d0000")
        self.assertEqual(config["libsre_reloc_elf"]["size"], 239344)
        self.assertRegex(config["libsre_reloc_elf"]["sha256"], r"^[0-9a-f]{64}$")
        # The config must stay path-free.
        self.assertNotRegex(info.DEFAULT_CONFIG.read_text(encoding="utf-8"), r"[A-Za-z]:[\\/]")


class PrxTest(InfoTestBase):
    BASE = 0x20000

    def use_prx(self, relocated: bool) -> None:
        self.data = fx.build_prx_fixture(relocated, self.BASE)
        self.elf_path.write_bytes(self.data)

    def write_prx_config(self, key: str, sha: str) -> Path:
        path = self.tmp / "target.json"
        path.write_text(json.dumps({"eboot_bin": {"sha256": "0" * 64},
                                    "eboot_elf": {"sha256": "1" * 64},
                                    "libsre_prx": {"sha256": "2" * 64},
                                    "libsre_reloc_elf": {"sha256": "3" * 64},
                                    key: {"sha256": sha}}), encoding="utf-8")
        return path

    def test_summary_has_module_info_and_library_tables(self):
        self.use_prx(True)
        code, stdout, _ = self.run_info("--no-verify")
        self.assertEqual(code, 0)
        self.assertIn("module:   fixSre_Library 1.1 toc 0x%08x"
                      % (self.BASE + fx.PRX_TOC), stdout)
        self.assertIn("imports:  1 modules, 2 functions, 0 named", stdout)
        self.assertIn("exports:  1 functions", stdout)
        doc = self.load_output()
        self.assertEqual(doc["entry"]["kind"], "prx")
        self.assertIsNone(doc["entry"]["func"])
        self.assertEqual(doc["module_info"], {
            "addr": "0x%08x" % (self.BASE + fx.PRX_MODULE_INFO),
            "name": fx.PRX_NAME, "version": "1.1", "attributes": "0x00000000",
            "toc": "0x%08x" % (self.BASE + fx.PRX_TOC),
            "exports": {"start": "0x%08x" % (self.BASE + fx.PRX_LIBENT[0]),
                        "end": "0x%08x" % (self.BASE + fx.PRX_LIBENT[1])},
            "imports": {"start": "0x%08x" % (self.BASE + fx.PRX_LIBSTUB[0]),
                        "end": "0x%08x" % (self.BASE + fx.PRX_LIBSTUB[1])},
            "text_range": {"start": "0x%08x" % self.BASE,
                           "end": "0x%08x" % (self.BASE + fx.PRX_THUNKS_END)},
            "code_range": {"start": "0x%08x" % self.BASE,
                           "end": "0x%08x" % (self.BASE + fx.PRX_CODE_END)}})

    def test_executable_summary_has_no_module_info(self):
        self.run_info("--no-verify")
        self.assertNotIn("module_info", self.load_output())

    def test_relocated_prx_is_checked_against_libsre_reloc_elf(self):
        self.use_prx(True)
        config = self.write_prx_config("libsre_reloc_elf", _sha(self.data))
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.load_output()["input"]["pin"], "verified")

    def test_unrelocated_prx_is_checked_against_libsre_prx(self):
        self.use_prx(False)
        config = self.write_prx_config("libsre_prx", _sha(self.data))
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.load_output()["input"]["pin"], "verified")

    def test_mismatch_names_the_pin(self):
        self.use_prx(True)
        config = self.write_prx_config("libsre_prx", _sha(self.data))
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 4)
        self.assertIn("libsre_reloc_elf", err)


class ImportNamesTest(InfoTestBase):
    def write_nids(self, modules) -> Path:
        path = self.tmp / "nids.json"
        path.write_text(json.dumps({"schema": 1, "modules": modules}), encoding="utf-8")
        return path

    def test_named_and_unresolved(self):
        nids = self.write_nids({"cellSpurs": {"0x%08x" % fx.PS3_FNIDS[2]: "cellSpursNamed"}})
        code, stdout, err = self.run_info("--no-verify", "--nids", str(nids))
        self.assertEqual(code, 0, err)
        names = [f["name"] for m in self.load_output()["imports"] for f in m["functions"]]
        self.assertEqual(names, [None, None, "cellSpursNamed"])
        lines = stdout.splitlines()
        self.assertIn("imports:  2 modules, 3 functions, 1 named", lines)
        self.assertEqual([ln for ln in lines if ln.startswith("unresolved import:")],
                         ["unresolved import: cellSysutil 0x%08x" % fx.PS3_FNIDS[0],
                          "unresolved import: cellSysutil 0x%08x" % fx.PS3_FNIDS[1]])

    def test_global_fallback_for_other_module(self):
        nids = self.write_nids({"cellOther": {"0x%08x" % fx.PS3_FNIDS[0]: "cellSharedName"}})
        self.run_info("--no-verify", "--nids", str(nids))
        sysutil = self.load_output()["imports"][0]["functions"]
        self.assertEqual([f["name"] for f in sysutil], ["cellSharedName", None])

    def test_default_missing_is_a_note(self):
        with mock.patch.object(info, "DEFAULT_NIDS", self.tmp / "missing_nids.json"):
            code, stdout, err = self.run_info("--no-verify")
        self.assertEqual(code, 0)
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertIn("note:", err)
        names = [f["name"] for m in self.load_output()["imports"] for f in m["functions"]]
        self.assertEqual(names, [None, None, None])
        self.assertIn("0 named", stdout)

    def test_default_table_is_used(self):
        nids = self.write_nids({"cellSpurs": {"0x%08x" % fx.PS3_FNIDS[2]: "cellSpursNamed"}})
        with mock.patch.object(info, "DEFAULT_NIDS", nids):
            code, _, err = self.run_info("--no-verify")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.load_output()["imports"][1]["functions"][0]["name"],
                         "cellSpursNamed")

    def test_explicit_missing_exits_1(self):
        code, _, err = self.run_info("--no-verify", "--nids", str(self.tmp / "missing.json"))
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertFalse(self.out_path.exists())

    def test_invalid_table_exits_1(self):
        bad = self.tmp / "bad.json"
        bad.write_text("{", encoding="utf-8")
        code, _, err = self.run_info("--no-verify", "--nids", str(bad))
        self.assertEqual(code, 1)
        self.assertIn("error:", err)

    def test_committed_table_loads(self):
        table = nids.load_nid_table(nids.DEFAULT_NIDS)
        self.assertEqual(table.lookup("cellSpurs", nids.compute_nid("cellSpursFinalize")),
                         "cellSpursFinalize")


class RejectionTest(InfoTestBase):
    def test_self_magic_exits_3(self):
        path = self.tmp / "eboot.bin"
        path.write_bytes(b"SCE\0" + bytes(64))
        code, _, err = self.run_info("--no-verify", elf_path=path)
        self.assertEqual(code, 3)
        self.assertIn("encrypted SELF; decrypt it with RPCS3", err)
        self.assertFalse(self.out_path.exists())

    def test_known_eboot_bin_hash_exits_3(self):
        config = self.write_config(None, bin_sha=_sha(self.data))
        code, _, err = self.run_info("--target-config", str(config))
        self.assertEqual(code, 3)
        self.assertIn("encrypted SELF", err)

    def test_elf32_exits_3(self):
        path = self.tmp / "spu.elf"
        path.write_bytes(build_elf32_be(entry=0, segments=[
            FixtureSegment(1, 0, bytes(16), flags=5)]))
        code, _, _ = self.run_info("--no-verify", elf_path=path)
        self.assertEqual(code, 3)

    def test_wrong_machine_exits_3(self):
        path = self.tmp / "other.elf"
        path.write_bytes(fx.build_elf64_be(entry=0x10000, machine=62, segments=[
            FixtureSegment(1, 0x10000, bytes(16), flags=5)]))
        code, _, _ = self.run_info("--no-verify", elf_path=path)
        self.assertEqual(code, 3)

    def test_bad_magic_exits_3(self):
        path = self.tmp / "junk.bin"
        path.write_bytes(b"MZ" + bytes(100))
        code, _, _ = self.run_info("--no-verify", elf_path=path)
        self.assertEqual(code, 3)

    def test_missing_file_exits_1(self):
        code, _, err = self.run_info("--no-verify", elf_path=self.tmp / "nonexistent.elf")
        self.assertEqual(code, 1)
        self.assertEqual(len(err.strip().splitlines()), 1)
        self.assertNotIn("Traceback", err)

    def test_missing_file_subprocess(self):
        proc = subprocess.run(
            [sys.executable, "-B", str(TOOLS_DIR / "ps3tool.py"), "info",
             str(self.tmp / "nonexistent.elf")],
            capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(len(proc.stderr.strip().splitlines()), 1)

    def test_usage_error_exits_2(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(info.main([]), 2)


if __name__ == "__main__":
    unittest.main()

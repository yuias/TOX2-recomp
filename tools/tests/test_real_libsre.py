import contextlib
import hashlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

from ps3recomp import ghidra_summary, recover

_ELF_PATH = os.environ.get("TOX2_LIBSRE_ELF")
_COMMITTED_DB = ghidra_summary.REPO_ROOT / "config" / "libsre_functions.json"

# Functions that a function descriptor reaches but no `bl` does; they exist only because the
# descriptor scan is 4-aligned for a PRX.
_DESCRIPTOR_ONLY = [0x011DB504, 0x011DEA78, 0x011DEB58, 0x011E0060, 0x011E37F8, 0x011E4DF8,
                    0x011ED490]

_SKIP_REASON = "TOX2_LIBSRE_ELF is not set or does not name an existing file"
_RECOVER_CACHE = []


def _fresh_recover():
    """One `recover` run per process, written into a temporary directory."""
    if not _RECOVER_CACHE:
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "libsre_functions.json"
            report_path = Path(tmp) / "report.json"
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                code = recover.main([_ELF_PATH, "--no-ghidra", "-o", str(out_path),
                                     "--report", str(report_path),
                                     "--seeds-out", str(Path(tmp) / "seeds.txt")])
            data = out_path.read_bytes() if code == 0 else None
            report = (json.loads(report_path.read_text(encoding="utf-8"))
                      if report_path.is_file() else None)
        _RECOVER_CACHE.append((code, data, report))
    return _RECOVER_CACHE[0]


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(), _SKIP_REASON)
class RealLibsreDatabaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = json.loads(_COMMITTED_DB.read_text(encoding="utf-8"))
        cls.entries = {int(f["entry"], 16): f for f in cls.db["functions"]}

    def test_hash_matches_the_file(self):
        digest = hashlib.sha256(Path(_ELF_PATH).read_bytes()).hexdigest()
        self.assertEqual(self.db["input"]["executable_sha256"], digest)
        self.assertIsNone(self.db["input"]["ghidra_summary_sha256"])
        self.assertEqual(self.db["input"]["extra_seeds"], 0)

    def test_ranges_and_toc(self):
        self.assertEqual(self.db["text"], {"start": "0x011d0000", "end": "0x011edb18"})
        self.assertEqual(self.db["code"], {"start": "0x011d0000", "end": "0x011ed718"})
        self.assertEqual(self.db["toc"], "0x01209ab0")

    def test_every_descriptor_function_is_a_function(self):
        descriptors = [f for f in self.db["functions"] if "opd" in f["sources"]]
        self.assertEqual(len(descriptors), 352)
        self.assertEqual(len(self.db["functions"]), 352)

    def test_descriptor_only_functions(self):
        for addr in _DESCRIPTOR_ONLY:
            with self.subTest(addr=hex(addr)):
                self.assertIn(addr, self.entries)
                self.assertNotIn("bl", self.entries[addr]["sources"])
                self.assertIn("opd", self.entries[addr]["sources"])

    def test_thunks_have_no_slot_mismatch(self):
        thunks = self.db["import_thunks"]
        self.assertEqual(len(thunks), 32)
        # The thunks follow the code in import order, 32 bytes each, and load slots 4 bytes apart.
        for i, thunk in enumerate(thunks):
            self.assertEqual(int(thunk["addr"], 16), 0x011ED718 + 32 * i)
            self.assertEqual(int(thunk["slot"], 16), 0x011FE000 + 4 * i)
        _, _, report = _fresh_recover()
        self.assertEqual(report["thunks"]["count"], 32)
        self.assertEqual(report["thunks"]["slot_mismatches"], [])
        self.assertEqual(report["thunks"]["never_called"]["count"], 0)


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(), _SKIP_REASON)
class RealLibsreRecoverTest(unittest.TestCase):
    def test_fresh_recover_is_byte_identical_to_the_committed_database(self):
        code, data, report = _fresh_recover()
        self.assertEqual(code, 0)
        self.assertTrue(report["partition"]["converged"])
        self.assertEqual(data, _COMMITTED_DB.read_bytes())


if __name__ == "__main__":
    unittest.main()

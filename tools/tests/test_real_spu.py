"""Checks `spu-recover` and `spu-emit` on the real SPU images.

Needs TOX2_EBOOT_ELF and TOX2_LIBSRE_ELF (the relocated libsre); skipped without them. Everything is
written to a temporary directory, so the committed databases and the generated tree stay untouched.
"""

import contextlib
import io
import json
import os
import re
import tempfile
import unittest
from pathlib import Path

from ps3recomp import spu_emit, spu_recover
from ps3recomp.info import REPO_ROOT

_EBOOT = os.environ.get("TOX2_EBOOT_ELF")
_LIBSRE = os.environ.get("TOX2_LIBSRE_ELF")
_SKIP_REASON = "TOX2_EBOOT_ELF and TOX2_LIBSRE_ELF must name existing files"
_AVAILABLE = bool(_EBOOT and _LIBSRE and Path(_EBOOT).is_file() and Path(_LIBSRE).is_file())

_DB_DIR = REPO_ROOT / "config" / "spu"
_GATE_IMAGES = ["eboot_edgelzma", "libsre_kernel1", "libsre_system_service", "libsre_taskset_pm"]

# image: (functions, instructions, dispatchable blocks, unhandled words)
_COUNTS = {
    "eboot_spline": (14, 1061, 39, 142),
    "eboot_edgelzma": (38, 3823, 87, 0),
    "eboot_bnuscore": (50, 3583, 232, 44),
    "eboot_animation": (51, 3838, 113, 77),
    "eboot_updatematrix": (174, 14393, 600, 829),
    "eboot_renderparticle": (66, 8112, 261, 575),
    "eboot_morph": (60, 3644, 143, 17),
    "eboot_skin": (42, 3810, 99, 188),
    "libsre_kernel1": (4, 366, 7, 0),
    "libsre_kernel2": (3, 377, 6, 0),
    "libsre_system_service": (18, 2104, 50, 0),
    "libsre_taskset_pm": (19, 1766, 89, 0),
    "libsre_jobchain_pm1": (32, 3872, 114, 0),
    "libsre_jobchain_pm2": (30, 3861, 110, 0),
}

# The kernel is small enough to pin whole: select-workload, the never-returning function, the
# fall-through before the exit-to-kernel address, the exit-to-kernel address, the return of the policy
# module call, the image entry and the return site after the select call.
_KERNEL1_DISPATCHABLE = [0x290, 0x6C0, 0x804, 0x808, 0x814, 0x818, 0x844]

# Addresses the recovery cannot see without help (registry extra_entries), return sites and resume
# points that the runtime dispatches to.
_MUST_DISPATCH = {
    "libsre_system_service": [0xA00, 0xCC8, 0x2864, 0x288C],
    "libsre_taskset_pm": [
        0xA00, 0xA70, 0xB60, 0xC40, 0x231C, 0x2344,
        0x18F4, 0x18F8, 0x18FC, 0x1900, 0x1904, 0x1908, 0x190C, 0x1910,  # Duff table, region fill
        0x223C, 0x2240, 0x2244, 0x2248, 0x224C, 0x2250, 0x2254, 0x2258,  # Duff table, bss zeroing
    ],
    "eboot_edgelzma": [0x3090, 0x3628, 0x5078, 0x32A4, 0x32B0, 0x5770, 0x5720, 0x59F8, 0x5BF8,
                       0x4DC0, 0x4DEC],
}

# The policy module's two switches and the two Duff-device branches. Table 0x1cc4 is found although a
# register of its address computation has no definition inside the resolver's scan window at either
# use (the "same definition" test then compares two absent definitions); tightening that test must not
# lose this table.
_PM_JUMP_TABLES = [
    {"site": "0xfa0", "kind": "table", "table": "0xfa4", "count": 8,
     "targets": ["0xfc4", "0xfcc", "0xfd8", "0x1024", "0x103c", "0x1054", "0x1094", "0x10f8"]},
    {"site": "0x18ec", "kind": "computed", "table": None, "count": 8,
     "targets": ["0x18f4", "0x18f8", "0x18fc", "0x1900", "0x1904", "0x1908", "0x190c", "0x1910"]},
    {"site": "0x1cc0", "kind": "table", "table": "0x1cc4", "count": 5,
     "targets": ["0x1cd8", "0x1d34", "0x1d80", "0x1db8", "0x1dfc"]},
    {"site": "0x2234", "kind": "computed", "table": None, "count": 8,
     "targets": ["0x223c", "0x2240", "0x2244", "0x2248", "0x224c", "0x2250", "0x2254", "0x2258"]},
]

_ENTRY_RE = re.compile(r"^\s*\{ 0x([0-9a-f]{8})u, (\d)u, \{[^}]*\}, (spu_\w+) \},$", re.MULTILINE)
_TABLE_RE = re.compile(r"static const Tox2SpuEntry kEntries_(\w+)\[\] = \{\n(.*?)\n\};", re.DOTALL)
_LINE_RE = re.compile(r"^(\w+): functions (\d+) instructions (\d+) dispatchable (\d+) unhandled (\d+)$")

_CACHE = {}


def _run(module, argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = module.main(argv)
    return code, out.getvalue(), err.getvalue()


def _tree(directory):
    return {p.name: p.read_bytes() for p in sorted(Path(directory).iterdir())}


def _emit_runs():
    """Two spu-emit runs into one directory, as the build does; cached per process."""
    if not _CACHE:
        tmp = tempfile.TemporaryDirectory()
        out = Path(tmp.name) / "spu"
        report = Path(tmp.name) / "report.json"
        argv = ["--eboot", _EBOOT, "--libsre", _LIBSRE, "--out", str(out), "--report", str(report),
                "--max-unhandled", "0"]
        first = _run(spu_emit, argv)
        tree = _tree(out)
        second = _run(spu_emit, argv)
        _CACHE.update(tmp=tmp, first=first, second=second, tree=tree, tree_again=_tree(out),
                      report=json.loads(report.read_text(encoding="utf-8")))
    return _CACHE


def _tables(tree):
    """image -> {ls: (check_count, host function)} read from the emitted spu_tables.c."""
    text = tree["spu_tables.c"].decode("utf-8")
    tables = {}
    for name, body in _TABLE_RE.findall(text):
        tables[name] = {int(ls, 16): (int(count), host) for ls, count, host in _ENTRY_RE.findall(body)}
    return tables


def _database(name):
    return json.loads((_DB_DIR / (name + ".json")).read_text(encoding="utf-8"))


@unittest.skipUnless(_AVAILABLE, _SKIP_REASON)
class RealSpuEmitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runs = _emit_runs()
        cls.tables = _tables(cls.runs["tree"])

    def test_gate_passes(self):
        code, out, err = self.runs["first"]
        self.assertEqual((code, err), (0, ""))
        self.assertIn("gate unhandled: 0\n", out)

    def test_output_lines_cover_every_image(self):
        _, out, _ = self.runs["first"]
        counts = {}
        for line in out.splitlines():
            m = _LINE_RE.match(line)
            if m:
                counts[m.group(1)] = tuple(int(g) for g in m.groups()[1:])
        self.assertEqual(counts, _COUNTS)

    def test_ungated_images_report_their_unhandled_words(self):
        _, out, _ = self.runs["first"]
        for name, counts in _COUNTS.items():
            if name in _GATE_IMAGES:
                self.assertNotIn("ungated unhandled: %s " % name, out)
            else:
                self.assertIn("ungated unhandled: %s %d\n" % (name, counts[3]), out)

    def test_unhandled_words_are_floating_point_only(self):
        for image in self.runs["report"]["images"]:
            with self.subTest(image=image["image"]):
                groups = image["unhandled"]["by_group"]
                other = {g: n for g, n in groups.items() if g not in ("fp_single", "fp_double")}
                self.assertEqual(other, {g: 0 for g in other})
                self.assertEqual(image["unhandled"]["total"], _COUNTS[image["image"]][3])
                self.assertEqual(image["gate"], image["image"] in _GATE_IMAGES)

    def test_no_invalid_flow_or_unlisted_target_in_gate_images(self):
        for image in self.runs["report"]["images"]:
            if image["gate"]:
                with self.subTest(image=image["image"]):
                    self.assertEqual(image["unlisted_targets"], [])
                    # kernel 1 falls from 0x844 into the data that follows the code
                    expected = 1 if image["image"] == "libsre_kernel1" else 0
                    self.assertEqual(image["edges"]["invalid_flow"], expected)

    def test_second_run_rewrites_nothing(self):
        code, out, _ = self.runs["second"]
        self.assertEqual(code, 0)
        self.assertRegex(out, r"output: \d+ files, 0 written, in ")
        self.assertEqual(self.runs["tree"], self.runs["tree_again"])

    def test_tables_match_the_report(self):
        self.assertEqual(sorted(self.tables), sorted(_COUNTS))
        for name, counts in _COUNTS.items():
            with self.subTest(image=name):
                self.assertEqual(len(self.tables[name]), counts[2])
                self.assertTrue(all(1 <= n <= 4 for n, _ in self.tables[name].values()))

    def test_tables_equal_the_database_seeds(self):
        for name in _COUNTS:
            with self.subTest(image=name):
                seeds = {int(s["ls"], 16) for s in _database(name)["dispatch_seeds"]}
                self.assertEqual(set(self.tables[name]), seeds)

    def test_kernel_dispatchable_set(self):
        self.assertEqual(sorted(self.tables["libsre_kernel1"]), _KERNEL1_DISPATCHABLE)

    def test_required_dispatchable_addresses(self):
        for name, addrs in _MUST_DISPATCH.items():
            for addr in addrs:
                with self.subTest(image=name, ls=hex(addr)):
                    self.assertIn(addr, self.tables[name])

    def test_jump_table_targets_are_dispatchable_in_the_taskset_module(self):
        for table in _PM_JUMP_TABLES:
            for target in table["targets"]:
                with self.subTest(site=table["site"], target=target):
                    self.assertIn(int(target, 16), self.tables["libsre_taskset_pm"])

    def test_kernel_images_start_at_their_entry(self):
        for name, entry in (("libsre_kernel1", 0x818), ("libsre_system_service", 0xA00),
                            ("libsre_taskset_pm", 0xA00), ("eboot_edgelzma", 0x3090)):
            with self.subTest(image=name):
                self.assertEqual(int(_database(name)["entry"], 16), entry)
                self.assertIn(entry, self.tables[name])

    def test_generated_symbols_are_per_image(self):
        for name, table in self.tables.items():
            for host in {h for _, h in table.values()}:
                with self.subTest(image=name, host=host):
                    self.assertTrue(host.startswith("spu_%s_" % name))


@unittest.skipUnless(_AVAILABLE, _SKIP_REASON)
class RealSpuDatabaseTest(unittest.TestCase):
    def test_taskset_jump_tables(self):
        self.assertEqual(_database("libsre_taskset_pm")["jump_tables"], _PM_JUMP_TABLES)

    def test_taskset_jump_table_targets_are_seeds_from_the_table(self):
        db = _database("libsre_taskset_pm")
        sources = {s["ls"]: s["sources"] for s in db["dispatch_seeds"]}
        for table in _PM_JUMP_TABLES:
            for target in table["targets"]:
                with self.subTest(site=table["site"], target=target):
                    self.assertIn("jump_table", sources[target])

    def test_unresolved_indirect_sites_are_the_known_run_time_pointers(self):
        sites = {name: [u["site"] for u in _database(name)["unresolved_indirect"]]
                 for name in _GATE_IMAGES}
        self.assertEqual(sites, {
            "eboot_edgelzma": ["0x5074", "0x50ac", "0x50d8", "0x576c", "0x5e34", "0x6214",
                               "0x6a6c", "0x6d3c"],
            "libsre_kernel1": [],
            "libsre_system_service": ["0x2860", "0x2888"],
            "libsre_taskset_pm": ["0x2318", "0x2340"],
        })

    def test_committed_databases_are_what_spu_recover_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "db"
            code, _, err = _run(spu_recover, ["--eboot", _EBOOT, "--libsre", _LIBSRE, "--out", str(out),
                                              "--report", str(Path(tmp) / "report.json")])
            self.assertEqual((code, err), (0, ""))
            self.assertEqual(sorted(_tree(out)), sorted(n + ".json" for n in _COUNTS))
            for name, data in _tree(out).items():
                with self.subTest(file=name):
                    self.assertEqual(data, (_DB_DIR / name).read_bytes())


if __name__ == "__main__":
    unittest.main()

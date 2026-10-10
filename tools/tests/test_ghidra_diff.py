import contextlib
import hashlib
import io
import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import ghidra_diff, ghidra_norm
from ps3recomp.ppu import decode

from tests.elf_fixture import FixtureSegment, build_elf64_be

TEXT_VA = 0x10000
DATA_VA = 0x20000

MFLR = 0x7C0802A6  # mflr r0 (mfspr r0,8)
BLR = 0x4E800020
STDU = 0xF821FF91  # stdu r1,-0x70(r1)
CMPWI = 0x2C030000  # cmpwi r3,0
SLWI = 21 << 26 | 4 << 21 | 3 << 16 | 2 << 11 | 0 << 6 | 29 << 1  # rlwinm r3,r4,2,0,29
BEQ_CR1 = 16 << 26 | 12 << 21 | 6 << 16 | 0x10  # bc 12,6,+0x10 at address 0
ADDI_M1 = 0x3863FFFF  # addi r3,r3,-1
UNKNOWN = 0x00000000

# (word, (kind, mnemonic, operands)) rows; the expected categories are listed in the tests.
ROWS = (
    (MFLR, ("code", "mflr", "r0")),                      # match
    (MFLR, ("code", "mfspr", "r0,CTR")),                 # operand_mismatch
    (MFLR, ("code", "mtspr", "r0,CTR")),                 # mnemonic_mismatch
    (UNKNOWN, ("code", "lvlx", "v2,r0,r5")),             # ghidra_code_we_unknown
    (MFLR, ("data", None, None)),                        # we_code_ghidra_noncode, ghidra_bad
    (UNKNOWN, ("undefined", None, None)),                # both_noncode_unknown, agree
    (BLR, ("code", "blr", "")),                          # match
    (BLR, ("data", "blr", "")),                          # we_code_ghidra_noncode, agree
    (UNKNOWN, ("data", "add", "r1,r2,r3")),              # both_noncode_unknown, we_unknown
    (MFLR, ("undefined", "mfspr", "r0,CTR")),            # we_code_ghidra_noncode, operand_mismatch
    (MFLR, ("undefined", "mtspr", "r0,CTR")),            # we_code_ghidra_noncode, mnemonic_mismatch
)


def build_elf(words) -> bytes:
    text = b"".join(w.to_bytes(4, "big") for w in words)
    data = bytearray(0x20)
    data[0:8] = TEXT_VA.to_bytes(4, "big") + (DATA_VA + 0x8000).to_bytes(4, "big")
    return build_elf64_be(entry=DATA_VA, segments=[
        FixtureSegment(1, TEXT_VA, text, flags=5),
        FixtureSegment(1, DATA_VA, bytes(data), memsz=0x1000, flags=6),
    ])


def make_export(data: bytes, words, rows, **overrides) -> dict:
    """Synthetic export; `rows` holds one (kind, mnemonic, operands) per word."""
    entries = [{"addr": "0x%08x" % (TEXT_VA + 4 * i), "word": "0x%08x" % word,
                "kind": kind, "mnemonic": mnemonic, "operands": operands}
               for i, (word, (kind, mnemonic, operands)) in enumerate(zip(words, rows))]
    export = {
        "schema": 1,
        "executable_sha256": hashlib.sha256(data).hexdigest(),
        "memory_blocks": [],
        "text": {"start": "0x%08x" % TEXT_VA, "end": "0x%08x" % (TEXT_VA + 4 * len(words))},
        "functions": [],
        "words": entries,
        "jump_tables": [],
        "data_refs": [],
    }
    export.update(overrides)
    return export


class NormalizeTest(unittest.TestCase):
    def test_memory_operand_splits_into_two_tokens(self):
        self.assertEqual(ghidra_norm.normalize("stdu", "r1,-0x70(r1)"),
                         ("stdu", ("r1", -0x70, "r1")))
        self.assertEqual(ghidra_norm.normalize("STDU", " R1, -0x70 (R1)"),
                         ("stdu", ("r1", -0x70, "r1")))

    def test_leading_cr0_is_dropped(self):
        self.assertEqual(ghidra_norm.normalize("cmpwi", "cr0,r3,0x0"),
                         ghidra_norm.normalize("cmpwi", "r3,0x0"))
        self.assertEqual(ghidra_norm.normalize("cmpwi", "cr1,r3,0x0"),
                         ("cmpwi", ("cr1", "r3", 0)))
        self.assertEqual(ghidra_norm.normalize("blr", ""), ("blr", ()))
        self.assertEqual(ghidra_norm.normalize("blr", None), ("blr", ()))

    def test_branch_hint_is_stripped(self):
        self.assertEqual(ghidra_norm.normalize("beq+", "cr1,0x100"), ("beq", ("cr1", 0x100)))
        self.assertEqual(ghidra_norm.normalize("bne-", "0x100"), ("bne", (0x100,)))
        self.assertEqual(ghidra_norm.normalize("stwcx.", "r3,0,r9")[0], "stwcx.")

    def test_integer_forms(self):
        self.assertEqual(ghidra_norm.normalize("li", "r3,-1")[1], ("r3", -1))
        self.assertEqual(ghidra_norm.normalize("li", "r3,0xFF")[1], ("r3", 255))
        self.assertEqual(ghidra_norm.normalize("mfspr", "r0,LR")[1], ("r0", "lr"))

    def test_zero_base_in_memory_operand(self):
        self.assertEqual(ghidra_norm.normalize("lwz", "r0,0x0(0)"), ("lwz", ("r0", 0, "r0")))
        self.assertEqual(ghidra_norm.normalize("lwz", "r0,0x0(r0)"), ("lwz", ("r0", 0, "r0")))

    def test_sign_extension_display(self):
        insn = decode(STDU)
        self.assertEqual(ghidra_norm.compare(insn, "stdu", "r1,-0x70(r1)"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "stdu", "r1,0xff90(r1)"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "stdu", "r1,0xffffff90(r1)"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "stdu", "r1,0xff91(r1)"), "operand_mismatch")

    def test_cr0_on_either_side(self):
        insn = decode(CMPWI)
        self.assertEqual(ghidra_norm.compare(insn, "cmpwi", "cr0,r3,0x0"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "cmpwi", "r3,0x0"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "cmpwi", "cr1,r3,0x0"), "operand_mismatch")

    def test_decimal_fields_vs_hex(self):
        insn = decode(SLWI)
        # Simplified form and raw form both match.
        self.assertEqual(ghidra_norm.compare(insn, "rlwinm", "r3,r4,0x2,0x0,0x1d"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "slwi", "r3,r4,0x2"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "rlwinm", "r3,r4,0x2,0x0,0x1c"),
                         "operand_mismatch")

    def test_branch_hint_in_compare(self):
        insn = decode(BEQ_CR1, 0)
        self.assertEqual(ghidra_norm.compare(insn, "beq", "cr1,0x10"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "beq+", "cr1,0x10"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "bne+", "cr1,0x10"), "mnemonic_mismatch")

    def test_int_vs_string_tokens_differ(self):
        insn = decode(MFLR)
        self.assertEqual(ghidra_norm.compare(insn, "mfspr", "r0,LR"), "match")
        self.assertEqual(ghidra_norm.compare(insn, "mfspr", "r0,lr_"), "operand_mismatch")

    def test_rewrite_hook(self):
        insn = decode(ADDI_M1)
        self.assertEqual(ghidra_norm.compare(insn, "frobi", "r3,r3,0x1"), "mnemonic_mismatch")
        rewrite = {"frobi": lambda t: ("addi", (t[0], t[1], -t[2]))}
        with mock.patch.dict(ghidra_norm.GHIDRA_REWRITES, rewrite):
            self.assertEqual(ghidra_norm.compare(insn, "frobi", "r3,r3,0x1"), "match")

    def test_rewrite_keys_are_normalised_mnemonics(self):
        self.assertTrue(ghidra_norm.GHIDRA_REWRITES)
        for name in ghidra_norm.GHIDRA_REWRITES:
            self.assertEqual(ghidra_norm.normalize_mnemonic(name), name)


class RewriteTest(unittest.TestCase):
    """Real words from the executable whose Ghidra text differs only in syntax."""

    def check(self, word, mnemonic, operands, expected="match"):
        self.assertEqual(ghidra_norm.compare(decode(word), mnemonic, operands), expected,
                         "%08x %s %s" % (word, mnemonic, operands))

    def test_sub_forms_are_negative_add_forms(self):
        self.check(0x38638814, "subi", "r3,r3,0x77ec")      # addi r3,r3,-0x77ec
        self.check(0x3084E4F8, "subic", "r4,r4,0x1b08")     # addic r4,r4,-0x1b08
        self.check(0x37FFFFFF, "subic.", "r31,r31,0x1")     # addic. r31,r31,-1
        self.check(0x3D29FEED, "subis", "r9,r9,0x113")      # addis r9,r9,-0x113
        self.check(0x38638814, "subi", "r3,r3,0x77ed", "operand_mismatch")
        self.check(0x38638814, "subi", "r3,r3,-0x77ec", "operand_mismatch")
        self.check(0x38638814, "subic", "r3,r3,0x77ec", "mnemonic_mismatch")

    def test_spr_names(self):
        self.check(0x7C0803A6, "mtspr", "LR,r0")            # mtlr r0
        self.check(0x7C0802A6, "mfspr", "r0,LR")
        self.check(0x7C0903A6, "mtspr", "CTR,r0")           # mtctr r0
        self.check(0x7C6C42E6, "mftb", "r3,TBLr")           # mftb r3,268
        self.check(0x7C0802A6, "mfspr", "r0,CTR", "operand_mismatch")
        self.check(0x7C0802A6, "mfspr", "r0,8")

    def test_zero_base_register(self):
        self.check(0x7C4018CE, "lvx", "v2,0,r3")            # lvx v2,r0,r3
        self.check(0x7FE001CE, "stvx", "v31,0,r0")
        self.check(0x7C001A2C, "dcbt", "0,r3")
        self.check(0x7C004FAE, "stfiwx", "f0,0,r9")
        self.check(0x7C60198E, "stvewx", "v3,0,r3")
        self.check(0x80000000, "lwz", "r0,0x0(0)")
        self.check(0x7C4018CE, "lvx", "v2,r0,r3")
        # A base register that is not r0 must not match a zero.
        self.check(0x7C4118CE, "lvx", "v2,0,r3", "operand_mismatch")
        self.check(0x7C4118CE, "lvx", "v2,r1,r3")

    def test_zero_base_rewrites_cover_indexed_forms_only(self):
        for name in ("lvx", "stvx", "dcbt", "stfiwx", "stvewx", "lwzx", "stwcx.", "lfdx"):
            self.assertIn(name, ghidra_norm.GHIDRA_REWRITES)
        for name in ("add", "and", "lwz", "cmp", "tw"):
            self.assertNotIn(name, ghidra_norm.GHIDRA_REWRITES)

    def test_sc_level(self):
        self.check(0x44000002, "sc", "0x0")
        self.check(0x44000002, "sc", "")
        self.check(0x44000002, "sc", "0x1", "operand_mismatch")

    def test_trap_without_to_operand(self):
        self.check(0x0C0D0E0F, "twi", "r13,0xe0f")          # twi 0,r13,0xe0f
        self.check(0x08090A0B, "tdi", "r9,0xa0b")
        self.check(0x0C8D0001, "twi", "r13,0x1", "operand_mismatch")   # twi 4,r13,1

    def test_condition_register_bit_names(self):
        self.check(0x4C402B82, "cror", "eq,lt,4*cr1+gt")    # cror 2,0,5
        self.check(0x4C460342, "crorc", "eq,4*cr1+eq,lt")   # crorc 2,6,0
        self.check(0x4C4231C2, "crnand", "eq,eq,4*cr1+eq")  # crnand 2,2,6
        self.check(0x4C423042, "crnor", "eq,eq,4*cr1+eq")   # crnor 2,2,6
        self.check(0x4C402B82, "cror", "eq,lt,4*cr1+eq", "operand_mismatch")
        self.check(0x4C402B82, "cror", "eq,lt,gt", "operand_mismatch")

    def test_no_known_ghidra_differences_yet(self):
        # No word in the real .text is a Ghidra-side limitation; add entries here when one is.
        self.assertEqual(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, {})


class ClassifyTest(unittest.TestCase):
    def classify(self, word, row):
        return ghidra_diff.classify(decode(word), *row)

    def test_each_category(self):
        expected = [
            "match", "operand_mismatch", "mnemonic_mismatch", "ghidra_code_we_unknown",
            "we_code_ghidra_noncode", "both_noncode_unknown", "match",
            "we_code_ghidra_noncode", "both_noncode_unknown", "we_code_ghidra_noncode",
            "we_code_ghidra_noncode",
        ]
        self.assertEqual([self.classify(w, row)[0] for w, row in ROWS], expected)

    def test_decoder_assumptions(self):
        self.assertIsNone(decode(UNKNOWN).spec)
        self.assertEqual(decode(MFLR).spec.name, "mfspr")

    def test_pseudo_counters(self):
        expected = [None, None, None, None, "ghidra_bad", "agree", None, "agree",
                    "we_unknown", "operand_mismatch", "mnemonic_mismatch"]
        self.assertEqual([self.classify(w, row)[1] for w, row in ROWS], expected)

    def test_match_via_raw_only(self):
        self.assertEqual(self.classify(SLWI, ("code", "rlwinm", "r3,r4,0x2,0x0,0x1d"))[0],
                         "match")

    def test_known_difference_wins_over_bug_categories(self):
        known = {("addi", "addic"): "different mnemonic"}
        row = ("code", "addic", "r3,r3,-0x1")
        self.assertEqual(self.classify(ADDI_M1, row)[0], "mnemonic_mismatch")
        with mock.patch.dict(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, known):
            self.assertEqual(self.classify(ADDI_M1, row)[0], "known_difference")
            # A real match still counts as a match.
            self.assertEqual(self.classify(ADDI_M1, ("code", "addi", "r3,r3,-0x1"))[0],
                             "match")

    def test_known_difference_for_word_we_cannot_decode(self):
        row = ("code", "lvlx", "v2,r0,r5")
        self.assertEqual(self.classify(UNKNOWN, row)[0], "ghidra_code_we_unknown")
        with mock.patch.dict(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, {(None, "lvlx"): "Cell-only"}):
            self.assertEqual(self.classify(UNKNOWN, row)[0], "known_difference")

    def test_known_difference_key_uses_normalised_mnemonic(self):
        known = {("bc", "beq"): "x"}
        with mock.patch.dict(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, known):
            self.assertEqual(self.classify(BEQ_CR1, ("code", "BEQ+", "0x99"))[0],
                             "known_difference")

    def test_known_difference_not_applied_to_noncode(self):
        with mock.patch.dict(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, {("mfspr", None): "x"}):
            self.assertEqual(self.classify(MFLR, ("data", None, None))[0],
                             "we_code_ghidra_noncode")

    def test_code_word_with_null_mnemonic(self):
        self.assertEqual(self.classify(MFLR, ("code", None, None))[0], "mnemonic_mismatch")
        self.assertEqual(self.classify(UNKNOWN, ("code", None, None))[0],
                         "ghidra_code_we_unknown")


class DiffCliTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.elf_path = self.tmp / "input.elf"
        self.json_path = self.tmp / "export.json"
        self.out_path = self.tmp / "out" / "report.json"
        self.words = [w for w, _ in ROWS]
        self.rows = [r for _, r in ROWS]
        self.data = build_elf(self.words)

    def run_diff(self, export=None, *extra, data=None, write_export=True):
        self.elf_path.write_bytes(data if data is not None else self.data)
        if write_export:
            if export is None:
                export = make_export(self.data, self.words, self.rows)
            self.json_path.write_text(json.dumps(export), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ghidra_diff.main([str(self.elf_path), "--ghidra", str(self.json_path),
                                     "-o", str(self.out_path), *extra])
        return code, out.getvalue(), err.getvalue()

    def report(self):
        return json.loads(self.out_path.read_text(encoding="utf-8"))

    def test_report_counts(self):
        code, out, err = self.run_diff()
        self.assertEqual((code, err), (0, ""))
        report = self.report()
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["input"]["sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertEqual(report["input"]["ghidra_json"], str(self.json_path))
        self.assertEqual(report["range"], {"start": "0x00010000", "end": "0x0001002c"})
        self.assertIs(report["strict"], False)
        self.assertEqual(report["total_words"], 11)
        self.assertEqual(report["ghidra_kinds"], {"code": 5, "data": 3, "undefined": 3})
        self.assertEqual(report["categories"], {
            "match": 2, "known_difference": 0, "ghidra_code_we_unknown": 1,
            "mnemonic_mismatch": 1, "operand_mismatch": 1, "we_code_ghidra_noncode": 4,
            "both_noncode_unknown": 2})
        self.assertEqual(report["noncode_pseudo"], {
            "agree": 2, "mnemonic_mismatch": 1, "operand_mismatch": 1, "ghidra_bad": 1,
            "we_unknown": 1})

    def test_strict_rejects_reserved_bits(self):
        # mtspr with reserved bit 31 set: decoded non-strictly, unknown in strict mode.
        self.words = [0x7C0803A7]
        self.rows = [("code", "mtspr", "LR,r0")]
        self.data = build_elf(self.words)
        code, _, err = self.run_diff(None, "--strict")
        self.assertEqual((code, err), (0, ""))
        report = self.report()
        self.assertIs(report["strict"], True)
        self.assertEqual(report["categories"]["ghidra_code_we_unknown"], 1)
        code, _, _ = self.run_diff()
        self.assertEqual(code, 0)
        self.assertIs(self.report()["strict"], False)
        self.assertEqual(self.report()["categories"]["ghidra_code_we_unknown"], 0)

    def test_stdout(self):
        _, out, _ = self.run_diff()
        self.assertIn("total words: 11\n", out)
        self.assertIn("  match                    2\n", out)
        self.assertIn("  both_noncode_unknown     2\n", out)
        self.assertIn("  ghidra_bad               1\n", out)
        self.assertIn("top pairs for operand_mismatch", out)
        self.assertIn("top pairs for ghidra_code_we_unknown", out)
        self.assertNotIn("top pairs for known_difference", out)
        self.assertIn("report: %s\n" % self.out_path, out)

    def test_pair_structure(self):
        self.run_diff()
        pairs = self.report()["pairs"]
        self.assertEqual(list(pairs), [
            "known_difference", "ghidra_code_we_unknown", "mnemonic_mismatch",
            "operand_mismatch", "we_code_ghidra_noncode", "noncode_mnemonic_mismatch",
            "noncode_operand_mismatch", "noncode_ghidra_bad"])
        self.assertEqual(pairs["known_difference"], [])
        self.assertEqual(pairs["operand_mismatch"], [{
            "ours": "mfspr", "ghidra": "mfspr", "count": 1,
            "examples": [{"addr": "0x00010004", "word": "0x7c0802a6", "ours": "mflr r0",
                          "ghidra": "mfspr r0,CTR"}]}])
        self.assertEqual(pairs["ghidra_code_we_unknown"], [{
            "ours": None, "ghidra": "lvlx", "count": 1,
            "examples": [{"addr": "0x0001000c", "word": "0x00000000", "ours": ".long 0x00000000",
                          "ghidra": "lvlx v2,r0,r5"}]}])
        noncode = {(p["ours"], p["ghidra"]) for p in pairs["we_code_ghidra_noncode"]}
        self.assertEqual(noncode, {("mfspr", None), ("bclr", "blr"), ("mfspr", "mfspr"),
                                   ("mfspr", "mtspr")})
        self.assertEqual(pairs["noncode_ghidra_bad"][0]["ghidra"], None)
        self.assertEqual(pairs["noncode_mnemonic_mismatch"][0]["ghidra"], "mtspr")
        self.assertEqual(pairs["noncode_operand_mismatch"][0]["ghidra"], "mfspr")

    def test_known_difference_in_report(self):
        known = {(None, "lvlx"): "Cell-only"}
        with mock.patch.dict(ghidra_norm.KNOWN_GHIDRA_DIFFERENCES, known):
            self.run_diff()
        report = self.report()
        self.assertEqual(report["categories"]["known_difference"], 1)
        self.assertEqual(report["categories"]["ghidra_code_we_unknown"], 0)
        self.assertEqual(report["pairs"]["known_difference"][0]["ghidra"], "lvlx")

    def test_top_and_examples_limits(self):
        words = [MFLR] * 3 + [BLR] * 2
        rows = [("data", None, None)] * 3 + [("data", "blr", "")] * 2
        data = build_elf(words)
        export = make_export(data, words, rows)
        self.run_diff(export, "--top", "1", "--examples", "2", data=data)
        pairs = self.report()["pairs"]["we_code_ghidra_noncode"]
        self.assertEqual(len(pairs), 1)
        self.assertEqual((pairs[0]["ours"], pairs[0]["count"], len(pairs[0]["examples"])),
                         ("mfspr", 3, 2))
        self.assertEqual([e["addr"] for e in pairs[0]["examples"]],
                         ["0x00010000", "0x00010004"])
        self.run_diff(export, "--examples", "0", data=data)
        self.assertEqual(self.report()["pairs"]["we_code_ghidra_noncode"][0]["examples"], [])

    def test_sub_range(self):
        code, _, _ = self.run_diff(None, "--start", "0x10004", "--end", "0x10010")
        self.assertEqual(code, 0)
        report = self.report()
        self.assertEqual(report["range"], {"start": "0x00010004", "end": "0x00010010"})
        self.assertEqual(report["total_words"], 3)
        self.assertEqual(report["categories"]["operand_mismatch"], 1)
        self.assertEqual(report["categories"]["mnemonic_mismatch"], 1)
        self.assertEqual(report["categories"]["ghidra_code_we_unknown"], 1)
        self.assertEqual(report["categories"]["match"], 0)

    def test_sha256_mismatch_is_exit_4(self):
        export = make_export(self.data, self.words, self.rows, executable_sha256="0" * 64)
        self.assertEqual(self.run_diff(export)[0], 4)

    def test_word_mismatch_is_exit_3(self):
        export = make_export(self.data, self.words, self.rows)
        export["words"][3]["word"] = "0x00000001"
        code, _, err = self.run_diff(export)
        self.assertEqual(code, 3)
        self.assertIn("0x0001000c", err)
        self.assertEqual(len(err.strip().splitlines()), 1)

    def test_range_outside_text_is_exit_5(self):
        self.assertEqual(self.run_diff(None, "--start", "0x10000", "--end", "0x10030")[0], 5)
        self.assertEqual(self.run_diff(None, "--start", "0xfffc", "--end", "0x10004")[0], 5)

    def test_range_outside_exec_segment_is_exit_5(self):
        export = make_export(self.data, self.words, self.rows,
                             text={"start": "0x00020000", "end": "0x00020010"})
        self.assertEqual(self.run_diff(export)[0], 5)

    def test_missing_json_is_exit_1(self):
        code, _, err = self.run_diff(write_export=False)
        self.assertEqual(code, 1)
        self.assertTrue(err.startswith("error: "))

    def test_invalid_json_is_exit_1(self):
        self.elf_path.write_bytes(self.data)
        self.json_path.write_text("{", encoding="utf-8")
        self.assertEqual(self.run_diff(write_export=False)[0], 1)

    def test_wrong_schema_is_exit_1(self):
        export = make_export(self.data, self.words, self.rows, schema=2)
        self.assertEqual(self.run_diff(export)[0], 1)

    def test_truncated_words_is_exit_1(self):
        export = make_export(self.data, self.words, self.rows)
        del export["words"][-1]
        self.assertEqual(self.run_diff(export)[0], 1)

    def test_bad_kind_is_exit_1(self):
        export = make_export(self.data, self.words, self.rows)
        export["words"][0]["kind"] = "bogus"
        self.assertEqual(self.run_diff(export)[0], 1)

    def test_memory_map_is_not_checked(self):
        self.assertEqual(self.run_diff()[0], 0)

    def test_not_an_elf_is_exit_3(self):
        self.assertEqual(self.run_diff(data=b"not an elf")[0], 3)

    def test_usage_errors(self):
        for args in (["--start", "0x10000"], ["--start", "0x10002", "--end", "0x10010"],
                     ["--start", "0x10010", "--end", "0x10000"], ["--top", "0"],
                     ["--examples", "-1"]):
            self.assertEqual(self.run_diff(None, *args)[0], 2, args)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ghidra_diff.main([str(self.elf_path)]), 2)


@unittest.skipUnless(os.environ.get("TOX2_EBOOT_ELF") and os.environ.get("TOX2_GHIDRA_JSON"),
                     "TOX2_EBOOT_ELF and TOX2_GHIDRA_JSON are not set")
class RealGhidraDiffTest(unittest.TestCase):
    def test_diff_invariants(self):
        elf_path, json_path = os.environ["TOX2_EBOOT_ELF"], os.environ["TOX2_GHIDRA_JSON"]
        if not (os.path.isfile(elf_path) and os.path.isfile(json_path)):
            self.skipTest("TOX2_EBOOT_ELF or TOX2_GHIDRA_JSON does not point to a file")
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "report.json"
            started = time.monotonic()
            with contextlib.redirect_stdout(io.StringIO()):
                code = ghidra_diff.main([elf_path, "--ghidra", json_path, "-o", str(out_path)])
            elapsed = time.monotonic() - started
            report = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(code, 0)
        self.assertLess(elapsed, 60)
        self.assertEqual(report["total_words"], 3275877)
        self.assertEqual(report["range"], {"start": "0x00010240", "end": "0x00c8f3d4"})
        self.assertEqual(sum(report["ghidra_kinds"].values()), 3275877)
        self.assertEqual(sum(report["categories"].values()), 3275877)
        self.assertEqual(report["ghidra_kinds"]["code"], sum(
            report["categories"][k] for k in (
                "match", "known_difference", "ghidra_code_we_unknown", "mnemonic_mismatch",
                "operand_mismatch")))


if __name__ == "__main__":
    unittest.main()

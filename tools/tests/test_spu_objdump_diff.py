"""Tests for the objdump cross-check of the SPU decoder; no WSL or objdump is needed.

Listings are written by hand in the layout of the documented objdump listing (address, raw bytes,
mnemonic, operands); the words are encoded with `spu_asm` and the instruction text is derived from the
SPU ISA assembler syntax.
"""

import contextlib
import io
import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ps3tool
from ps3recomp import spu_objdump, spu_objdump_diff
from ps3recomp.spu import decode
from ps3recomp.spu_format import format_insn
from ps3recomp.spu_objdump import CanonInsn, ListingError, ObjdumpUnavailable, Operand, canon_text
from ps3recomp.spu_objdump_diff import KnownDifference, compare

from tests import test_spu_images as base
from tests.elf_fixture import FixtureSegment, build_elf32_be
from tests.spu_asm import encode

HEADER = [
    "",
    "/tmp/tmp.AbCdEf:     file format binary",
    "",
    "",
    "Disassembly of section .data:",
    "",
    "00003000 <.data>:",
]


def listing_line(addr: int, word: int, text: str) -> str:
    raw = "".join("%02x " % b for b in struct.pack(">I", word))
    return "    %x:\t%s\t%s" % (addr, raw, text)


def listing(ls: int, entries: list[tuple[int, str]]) -> str:
    lines = list(HEADER)
    lines += [listing_line(ls + 4 * i, word, text) for i, (word, text) in enumerate(entries)]
    return "\n".join(lines) + "\n"


def words_to_bytes(words: list[int]) -> bytes:
    return b"".join(struct.pack(">I", w) for w in words)


def category(word: int, objdump_text: str, addr: int = 0x3000, known=()) -> str:
    ours = decode(word, addr)
    return compare(ours, canon_text(format_insn(ours)), canon_text(objdump_text), known)


# The ISA assembler syntax of words that are encoded from the instruction formats (ISA chapter
# "Instruction formats"); the same words and texts appear in the listing layout notes.
DOCUMENTED = [
    (0x3000, 0x40800003, "il\t$3,0"),
    (0x3004, 0x1CF80081, "ai\t$1,$1,-32"),
    (0x3008, 0x24004080, "stqd\t$0,16($1)"),
    (0x300C, 0x34008083, "lqd\t$3,32($1)\t# 20"),
    (0x3010, 0x34FFC084, "lqd\t$4,-16($1)"),
    (0x3014, 0x40FFFF86, "il\t$6,-1"),
    (0x3018, 0x60DF7785, "iohl\t$5,48879\t# beef"),
    (0x301C, 0x01A00183, "rdch\t$3,$ch3"),
    (0x3020, 0x21A00E04, "wrch\t$ch28,$4"),
    (0x3024, 0x01E00E85, "rchcnt\t$5,$ch29"),
    (0x3028, 0x33000800, "brsl\t$0,0x3068\t# 3068"),
    (0x302C, 0x327FFE00, "br\t0x301c\t# 301c"),
    (0x3030, 0x35040000, "bie\t$0"),
    (0x3034, 0x35080280, "bid\t$5"),
    (0x3038, 0x35800182, "hbr\t0x3040,$3"),
    (0x303C, 0x35900000, "hbrp\t0x303c,$0"),
    (0x3040, 0x10008004, "hbra\t0x3050,0x400"),
    (0x3044, 0x12000801, "hbrr\t0x3048,0x3084\t# 3084"),
    (0x3048, 0x00002001, "stop"),
    (0x304C, 0x40200000, "nop\t$0"),
    (0x3050, 0x00200000, "lnop"),
    (0x3054, 0x35400000, "iret\t$0"),
    (0x3058, 0x32F87804, "fsmbi\t$4,61680\t# f0f0"),
    (0x305C, 0xB0A1C308, "shufb\t$5,$6,$7,$8"),
    (0x3060, 0x90000000, ".long 0x90000000"),
    (0x3064, 0x00000000, "stop"),
]


class ListingParserTest(unittest.TestCase):
    CODE = words_to_bytes([0x40800003, 0x00200000])

    def test_skips_header_section_and_label_lines(self):
        text = listing(0x3000, [(0x40800003, "il\t$3,0"), (0x00200000, "lnop")])
        text += "00003008 <next>:\n"
        lines = spu_objdump.parse_listing(text, 0x3000, self.CODE)
        self.assertEqual([(l.addr, l.word, l.text) for l in lines],
                         [(0x3000, 0x40800003, "il\t$3,0"), (0x3004, 0x00200000, "lnop")])

    def test_trailing_comment_is_kept_in_text_and_stripped_by_canon(self):
        text = listing(0x3000, [(0x40800003, "il\t$3,0\t# 0"), (0x00200000, "lnop")])
        self.assertEqual(spu_objdump.parse_listing(text, 0x3000, self.CODE)[0].text, "il\t$3,0\t# 0")
        self.assertEqual(canon_text("il\t$3,0\t# 0"), canon_text("il\t$3,0"))

    def test_wrong_address(self):
        text = listing(0x3000, [(0x40800003, "il\t$3,0")])
        text += listing_line(0x3008, 0x00200000, "lnop") + "\n"
        with self.assertRaisesRegex(ListingError, "listing out of step at ls 0x03004") as ctx:
            spu_objdump.parse_listing(text, 0x3000, self.CODE)
        self.assertEqual(ctx.exception.code, 1)

    def test_wrong_bytes(self):
        text = listing(0x3000, [(0x40800003, "il\t$3,0"), (0x00200001, "stop")])
        with self.assertRaisesRegex(ListingError, "listing out of step at ls 0x03004"):
            spu_objdump.parse_listing(text, 0x3000, self.CODE)

    def test_missing_word(self):
        text = listing(0x3000, [(0x40800003, "il\t$3,0")])
        with self.assertRaisesRegex(ListingError, "listing out of step at ls 0x03004"):
            spu_objdump.parse_listing(text, 0x3000, self.CODE)

    def test_missing_first_word(self):
        with self.assertRaisesRegex(ListingError, "listing out of step at ls 0x03000"):
            spu_objdump.parse_listing("\n".join(HEADER) + "\n", 0x3000, self.CODE)

    def test_duplicate_and_extra_words(self):
        dup = listing(0x3000, [(0x40800003, "il\t$3,0"), (0x40800003, "il\t$3,0")])
        with self.assertRaisesRegex(ListingError, "listing out of step"):
            spu_objdump.parse_listing(dup, 0x3000, self.CODE)
        extra = listing(0x3000, [(0x40800003, "il\t$3,0"), (0x00200000, "lnop"), (0, "stop")])
        with self.assertRaisesRegex(ListingError, "listing out of step at ls 0x03008"):
            spu_objdump.parse_listing(extra, 0x3000, self.CODE)

    def test_address_at_zero(self):
        text = listing(0, [(0x40800003, "il\t$3,0"), (0x00200000, "lnop")])
        self.assertEqual(spu_objdump.parse_listing(text, 0, self.CODE)[1].addr, 4)


class CanonTest(unittest.TestCase):
    def test_rule1_registers_are_numeric(self):
        self.assertEqual(canon_text("a\t$127,$0,$1").operands,
                         (Operand("reg", 127), Operand("reg", 0), Operand("reg", 1)))
        self.assertEqual(category(encode("a", RT=127, RA=0, RB=1), "a\t$127,$0,$2"), "operand_mismatch")

    def test_rule2_channels_are_numeric(self):
        self.assertEqual(canon_text("wrch\t$ch28,$4").operands, (Operand("chan", 28), Operand("reg", 4)))
        self.assertEqual(category(encode("wrch", CA=28, RT=4), "wrch\t$ch28,$4"), "match")
        self.assertEqual(category(encode("wrch", CA=28, RT=4), "wrch\t$ch29,$4"), "operand_mismatch")

    def test_rule3_number_notations(self):
        for text in ("0x1f", "0X1F", "31", "+31", "1f"):
            self.assertEqual(spu_objdump.parse_number(text), 31, text)
        self.assertEqual(spu_objdump.parse_number("-0x10"), -16)
        self.assertEqual(spu_objdump.parse_number("-16"), -16)
        self.assertIsNone(spu_objdump.parse_number("$3"))
        word = encode("il", RT=3, I16=31)
        for text in ("il\t$3,31", "il\t$3,0x1f", "il\t$3,+31"):
            self.assertEqual(category(word, text), "match", text)

    def test_rule4_immediates_compare_modulo_the_field_width(self):
        # il: 16-bit field, so -1 and 65535 are the same bits.
        self.assertEqual(category(encode("il", RT=6, I16=-1), "il\t$6,65535"), "match")
        self.assertEqual(category(encode("il", RT=6, I16=-1), "il\t$6,-1"), "match")
        self.assertEqual(category(encode("il", RT=6, I16=-1), "il\t$6,65534"), "operand_mismatch")
        # lqd: 10 bits scaled by 16, so modulo 2**14.
        word = encode("lqd", RT=4, RA=1, I10x16=-16)
        self.assertEqual(category(word, "lqd\t$4,16368($1)"), "match")
        self.assertEqual(category(word, "lqd\t$4,-16($1)"), "match")
        self.assertEqual(category(word, "lqd\t$4,-16($2)"), "operand_mismatch")
        self.assertEqual(category(word, "lqd\t$4,-32($1)"), "operand_mismatch")
        # ai: signed 10 bits, so -32 and 992 are the same bits.
        self.assertEqual(category(encode("ai", RT=1, RA=1, I10=-32), "ai\t$1,$1,992"), "match")

    def test_rule5_targets_drop_the_symbol_and_wrap(self):
        word = encode("br", addr=0x3000, target=0x3040)
        self.assertEqual(category(word, "br\t0x3040 <label+0x40>"), "match")
        self.assertEqual(category(word, "br\t0x43040"), "match")
        self.assertEqual(category(word, "br\t0x3044"), "operand_mismatch")
        self.assertEqual(canon_text("brz\t$3,0x3040 <f+4>").operands, (Operand("reg", 3), Operand("target", 0x3040)))

    def test_rule6_flag_suffixes(self):
        self.assertEqual(canon_text("bie\t$0"), CanonInsn("bi", frozenset("e"), (Operand("reg", 0),)))
        self.assertEqual(canon_text("bid\t$5").flags, frozenset("d"))
        self.assertEqual(canon_text("hbrp\t0x303c,$0").mnemonic, "hbr")
        self.assertEqual(canon_text("hbrp\t0x303c,$0").flags, frozenset("p"))
        self.assertEqual(canon_text("syncc").flags, frozenset("c"))
        self.assertEqual(canon_text("binze\t$3,$4").mnemonic, "binz")
        self.assertEqual(canon_text("bisl\t$0,$3").flags, frozenset())
        # `bisled` is an instruction, not `bisl` with two flags.
        self.assertEqual(canon_text("bisled\t$0,$3"), CanonInsn("bisled", frozenset(), (Operand("reg", 0), Operand("reg", 3))))
        self.assertEqual(canon_text("bi\t$0").flags, frozenset())
        self.assertEqual(category(encode("bi", RA=0, E=1), "bie\t$0"), "match")
        self.assertEqual(category(encode("bi", RA=5, D=1), "bid\t$5"), "match")
        self.assertEqual(category(encode("bi", RA=5, D=1), "bie\t$5"), "mnemonic_mismatch")
        self.assertEqual(category(encode("bi", RA=5), "bie\t$5"), "mnemonic_mismatch")

    def test_rule7_undecoded_words(self):
        for text in (".long 0x90000000", ".word 0x9000", "(bad)", ".long"):
            self.assertEqual(canon_text(text), CanonInsn(".long", frozenset(), ()), text)

    def test_halts_drop_objdumps_false_target(self):
        # ISA 1.2 p.151: `heqi ra,symbol`; RT is a false target that objdump prints first.
        for name, ours_text, kw in (("heq", "heq $5,$6", {"RB": 6}), ("heqi", "heqi $5,-2", {"I10": -2}),
                                    ("hgt", "hgt $5,$6", {"RB": 6}), ("hgti", "hgti $5,7", {"I10": 7}),
                                    ("hlgt", "hlgt $5,$6", {"RB": 6}), ("hlgti", "hlgti $5,7", {"I10": 7})):
            word = encode(name, RA=5, **kw) | 9  # RT = 9
            self.assertEqual(format_insn(decode(word)), ours_text)
            with self.subTest(name):
                self.assertEqual(category(word, "%s	$9,%s" % (name, ours_text.split(" ", 1)[1])), "match")
                self.assertEqual(category(word, ours_text.replace(" ", "	", 1)), "match")
                # Only a register may be the extra operand, and the real operands still have to agree.
                self.assertEqual(category(word, "%s	$9,$4,%s" % (name, ours_text.split(",", 1)[1])),
                                 "operand_mismatch")
                self.assertEqual(category(word, "%s	7,%s" % (name, ours_text.split(" ", 1)[1])),
                                 "operand_mismatch")
        # Not a halt: an extra operand stays a mismatch.
        self.assertEqual(category(encode("ai", RT=1, RA=1, I10=-32), "ai	$9,$1,$1,-32"), "operand_mismatch")

    def test_zero_displacement_target(self):
        # ISA 1.2 p.174: target = PC + I16*4, so a zero field is a branch to the instruction itself;
        # objdump prints it as 0.
        for name, kw in (("br", {}), ("brz", {"RT": 3}), ("brnz", {"RT": 3}), ("brhz", {"RT": 3}),
                         ("brhnz", {"RT": 3}), ("hbrr", {"site": 0x3008})):
            word = encode(name, addr=0x3000, target=0x3000, **kw)
            ours = format_insn(decode(word, 0x3000))
            self.assertIn("0x03000", ours)
            theirs = ours.replace("0x03000", "0").replace(" ", "	", 1)
            with self.subTest(name):
                self.assertEqual(category(word, theirs), "match")
                # A real target that is printed as 0 elsewhere in the field is still a mismatch.
                self.assertEqual(category(word, ours.replace("0x03000", "0x03004").replace(" ", "	", 1)),
                                 "operand_mismatch")
        # A non-zero displacement never matches a printed 0, and brsl is not covered by the rule.
        word = encode("br", addr=0x3000, target=0x3040)
        self.assertEqual(category(word, "br	0"), "operand_mismatch")
        word = encode("brsl", RT=0, addr=0x3000, target=0x3000)
        self.assertEqual(category(word, "brsl	$0,0"), "operand_mismatch")

    def test_spr_token_accepts_sp_and_spr(self):
        self.assertEqual(canon_text("mfspr	$50,$sp4").operands, (Operand("reg", 50), Operand("spr", 4)))
        self.assertEqual(canon_text("mtspr	$spr4,$50"), canon_text("mtspr	$sp4,$50"))
        word = encode("mfspr", RT=50, SA=4)
        self.assertEqual(format_insn(decode(word)), "mfspr $50,$spr4")
        self.assertEqual(category(word, "mfspr	$50,$sp4"), "match")
        self.assertEqual(category(word, "mfspr	$50,$sp5"), "operand_mismatch")
        self.assertEqual(category(encode("mtspr", SA=4, RT=50), "mtspr	$sp4,$50"), "match")

    def test_conversion_scale_is_the_isa_operand(self):
        # ISA 1.2 p.220-223: scale = 173 - I8 (cflts, cfltu) or 155 - I8 (csflt, cuflt).
        self.assertEqual(category(encode("cflts", RT=78, RA=13, I8=173), "cflts	$78,$13,0"), "match")
        self.assertEqual(category(encode("cflts", RT=78, RA=13, I8=173), "cflts	$78,$13,173"), "operand_mismatch")
        self.assertEqual(category(encode("cfltu", RT=30, RA=40, I8=172), "cfltu	$30,$40,1"), "match")
        self.assertEqual(category(encode("csflt", RT=8, RA=4, I8=155), "csflt	$8,$4,0"), "match")
        self.assertEqual(category(encode("cuflt", RT=54, RA=61, I8=150), "cuflt	$54,$61,5"), "match")

    def test_stop_drops_its_code(self):
        self.assertEqual(canon_text("stop 0x2001"), canon_text("stop"))
        self.assertEqual(category(encode("stop", STOP=0x2001), "stop"), "match")
        self.assertEqual(category(0x00000000, "stop"), "match")

    def test_unknown_operand_text_is_kept(self):
        self.assertEqual(canon_text("foo\t$x").operands, (Operand("raw", 0, text="$x"),))

    def test_documented_lines_agree(self):
        for addr, word, text in DOCUMENTED:
            self.assertEqual(category(word, text, addr), "both_unknown" if word == 0x90000000 else "match",
                             "%x: %s" % (addr, text))


class CategoryTest(unittest.TestCase):
    IL = 0x40800003  # il $3,0

    def test_match(self):
        self.assertEqual(category(self.IL, "il\t$3,0"), "match")

    def test_objdump_code_we_unknown(self):
        self.assertEqual(category(0x90000000, "il\t$0,0"), "objdump_code_we_unknown")

    def test_we_code_objdump_unknown(self):
        self.assertEqual(category(self.IL, ".long 0x40800003"), "we_code_objdump_unknown")
        self.assertEqual(category(self.IL, "(bad)"), "we_code_objdump_unknown")

    def test_mnemonic_mismatch(self):
        self.assertEqual(category(self.IL, "ila\t$3,0"), "mnemonic_mismatch")

    def test_operand_mismatch(self):
        self.assertEqual(category(self.IL, "il\t$4,0"), "operand_mismatch")
        self.assertEqual(category(self.IL, "il\t$3"), "operand_mismatch")
        self.assertEqual(category(self.IL, "il\t$3,0,$1"), "operand_mismatch")

    def test_both_unknown(self):
        self.assertEqual(category(0x90000000, ".long 0x90000000"), "both_unknown")

    def test_known_difference_turns_a_mismatch_into_known(self):
        known = (KnownDifference("mnemonic_mismatch", "il", "ila", "both spellings are valid (test)"),)
        self.assertEqual(category(self.IL, "ila\t$3,0", known=known), "known_difference")
        # The entry names the category and both mnemonics; a different pair stays a disagreement.
        self.assertEqual(category(self.IL, "ilh\t$3,0", known=known), "mnemonic_mismatch")
        self.assertEqual(category(self.IL, "il\t$4,0", known=known), "operand_mismatch")
        known = (KnownDifference("objdump_code_we_unknown", None, "il", "test"),)
        self.assertEqual(category(0x90000000, "il\t$0,0", known=known), "known_difference")

    def test_known_differences_cite_the_isa(self):
        pairs = {(e.category, e.ours, e.objdump) for e in spu_objdump_diff.KNOWN_DIFFERENCES}
        self.assertEqual(pairs, {("mnemonic_mismatch", "bihnz", "bihnzp"), ("mnemonic_mismatch", "bihnz", "bihnzpd"),
                                 *(("mnemonic_mismatch", "sync", "sync" + f)
                                   for f in ("d", "e", "de", "cd", "ce", "cde"))})
        for entry in spu_objdump_diff.KNOWN_DIFFERENCES:
            self.assertIn("ISA 1.2 p.", entry.reason)

    def test_reserved_bit_entries(self):
        known = spu_objdump_diff.KNOWN_DIFFERENCES
        # Bit 11 (P in objdump) of bihnz is reserved: ISA 1.2 p.186-189.
        word = encode("bihnz", RT=37, RA=80) | 1 << 20
        self.assertEqual(format_insn(decode(word)), "bihnz $37,$80")
        self.assertEqual(category(word, "bihnzp	$37,$80", known=known), "known_difference")
        self.assertEqual(category(word, "bihnzp	$37,$81", known=known), "mnemonic_mismatch")
        self.assertEqual(category(word, "bihnzp	$37,$80", known=()), "mnemonic_mismatch")
        word = encode("bihnz", RT=32, RA=82, D=1) | 1 << 20
        self.assertEqual(category(word, "bihnzpd	$32,$82", known=known), "known_difference")
        # Reserved D/E bits of sync: ISA 1.2 p.242 defines only C.
        for bits, text in ((1 << 19, "syncd"), (1 << 18, "synce"), (3 << 18, "syncde"),
                           (1 << 20 | 1 << 19, "synccd"), (1 << 20 | 1 << 18, "syncce"), (7 << 18, "synccde")):
            with self.subTest(text):
                word = encode("sync") | bits
                self.assertEqual(category(word, text, known=known), "known_difference")
                self.assertEqual(category(word, text, known=()), "mnemonic_mismatch")
        # Other pairs, and a different mnemonic of the same family, are not covered.
        self.assertEqual(category(encode("sync"), "syncp", known=known), "mnemonic_mismatch")
        self.assertEqual(category(encode("bihz", RT=37, RA=80), "bihzp	$37,$80", known=known), "mnemonic_mismatch")


class InvocationTest(unittest.TestCase):
    def completed(self, stdout=b"", stderr=b"", returncode=0):
        return subprocess.CompletedProcess([], returncode, stdout, stderr)

    def test_argument_list_with_the_default_distro(self):
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=self.completed(b"text")) as run:
            text = spu_objdump.run_objdump(b"\x40\x80\x00\x03", 0x3000, "/opt/x/objdump")
        self.assertEqual(text, "text")
        run.assert_called_once_with(
            ["wsl.exe", "-d", "Ubuntu-26.04", "-e", "sh", "-c", spu_objdump.RUN_SCRIPT, "sh", "/opt/x/objdump", "0x3000"],
            input=b"\x40\x80\x00\x03", capture_output=True, timeout=300)

    def test_argument_list_with_a_named_distro_and_default_path(self):
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=self.completed()) as run:
            spu_objdump.run_objdump(b"\0\0\0\0", 0xA00, "", "Other-Distro")
        self.assertEqual(run.call_args.args[0],
                         ["wsl.exe", "-d", "Other-Distro", "-e", "sh", "-c", spu_objdump.RUN_SCRIPT, "sh", "", "0xa00"])

    def test_argument_list_without_a_distro(self):
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=self.completed()) as run:
            spu_objdump.run_objdump(b"\0\0\0\0", 0, "", None)
        self.assertEqual(run.call_args.args[0],
                         ["wsl.exe", "-e", "sh", "-c", spu_objdump.RUN_SCRIPT, "sh", "", "0x0"])

    def test_script_is_a_constant_with_the_documented_flags(self):
        script = spu_objdump.RUN_SCRIPT
        self.assertIn("-D -z -b binary -m spu", script)
        self.assertIn('--adjust-vma="$2"', script)
        self.assertNotIn("-EB", script)
        self.assertIn("mktemp", script)
        self.assertIn('rm -f "$f"', script)
        self.assertIn("exit $s", script)

    def test_objdump_path_resolution(self):
        self.assertEqual(spu_objdump.resolve_objdump("/a", {"TOX2_SPU_OBJDUMP": "/b"}), "/a")
        self.assertEqual(spu_objdump.resolve_objdump(None, {"TOX2_SPU_OBJDUMP": "/b"}), "/b")
        self.assertEqual(spu_objdump.resolve_objdump(None, {}), "")

    def test_version_line(self):
        out = b"GNU objdump (GNU Binutils) 2.47.20260726\nCopyright\n"
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=self.completed(out)) as run:
            self.assertEqual(spu_objdump.objdump_version("/o"), "GNU objdump (GNU Binutils) 2.47.20260726")
        self.assertEqual(run.call_args.args[0][-2:], ["sh", "/o"])

    def test_missing_wsl(self):
        with mock.patch.object(spu_objdump.subprocess, "run", side_effect=FileNotFoundError(2, "no such file")):
            with self.assertRaises(ObjdumpUnavailable) as ctx:
                spu_objdump.run_objdump(b"", 0)
        self.assertEqual(ctx.exception.code, 6)
        self.assertIn("objdump not available:", str(ctx.exception))
        self.assertIn("wsl.exe -e bash -s < tools/build-spu-objdump.sh", str(ctx.exception))

    def test_status_127_is_unavailable(self):
        done = self.completed(stderr=b"sh: 1: /x: not found\n", returncode=127)
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=done):
            with self.assertRaisesRegex(ObjdumpUnavailable, "not found") as ctx:
                spu_objdump.run_objdump(b"", 0)
        self.assertEqual(ctx.exception.code, 6)

    def test_other_failures_are_tool_errors(self):
        done = self.completed(stderr=b"boom", returncode=1)
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=done):
            with self.assertRaisesRegex(ListingError, "status 1: boom") as ctx:
                spu_objdump.run_objdump(b"", 0)
        self.assertEqual(ctx.exception.code, 1)
        with mock.patch.object(spu_objdump.subprocess, "run", side_effect=subprocess.TimeoutExpired("wsl", 300)):
            with self.assertRaisesRegex(ListingError, "did not finish"):
                spu_objdump.run_objdump(b"", 0)


ELF_CODE = [
    encode("il", RT=3, I16=0),
    encode("ai", RT=1, RA=1, I10=-32),
    encode("stqd", RT=0, RA=1, I10x16=16),
    encode("lqd", RT=3, RA=1, I10x16=32),
    encode("br", addr=0x3010, target=0x3000),
    encode("brsl", RT=0, addr=0x3014, target=0x3018),
    encode("stop", STOP=1),
    encode("lnop"),
]
RAW_CODE = [encode("il", RT=4, I16=-1), encode("wrch", CA=28, RT=4), 0x90000000, encode("bi", RA=0, E=1)]
RAW_CODE += [encode("lnop")] * (base.RAW_SIZE // 4 - len(RAW_CODE))
PIC_CODE = [encode("br", addr=0, target=8), encode("lnop"), encode("hbra", addr=8, site=0xC, target=0x20),
            encode("fsmbi", RT=4, U16=0xF0F0)]
PIC_CODE += [encode("lnop")] * (base.PIC_SIZE // 4 - len(PIC_CODE))


def objdump_text(word: int, addr: int) -> str:
    text = format_insn(decode(word, addr))
    return text if text.startswith(".long") else text.replace(" ", "\t", 1)


def make_listing(ls: int, words: list[int]) -> str:
    return listing(ls, [(w, objdump_text(w, ls + 4 * i)) for i, w in enumerate(words)])


class CommandTest(base.SpuFixtureTest):
    """`spu-objdump-diff` on synthetic containers; the listings come from `--listing-dir`."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        elf = build_elf32_be(entry=0x3000, machine=23, segments=[
            FixtureSegment(1, 0x3000, words_to_bytes(ELF_CODE), flags=5),
            FixtureSegment(1, 0x3100, bytes(16), memsz=0x40, flags=6),
        ])
        self.eboot = base.container_elf({base.ELF_AT: elf})
        self.libsre = base.container_elf({base.RAW_AT: words_to_bytes(RAW_CODE),
                                          base.PIC_AT: words_to_bytes(PIC_CODE)})
        self.eboot_path = self.write("EBOOT.elf", self.eboot)
        self.libsre_path = self.write("libsre.reloc.elf", self.libsre)
        self.config_path = self.write_json("target.json", {
            "eboot_elf": {"sha256": base.sha(self.eboot)},
            "libsre_reloc_elf": {"sha256": base.sha(self.libsre)},
        })
        self.registry_path = self.write_json("registry.json", base.registry_doc(
            base.elf_entry(), base.raw_entry(entry="0xa00"), base.pic_entry()))
        self.listing_dir = self.tmp / "listings"
        self.listing_dir.mkdir()
        self.write_listing("t_elf_03000.txt", make_listing(0x3000, ELF_CODE))
        self.write_listing("t_raw_00a00.txt", make_listing(0xA00, RAW_CODE))
        self.write_listing("t_pic_00000.txt", make_listing(0, PIC_CODE))
        self.report_path = self.tmp / "report.json"

    def write_listing(self, name: str, text: str) -> None:
        (self.listing_dir / name).write_text(text, encoding="utf-8", newline="\n")

    def run_diff(self, *extra, listing_dir=True):
        argv = ["spu-objdump-diff", "--eboot", str(self.eboot_path), "--libsre", str(self.libsre_path),
                "--registry", str(self.registry_path), "--target-config", str(self.config_path),
                "--report", str(self.report_path), *extra]
        if listing_dir:
            argv += ["--listing-dir", str(self.listing_dir)]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ps3tool.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_clean_run(self):
        code, out, err = self.run_diff()
        self.assertEqual((code, err), (0, ""))
        lines = out.splitlines()
        self.assertEqual(lines[0], "t_elf match 8 known_difference 0 objdump_code_we_unknown 0 "
                                   "we_code_objdump_unknown 0 mnemonic_mismatch 0 operand_mismatch 0 both_unknown 0")
        self.assertEqual(lines[1], "t_raw match 15 known_difference 0 objdump_code_we_unknown 0 "
                                   "we_code_objdump_unknown 0 mnemonic_mismatch 0 operand_mismatch 0 both_unknown 1")
        self.assertTrue(lines[2].startswith("t_pic match 8 "))
        self.assertEqual(lines[3:], ["gate: ok"])
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        self.assertIsNone(report["objdump_version"])
        self.assertEqual([i["name"] for i in report["images"]], ["t_elf", "t_raw", "t_pic"])
        self.assertEqual(report["images"][1]["triples"][0]["category"], "both_unknown")
        self.assertEqual(report["images"][1]["segments"][0]["ls"], "0xa00")

    def test_gate_image_mismatch_exits_9(self):
        words = list(RAW_CODE)
        text = make_listing(0xA00, words).replace("wrch\t$ch28,$4", "wrch\t$ch29,$4")
        self.write_listing("t_raw_00a00.txt", text)
        code, out, err = self.run_diff()
        self.assertEqual((code, err), (9, ""))
        self.assertIn("t_raw match 14 known_difference 0 objdump_code_we_unknown 0 we_code_objdump_unknown 0 "
                      "mnemonic_mismatch 0 operand_mismatch 1 both_unknown 1", out)
        self.assertEqual(out.splitlines()[-1], "gate: 1 unexplained disagreements in t_raw")
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        (triple,) = [t for t in report["images"][1]["triples"] if t["category"] == "operand_mismatch"]
        self.assertEqual((triple["ours"], triple["objdump"], triple["count"]), ("wrch", "wrch", 1))
        self.assertEqual(triple["examples"], [{"ls": "0x00a04", "word": "0x%08x" % RAW_CODE[1],
                                               "ours": "wrch $ch28,$4", "objdump": "wrch $ch29,$4"}])

    def test_ungated_disagreement_does_not_fail(self):
        text = make_listing(0x3000, ELF_CODE).replace("ai\t$1,$1,-32", "ai\t$1,$1,-31")
        self.write_listing("t_elf_03000.txt", text)
        code, out, _ = self.run_diff()
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertEqual(lines[-2:], ["ungated disagreement: t_elf operand_mismatch 1", "gate: ok"])

    def test_images_option_selects(self):
        code, out, _ = self.run_diff("--images", "t_pic")
        self.assertEqual(code, 0)
        self.assertEqual(len(out.splitlines()), 2)

    def test_position_independent_blob_is_fed_at_ls_0(self):
        text = make_listing(0, PIC_CODE)
        self.assertIn("    8:\t", text)
        code, out, _ = self.run_diff("--images", "t_pic")
        self.assertEqual(code, 0)
        self.assertIn("t_pic match 8 ", out)

    def test_out_of_step_listing(self):
        text = make_listing(0xA00, RAW_CODE)
        text = "\n".join(l for l in text.splitlines() if not l.startswith("    a08:")) + "\n"
        self.write_listing("t_raw_00a00.txt", text)
        code, out, err = self.run_diff()
        self.assertEqual(code, 1)
        self.assertIn("listing out of step at ls 0x00a08", err)

    def test_missing_listing_file(self):
        (self.listing_dir / "t_pic_00000.txt").unlink()
        code, _, err = self.run_diff()
        self.assertEqual(code, 1)
        self.assertIn("cannot read listing", err)

    def test_container_hash_mismatch(self):
        self.write("EBOOT.elf", self.eboot + b"\0")
        code, _, err = self.run_diff()
        self.assertEqual(code, 4)

    def test_unknown_image_is_a_usage_error(self):
        code, _, err = self.run_diff("--images", "nope")
        self.assertEqual(code, 2)
        self.assertIn("unknown image nope", err)

    def test_bad_option(self):
        code, _, _ = self.run_diff("--bogus")
        self.assertEqual(code, 2)

    def test_missing_wsl_exits_6(self):
        with mock.patch.object(spu_objdump.subprocess, "run", side_effect=FileNotFoundError(2, "not found")):
            code, out, err = self.run_diff(listing_dir=False)
        self.assertEqual((code, out), (6, ""))
        self.assertTrue(err.startswith("spu-objdump-diff: objdump not available: "), err)
        self.assertIn("build it with: wsl.exe -e bash -s < tools/build-spu-objdump.sh", err)

    def test_status_127_exits_6(self):
        done = subprocess.CompletedProcess([], 127, b"", b"sh: not found\n")
        with mock.patch.object(spu_objdump.subprocess, "run", return_value=done):
            code, _, err = self.run_diff(listing_dir=False)
        self.assertEqual(code, 6)
        self.assertIn("sh: not found", err)

    def test_run_mode_feeds_each_segment_and_saves_listings(self):
        listings = {
            b"".join(struct.pack(">I", w) for w in ELF_CODE): make_listing(0x3000, ELF_CODE),
            words_to_bytes(RAW_CODE): make_listing(0xA00, RAW_CODE),
            words_to_bytes(PIC_CODE): make_listing(0, PIC_CODE),
        }

        def fake_run(command, *, input, capture_output, timeout):
            if spu_objdump.VERSION_SCRIPT in command:
                return subprocess.CompletedProcess(command, 0, b"GNU objdump (GNU Binutils) 2.47.1\n", b"")
            return subprocess.CompletedProcess(command, 0, listings[input].encode("utf-8"), b"")

        saved = self.tmp / "saved"
        with mock.patch.object(spu_objdump.subprocess, "run", side_effect=fake_run) as run, \
                mock.patch.object(spu_objdump_diff, "DEFAULT_LISTING_DIR", saved):
            code, out, err = self.run_diff("--objdump", "/o/objdump", "--wsl-distro", "D", listing_dir=False)
        self.assertEqual((code, err), (0, ""))
        commands = [c.args[0] for c in run.call_args_list]
        self.assertEqual(commands[0], ["wsl.exe", "-d", "D", "-e", "sh", "-c", spu_objdump.VERSION_SCRIPT,
                                       "sh", "/o/objdump"])
        self.assertEqual([c[-1] for c in commands[1:]], ["0x3000", "0xa00", "0x0"])
        self.assertEqual({c[-2] for c in commands[1:]}, {"/o/objdump"})
        self.assertEqual(sorted(p.name for p in saved.iterdir()),
                         ["t_elf_03000.txt", "t_pic_00000.txt", "t_raw_00a00.txt"])
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["objdump_version"], "GNU objdump (GNU Binutils) 2.47.1")

    def test_examples_limit(self):
        text = make_listing(0x3000, ELF_CODE).replace("lnop", "nop\t$1")
        self.write_listing("t_elf_03000.txt", text)
        self.run_diff("--examples", "0")
        report = json.loads(self.report_path.read_text(encoding="utf-8"))
        (triple,) = report["images"][0]["triples"]
        self.assertEqual((triple["count"], triple["examples"]), (1, []))


if __name__ == "__main__":
    unittest.main()

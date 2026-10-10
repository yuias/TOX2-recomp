"""Tests for PPU instruction formatting and simplified mnemonics."""

import unittest

from ps3recomp.ppu import decode
from ps3recomp.ppu_format import format_insn

SAME = None

# (word, addr, raw text, simplified text or SAME), the encodings listed in the spec.
CASES = [
    (0x7C0802A6, 0x10000, "mfspr r0,8", "mflr r0"),
    (0x7C0803A6, 0x10000, "mtspr 8,r0", "mtlr r0"),
    (0x7D8903A6, 0x10000, "mtspr 9,r12", "mtctr r12"),
    (0x4E800020, 0x10000, "bclr 20,0", "blr"),
    (0x4E800420, 0x10000, "bcctr 20,0", "bctr"),
    (0x4E800421, 0x10000, "bcctrl 20,0", "bctrl"),
    (0x60000000, 0x10000, "ori r0,r0,0x0", "nop"),
    (0x38600000, 0x10000, "addi r3,r0,0x0", "li r3,0x0"),
    (0x3C600001, 0x10000, "addis r3,r0,0x1", "lis r3,0x1"),
    (0x38210070, 0x10000, "addi r1,r1,0x70", SAME),
    (0xF821FF91, 0x10000, "stdu r1,-0x70(r1)", SAME),
    (0xFBE1FFF8, 0x10000, "std r31,-0x8(r1)", SAME),
    (0xE8010080, 0x10000, "ld r0,0x80(r1)", SAME),
    (0x90010008, 0x10000, "stw r0,0x8(r1)", SAME),
    (0x80630000, 0x10000, "lwz r3,0x0(r3)", SAME),
    (0x44000002, 0x10000, "sc", SAME),
    (0x7C7F1B78, 0x10000, "or r31,r3,r3", "mr r31,r3"),
    (0x7C642A15, 0x10000, "add. r3,r4,r5", SAME),
    (0x7C642E14, 0x10000, "addo r3,r4,r5", SAME),
    (0x7C000026, 0x10000, "mfcr r0", SAME),
    (0x7C102026, 0xA0068, "mfocrf r0,cr6", SAME),
    (0x7D908120, 0x95C5C, "mtocrf 8,r12", SAME),
    (0x7C0004AC, 0x10000, "sync 0", "sync"),
    (0x7C2004AC, 0x10000, "sync 1", "lwsync"),
    (0x4C00012C, 0x10000, "isync", SAME),
    (0x7C6307B4, 0x10000, "extsw r3,r3", SAME),
    (0x78630020, 0x10000, "rldicl r3,r3,0,32", "clrldi r3,r3,32"),
    (0x788345E4, 0x10000, "rldicr r3,r4,8,55", "sldi r3,r4,8"),
    (0x5483103A, 0x10000, "rlwinm r3,r4,2,0,29", "slwi r3,r4,2"),
    (0x7C831E74, 0x10000, "sradi r3,r4,3", SAME),
    (0x7C60212D, 0x10000, "stwcx. r3,r0,r4", SAME),
    (0x2C030000, 0x10000, "cmpi cr0,0,r3,0x0", "cmpwi r3,0x0"),
    (0x2FA30000, 0x10000, "cmpi cr7,1,r3,0x0", "cmpdi cr7,r3,0x0"),
    (0x4182000C, 0x10000, "bc 12,2,0x1000c", "beq 0x1000c"),
    (0x4200FFF8, 0x10008, "bc 16,0,0x10000", "bdnz 0x10000"),
    (0x4BFFFFFC, 0x10000, "b 0xfffc", SAME),
    (0x48000011, 0x10000, "bl 0x10010", SAME),
    (0xFC000890, 0x10000, "fmr f0,f1", SAME),
    (0xEC21102A, 0x10000, "fadds f1,f1,f2", SAME),
    (0xFC2220FA, 0x10000, "fmadd f1,f2,f3,f4", SAME),
    (0x7C0018CE, 0x10000, "lvx v0,r0,r3", SAME),
    (0x7C0019CE, 0x10000, "stvx v0,r0,r3", SAME),
    (0x7C03240E, 0x10000, "lvlx v0,r3,r4", SAME),
    (0x10221484, 0x10000, "vor v1,v2,v2", "vmr v1,v2"),
    (0x100004C4, 0x10000, "vxor v0,v0,v0", SAME),
    (0x1022192B, 0x10000, "vperm v1,v2,v3,v4", SAME),
    (0x102220EE, 0x10000, "vmaddfp v1,v2,v3,v4", SAME),
    (0x1022192C, 0x10000, "vsldoi v1,v2,v3,4", SAME),
    (0x10011486, 0x10000, "vcmpequw. v0,v1,v2", SAME),
    (0x101F038C, 0x10000, "vspltisw v0,-1", SAME),
    (0x00000000, 0x10000, ".long 0x00000000", SAME),
]


class AppendixBTest(unittest.TestCase):
    def test_raw_and_simplified(self):
        for word, addr, raw, simple in CASES:
            with self.subTest(word=f"{word:08x}"):
                insn = decode(word, addr)
                self.assertEqual(format_insn(insn, simplify=False), raw)
                self.assertEqual(format_insn(insn), raw if simple is SAME else simple)


class SimplifyRulesTest(unittest.TestCase):
    def check(self, word, expected, addr=0x10000):
        self.assertEqual(format_insn(decode(word, addr)), expected)

    def test_rlwinm_rule_order(self):
        # MB=0, ME=31, SH=0 hits rotlwi first.
        self.check(0x54830000 | 31 << 1, "rotlwi r3,r4,0")
        # SH=32-MB, ME=31, MB>0 -> srwi
        self.check(0x54830000 | 28 << 11 | 4 << 6 | 31 << 1, "srwi r3,r4,4")
        # SH=0, MB=5, ME=31 -> clrlwi
        self.check(0x54830000 | 5 << 6 | 31 << 1, "clrlwi r3,r4,5")
        # no rule -> raw
        self.check(0x54830000 | 1 << 11 | 3 << 6 | 7 << 1, "rlwinm r3,r4,1,3,7")

    def test_record_forms_keep_dot(self):
        self.check(0x7C7F1B79, "mr. r31,r3")
        self.check(0x7C6318F9, "not. r3,r3")
        self.check(0x54831001 | 29 << 1, "slwi. r3,r4,2")

    def test_rldicl_rules(self):
        # sh=8, mb=56 -> srdi 56
        self.check(0x78830000 | 8 << 11 | (56 & 31) << 6 | 1 << 5, "srdi r3,r4,56")
        # sh=4, mb=0 -> rotldi 4
        self.check(0x78830000 | 4 << 11, "rotldi r3,r4,4")
        # sh=4, mb=8 -> raw
        self.check(0x78830000 | 4 << 11 | 8 << 6, "rldicl r3,r4,4,8")

    def test_conditional_branches(self):
        self.check(0x4082000C, "bne 0x1000c")
        self.check(0x4181000C, "bgt 0x1000c")
        self.check(0x4086000C, "bne cr1,0x1000c")
        self.check(0x41800010, "blt 0x10010")
        self.check(0x4D820020, "beqlr")
        self.check(0x4D860021, "beqlrl cr1")
        self.check(0x4D820421, "beqctrl")
        self.check(0x4200000C, "bdnz 0x1000c")
        self.check(0x4240000C, "bdz 0x1000c")
        self.check(0x4200000D, "bdnzl 0x1000c")

    def test_absolute_branch_target(self):
        self.check(0x48000003, "bla 0x0")
        self.check(0x48002002, "ba 0x2000")

    def test_compare_variants(self):
        self.check(0x28030001, "cmplwi r3,0x1")
        self.check(0x2BA30001, "cmpldi cr7,r3,0x1")
        self.check(0x7C032000, "cmpw r3,r4")
        self.check(0x7FA32040, "cmpld cr7,r3,r4")
        self.check(0x7C032040, "cmplw r3,r4")
        self.check(0x7FA42000, "cmpd cr7,r4,r4")

    def test_unsimplified_fall_back_to_raw(self):
        self.check(0x7C0902A6, "mfctr r0")
        self.check(0x7C0A02A6, "mfspr r0,10")
        self.check(0x7C0FF120, "mtcr r0")
        self.check(0x7C0FE120, "mtcrf 254,r0")
        # Bit 11 set selects the single-field forms; they never become mtcr.
        self.check(0x7C1FF120, "mtocrf 255,r0")
        self.check(0x7C180026, "mfocrf r0,cr0")
        self.check(0x7C1FF026, "mfocrf r0,255")
        self.check(0x7C0FF026, "mfcr r0")
        self.check(0x7C4004AC, "sync 2")
        self.check(0x4C000020 | 2 << 21, "bclr 2,0")

    def test_memory_operand_negative_and_zero_base(self):
        self.check(0x8061FFF0, "lwz r3,-0x10(r1)")
        self.check(0x90600000, "stw r3,0x0(r0)")

    def test_vor_with_distinct_sources_stays_raw(self):
        self.check(0x10221C84, "vor v1,v2,v3")


if __name__ == "__main__":
    unittest.main()

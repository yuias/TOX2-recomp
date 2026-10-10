"""Tests for SPU instruction formatting. Words are hand-derived from the ISA 1.2 instruction formats."""

import unittest

from ps3recomp.spu import ALL_OPS, decode
from ps3recomp.spu_format import format_insn, format_line
from tests import spu_asm

# (word, addr, text)
CASES = [
    (0x80614206, 0x100, "selb $3,$4,$5,$6"),
    (0x18014203, 0x100, "a $3,$4,$5"),
    (0x0F608203, 0x100, "shli $3,$4,2"),
    (0x0F3F4203, 0x100, "rotmi $3,$4,-3"),
    # Conversions print scale (ISA 1.2, p.220-223): 155 - I8 for csflt/cuflt, 173 - I8 for cflts/cfltu.
    (0x76A6C203, 0x100, "csflt $3,$4,0"),
    (0x76800203, 0x100, "csflt $3,$4,155"),
    (0x76C00203, 0x100, "cuflt $3,$4,155"),
    (0x76C40203, 0x100, "cuflt $3,$4,139"),
    (0x762B4203, 0x100, "cflts $3,$4,0"),
    (0x76004203, 0x100, "cflts $3,$4,172"),
    (0x766B4203, 0x100, "cfltu $3,$4,0"),
    (0x763FC203, 0x100, "cflts $3,$4,174"),  # I8 = 255: 173 - 255 wraps to 8 bits
    (0x1CF80081, 0x100, "ai $1,$1,-32"),
    (0x34004182, 0x100, "lqd $2,16($3)"),
    (0x24FFC080, 0x100, "stqd $0,-16($1)"),
    (0x40FFFF83, 0x100, "il $3,-1"),
    (0x41FFFF83, 0x100, "ilh $3,0xffff"),
    (0x60891A03, 0x100, "iohl $3,0x1234"),
    (0x4291A283, 0x100, "ila $3,0x12345"),
    (0x33000100, 0x100, "brsl $0,0x00108"),
    (0x327FFE00, 0x110, "br 0x00100"),
    (0x33800403, 0x200, "lqr $3,0x00220"),
    (0x35000000, 0x100, "bi $0"),
    (0x35200580, 0x100, "bisl $0,$11"),
    (0x35080180, 0x100, "bid $3"),
    (0x2524010B, 0x100, "binze $11,$2"),
    (0x40200000, 0x100, "nop $0"),
    (0x00200000, 0x100, "lnop"),
    (0x00400000, 0x100, "sync"),
    (0x00500000, 0x100, "syncc"),
    (0x00600000, 0x100, "dsync"),
    (0x00000101, 0x100, "stop 0x101"),
    (0x28014203, 0x100, "stopd $3,$4,$5"),
    (0x01A00683, 0x100, "rdch $3,$ch13"),
    (0x21A00A83, 0x100, "wrch $ch21,$3"),
    (0x7B014200, 0x100, "heq $4,$5"),
    (0x3E814203, 0x100, "cbd $3,5($4)"),
    (0x12802004, 0x100, "hbrr 0x00310,0x00200"),
    (0x10004004, 0x100, "hbra 0x00110,0x00200"),
    (0x35800284, 0x100, "hbr 0x00110,$5"),
    (0x90000000, 0x100, ".long 0x90000000"),
]


class FormatTest(unittest.TestCase):
    def test_cases(self):
        for word, addr, text in CASES:
            with self.subTest(word=f"{word:08x}", text=text):
                self.assertEqual(format_insn(decode(word, addr)), text)

    def test_listing_line(self):
        self.assertEqual(format_line(decode(0x35200580, 0x800)), "00800: 35200580  bisl $0,$11")

    def test_no_link_register_aliases(self):
        text = format_insn(decode(spu_asm.encode("bi", RA=0)))
        self.assertEqual(text, "bi $0")
        self.assertNotIn("lr", format_insn(decode(spu_asm.encode("ai", RT=1, RA=1, I10=-16))))

    def test_every_spec_formats(self):
        # Operand names the formatter does not know raise, so this covers the whole table.
        for spec in ALL_OPS:
            insn = decode(spu_asm.encode(spec.mnemonic), 0x2000)
            with self.subTest(mnemonic=spec.mnemonic):
                text = format_insn(insn)
                self.assertTrue(text.startswith(insn.mnemonic), text)
                self.assertEqual(len(text.split(" ", 1)) == 2, bool(spec.operands))
                self.assertNotIn(", ", text)


if __name__ == "__main__":
    unittest.main()

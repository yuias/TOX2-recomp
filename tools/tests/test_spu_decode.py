"""Tests for the SPU decoder tables and field extraction.

Hand-derived words come from the instruction formats and opcodes of the "Synergistic Processor Unit
Instruction Set Architecture" (version 1.2): chapter "Instruction formats" for the field layout and
the per-instruction pages for the opcodes. Each case notes the arithmetic.
"""

import unittest

from ps3recomp import spu
from ps3recomp.spu import ALL_OPS, FIELDS, SPECS_BY_NAME, decode, decode_range
from tests import spu_asm

# (word, addr, mnemonic, expected field values)
CASES = [
    # RRR: op 1000 (selb) | RT 4-10 | RB 11-17 | RA 18-24 | RC 25-31.
    # 0x8<<28 | 3<<21 | 5<<14 | 4<<7 | 6
    (0x80614206, 0x100, "selb", {"RT4": 3, "RB": 5, "RA": 4, "RC": 6}),
    # fma: op 1110 -> 0xE<<28 with the same operands.
    (0xE0614206, 0x100, "fma", {"RT4": 3, "RB": 5, "RA": 4, "RC": 6}),
    # RR: op 00011000000 (a, 0x0C0) | RB | RA | RT; 0xC0<<21 = 0x18000000.
    (0x18014203, 0x100, "a", {"RT": 3, "RA": 4, "RB": 5}),
    # RI7: shli = 0x07B<<21 = 0x0F600000, I7 = 2 in bits 11-17.
    (0x0F608203, 0x100, "shli", {"RT": 3, "RA": 4, "I7": 2}),
    # RI7 with a negative immediate: rotmi $3,$4,-3 (0x079<<21 = 0x0F200000; -3 & 0x7f = 0x7d).
    (0x0F3F4203, 0x100, "rotmi", {"RT": 3, "RA": 4, "I7": -3}),
    # RI8: csflt (10-bit op 0x1DA, 0x1DA<<22 = 0x76800000), I8 = 155 in bits 10-17.
    (0x76A6C203, 0x100, "csflt", {"RT": 3, "RA": 4, "I8": 155}),
    # RI10: ai $1,$1,-32 (0x1C<<24; -32 & 0x3ff = 0x3e0).
    (0x1CF80081, 0x100, "ai", {"RT": 1, "RA": 1, "I10": -32}),
    # RI10 memory forms scale the displacement by 16: lqd $2,16($3) has I10 = 1.
    (0x34004182, 0x100, "lqd", {"RT": 2, "RA": 3, "I10x16": 16}),
    (0x24FFC080, 0x100, "stqd", {"RT": 0, "RA": 1, "I10x16": -16}),
    # RI16: il $3,-1 (0x081<<23 = 0x40800000, I16 = 0xffff).
    (0x40FFFF83, 0x100, "il", {"RT": 3, "I16": -1}),
    # ilh keeps the halfword unsigned.
    (0x41FFFF83, 0x100, "ilh", {"RT": 3, "U16": 0xFFFF}),
    # brsl $0,+8 (0x066<<23 = 0x33000000; I16 = 2 words).
    (0x33000100, 0x100, "brsl", {"RT": 0, "REL16": 8}),
    # br -16: I16 = -4 (0xfffc).
    (0x327FFE00, 0x110, "br", {"REL16": -16}),
    # RI18: ila $3,0x12345 (0x21<<25 = 0x42000000).
    (0x4291A283, 0x100, "ila", {"RT": 3, "I18": 0x12345}),
    # Indirect branches (D = bit 12, E = bit 13): bi $0, bisl $0,$11, bi with D, binz with E.
    (0x35000000, 0x100, "bi", {"RA": 0, "D": 0, "E": 0}),
    (0x35200580, 0x100, "bisl", {"RT": 0, "RA": 11, "D": 0, "E": 0}),
    (0x35080180, 0x100, "bi", {"RA": 3, "D": 1, "E": 0}),
    (0x2524010B, 0x100, "binz", {"RT": 11, "RA": 2, "D": 0, "E": 1}),
    # Special encodings: nop 0x201, lnop 0x001, sync 0x002 (C = bit 11), dsync 0x003, stop with a
    # 14-bit signal type.
    (0x40200000, 0x100, "nop", {"RT": 0}),
    (0x00200000, 0x100, "lnop", {}),
    (0x00400000, 0x100, "sync", {"C": 0}),
    (0x00500000, 0x100, "sync", {"C": 1}),
    (0x00600000, 0x100, "dsync", {}),
    (0x00000101, 0x100, "stop", {"STOP": 0x101}),
    (0x28014203, 0x100, "stopd", {"RC": 3, "RA": 4, "RB": 5}),
    # Channel forms put the channel number in the RA position: rdch $3,$ch13 and wrch $ch21,$3.
    (0x01A00683, 0x100, "rdch", {"RT": 3, "CA": 13}),
    (0x21A00A83, 0x100, "wrch", {"CA": 21, "RT": 3}),
    # Halts have no RT operand.
    (0x7B014200, 0x100, "heq", {"RA": 4, "RB": 5}),
    # Hints: hbrr at 0x100 with the branch at 0x310 (ROH 1, ROL 4 -> 132 words) and target 0x200.
    (0x12802004, 0x100, "hbrr", {"RO": 528, "REL16": 0x100}),
    (0x10004004, 0x100, "hbra", {"RO": 16, "ABS16": 0x200}),
    (0x35800284, 0x100, "hbr", {"RO_IND": 16, "RA": 5, "P": 0}),
    (0x73000003, 0x100, "fscrrd", {"RT": 3}),
]


class DecodeCaseTest(unittest.TestCase):
    def test_cases(self):
        for word, addr, mnemonic, expected in CASES:
            with self.subTest(word=f"{word:08x}", mnemonic=mnemonic):
                insn = decode(word, addr)
                self.assertIsNotNone(insn.spec)
                self.assertEqual(insn.spec.mnemonic, mnemonic)
                for name, value in expected.items():
                    self.assertEqual(insn.field(name), value, name)

    def test_cases_encode(self):
        for word, addr, mnemonic, expected in CASES:
            with self.subTest(word=f"{word:08x}", mnemonic=mnemonic):
                self.assertEqual(spu_asm.encode(mnemonic, **expected), word)

    def test_display_mnemonic_suffixes(self):
        self.assertEqual(decode(0x35080180).mnemonic, "bid")
        self.assertEqual(decode(0x2524010B).mnemonic, "binze")
        self.assertEqual(decode(0x00500000).mnemonic, "syncc")
        self.assertEqual(decode(0x35900284).mnemonic, "hbrp")
        self.assertEqual(decode(0x35000000).mnemonic, "bi")

    def test_unassigned_opcodes_are_long(self):
        # Top nibbles 1001 and 1010 are not RRR opcodes, and 11-bit opcode 0x004 is unassigned.
        for word in (0x90000000, 0xA0000000, 0x00800000):
            insn = decode(word, 0x40)
            with self.subTest(word=f"{word:08x}"):
                self.assertIsNone(insn.spec)
                self.assertIsNone(insn.mnemonic)
                self.assertEqual(insn.fields(), {})


class ImmediateTest(unittest.TestCase):
    def test_ri10_sign_extension(self):
        self.assertEqual(decode(spu_asm.encode("ai", I10=511)).field("I10"), 511)
        self.assertEqual(decode(spu_asm.encode("ai", I10=-512)).field("I10"), -512)
        self.assertEqual(decode(0x1C000000 | 0x200 << 14).field("I10"), -512)
        self.assertEqual(decode(0x1C000000 | 0x1FF << 14).field("I10"), 511)

    def test_ri16_sign_extension(self):
        self.assertEqual(decode(0x40000000 | 0x7FFF << 7).field("I16"), 0x7FFF)
        self.assertEqual(decode(0x40000000 | 0x8000 << 7).field("I16"), -0x8000)

    def test_memory_displacement_is_scaled_by_16(self):
        for count, expected in ((0, 0), (1, 16), (-1, -16), (511, 8176), (-512, -8192)):
            word = 0x34000000 | (count & 0x3FF) << 14
            self.assertEqual(decode(word).field("I10x16"), expected)
            self.assertEqual(spu_asm.encode("lqd", I10x16=expected), word)
        with self.assertRaises(ValueError):
            spu_asm.encode("lqd", I10x16=8)

    def test_branch_targets(self):
        # Relative targets wrap at the 256 KiB local store; absolute ones are the sign-extended
        # word address masked to 18 bits.
        self.assertEqual(decode(spu_asm.encode("br", REL16=-16), 0x110).branch_target, 0x100)
        self.assertEqual(decode(spu_asm.encode("brz", REL16=-16), 0x0).branch_target, 0x3FFF0)
        self.assertEqual(decode(spu_asm.encode("bra", ABS16=-4), 0x100).branch_target, 0x3FFFC)
        self.assertEqual(decode(spu_asm.encode("brasl", ABS16=0x1234 * 4), 0x100).branch_target, 0x48D0)
        self.assertEqual(decode(spu_asm.encode("lqr", REL16=0x20), 0x200).field("REL16"), 0x20)
        self.assertIsNone(decode(0x35000000).branch_target)

    def test_hint_addresses(self):
        hbrr = decode(0x12802004, 0x100)
        self.assertEqual((hbrr.hint_site, hbrr.hint_target), (0x310, 0x200))
        hbra = decode(0x10004004, 0x100)
        self.assertEqual((hbra.hint_site, hbra.hint_target), (0x110, 0x200))
        hbr = decode(0x35800284, 0x100)
        self.assertEqual((hbr.hint_site, hbr.hint_target), (0x110, None))
        self.assertIsNone(decode(0x35000000).hint_site)

    def test_encode_rejects_bad_input(self):
        for call in (
            lambda: spu_asm.encode("nosuch"),
            lambda: spu_asm.encode("a", I10=1),
            lambda: spu_asm.encode("a", RT=128),
            lambda: spu_asm.encode("ai", I10=512),
            lambda: spu_asm.encode("br", REL16=2),
            lambda: spu_asm.encode("a", D=1),
        ):
            with self.subTest():
                with self.assertRaises(ValueError):
                    call()

    def test_decode_range(self):
        data = bytes.fromhex("35000000" "40200000" "123456")
        insns = list(decode_range(data, 0x3000))
        self.assertEqual([(i.addr, i.mnemonic) for i in insns], [(0x3000, "bi"), (0x3004, "nop")])


def _sample_values(spec, variant):
    """Field values that are non-zero in every field, so a field at the wrong bit position fails."""
    values = {}
    for index, name in enumerate(spec.field_names):
        field = FIELDS[name]
        low, high = field.range
        step = 1 << field.scale
        if variant == 0:
            value = high
        elif field.signed:
            value = low + step * (index + 1)
        else:
            value = (high * (index + 2) // 7) // step * step or step
        values[name] = value
    for name in spec.flag_fields:
        values[name] = 1
    return values


class TableTest(unittest.TestCase):
    def test_every_spec_round_trips_with_every_field_set(self):
        for spec in ALL_OPS:
            for variant in (0, 1):
                values = _sample_values(spec, variant)
                with self.subTest(mnemonic=spec.mnemonic, variant=variant):
                    word = spu_asm.encode(spec.mnemonic, **values)
                    insn = decode(word, 0x1000)
                    self.assertIs(insn.spec, spec)
                    for name, value in values.items():
                        self.assertEqual(insn.field(name), value, name)

    def test_single_field_isolation(self):
        # Setting one field touches only its bits: the other fields stay 0.
        for spec in ALL_OPS:
            for name in spec.field_names + spec.flag_fields:
                field = FIELDS[name]
                value = field.range[1] if not field.signed else -(1 << field.scale)
                word = spu_asm.encode(spec.mnemonic, **{name: value})
                insn = decode(word)
                with self.subTest(mnemonic=spec.mnemonic, field=name):
                    self.assertIs(insn.spec, spec)
                    self.assertEqual(insn.field(name), value)
                    self.assertEqual(word & ~spec.mask & ~field.mask & 0xFFFFFFFF, 0)

    def test_opcodes_are_a_prefix_code(self):
        for a in ALL_OPS:
            for b in ALL_OPS:
                if a is b:
                    continue
                shorter = min(a.width, b.width)
                self.assertNotEqual(a.match >> (32 - shorter), b.match >> (32 - shorter),
                                    f"{a.mnemonic} / {b.mnemonic}")

    def test_mnemonics_are_unique_and_grouped(self):
        names = [spec.mnemonic for spec in ALL_OPS]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual({spec.group for spec in ALL_OPS}, set(spu.GROUPS))
        for spec in ALL_OPS:
            self.assertLessEqual(spec.flags, spu.FLOW_FLAGS)

    def test_isa_instruction_set_is_present(self):
        expected = """
        lqd lqx lqa lqr stqd stqx stqa stqr cbd cbx chd chx cwd cwx cdd cdx
        ilh ilhu il ila iohl fsmbi
        ah ahi a ai sfh sfhi sf sfi addx cg cgx sfx bg bgx
        mpy mpyu mpyi mpyui mpya mpyh mpys mpyhh mpyhha mpyhhu mpyhhau
        clz cntb fsmb fsmh fsm gbb gbh gb avgb absdb sumb xsbh xshw xswd
        and andc andbi andhi andi or orc orbi orhi ori orx xor xorbi xorhi xori nand nor eqv selb shufb
        shlh shlhi shl shli roth rothi rot roti rothm rothmi rotm rotmi rotmah rotmahi rotma rotmai
        shlqbi shlqbii shlqby shlqbyi shlqbybi rotqbi rotqbii rotqby rotqbyi rotqbybi
        rotqmbi rotqmbii rotqmby rotqmbyi rotqmbybi
        heq heqi hgt hgti hlgt hlgti
        ceqb ceqbi ceqh ceqhi ceq ceqi cgtb cgtbi cgth cgthi cgt cgti clgtb clgtbi clgth clgthi clgt clgti
        br bra brsl brasl brz brnz brhz brhnz bi iret bisled bisl biz binz bihz bihnz
        hbr hbra hbrr
        fa dfa fs dfs fm dfm fma dfma fnms dfnms fms dfms dfnma
        frest frsqest fi csflt cflts cuflt cfltu frds fesd fceq fcmeq fcgt fcmgt
        fscrwr fscrrd dfceq dfcmeq dfcgt dfcmgt dftsv
        stop stopd lnop nop sync dsync mfspr mtspr rdch rchcnt wrch
        """.split()
        self.assertEqual(sorted(set(expected) - set(SPECS_BY_NAME)), [])
        self.assertEqual(sorted(set(SPECS_BY_NAME) - set(expected)), [])

    def test_flow_flags(self):
        self.assertEqual(SPECS_BY_NAME["br"].flags, {"branch", "relative"})
        self.assertEqual(SPECS_BY_NAME["brasl"].flags, {"branch", "absolute", "link"})
        self.assertEqual(SPECS_BY_NAME["brhnz"].flags, {"branch", "relative", "conditional"})
        self.assertEqual(SPECS_BY_NAME["bisl"].flags, {"branch", "indirect", "link"})
        self.assertIn("conditional", SPECS_BY_NAME["bisled"].flags)
        self.assertIn("halt", SPECS_BY_NAME["hgti"].flags)
        self.assertIn("stop", SPECS_BY_NAME["stopd"].flags)
        self.assertIn("load", SPECS_BY_NAME["lqr"].flags)
        self.assertIn("store", SPECS_BY_NAME["stqx"].flags)
        self.assertEqual(SPECS_BY_NAME["rdch"].group, "chan")
        self.assertEqual(SPECS_BY_NAME["dftsv"].group, "fp_double")


if __name__ == "__main__":
    unittest.main()

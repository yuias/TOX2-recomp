"""Tests for the PPU decoder engine and its opcode tables."""

import struct
import unittest

from ps3recomp import ppu, ppu_ops, ppu_vmx
from ps3recomp.ppu import ALL_OPS, bits, decode, decode_range, op

# (word, addr, mnemonic, expected field values)
INT_CASES = [
    (0x7C0802A6, 0x10000, "mfspr", {"rD": 0, "SPR": 8}),
    (0x7C0803A6, 0x10000, "mtspr", {"SPR": 8, "rS": 0}),
    (0x7D8903A6, 0x10000, "mtspr", {"SPR": 9, "rS": 12}),
    (0x4E800020, 0x10000, "bclr", {"BO": 20, "BI": 0}),
    (0x4E800420, 0x10000, "bcctr", {"BO": 20, "BI": 0}),
    (0x4E800421, 0x10000, "bcctrl", {"BO": 20, "BI": 0}),
    (0x60000000, 0x10000, "ori", {"rA": 0, "rS": 0, "UIMM": 0}),
    (0x38600000, 0x10000, "addi", {"rD": 3, "rA": 0, "SIMM": 0}),
    (0x3C600001, 0x10000, "addis", {"rD": 3, "rA": 0, "SIMM": 1}),
    (0x38210070, 0x10000, "addi", {"rD": 1, "rA": 1, "SIMM": 0x70}),
    (0xF821FF91, 0x10000, "stdu", {"rS": 1, "rA": 1, "DS": -112}),
    (0xFBE1FFF8, 0x10000, "std", {"rS": 31, "rA": 1, "DS": -8}),
    (0xE8010080, 0x10000, "ld", {"rD": 0, "rA": 1, "DS": 0x80}),
    (0x90010008, 0x10000, "stw", {"rS": 0, "rA": 1, "D": 8}),
    (0x80630000, 0x10000, "lwz", {"rD": 3, "rA": 3, "D": 0}),
    (0x44000002, 0x10000, "sc", {}),
    (0x7C7F1B78, 0x10000, "or", {"rA": 31, "rS": 3, "rB": 3, "Rc": 0}),
    (0x7C642A15, 0x10000, "add.", {"rD": 3, "rA": 4, "rB": 5, "OE": 0, "Rc": 1}),
    (0x7C642E14, 0x10000, "addo", {"rD": 3, "rA": 4, "rB": 5, "OE": 1, "Rc": 0}),
    (0x7C000026, 0x10000, "mfcr", {"rD": 0}),
    (0x7C102026, 0xA0068, "mfocrf", {"rD": 0, "FXM": 0x02}),
    (0x7D908120, 0x95C5C, "mtocrf", {"CRM": 0x08, "rS": 12}),
    (0x7C0FF120, 0x10000, "mtcrf", {"CRM": 0xFF, "rS": 0}),
    (0x7C0004AC, 0x10000, "sync", {"L_sync": 0}),
    (0x7C2004AC, 0x10000, "sync", {"L_sync": 1}),
    (0x4C00012C, 0x10000, "isync", {}),
    (0x7C6307B4, 0x10000, "extsw", {"rA": 3, "rS": 3}),
    (0x78630020, 0x10000, "rldicl", {"rA": 3, "rS": 3, "SH64": 0, "MB64": 32}),
    (0x788345E4, 0x10000, "rldicr", {"rA": 3, "rS": 4, "SH64": 8, "MB64": 55}),
    (0x5483103A, 0x10000, "rlwinm", {"rA": 3, "rS": 4, "SH": 2, "MB": 0, "ME": 29}),
    (0x7C831E74, 0x10000, "sradi", {"rA": 3, "rS": 4, "SH64": 3}),
    (0x7C60212D, 0x10000, "stwcx.", {"rS": 3, "rA": 0, "rB": 4}),
    (0x2C030000, 0x10000, "cmpi", {"crfD": 0, "L": 0, "rA": 3, "SIMM": 0}),
    (0x2FA30000, 0x10000, "cmpi", {"crfD": 7, "L": 1, "rA": 3, "SIMM": 0}),
    (0x4182000C, 0x10000, "bc", {"BO": 12, "BI": 2, "BD": 12, "AA": 0, "LK": 0}),
    (0x4200FFF8, 0x10008, "bc", {"BO": 16, "BI": 0, "BD": -8}),
    (0x4BFFFFFC, 0x10000, "b", {"LI": -4}),
    (0x48000011, 0x10000, "bl", {"LI": 0x10, "LK": 1}),
]

FP_CASES = [
    (0xFC000890, 0x10000, "fmr", {"frD": 0, "frB": 1, "Rc": 0}),
    (0xEC21102A, 0x10000, "fadds", {"frD": 1, "frA": 1, "frB": 2}),
    (0xFC2220FA, 0x10000, "fmadd", {"frD": 1, "frA": 2, "frC": 3, "frB": 4}),
    (0xFC2220FB, 0x10000, "fmadd.", {"Rc": 1}),
]

VMX_CASES = [
    (0x7C0018CE, 0x10000, "lvx", {"vD": 0, "rA": 0, "rB": 3}),
    (0x7C0019CE, 0x10000, "stvx", {"vS": 0, "rA": 0, "rB": 3}),
    (0x7C03240E, 0x10000, "lvlx", {"vD": 0, "rA": 3, "rB": 4}),
    (0x10221484, 0x10000, "vor", {"vD": 1, "vA": 2, "vB": 2}),
    (0x100004C4, 0x10000, "vxor", {"vD": 0, "vA": 0, "vB": 0}),
    (0x1022192B, 0x10000, "vperm", {"vD": 1, "vA": 2, "vB": 3, "vC": 4}),
    (0x102220EE, 0x10000, "vmaddfp", {"vD": 1, "vA": 2, "vC": 3, "vB": 4}),
    (0x1022192C, 0x10000, "vsldoi", {"vD": 1, "vA": 2, "vB": 3, "SHB": 4}),
    (0x10011486, 0x10000, "vcmpequw.", {"vD": 0, "vA": 1, "vB": 2, "Rc_vc": 1}),
    (0x10011086, 0x10000, "vcmpequw", {"Rc_vc": 0}),
    (0x101F038C, 0x10000, "vspltisw", {"vD": 0, "SIMM5": -1}),
]


class BitsTest(unittest.TestCase):
    def test_ibm_bit_numbering(self):
        self.assertEqual(bits(0x7C0802A6, 0, 5), 31)
        self.assertEqual(bits(0x7C0802A6, 21, 30), 339)
        self.assertEqual(bits(0x80000000, 0, 0), 1)
        self.assertEqual(bits(0x00000001, 31, 31), 1)

    def test_swapped_spr_halves(self):
        # Bits 11-15 hold the low half, bits 16-20 the high half.
        word = 0x7C0002A6 | (3 << 16) | (8 << 11)
        self.assertEqual(ppu.FIELDS["SPR"](word), 8 << 5 | 3)


class StructureTest(unittest.TestCase):
    def test_table_not_empty(self):
        self.assertGreater(len(ALL_OPS), 150)

    def test_match_within_mask(self):
        for spec in ALL_OPS:
            with self.subTest(spec=spec.name):
                self.assertEqual(spec.match & ~spec.mask & 0xFFFFFFFF, 0)

    def test_specs_decode_from_own_match(self):
        for spec in ALL_OPS:
            with self.subTest(spec=spec.name):
                self.assertIs(decode(spec.match).spec, spec)

    def test_no_duplicate_mask_match(self):
        seen = {}
        for spec in ALL_OPS:
            key = (spec.mask, spec.match)
            self.assertNotIn(key, seen, f"{spec.name} duplicates {seen.get(key)}")
            seen[key] = spec.name

    def test_overlaps_are_nested(self):
        by_primary = {}
        for spec in ALL_OPS:
            by_primary.setdefault(spec.primary, []).append(spec)
        for specs in by_primary.values():
            for i, a in enumerate(specs):
                for b in specs[i + 1:]:
                    if (a.match ^ b.match) & a.mask & b.mask:
                        continue
                    superset = (a.mask & b.mask) in (a.mask, b.mask) and a.mask != b.mask
                    self.assertTrue(superset, f"{a.name} and {b.name} overlap ambiguously")

    def test_duplicate_detected(self):
        dup = op("dup", "D", 14)
        with self.assertRaises(ValueError):
            ppu._build_lookup((dup, dup))

    def test_op_validation(self):
        with self.assertRaises(ValueError):
            op("bad", "D", 14, xo=1)
        with self.assertRaises(ValueError):
            op("bad", "X", 31)
        with self.assertRaises(ValueError):
            op("bad", "DS", 58, xo=4)
        with self.assertRaises(ValueError):
            op("bad", "Q", 1)


class DecodeTest(unittest.TestCase):
    def test_known_integer_encodings(self):
        for word, addr, mnemonic, fields in INT_CASES:
            with self.subTest(word="%08x" % word):
                insn = decode(word, addr)
                self.assertIsNotNone(insn.spec)
                self.assertEqual(insn.addr, addr)
                self.assertEqual(insn.mnemonic, mnemonic)
                for name, value in fields.items():
                    self.assertEqual(insn.field(name), value, name)

    def _check_cases(self, cases):
        for word, addr, mnemonic, fields in cases:
            with self.subTest(word="%08x" % word):
                insn = decode(word, addr)
                self.assertIsNotNone(insn.spec)
                self.assertEqual(insn.mnemonic, mnemonic)
                for name, value in fields.items():
                    self.assertEqual(insn.field(name), value, name)

    def test_known_fp_encodings(self):
        self._check_cases(FP_CASES)

    def test_known_vmx_encodings(self):
        self._check_cases(VMX_CASES)

    def test_vector_ops_come_from_vmx_table(self):
        self.assertEqual(len(ALL_OPS), len(ppu_ops.OPS) + len(ppu_vmx.OPS))
        self.assertIs(decode(0x7C0018CE).spec, next(s for s in ppu_vmx.OPS if s.name == "lvx"))

    def test_sweep_never_raises(self):
        decoded = 0
        for primary in (4, 31, 59, 63):
            for i in range(1 << 16):
                if decode(primary << 26 | i << 1).spec is not None:
                    decoded += 1
        self.assertGreater(decoded, 0)

    def test_unknown_words(self):
        for word in (0x00000000, 0x04000000):
            with self.subTest(word="%08x" % word):
                insn = decode(word)
                self.assertIsNone(insn.spec)
                self.assertIsNone(insn.mnemonic)

    def test_absolute_branch_suffix(self):
        self.assertEqual(decode(0x48000003).mnemonic, "bla")
        self.assertEqual(decode(0x48000002).mnemonic, "ba")

    def test_extended_opcode_collisions_resolved(self):
        # XO (bits 22-30) and X (bits 21-30) forms share primary 31.
        self.assertEqual(decode(0x7C642A14).mnemonic, "add")
        self.assertEqual(decode(0x7C642B78).mnemonic, "or")
        self.assertEqual(decode(0x7C6307B4).mnemonic, "extsw")

    def test_decode_range(self):
        data = struct.pack(">3I", 0x60000000, 0x4E800020, 0x00000000) + b"\xAA\xBB"
        insns = list(decode_range(data, 0x10000))
        self.assertEqual([i.addr for i in insns], [0x10000, 0x10004, 0x10008])
        self.assertEqual([i.mnemonic for i in insns], ["ori", "bclr", None])



def _x_form(rt, ra, rb, xo, low=0):
    return 0x7C000000 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | low


class StrictStructureTest(unittest.TestCase):
    def test_masks_consistent(self):
        for spec in ALL_OPS:
            with self.subTest(spec=spec.name):
                self.assertEqual(spec.strict_mask & spec.mask, spec.mask)
                self.assertEqual(spec.strict_match & ~spec.strict_mask & 0xFFFFFFFF, 0)
                self.assertEqual(spec.strict_match & spec.mask, spec.match)
                self.assertIs(decode(spec.strict_match, strict=True).spec, spec)

    def test_operand_names_have_field_bits(self):
        names = set()
        for spec in ALL_OPS:
            for operand in spec.operands:
                names.update(operand.replace(")", "").split("("))
        self.assertLessEqual(names, set(ppu.FIELD_BITS))
        self.assertEqual(set(ppu.FIELD_BITS), set(ppu.FIELDS))

    def test_field_bits_match_extractors(self):
        # Flipping a bit changes the extracted value exactly when the bit is listed.
        word = 0x5A3CA5C3
        for name, extract in ppu.FIELDS.items():
            for ibm_bit in range(32):
                flipped = word ^ (1 << (31 - ibm_bit))
                listed = any(a <= ibm_bit <= b for a, b in ppu.FIELD_BITS[name])
                with self.subTest(field=name, bit=ibm_bit):
                    self.assertEqual(extract(word) != extract(flipped), listed)

    def test_exceptions_name_real_specs(self):
        names = {spec.name for spec in ALL_OPS}
        self.assertLessEqual(set(ppu.STRICT_EXCEPTIONS), names)

    def test_replaced_spec_recomputes_strict_masks(self):
        # mfocrf is built with dataclasses.replace() on mask/match.
        mfocrf = next(s for s in ALL_OPS if s.name == "mfocrf")
        mfcr = next(s for s in ALL_OPS if s.name == "mfcr")
        self.assertTrue(mfocrf.strict_match & 1 << 20)
        self.assertFalse(mfcr.strict_match & 1 << 20)
        self.assertTrue(mfcr.strict_mask & 1 << 20)


class StrictDecodeTest(unittest.TestCase):
    def assertStrictOk(self, word, mnemonic):
        self.assertEqual(decode(word, strict=True).mnemonic, mnemonic, "%08x" % word)

    def assertStrictRejected(self, word, mnemonic):
        # The word decodes without strict mode but is rejected with it.
        self.assertEqual(decode(word).mnemonic, mnemonic, "%08x" % word)
        self.assertIsNone(decode(word, strict=True).spec, "%08x" % word)

    def test_sc_is_exactly_one_word(self):
        self.assertStrictOk(0x44000002, "sc")
        self.assertStrictRejected(0x47616D65, "sc")
        self.assertStrictRejected(0x44000022, "sc")  # LEV = 1
        self.assertStrictRejected(0x44A00000, "sc")
        self.assertStrictRejected(0x44000003, "sc")
        self.assertStrictRejected(0x44000000, "sc")

    def test_stwcx_stdcx_dot_is_part_of_encoding(self):
        self.assertStrictOk(0x7C60492D, "stwcx.")
        self.assertStrictRejected(0x7C60492C, "stwcx.")
        self.assertStrictOk(_x_form(3, 0, 9, 214, 1), "stdcx.")
        self.assertStrictRejected(_x_form(3, 0, 9, 214), "stdcx.")

    def test_unused_flag_bit_is_reserved(self):
        self.assertStrictOk(_x_form(3, 4, 5, 23), "lwzx")
        self.assertStrictRejected(_x_form(3, 4, 5, 23, 1), "lwzx")
        self.assertStrictOk(_x_form(3, 4, 5, 75), "mulhw")
        self.assertStrictOk(_x_form(3, 4, 5, 75, 1), "mulhw.")
        self.assertStrictRejected(_x_form(3, 4, 5, 75) | 1 << 10, "mulhw")  # OE bit

    def test_unused_operand_field_is_reserved(self):
        fmul = 0xFC000000 | 1 << 21 | 2 << 16 | 4 << 6 | 25 << 1
        self.assertStrictOk(fmul, "fmul")
        self.assertStrictRejected(fmul | 3 << 11, "fmul")  # frB
        frsqrte = 0xFC000000 | 1 << 21 | 2 << 11 | 26 << 1
        self.assertStrictOk(frsqrte, "frsqrte")
        self.assertStrictRejected(frsqrte | 3 << 16, "frsqrte")  # frA
        self.assertStrictRejected(frsqrte | 3 << 6, "frsqrte")  # frC
        self.assertStrictRejected(0xFFE00000, "fcmpu")  # bits 9-10

    def test_cmp_reserved_bit_9(self):
        cmpw = _x_form(0, 3, 4, 0)
        self.assertStrictOk(cmpw, "cmp")
        self.assertStrictOk(cmpw | 1 << 21, "cmp")  # L
        self.assertStrictRejected(cmpw | 1 << 22, "cmp")

    def test_branch_hint_bits(self):
        self.assertStrictOk(0x4E800020, "bclr")
        self.assertStrictOk(0x4E800820, "bclr")  # BH
        self.assertStrictOk(0x4E801820, "bclr")
        self.assertStrictRejected(0x4E808020, "bclr")  # bit 16
        self.assertStrictOk(0x4E800420, "bcctr")
        self.assertStrictRejected(0x4E808420, "bcctr")

    def test_invalid_bo_values_are_not_reserved_bits(self):
        # BO = 21 has a set "z" bit; that is an invalid operand value, not a reserved bit.
        self.assertStrictOk(0x40000000 | 21 << 21, "bc")

    def test_mfocrf_mtocrf(self):
        self.assertStrictOk(0x7C000026, "mfcr")
        self.assertStrictOk(0x7C102026, "mfocrf")
        self.assertStrictOk(0x7D908120, "mtocrf")
        self.assertStrictOk(0x7C0FF120, "mtcrf")
        # Multi-bit FXM is undefined rather than reserved, so it is accepted.
        self.assertStrictOk(0x7C1FF026, "mfocrf")

    def test_ocrf_with_reserved_bit_does_not_fall_back_to_plain_form(self):
        for word, name in ((0x7C102026 | 1 << 11, "mfocrf"), (0x7D908120 | 1 << 11, "mtocrf")):
            with self.subTest(word="%08x" % word):
                self.assertStrictRejected(word, name)
        # With bit 11 clear, FXM-like bits in mfcr and bit 20 in mtcrf are reserved too.
        self.assertStrictRejected(0x7C002026, "mfcr")
        self.assertStrictRejected(0x7C0FF120 | 1 << 11, "mtcrf")

    def test_cache_hint_fields(self):
        self.assertStrictOk(_x_form(0, 3, 4, 278), "dcbt")
        self.assertStrictOk(_x_form(16, 3, 4, 278), "dcbt")  # TH
        self.assertStrictOk(_x_form(16, 3, 4, 246), "dcbtst")
        self.assertStrictOk(_x_form(0, 3, 4, 54), "dcbst")
        self.assertStrictRejected(_x_form(16, 3, 4, 54), "dcbst")

    def test_data_stream_bits(self):
        self.assertStrictOk(0x7E00066C, "dss")  # dssall
        self.assertStrictOk(0x7C00066C, "dss")
        self.assertStrictRejected(0x7C00066C | 1 << 24, "dss")  # bit 7
        self.assertStrictOk(_x_form(16, 3, 4, 342), "dst")  # T
        self.assertStrictRejected(_x_form(8, 3, 4, 342), "dst")  # bit 7

    def test_vector_reserved_fields(self):
        vspltisw = 0x101F038C
        self.assertStrictOk(vspltisw, "vspltisw")
        self.assertStrictRejected(vspltisw | 1 << 11, "vspltisw")  # vB
        vsldoi = 0x1022192C
        self.assertStrictOk(vsldoi, "vsldoi")
        self.assertStrictRejected(vsldoi | 1 << 10, "vsldoi")  # bit 21

    def test_canonical_cases_are_strict_ok(self):
        for word, _, mnemonic, _ in INT_CASES + FP_CASES + VMX_CASES:
            with self.subTest(word="%08x" % word):
                self.assertEqual(decode(word).mnemonic, mnemonic)
                self.assertIsNotNone(decode(word, strict=True).spec)

    def test_non_strict_ignores_reserved_bits(self):
        for word in (0x47616D65, 0x44A00000, 0xFFE00000):
            self.assertIsNotNone(decode(word).spec)
            self.assertIs(decode(word).spec, decode(word, strict=False).spec)

    def test_decode_range_strict(self):
        data = struct.pack(">3I", 0x44000002, 0x47616D65, 0x4E800020)
        self.assertEqual([i.mnemonic for i in decode_range(data, 0)], ["sc", "sc", "bclr"])
        self.assertEqual([i.mnemonic for i in decode_range(data, 0, strict=True)],
                         ["sc", None, "bclr"])


if __name__ == "__main__":
    unittest.main()

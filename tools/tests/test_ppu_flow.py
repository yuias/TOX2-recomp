"""Tests for the test encoder and the flow primitives."""

import unittest

from ps3recomp.ppu import decode
from ps3recomp.ppu_flow import (FLOW_KINDS, CodeImage, Flow, eval_const, find_def, flow_of,
                                walk_back, written_gprs)

from tests import ppc_asm as asm

BASE = 0x10000
TOC = 0x00109F80


def image(*words: int, start: int = BASE) -> CodeImage:
    return CodeImage.from_words(start, words)


def at(word: int, addr: int = BASE):
    return decode(word, addr, strict=True)


class EncoderTest(unittest.TestCase):
    CASES = [
        (asm.b(BASE, BASE + 0x100), "b", {"LI": 0x100}),
        (asm.b(BASE, BASE - 8, lk=True), "bl", {"LI": -8}),
        (asm.b(BASE, 0x2000, aa=True), "ba", {"LI": 0x2000}),
        (asm.b(BASE, 0x2000, lk=True, aa=True), "bla", {"LI": 0x2000}),
        (asm.bc(12, 2, BASE, BASE + 0x20), "bc", {"BO": 12, "BI": 2, "BD": 0x20}),
        (asm.bc(4, 1, BASE, BASE - 0x20, lk=True), "bcl", {"BO": 4, "BI": 1, "BD": -0x20}),
        (asm.blr(), "bclr", {"BO": 20, "BI": 0}),
        (asm.bctr(), "bcctr", {"BO": 20, "BI": 0}),
        (asm.bctrl(), "bcctrl", {"BO": 20, "BI": 0}),
        (asm.bclr(12, 2), "bclr", {"BO": 12, "BI": 2}),
        (asm.bcctr(12, 2), "bcctr", {"BO": 12, "BI": 2}),
        (asm.mtctr(12), "mtspr", {"SPR": 9, "rS": 12}),
        (asm.mflr(0), "mfspr", {"SPR": 8, "rD": 0}),
        (asm.nop(), "ori", {"rA": 0, "rS": 0, "UIMM": 0}),
        (asm.li(3, -5), "addi", {"rD": 3, "rA": 0, "SIMM": -5}),
        (asm.lis(4, 0x1234), "addis", {"rD": 4, "rA": 0, "SIMM": 0x1234}),
        (asm.addi(5, 6, 0x70), "addi", {"rD": 5, "rA": 6, "SIMM": 0x70}),
        (asm.addis(12, 2, -1), "addis", {"rD": 12, "rA": 2, "SIMM": -1}),
        (asm.ori(3, 4, 0xBEEF), "ori", {"rA": 3, "rS": 4, "UIMM": 0xBEEF}),
        (asm.oris(3, 4, 0xBEEF), "oris", {"rA": 3, "rS": 4, "UIMM": 0xBEEF}),
        (asm.mr(31, 3), "or", {"rA": 31, "rS": 3, "rB": 3, "Rc": 0}),
        (asm.lwz(9, -4, 10), "lwz", {"rD": 9, "rA": 10, "D": -4}),
        (asm.ld(9, -8, 2), "ld", {"rD": 9, "rA": 2, "DS": -8}),
        (asm.lwzx(3, 4, 5), "lwzx", {"rD": 3, "rA": 4, "rB": 5}),
        (asm.lwax(3, 4, 5), "lwax", {"rD": 3, "rA": 4, "rB": 5}),
        (asm.add(3, 4, 5), "add", {"rD": 3, "rA": 4, "rB": 5, "OE": 0, "Rc": 0}),
        (asm.slwi(3, 4, 2), "rlwinm", {"rA": 3, "rS": 4, "SH": 2, "MB": 0, "ME": 29}),
        (asm.sldi(3, 4, 3), "rldicr", {"rA": 3, "rS": 4, "SH64": 3, "MB64": 60}),
        (asm.sldi(3, 4, 40), "rldicr", {"rA": 3, "rS": 4, "SH64": 40, "MB64": 23}),
        (asm.clrldi(3, 4, 32), "rldicl", {"rA": 3, "rS": 4, "SH64": 0, "MB64": 32}),
        (asm.rlwinm(9, 9, 2, 22, 29), "rlwinm", {"rA": 9, "rS": 9, "SH": 2, "MB": 22, "ME": 29}),
        (asm.clrlwi(0, 9, 24), "rlwinm", {"rA": 0, "rS": 9, "SH": 0, "MB": 24, "ME": 31}),
        (asm.rldic(9, 9, 2, 30), "rldic", {"rA": 9, "rS": 9, "SH64": 2, "MB64": 30}),
        (asm.extsw(0, 3), "extsw", {"rA": 0, "rS": 3}),
        (asm.bgt(7, BASE, BASE + 8), "bc", {"BO": 12, "BI": 29, "BD": 8}),
        (asm.ble(7, BASE, BASE + 8), "bc", {"BO": 4, "BI": 29, "BD": 8}),
        (asm.bge(1, BASE, BASE + 8), "bc", {"BO": 4, "BI": 4, "BD": 8}),
        (asm.cmplwi(7, 4, 0x1F), "cmpli", {"crfD": 7, "L": 0, "rA": 4, "UIMM": 0x1F}),
        (asm.cmplw(1, 4, 5), "cmpl", {"crfD": 1, "L": 0, "rA": 4, "rB": 5}),
        (asm.stdu(1, -112, 1), "stdu", {"rS": 1, "rA": 1, "DS": -112}),
        (asm.sc(), "sc", {}),
        (asm.trap(), "tw", {"TO": 31, "rA": 0, "rB": 0}),
        (asm.tw(4, 3, 4), "tw", {"TO": 4, "rA": 3, "rB": 4}),
        (asm.twi(31, 3, 7), "twi", {"TO": 31, "rA": 3, "SIMM": 7}),
        (asm.lvlx(1, 2, 3), "lvlx", {"vD": 1, "rA": 2, "rB": 3}),
        (asm.lvlxl(1, 2, 3), "lvlxl", {"vD": 1, "rA": 2, "rB": 3}),
        (asm.lvrx(1, 2, 3), "lvrx", {"vD": 1, "rA": 2, "rB": 3}),
        (asm.lvrxl(1, 2, 3), "lvrxl", {"vD": 1, "rA": 2, "rB": 3}),
        (asm.stvlx(1, 2, 3), "stvlx", {"vS": 1, "rA": 2, "rB": 3}),
        (asm.stvlxl(1, 2, 3), "stvlxl", {"vS": 1, "rA": 2, "rB": 3}),
        (asm.stvrx(1, 2, 3), "stvrx", {"vS": 1, "rA": 2, "rB": 3}),
        (asm.stvrxl(1, 2, 3), "stvrxl", {"vS": 1, "rA": 2, "rB": 3}),
    ]

    def test_strict_decode_matches_intent(self):
        for word, mnemonic, fields in self.CASES:
            with self.subTest(word="0x%08x" % word, mnemonic=mnemonic):
                insn = at(word)
                self.assertIsNotNone(insn.spec, "strict decode failed")
                self.assertEqual(insn.mnemonic, mnemonic)
                for name, value in fields.items():
                    self.assertEqual(insn.field(name), value, name)

    def test_known_words(self):
        self.assertEqual(asm.blr(), 0x4E800020)
        self.assertEqual(asm.bctr(), 0x4E800420)
        self.assertEqual(asm.bctrl(), 0x4E800421)
        self.assertEqual(asm.nop(), 0x60000000)
        self.assertEqual(asm.mr(31, 3), 0x7C7F1B78)
        self.assertEqual(asm.stdu(1, -112, 1), 0xF821FF91)
        self.assertEqual(asm.mflr(0), 0x7C0802A6)
        self.assertEqual(asm.mtctr(12), 0x7D8903A6)
        self.assertEqual(asm.trap(), 0x7FE00008)


class FlowOfTest(unittest.TestCase):
    def check(self, word, kind, target, falls, addr=BASE):
        flow = flow_of(at(word, addr))
        self.assertEqual(flow, Flow(kind, target, falls))
        self.assertIn(flow.kind, FLOW_KINDS)

    def test_b(self):
        self.check(asm.b(BASE, BASE + 0x40), "jump", BASE + 0x40, False)
        self.check(asm.b(BASE, BASE - 0x40, lk=True), "call", BASE - 0x40, True)

    def test_b_absolute(self):
        self.check(asm.b(BASE, 0x2000, aa=True), "jump", 0x2000, False)
        self.check(asm.b(BASE, 0x2000, lk=True, aa=True), "call", 0x2000, True)

    def test_b_target_wraps_to_32_bits(self):
        word = 18 << 26 | 0x03FFFFC0 | 2  # AA=1, LI = -64
        self.check(word, "jump", 0xFFFFFFC0, False)

    def test_bc(self):
        self.check(asm.bc(20, 0, BASE, BASE + 8), "jump", BASE + 8, False)
        self.check(asm.bc(12, 2, BASE, BASE + 8), "cond_jump", BASE + 8, True)
        self.check(asm.bc(20, 0, BASE, BASE + 8, lk=True), "call", BASE + 8, True)
        self.check(asm.bc(12, 2, BASE, BASE + 8, lk=True), "cond_call", BASE + 8, True)
        self.check(asm.bc(12, 2, BASE, 0x3000, aa=True), "cond_jump", 0x3000, True)

    def test_get_pc_idiom_is_seq(self):
        self.check(asm.bc(20, 31, BASE, BASE + 4, lk=True), "seq", None, True)
        self.check(asm.b(BASE, BASE + 4, lk=True), "seq", None, True)
        # Without LK it is an ordinary jump to the next word.
        self.check(asm.b(BASE, BASE + 4), "jump", BASE + 4, False)

    def test_bclr_bcctr(self):
        self.check(asm.blr(), "return", None, False)
        self.check(asm.bclr(12, 2), "cond_return", None, True)
        self.check(asm.bctr(), "indirect_jump", None, False)
        self.check(asm.bcctr(12, 2), "cond_indirect_jump", None, True)
        self.check(asm.bctrl(), "indirect_call", None, True)
        self.check(asm.bclr(20, 0, lk=True), "indirect_call", None, True)

    def test_sc(self):
        self.check(asm.sc(), "syscall", None, True)

    def test_trap_requires_to_31(self):
        self.check(asm.trap(), "trap", None, False)
        self.check(asm.twi(31, 3, 0), "trap", None, False)
        self.check(asm.tw(4, 3, 4), "seq", None, True)
        self.check(asm.twi(8, 3, 0), "seq", None, True)

    def test_other_is_seq(self):
        for word in (asm.nop(), asm.li(3, 1), asm.lwz(3, 0, 4), asm.mtctr(12), asm.lvlx(1, 2, 3)):
            self.check(word, "seq", None, True)

    def test_undecodable_is_invalid(self):
        # Unassigned primary opcodes.
        self.check(0x00000000, "invalid", None, False)
        self.check(0x04000000, "invalid", None, False)
        # `sc` with LEV != 0 is rejected by strict decoding.
        self.check(0x44000022, "invalid", None, False)


class CodeImageTest(unittest.TestCase):
    def test_accessors(self):
        img = image(asm.li(3, 1), 0, asm.blr())
        self.assertEqual((img.start, img.end), (BASE, BASE + 12))
        self.assertEqual(img.word(BASE + 8), asm.blr())
        self.assertEqual(img.insn(BASE).addr, BASE)
        self.assertEqual(img.insn(BASE + 8).addr, BASE + 8)

    def test_contains_and_valid(self):
        img = image(asm.li(3, 1), 0)
        self.assertTrue(img.contains(BASE))
        self.assertTrue(img.contains(BASE + 4))
        self.assertFalse(img.contains(BASE + 8))
        self.assertFalse(img.contains(BASE - 4))
        self.assertFalse(img.contains(BASE + 2))
        self.assertTrue(img.valid(BASE))
        self.assertFalse(img.valid(BASE + 4))
        self.assertFalse(img.valid(BASE + 8))

    def test_out_of_range_raises(self):
        img = image(asm.nop())
        with self.assertRaises(ValueError):
            img.word(BASE + 4)
        with self.assertRaises(ValueError):
            img.insn(BASE - 4)


class WrittenGprsTest(unittest.TestCase):
    def test_cases(self):
        self.assertEqual(written_gprs(at(asm.li(3, 1))), {3})
        self.assertEqual(written_gprs(at(asm.ori(5, 6, 1))), {5})
        self.assertEqual(written_gprs(at(asm.mr(31, 3))), {31})
        self.assertEqual(written_gprs(at(asm.stdu(1, -16, 1))), {1})
        self.assertEqual(written_gprs(at(asm.cmplwi(0, 3, 1))), frozenset())
        self.assertEqual(written_gprs(at(asm.nop())), {0})


class WalkBackTest(unittest.TestCase):
    def addrs(self, img, addr, limit=16):
        return [i.addr for i in walk_back(img, addr, limit)]

    def test_nearest_first_and_limit(self):
        img = image(asm.li(3, 1), asm.li(4, 2), asm.li(5, 3), asm.blr())
        self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8, BASE + 4, BASE])
        self.assertEqual(self.addrs(img, BASE + 12, 2), [BASE + 8, BASE + 4])
        self.assertEqual(self.addrs(img, BASE + 12, 0), [])

    def test_stops_at_image_start(self):
        img = image(asm.li(3, 1), asm.nop())
        self.assertEqual(self.addrs(img, BASE + 4, 10), [BASE])
        self.assertEqual(self.addrs(img, BASE), [])

    def test_stops_before_invalid_word(self):
        img = image(asm.li(3, 1), 0, asm.li(4, 2), asm.nop())
        self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8])

    def test_stops_before_call(self):
        img = image(asm.li(3, 1), asm.b(BASE + 4, 0x2000, lk=True), asm.li(4, 2), asm.nop())
        self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8])

    def test_stops_before_indirect_call(self):
        img = image(asm.li(3, 1), asm.bctrl(), asm.li(4, 2), asm.nop())
        self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8])

    def test_stops_before_unconditional_transfers(self):
        for word in (asm.b(BASE + 4, 0x2000), asm.blr(), asm.bctr(),
                     asm.bc(20, 0, BASE + 4, 0x2000)):
            with self.subTest(word="0x%08x" % word):
                img = image(asm.li(3, 1), word, asm.li(4, 2), asm.nop())
                self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8])

    def test_passes_conditional_branches(self):
        img = image(asm.li(3, 1), asm.bc(12, 2, BASE + 4, BASE + 0x40), asm.bclr(12, 2),
                    asm.bcctr(12, 2), asm.nop())
        self.assertEqual(self.addrs(img, BASE + 16), [BASE + 12, BASE + 8, BASE + 4, BASE])

    def test_passes_get_pc_idiom_and_sc(self):
        img = image(asm.li(3, 1), asm.bc(20, 31, BASE + 4, BASE + 8, lk=True), asm.sc(),
                    asm.nop())
        self.assertEqual(self.addrs(img, BASE + 12), [BASE + 8, BASE + 4, BASE])


class FindDefTest(unittest.TestCase):
    def test_finds_nearest_def(self):
        img = image(asm.li(3, 1), asm.li(3, 2), asm.li(4, 9), asm.nop())
        d = find_def(img, BASE + 12, 3)
        self.assertEqual(d.addr, BASE + 4)

    def test_logical_op_defines_ra(self):
        img = image(asm.ori(7, 3, 1), asm.nop())
        self.assertEqual(find_def(img, BASE + 4, 7).addr, BASE)
        self.assertIsNone(find_def(img, BASE + 4, 3))

    def test_barrier_and_limit(self):
        img = image(asm.li(3, 1), asm.b(BASE + 4, 0x2000, lk=True), asm.nop())
        self.assertIsNone(find_def(img, BASE + 8, 3))
        img = image(asm.li(3, 1), asm.nop(), asm.nop(), asm.nop())
        self.assertIsNone(find_def(img, BASE + 16, 3, limit=2))
        self.assertIsNotNone(find_def(img, BASE + 16, 3, limit=4))

    def test_not_found(self):
        self.assertIsNone(find_def(image(asm.nop(), asm.nop()), BASE + 8, 9))


class EvalConstTest(unittest.TestCase):
    def test_lis_addi(self):
        img = image(asm.lis(3, 0x0011), asm.addi(3, 3, 0x2345), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 3), 0x00112345)

    def test_lis_addi_negative_low_half(self):
        img = image(asm.lis(3, 0x0011), asm.addi(3, 3, -0x10), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 3), 0x0010FFF0)

    def test_li_oris(self):
        img = image(asm.li(4, 0x1234), asm.oris(4, 4, 0x00AB), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 4), 0x00AB1234)

    def test_ori(self):
        img = image(asm.lis(4, 0x00AB), asm.ori(5, 4, 0x1234), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 5), 0x00AB1234)

    def test_li_negative_is_masked_to_32_bits(self):
        img = image(asm.li(3, -1), asm.nop())
        self.assertEqual(eval_const(img, BASE + 4, 3), 0xFFFFFFFF)

    def test_toc_relative(self):
        img = image(asm.addis(12, 2, 1), asm.addi(12, 12, 0x20), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 12, toc=TOC), TOC + 0x10020)

    def test_r2_is_toc(self):
        img = image(asm.li(2, 5), asm.nop())
        self.assertEqual(eval_const(img, BASE + 4, 2, toc=TOC), TOC)
        self.assertIsNone(eval_const(img, BASE + 4, 2))

    def test_toc_unknown_propagates_none(self):
        img = image(asm.addis(12, 2, 1), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 4, 12))

    def test_mr_and_clrldi(self):
        img = image(asm.lis(3, 0x0012), asm.mr(4, 3), asm.clrldi(5, 4, 32), asm.nop())
        self.assertEqual(eval_const(img, BASE + 8, 4), 0x00120000)
        self.assertEqual(eval_const(img, BASE + 12, 5), 0x00120000)

    def test_clrldi_other_than_32_is_unknown(self):
        img = image(asm.li(3, 1), asm.clrldi(5, 3, 40), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 8, 5))

    def test_ld_toc_load(self):
        reads = []

        def read_mem(ea, size):
            reads.append((ea, size))
            return bytes.fromhex("0000000000123456") if size == 8 else None

        img = image(asm.ld(9, -0x20, 2), asm.nop())
        self.assertEqual(eval_const(img, BASE + 4, 9, toc=TOC, read_mem=read_mem), 0x123456)
        self.assertEqual(reads, [(TOC - 0x20, 8)])

    def test_ld_keeps_low_32_bits(self):
        img = image(asm.ld(9, 0, 2), asm.nop())
        value = eval_const(img, BASE + 4, 9, toc=TOC,
                           read_mem=lambda ea, size: bytes.fromhex("1122334455667788"))
        self.assertEqual(value, 0x55667788)

    def test_lwz_big_endian(self):
        img = image(asm.lis(3, 0x0010), asm.lwz(4, 0x10, 3), asm.nop())
        seen = []

        def read_mem(ea, size):
            seen.append((ea, size))
            return bytes.fromhex("DEADBEEF")

        self.assertEqual(eval_const(img, BASE + 8, 4, read_mem=read_mem), 0xDEADBEEF)
        self.assertEqual(seen, [(0x00100010, 4)])

    def test_load_without_read_mem_or_data_is_none(self):
        img = image(asm.ld(9, 0, 2), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 4, 9, toc=TOC))
        self.assertIsNone(eval_const(img, BASE + 4, 9, toc=TOC, read_mem=lambda ea, n: None))

    def test_depth_exhaustion(self):
        words = [asm.li(3, 1)] + [asm.addi(3, 3, 1)] * 8 + [asm.nop()]
        img = image(*words)
        end = BASE + 4 * len(words)
        self.assertIsNone(eval_const(img, end, 3, depth=6))
        self.assertEqual(eval_const(img, end, 3, depth=20), 9)
        self.assertIsNone(eval_const(img, end, 3, depth=0))

    def test_unknown_def_is_none(self):
        img = image(asm.lwzx(3, 4, 5), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 4, 3))
        img = image(asm.mflr(3), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 4, 3))

    def test_no_def_is_none(self):
        self.assertIsNone(eval_const(image(asm.nop(), asm.nop()), BASE + 8, 9))

    def test_barrier_blocks_definition(self):
        img = image(asm.li(3, 1), asm.b(BASE + 4, 0x2000, lk=True), asm.nop())
        self.assertIsNone(eval_const(img, BASE + 8, 3))

    def test_calls_stay_barriers_unless_through_calls(self):
        img = image(asm.lwz(30, -8, 2), asm.bl(BASE + 4, 0x2000), asm.nop())
        read_mem = lambda ea, n: (0x1234).to_bytes(n, "big")
        self.assertIsNone(eval_const(img, BASE + 8, 30, toc=TOC, read_mem=read_mem))
        self.assertEqual(eval_const(img, BASE + 8, 30, toc=TOC, read_mem=read_mem,
                                    through_calls=True), 0x1234)

    def test_through_calls_keeps_volatile_registers_behind_calls(self):
        img = image(asm.lwz(11, -8, 2), asm.bl(BASE + 4, 0x2000), asm.nop())
        read_mem = lambda ea, n: (0x1234).to_bytes(n, "big")
        self.assertIsNone(eval_const(img, BASE + 8, 11, toc=TOC, read_mem=read_mem,
                                     through_calls=True))


if __name__ == "__main__":
    unittest.main()

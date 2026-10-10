"""Tests for SPU flow classification and backward constant evaluation."""

import unittest
from types import SimpleNamespace

from ps3recomp.spu import decode
from ps3recomp.spu_flow import (SpuCode, const_target, eval_const, flow_of, written_reg)

from tests import spu_asm

BASE = 0x1000
DATA = 0x2000


def enc(mnemonic: str, **fields) -> int:
    return spu_asm.encode(mnemonic, **fields)


def at(index: int) -> int:
    return BASE + 4 * index


def flow(word: int, addr: int = BASE, const_of=None):
    return flow_of(decode(word, addr), const_of)


class FlowKindTest(unittest.TestCase):
    def test_direct_jumps(self):
        f = flow(enc("br", addr=BASE, target=at(5)))
        self.assertEqual((f.kind, f.target, f.falls_through), ("jump", at(5), False))
        f = flow(enc("bra", addr=BASE, target=0x3000))
        self.assertEqual((f.kind, f.target, f.falls_through), ("jump", 0x3000, False))

    def test_conditional_branches_fall_through(self):
        for name in ("brz", "brnz", "brhz", "brhnz"):
            f = flow(enc(name, RT=3, addr=BASE, target=at(9)))
            self.assertEqual((f.kind, f.target, f.falls_through), ("cond_jump", at(9), True), name)

    def test_calls_carry_the_link_register(self):
        f = flow(enc("brsl", RT=0, addr=BASE, target=at(7)))
        self.assertEqual((f.kind, f.target, f.falls_through, f.link), ("call", at(7), True, 0))
        f = flow(enc("brasl", RT=2, addr=BASE, target=0x4000))
        self.assertEqual((f.kind, f.target, f.link), ("call", 0x4000, 2))

    def test_pc_read_is_not_a_call(self):
        f = flow(enc("brsl", RT=5, addr=BASE, target=at(1)))
        self.assertEqual((f.kind, f.falls_through), ("seq", True))

    def test_bi_zero_is_a_return_with_or_without_interrupt_bits(self):
        for fields in ({}, {"D": 1}, {"E": 1}):
            f = flow(enc("bi", RA=0, **fields))
            self.assertEqual((f.kind, f.falls_through), ("return", False), fields)

    def test_bi_other_register_is_an_indirect_jump(self):
        f = flow(enc("bi", RA=5))
        self.assertEqual((f.kind, f.reg, f.falls_through), ("indirect_jump", 5, False))

    def test_iret_is_an_indirect_jump_without_register(self):
        f = flow(enc("iret"))
        self.assertEqual((f.kind, f.reg, f.falls_through), ("indirect_jump", None, False))

    def test_conditional_indirect_branches(self):
        for name in ("biz", "binz", "bihz", "bihnz"):
            f = flow(enc(name, RT=3, RA=4))
            self.assertEqual((f.kind, f.reg, f.falls_through), ("cond_indirect_jump", 4, True), name)
        f = flow(enc("binz", RT=3, RA=0, E=1))
        self.assertEqual((f.kind, f.falls_through), ("cond_return", True))

    def test_indirect_calls(self):
        for name in ("bisl", "bisled"):
            f = flow(enc(name, RT=0, RA=7))
            self.assertEqual((f.kind, f.reg, f.link, f.falls_through), ("indirect_call", 7, 0, True), name)

    def test_stop_codes(self):
        for code in (0x101, 0x102):
            self.assertEqual(flow(enc("stop", STOP=code)).kind, "terminate", hex(code))
        for code in (0x100, 0x3, 0x2101):
            f = flow(enc("stop", STOP=code))
            self.assertEqual((f.kind, f.falls_through), ("seq", True), hex(code))
        self.assertEqual(flow(enc("stopd", RC=3, RA=4, RB=5)).kind, "terminate")

    def test_hints_and_nops_fall_through(self):
        words = [enc("nop", RT=1), enc("lnop"), enc("sync"), enc("dsync"),
                 enc("hbrr", addr=BASE, site=at(3), target=at(9)), enc("hbr", RA=4, site=at(3), addr=BASE),
                 enc("il", RT=3, I16=1)]
        for word in words:
            f = flow(word)
            self.assertEqual((f.kind, f.falls_through), ("seq", True), hex(word))

    def test_zero_and_undecoded_words_are_invalid(self):
        self.assertEqual(flow(0).kind, "invalid")
        self.assertIsNone(decode(0x90000000).spec)
        self.assertEqual(flow(0x90000000).kind, "invalid")


class HaltTest(unittest.TestCase):
    def test_heq_of_a_register_with_itself_always_halts(self):
        self.assertEqual(flow(enc("heq", RA=3, RB=3)).kind, "terminate")

    def test_hgt_and_hlgt_of_a_register_with_itself_never_halt(self):
        for name in ("hgt", "hlgt"):
            self.assertEqual(flow(enc(name, RA=3, RB=3)).kind, "seq", name)

    def test_unknown_operands_do_not_halt(self):
        self.assertEqual(flow(enc("heq", RA=3, RB=4)).kind, "seq")
        self.assertEqual(flow(enc("heqi", RA=3, I10=0), const_of=lambda r: None).kind, "seq")

    def test_constant_conditions(self):
        consts = {3: 5, 4: 5, 6: 0xFFFFFFFE}

        def kind(name, **fields):
            return flow(enc(name, **fields), const_of=consts.get).kind

        self.assertEqual(kind("heq", RA=3, RB=4), "terminate")
        self.assertEqual(kind("hgt", RA=3, RB=4), "seq")
        self.assertEqual(kind("heqi", RA=3, I10=5), "terminate")
        self.assertEqual(kind("heqi", RA=3, I10=6), "seq")
        # 5 > -1 signed, but 5 < 0xffffffff unsigned.
        self.assertEqual(kind("hgti", RA=3, I10=-1), "terminate")
        self.assertEqual(kind("hlgti", RA=3, I10=-1), "seq")
        # 0xfffffffe is -2 signed.
        self.assertEqual(kind("hgti", RA=6, I10=-1), "seq")
        self.assertEqual(kind("hlgti", RA=6, I10=-3), "terminate")
        self.assertEqual(kind("hlgt", RA=6, RB=3), "terminate")


class WrittenRegisterTest(unittest.TestCase):
    def test_written_registers(self):
        cases = [
            (enc("il", RT=3, I16=1), 3),
            (enc("a", RT=4, RA=1, RB=2), 4),
            (enc("selb", RT4=9, RA=1, RB=2, RC=3), 9),
            (enc("lqd", RT=5, RA=1), 5),
            (enc("rdch", RT=6, CA=13), 6),
            (enc("brsl", RT=0, addr=BASE, target=at(4)), 0),
            (enc("bisl", RT=0, RA=3), 0),
            (enc("stqd", RT=5, RA=1), None),
            (enc("wrch", CA=21, RT=5), None),
            (enc("brnz", RT=5, addr=BASE, target=at(4)), None),
            (enc("binz", RT=5, RA=6), None),
            (enc("bi", RA=0), None),
            (enc("nop", RT=7), None),
            (enc("stop", STOP=0x100), None),
        ]
        for word, reg in cases:
            self.assertEqual(written_reg(decode(word, BASE)), reg, hex(word))


def code_of(words, data=()):
    blob = b"".join(w.to_bytes(4, "big") for w in words)
    return SpuCode([(BASE, blob)], data)


class EvalConstTest(unittest.TestCase):
    def value(self, words, reg, index=None, **kwargs):
        index = len(words) if index is None else index
        return eval_const(code_of(words), at(index), reg, **kwargs)

    def test_immediate_loads(self):
        self.assertEqual(self.value([enc("il", RT=3, I16=-2)], 3), 0xFFFFFFFE)
        self.assertEqual(self.value([enc("ila", RT=3, I18=0x2A5C0)], 3), 0x2A5C0)
        self.assertEqual(self.value([enc("ilhu", RT=3, U16=0x1234)], 3), 0x12340000)
        self.assertEqual(self.value([enc("ilh", RT=3, U16=0x1234)], 3), 0x12341234)

    def test_ilhu_with_iohl(self):
        words = [enc("ilhu", RT=3, U16=0x1), enc("iohl", RT=3, U16=0x00B4)]
        self.assertEqual(self.value(words, 3), 0x100B4)

    def test_arithmetic_and_moves(self):
        words = [enc("il", RT=3, I16=0x100), enc("ai", RT=4, RA=3, I10=-0x10),
                 enc("ori", RT=5, RA=4, I10=0), enc("or", RT=6, RA=5, RB=5),
                 enc("shli", RT=7, RA=6, I7=2), enc("a", RT=8, RA=7, RB=3),
                 enc("andi", RT=9, RA=8, I10=0xFF)]
        self.assertEqual(self.value(words, 4), 0xF0)
        self.assertEqual(self.value(words, 5), 0xF0)
        self.assertEqual(self.value(words, 6), 0xF0)
        self.assertEqual(self.value(words, 7), 0x3C0)
        self.assertEqual(self.value(words, 8), 0x4C0)
        self.assertEqual(self.value(words, 9), 0xC0)

    def test_unknown_source_and_overwritten_register(self):
        words = [enc("il", RT=3, I16=1), enc("rdch", RT=3, CA=24)]
        self.assertIsNone(self.value(words, 3))
        words = [enc("ai", RT=4, RA=9, I10=1)]
        self.assertIsNone(self.value(words, 4))

    def test_pc_read(self):
        words = [enc("brsl", RT=5, addr=at(0), target=at(1)), enc("nop", RT=1)]
        self.assertEqual(self.value(words, 5), at(1))

    def test_label_between_definition_and_use_stops_the_walk(self):
        words = [enc("brnz", RT=9, addr=at(0), target=at(3)), enc("il", RT=3, I16=0x100),
                 enc("nop", RT=1), enc("nop", RT=1), enc("nop", RT=1)]
        self.assertEqual(self.value(words, 3, index=2), 0x100)
        self.assertIsNone(self.value(words, 3, index=3))      # the label itself
        self.assertIsNone(self.value(words, 3, index=4))      # a join point lies in between
        # The caller can add entries the image does not name directly.
        self.assertIsNone(self.value(words, 3, index=2, labels=frozenset({at(2)})))

    def test_calls_and_unconditional_transfers_end_the_walk(self):
        call = [enc("il", RT=3, I16=1), enc("brsl", RT=0, addr=at(1), target=at(9)), enc("nop", RT=1)]
        self.assertIsNone(self.value(call, 3))
        indirect = [enc("il", RT=3, I16=1), enc("bisl", RT=0, RA=7), enc("nop", RT=1)]
        self.assertIsNone(self.value(indirect, 3))
        jump = [enc("il", RT=3, I16=1), enc("br", addr=at(1), target=at(9)), enc("nop", RT=1)]
        self.assertIsNone(self.value(jump, 3))
        cond = [enc("il", RT=3, I16=1), enc("brnz", RT=9, addr=at(1), target=at(9)), enc("nop", RT=1)]
        self.assertEqual(self.value(cond, 3), 1)

    def test_walk_ends_at_invalid_words_and_the_start_of_the_image(self):
        self.assertIsNone(self.value([0, enc("nop", RT=1)], 3))
        self.assertIsNone(self.value([enc("nop", RT=1)], 3))

    def test_walk_limit(self):
        words = [enc("il", RT=3, I16=7)] + [enc("nop", RT=1)] * 10
        self.assertEqual(self.value(words, 3), 7)
        self.assertIsNone(self.value(words, 3, limit=5))

    def test_memory_loads_follow_the_image_bytes_only_when_asked(self):
        table = (0x1234).to_bytes(4, "big") + bytes(12)
        words = [enc("lqr", RT=4, addr=at(0), target=DATA + 8),    # unaligned: the quadword at DATA
                 enc("lqa", RT=5, ABS16=DATA),
                 enc("il", RT=6, I16=DATA), enc("lqd", RT=7, RA=6, I10x16=0)]
        code = code_of(words, [(DATA, table)])
        for reg in (4, 5, 7):
            self.assertIsNone(eval_const(code, at(4), reg), reg)
            self.assertEqual(eval_const(code, at(4), reg, memory=True), 0x1234, reg)
        # Bytes past the file data of a segment are not known.
        self.assertIsNone(eval_const(code_of(words, [(DATA, table[:3])]), at(4), 4, memory=True))


class ConstTargetTest(unittest.TestCase):
    def test_masks_the_address_and_uses_the_branch_register(self):
        words = [enc("ila", RT=4, I18=0x1002), enc("bi", RA=4)]
        code = code_of(words)
        insn = code.insn(at(1))
        self.assertEqual(const_target(code, insn, flow_of(insn)), 0x1000)
        iret = code_of([enc("iret")])
        self.assertIsNone(const_target(iret, iret.insn(BASE), flow_of(iret.insn(BASE))))

    def test_value_from_memory_must_be_code(self):
        words = [enc("lqr", RT=4, addr=at(0), target=DATA), enc("bisl", RT=0, RA=4)]
        for value, expected in ((at(1), at(1)), (0, None), (0x50000, None)):
            code = code_of(words, [(DATA, value.to_bytes(4, "big") + bytes(12))])
            insn = code.insn(at(1))
            self.assertEqual(const_target(code, insn, flow_of(insn)), expected, hex(value))

    def test_immediate_outside_the_image_is_still_a_constant(self):
        words = [enc("ila", RT=4, I18=0x20000), enc("bisl", RT=0, RA=4)]
        code = code_of(words)
        insn = code.insn(at(1))
        self.assertEqual(const_target(code, insn, flow_of(insn)), 0x20000)


class SpuCodeTest(unittest.TestCase):
    def test_segments_and_validity(self):
        a = b"".join(w.to_bytes(4, "big") for w in (enc("nop", RT=1), 0, 0x90000000))
        b = enc("lnop").to_bytes(4, "big")
        code = SpuCode([(0x100, a), (0x300, b)], [(0x200, bytes(16))])
        self.assertEqual(code.addrs, [0x100, 0x104, 0x108, 0x300])
        self.assertTrue(code.valid(0x100))
        self.assertFalse(code.valid(0x104))
        self.assertFalse(code.valid(0x108))
        self.assertTrue(code.contains(0x108))
        self.assertFalse(code.contains(0x200))
        self.assertFalse(code.contains(0x102))
        self.assertEqual(code.read_word(0x200), 0)
        self.assertIsNone(code.read_word(0x210))

    def test_from_image_keeps_data_segments_for_memory_reads(self):
        code_bytes = enc("nop", RT=1).to_bytes(4, "big")
        segments = [SimpleNamespace(ls=BASE, code=True, data=code_bytes),
                    SimpleNamespace(ls=DATA, code=False, data=(0x1234).to_bytes(4, "big"))]
        image = SimpleNamespace(segments=segments, segment_bytes=lambda seg: seg.data)
        code = SpuCode.from_image(image)
        self.assertEqual(code.addrs, [BASE])
        self.assertEqual(code.read_word(DATA), 0x1234)

    def test_overlapping_code_segments_are_rejected(self):
        with self.assertRaises(ValueError):
            SpuCode([(0x100, bytes(8)), (0x104, bytes(8))])

    def test_direct_targets_exclude_the_pc_read(self):
        words = [enc("brsl", RT=5, addr=at(0), target=at(1)), enc("brnz", RT=3, addr=at(1), target=at(0)),
                 enc("brsl", RT=0, addr=at(2), target=0x9000)]
        self.assertEqual(code_of(words).direct_targets, {at(0)})


if __name__ == "__main__":
    unittest.main()

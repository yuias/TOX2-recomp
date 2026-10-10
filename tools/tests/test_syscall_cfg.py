"""Tests for CFG-based lv2 syscall number resolution."""

import unittest

from ps3recomp.partition import partition
from ps3recomp.ppu_flow import CodeImage
from ps3recomp.probe import resolve_syscall_number
from ps3recomp.seeds import Seed
from ps3recomp.syscall_cfg import resolve_syscalls

from tests import ppc_asm as asm

BASE = 0x10000


def at(index: int) -> int:
    return BASE + 4 * index


def run(words, starts=(0,)):
    image = CodeImage.from_words(BASE, words)
    result = partition(image, [Seed(at(i), "ghidra") for i in starts], {}, {})
    assert result.converged
    return image, resolve_syscalls(image, result.functions, {})


class SyscallCfgTest(unittest.TestCase):
    def only_site(self, words, starts=(0,)):
        image, sites = run(words, starts)
        self.assertEqual(len(sites), 1)
        return image, sites[0]

    def test_two_paths_with_the_same_number_resolve(self):
        words = [asm.cmplwi(0, 3, 0), asm.bgt(0, at(1), at(4)), asm.li(11, 7),
                 asm.b(at(3), at(5)), asm.li(11, 7), asm.sc(), asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.addr, site.function, site.number, site.reason),
                         (at(5), at(0), 7, None))

    def test_two_paths_with_different_numbers_do_not_resolve(self):
        words = [asm.cmplwi(0, 3, 0), asm.bgt(0, at(1), at(4)), asm.li(11, 7),
                 asm.b(at(3), at(5)), asm.li(11, 8), asm.sc(), asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.number, site.reason), (None, "conflict"))

    def test_join_point_bringing_another_value_is_caught(self):
        # The linear scan stops at the nearest `li` and misses the path that skips it.
        words = [asm.li(11, 9), asm.cmplwi(0, 3, 0), asm.bgt(0, at(2), at(5)),
                 asm.li(11, 7), asm.nop(), asm.sc(), asm.blr()]
        image, site = self.only_site(words)
        self.assertEqual(resolve_syscall_number(image.words, 5), 7)
        self.assertEqual((site.number, site.reason), (None, "conflict"))

    def test_join_point_from_the_entry_is_unresolved(self):
        words = [asm.cmplwi(0, 3, 0), asm.bgt(0, at(1), at(4)), asm.li(11, 7),
                 asm.nop(), asm.sc(), asm.blr()]
        image, site = self.only_site(words)
        self.assertEqual(resolve_syscall_number(image.words, 4), 7)
        self.assertEqual((site.number, site.reason), (None, "entry"))

    def test_call_on_the_path_clobbers_r11(self):
        words = [asm.li(11, 5), asm.b(at(1), at(5), lk=True), asm.sc(), asm.blr(),
                 asm.nop(), asm.blr()]
        _, site = self.only_site(words, starts=(0, 5))
        self.assertEqual((site.number, site.reason), (None, "call"))

    def test_loop_back_to_the_site_resolves(self):
        words = [asm.li(11, 7), asm.sc(), asm.cmplwi(0, 3, 0), asm.bgt(0, at(3), at(1)),
                 asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.addr, site.number), (at(1), 7))

    def test_a_previous_syscall_is_transparent(self):
        words = [asm.li(11, 7), asm.sc(), asm.sc(), asm.blr()]
        _, sites = run(words)
        self.assertEqual([(s.addr, s.number) for s in sites], [(at(1), 7), (at(2), 7)])

    def test_unowned_site(self):
        words = [asm.blr(), asm.sc()]
        _, site = self.only_site(words)
        self.assertEqual((site.function, site.number, site.reason), (None, None, "unowned"))

    def test_r11_written_by_a_move(self):
        words = [asm.li(11, 7), asm.mr(11, 3), asm.sc(), asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.number, site.reason), (None, "r11_written"))

    def test_site_at_the_function_entry(self):
        words = [asm.sc(), asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.number, site.reason), (None, "entry"))

    def test_visit_limit(self):
        # A long straight run of no-ops with the `li` further back than the node limit.
        words = [asm.li(11, 7)] + [asm.nop()] * 600 + [asm.sc(), asm.blr()]
        _, site = self.only_site(words)
        self.assertEqual((site.number, site.reason), (None, "limit"))


if __name__ == "__main__":
    unittest.main()

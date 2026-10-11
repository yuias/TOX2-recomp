"""Tests for the whole-program SPU test generator (`tests.spu_programs.gen`)."""

import tempfile
import unittest
from pathlib import Path

from ps3recomp import spu_recover
from tests.spu_programs import gen


class GenerateTest(unittest.TestCase):
    def test_every_program_is_recovered_and_emitted(self):
        for make in gen.PROGRAMS:
            program = make()
            with self.subTest(program=program.name):
                result = gen.emit_program(program, 0)
                self.assertEqual(result.unhandled, 0)
                self.assertEqual(result.stats.invalid_flow, [])

    def test_jump_table_program_resolves_its_indirect_branch(self):
        program = gen.jump_table()
        image = gen.build_image(program, 0)
        rec = spu_recover.recover_image(image, image.sha256)
        (table,) = rec.tables.tables.values()
        self.assertEqual(table.kind, "table")
        self.assertEqual(table.table, gen.TABLE_LS)
        self.assertEqual(table.count, gen.TABLE_ENTRIES)
        self.assertEqual(rec.result.unresolved_indirect, ())

    def test_output_is_complete_and_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self.assertTrue(gen.generate(out))
            names = sorted(p.name for p in out.iterdir())
            for expected in ("spu_tables.c", "spu_funcs.h", "spu_programs_data.c", "spu_programs_data.h",
                             "spu_sum_array_000.c", "spu_atomic_counter_000.c", "spu_jump_table_000.c",
                             "spu_throw_event_000.c"):
                self.assertIn(expected, names)
            for path in out.iterdir():
                self.assertNotIn(b"\r", path.read_bytes(), path.name)
            self.assertFalse(gen.generate(out))


if __name__ == "__main__":
    unittest.main()

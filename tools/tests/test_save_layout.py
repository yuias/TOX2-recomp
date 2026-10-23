import struct
import unittest

from tox2_save import EXIT_FORMAT, CliError, save

from tests.save_fixture import build_save

LUDGER = save.CHARACTER_BASE + 8 * save.CHARACTER_STRIDE


class ParseSaveTest(unittest.TestCase):
    def test_default_save(self):
        parsed = save.parse_save(bytes(build_save()))
        self.assertEqual(parsed.total_size, save.SAVE_SIZE)
        self.assertEqual(len(parsed.data), save.SAVE_SIZE)
        self.assertEqual([(s.index, s.name, s.offset, s.size) for s in parsed.sections], [
            (0, "SAVE_DATA_COMMON", 0x2D0, 0x100),
            (1, "PLAYER_3_STATUS_SAVE_DATA", 0x8890, 0xA000),
            (2, "SAVE_DATA_ID_ITEM_DATA_MANAGER", 0x8A170, 0x100),
        ])

    def test_small_save(self):
        parsed = save.parse_save(bytes(build_save(0x400)))
        self.assertEqual([s.name for s in parsed.sections], ["SAVE_DATA_COMMON"])

    def test_total_size_may_be_smaller_than_the_file(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 4, 0x300)
        self.assertEqual(save.parse_save(bytes(buf)).total_size, 0x300)

    def assertRejected(self, data: bytes):
        with self.assertRaises(CliError) as ctx:
            save.parse_save(bytes(data))
        self.assertEqual(ctx.exception.code, EXIT_FORMAT)
        self.assertIn("decrypt", str(ctx.exception))

    def test_bad_magic(self):
        buf = build_save(0x400)
        struct.pack_into(">H", buf, 0, 0x0065)
        self.assertRejected(buf)

    def test_too_short(self):
        self.assertRejected(bytes(8))

    def test_section_table_past_the_end(self):
        buf = build_save(0x400)
        struct.pack_into(">H", buf, 2, 0xFFFF)
        self.assertRejected(buf)

    def test_name_offset_past_the_end(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x10, 0x400)
        self.assertRejected(buf)

    def test_unterminated_name(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x10, 0x3F0)
        buf[0x3F0:] = b"A" * 16
        self.assertRejected(buf)

    def test_name_too_long(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x10, 0x300)
        buf[0x300:0x300 + 65] = b"A" * 65
        self.assertRejected(buf)

    def test_empty_name_is_accepted(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x10, 0x300)
        self.assertEqual(save.parse_save(bytes(buf)).sections[0].name, "")

    def test_unprintable_name(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x10, 0x300)
        buf[0x300:0x304] = b"AB\x80\0"
        self.assertRejected(buf)

    def test_section_past_the_end(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 0x18, 0x1000)
        self.assertRejected(buf)

    def test_section_offset_plus_size_does_not_wrap(self):
        buf = build_save(0x400)
        struct.pack_into(">II", buf, 0x14, 0xFFFFFFF0, 0x20)
        self.assertRejected(buf)

    def test_total_size_larger_than_the_file(self):
        buf = build_save(0x400)
        struct.pack_into(">I", buf, 4, 0x401)
        self.assertRejected(buf)


class DescribeTest(unittest.TestCase):
    def test_fields(self):
        buf = build_save()
        struct.pack_into(">I", buf, save.GALD_OFFSET, 123456)
        struct.pack_into(">I", buf, save.PLAY_TIME_OFFSET, 789)
        struct.pack_into(">I", buf, LUDGER + save.HP, 4321)
        struct.pack_into(">I", buf, LUDGER + save.TP, 321)
        struct.pack_into(">I", buf, LUDGER + save.EXP, 987654)
        buf[LUDGER + save.AC] = 42
        struct.pack_into(">I", buf, LUDGER + save.SP, 77)
        for i in range(6):
            struct.pack_into(">H", buf, LUDGER + save.STATS + i * save.STAT_STRIDE, 100 + i)
        doc = save.describe(save.parse_save(bytes(buf)))

        self.assertEqual(doc["schema"], 1)
        self.assertEqual(doc["size"], save.SAVE_SIZE)
        self.assertEqual(doc["total_size"], save.SAVE_SIZE)
        self.assertEqual(doc["gald"], 123456)
        self.assertEqual(doc["play_time_raw"], 789)
        self.assertEqual(doc["sections"][1], {
            "index": 1,
            "name": "PLAYER_3_STATUS_SAVE_DATA",
            "offset": "0x00008890",
            "size": 0xA000
        })
        self.assertEqual([c["name"] for c in doc["characters"]], list(save.CHARACTERS))
        ludger = doc["characters"][8]
        self.assertEqual(ludger, {
            "index": 8,
            "name": "Ludger",
            "offset": "0x%08x" % LUDGER,
            "hp": 4321,
            "tp": 321,
            "exp": 987654,
            "ac": 42,
            "sp": 77,
            "strength": 100,
            "intelligence": 101,
            "vitality": 102,
            "dexterity": 103,
            "agility": 104,
            "spirit": 105,
        })
        self.assertEqual(doc["characters"][0]["hp"], 0)
        self.assertEqual(doc["characters"][0]["offset"], "0x00008890")

    def test_characters_do_not_overlap(self):
        buf = build_save()
        struct.pack_into(">I", buf, save.CHARACTER_BASE + save.CHARACTER_STRIDE + save.HP, 55)
        doc = save.describe(save.parse_save(bytes(buf)))
        self.assertEqual([c["hp"] for c in doc["characters"]], [0, 55, 0, 0, 0, 0, 0, 0, 0])

    def test_short_save_parses_but_cannot_be_described(self):
        parsed = save.parse_save(bytes(build_save(0x400)))
        with self.assertRaises(CliError) as ctx:
            save.describe(parsed)
        self.assertEqual(ctx.exception.code, EXIT_FORMAT)


if __name__ == "__main__":
    unittest.main()

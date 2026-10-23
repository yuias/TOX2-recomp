import unittest

from tox2_save import EXIT_FORMAT, EXIT_USAGE, CliError, codes


def run(*lines, size=0x100):
    buf = bytearray(size)
    codes.apply_writes(buf, codes.compile_code([(i + 1, s) for i, s in enumerate(lines)], "t"))
    return buf


class CompileTest(unittest.TestCase):
    def test_types_0_1_2_write_big_endian(self):
        buf = run("00000010 000000AB", "10000020 00001234", "20000030 12345678")
        self.assertEqual(buf[0x10], 0xAB)
        self.assertEqual(bytes(buf[0x20:0x22]), b"\x12\x34")
        self.assertEqual(bytes(buf[0x30:0x34]), b"\x12\x34\x56\x78")
        self.assertEqual(sum(buf), 0xAB + 0x12 + 0x34 + 0x12 + 0x34 + 0x56 + 0x78)

    def test_lowercase_hex_and_tabs(self):
        self.assertEqual(run("0000000a\t000000ff")[0x0A], 0xFF)

    def test_type_4_16bit_wraps(self):
        buf = run("41000000 0000FFFF", "40030002 00000001")
        self.assertEqual(bytes(buf[0:6]), b"\xff\xff\x00\x00\x00\x01")

    def test_type_4_8bit_and_32bit(self):
        buf = run("40000010 000000FE", "40030001 00000001")
        self.assertEqual(bytes(buf[0x10:0x13]), b"\xfe\xff\x00")
        buf = run("42000000 FFFFFFFF", "40020004 00000002")
        self.assertEqual(bytes(buf[0:8]), b"\xff\xff\xff\xff\x00\x00\x00\x01")

    def test_type_4_nine_writes(self):
        buf = run("4100D1BA 0000270F", "4009A000 00000000", size=0x8890 + 9 * 0xA000)
        offsets = [0xD1BA + i * 0xA000 for i in range(9)]
        for off in offsets:
            self.assertEqual(bytes(buf[off : off + 2]), b"\x27\x0f")
        self.assertEqual(sum(buf), 9 * (0x27 + 0x0F))

    def test_later_write_wins(self):
        self.assertEqual(run("00000000 00000001", "00000000 00000002")[0], 2)

    def test_type_4_then_plain_line(self):
        buf = run("40000000 00000001", "40020001 00000001", "00000008 00000009")
        self.assertEqual(bytes(buf[0:3]), b"\x01\x02\x00")
        self.assertEqual(buf[8], 9)


class CompileErrorTest(unittest.TestCase):
    def assertFormat(self, *lines, size=0x100, line=None):
        with self.assertRaises(CliError) as cm:
            run(*lines, size=size)
        self.assertEqual(cm.exception.code, EXIT_FORMAT)
        if line is not None:
            self.assertIn(f"t:{line}:", str(cm.exception))
        return str(cm.exception)

    def test_offset_past_end_leaves_buffer_unchanged(self):
        buf = bytearray(0x100)
        writes = codes.compile_code([(1, "00000000 00000001"), (2, "00000100 00000001")], "t")
        with self.assertRaises(CliError) as cm:
            codes.apply_writes(buf, writes)
        self.assertEqual(cm.exception.code, EXIT_FORMAT)
        self.assertIn("t:2:", str(cm.exception))
        self.assertIn("0x000100", str(cm.exception))
        self.assertEqual(buf, bytearray(0x100))

    def test_wide_write_straddling_the_end(self):
        self.assertFormat("200000FE 00000001", line=1)

    def test_type_3(self):
        msg = self.assertFormat("00000000 00000001", "30000000 00000001", line=2)
        self.assertIn("type 3", msg)

    def test_type_0_with_t8(self):
        msg = self.assertFormat("08000000 00000001", line=1)
        self.assertIn("T=8", msg)

    def test_value_too_wide(self):
        self.assertFormat("00000000 00000100", line=1)
        self.assertFormat("10000000 00010000", line=1)
        self.assertFormat("41000000 00010000", "40010002 00000001", line=1)

    def test_type_4_without_second_line(self):
        self.assertFormat("40000000 00000001", line=1)

    def test_type_4_second_line_not_type_4(self):
        self.assertFormat("40000000 00000001", "00000000 00000001", line=2)

    def test_type_4_zero_count(self):
        self.assertFormat("40000000 00000001", "40000001 00000000", line=2)

    def test_type_4_bad_t(self):
        self.assertFormat("43000000 00000001", "40010001 00000000", line=1)

    def test_not_hex(self):
        self.assertFormat("0000000G 00000001", line=1)
        self.assertFormat("00000000", line=1)
        self.assertFormat("00000000 00000001 00000002", line=1)

    def test_empty_code(self):
        with self.assertRaises(CliError) as cm:
            codes.compile_code([], "t")
        self.assertEqual(cm.exception.code, EXIT_FORMAT)


class SavepatchTest(unittest.TestCase):
    TEXT = (
        "; comment\n"
        "\n"
        ":SAVE\n"
        "[GROUP:a]\n"
        "[Plain]\n"
        "00000000 00000001\n"
        "; inside a code\n"
        "\n"
        "00000001 00000002\n"
        "[GROUP:a\\---]\n"
        "[Name=with, odd: chars/here]\n"
        "00000002 00000003\n"
        ":OTHER\n"
        "[Other target]\n"
        "00000003 00000004\n"
    )

    def test_parse(self):
        sp = codes.parse_savepatch(self.TEXT, "f.savepatch")
        self.assertEqual([c.name for c in sp.codes], ["Plain", "Name=with, odd: chars/here", "Other target"])
        plain, odd, other = sp.codes
        self.assertEqual((plain.group, plain.target, plain.line_no), ("a", "SAVE", 5))
        self.assertEqual(plain.lines, ((6, "00000000 00000001"), (9, "00000001 00000002")))
        self.assertEqual((odd.group, odd.target), ("a\\---", "SAVE"))
        self.assertEqual(other.target, "OTHER")

    def test_crlf_and_bom(self):
        text = "\ufeff:SAVE\r\n[A]\r\n00000000 00000001\r\n"
        sp = codes.parse_savepatch(text, "f")
        self.assertEqual(sp.codes[0].name, "A")
        self.assertEqual(sp.codes[0].target, "SAVE")
        self.assertEqual(sp.codes[0].lines, ((3, "00000000 00000001"),))

    def test_no_target(self):
        sp = codes.parse_savepatch("[A]\n00000000 00000001\n", "f")
        self.assertIsNone(sp.codes[0].target)
        self.assertIsNone(sp.codes[0].group)

    def test_group_ends_code(self):
        sp = codes.parse_savepatch("[A]\n00000000 00000001\n[GROUP:g]\n[B]\n00000001 00000001\n", "f")
        self.assertEqual([(c.name, c.group, len(c.lines)) for c in sp.codes], [("A", None, 1), ("B", "g", 1)])

    def test_line_outside_a_code(self):
        for text in ("00000000 00000001\n", "[GROUP:g]\n00000000 00000001\n", ":SAVE\n00000000 00000001\n"):
            with self.assertRaises(CliError) as cm:
                codes.parse_savepatch(text, "f.sp")
            self.assertEqual(cm.exception.code, EXIT_FORMAT)
            self.assertIn("line outside a code", str(cm.exception))
            self.assertIn("f.sp:", str(cm.exception))

    def test_empty_target_name(self):
        with self.assertRaises(CliError) as cm:
            codes.parse_savepatch("[A]\n:\n", "f")
        self.assertEqual(cm.exception.code, EXIT_FORMAT)
        self.assertIn("f:2:", str(cm.exception))

    def test_bad_body_is_only_checked_on_compile(self):
        sp = codes.parse_savepatch("[Bad]\nzzz\n[Good]\n00000000 00000001\n", "f")
        self.assertEqual(sp.find("Good").lines, ((4, "00000000 00000001"),))
        with self.assertRaises(CliError):
            codes.compile_code(sp.find("Bad").lines, "f")

    def test_find_unknown_is_usage(self):
        sp = codes.parse_savepatch("[A]\n00000000 00000001\n", "f.sp")
        with self.assertRaises(CliError) as cm:
            sp.find("B")
        self.assertEqual(cm.exception.code, EXIT_USAGE)
        self.assertIn("'B'", str(cm.exception))
        self.assertIn("f.sp", str(cm.exception))

    def test_find_duplicate_is_format(self):
        sp = codes.parse_savepatch("[A]\n00000000 00000001\n\n[A]\n00000000 00000002\n", "f")
        with self.assertRaises(CliError) as cm:
            sp.find("A")
        self.assertEqual(cm.exception.code, EXIT_FORMAT)
        self.assertIn("lines 1, 4", str(cm.exception))

    def test_group_header_is_not_selectable(self):
        sp = codes.parse_savepatch("[GROUP:g]\n[A]\n00000000 00000001\n", "f")
        with self.assertRaises(CliError) as cm:
            sp.find("GROUP:g")
        self.assertEqual(cm.exception.code, EXIT_USAGE)


class ParseRawTest(unittest.TestCase):
    def test_one_pair(self):
        self.assertEqual(codes.parse_raw("00000010 000000ab", 1), [(1, "00000010 000000ab")])

    def test_two_pairs_make_a_type_4_code(self):
        lines = codes.parse_raw("41000000 0000FFFF\n  40030002\t00000001", 2)
        self.assertEqual(lines, [(1, "41000000 0000FFFF"), (2, "40030002 00000001")])
        buf = bytearray(8)
        codes.apply_writes(buf, codes.compile_code(lines, "--raw 2"))
        self.assertEqual(bytes(buf[:6]), b"\xff\xff\x00\x00\x00\x01")

    def test_odd_token_count(self):
        with self.assertRaises(CliError) as cm:
            codes.parse_raw("00000000 00000001 00000002", 1)
        self.assertEqual(cm.exception.code, EXIT_FORMAT)
        self.assertIn("--raw 1", str(cm.exception))

    def test_bad_token(self):
        for text in ("0000000 00000001", "0000000G 00000001", "000000000 00000001"):
            with self.assertRaises(CliError) as cm:
                codes.parse_raw(text, 3)
            self.assertEqual(cm.exception.code, EXIT_FORMAT)

    def test_error_names_the_raw_index(self):
        with self.assertRaises(CliError) as cm:
            codes.compile_code(codes.parse_raw("30000000 00000001", 4), "--raw 4")
        self.assertIn("--raw 4:1:", str(cm.exception))


if __name__ == "__main__":
    unittest.main()

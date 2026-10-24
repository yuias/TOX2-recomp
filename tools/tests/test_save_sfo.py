import struct
import unittest

from tox2_save import CliError, sfo


def _entry_table(entries):
    """Builds a PARAM.SFO from (key, format, length, capacity, payload) tuples, in the given order."""
    key_table = 20 + 16 * len(entries)
    keys = b"".join(k.encode() + b"\0" for k, *_ in entries)
    data_table = key_table + (len(keys) + 3) // 4 * 4
    out = struct.pack("<4sIIII", b"\0PSF", 0x101, key_table, data_table, len(entries))
    key_off = data_off = 0
    for key, fmt, length, capacity, _ in entries:
        out += struct.pack("<HHIII", key_off, fmt, length, capacity, data_off)
        key_off += len(key) + 1
        data_off += capacity
    out += keys.ljust(data_table - len(out), b"\0")
    for _, _, _, capacity, payload in entries:
        out += payload.ljust(capacity, b"\0")
    return out


def _us_like() -> sfo.Sfo:
    return sfo.Sfo(
        ints={"ATTRIBUTE": 5, "PARENTAL_LEVEL": 3},
        strings={
            "ACCOUNT_ID": "1234567890abcdef",
            "TITLE": "Title",
            "SUB_TITLE": "Sub",
            "DETAIL": "Detail",
            "SAVEDATA_LIST_PARAM": "list",
            "SAVEDATA_DIRECTORY": "BLUS31397_SAVE1_000",
            "CATEGORY": "SD",
        },
        binary={"PARAMS": bytes(range(16)), "PARAMS2": bytes(8)},
    )


class SerializeTest(unittest.TestCase):
    def test_hand_built_bytes(self):
        data = sfo.serialize_sfo(sfo.Sfo(ints={"*SAVE": 1}, strings={"TITLE": "Hi", "CATEGORY": "SD"}))
        # Header, three index entries, keys "*SAVE\0CATEGORY\0TITLE\0" padded to 24, then data slots 4, 4 and 128.
        expected = struct.pack("<4sIIII", b"\0PSF", 0x101, 68, 92, 3)
        expected += struct.pack("<HHIII", 0, 0x0404, 4, 4, 0)
        expected += struct.pack("<HHIII", 6, 0x0204, 3, 4, 4)
        expected += struct.pack("<HHIII", 15, 0x0204, 3, 128, 8)
        expected += b"*SAVE\0CATEGORY\0TITLE\0" + bytes(3)
        expected += struct.pack("<I", 1) + b"SD\0\0" + b"Hi\0".ljust(128, b"\0")
        self.assertEqual(data, expected)

    def test_other_text_capacity_is_the_length_aligned(self):
        data = sfo.serialize_sfo(sfo.Sfo(strings={"DETAIL": "a" * 6}))
        self.assertEqual(struct.unpack_from("<HHIII", data, 20), (0, 0x0204, 7, 8, 0))
        self.assertEqual(len(data), 20 + 16 + 8 + 8)

    def test_sorted_by_key_bytes(self):
        data = sfo.serialize_sfo(sfo.Sfo(ints={"b": 1, "B": 2, "*a": 3}))
        self.assertEqual(sfo.parse_sfo(data).ints, {"*a": 3, "B": 2, "b": 1})
        self.assertEqual(data[data.index(b"*a"):][:7], b"*a\0B\0b\0")

    def test_binary_entries_are_rejected(self):
        with self.assertRaises(ValueError):
            sfo.serialize_sfo(sfo.Sfo(binary={"PARAMS": b"x"}))

    def test_key_in_both_dicts_is_rejected(self):
        with self.assertRaises(ValueError):
            sfo.serialize_sfo(sfo.Sfo(ints={"X": 1}, strings={"X": "y"}))

    def test_round_trip(self):
        original = sfo.Sfo(ints={"*SAVE": 1, "ATTRIBUTE": 0}, strings={"TITLE": "Tales é", "TITLE_ID": "BLJS10188"})
        data = sfo.serialize_sfo(original)
        parsed = sfo.parse_sfo(data)
        self.assertEqual(parsed, original)
        self.assertEqual(sfo.serialize_sfo(parsed), data)


class ParseTest(unittest.TestCase):
    def test_binary_entry(self):
        data = _entry_table([("PARAMS", 0x0004, 5, 8, b"\x01\x00\x02\x00\x03"), ("TITLE", 0x0204, 3, 8, b"Hi\0")])
        parsed = sfo.parse_sfo(data)
        self.assertEqual(parsed.binary, {"PARAMS": b"\x01\x00\x02\x00\x03"})
        self.assertEqual(parsed.strings, {"TITLE": "Hi"})
        self.assertEqual(parsed.ints, {})

    def test_text_stops_at_the_first_nul(self):
        data = _entry_table([("TITLE", 0x0204, 8, 8, b"ab\0cd\0\0\0")])
        self.assertEqual(sfo.parse_sfo(data).strings, {"TITLE": "ab"})

    def test_unknown_format_is_skipped(self):
        data = _entry_table([("A", 0x0999, 4, 4, b"xxxx"), ("B", 0x0404, 4, 4, struct.pack("<I", 7))])
        parsed = sfo.parse_sfo(data)
        self.assertEqual((parsed.ints, parsed.strings, parsed.binary), ({"B": 7}, {}, {}))

    def assertFormatError(self, data):
        with self.assertRaises(CliError) as cm:
            sfo.parse_sfo(data)
        self.assertEqual(cm.exception.code, 3)

    def test_bad_magic(self):
        data = bytearray(_entry_table([("A", 0x0404, 4, 4, bytes(4))]))
        data[1] = ord("X")
        self.assertFormatError(bytes(data))
        self.assertFormatError(b"")

    def test_truncated_index(self):
        data = _entry_table([("A", 0x0404, 4, 4, bytes(4)), ("B", 0x0404, 4, 4, bytes(4))])
        self.assertFormatError(data[:20 + 16])
        self.assertFormatError(data[:10])

    def test_data_out_of_range(self):
        data = _entry_table([("A", 0x0404, 4, 4, bytes(4))])
        self.assertFormatError(data[:-1])
        self.assertFormatError(_entry_table([("T", 0x0204, 0x1000, 8, b"x")]))
        self.assertFormatError(_entry_table([("A", 0x0404, 2, 4, bytes(4))]))

    def test_key_out_of_range_or_unterminated(self):
        data = bytearray(_entry_table([("A", 0x0404, 4, 4, bytes(4))]))
        struct.pack_into("<H", data, 20, 0xFFFF)
        self.assertFormatError(bytes(data))
        # The key table runs to the end of the file with no NUL.
        self.assertFormatError(struct.pack("<4sIIII", b"\0PSF", 0x101, 36, 36, 1) +
                               struct.pack("<HHIII", 0, 0x0404, 4, 4, 0) + b"AAAA")

    def test_duplicate_key(self):
        data = _entry_table([("A", 0x0404, 4, 4, bytes(4)), ("A", 0x0204, 2, 4, b"x\0")])
        self.assertFormatError(data)

    def test_invalid_utf8_text(self):
        self.assertFormatError(_entry_table([("T", 0x0204, 3, 4, b"\xff\xfe\0")]))


class InstallSfoTest(unittest.TestCase):
    def test_design_key_set(self):
        result = sfo.build_install_sfo(_us_like(), "BLJS10188_SAVE1_000", ["SAVE", "ICON0.PNG"])
        self.assertEqual(
            result.ints, {"ATTRIBUTE": 5, "PARENTAL_LEVEL": 3, "*SAVE": 1, "*ICON0.PNG": 0})
        self.assertEqual(
            result.strings, {
                "ACCOUNT_ID": "0000000000000000",
                "CATEGORY": "SD",
                "SAVEDATA_DIRECTORY": "BLJS10188_SAVE1_000",
                "DETAIL": "Detail",
                "SAVEDATA_LIST_PARAM": "list",
                "SUB_TITLE": "Sub",
                "TITLE": "Title",
            })
        self.assertEqual(result.binary, {})
        sfo.serialize_sfo(result)

    def test_missing_source_keys_default(self):
        result = sfo.build_install_sfo(sfo.Sfo(), "X", ["SAVE"])
        self.assertEqual(result.ints, {"ATTRIBUTE": 0, "PARENTAL_LEVEL": 0, "*SAVE": 1})
        for key in ("DETAIL", "SAVEDATA_LIST_PARAM", "SUB_TITLE", "TITLE"):
            self.assertEqual(result.strings[key], "")

    def test_source_secure_flag_is_kept(self):
        source = _us_like()
        source.ints["*SAVE"] = 0
        source.ints["*ICON0.PNG"] = 1
        result = sfo.build_install_sfo(source, "X", ["SAVE", "ICON0.PNG"])
        self.assertEqual((result.ints["*SAVE"], result.ints["*ICON0.PNG"]), (0, 1))

    def test_param_files_are_not_listed(self):
        result = sfo.build_install_sfo(sfo.Sfo(), "X", ["SAVE", "PARAM.SFO", "PARAM.PFD", "OTHER.BIN"])
        self.assertEqual({k for k in result.ints if k.startswith("*")}, {"*SAVE", "*OTHER.BIN"})

    def test_install_sfo_round_trips(self):
        result = sfo.build_install_sfo(_us_like(), "X", ["SAVE", "ICON0.PNG"])
        self.assertEqual(sfo.parse_sfo(sfo.serialize_sfo(result)), result)


if __name__ == "__main__":
    unittest.main()

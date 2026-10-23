import struct
import unittest

from tox2_save import EXIT_FORMAT, EXIT_VERIFY, CliError, pfd

from tests.save_fixture import FAKE_SECURE_ID, FAKE_SYSCON_KEY, build_pfd_dir

TABLE = 0x78 + 7 * 8  # entry table offset for the fixture's default capacity of 7
ENTRY = 272


def _sample_files() -> dict[str, bytes]:
    return {
        "SAVE": bytes((i * 7 + 3) & 0xFF for i in range(1000)),
        "PARAM.SFO": b"\x00PSF plain sfo bytes",
        "ICON0.PNG": b"not listed in the pfd",
    }


def _build(**kwargs) -> dict[str, bytes]:
    return build_pfd_dir(_sample_files(), ["SAVE"], syscon_key=FAKE_SYSCON_KEY, secure_file_id=FAKE_SECURE_ID,
                         **kwargs)


def _patched(data: bytes, offset: int, raw: bytes) -> bytes:
    return data[:offset] + raw + data[offset + len(raw):]


class HashKeyTest(unittest.TestCase):
    def test_fixed_positions(self):
        self.assertEqual(pfd.hash_key(bytes(range(16))).hex(), "000b0f01020e03040a05060708090a0b0c0d0e0f")

    def test_wrong_length(self):
        with self.assertRaises(ValueError):
            pfd.hash_key(bytes(15))


class DataTest(unittest.TestCase):
    def test_round_trip_with_unaligned_size(self):
        key = bytes(range(16))
        plain = bytes((i * 31) & 0xFF for i in range(1000))
        stored = pfd.encrypt_data(key, plain)
        self.assertEqual(len(stored), 1008)
        self.assertEqual(pfd.decrypt_data(key, stored)[:1000], plain)

    def test_blocks_use_their_counter(self):
        stored = pfd.encrypt_data(bytes(range(16)), bytes(32))
        self.assertNotEqual(stored[:16], stored[16:])

    def test_decrypt_data_needs_whole_blocks(self):
        with self.assertRaises(ValueError):
            pfd.decrypt_data(bytes(16), bytes(15))


class ParsePfdTest(unittest.TestCase):
    def test_entries(self):
        for version in (3, 4):
            with self.subTest(version=version):
                parsed = pfd.parse_pfd(_build(version=version)["PARAM.PFD"])
                self.assertEqual(parsed.version, version)
                self.assertEqual(parsed.capacity, 7)
                self.assertEqual(parsed.num_reserved, 4)
                self.assertEqual([e.name for e in parsed.entries], ["SAVE", "PARAM.SFO"])
                self.assertEqual([e.file_size for e in parsed.entries], [1000, len(_sample_files()["PARAM.SFO"])])
                self.assertEqual([e.index for e in parsed.entries], [0, 1])
                self.assertEqual(len(parsed.entries[0].key), 64)
                self.assertEqual(len(parsed.entries[0].file_hashes), 4)

    def test_plain_entry_name(self):
        self.assertTrue(pfd.is_plain_entry("PARAM.SFO"))
        self.assertTrue(pfd.is_plain_entry("param.sfo"))
        self.assertFalse(pfd.is_plain_entry("SAVE"))

    def assertFormatError(self, data: bytes):
        with self.assertRaises(CliError) as ctx:
            pfd.parse_pfd(data)
        self.assertEqual(ctx.exception.code, EXIT_FORMAT)

    def test_bad_magic(self):
        self.assertFormatError(_patched(_build()["PARAM.PFD"], 0, struct.pack(">Q", 0x50464443)))

    def test_unsupported_version(self):
        self.assertFormatError(_build(version=5)["PARAM.PFD"])

    def test_too_short(self):
        self.assertFormatError(bytes(0x20))

    def test_truncated_entry_table(self):
        self.assertFormatError(_build()["PARAM.PFD"][:-1])

    def test_used_exceeds_reserved(self):
        self.assertFormatError(_patched(_build()["PARAM.PFD"], 0x70, struct.pack(">Q", 5)))

    def test_table_sizes_are_bounded(self):
        data = _build()["PARAM.PFD"]
        self.assertFormatError(_patched(data, 0x60, struct.pack(">Q", 1 << 40)))
        self.assertFormatError(_patched(data, 0x68, struct.pack(">Q", 1 << 40)))

    def test_unsafe_name(self):
        self.assertFormatError(_patched(_build()["PARAM.PFD"], TABLE + 8, b"../x\0"))

    def test_unterminated_name(self):
        self.assertFormatError(_patched(_build()["PARAM.PFD"], TABLE + 8, b"A" * 65))

    def test_duplicate_name(self):
        self.assertFormatError(_patched(_build()["PARAM.PFD"], TABLE + ENTRY + 8, b"SAVE\0"))


class DecryptEntryTest(unittest.TestCase):
    def setUp(self):
        self.files = _build()
        self.entry = pfd.parse_pfd(self.files["PARAM.PFD"]).entries[0]

    def test_right_id(self):
        plain = pfd.decrypt_entry(self.entry, self.files["SAVE"], FAKE_SYSCON_KEY, FAKE_SECURE_ID)
        self.assertEqual(plain, _sample_files()["SAVE"])

    def test_other_files_are_stored_plain(self):
        self.assertEqual(self.files["PARAM.SFO"], _sample_files()["PARAM.SFO"])
        self.assertEqual(self.files["ICON0.PNG"], _sample_files()["ICON0.PNG"])

    def test_wrong_secure_id(self):
        with self.assertRaises(CliError) as ctx:
            pfd.decrypt_entry(self.entry, self.files["SAVE"], FAKE_SYSCON_KEY, bytes(range(0x50, 0x60)))
        self.assertEqual(ctx.exception.code, EXIT_VERIFY)
        self.assertIn("'SAVE'", str(ctx.exception))
        self.assertIn("--secure-file-id", str(ctx.exception))

    def test_stored_file_too_short(self):
        with self.assertRaises(CliError) as ctx:
            pfd.decrypt_entry(self.entry, self.files["SAVE"][:-16], FAKE_SYSCON_KEY, FAKE_SECURE_ID)
        self.assertEqual(ctx.exception.code, EXIT_FORMAT)

    def test_modified_file_fails_hash(self):
        stored = bytearray(self.files["SAVE"])
        stored[0] ^= 1
        with self.assertRaises(CliError) as ctx:
            pfd.decrypt_entry(self.entry, bytes(stored), FAKE_SYSCON_KEY, FAKE_SECURE_ID)
        self.assertEqual(ctx.exception.code, EXIT_VERIFY)

    def test_wrong_syscon_key_gives_wrong_bytes(self):
        # The hash check does not cover the syscon key; the caller detects it through the SAVE header.
        plain = pfd.decrypt_entry(self.entry, self.files["SAVE"], bytes(16), FAKE_SECURE_ID)
        self.assertNotEqual(plain, _sample_files()["SAVE"])
        self.assertEqual(len(plain), 1000)


if __name__ == "__main__":
    unittest.main()

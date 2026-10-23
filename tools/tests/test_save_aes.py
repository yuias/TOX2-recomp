import unittest

from tox2_save import CliError, EXIT_FORMAT, EXIT_OK, aes

FIPS_KEY = bytes(range(16))
FIPS_PLAIN = bytes.fromhex("00112233445566778899aabbccddeeff")
FIPS_CIPHER = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")

# NIST SP 800-38A F.2.1/F.2.2 (CBC-AES128), the same vector as runtime/tests/crypto_test.cpp.
CBC_KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
CBC_IV = bytes(range(16))
CBC_PLAIN = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a"
                          "ae2d8a571e03ac9c9eb76fac45af8e51"
                          "30c81c46a35ce411e5fbc1191a0a52ef"
                          "f69f2445df4f9b17ad2b417be66c3710")
CBC_CIPHER = bytes.fromhex("7649abac8119b246cee98e9b12e9197d"
                           "5086cb9b507219ee95db113a917678b2"
                           "73bed6b8e3c1743b7116e69e22229516"
                           "3ff1caa1681fac09120eca307586e1a7")


class ExitCodesTest(unittest.TestCase):
    def test_cli_error_carries_code_and_message(self):
        err = CliError(EXIT_FORMAT, "bad thing")
        self.assertEqual(err.code, EXIT_FORMAT)
        self.assertEqual(str(err), "bad thing")
        self.assertEqual(EXIT_OK, 0)


class BlockKatTest(unittest.TestCase):
    def test_fips197_c1_encrypt(self):
        self.assertEqual(aes.Aes128(FIPS_KEY).encrypt_block(FIPS_PLAIN), FIPS_CIPHER)

    def test_fips197_c1_decrypt(self):
        self.assertEqual(aes.Aes128(FIPS_KEY).decrypt_block(FIPS_CIPHER), FIPS_PLAIN)

    def test_ecb_identical_blocks(self):
        cipher = aes.ecb_encrypt(FIPS_KEY, FIPS_PLAIN * 3)
        self.assertEqual(cipher, FIPS_CIPHER * 3)
        self.assertEqual(aes.ecb_decrypt(FIPS_KEY, cipher), FIPS_PLAIN * 3)


class CbcKatTest(unittest.TestCase):
    def test_sp800_38a_cbc_encrypt(self):
        self.assertEqual(aes.cbc_encrypt(CBC_KEY, CBC_IV, CBC_PLAIN), CBC_CIPHER)

    def test_sp800_38a_cbc_decrypt(self):
        self.assertEqual(aes.cbc_decrypt(CBC_KEY, CBC_IV, CBC_CIPHER), CBC_PLAIN)


class ArgumentTest(unittest.TestCase):
    def test_short_key_rejected(self):
        with self.assertRaises(ValueError):
            aes.Aes128(bytes(15))
        with self.assertRaises(ValueError):
            aes.ecb_encrypt(bytes(15), bytes(16))

    def test_unaligned_data_rejected(self):
        for func in (aes.ecb_encrypt, aes.ecb_decrypt):
            with self.assertRaises(ValueError):
                func(bytes(16), bytes(15))
        for func in (aes.cbc_encrypt, aes.cbc_decrypt):
            with self.assertRaises(ValueError):
                func(bytes(16), bytes(16), bytes(15))

    def test_short_iv_rejected(self):
        with self.assertRaises(ValueError):
            aes.cbc_encrypt(bytes(16), bytes(15), bytes(16))


class RoundTripTest(unittest.TestCase):
    def test_round_trip_64_blocks(self):
        key = bytes((i * 37 + 11) & 0xFF for i in range(16))
        iv = bytes((i * 5 + 3) & 0xFF for i in range(16))
        data = bytes((i * 131 + (i >> 3)) & 0xFF for i in range(64 * 16))
        ecb = aes.ecb_encrypt(key, data)
        self.assertNotEqual(ecb, data)
        self.assertEqual(aes.ecb_decrypt(key, ecb), data)
        cbc = aes.cbc_encrypt(key, iv, data)
        self.assertNotEqual(cbc, ecb)
        self.assertEqual(aes.cbc_decrypt(key, iv, cbc), data)


if __name__ == "__main__":
    unittest.main()

"""Pure-Python AES-128 (ECB and CBC), so that the save tool and its tests need no third-party package."""

from __future__ import annotations

import struct


def _build_sbox() -> tuple[list[int], list[int]]:
    # exp/log tables of GF(2^8) with generator 3 give the multiplicative inverse for the S-box.
    exp = [0] * 255
    log = [0] * 256
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        x ^= ((x << 1) ^ (0x11B if x & 0x80 else 0)) & 0xFF
    sbox = [0] * 256
    for v in range(256):
        inv = 0 if v == 0 else exp[(255 - log[v]) % 255]
        # Affine map: b ^ rotl(b,1) ^ rotl(b,2) ^ rotl(b,3) ^ rotl(b,4) ^ 0x63.
        s = inv
        for shift in (1, 2, 3, 4):
            s ^= ((inv << shift) | (inv >> (8 - shift))) & 0xFF
        sbox[v] = s ^ 0x63
    inv_sbox = [0] * 256
    for v, s in enumerate(sbox):
        inv_sbox[s] = v
    return sbox, inv_sbox


def _xtime(v: int) -> int:
    v <<= 1
    return (v ^ 0x11B) if v & 0x100 else v


def _gmul(a: int, b: int) -> int:
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = _xtime(a)
        b >>= 1
    return result


def _ror8(w: int) -> int:
    return ((w >> 8) | (w << 24)) & 0xFFFFFFFF


def _build_tables():
    sbox, inv_sbox = _build_sbox()
    te0 = [0] * 256
    td0 = [0] * 256
    for x in range(256):
        s = sbox[x]
        te0[x] = (_gmul(s, 2) << 24) | (s << 16) | (s << 8) | _gmul(s, 3)
        si = inv_sbox[x]
        td0[x] = (_gmul(si, 14) << 24) | (_gmul(si, 9) << 16) | (_gmul(si, 13) << 8) | _gmul(si, 11)
    te = [te0]
    td = [td0]
    for _ in range(3):
        te.append([_ror8(w) for w in te[-1]])
        td.append([_ror8(w) for w in td[-1]])
    return sbox, inv_sbox, te, td


_SBOX, _INV_SBOX, _TE, _TD = _build_tables()

_ROUNDS = 10


def _expand_key(key: bytes) -> list[int]:
    words = list(struct.unpack(">4I", key))
    rcon = 1
    for i in range(4, 4 * (_ROUNDS + 1)):
        t = words[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = ((_SBOX[t >> 24] << 24) | (_SBOX[(t >> 16) & 255] << 16) | (_SBOX[(t >> 8) & 255] << 8) |
                 _SBOX[t & 255])
            t ^= rcon << 24
            rcon = _xtime(rcon)
        words.append(words[i - 4] ^ t)
    return words


def _decryption_keys(enc: list[int]) -> list[int]:
    # Equivalent inverse cipher: round keys in reverse order, InvMixColumns on all but the first and last.
    td0, td1, td2, td3 = _TD
    dec: list[int] = []
    for r in range(_ROUNDS, -1, -1):
        words = enc[4 * r:4 * r + 4]
        if 0 < r < _ROUNDS:
            words = [td0[_SBOX[w >> 24]] ^ td1[_SBOX[(w >> 16) & 255]] ^ td2[_SBOX[(w >> 8) & 255]] ^
                     td3[_SBOX[w & 255]] for w in words]
        dec.extend(words)
    return dec


class Aes128:
    def __init__(self, key: bytes):
        if len(key) != 16:
            raise ValueError("AES-128 key must be 16 bytes")
        self._enc_keys = _expand_key(bytes(key))
        self._dec_keys = _decryption_keys(self._enc_keys)

    def _encrypt_words(self, s0: int, s1: int, s2: int, s3: int) -> tuple[int, int, int, int]:
        rk = self._enc_keys
        te0, te1, te2, te3 = _TE
        s0 ^= rk[0]
        s1 ^= rk[1]
        s2 ^= rk[2]
        s3 ^= rk[3]
        k = 4
        for _ in range(_ROUNDS - 1):
            t0 = te0[s0 >> 24] ^ te1[(s1 >> 16) & 255] ^ te2[(s2 >> 8) & 255] ^ te3[s3 & 255] ^ rk[k]
            t1 = te0[s1 >> 24] ^ te1[(s2 >> 16) & 255] ^ te2[(s3 >> 8) & 255] ^ te3[s0 & 255] ^ rk[k + 1]
            t2 = te0[s2 >> 24] ^ te1[(s3 >> 16) & 255] ^ te2[(s0 >> 8) & 255] ^ te3[s1 & 255] ^ rk[k + 2]
            t3 = te0[s3 >> 24] ^ te1[(s0 >> 16) & 255] ^ te2[(s1 >> 8) & 255] ^ te3[s2 & 255] ^ rk[k + 3]
            s0, s1, s2, s3 = t0, t1, t2, t3
            k += 4
        sb = _SBOX
        return (
            ((sb[s0 >> 24] << 24) | (sb[(s1 >> 16) & 255] << 16) | (sb[(s2 >> 8) & 255] << 8) | sb[s3 & 255]) ^
            rk[k],
            ((sb[s1 >> 24] << 24) | (sb[(s2 >> 16) & 255] << 16) | (sb[(s3 >> 8) & 255] << 8) | sb[s0 & 255]) ^
            rk[k + 1],
            ((sb[s2 >> 24] << 24) | (sb[(s3 >> 16) & 255] << 16) | (sb[(s0 >> 8) & 255] << 8) | sb[s1 & 255]) ^
            rk[k + 2],
            ((sb[s3 >> 24] << 24) | (sb[(s0 >> 16) & 255] << 16) | (sb[(s1 >> 8) & 255] << 8) | sb[s2 & 255]) ^
            rk[k + 3],
        )

    def _decrypt_words(self, s0: int, s1: int, s2: int, s3: int) -> tuple[int, int, int, int]:
        rk = self._dec_keys
        td0, td1, td2, td3 = _TD
        s0 ^= rk[0]
        s1 ^= rk[1]
        s2 ^= rk[2]
        s3 ^= rk[3]
        k = 4
        for _ in range(_ROUNDS - 1):
            t0 = td0[s0 >> 24] ^ td1[(s3 >> 16) & 255] ^ td2[(s2 >> 8) & 255] ^ td3[s1 & 255] ^ rk[k]
            t1 = td0[s1 >> 24] ^ td1[(s0 >> 16) & 255] ^ td2[(s3 >> 8) & 255] ^ td3[s2 & 255] ^ rk[k + 1]
            t2 = td0[s2 >> 24] ^ td1[(s1 >> 16) & 255] ^ td2[(s0 >> 8) & 255] ^ td3[s3 & 255] ^ rk[k + 2]
            t3 = td0[s3 >> 24] ^ td1[(s2 >> 16) & 255] ^ td2[(s1 >> 8) & 255] ^ td3[s0 & 255] ^ rk[k + 3]
            s0, s1, s2, s3 = t0, t1, t2, t3
            k += 4
        ib = _INV_SBOX
        return (
            ((ib[s0 >> 24] << 24) | (ib[(s3 >> 16) & 255] << 16) | (ib[(s2 >> 8) & 255] << 8) | ib[s1 & 255]) ^
            rk[k],
            ((ib[s1 >> 24] << 24) | (ib[(s0 >> 16) & 255] << 16) | (ib[(s3 >> 8) & 255] << 8) | ib[s2 & 255]) ^
            rk[k + 1],
            ((ib[s2 >> 24] << 24) | (ib[(s1 >> 16) & 255] << 16) | (ib[(s0 >> 8) & 255] << 8) | ib[s3 & 255]) ^
            rk[k + 2],
            ((ib[s3 >> 24] << 24) | (ib[(s2 >> 16) & 255] << 16) | (ib[(s1 >> 8) & 255] << 8) | ib[s0 & 255]) ^
            rk[k + 3],
        )

    def encrypt_block(self, block: bytes) -> bytes:
        if len(block) != 16:
            raise ValueError("AES block must be 16 bytes")
        return struct.pack(">4I", *self._encrypt_words(*struct.unpack(">4I", block)))

    def decrypt_block(self, block: bytes) -> bytes:
        if len(block) != 16:
            raise ValueError("AES block must be 16 bytes")
        return struct.pack(">4I", *self._decrypt_words(*struct.unpack(">4I", block)))


def _check_length(data: bytes) -> None:
    if len(data) % 16 != 0:
        raise ValueError("data length must be a multiple of 16")


def ecb_encrypt(key: bytes, data: bytes) -> bytes:
    _check_length(data)
    encrypt = Aes128(key)._encrypt_words
    out = bytearray()
    for s0, s1, s2, s3 in struct.iter_unpack(">4I", data):
        out += struct.pack(">4I", *encrypt(s0, s1, s2, s3))
    return bytes(out)


def ecb_decrypt(key: bytes, data: bytes) -> bytes:
    _check_length(data)
    decrypt = Aes128(key)._decrypt_words
    out = bytearray()
    for s0, s1, s2, s3 in struct.iter_unpack(">4I", data):
        out += struct.pack(">4I", *decrypt(s0, s1, s2, s3))
    return bytes(out)


def _check_iv(iv: bytes) -> tuple[int, int, int, int]:
    if len(iv) != 16:
        raise ValueError("IV must be 16 bytes")
    return struct.unpack(">4I", iv)


def cbc_encrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    _check_length(data)
    p0, p1, p2, p3 = _check_iv(iv)
    encrypt = Aes128(key)._encrypt_words
    out = bytearray()
    for s0, s1, s2, s3 in struct.iter_unpack(">4I", data):
        p0, p1, p2, p3 = encrypt(s0 ^ p0, s1 ^ p1, s2 ^ p2, s3 ^ p3)
        out += struct.pack(">4I", p0, p1, p2, p3)
    return bytes(out)


def cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    _check_length(data)
    p0, p1, p2, p3 = _check_iv(iv)
    decrypt = Aes128(key)._decrypt_words
    out = bytearray()
    for s0, s1, s2, s3 in struct.iter_unpack(">4I", data):
        d0, d1, d2, d3 = decrypt(s0, s1, s2, s3)
        out += struct.pack(">4I", d0 ^ p0, d1 ^ p1, d2 ^ p2, d3 ^ p3)
        p0, p1, p2, p3 = s0, s1, s2, s3
    return bytes(out)

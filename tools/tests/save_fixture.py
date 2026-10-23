"""Builders for synthetic save data. Every key here is made up; no real key bytes belong in this file."""

from __future__ import annotations

FAKE_SYSCON_KEY = bytes(range(0xA0, 0xB0))
# Differs from pfd.TOX2_SECURE_FILE_ID, so a run that relies on the built-in id fails the hash check.
FAKE_SECURE_ID = bytes(range(0x30, 0x40))


def _c_array(name: str, key: bytes, decl: str = "16") -> str:
    body = ", ".join("0x%02X" % b for b in key)
    return "constexpr u8 %s[%s] = {\n    %s\n};\n" % (name, decl, body)


def fake_key_vault_h(key: bytes, *, crlf: bool = False, omit: bool = False, short: bool = False) -> str:
    decoy = bytes(range(0x11, 0x21))
    lines = [
        "#pragma once",
        "",
        "// SC_ISO_SERIES_KEY_1[16] = { 0x00 }",
        "/* SC_ISO_SERIES_KEY_1[16] = { 0x00 } */",
        "constexpr int PASSPHRASE_KEY_LEN = 16;",
        _c_array("SC_ISO_SERIES_KEY_10", decoy),
        _c_array("SC_ISO_SERIES_KEY_2", decoy[::-1]),
        # The block-comment opener inside a string must not start a comment that swallows the real array.
        'const char* kNote = "SC_ISO_SERIES_KEY_1 /* not a comment";',
        # A subscript use before the real initializer is not the array.
        "u8 first = SC_ISO_SERIES_KEY_1[0];",
    ]
    if not omit:
        real = key[:15] if short else key
        body = ", ".join("0x%02X" % b for b in real[:8]) + ",\n    // 0xEE\n    " + \
            ", ".join("0x%02X" % b for b in real[8:])
        lines.append("constexpr u8 SC_ISO_SERIES_KEY_1[PASSPHRASE_KEY_LEN] = {\n    %s\n};" % body)
    lines.append("")
    text = "\n".join(lines)
    return text.replace("\n", "\r\n") if crlf else text

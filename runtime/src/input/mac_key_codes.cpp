#include "input/mac_key_codes.h"

#include <array>
#include <cstddef>

namespace tox2 {
namespace {

// Indexed by kVK_ code; 0 marks a key without a set 1 code (no real set 1 make code is 0).
constexpr std::size_t kKeyCodeCount = 0x80;

constexpr std::array<std::uint16_t, kKeyCodeCount> makeScanCodeTable() {
    std::array<std::uint16_t, kKeyCodeCount> table{};
    // Letters, digits and punctuation of the ANSI layout. Positions, not characters: the Z key stays the
    // Z key on AZERTY and QWERTZ layouts.
    table[0x00] = 0x1E;  // A
    table[0x01] = 0x1F;  // S
    table[0x02] = 0x20;  // D
    table[0x03] = 0x21;  // F
    table[0x04] = 0x23;  // H
    table[0x05] = 0x22;  // G
    table[0x06] = 0x2C;  // Z
    table[0x07] = 0x2D;  // X
    table[0x08] = 0x2E;  // C
    table[0x09] = 0x2F;  // V
    table[0x0A] = 0x56;  // ISO section (the key left of Z on ISO boards)
    table[0x0B] = 0x30;  // B
    table[0x0C] = 0x10;  // Q
    table[0x0D] = 0x11;  // W
    table[0x0E] = 0x12;  // E
    table[0x0F] = 0x13;  // R
    table[0x10] = 0x15;  // Y
    table[0x11] = 0x14;  // T
    table[0x12] = 0x02;  // 1
    table[0x13] = 0x03;  // 2
    table[0x14] = 0x04;  // 3
    table[0x15] = 0x05;  // 4
    table[0x16] = 0x07;  // 6
    table[0x17] = 0x06;  // 5
    table[0x18] = 0x0D;  // =
    table[0x19] = 0x0A;  // 9
    table[0x1A] = 0x08;  // 7
    table[0x1B] = 0x0C;  // -
    table[0x1C] = 0x09;  // 8
    table[0x1D] = 0x0B;  // 0
    table[0x1E] = 0x1B;  // ]
    table[0x1F] = 0x18;  // O
    table[0x20] = 0x16;  // U
    table[0x21] = 0x1A;  // [
    table[0x22] = 0x17;  // I
    table[0x23] = 0x19;  // P
    table[0x24] = 0x1C;  // Return
    table[0x25] = 0x26;  // L
    table[0x26] = 0x24;  // J
    table[0x27] = 0x28;  // '
    table[0x28] = 0x25;  // K
    table[0x29] = 0x27;  // ;
    table[0x2A] = 0x2B;  // backslash
    table[0x2B] = 0x33;  // ,
    table[0x2C] = 0x35;  // /
    table[0x2D] = 0x31;  // N
    table[0x2E] = 0x32;  // M
    table[0x2F] = 0x34;  // .
    table[0x30] = 0x0F;  // Tab
    table[0x31] = 0x39;  // Space
    table[0x32] = 0x29;  // `
    table[0x33] = 0x0E;  // Delete (Backspace)
    table[0x35] = 0x01;  // Escape

    // Modifiers. Command takes the place of the Windows key, as on a PC keyboard attached to a Mac.
    table[0x36] = 0xE05C;  // right Command
    table[0x37] = 0xE05B;  // left Command
    table[0x38] = 0x2A;    // left Shift
    table[0x39] = 0x3A;    // Caps Lock
    table[0x3A] = 0x38;    // left Option
    table[0x3B] = 0x1D;    // left Control
    table[0x3C] = 0x36;    // right Shift
    table[0x3D] = 0xE038;  // right Option
    table[0x3E] = 0xE01D;  // right Control
    // 0x3F is Fn, which has no set 1 code.

    table[0x40] = 0x68;    // F17
    table[0x41] = 0x53;    // keypad .
    table[0x43] = 0x37;    // keypad *
    table[0x45] = 0x4E;    // keypad +
    table[0x47] = 0x45;    // keypad Clear (the Num Lock position)
    table[0x4B] = 0xE035;  // keypad /
    table[0x4C] = 0xE01C;  // keypad Enter
    table[0x4E] = 0x4A;    // keypad -
    table[0x4F] = 0x69;    // F18
    table[0x50] = 0x6A;    // F19
    table[0x51] = 0x59;    // keypad =
    table[0x52] = 0x52;    // keypad 0
    table[0x53] = 0x4F;    // keypad 1
    table[0x54] = 0x50;    // keypad 2
    table[0x55] = 0x51;    // keypad 3
    table[0x56] = 0x4B;    // keypad 4
    table[0x57] = 0x4C;    // keypad 5
    table[0x58] = 0x4D;    // keypad 6
    table[0x59] = 0x47;    // keypad 7
    table[0x5A] = 0x6B;    // F20
    table[0x5B] = 0x48;    // keypad 8
    table[0x5C] = 0x49;    // keypad 9
    table[0x5D] = 0x7D;    // JIS Yen
    table[0x5E] = 0x73;    // JIS underscore (Ro)
    table[0x5F] = 0x7E;    // JIS keypad ,

    table[0x60] = 0x3F;    // F5
    table[0x61] = 0x40;    // F6
    table[0x62] = 0x41;    // F7
    table[0x63] = 0x3D;    // F3
    table[0x64] = 0x42;    // F8
    table[0x65] = 0x43;    // F9
    table[0x67] = 0x57;    // F11
    table[0x69] = 0x64;    // F13
    table[0x6A] = 0x67;    // F16
    table[0x6B] = 0x65;    // F14
    table[0x6D] = 0x44;    // F10
    table[0x6E] = 0xE05D;  // contextual menu
    table[0x6F] = 0x58;    // F12
    table[0x71] = 0x66;    // F15
    table[0x72] = 0xE052;  // Help (the Insert position)
    table[0x73] = 0xE047;  // Home
    table[0x74] = 0xE049;  // Page Up
    table[0x75] = 0xE053;  // Forward Delete
    table[0x76] = 0x3E;    // F4
    table[0x77] = 0xE04F;  // End
    table[0x78] = 0x3C;    // F2
    table[0x79] = 0xE051;  // Page Down
    table[0x7A] = 0x3B;    // F1
    table[0x7B] = 0xE04B;  // Left
    table[0x7C] = 0xE04D;  // Right
    table[0x7D] = 0xE050;  // Down
    table[0x7E] = 0xE048;  // Up
    return table;
}

constexpr std::array<std::uint16_t, kKeyCodeCount> kScanCodes = makeScanCodeTable();

}  // namespace

std::optional<std::uint32_t> scanCodeFromMacKeyCode(std::uint16_t key_code) {
    if (key_code >= kKeyCodeCount || kScanCodes[key_code] == 0) return std::nullopt;
    return kScanCodes[key_code];
}

std::optional<std::uint64_t> macModifierDownMask(std::uint16_t key_code) {
    switch (key_code) {
        case 0x3B: return 0x00000001;  // left Control
        case 0x3E: return 0x00002000;  // right Control
        case 0x38: return 0x00000002;  // left Shift
        case 0x3C: return 0x00000004;  // right Shift
        case 0x3A: return 0x00000020;  // left Option
        case 0x3D: return 0x00000040;  // right Option
        case 0x37: return 0x00000008;  // left Command
        case 0x36: return 0x00000010;  // right Command
        case 0x39: return 0x00010000;  // Caps Lock
        default: return std::nullopt;
    }
}

}  // namespace tox2

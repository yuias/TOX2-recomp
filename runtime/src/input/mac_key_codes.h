// macOS keyboard codes as the PC/AT set 1 scan codes the keyboard pad is keyed by (see keyboard_pad.h).
// Plain integers, so the table is portable data that every platform builds and tests; only the Cocoa
// window feeds it real key codes.

#pragma once

#include <cstdint>
#include <optional>

namespace tox2 {

// PC/AT set 1 make code (0xE000 | code for extended keys) of a macOS virtual key code (kVK_* from
// HIToolbox Events.h). Both are physical key positions, so the result does not depend on the keyboard
// layout. nullopt for keys without a set 1 code (Fn, media keys, the JIS Eisu and Kana keys).
std::optional<std::uint32_t> scanCodeFromMacKeyCode(std::uint16_t key_code);

// The bit of NSEvent.modifierFlags that tells whether a modifier key, given by its kVK_ code, is down
// after a flagsChanged event. Left and right keys have separate device-dependent bits (NX_DEVICE*KEYMASK):
// 0x01 left Control, 0x2000 right Control, 0x02 left Shift, 0x04 right Shift, 0x20 left Option, 0x40 right
// Option, 0x08 left Command, 0x10 right Command. Caps Lock has no per-key bit; it uses the generic
// NSEventModifierFlagCapsLock, 0x10000, which is a toggle state rather than the key's position. nullopt for
// keys that are not modifiers.
std::optional<std::uint64_t> macModifierDownMask(std::uint16_t key_code);

}  // namespace tox2

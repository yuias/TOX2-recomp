#pragma once

// Host copy of the RSX method register file plus the vertex program ucode and constant stores.
// Behaviour follows RPCS3 `Emu/RSX/NV47/HW/nv4097.cpp` (`set_transform_program`,
// `set_transform_constant`), `Emu/RSX/rsx_methods.h` (`rsx_state`) and `Emu/RSX/rsx_methods.cpp`
// (`rsx_state::init`). Not thread safe; the RSX consumer owns it under the guest lock.

#include <array>
#include <cstdint>

namespace tox2 {

inline constexpr std::uint32_t kRsxVpInstructionCount = 544;  // RPCS3 max_vertex_program_instructions
inline constexpr std::uint32_t kRsxVpConstantCount = 468;     // vec4 slots, RPCS3 `set_transform_constant::impl`

inline constexpr std::uint32_t kRsxTransformWindow = 32;  // registers per TRANSFORM_PROGRAM / TRANSFORM_CONSTANT block

struct RsxRegisters {
    std::array<std::uint32_t, 0x4000> regs{};                           // by method >> 2
    std::array<std::uint32_t, kRsxVpInstructionCount * 4> vp_ucode{};   // BE-decoded words
    std::array<std::uint32_t, kRsxVpConstantCount * 4> vp_constants{};  // raw float bits
    // Mirrors of regs[] for the three registers the loader reads; kept in sync by apply().
    std::uint32_t vp_load = 0, vp_start = 0, constant_load = 0;
    // Bit n: slot n of the SET_VERTEX_DATA*_M default attribute registers has been written since init.
    // RPCS3 keeps these in `register_vertex_info`, cleared by `rsx_state::init` only, so contextReset()
    // leaves the mask alone. A slot that was never written reads as (0, 0, 0, 1).
    std::uint32_t vertex_default_written = 0;

    // Applies one completed write. TRANSFORM_PROGRAM words land at vp_load * 4 + (slot % 4) and vp_load
    // advances after each fourth word. TRANSFORM_CONSTANT words land at
    // (constant_load + slot / 4) * 4 + slot % 4; constant_load itself never advances (RPCS3 only ever
    // changes it through a write). Writes past the end of either store are dropped. The 1F, 2F, 3F and 4F
    // immediate vertex data forms all land in the 4F register block (the first components written, the
    // others (0, 0, 0, 1) as RPCS3's register_vertex_info defaults) and set the written bit of the slot;
    // the integer forms are not decoded and leave the slot alone.
    void apply(std::uint32_t method, std::uint32_t value);
    std::uint32_t reg(std::uint32_t method) const { return regs[(method >> 2) & 0x3FFF]; }
    void reset();  // RPCS3 `rsx_state::init` defaults
    // RPCS3 `rsx_state::reset`, run at every flip: overwrites the registers it assigns (zeros included)
    // and leaves all others, the vertex program store and constants, and the load pointers alone.
    void contextReset();
};

}  // namespace tox2

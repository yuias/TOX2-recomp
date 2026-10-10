#pragma once

#include <cstdint>

namespace dal {

// SPU thread groups exist only as ids: the game's SPURS instance asks for one, but no SPU thread
// is ever created. Ids follow RPCS3 `Emu/Cell/lv2/sys_spu.h` `lv2_spu_group` (base 0x04000100,
// step 0x100, 255 slots). Call under the guest lock.

// Allocates a group id, or returns 0 when every slot is taken.
std::uint32_t createSpuThreadGroup();

// Releases `id`; unknown ids are ignored.
void destroySpuThreadGroup(std::uint32_t id);

// True while `id` was created and not destroyed.
bool spuThreadGroupExists(std::uint32_t id);

// Forgets every group. Intended for tests.
void resetSpuThreadGroups();

}  // namespace dal

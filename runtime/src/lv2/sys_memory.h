#pragma once

#include <cstdint>

// Memory containers (sys_misc.cpp). Everything here runs under the guest lock.

namespace tox2 {

// True while the memory container `id` exists.
bool memoryContainerExists(std::uint32_t id);

// Forgets every memory container and the blocks allocated from them, without returning their
// memory to the default container. Intended for tests.
void resetMemoryContainers();

}  // namespace tox2

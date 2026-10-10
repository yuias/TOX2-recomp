#pragma once

// Memory containers (sys_misc.cpp). Everything here runs under the guest lock.

namespace tox2 {

// Forgets every memory container and the blocks allocated from them, without returning their
// memory to the default container. Intended for tests.
void resetMemoryContainers();

}  // namespace tox2

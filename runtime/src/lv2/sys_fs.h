#pragma once

#include <cstdint>

namespace tox2 {

// Host ReadFile/WriteFile work done by the sys_fs transfers since the previous takeFsTransferStats() call. A
// transfer is one guest read or write (a positioned one counts once); the duration is that of the whole host loop.
struct FsTransferStats {
    std::uint64_t reads = 0, read_bytes = 0, read_max_us = 0;
    std::uint64_t writes = 0, write_bytes = 0, write_max_us = 0;
};

// Returns the counters and resets them. For the one periodic reporter; any thread may record concurrently.
FsTransferStats takeFsTransferStats();

}  // namespace tox2

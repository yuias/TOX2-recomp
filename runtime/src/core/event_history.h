#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tox2 {

// Always-on ring of the last kHistoryCapacity lv2 events that matter for lifetime races (an SPU event reaching a
// PPU thread, a block being freed, join, mutex destroy). A fatal guest fault prints it so the order of these
// events can be read from the log of a rare crash.
enum class HistoryKind : std::uint8_t { SpuSend, EventReceive, MemoryFree, JoinEnter, JoinReturn, MutexDestroy };

inline constexpr std::size_t kHistoryCapacity = 512;

// Lock-free and allocation-free; safe from any host thread. The meaning of a, b and c depends on `kind`:
// SpuSend / EventReceive: queue id, event code, data3. MemoryFree: address, size, caller lr (lr1 and lr2 are the next
// two return addresses). JoinEnter: target thread. JoinReturn: target thread, result. MutexDestroy: id, result.
void recordHistory(HistoryKind kind, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint32_t lr1 = 0,
                   std::uint32_t lr2 = 0);

// Formats the ring oldest to newest, one line per entry; an entry that is being overwritten is skipped. With
// `fault_addr` a final line names the newest recorded free whose block contains it.
std::vector<std::string> historyLines(std::optional<std::uint32_t> fault_addr);

// Empties the ring. Intended for tests; not safe against concurrent recordHistory calls.
void resetHistory();

}  // namespace tox2

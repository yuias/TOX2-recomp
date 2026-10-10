// Dynamic transfers: guest address -> generated function through a flat table with one slot per
// 4-byte instruction address.

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "tox2/ppu_context.h"

namespace {

// Written once by tox2RegisterFuncs before any guest thread starts, then read-only.
std::vector<Tox2PpuFunc*> g_table;
std::uint32_t g_span_begin = 0;
std::uint32_t g_span_end = 0;  // one past the highest function's first instruction

// A table of more slots than this means a corrupt mapping list, not a real program.
constexpr std::uint64_t kMaxSlots = 0x4000000;

// Unsigned wrap-around makes this correct even when the span ends at 2^32 (g_span_end == 0).
bool insideSpan(std::uint32_t addr) { return addr - g_span_begin < g_span_end - g_span_begin; }

struct HostFunction {
    const char* name = nullptr;
    Tox2PpuFunc* function = nullptr;
};

// Fixed capacity so lookups never race a reallocation. Registration happens under the guest lock
// (or before boot), the same lock every tox2Lookup caller holds.
std::array<HostFunction, tox2::kMaxHostFunctions> g_host_functions;
std::uint32_t g_host_function_count = 0;
std::uint32_t g_opd_block = 0;
const std::uint8_t* g_opd_block_memory = nullptr;  // reservation the block was taken from

// The newest entries are the callers that led to the bad transfer.
std::string recentTrace(const Tox2PpuContext* ctx) {
    constexpr std::uint32_t kShown = 16;
    const std::uint32_t count = ctx->trace_pos < kShown ? ctx->trace_pos : kShown;
    std::string text;
    for (std::uint32_t i = 0; i < count; i++) {
        text += std::format(" {:08x}", ctx->trace[(ctx->trace_pos - count + i) & (kTox2TraceSize - 1)]);
    }
    return text;
}

}  // namespace

namespace tox2 {

std::uint32_t findOwningFunction(std::uint32_t addr) {
    if (!insideSpan(addr)) {
        return 0;
    }
    // The table is dense (one slot per instruction), so walk back to the nearest populated slot.
    for (std::uint32_t slot = (addr - g_span_begin) / 4 + 1; slot-- > 0;) {
        if (g_table[slot] != nullptr) {
            return g_span_begin + slot * 4;
        }
    }
    return 0;
}

std::uint32_t registerHostFunction(const char* name, GuestHandler handler) {
    if (handler == nullptr) {
        fatal(FatalKind::Internal, std::format("host function {}: null handler", name));
    }
    if (g_host_function_count >= kMaxHostFunctions) {
        fatal(FatalKind::Internal, std::format("host function {}: table is full", name));
    }
    GuestMemory& memory = process().memory;
    if (g_opd_block == 0) {
        g_opd_block = process().main.allocate(kMaxHostFunctions * 8, 0x10000);
        if (g_opd_block == 0) {
            fatal(FatalKind::Internal, std::format("host function {}: no room for the OPD block", name));
        }
        g_opd_block_memory = memory.base();
    }

    const std::uint32_t index = g_host_function_count++;
    g_host_functions[index] = {name, reinterpret_cast<Tox2PpuFunc*>(handler)};
    const std::uint32_t opd = g_opd_block + index * 8;
    tox2Write32(memory.base(), opd, kHostFunctionBase + index * 4);
    tox2Write32(memory.base(), opd + 4, 0);
    return opd;
}

void resetHostFunctions() {
    // The block is only ours to free while the reservation it came from is still the current one.
    if (g_opd_block != 0 && g_opd_block_memory == process().memory.base()) {
        process().main.free(g_opd_block);
    }
    g_host_functions.fill({});
    g_host_function_count = 0;
    g_opd_block = 0;
    g_opd_block_memory = nullptr;
}

}  // namespace tox2

extern "C" {

void tox2RegisterFuncs(const Tox2FuncMapping* mappings, uint32_t count) {
    // Replaces any previous registration (unit tests register several programs).
    g_table.clear();
    g_span_begin = 0;
    g_span_end = 0;
    if (count == 0) {
        tox2::logInfo("tox2: dispatch: 0 entries");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        const Tox2FuncMapping& m = mappings[i];
        if ((m.guest & 3) != 0) {
            tox2::fatal(tox2::FatalKind::Internal,
                        std::format("function table: unaligned guest address {:#010x}", m.guest));
        }
        if (m.host == nullptr) {
            tox2::fatal(tox2::FatalKind::Internal,
                        std::format("function table: no host function for {:#010x}", m.guest));
        }
        if (i > 0 && m.guest <= mappings[i - 1].guest) {
            tox2::fatal(tox2::FatalKind::Internal,
                        std::format("function table: {:#010x} follows {:#010x} (unsorted or duplicate)", m.guest,
                                    mappings[i - 1].guest));
        }
    }

    const std::uint64_t begin = mappings[0].guest;
    const std::uint64_t end = std::uint64_t{mappings[count - 1].guest} + 4;
    if (end > 0x100000000ull || (end - begin) / 4 > kMaxSlots) {
        tox2::fatal(tox2::FatalKind::Internal,
                    std::format("function table: span {:#x}..{:#x} is too large", begin, end));
    }

    g_table.assign((end - begin) / 4, nullptr);
    for (uint32_t i = 0; i < count; i++) {
        g_table[(mappings[i].guest - begin) / 4] = mappings[i].host;
    }
    g_span_begin = static_cast<std::uint32_t>(begin);
    g_span_end = static_cast<std::uint32_t>(end);  // end == 2^32 wraps to 0; see insideSpan
    tox2::logInfo("tox2: dispatch: {} entries, span {:#010x}..{:#010x}", count, begin, end);
}

Tox2PpuFunc* tox2Lookup(uint32_t addr) {
    if ((addr & 3) != 0) {
        return nullptr;
    }
    if (addr - tox2::kHostFunctionBase < 4 * g_host_function_count) {
        return g_host_functions[(addr - tox2::kHostFunctionBase) / 4].function;
    }
    if (!insideSpan(addr)) {
        return nullptr;
    }
    return g_table[(addr - g_span_begin) / 4];
}

void tox2UnknownTarget(Tox2PpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site) {
    (void)base;
    const bool first = tox2::censusUnknownTarget(target, site);
    if (tox2::process().strict) {
        tox2::fatal(tox2::FatalKind::UnknownTarget, std::format("target {:#010x} from {:#010x}", target, site));
    }
    if (first) {
        tox2::logWarn("tox2: unknown target {:#010x} from {:#010x}: {}", target, site,
                      insideSpan(target) ? "inside the code span (missed function?)"
                                         : "outside the code span (corrupt pointer?)");
        tox2::logWarn("tox2: trace (oldest -> newest):{}", recentTrace(ctx));
    }
    // Returning here acts like a callee that returned at once: the caller continues after the call
    // site with the registers as they are.
}

// The musttail keeps dynamic transfers from adding a host frame per hop.
void tox2Dispatch(Tox2PpuContext* __restrict ctx, uint8_t* __restrict base) {
    Tox2PpuFunc* f = tox2Lookup(ctx->jump_target);
    if (f != nullptr) {
        [[clang::musttail]] return f(ctx, base);
    }
    tox2UnknownTarget(ctx, base, ctx->jump_target, ctx->jump_site);
}

}  // extern "C"

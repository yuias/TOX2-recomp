// Dynamic transfers: guest address -> generated function through flat tables with one slot per
// 4-byte instruction address, one table (span) per image.

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "core/census.h"
#include "core/dispatch.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"

namespace {

// One image's functions. A single table over several images would also cover the data between them,
// so unknown-target reports and owner lookups would attribute data addresses to a function.
struct Span {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;  // one past the highest function's first instruction; may be 2^32
    std::vector<Tox2PpuFunc*> table;
};

// Written by tox2RegisterFuncs and appendFuncs before any guest thread starts, then read-only. A
// handful of entries at most, so lookups scan it.
std::vector<Span> g_spans;

// A table of more slots than this means a corrupt mapping list, not a real program.
constexpr std::uint64_t kMaxSlots = 0x4000000;

const Span* findSpan(std::uint32_t addr) {
    for (const Span& span : g_spans) {
        if (addr >= span.begin && addr < span.end) {
            return &span;
        }
    }
    return nullptr;
}

Span buildSpan(const Tox2FuncMapping* mappings, std::uint32_t count) {
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

    Span span;
    span.begin = mappings[0].guest;
    span.end = std::uint64_t{mappings[count - 1].guest} + 4;
    if (span.end > 0x100000000ull || (span.end - span.begin) / 4 > kMaxSlots) {
        tox2::fatal(tox2::FatalKind::Internal,
                    std::format("function table: span {:#x}..{:#x} is too large", span.begin, span.end));
    }

    span.table.assign((span.end - span.begin) / 4, nullptr);
    for (uint32_t i = 0; i < count; i++) {
        span.table[(mappings[i].guest - span.begin) / 4] = mappings[i].host;
    }
    return span;
}

void addSpan(Span span, std::uint32_t count) {
    for (const Span& other : g_spans) {
        if (span.begin < other.end && other.begin < span.end) {
            tox2::fatal(tox2::FatalKind::Internal,
                        std::format("function table: span {:#010x}..{:#010x} overlaps span {:#010x}..{:#010x}",
                                    span.begin, span.end, other.begin, other.end));
        }
    }
    tox2::logInfo("tox2: dispatch: {} entries, span {:#010x}..{:#010x}", count, span.begin, span.end);
    g_spans.push_back(std::move(span));
}

struct HostFunction {
    const char* name = nullptr;
    Tox2PpuFunc* function = nullptr;
};

// Fixed capacity so lookups never race a reallocation. Registration runs in a kernel section (or before boot), so
// there is one writer at a time. Lookups take no lock (two guest threads run in parallel in the split modes): the
// count is published with release after the entry and the OPD are written, and tox2Lookup reads it with acquire.
std::array<HostFunction, tox2::kMaxHostFunctions> g_host_functions;
std::atomic<std::uint32_t> g_host_function_count{0};
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
    const Span* span = findSpan(addr);
    if (span == nullptr) {
        return 0;
    }
    // The table is dense (one slot per instruction), so walk back to the nearest populated slot. The
    // walk stays inside this span: a function of another image is not the owner.
    for (std::uint64_t slot = (addr - span->begin) / 4 + 1; slot-- > 0;) {
        if (span->table[slot] != nullptr) {
            return static_cast<std::uint32_t>(span->begin + slot * 4);
        }
    }
    return 0;
}

void appendFuncs(const Tox2FuncMapping* mappings, std::uint32_t count) {
    if (count == 0) {
        tox2::logInfo("tox2: dispatch: 0 entries");
        return;
    }
    addSpan(buildSpan(mappings, count), count);
}

std::uint32_t registerHostFunction(const char* name, GuestHandler handler) {
    if (handler == nullptr) {
        fatal(FatalKind::Internal, std::format("host function {}: null handler", name));
    }
    // Covers the whole function: the lazy OPD block allocation below takes from process().main, which is lv2 state.
    const KernelSection kernel;
    const std::uint32_t index = g_host_function_count.load(std::memory_order_relaxed);
    if (index >= kMaxHostFunctions) {
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

    g_host_functions[index] = {name, reinterpret_cast<Tox2PpuFunc*>(handler)};
    const std::uint32_t opd = g_opd_block + index * 8;
    tox2Write32(memory.base(), opd, kHostFunctionBase + index * 4);
    tox2Write32(memory.base(), opd + 4, 0);
    g_host_function_count.store(index + 1, std::memory_order_release);
    return opd;
}

void resetHostFunctions() {
    // The block is only ours to free while the reservation it came from is still the current one.
    if (g_opd_block != 0 && g_opd_block_memory == process().memory.base()) {
        process().main.free(g_opd_block);
    }
    g_host_functions.fill({});
    g_host_function_count.store(0, std::memory_order_release);
    g_opd_block = 0;
    g_opd_block_memory = nullptr;
}

}  // namespace tox2

extern "C" {

void tox2RegisterFuncs(const Tox2FuncMapping* mappings, uint32_t count) {
    // Replaces every previous registration (unit tests register several programs).
    g_spans.clear();
    tox2::appendFuncs(mappings, count);
}

Tox2PpuFunc* tox2Lookup(uint32_t addr) {
    if ((addr & 3) != 0) {
        return nullptr;
    }
    if (addr - tox2::kHostFunctionBase < 4 * g_host_function_count.load(std::memory_order_acquire)) {
        return g_host_functions[(addr - tox2::kHostFunctionBase) / 4].function;
    }
    const Span* span = findSpan(addr);
    return span != nullptr ? span->table[(addr - span->begin) / 4] : nullptr;
}

void tox2UnknownTarget(Tox2PpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site) {
    (void)base;
    const bool first = tox2::censusUnknownTarget(target, site);
    if (tox2::process().strict) {
        tox2::fatal(tox2::FatalKind::UnknownTarget, std::format("target {:#010x} from {:#010x}", target, site));
    }
    if (first) {
        tox2::logWarn("tox2: unknown target {:#010x} from {:#010x}: {}", target, site,
                      findSpan(target) != nullptr ? "inside a code span (missed function?)"
                                                  : "outside the code spans (corrupt pointer?)");
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

// HLE dispatch: the generated import stubs call tox2HleCall(index), which runs the registered
// handler for that import or the default one.

#include "hle/hle.h"

#include <array>
#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"

namespace tox2 {
namespace {

// The default handler logs this many calls of each import, then only counts.
constexpr std::uint32_t kDefaultLogLimit = 8;

struct InstalledImport {
    const char* module = nullptr;
    const char* name = nullptr;
    GuestHandler handler = nullptr;
    std::uint32_t default_calls = 0;
};

// Every module's handler list. No static self-registration: archive members are not pulled in by
// static initialisers, so each module is named here.
using HandlerList = std::span<const HleEntry> (*)();
constexpr std::array<HandlerList, 17> kRegistries{
        sysPrxForUserHandlers,  cellSysmoduleHandlers,   cellGcmSysHandlers,  cellGameHandlers,
        cellFsHandlers,         sceNpHandlers,           sceNpTrophyHandlers, cellSysutilHandlers,
        cellMsgDialogHandlers,  cellSaveDataHandlers,    cellPadHandlers,     cellPngDecHandlers,
        cellSailPlayerHandlers, cellSailAdapterHandlers, cellPamfHandlers,    cellAtracHandlers,
        cellAudioHandlers};

// Written by installImports before any guest thread starts; the call counters are updated under
// the guest lock.
std::vector<InstalledImport> g_imports;
bool g_quiet = false;

// Logs the arguments of a default-handled import that look like out-pointers (a guest address in
// committed memory that is not code) and records them in the census, so the census doubles as a
// work list for the handlers still missing.
void hintOutPointers(const InstalledImport& import, const Tox2PpuContext* ctx) {
    const char* module = import.module != nullptr ? import.module : "?";
    const char* name = import.name != nullptr ? import.name : "?";
    std::string note;
    for (int reg = 3; reg <= 10; reg++) {
        const std::uint64_t value = ctx->gpr[reg];
        if (value == 0 || (value >> 32) != 0) continue;
        const auto addr = static_cast<std::uint32_t>(value);
        if (!process().memory.isCommitted(addr, 4) || findOwningFunction(addr) != 0) continue;

        const char* range = guestRangeName(addr);
        logInfo("tox2: default hle {}::{}: possible out-pointer r{}={:#010x}{}", module, name, reg, addr,
                range != nullptr ? std::format(" ({})", range) : std::string());
        note += note.empty() ? std::format("ptr r{}", reg) : std::format(",r{}", reg);
    }
    if (!note.empty()) {
        censusHleNote(module, name, std::move(note));
    }
}

}  // namespace

void installImports(const Tox2ImportInfo* imports, std::uint32_t count) {
    std::map<std::pair<std::string, std::uint32_t>, const HleEntry*> registry;
    for (const HandlerList list : kRegistries) {
        for (const HleEntry& entry : list()) {
            const auto [it, inserted] = registry.emplace(std::pair{std::string(entry.module), entry.fnid}, &entry);
            if (!inserted) {
                fatal(FatalKind::Internal,
                      std::format("hle: {}::{} (fnid {:#010x}) is registered twice (also as {})", entry.module,
                                  entry.name != nullptr ? entry.name : "?", entry.fnid,
                                  it->second->name != nullptr ? it->second->name : "?"));
            }
        }
    }

    g_imports.assign(count, InstalledImport{});
    std::uint32_t implemented = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        const Tox2ImportInfo& info = imports[i];
        InstalledImport& slot = g_imports[i];
        slot.module = info.module;
        slot.name = info.name;
        const auto it = registry.find({info.module != nullptr ? info.module : "", info.fnid});
        if (it == registry.end()) continue;

        const HleEntry& entry = *it->second;
        if (info.name != nullptr && entry.name != nullptr && std::string_view(info.name) != entry.name) {
            logWarn("tox2: hle: {}::{} (fnid {:#010x}) is registered as {}", info.module, info.name, info.fnid,
                    entry.name);
        }
        slot.handler = entry.handler;
        if (slot.handler != nullptr) implemented++;
    }
    logInfo("tox2: hle: {} imports, {} with handlers", count, implemented);
}

void setQuietHle(bool quiet) { g_quiet = quiet; }

void callGuestOpd(Tox2PpuContext* ctx, uint8_t* base, std::uint32_t opd_addr) {
    const std::uint64_t saved_lr = ctx->lr;
    const std::uint64_t saved_toc = ctx->gpr[2];
    const std::uint32_t entry = tox2Read32(base, opd_addr);
    ctx->gpr[2] = tox2Read32(base, opd_addr + 4);
    ctx->lr = 0;
    ctx->jump_site = opd_addr;
    ctx->jump_target = entry;
    tox2Dispatch(ctx, base);
    ctx->gpr[2] = saved_toc;
    ctx->lr = saved_lr;
}

std::uint64_t callGuestFunction(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t opd_addr,
                                std::initializer_list<std::uint64_t> args) {
    if (args.size() > 8) {
        fatal(FatalKind::Internal,
              std::format("callGuestFunction({:#010x}): {} arguments, at most 8 are supported", opd_addr, args.size()));
    }
    const std::uint64_t saved_r1 = ctx->gpr[1];
    // RPCS3 PPUCallback.h `_func_caller`: align r1, then reserve 0x70 bytes below it.
    const std::uint64_t frame = ((saved_r1 & ~std::uint64_t{15}) - 0x70);
    tox2Write64(base, static_cast<std::uint32_t>(frame), saved_r1);
    ctx->gpr[1] = frame;
    int reg = 3;
    for (const std::uint64_t arg : args) {
        ctx->gpr[reg++] = arg;
    }

    callGuestOpd(ctx, base, opd_addr);

    const std::uint64_t result = ctx->gpr[3];
    ctx->gpr[1] = saved_r1;
    return result;
}

std::string readGuestString(const std::uint8_t* base, std::uint32_t addr, std::uint32_t max) {
    std::string text;
    constexpr std::uint32_t kPageMask = 0xfff;
    for (std::uint32_t i = 0; i < max; i++) {
        const std::uint32_t at = addr + i;
        if (at < addr) break;  // wrapped past the 32-bit space
        // Commitment is per page, so ask once at the start and at each page boundary; a string that runs
        // into unmapped memory ends there instead of faulting.
        if ((i == 0 || (at & kPageMask) == 0) && !process().memory.isCommitted(at, 1)) break;
        const char c = static_cast<char>(base[at]);
        if (c == '\0') break;
        text += c;
    }
    return text;
}

}  // namespace tox2

extern "C" {

void tox2HleCall(Tox2PpuContext* ctx, uint8_t* base, uint32_t import_index) {
    if (import_index >= tox2::g_imports.size()) {
        tox2::fatal(tox2::FatalKind::Internal, std::format("hle call: import index {} out of range ({} installed)",
                                                           import_index, tox2::g_imports.size()));
    }
    tox2::InstalledImport& import = tox2::g_imports[import_index];
    const bool implemented = import.handler != nullptr;
    tox2::censusHle(import.module, import.name, implemented);

    if (implemented) {
        import.handler(ctx, base);
        return;
    }

    if (!tox2::g_quiet && import.default_calls < tox2::kDefaultLogLimit) {
        tox2::logInfo("tox2: default hle {}::{}({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
                      import.module != nullptr ? import.module : "?", import.name != nullptr ? import.name : "?",
                      ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
    }
    if (import.default_calls == 0) {
        tox2::hintOutPointers(import, ctx);
    }
    import.default_calls++;
    ctx->gpr[3] = tox2::kCellOk;
}

}  // extern "C"

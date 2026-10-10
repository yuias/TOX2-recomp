// HLE dispatch: the generated import stubs call tox2HleCall(index), which runs the registered
// handler for that import or the default one.

#include "hle/hle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
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
    std::uint32_t stub = 0;
    const char* module = nullptr;
    const char* name = nullptr;
    GuestHandler handler = nullptr;
    std::uint32_t default_calls = 0;
    std::uint32_t bound_opd = 0;  // descriptor of the recompiled export this import calls; 0: not bound
    std::string bound_image;
};

// The import table of a module other than the main executable.
struct ModuleImports {
    std::string image;
    const Tox2ImportInfo* first = nullptr;  // the table the module's trampolines point into
    std::uint32_t count = 0;
    std::vector<InstalledImport> imports;
};

using Registry = std::map<std::pair<std::string, std::uint32_t>, const HleEntry*>;

struct Binding {
    std::size_t image = 0;  // index into the GuestExports span
    std::uint32_t opd = 0;
};
using BindingMap = std::map<std::pair<std::string, std::uint32_t>, Binding>;

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
std::vector<ModuleImports> g_modules;
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

Registry buildRegistry() {
    Registry registry;
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
    return registry;
}

// Maps every function export of `guest` by (module, fnid). Each descriptor is checked now so that a
// bad export table stops the start-up instead of the first call through it.
BindingMap buildBindings(std::span<const GuestExports> guest) {
    BindingMap bindings;
    const std::uint8_t* base = process().memory.base();
    for (std::size_t image = 0; image < guest.size(); image++) {
        const GuestExports& set = guest[image];
        for (std::uint32_t i = 0; i < set.count; i++) {
            const Tox2ExportInfo& info = set.exports[i];
            const std::string module = info.module != nullptr ? info.module : "";
            if (!process().memory.isCommitted(info.opd, 8)) {
                fatal(FatalKind::Internal,
                      std::format("hle: {} export {}::{:#010x}: descriptor {:#010x} is not in committed memory",
                                  set.image, module, info.fnid, info.opd));
            }
            const std::uint32_t entry = tox2Read32(base, info.opd);
            if (tox2Lookup(entry) == nullptr) {
                fatal(FatalKind::Internal,
                      std::format("hle: {} export {}::{:#010x}: descriptor {:#010x} names {:#010x}, which is not a "
                                  "registered function",
                                  set.image, module, info.fnid, info.opd, entry));
            }
            const auto [it, inserted] = bindings.emplace(std::pair{module, info.fnid}, Binding{image, info.opd});
            // The same export listed twice is harmless; two different targets would make the binding ambiguous.
            if (!inserted && (it->second.image != image || it->second.opd != info.opd)) {
                fatal(FatalKind::Internal, std::format("hle: {}::{:#010x} is exported twice with different "
                                                       "descriptors ({:#010x}, {:#010x})",
                                                       module, info.fnid, it->second.opd, info.opd));
            }
        }
    }
    return bindings;
}

// Resolves a table of imports: a binding wins over a handler, everything else is default-handled.
// `implemented` counts handlers, `bound[i]` the imports bound to guest[i].
std::vector<InstalledImport> resolveImports(const Tox2ImportInfo* imports, std::uint32_t count,
                                            const Registry& registry, const BindingMap& bindings,
                                            std::span<const GuestExports> guest, std::vector<std::uint32_t>& bound,
                                            std::uint32_t& implemented) {
    std::vector<InstalledImport> installed(count);
    implemented = 0;
    for (std::uint32_t i = 0; i < count; i++) {
        const Tox2ImportInfo& info = imports[i];
        InstalledImport& slot = installed[i];
        slot.stub = info.stub;
        slot.module = info.module;
        slot.name = info.name;
        const std::pair<std::string, std::uint32_t> key{info.module != nullptr ? info.module : "", info.fnid};

        const auto registered = registry.find(key);
        if (registered != registry.end()) {
            const HleEntry& entry = *registered->second;
            if (info.name != nullptr && entry.name != nullptr && std::string_view(info.name) != entry.name) {
                logWarn("tox2: hle: {}::{} (fnid {:#010x}) is registered as {}", info.module, info.name, info.fnid,
                        entry.name);
            }
            if (slot.name == nullptr) slot.name = entry.name;
            slot.handler = entry.handler;
        }

        const auto binding = bindings.find(key);
        if (binding != bindings.end()) {
            slot.handler = nullptr;
            slot.bound_opd = binding->second.opd;
            slot.bound_image = guest[binding->second.image].image;
            bound[binding->second.image]++;
            continue;
        }
        if (slot.handler != nullptr) implemented++;
    }
    return installed;
}

// Runs one installed import: the bound export, the handler, or the default handler.
void runImport(InstalledImport& import, Tox2PpuContext* ctx, std::uint8_t* base) {
    if (import.bound_opd != 0) {
        censusHleBound(import.module, import.name, import.bound_image.c_str());
        // lr stays the caller's, so the callee returns straight to the caller of the stub; the caller
        // restores its own r2 from the slot the stub saved it in.
        ctx->gpr[2] = tox2Read32(base, import.bound_opd + 4);
        ctx->jump_site = import.stub;
        ctx->jump_target = tox2Read32(base, import.bound_opd);
        tox2Dispatch(ctx, base);
        return;
    }

    const bool implemented = import.handler != nullptr;
    censusHle(import.module, import.name, implemented);

    if (implemented) {
        import.handler(ctx, base);
        return;
    }

    if (!g_quiet && import.default_calls < kDefaultLogLimit) {
        logInfo("tox2: default hle {}::{}({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
                import.module != nullptr ? import.module : "?", import.name != nullptr ? import.name : "?", ctx->gpr[3],
                ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
    }
    if (import.default_calls == 0) {
        hintOutPointers(import, ctx);
    }
    import.default_calls++;
    ctx->gpr[3] = kCellOk;
}

}  // namespace

std::vector<std::string> guestExportConflicts(std::span<const GuestExports> guest) {
    const Registry registry = buildRegistry();
    // Ordered by (module, fnid); an export listed by several images is reported once.
    std::map<std::pair<std::string, std::uint32_t>, std::string> found;
    for (const GuestExports& set : guest) {
        for (std::uint32_t i = 0; i < set.count; i++) {
            const Tox2ExportInfo& info = set.exports[i];
            const std::string module = info.module != nullptr ? info.module : "";
            const auto it = registry.find({module, info.fnid});
            if (it == registry.end()) continue;
            found.emplace(std::pair{module, info.fnid},
                          std::format("{}::{} (fnid {:#010x})", module,
                                      it->second->name != nullptr ? it->second->name : "?", info.fnid));
        }
    }
    std::vector<std::string> conflicts;
    for (auto& [key, text] : found) {
        conflicts.push_back(std::move(text));
    }
    return conflicts;
}

void installImports(const Tox2ImportInfo* imports, std::uint32_t count, std::span<const GuestExports> guest) {
    const std::vector<std::string> conflicts = guestExportConflicts(guest);
    if (!conflicts.empty()) {
        std::string list;
        for (const std::string& conflict : conflicts) {
            list += (list.empty() ? "" : ", ") + conflict;
        }
        fatal(FatalKind::Internal, std::format("hle: {} export(s) of recompiled modules are also registered as "
                                               "handlers: {}",
                                               conflicts.size(), list));
    }

    const Registry registry = buildRegistry();
    const BindingMap bindings = buildBindings(guest);
    std::vector<std::uint32_t> bound(guest.size(), 0);
    std::uint32_t implemented = 0;
    g_modules.clear();
    g_imports = resolveImports(imports, count, registry, bindings, guest, bound, implemented);

    std::string line = std::format("tox2: hle: {} imports, {} with handlers", count, implemented);
    for (std::size_t i = 0; i < guest.size(); i++) {
        line += std::format(", {} bound to {}", bound[i], guest[i].image);
    }
    logInfo("{}", line);
}

void installModuleImports(const char* image, const Tox2ImportInfo* imports, std::uint32_t count) {
    const Registry registry = buildRegistry();
    std::vector<std::uint32_t> no_bound;
    std::uint32_t implemented = 0;
    ModuleImports table;
    table.image = image;
    table.first = imports;
    table.count = count;
    table.imports = resolveImports(imports, count, registry, {}, {}, no_bound, implemented);

    const auto existing = std::ranges::find(g_modules, table.image, &ModuleImports::image);
    if (existing != g_modules.end()) {
        *existing = std::move(table);
    } else {
        g_modules.push_back(std::move(table));
    }
    logInfo("tox2: hle: {}: {} imports, {} with handlers", image, count, implemented);
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
    tox2::runImport(tox2::g_imports[import_index], ctx, base);
}

void tox2HleCallImport(Tox2PpuContext* ctx, uint8_t* base, const Tox2ImportInfo* import) {
    const std::less<const Tox2ImportInfo*> before;
    for (tox2::ModuleImports& module : tox2::g_modules) {
        if (!before(import, module.first) && before(import, module.first + module.count)) {
            tox2::runImport(module.imports[static_cast<std::size_t>(import - module.first)], ctx, base);
            return;
        }
    }
    tox2::fatal(tox2::FatalKind::Internal,
                std::format("hle call: import {}::{} (stub {:#010x}) is in no installed module table",
                            import->module != nullptr ? import->module : "?",
                            import->name != nullptr ? import->name : "?", import->stub));
}

}  // extern "C"

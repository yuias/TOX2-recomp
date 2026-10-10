// HLE handlers of the cellSysmodule module. RPCS3 `Modules/cellSysmodule.cpp` rejects module ids it
// does not know and tracks load counts; here every id succeeds, because no module is actually
// loaded (the HLE functions are always available). Handlers run under the guest lock.

#include <cstdint>
#include <format>

#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellSysmodule";

// The id argument is a u16 in the SDK.
void logModule(const char* verb, const Tox2PpuContext* ctx) {
    const auto id = static_cast<std::uint16_t>(argU32(ctx, 0));
    logOnce(std::format("sysmodule-{}-{}", verb, id), "tox2: sysmodule: {} {:#06x}", verb, id);
}

// cellSysmoduleLoadModule(u16 id)
void sysmoduleLoadModule(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    logModule("load", ctx);
    ctx->gpr[3] = kCellOk;
}

// cellSysmoduleUnloadModule(u16 id)
void sysmoduleUnloadModule(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    logModule("unload", ctx);
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x112a5ee9, "cellSysmoduleUnloadModule", sysmoduleUnloadModule},
        {kModule, 0x32267a31, "cellSysmoduleLoadModule", sysmoduleLoadModule},
};

}  // namespace

std::span<const HleEntry> cellSysmoduleHandlers() { return kHandlers; }

}  // namespace tox2

// HLE handlers of the sceNp module: sceNpInit, sceNpTerm and sceNpDrmIsAvailable.
// Behaviour follows RPCS3 `Emu/Cell/Modules/sceNp.cpp` (function of the same name) with no network
// service behind it. Handlers run under the kernel lock, so the init flag needs no mutex.
//
// Deviation from RPCS3: sceNpDrmIsAvailable only checks that the file exists. RPCS3 verifies the
// NPD/SCE header against the licensee key (and a RAP file) before accepting an edata/sdata file;
// here the game's sdata files are decrypted offline and served through the sdata cache, so there is
// no entitlement to verify.

#include <cstdint>
#include <string>

#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/vfs.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "sceNp";

constexpr std::uint32_t kErrorNotInitialized = 0x8002aa01;
constexpr std::uint32_t kErrorAlreadyInitialized = 0x8002aa02;
constexpr std::uint32_t kErrorInvalidArgument = 0x8002aa03;
constexpr std::uint32_t kErrorInsufficientBuffer = 0x8002aa0f;
constexpr std::uint32_t kDrmErrorInvalidParam = 0x80029502;

constexpr std::uint32_t kMinPoolSize = 128 * 1024;  // SCE_NP_MIN_POOL_SIZE
constexpr std::uint32_t kDrmPathMax = 0x100;

bool g_initialized = false;

// sceNpInit(u32 poolsize, void* poolptr)
void npInit(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t pool_size = argU32(ctx, 0);
    const std::uint32_t pool_ptr = argU32(ctx, 1);
    if (g_initialized) {
        ctx->gpr[3] = kErrorAlreadyInitialized;
        return;
    }
    if (pool_size == 0 || pool_ptr == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (pool_size < kMinPoolSize) {
        ctx->gpr[3] = kErrorInsufficientBuffer;
        return;
    }
    g_initialized = true;
    ctx->gpr[3] = kCellOk;
}

// sceNpTerm()
void npTerm(Tox2PpuContext* ctx, std::uint8_t*) {
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    g_initialized = false;
    ctx->gpr[3] = kCellOk;
}

// sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path)
void npDrmIsAvailable(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t path_addr = argU32(ctx, 1);
    if (path_addr == 0) {
        ctx->gpr[3] = kDrmErrorInvalidParam;
        return;
    }
    const std::string path = readGuestString(base, path_addr, kDrmPathMax);
    FsStat stat;
    const std::uint32_t error = fsStatPath(path, stat);
    logInfo("tox2: np: sceNpDrmIsAvailable \"{}\" -> {:#x}", path, error);
    ctx->gpr[3] = error;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x4885aa18, "sceNpTerm", npTerm},
        {kModule, 0xad218faf, "sceNpDrmIsAvailable", npDrmIsAvailable},
        {kModule, 0xbd28fdbf, "sceNpInit", npInit},
};

}  // namespace

std::span<const HleEntry> sceNpHandlers() { return kHandlers; }

void resetSceNp() { g_initialized = false; }

}  // namespace tox2

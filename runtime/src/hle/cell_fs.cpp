// HLE handlers of the sys_fs module (cellFs*). They forward to the same operations as the lv2 sys_fs
// syscalls, so both share one object table. Argument checks follow RPCS3 `Emu/Cell/Modules/cellFs.cpp`
// (function of the same name); the mode RPCS3 passes for CELL_FS_O_CREAT is dropped because host
// permissions are not modelled.

#include <cstdint>
#include <span>

#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "sys_fs";

// cellFsOpen(const char* path, s32 flags, u32* fd, const void* arg, u64 size)
void fsOpen(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t fd_addr = argU32(ctx, 2);
    if (fd_addr == 0) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    setResultCode(ctx, fsOpenGuest(base, argU32(ctx, 0), static_cast<std::int32_t>(argU32(ctx, 1)), fd_addr,
                                   argU32(ctx, 3), ctx->gpr[7]));
}

// cellFsSdataOpen(const char* path, s32 flags, u32* fd, const void* arg, u64 size); arg and size are
// ignored, the flags must be CELL_FS_O_RDONLY.
void fsSdataOpen(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx,
                  fsSdataOpenGuest(base, argU32(ctx, 0), static_cast<std::int32_t>(argU32(ctx, 1)), argU32(ctx, 2)));
}

// cellFsClose(u32 fd)
void fsClose(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, fsCloseFd(argU32(ctx, 0)));
}

// cellFsStat(const char* path, CellFsStat* sb)
void fsStat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t sb = argU32(ctx, 1);
    setResultCode(ctx, sb == 0 ? kCellEfault : fsStatGuest(base, argU32(ctx, 0), sb));
}

// cellFsFstat(u32 fd, CellFsStat* sb)
void fsFstat(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, fsFstatGuest(base, argU32(ctx, 0), argU32(ctx, 1)));
}

// cellFsMkdir(const char* path, s32 mode)
void fsMkdir(Tox2PpuContext* ctx, std::uint8_t* base) { setResultCode(ctx, fsMkdirGuest(base, argU32(ctx, 0))); }

// cellFsUnlink(const char* path)
void fsUnlink(Tox2PpuContext* ctx, std::uint8_t* base) { setResultCode(ctx, fsUnlinkGuest(base, argU32(ctx, 0))); }

// cellFsReadWithOffset(u32 fd, u64 offset, void* buf, u64 buffer_size, u64* nread)
void fsReadWithOffsetHandler(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx,
                  fsReadWithOffset(base, argU32(ctx, 0), ctx->gpr[4], argU32(ctx, 2), ctx->gpr[6], argU32(ctx, 4)));
}

// cellFsWriteWithOffset(u32 fd, u64 offset, const void* buf, u64 data_size, u64* nwrite)
void fsWriteWithOffsetHandler(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx,
                  fsWriteWithOffset(base, argU32(ctx, 0), ctx->gpr[4], argU32(ctx, 2), ctx->gpr[6], argU32(ctx, 4)));
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x0d5b4a14, "cellFsReadWithOffset", fsReadWithOffsetHandler},
        {kModule, 0x2cb51f0d, "cellFsClose", fsClose},
        {kModule, 0x718bf5f8, "cellFsOpen", fsOpen},
        {kModule, 0x7de6dced, "cellFsStat", fsStat},
        {kModule, 0x7f4677a8, "cellFsUnlink", fsUnlink},
        {kModule, 0x8cb722d5, "cellFsWriteWithOffset", fsWriteWithOffsetHandler},
        {kModule, 0xb1840b53, "cellFsSdataOpen", fsSdataOpen},
        {kModule, 0xba901fe6, "cellFsMkdir", fsMkdir},
        {kModule, 0xef3efa34, "cellFsFstat", fsFstat},
};

}  // namespace

std::span<const HleEntry> cellFsHandlers() { return kHandlers; }

}  // namespace tox2

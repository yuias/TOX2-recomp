#pragma once

#include <cstdint>

// Error codes and sync constants shared by the lv2 syscalls and the HLE modules. Values follow
// RPCS3 `Emu/Cell/ErrorCodes.h` and `Emu/Cell/lv2/sys_sync.h`, `sys_event_flag.h`, `sys_event.h`,
// `sys_ppu_thread.h` and `Modules/sys_lwmutex_.h`.
namespace tox2 {

inline constexpr std::uint32_t kCellOk = 0;
inline constexpr std::uint32_t kCellEagain = 0x80010001;
inline constexpr std::uint32_t kCellEinval = 0x80010002;
inline constexpr std::uint32_t kCellEnosys = 0x80010003;
inline constexpr std::uint32_t kCellEnomem = 0x80010004;
inline constexpr std::uint32_t kCellEsrch = 0x80010005;
inline constexpr std::uint32_t kCellEnoent = 0x80010006;
inline constexpr std::uint32_t kCellEnoexec = 0x80010007;
inline constexpr std::uint32_t kCellEdeadlk = 0x80010008;
inline constexpr std::uint32_t kCellEperm = 0x80010009;
inline constexpr std::uint32_t kCellEbusy = 0x8001000A;
inline constexpr std::uint32_t kCellEtimedout = 0x8001000B;
inline constexpr std::uint32_t kCellEabort = 0x8001000C;
inline constexpr std::uint32_t kCellEfault = 0x8001000D;
inline constexpr std::uint32_t kCellEstat = 0x8001000F;
inline constexpr std::uint32_t kCellEalign = 0x80010010;
inline constexpr std::uint32_t kCellEkresource = 0x80010011;
inline constexpr std::uint32_t kCellEcanceled = 0x80010013;
inline constexpr std::uint32_t kCellEexist = 0x80010014;
inline constexpr std::uint32_t kCellEisconn = 0x80010015;
inline constexpr std::uint32_t kCellEnotconn = 0x80010016;
inline constexpr std::uint32_t kCellEisdir = 0x80010012;
inline constexpr std::uint32_t kCellErofs = 0x80010026;
inline constexpr std::uint32_t kCellEacces = 0x80010029;
inline constexpr std::uint32_t kCellEbadf = 0x8001002A;
inline constexpr std::uint32_t kCellEio = 0x8001002B;
inline constexpr std::uint32_t kCellEmfile = 0x8001002C;
inline constexpr std::uint32_t kCellEnotdir = 0x8001002E;
inline constexpr std::uint32_t kCellEnametoolong = 0x80010034;
inline constexpr std::uint32_t kCellEnotempty = 0x80010036;
inline constexpr std::uint32_t kCellEfsspecific = 0x80010038;
inline constexpr std::uint32_t kCellEnotmounted = 0x8001003A;

// Synchronization object attributes.
inline constexpr std::uint32_t kSyncFifo = 1;
inline constexpr std::uint32_t kSyncPriority = 2;
inline constexpr std::uint32_t kSyncRetry = 4;
inline constexpr std::uint32_t kSyncRecursive = 0x10;

// Event flag wait modes.
inline constexpr std::uint32_t kWaitAnd = 1;
inline constexpr std::uint32_t kWaitOr = 2;
inline constexpr std::uint32_t kWaitClear = 0x10;
inline constexpr std::uint32_t kWaitClearAll = 0x20;

// PPU thread creation flags and the once-control states.
inline constexpr std::uint32_t kCreateJoinable = 1;
inline constexpr std::uint32_t kCreateInterrupt = 2;
inline constexpr std::uint32_t kOnceInit = 0;
inline constexpr std::uint32_t kDoneInit = 1;

// Owner field values of a guest sys_lwmutex_t.
inline constexpr std::uint32_t kLwmutexFree = 0xffffffff;
inline constexpr std::uint32_t kLwmutexDead = 0xfffffffe;
inline constexpr std::uint32_t kLwmutexReserved = 0xfffffffd;

}  // namespace tox2

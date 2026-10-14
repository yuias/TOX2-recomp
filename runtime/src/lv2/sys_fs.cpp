// lv2 file system syscalls over the host file system (see lv2/vfs.h for the path mapping).
// Behaviour and error codes follow RPCS3 `Emu/Cell/lv2/sys_fs.cpp` / `sys_fs.h`. Host I/O runs
// while the calling guest thread holds the kernel lock, so a long read blocks every other kernel section (other guest
// threads' syscalls, RSX and audio callbacks) for its duration, and in legacy mode all guest code; that is accepted for
// boot.

#include "lv2/sys_fs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tox2 {
namespace {

namespace stdfs = std::filesystem;

// Open flags (octal, as in RPCS3 `CELL_FS_O_*`).
constexpr std::int32_t kOWronly = 01;
constexpr std::int32_t kOAccMode = 03;
constexpr std::int32_t kOCreat = 0100;
constexpr std::int32_t kOExcl = 0200;
constexpr std::int32_t kOTrunc = 01000;
constexpr std::int32_t kOAppend = 02000;
constexpr std::int32_t kOMself = 010000;
constexpr std::int32_t kOUnk = 01000000;
constexpr std::int32_t kOKnownFlags = kOAccMode | kOCreat | kOExcl | kOTrunc | kOAppend | kOMself | kOUnk;

constexpr std::int32_t kSIfDir = 0040000;
constexpr std::int32_t kSIfReg = 0100000;

// Object ids: lv2_fs_object id_base 3, step 1, count 255 - 3, lowest free first.
constexpr std::uint32_t kIdBase = 3;
constexpr std::uint32_t kIdCount = 252;
constexpr std::size_t kMaxPathLength = 1024;  // CELL_FS_MAX_FS_PATH_LENGTH
constexpr std::size_t kMaxNameLength = 255;
// CellFsDirent: u8 d_type, u8 d_namlen, char d_name[256].
constexpr std::uint32_t kDirentSize = 258;
constexpr std::uint8_t kTypeDirectory = 1;
constexpr std::uint8_t kTypeRegular = 2;

struct DirEntry {
    std::string name;
    bool is_dir;
};

// A file or a directory snapshot; both share one id space like RPCS3's lv2_fs_object.
struct FsObject {
    FsObject() = default;
    FsObject(const FsObject&) = delete;
    FsObject& operator=(const FsObject&) = delete;
    ~FsObject() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }

    bool is_dir = false;
    VfsDevice device = VfsDevice::None;
    VfsDeviceInfo info = {512, true};
    bool read_only = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::int32_t flags = 0;
    std::vector<DirEntry> entries;
    std::size_t pos = 0;
};

std::mutex g_table_mutex;
std::map<std::uint32_t, std::shared_ptr<FsObject>> g_objects;

std::uint32_t addObject(std::shared_ptr<FsObject> object) {
    const std::lock_guard lock(g_table_mutex);
    std::uint32_t id = kIdBase;
    for (const auto& [used, unused] : g_objects) {
        if (used != id) {
            break;
        }
        id++;
    }
    if (id >= kIdBase + kIdCount) {
        return 0;
    }
    g_objects.emplace(id, std::move(object));
    return id;
}

std::shared_ptr<FsObject> findObject(std::uint32_t id, bool want_dir) {
    const std::lock_guard lock(g_table_mutex);
    const auto it = g_objects.find(id);
    return it != g_objects.end() && it->second->is_dir == want_dir ? it->second : nullptr;
}

bool removeObject(std::uint32_t id, bool want_dir) {
    const std::lock_guard lock(g_table_mutex);
    const auto it = g_objects.find(id);
    if (it == g_objects.end() || it->second->is_dir != want_dir) {
        return false;
    }
    g_objects.erase(it);
    return true;
}

bool guestRangeOk(std::uint32_t addr, std::uint64_t size) {
    return size <= 0xFFFFFFFFull && process().memory.isCommitted(addr, static_cast<std::uint32_t>(size));
}

// Reads the NUL-terminated guest path. Errors follow RPCS3 `translate_to_str`.
std::uint32_t readGuestPath(const std::uint8_t* base, std::uint32_t addr, std::string& out) {
    out.clear();
    if (addr == 0) {
        return kCellEfault;
    }
    for (std::uint64_t i = 0;; i++) {
        const std::uint64_t ea = addr + i;
        if (ea > 0xFFFFFFFFull) {
            return kCellEfault;
        }
        if ((i == 0 || (ea & 0xFFF) == 0) && !process().memory.isCommitted(static_cast<std::uint32_t>(ea), 1)) {
            return kCellEfault;
        }
        const char c = static_cast<char>(base[ea]);
        if (c == '\0') {
            break;
        }
        out.push_back(c);
        if (out.size() == kMaxPathLength + 1) {
            return kCellEnametoolong;
        }
    }
    return out.empty() || out[0] != '/' ? kCellEnoent : kCellOk;
}

// Logs the first occurrence of each (operation, path, result) so a game's file accesses show up
// without flooding the log.
void logPathOp(const char* op, std::string_view path, std::uint32_t code) {
    const std::string key = std::format("fs-{}:{}:{:x}", op, path, code);
    if (firstUse(key)) {
        if (code == kCellOk) {
            logInfo("tox2: fs: {} \"{}\" ok", op, path);
        } else {
            logWarn("tox2: fs: {} \"{}\" failed {:#010x}", op, path, code);
        }
    }
}

// Like readGuestPath, and reports a path the guest could not provide (once per operation and code).
std::uint32_t readOpPath(const char* op, const std::uint8_t* base, std::uint32_t addr, std::string& out) {
    const std::uint32_t code = readGuestPath(base, addr, out);
    if (code != kCellOk) {
        logOnce(std::format("fs-badpath-{}-{:x}", op, code),
                "tox2: fs: {} with an unusable path: {:#010x} (pointer {:#010x}, \"{}\")", op, code, addr,
                out.substr(0, 64));
    }
    return code;
}

std::uint32_t vfsError(VfsStatus status) { return status == VfsStatus::NotMounted ? kCellEnotmounted : kCellEnoent; }

// A missing path where an ancestor is a regular file is ENOTDIR on the console.
std::uint32_t notFoundOrNotDir(const stdfs::path& host) {
    for (stdfs::path parent = host.parent_path(); !parent.empty() && parent != parent.parent_path();
         parent = parent.parent_path()) {
        std::error_code ec;
        const stdfs::file_status status = stdfs::status(parent, ec);
        if (stdfs::exists(status)) {
            return stdfs::is_directory(status) ? kCellEnoent : kCellEnotdir;
        }
    }
    return kCellEnoent;
}

std::uint32_t mapWinError(DWORD error, const stdfs::path& host) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return notFoundOrNotDir(host);
        case ERROR_INVALID_NAME: return kCellEnoent;
        case ERROR_DIRECTORY: return kCellEnotdir;
        case ERROR_ACCESS_DENIED: return kCellEacces;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return kCellEexist;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return kCellEbusy;
        case ERROR_NEGATIVE_SEEK:
        case ERROR_INVALID_PARAMETER: return kCellEinval;
        default:
            logOnce(std::format("fs-win-error-{}", error), "tox2: fs: unmapped host error {}", error);
            return kCellEio;
    }
}

std::int64_t toUnixTime(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr std::int64_t kEpochDiffSeconds = 11644473600;  // 1601 -> 1970
    return static_cast<std::int64_t>(value.QuadPart / 10000000ull) - kEpochDiffSeconds;
}

FsStat makeStat(bool is_dir, bool read_only, const VfsDeviceInfo& info, std::uint64_t size, const FILETIME& atime,
                const FILETIME& mtime, const FILETIME& ctime) {
    FsStat out;
    out.mode = is_dir ? (kSIfDir | 0777) : (kSIfReg | 0666);
    if (read_only) {
        out.mode &= ~0222;
    }
    out.uid = out.gid = info.no_uid_gid ? -1 : 0;
    out.atime = toUnixTime(atime);
    out.mtime = toUnixTime(mtime);
    out.ctime = toUnixTime(ctime);
    out.size = is_dir ? info.block_size : size;
    out.blksize = info.block_size;
    return out;
}

std::uint32_t openFile(std::string_view path, std::int32_t flags, std::uint32_t& fd) {
    const VfsResolution res = resolveVfs(path);
    if (res.device == VfsDevice::Root) {
        return kCellEisdir;
    }
    if (res.status != VfsStatus::Ok) {
        return vfsError(res.status);
    }

    std::error_code ec;
    const stdfs::file_status status = stdfs::status(res.host, ec);
    const bool exists = stdfs::exists(status);
    if (stdfs::is_directory(status)) {
        return kCellEisdir;
    }

    // RPCS3 treats these as internal errors; the guest sees EINVAL instead of a stop.
    if ((flags & kOAccMode) == kOAccMode || (flags & ~kOKnownFlags) != 0 ||
        ((flags & kOMself) != 0 && (flags & ~kOMself) != 0)) {
        return kCellEinval;
    }
    if ((flags & kOUnk) != 0) {
        logOnce("fs-open-unk-flag", "tox2: fs: open with unknown flag 01000000 (flags {:#o})", flags);
    }

    const std::int32_t access_mode = flags & kOAccMode;
    const bool no_write = res.read_only || res.protected_parent;
    if (res.read_only && access_mode != 0 && exists) {
        return kCellEperm;
    }
    if (no_write) {
        if (!exists) {
            return (flags & kOCreat) != 0 ? (res.read_only ? kCellEperm : kCellEacces) : notFoundOrNotDir(res.host);
        }
        if ((flags & kOTrunc) != 0) {
            return res.read_only ? kCellEperm : kCellEacces;
        }
    }

    DWORD access = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    if (!no_write) {
        const bool create = (flags & kOCreat) != 0;
        const bool trunc = (flags & kOTrunc) != 0;
        if (access_mode == kOWronly) {
            access = GENERIC_WRITE;
        } else if (access_mode != 0) {
            access = GENERIC_READ | GENERIC_WRITE;
        }
        if (trunc) {
            access |= GENERIC_WRITE;
        }
        if (create && (flags & kOExcl) != 0) {
            disposition = CREATE_NEW;
        } else if (create && trunc) {
            disposition = CREATE_ALWAYS;
        } else if (create) {
            disposition = OPEN_ALWAYS;
        } else if (trunc) {
            disposition = TRUNCATE_EXISTING;
        }
    }

    // Sharing everything lets the guest unlink or reopen a file it still holds open.
    const HANDLE handle = CreateFileW(res.host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return mapWinError(GetLastError(), res.host);
    }

    auto object = std::make_shared<FsObject>();
    object->file = handle;
    object->flags = flags;
    object->device = res.device;
    object->info = res.info;
    object->read_only = res.read_only;
    fd = addObject(std::move(object));
    return fd != 0 ? kCellOk : kCellEmfile;
}

std::uint32_t openDirectory(std::string_view path, std::uint32_t& fd) {
    const VfsResolution res = resolveVfs(path);
    if (res.status != VfsStatus::Ok) {
        return vfsError(res.status);
    }

    auto object = std::make_shared<FsObject>();
    object->is_dir = true;
    object->device = res.device;
    object->info = res.info;
    object->read_only = res.read_only;

    if (res.device == VfsDevice::Root) {
        for (std::string& name : vfsMountNames()) {
            object->entries.push_back({std::move(name), true});
        }
    } else {
        std::error_code ec;
        const stdfs::file_status status = stdfs::status(res.host, ec);
        if (!stdfs::exists(status)) {
            return notFoundOrNotDir(res.host);
        }
        if (!stdfs::is_directory(status)) {
            return kCellEnotdir;
        }
        stdfs::directory_iterator it(res.host, stdfs::directory_options::skip_permission_denied, ec);
        for (const stdfs::directory_iterator end; !ec && it != end; it.increment(ec)) {
            const std::u8string name = it->path().filename().u8string();
            std::error_code type_ec;
            object->entries.push_back(
                    {std::string(reinterpret_cast<const char*>(name.data()), name.size()), it->is_directory(type_ec)});
        }
        if (ec) {
            return kCellEio;
        }
    }

    // The console lists "." and ".." first and the rest sorted by name.
    auto& entries = object->entries;
    std::erase_if(entries, [](const DirEntry& e) { return e.name == "." || e.name == ".."; });
    std::ranges::sort(entries, {}, &DirEntry::name);
    entries.erase(std::ranges::unique(entries, {}, &DirEntry::name).begin(), entries.end());
    entries.insert(entries.begin(), {DirEntry{".", true}, DirEntry{"..", true}});

    fd = addObject(std::move(object));
    return fd != 0 ? kCellOk : kCellEmfile;
}

bool setPos(HANDLE file, std::int64_t offset, DWORD method, std::uint64_t* result) {
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    LARGE_INTEGER position;
    if (!SetFilePointerEx(file, distance, &position, method)) {
        return false;
    }
    if (result != nullptr) {
        *result = static_cast<std::uint64_t>(position.QuadPart);
    }
    return true;
}

// Per-interval figures behind takeFsTransferStats(); relaxed, since they only feed a periodic log line.
struct TransferCounters {
    std::atomic<std::uint64_t> count{0}, bytes{0}, max_us{0};

    void record(std::uint64_t transferred, std::uint64_t us) {
        count.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(transferred, std::memory_order_relaxed);
        std::uint64_t seen = max_us.load(std::memory_order_relaxed);
        while (us > seen && !max_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
        }
    }
};
TransferCounters g_read_counters, g_write_counters;

// Reads or writes `size` bytes at the current position; returns the byte count and sets `code` on
// a host error. Timed as a whole, because the host call runs under the kernel lock.
std::uint64_t transfer(HANDLE file, bool write, std::uint8_t* data, std::uint64_t size, std::uint32_t& code) {
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t total = 0;
    while (total < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::uint64_t>(size - total, 0x40000000));
        DWORD done = 0;
        const BOOL ok = write ? WriteFile(file, data + total, chunk, &done, nullptr)
                              : ReadFile(file, data + total, chunk, &done, nullptr);
        if (!ok) {
            code = mapWinError(GetLastError(), {});
            break;
        }
        total += done;
        if (done < chunk) {
            break;
        }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    (write ? g_write_counters : g_read_counters).record(total, static_cast<std::uint64_t>(us.count()));
    return total;
}

// Like transfer() at an absolute offset, leaving the file position where it was (the console's
// cellFsReadWithOffset/cellFsWriteWithOffset do not move it either).
std::uint64_t transferAt(HANDLE file, bool write, std::uint64_t offset, std::uint8_t* data, std::uint64_t size,
                         std::uint32_t& code) {
    std::uint64_t saved = 0;
    if (!setPos(file, 0, FILE_CURRENT, &saved)) {
        code = mapWinError(GetLastError(), {});
        return 0;
    }
    std::uint64_t total = 0;
    if (offset > static_cast<std::uint64_t>(INT64_MAX)) {
        // Beyond any file: a read hits the end, a write cannot be placed.
        code = write ? kCellEinval : kCellOk;
    } else if (!setPos(file, static_cast<std::int64_t>(offset), FILE_BEGIN, nullptr)) {
        code = mapWinError(GetLastError(), {});
    } else {
        total = transfer(file, write, data, size, code);
    }
    setPos(file, static_cast<std::int64_t>(saved), FILE_BEGIN, nullptr);
    return total;
}

// The container kind a guest asks for through the open argument (RPCS3 `lv2_file::open`): a
// big-endian u64 at `arg` when the argument size is 8.
enum class FileKind { Plain, Sdata, Edata };
constexpr std::uint64_t kArgSdata = 0x18000000010;
constexpr std::uint64_t kArgEdata = 2;

// Opens an sdata container file. The runtime does not decrypt: it serves a copy decrypted ahead
// of time (outside this repository) from the --sdata-cache directory, which holds each file at its path
// relative to the game directory, or a file override. Without either the file is reported missing.
std::uint32_t openSdata(std::string_view path, std::uint32_t& fd) {
    const VfsResolution res = resolveVfs(path);
    if (res.device == VfsDevice::Root) {
        return kCellEisdir;
    }
    if (res.status != VfsStatus::Ok) {
        return vfsError(res.status);
    }
    const std::optional<stdfs::path> cached = sdataHostPath(res);
    if (!cached) {
        logOnce(LogLevel::Warn, std::format("fs-sdata:{}", path), "tox2: fs: sdata open not supported: {}", path);
        return kCellEnoent;
    }
    const HANDLE handle =
            CreateFileW(cached->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        logOnce(LogLevel::Warn, std::format("fs-sdata:{}", path),
                "tox2: fs: no readable decrypted copy of sdata file {}", path);
        return kCellEnoent;
    }
    auto object = std::make_shared<FsObject>();
    object->file = handle;
    object->device = res.device;
    object->info = res.info;
    object->read_only = true;
    fd = addObject(std::move(object));
    return fd != 0 ? kCellOk : kCellEmfile;
}

std::uint32_t openGuest(std::uint8_t* base, std::uint32_t path_addr, std::int32_t flags, std::uint32_t fd_addr,
                        FileKind kind) {
    std::string path;
    std::uint32_t code = readOpPath("open", base, path_addr, path);
    if (code != kCellOk) {
        return code;
    }
    std::uint32_t fd = 0;
    if (kind == FileKind::Sdata) {
        code = openSdata(path, fd);
        if (code == kCellOk) {
            logPathOp("sdata-open", path, code);
        }
    } else {
        if (kind == FileKind::Edata) {
            logOnce("fs-open-edata", "tox2: fs: edata open is not decrypted, \"{}\" opens as a plain file", path);
        }
        code = openFile(path, flags, fd);
        logPathOp("open", path, code);
    }
    if (code == kCellOk && !guestRangeOk(fd_addr, 4)) {
        removeObject(fd, false);
        code = kCellEfault;
    }
    if (code == kCellOk) {
        tox2Write32(base, fd_addr, fd);
    }
    return code;
}

}  // namespace

FsTransferStats takeFsTransferStats() {
    FsTransferStats out;
    out.reads = g_read_counters.count.exchange(0, std::memory_order_relaxed);
    out.read_bytes = g_read_counters.bytes.exchange(0, std::memory_order_relaxed);
    out.read_max_us = g_read_counters.max_us.exchange(0, std::memory_order_relaxed);
    out.writes = g_write_counters.count.exchange(0, std::memory_order_relaxed);
    out.write_bytes = g_write_counters.bytes.exchange(0, std::memory_order_relaxed);
    out.write_max_us = g_write_counters.max_us.exchange(0, std::memory_order_relaxed);
    return out;
}

std::uint32_t fsStatPath(std::string_view guest_path, FsStat& out) {
    const VfsResolution res = resolveVfs(guest_path);
    if (res.device == VfsDevice::Root) {
        // RPCS3 reports the admin file system root with fixed values.
        out = FsStat{kSIfDir | 0711, -1, -1, -1, -1, -1, 258, 512};
        return kCellOk;
    }
    if (res.status != VfsStatus::Ok) {
        return vfsError(res.status);
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(res.host.c_str(), GetFileExInfoStandard, &data)) {
        return mapWinError(GetLastError(), res.host);
    }
    const bool is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (res.dir_only && !is_dir) {
        return kCellEnotdir;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out = makeStat(is_dir, res.read_only, res.info, size, data.ftLastAccessTime, data.ftLastWriteTime,
                   data.ftCreationTime);
    return kCellOk;
}

std::uint32_t fsStatFd(std::uint32_t fd, FsStat& out) {
    const std::shared_ptr<FsObject> object = findObject(fd, false);
    if (object == nullptr) {
        return kCellEbadf;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(object->file, &info)) {
        return kCellEio;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    out = makeStat(false, object->read_only, object->info, size, info.ftLastAccessTime, info.ftLastWriteTime,
                   info.ftCreationTime);
    return kCellOk;
}

void writeCellFsStat(std::uint8_t* base, std::uint32_t addr, const FsStat& stat) {
    tox2Write32(base, addr, static_cast<std::uint32_t>(stat.mode));
    tox2Write32(base, addr + 4, static_cast<std::uint32_t>(stat.uid));
    tox2Write32(base, addr + 8, static_cast<std::uint32_t>(stat.gid));
    tox2Write64(base, addr + 12, static_cast<std::uint64_t>(stat.atime));
    tox2Write64(base, addr + 20, static_cast<std::uint64_t>(stat.mtime));
    tox2Write64(base, addr + 28, static_cast<std::uint64_t>(stat.ctime));
    tox2Write64(base, addr + 36, stat.size);
    tox2Write64(base, addr + 44, stat.blksize);
}

// The mode argument is not used: host permissions are not modelled.
std::uint32_t fsOpenGuest(std::uint8_t* base, std::uint32_t path_addr, std::int32_t flags, std::uint32_t fd_addr,
                          std::uint32_t arg_addr, std::uint64_t arg_size) {
    FileKind kind = FileKind::Plain;
    if (arg_size == 8) {
        if (!guestRangeOk(arg_addr, 8)) {
            return kCellEfault;
        }
        const std::uint64_t marker = tox2Read64(base, arg_addr);
        kind = marker == kArgSdata ? FileKind::Sdata : marker == kArgEdata ? FileKind::Edata : FileKind::Plain;
    }
    return openGuest(base, path_addr, flags, fd_addr, kind);
}

std::uint32_t fsSdataOpenGuest(std::uint8_t* base, std::uint32_t path_addr, std::int32_t flags, std::uint32_t fd_addr) {
    if (flags != 0) {
        return kCellEinval;
    }
    if (fd_addr == 0) {
        return kCellEfault;
    }
    return openGuest(base, path_addr, 0, fd_addr, FileKind::Sdata);
}

std::uint32_t fsCloseFd(std::uint32_t fd) { return removeObject(fd, false) ? kCellOk : kCellEbadf; }

std::uint32_t fsStatGuest(std::uint8_t* base, std::uint32_t path_addr, std::uint32_t sb) {
    std::string path;
    std::uint32_t code = readOpPath("stat", base, path_addr, path);
    if (code != kCellOk) {
        return code;
    }
    FsStat stat;
    code = fsStatPath(path, stat);
    if (code == kCellOk && !guestRangeOk(sb, kCellFsStatSize)) {
        code = kCellEfault;
    }
    if (code == kCellOk) {
        writeCellFsStat(base, sb, stat);
    }
    logPathOp("stat", path, code);
    return code;
}

std::uint32_t fsFstatGuest(std::uint8_t* base, std::uint32_t fd, std::uint32_t sb) {
    FsStat stat;
    std::uint32_t code = fsStatFd(fd, stat);
    if (code == kCellOk && !guestRangeOk(sb, kCellFsStatSize)) {
        code = kCellEfault;
    }
    if (code == kCellOk) {
        writeCellFsStat(base, sb, stat);
    }
    return code;
}

std::uint32_t fsMkdirGuest(std::uint8_t* base, std::uint32_t path_addr) {
    std::string path;
    std::uint32_t code = readOpPath("mkdir", base, path_addr, path);
    if (code != kCellOk) {
        return code;
    }
    const VfsResolution res = resolveVfs(path);
    if (res.device == VfsDevice::Root) {
        code = kCellEexist;
    } else if (res.status != VfsStatus::Ok) {
        code = vfsError(res.status);
    } else if (res.read_only) {
        code = kCellErofs;
    } else {
        std::error_code ec;
        if (stdfs::exists(stdfs::symlink_status(res.host, ec))) {
            code = kCellEexist;
        } else if (res.protected_parent) {
            code = kCellEacces;
        } else if (!CreateDirectoryW(res.host.c_str(), nullptr)) {
            code = mapWinError(GetLastError(), res.host);
        }
    }
    logPathOp("mkdir", path, code);
    return code;
}

std::uint32_t fsUnlinkGuest(std::uint8_t* base, std::uint32_t path_addr) {
    std::string path;
    std::uint32_t code = readOpPath("unlink", base, path_addr, path);
    if (code != kCellOk) {
        return code;
    }
    const VfsResolution res = resolveVfs(path);
    if (res.device == VfsDevice::Root) {
        code = kCellEisdir;
    } else if (res.status != VfsStatus::Ok) {
        code = vfsError(res.status);
    } else {
        std::error_code ec;
        if (stdfs::is_directory(stdfs::status(res.host, ec))) {
            code = kCellEisdir;
        } else if (res.read_only) {
            code = kCellErofs;
        } else if (!DeleteFileW(res.host.c_str())) {
            code = mapWinError(GetLastError(), res.host);
        }
    }
    logPathOp("unlink", path, code);
    return code;
}

// The console reaches these through sys_fs_fcntl (RPCS3 `sys_fs_fcntl` 0x8000000a / 0x8000000b).
std::uint32_t fsReadWithOffset(std::uint8_t* base, std::uint32_t fd, std::uint64_t offset, std::uint32_t buf,
                               std::uint64_t size, std::uint32_t nread_addr) {
    if (nread_addr != 0 && !guestRangeOk(nread_addr, 8)) {
        return kCellEfault;
    }
    const auto finish = [&](std::uint32_t code, std::uint64_t done) {
        if (nread_addr != 0) {
            tox2Write64(base, nread_addr, code == kCellOk ? done : 0);
        }
        return code;
    };

    const std::shared_ptr<FsObject> file = findObject(fd, false);
    if (file == nullptr || (file->flags & kOWronly) != 0) {
        return finish(kCellEbadf, 0);
    }
    if (size != 0 && (buf == 0 || !guestRangeOk(buf, size))) {
        return finish(kCellEfault, 0);
    }
    std::uint32_t code = kCellOk;
    const std::uint64_t done = transferAt(file->file, false, offset, base + buf, size, code);
    return finish(code, done);
}

std::uint32_t fsWriteWithOffset(std::uint8_t* base, std::uint32_t fd, std::uint64_t offset, std::uint32_t buf,
                                std::uint64_t size, std::uint32_t nwrite_addr) {
    if (nwrite_addr != 0 && !guestRangeOk(nwrite_addr, 8)) {
        return kCellEfault;
    }
    const auto finish = [&](std::uint32_t code, std::uint64_t done) {
        if (nwrite_addr != 0) {
            tox2Write64(base, nwrite_addr, code == kCellOk ? done : 0);
        }
        return code;
    };

    if (buf == 0) {
        return finish(kCellEfault, 0);
    }
    const std::shared_ptr<FsObject> file = findObject(fd, false);
    // The positional write is refused on append-mode files, as on the console.
    if (file == nullptr || (file->flags & kOAccMode) == 0 || (file->flags & kOAppend) != 0) {
        return finish(kCellEbadf, 0);
    }
    if (file->read_only) {
        return finish(kCellErofs, 0);
    }
    if (size != 0 && !guestRangeOk(buf, size)) {
        return finish(kCellEfault, 0);
    }
    std::uint32_t code = kCellOk;
    const std::uint64_t done = transferAt(file->file, true, offset, base + buf, size, code);
    return finish(code, done);
}

// 801 sys_fs_open(const char* path, s32 flags, u32* fd, s32 mode, const void* arg, u64 size).
void sysFsOpen(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, fsOpenGuest(base, static_cast<std::uint32_t>(ctx->gpr[3]),
                                   static_cast<std::int32_t>(ctx->gpr[4]), static_cast<std::uint32_t>(ctx->gpr[5]),
                                   static_cast<std::uint32_t>(ctx->gpr[7]), ctx->gpr[8]));
}

// 802 sys_fs_read(u32 fd, void* buf, u64 nbytes, u64* nread)
void sysFsRead(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto fd = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto buf = static_cast<std::uint32_t>(ctx->gpr[4]);
    const std::uint64_t nbytes = ctx->gpr[5];
    const auto nread = static_cast<std::uint32_t>(ctx->gpr[6]);

    if (nread == 0 || !guestRangeOk(nread, 8)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    if (buf == 0) {
        tox2Write64(base, nread, 0);
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::shared_ptr<FsObject> file = findObject(fd, false);
    if (file == nullptr || (nbytes != 0 && (file->flags & kOWronly) != 0)) {
        tox2Write64(base, nread, 0);
        setResultCode(ctx, kCellEbadf);
        return;
    }
    if (nbytes != 0 && !guestRangeOk(buf, nbytes)) {
        tox2Write64(base, nread, 0);
        setResultCode(ctx, kCellEfault);
        return;
    }

    std::uint32_t code = kCellOk;
    const std::uint64_t total = transfer(file->file, false, base + buf, nbytes, code);
    tox2Write64(base, nread, total);
    setResultCode(ctx, code);
}

// 803 sys_fs_write(u32 fd, const void* buf, u64 nbytes, u64* nwrite)
void sysFsWrite(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto fd = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto buf = static_cast<std::uint32_t>(ctx->gpr[4]);
    const std::uint64_t nbytes = ctx->gpr[5];
    const auto nwrite = static_cast<std::uint32_t>(ctx->gpr[6]);

    if (nwrite == 0 || !guestRangeOk(nwrite, 8)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    if (buf == 0) {
        tox2Write64(base, nwrite, 0);
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::shared_ptr<FsObject> file = findObject(fd, false);
    if (file == nullptr || (nbytes != 0 && (file->flags & kOAccMode) == 0)) {
        tox2Write64(base, nwrite, 0);
        setResultCode(ctx, kCellEbadf);
        return;
    }
    if (nbytes == 0) {
        tox2Write64(base, nwrite, 0);
        setResultCode(ctx, kCellOk);
        return;
    }
    if (file->read_only) {
        tox2Write64(base, nwrite, 0);
        setResultCode(ctx, kCellErofs);
        return;
    }
    if (!guestRangeOk(buf, nbytes)) {
        tox2Write64(base, nwrite, 0);
        setResultCode(ctx, kCellEfault);
        return;
    }

    if ((file->flags & kOAppend) != 0) {
        setPos(file->file, 0, FILE_END, nullptr);
    }
    std::uint32_t code = kCellOk;
    const std::uint64_t total = transfer(file->file, true, base + buf, nbytes, code);
    tox2Write64(base, nwrite, total);
    setResultCode(ctx, code);
}

// 804 sys_fs_close(u32 fd)
void sysFsClose(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, fsCloseFd(static_cast<std::uint32_t>(ctx->gpr[3])));
}

// 805 sys_fs_opendir(const char* path, u32* fd)
void sysFsOpendir(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto path_addr = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto fd_addr = static_cast<std::uint32_t>(ctx->gpr[4]);

    std::string path;
    std::uint32_t code = readOpPath("opendir", base, path_addr, path);
    std::uint32_t fd = 0;
    if (code == kCellOk) {
        code = openDirectory(path, fd);
        if (code == kCellOk && !guestRangeOk(fd_addr, 4)) {
            removeObject(fd, true);
            code = kCellEfault;
        }
        logPathOp("opendir", path, code);
    }
    if (code == kCellOk) {
        tox2Write32(base, fd_addr, fd);
    }
    setResultCode(ctx, code);
}

// 806 sys_fs_readdir(u32 fd, CellFsDirent* dirent, u64* nread). At the end the dirent is zeroed and
// nread is 0; on hdd1 the console writes the last entry again instead.
void sysFsReaddir(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto fd = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto dirent = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto nread = static_cast<std::uint32_t>(ctx->gpr[5]);

    if (dirent == 0 || nread == 0 || !guestRangeOk(dirent, kDirentSize) || !guestRangeOk(nread, 8)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const std::shared_ptr<FsObject> dir = findObject(fd, true);
    if (dir == nullptr) {
        setResultCode(ctx, kCellEbadf);
        return;
    }

    const DirEntry* entry = nullptr;
    if (dir->pos < dir->entries.size()) {
        entry = &dir->entries[dir->pos++];
    }
    const bool at_end = entry == nullptr;
    if (at_end && dir->device == VfsDevice::Hdd1) {
        entry = &dir->entries.back();
    }

    std::memset(base + dirent, 0, kDirentSize);
    if (entry != nullptr) {
        const std::size_t length = std::min(entry->name.size(), kMaxNameLength);
        base[dirent] = entry->is_dir ? kTypeDirectory : kTypeRegular;
        base[dirent + 1] = static_cast<std::uint8_t>(length);
        std::memcpy(base + dirent + 2, entry->name.data(), length);
    }
    tox2Write64(base, nread, at_end ? 0 : kDirentSize);
    setResultCode(ctx, kCellOk);
}

// 807 sys_fs_closedir(u32 fd)
void sysFsClosedir(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, removeObject(static_cast<std::uint32_t>(ctx->gpr[3]), true) ? kCellOk : kCellEbadf);
}

// 808 sys_fs_stat(const char* path, CellFsStat* sb)
void sysFsStat(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx,
                  fsStatGuest(base, static_cast<std::uint32_t>(ctx->gpr[3]), static_cast<std::uint32_t>(ctx->gpr[4])));
}

// 809 sys_fs_fstat(u32 fd, CellFsStat* sb)
void sysFsFstat(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx,
                  fsFstatGuest(base, static_cast<std::uint32_t>(ctx->gpr[3]), static_cast<std::uint32_t>(ctx->gpr[4])));
}

// 811 sys_fs_mkdir(const char* path, s32 mode)
void sysFsMkdir(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, fsMkdirGuest(base, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// 814 sys_fs_unlink(const char* path)
void sysFsUnlink(Tox2PpuContext* ctx, std::uint8_t* base) {
    setResultCode(ctx, fsUnlinkGuest(base, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// 818 sys_fs_lseek(u32 fd, s64 offset, s32 whence, u64* pos)
void sysFsLseek(Tox2PpuContext* ctx, std::uint8_t* base) {
    const auto fd = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto offset = static_cast<std::int64_t>(ctx->gpr[4]);
    const auto whence = static_cast<std::uint32_t>(ctx->gpr[5]);
    const auto pos = static_cast<std::uint32_t>(ctx->gpr[6]);

    const std::shared_ptr<FsObject> file = findObject(fd, false);
    if (file == nullptr) {
        setResultCode(ctx, kCellEbadf);
        return;
    }
    if (whence >= 3) {
        setResultCode(ctx, kCellEinval);
        return;
    }
    if (pos == 0 || !guestRangeOk(pos, 8)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    // FILE_BEGIN, FILE_CURRENT and FILE_END have the same values as SEEK_SET, SEEK_CUR and SEEK_END.
    std::uint64_t result = 0;
    if (!setPos(file->file, offset, whence, &result)) {
        setResultCode(ctx, mapWinError(GetLastError(), {}));
        return;
    }
    tox2Write64(base, pos, result);
    setResultCode(ctx, kCellOk);
}

}  // namespace tox2

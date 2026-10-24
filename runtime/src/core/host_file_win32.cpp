#include "core/host_file.h"

#include <algorithm>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace tox2::host {
namespace {

FileError classify(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return FileError::NotFound;
        case ERROR_INVALID_NAME: return FileError::InvalidName;
        case ERROR_DIRECTORY: return FileError::NotDirectory;
        case ERROR_ACCESS_DENIED: return FileError::AccessDenied;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return FileError::Exists;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return FileError::Busy;
        case ERROR_NEGATIVE_SEEK:
        case ERROR_INVALID_PARAMETER: return FileError::InvalidArgument;
        default: return FileError::Other;
    }
}

std::unexpected<FileFailure> lastFailure() {
    const DWORD error = GetLastError();
    return std::unexpected(FileFailure{classify(error), static_cast<std::int64_t>(error)});
}

HANDLE handleOf(std::intptr_t native) { return reinterpret_cast<HANDLE>(native); }

std::int64_t toUnixTime(const FILETIME& ft) {
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr std::int64_t kEpochDiffSeconds = 11644473600;  // 1601 -> 1970
    return static_cast<std::int64_t>(value.QuadPart / 10000000ull) - kEpochDiffSeconds;
}

PathInfo makeInfo(bool is_dir, DWORD size_high, DWORD size_low, const FILETIME& atime, const FILETIME& mtime,
                  const FILETIME& ctime) {
    PathInfo info{};
    info.is_dir = is_dir;
    info.size = (static_cast<std::uint64_t>(size_high) << 32) | size_low;
    info.times = {toUnixTime(atime), toUnixTime(mtime), toUnixTime(ctime)};
    return info;
}

}  // namespace

File::File(File&& other) noexcept : native_(std::exchange(other.native_, -1)) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        native_ = std::exchange(other.native_, -1);
    }
    return *this;
}

File::~File() { close(); }

void File::close() {
    if (native_ != -1) {
        CloseHandle(handleOf(native_));
        native_ = -1;
    }
}

std::expected<File, FileFailure> File::open(const std::filesystem::path& path, bool read, bool write,
                                            OpenMode mode) {
    const DWORD access = (read ? GENERIC_READ : 0) | (write ? GENERIC_WRITE : 0);
    DWORD disposition = OPEN_EXISTING;
    switch (mode) {
        case OpenMode::OpenExisting: disposition = OPEN_EXISTING; break;
        case OpenMode::CreateNew: disposition = CREATE_NEW; break;
        case OpenMode::CreateAlways: disposition = CREATE_ALWAYS; break;
        case OpenMode::OpenAlways: disposition = OPEN_ALWAYS; break;
        case OpenMode::TruncateExisting: disposition = TRUNCATE_EXISTING; break;
    }
    // Sharing everything lets the guest unlink or reopen a file it still holds open.
    const HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return lastFailure();
    }
    return File(reinterpret_cast<std::intptr_t>(handle));
}

std::expected<std::uint64_t, FileFailure> File::read(std::uint8_t* data, std::uint64_t size) {
    const DWORD chunk = static_cast<DWORD>(std::min(size, kMaxTransferBytes));
    DWORD done = 0;
    if (!ReadFile(handleOf(native_), data, chunk, &done, nullptr)) {
        return lastFailure();
    }
    return static_cast<std::uint64_t>(done);
}

std::expected<std::uint64_t, FileFailure> File::write(const std::uint8_t* data, std::uint64_t size) {
    const DWORD chunk = static_cast<DWORD>(std::min(size, kMaxTransferBytes));
    DWORD done = 0;
    if (!WriteFile(handleOf(native_), data, chunk, &done, nullptr)) {
        return lastFailure();
    }
    return static_cast<std::uint64_t>(done);
}

std::expected<std::uint64_t, FileFailure> File::seek(std::int64_t offset, int whence) {
    // FILE_BEGIN, FILE_CURRENT and FILE_END have the same values as 0, 1 and 2.
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    LARGE_INTEGER position;
    if (!SetFilePointerEx(handleOf(native_), distance, &position, static_cast<DWORD>(whence))) {
        return lastFailure();
    }
    return static_cast<std::uint64_t>(position.QuadPart);
}

std::expected<PathInfo, FileFailure> File::info() const {
    BY_HANDLE_FILE_INFORMATION data;
    if (!GetFileInformationByHandle(handleOf(native_), &data)) {
        return lastFailure();
    }
    return makeInfo((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, data.nFileSizeHigh, data.nFileSizeLow,
                    data.ftLastAccessTime, data.ftLastWriteTime, data.ftCreationTime);
}

std::expected<PathInfo, FileFailure> statPath(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return lastFailure();
    }
    return makeInfo((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, data.nFileSizeHigh, data.nFileSizeLow,
                    data.ftLastAccessTime, data.ftLastWriteTime, data.ftCreationTime);
}

std::expected<void, FileFailure> createDirectory(const std::filesystem::path& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        return lastFailure();
    }
    return {};
}

std::expected<void, FileFailure> removeFile(const std::filesystem::path& path) {
    if (!DeleteFileW(path.c_str())) {
        return lastFailure();
    }
    return {};
}

}  // namespace tox2::host

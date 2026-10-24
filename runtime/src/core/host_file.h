#pragma once

// File access of the host OS, without OS types: CreateFileW and friends on Windows, open and friends
// elsewhere. Which implementation is built is decided by the build files.

#include <cstdint>
#include <expected>
#include <filesystem>

namespace tox2::host {

// The host error classes the lv2 file system distinguishes; everything else is Other.
enum class FileError { NotFound, InvalidName, NotDirectory, AccessDenied, Exists, Busy, InvalidArgument, Other };

struct FileFailure {
    FileError kind;
    std::int64_t native;  // Win32 error or errno, for log lines
};

// What open() does when the file does or does not exist.
enum class OpenMode {
    OpenExisting,      // fails when missing
    CreateNew,         // fails when present
    CreateAlways,      // creates, or empties the existing file (needs write access)
    OpenAlways,        // creates when missing, keeps the contents otherwise
    TruncateExisting,  // empties the existing file; fails when missing (needs write access)
};

struct FileTimes {
    std::int64_t atime;
    std::int64_t mtime;
    std::int64_t ctime;  // creation time where the OS keeps one
};  // Unix seconds

struct PathInfo {
    bool is_dir;
    std::uint64_t size;
    FileTimes times;
};

// The most bytes one read() or write() call transfers; larger requests are cut to this.
inline constexpr std::uint64_t kMaxTransferBytes = 0x40000000;

// An open file; closed when destroyed. Windows opens with every share mode, so the guest can unlink
// or reopen a file it still holds open; other systems have no such restriction.
class File {
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    ~File();

    static std::expected<File, FileFailure> open(const std::filesystem::path& path, bool read, bool write,
                                                 OpenMode mode);
    // Takes ownership of an open HANDLE or file descriptor.
    static File adopt(std::intptr_t native) { return File(native); }

    // Transfers at most min(size, kMaxTransferBytes) bytes at the current position and returns the
    // count. A read count below the request means the end of the file.
    std::expected<std::uint64_t, FileFailure> read(std::uint8_t* data, std::uint64_t size);
    std::expected<std::uint64_t, FileFailure> write(const std::uint8_t* data, std::uint64_t size);

    // `whence` is 0 (from the start), 1 (from the current position) or 2 (from the end). Returns the
    // new position.
    std::expected<std::uint64_t, FileFailure> seek(std::int64_t offset, int whence);

    std::expected<PathInfo, FileFailure> info() const;
    bool isOpen() const { return native_ != -1; }

private:
    explicit File(std::intptr_t native) : native_(native) {}
    void close();

    // HANDLE or file descriptor; -1 when closed (INVALID_HANDLE_VALUE is -1 too).
    std::intptr_t native_ = -1;
};

std::expected<PathInfo, FileFailure> statPath(const std::filesystem::path& path);
std::expected<void, FileFailure> createDirectory(const std::filesystem::path& path);
std::expected<void, FileFailure> removeFile(const std::filesystem::path& path);

}  // namespace tox2::host

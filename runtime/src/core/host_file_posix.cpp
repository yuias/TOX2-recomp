#include "core/host_file.h"

#include <algorithm>
#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tox2::host {
namespace {

FileError classify(int error) {
    switch (error) {
        case ENOENT: return FileError::NotFound;
        case ENAMETOOLONG: return FileError::InvalidName;
        case ENOTDIR: return FileError::NotDirectory;
        case EACCES:
        case EPERM:
        case EROFS:
        case EISDIR: return FileError::AccessDenied;
        case EEXIST: return FileError::Exists;
        case EBUSY:
        case ETXTBSY: return FileError::Busy;
        case EINVAL: return FileError::InvalidArgument;
        default: return FileError::Other;
    }
}

std::unexpected<FileFailure> failure(int error) { return std::unexpected(FileFailure{classify(error), error}); }

std::int64_t seconds(const timespec& time) { return static_cast<std::int64_t>(time.tv_sec); }

PathInfo toPathInfo(const struct stat& st) {
    PathInfo info{};
    info.is_dir = S_ISDIR(st.st_mode);
    info.size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
    info.times = {seconds(st.st_atimespec), seconds(st.st_mtimespec), seconds(st.st_birthtimespec)};
#else
    // No creation time in struct stat here; the inode change time is the closest value.
    info.times = {seconds(st.st_atim), seconds(st.st_mtim), seconds(st.st_ctim)};
#endif
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
        ::close(static_cast<int>(native_));
        native_ = -1;
    }
}

std::expected<File, FileFailure> File::open(const std::filesystem::path& path, bool read, bool write,
                                            OpenMode mode) {
    int flags = O_CLOEXEC | (read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY);
    switch (mode) {
        case OpenMode::OpenExisting: break;
        case OpenMode::CreateNew: flags |= O_CREAT | O_EXCL; break;
        case OpenMode::CreateAlways: flags |= O_CREAT | O_TRUNC; break;
        case OpenMode::OpenAlways: flags |= O_CREAT; break;
        case OpenMode::TruncateExisting: flags |= O_TRUNC; break;
    }
    // The creation mode is cut by the umask; host permissions are not modelled.
    const int fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
        return failure(errno);
    }
    File file(fd);
    // Windows refuses to open a directory as a file, open() on a directory for reading succeeds.
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        return failure(EISDIR);
    }
    return file;
}

std::expected<std::uint64_t, FileFailure> File::read(std::uint8_t* data, std::uint64_t size) {
    const std::size_t chunk = static_cast<std::size_t>(std::min(size, kMaxTransferBytes));
    ssize_t done;
    do {
        done = ::read(static_cast<int>(native_), data, chunk);
    } while (done < 0 && errno == EINTR);
    if (done < 0) {
        return failure(errno);
    }
    return static_cast<std::uint64_t>(done);
}

std::expected<std::uint64_t, FileFailure> File::write(const std::uint8_t* data, std::uint64_t size) {
    const std::size_t chunk = static_cast<std::size_t>(std::min(size, kMaxTransferBytes));
    ssize_t done;
    do {
        done = ::write(static_cast<int>(native_), data, chunk);
    } while (done < 0 && errno == EINTR);
    if (done < 0) {
        return failure(errno);
    }
    return static_cast<std::uint64_t>(done);
}

std::expected<std::uint64_t, FileFailure> File::seek(std::int64_t offset, int whence) {
    const int origin = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : -1;
    const off_t position = origin < 0 ? off_t{-1} : lseek(static_cast<int>(native_), static_cast<off_t>(offset), origin);
    if (position < 0) {
        return failure(origin < 0 ? EINVAL : errno);
    }
    return static_cast<std::uint64_t>(position);
}

std::expected<PathInfo, FileFailure> File::info() const {
    struct stat st;
    if (fstat(static_cast<int>(native_), &st) != 0) {
        return failure(errno);
    }
    return toPathInfo(st);
}

std::expected<PathInfo, FileFailure> statPath(const std::filesystem::path& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return failure(errno);
    }
    return toPathInfo(st);
}

std::expected<void, FileFailure> createDirectory(const std::filesystem::path& path) {
    if (mkdir(path.c_str(), 0777) != 0) {
        return failure(errno);
    }
    return {};
}

std::expected<void, FileFailure> removeFile(const std::filesystem::path& path) {
    if (unlink(path.c_str()) != 0) {
        return failure(errno);
    }
    return {};
}

}  // namespace tox2::host

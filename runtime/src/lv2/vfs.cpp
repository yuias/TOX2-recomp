// Guest path resolution. Device layout follows RPCS3 `Emu/System.cpp` (`Emulator::Load` mounts
// /dev_hdd0 and /app_home; /app_home is the directory of the boot ELF) and the mount point table
// in `Emu/Cell/lv2/sys_fs.cpp` (read-only flags, block sizes).

#include "lv2/vfs.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

#include "core/log.h"

namespace tox2 {
namespace {

struct MountEntry {
    std::string name;
    VfsDevice device = VfsDevice::Other;
    std::filesystem::path host;
    bool read_only = false;
    VfsDeviceInfo info = {};
};

struct FileEntry {
    std::filesystem::path host;
    bool read_only = true;
    VfsDevice device = VfsDevice::Other;
    VfsDeviceInfo info = {};
};

struct VfsState {
    std::vector<MountEntry> mounts;
    std::map<std::string, FileEntry, std::less<>> files;
    std::optional<std::filesystem::path> sdata_cache;

    const MountEntry* findMount(std::string_view name) const {
        for (const MountEntry& mount : mounts) {
            if (mount.name == name) {
                return &mount;
            }
        }
        return nullptr;
    }
};

// Readers take a snapshot of the pointer, so a resolution never copies the tables.
std::mutex g_mutex;
std::shared_ptr<const VfsState> g_state = std::make_shared<const VfsState>();

std::shared_ptr<const VfsState> snapshot() {
    const std::lock_guard lock(g_mutex);
    return g_state;
}

VfsDevice deviceForName(std::string_view name) {
    if (name == "dev_bdvd") return VfsDevice::Bdvd;
    if (name == "app_home") return VfsDevice::AppHome;
    if (name == "dev_hdd0") return VfsDevice::Hdd0;
    if (name == "dev_hdd1") return VfsDevice::Hdd1;
    return VfsDevice::Other;
}

// Components the host would interpret instead of treating them as plain names.
bool isHostSafeName(std::string_view name) {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) {
            return false;
        }
    }
    // Win32 device names open the device whatever the extension is.
    const std::string_view stem = name.substr(0, name.find('.'));
    std::string upper;
    for (const char c : stem) {
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return false;
    }
    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' &&
        upper[3] <= '9') {
        return false;
    }
    return true;
}

std::filesystem::path appendUtf8(std::filesystem::path path, std::string_view name) {
    const std::u8string utf8(reinterpret_cast<const char8_t*>(name.data()), name.size());
    path /= std::filesystem::path(utf8);
    return path;
}

// Directories the console's own software keeps on hdd0; a game may not create entries directly
// under /dev_hdd0 or /dev_hdd0/game, so they must exist up front. The set follows RPCS3
// `Emu/System.cpp` `Emulator::Init`, with its default user id 00000001.
constexpr std::string_view kHdd0Skeleton[] = {"game",
                                              "home/00000001/exdata",
                                              "home/00000001/savedata",
                                              "home/00000001/trophy",
                                              "savedata",
                                              "photo",
                                              "music",
                                              "theme",
                                              "video",
                                              "drm",
                                              "vsh",
                                              "crash_report",
                                              "tmp",
                                              "mms",
                                              "data",
                                              "vm"};

bool endsWithDirMarker(std::string_view guest) {
    return guest.ends_with('/') || guest.ends_with("/.") || guest.ends_with("/..");
}

}  // namespace

void configureVfs(VfsLayout layout) {
    auto state = std::make_shared<VfsState>();
    std::error_code ec;
    for (VfsMount& mount : layout.mounts) {
        MountEntry entry;
        entry.name = std::move(mount.name);
        entry.device = deviceForName(entry.name);
        entry.info = vfsDeviceInfo(entry.device);
        if (mount.block_size) {
            entry.info.block_size = *mount.block_size;
        }
        entry.read_only = mount.read_only;
        entry.host = std::move(mount.host);
        if (!entry.read_only) {
            std::filesystem::create_directories(entry.host, ec);
            if (entry.device == VfsDevice::Hdd0) {
                for (const std::string_view sub : kHdd0Skeleton) {
                    if (!ec) {
                        std::filesystem::create_directories(entry.host / sub, ec);
                    }
                }
            }
            if (ec) {
                logWarn("tox2: fs: cannot create the directories of /{}: {}", entry.name, ec.message());
                ec.clear();
            }
        }
        state->mounts.push_back(std::move(entry));
    }
    for (VfsFileOverride& file : layout.files) {
        FileEntry entry;
        entry.host = std::move(file.host);
        entry.read_only = file.read_only;
        // The device of a file is its first guest component; an unmounted one is just "other".
        const size_t slash = file.guest.find('/', 1);
        const MountEntry* mount = state->findMount(std::string_view(file.guest).substr(1, slash - 1));
        entry.device = mount != nullptr ? mount->device : VfsDevice::Other;
        entry.info = mount != nullptr ? mount->info : vfsDeviceInfo(VfsDevice::Other);
        state->files[std::move(file.guest)] = std::move(entry);
    }
    state->sdata_cache = std::move(layout.sdata_cache);
    const std::lock_guard lock(g_mutex);
    g_state = std::move(state);
}

std::vector<VfsMount> gameDirMounts(const std::filesystem::path& game_dir) {
    return {{"dev_bdvd", game_dir, true, std::nullopt},
            {"app_home", game_dir / "PS3_GAME" / "USRDIR", true, std::nullopt}};
}

std::vector<VfsMount> hddDirMounts(const std::filesystem::path& hdd_dir) {
    return {{"dev_hdd0", hdd_dir / "dev_hdd0", false, std::nullopt},
            {"dev_hdd1", hdd_dir / "dev_hdd1", false, std::nullopt}};
}

void configureVfs(std::filesystem::path game_dir, std::optional<std::filesystem::path> hdd_dir) {
    VfsLayout layout;
    layout.mounts = gameDirMounts(game_dir);
    if (hdd_dir) {
        for (VfsMount& mount : hddDirMounts(*hdd_dir)) {
            layout.mounts.push_back(std::move(mount));
        }
    }
    configureVfs(std::move(layout));
}

void configureSdataCache(std::optional<std::filesystem::path> cache_dir) {
    const std::lock_guard lock(g_mutex);
    auto state = std::make_shared<VfsState>(*g_state);
    state->sdata_cache = std::move(cache_dir);
    g_state = std::move(state);
}

std::optional<std::filesystem::path> sdataHostPath(const VfsResolution& res) {
    if (res.status != VfsStatus::Ok) {
        return std::nullopt;
    }
    if (res.file_override) {
        return res.host;
    }
    if (res.disc_path.empty()) {
        return std::nullopt;
    }
    const auto state = snapshot();
    if (!state->sdata_cache) {
        return std::nullopt;
    }
    return *state->sdata_cache / res.disc_path;
}

VfsResolution resolveVfs(std::string_view guest) {
    VfsResolution result;
    if (!guest.starts_with('/')) {
        return result;
    }
    result.dir_only = endsWithDirMarker(guest);

    // Lexical normalisation; the first component is the device.
    std::vector<std::string_view> parts;
    bool escaped = false;
    for (size_t pos = 0; pos < guest.size();) {
        const size_t end = std::min(guest.find('/', pos), guest.size());
        const std::string_view name = guest.substr(pos, end - pos);
        pos = end + 1;
        if (name.empty() || name == ".") {
            continue;
        }
        if (name == "..") {
            // The parent of "/" is "/"; leaving a device is refused.
            if (parts.size() == 1) {
                escaped = true;
            }
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(name);
    }

    if (escaped) {
        result.status = VfsStatus::NotFound;
        return result;
    }
    if (parts.empty()) {
        result.status = VfsStatus::Ok;
        result.device = VfsDevice::Root;
        result.read_only = true;
        return result;
    }
    const std::string_view device_name = parts.front();
    const auto state = snapshot();

    // An override names one exact guest path and wins over any mount.
    if (!state->files.empty()) {
        std::string key;
        for (const std::string_view part : parts) {
            key += '/';
            key += part;
        }
        const auto it = state->files.find(key);
        if (it != state->files.end()) {
            result.status = VfsStatus::Ok;
            result.device = it->second.device;
            result.info = it->second.info;
            result.host = it->second.host;
            result.read_only = it->second.read_only;
            result.file_override = true;
            return result;
        }
    }

    const MountEntry* mount = state->findMount(device_name);
    if (mount == nullptr) {
        if (device_name == "dev_hdd0" || device_name == "dev_hdd1") {
            logOnce("vfs-no-hdd-dir", "tox2: fs: {} used without an hdd mount (--hdd-dir or the VFS config)",
                    std::string(device_name));
            result.status = VfsStatus::NotFound;
        }
        return result;
    }
    result.device = mount->device;
    result.read_only = mount->read_only;
    result.info = mount->info;

    result.host = mount->host;
    try {
        for (size_t i = 1; i < parts.size(); i++) {
            if (!isHostSafeName(parts[i])) {
                result.status = VfsStatus::NotFound;
                result.host.clear();
                return result;
            }
            result.host = appendUtf8(std::move(result.host), parts[i]);
        }
    } catch (const std::exception&) {
        // A name that is not valid UTF-8 cannot exist on the host.
        result.status = VfsStatus::NotFound;
        result.host.clear();
        return result;
    }
    result.status = VfsStatus::Ok;
    result.protected_parent =
            result.device == VfsDevice::Hdd0 && (parts.size() == 2 || (parts.size() == 3 && parts[1] == "game"));
    if (result.device == VfsDevice::Bdvd || result.device == VfsDevice::AppHome) {
        if (result.device == VfsDevice::AppHome) {
            result.disc_path = std::filesystem::path("PS3_GAME") / "USRDIR";
        }
        for (size_t i = 1; i < parts.size(); i++) {
            result.disc_path = appendUtf8(std::move(result.disc_path), parts[i]);
        }
    }
    return result;
}

std::optional<std::filesystem::path> resolveGuestPath(std::string_view guest, bool& read_only) {
    VfsResolution resolution = resolveVfs(guest);
    if (resolution.status != VfsStatus::Ok || resolution.device == VfsDevice::Root) {
        return std::nullopt;
    }
    read_only = resolution.read_only;
    return std::move(resolution.host);
}

std::vector<std::string> vfsMountNames() {
    std::vector<std::string> names;
    for (const MountEntry& mount : snapshot()->mounts) {
        names.push_back(mount.name);
    }
    return names;
}

VfsDeviceInfo vfsDeviceInfo(VfsDevice device) {
    switch (device) {
        case VfsDevice::Hdd0: return {4096, false};
        case VfsDevice::Hdd1: return {32768, true};
        case VfsDevice::Bdvd:
        case VfsDevice::AppHome:  // reflects the mount point of the boot ELF's directory
            return {2048, true};
        case VfsDevice::Other: return {4096, false};
        case VfsDevice::Root:
        case VfsDevice::None: break;
    }
    return {512, true};
}

}  // namespace tox2

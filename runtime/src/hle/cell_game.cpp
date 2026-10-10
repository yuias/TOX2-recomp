// HLE handlers of the cellGame module: the content-directory handshake a game performs at startup
// (cellGameDataCheck, cellGameCreateGameData, cellGameContentPermit) and cellGameContentErrorDialog.
// Behaviour follows RPCS3 `Emu/Cell/Modules/cellGame.cpp` (function of the same name); the
// lv2_sleep delays that emulate the console's slowness are omitted. Handlers run under the guest
// lock, so the shared handshake state needs no mutex of its own.
//
// Deviations from RPCS3: cellGameContentPermit does not rewrite the PARAM.SFO of an existing game
// data directory (nothing here modifies it), and a failed temporary-directory move removes the
// temporary directory instead of leaving it behind. The content directories are created on the
// host through the VFS because the guest may not create entries under /dev_hdd0/game.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/fatal.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/vfs.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellGame";

constexpr std::uint32_t kErrorParam = 0x8002cb07;
constexpr std::uint32_t kErrorAccess = 0x8002cb09;
constexpr std::uint32_t kErrorNotSupported = 0x8002cb21;
constexpr std::uint32_t kErrorFailure = 0x8002cb22;
constexpr std::uint32_t kErrorBusy = 0x8002cb23;
constexpr std::uint32_t kErrorExist = 0x8002cb26;
constexpr std::uint32_t kErrorBroken = 0x8002cb05;
constexpr std::uint32_t kRetNone = 2;  // CELL_GAME_RET_NONE: success, the directory does not exist

constexpr std::uint32_t kPathMax = 128;     // CELL_GAME_PATH_MAX
constexpr std::uint32_t kDirNameSize = 32;  // CELL_GAME_DIRNAME_SIZE
constexpr std::uint32_t kTypeDisc = 1;      // CELL_GAME_GAMETYPE_DISC
constexpr std::uint32_t kTypeGameData = 3;  // CELL_GAME_GAMETYPE_GAMEDATA
constexpr std::uint32_t kDirNameReadMax = 255;

// CellGameContentSize offsets and the free space RPCS3 reports (40 GiB minus 256 KiB, in KiB).
constexpr std::uint32_t kSizeHddFree = 0;
constexpr std::uint32_t kSizeContent = 4;
constexpr std::uint32_t kSizeSystem = 8;
constexpr std::int32_t kHddFreeSizeKb = 40 * 1024 * 1024 - 256;
constexpr std::int32_t kSizeNotCalculated = -1;  // CELL_GAME_SIZEKB_NOTCALC

// CellGameSetInitParams offsets and field sizes.
constexpr std::uint32_t kInitTitle = 0;
constexpr std::uint32_t kInitTitleSize = 128;
constexpr std::uint32_t kInitTitleId = 128;
constexpr std::uint32_t kInitTitleIdSize = 10;
constexpr std::uint32_t kInitVersion = 140;
constexpr std::uint32_t kInitVersionSize = 6;

// CELL_GAME_ERRDIALOG_*
constexpr std::int32_t kErrDialogBrokenGameData = 0;
constexpr std::int32_t kErrDialogBrokenHddGame = 1;
constexpr std::int32_t kErrDialogNoSpace = 2;
constexpr std::int32_t kErrDialogBrokenExitGameData = 100;
constexpr std::int32_t kErrDialogBrokenExitHddGame = 101;
constexpr std::int32_t kErrDialogNoSpaceExit = 102;

constexpr std::string_view kDiscContentDir = "/dev_bdvd/PS3_GAME";
constexpr std::string_view kHddGameRoot = "/dev_hdd0/game/";

// Handshake state between DataCheck, CreateGameData and ContentPermit (RPCS3 content_permission).
struct ContentPermission {
    bool active = false;  // a check is outstanding (RPCS3: its init_mutex is initialised)
    std::string dir;      // content directory name below /dev_hdd0/game; empty for the disc
    ParamSfo sfo;
    std::filesystem::path temp;  // host path of the temporary directory from CreateGameData
    bool can_create = false;
    bool exists = false;

    void reset() { *this = ContentPermission{}; }
};

ContentPermission g_permission;

// Copies `text` into a guest char buffer of `capacity` bytes, truncating and NUL-terminating.
void storeGuestString(std::uint8_t* base, std::uint32_t addr, std::uint32_t capacity, std::string_view text) {
    const std::size_t n = std::min<std::size_t>(text.size(), capacity - 1);
    std::memcpy(base + addr, text.data(), n);
    base[addr + n] = 0;
}

std::string contentDir(std::string_view name) {
    return name.empty() ? std::string(kDiscContentDir) : std::string(kHddGameRoot) + std::string(name);
}

std::filesystem::path hostPathOf(const std::string& guest) {
    const VfsResolution resolved = resolveVfs(guest);
    return resolved.status == VfsStatus::Ok ? resolved.host : std::filesystem::path();
}

// RPCS3 psf::is_cat_hdd: any two-letter category except the disc and game data ones.
bool isHddCategory(std::string_view cat) { return cat.size() == 2 && cat[1] != 'D' && cat != "DG" && cat != "MS"; }

// cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
void gameDataCheck(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t type = argU32(ctx, 0);
    const std::uint32_t dir_name_addr = argU32(ctx, 1);
    const std::uint32_t size_addr = argU32(ctx, 2);

    if (type - 1 >= 3 || (type != kTypeDisc && dir_name_addr == 0)) {
        ctx->gpr[3] = kErrorParam;
        return;
    }
    std::string name;
    if (type != kTypeDisc) {
        name = readGuestString(base, dir_name_addr, kDirNameReadMax);
    }
    logInfo("dal: game: cellGameDataCheck type {} dirName \"{}\"", type, name);

    if (g_permission.active) {
        ctx->gpr[3] = kErrorBusy;
        return;
    }

    const std::string dir = type == kTypeDisc ? std::string(kDiscContentDir) : contentDir(name);
    const std::filesystem::path sfo_path = hostPathOf(dir + "/PARAM.SFO");
    ParamSfo sfo;
    bool file_missing = true;
    if (!sfo_path.empty()) {
        auto parsed = readParamSfoFile(sfo_path);
        if (parsed) {
            sfo = std::move(*parsed);
            file_missing = false;
        } else if (std::filesystem::exists(sfo_path)) {
            file_missing = false;  // present but unreadable: the category check below reports it
            logWarn("dal: game: {}: {}", sfo_path.string(), parsed.error());
        }
    }

    // RPCS3 only fails the check when the file could be opened; a missing file yields RET_NONE.
    if (!file_missing) {
        const auto category = sfo.strings.find("CATEGORY");
        const std::string_view cat = category != sfo.strings.end() ? category->second : std::string_view();
        const bool wrong = type == kTypeDisc ? cat != "DG" : type == kTypeGameData ? cat != "GD" : !isHddCategory(cat);
        if (wrong) {
            logWarn("dal: game: cellGameDataCheck: type {} does not match CATEGORY '{}'", type, cat);
            ctx->gpr[3] = kErrorBroken;
            return;
        }
    }

    if (size_addr != 0) {
        dalWrite32(base, size_addr + kSizeHddFree, static_cast<std::uint32_t>(kHddFreeSizeKb));
        dalWrite32(base, size_addr + kSizeContent, sfo.empty() ? 0u : static_cast<std::uint32_t>(kSizeNotCalculated));
        dalWrite32(base, size_addr + kSizeSystem, 0);
    }

    g_permission.reset();
    g_permission.active = true;
    g_permission.dir = std::move(name);
    g_permission.can_create = type == kTypeGameData;
    if (sfo.empty()) {
        logInfo("dal: game: cellGameDataCheck: directory '{}' not found", dir);
        ctx->gpr[3] = kRetNone;
        return;
    }
    g_permission.exists = true;
    g_permission.sfo = std::move(sfo);
    ctx->gpr[3] = kCellOk;
}

// cellGameContentPermit(char contentInfoPath[128], char usrdirPath[128])
void gameContentPermit(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t info_addr = argU32(ctx, 0);
    const std::uint32_t usrdir_addr = argU32(ctx, 1);
    if (info_addr == 0 || usrdir_addr == 0) {
        ctx->gpr[3] = kErrorParam;
        return;
    }
    if (!g_permission.active) {
        ctx->gpr[3] = kErrorFailure;
        return;
    }

    const std::string dir = contentDir(g_permission.dir);
    if (g_permission.temp.empty() && !g_permission.exists) {
        g_permission.reset();
        storeGuestString(base, info_addr, kPathMax, "");
        storeGuestString(base, usrdir_addr, kPathMax, "");
        ctx->gpr[3] = kCellOk;
        return;
    }

    if (!g_permission.temp.empty()) {
        std::error_code ec;
        const std::filesystem::path real_dir = hostPathOf(dir);
        const std::vector<std::uint8_t> sfo_bytes = serializeParamSfo(g_permission.sfo);
        bool ok = !real_dir.empty() && !std::filesystem::exists(real_dir, ec);
        if (ok) {
            std::ofstream out(g_permission.temp / "PARAM.SFO", std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(sfo_bytes.data()), static_cast<std::streamsize>(sfo_bytes.size()));
            ok = out.good();
            out.close();
        }
        if (ok) {
            std::filesystem::rename(g_permission.temp, real_dir, ec);
            ok = !ec;
        }
        if (ok) {
            logInfo("dal: game: directory '{}' has been created", dir);
        } else {
            logError("dal: game: cellGameContentPermit: failed to initialize directory '{}' ({})", dir,
                     ec ? ec.message() : std::string("target exists or cannot be written"));
            std::filesystem::remove_all(g_permission.temp, ec);
        }
    }

    g_permission.reset();
    storeGuestString(base, info_addr, kPathMax, dir);
    storeGuestString(base, usrdir_addr, kPathMax, dir + "/USRDIR");
    logInfo("dal: game: cellGameContentPermit -> \"{}\"", dir + "/USRDIR");
    ctx->gpr[3] = kCellOk;
}

// cellGameCreateGameData(CellGameSetInitParams* init, char tmpContentInfoPath[128],
//                        char tmpUsrdirPath[128])
void gameCreateGameData(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t init_addr = argU32(ctx, 0);
    const std::uint32_t info_addr = argU32(ctx, 1);
    const std::uint32_t usrdir_addr = argU32(ctx, 2);
    if (init_addr == 0) {
        ctx->gpr[3] = kErrorParam;
        return;
    }
    if (!g_permission.active || g_permission.dir.empty()) {
        ctx->gpr[3] = kErrorFailure;
        return;
    }
    if (!g_permission.can_create) {
        ctx->gpr[3] = kErrorNotSupported;
        return;
    }
    if (g_permission.exists) {
        ctx->gpr[3] = kErrorExist;
        return;
    }

    const std::string temp_name =
            "_GDATA_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string temp_guest = std::string(kHddGameRoot) + temp_name;
    const std::filesystem::path temp_host = hostPathOf(temp_guest);
    std::error_code ec;
    if (temp_host.empty() || !std::filesystem::create_directory(temp_host, ec)) {
        logError("dal: game: cellGameCreateGameData: failed to create directory '{}' ({})", temp_guest, ec.message());
        ctx->gpr[3] = kErrorAccess;
        return;
    }
    g_permission.temp = temp_host;  // from here on the directory is removed if the permit fails
    if (info_addr != 0) storeGuestString(base, info_addr, kPathMax, temp_guest);

    if (!std::filesystem::create_directory(temp_host / "USRDIR", ec)) {
        logError("dal: game: cellGameCreateGameData: failed to create '{}/USRDIR' ({})", temp_guest, ec.message());
        ctx->gpr[3] = kErrorAccess;
        return;
    }
    if (usrdir_addr != 0) storeGuestString(base, usrdir_addr, kPathMax, temp_guest + "/USRDIR");

    // Initial PARAM.SFO values replace whatever the check loaded.
    const auto text = [&](std::uint32_t offset, std::uint32_t size) {
        return readGuestString(base, init_addr + offset, size);
    };
    g_permission.sfo = ParamSfo{};
    g_permission.sfo.strings["CATEGORY"] = "GD";
    g_permission.sfo.strings["TITLE_ID"] = text(kInitTitleId, kInitTitleIdSize);
    g_permission.sfo.strings["TITLE"] = text(kInitTitle, kInitTitleSize);
    g_permission.sfo.strings["VERSION"] = text(kInitVersion, kInitVersionSize);
    logInfo("dal: game: cellGameCreateGameData: temporary directory '{}' title \"{}\"", temp_guest,
            g_permission.sfo.strings["TITLE"]);
    ctx->gpr[3] = kCellOk;
}

// cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
void gameContentErrorDialog(DalPpuContext* ctx, std::uint8_t* base) {
    const auto type = static_cast<std::int32_t>(argU32(ctx, 0));
    const auto need_kb = static_cast<std::int32_t>(argU32(ctx, 1));
    const std::uint32_t dir_name_addr = argU32(ctx, 2);

    std::string message;
    switch (type) {
        case kErrDialogBrokenGameData: message = "The game data is corrupted. The application will continue."; break;
        case kErrDialogBrokenHddGame: message = "The HDD boot game is corrupted. The application will continue."; break;
        case kErrDialogNoSpace:
            message = std::format("Not enough available space ({} KB needed). The application will continue.", need_kb);
            break;
        case kErrDialogBrokenExitGameData:
            message = "The game data is corrupted. The application will be terminated.";
            break;
        case kErrDialogBrokenExitHddGame:
            message = "The HDD boot game is corrupted. The application will be terminated.";
            break;
        case kErrDialogNoSpaceExit:
            message = std::format("Not enough available space ({} KB needed). The application will be terminated.",
                                  need_kb);
            break;
        default: ctx->gpr[3] = kErrorParam; return;
    }
    if (dir_name_addr != 0) {
        if (std::memchr(base + dir_name_addr, 0, kDirNameSize) == nullptr) {
            ctx->gpr[3] = kErrorParam;
            return;
        }
        message += std::format(" Directory: {}", readGuestString(base, dir_name_addr, kDirNameSize));
    }

    // There is no UI to show the dialog on. The terminating types end the application on the console;
    // here that is a game-data failure, so it stops with the fatal exit code instead of a clean exit.
    logError("dal: game: cellGameContentErrorDialog: {}", message);
    if (type > kErrDialogNoSpace) {
        stopProcess(ExitCode::Fatal,
                    std::format("dal: stop: cellGameContentErrorDialog (type {}) terminates the application: {}", type,
                                message));
    }
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x42a2e133, "cellGameCreateGameData", gameCreateGameData},
        {kModule, 0x70acec67, "cellGameContentPermit", gameContentPermit},
        {kModule, 0xb0a1f8c6, "cellGameContentErrorDialog", gameContentErrorDialog},
        {kModule, 0xdb9819f3, "cellGameDataCheck", gameDataCheck},
};

}  // namespace

std::span<const HleEntry> cellGameHandlers() { return kHandlers; }

void resetCellGame() { g_permission.reset(); }

}  // namespace dal

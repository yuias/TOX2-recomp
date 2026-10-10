// HLE handlers of the cellSaveData imports the game uses (exported by cellSysutil):
// cellSaveDataAutoLoad2, AutoSave2, ListLoad2, ListSave2 and Delete2. Behaviour follows RPCS3
// `Emu/Cell/Modules/cellSaveData.cpp` (`savedata_op`, `savedata_check_args`, `check_filename`,
// `select_and_delete`) and `cellSysutil.cpp` (`sysutil_check_name_string`). Handlers run under the
// guest lock; the callbacks (funcList, funcStat, funcFile) run synchronously on the calling thread
// through callGuestFunction, as in RPCS3, with their Get/Set/Result structures in one guest scratch
// block that is allocated on first use and reused (a nested or concurrent call gets BUSY).
//
// A save lives in /dev_hdd0/home/00000001/savedata/<dir name>/ with a PARAM.SFO (written through
// core/param_sfo) next to the files. Files are collected in memory and committed through a
// ".working_<dir>" directory and a ".backup_<dir>" swap, as RPCS3 does.
//
// Headless policy (RPCS3 opens dialogs and waits for the user; nothing here can):
//  - Error dialogs (errDialog of the Auto functions; ListSave2/ListLoad2 always ask for one): the
//    text is logged and the error code is returned, with RPCS3's NOREPEAT bookkeeping. No dialog
//    object is created, so the game sees no DRAWING_BEGIN/END events for it.
//  - The save list of ListSave2/ListLoad2 is never shown. The entry is chosen as the list would
//    highlight it (funcList's focus position); ListLoad2 falls back to the newest entry; ListSave2
//    falls back to the "new data" slot when the game supplied one; without a choice the call
//    returns CELL_CANCEL, as RPCS3 does without a save dialog. Confirmation prompts count as YES.
//  - cellSaveDataDelete2 only opens a selection UI in RPCS3, so it returns CELL_CANCEL.
//
// Deviations from RPCS3:
//  - Secure files are stored in plain text: the secure file id and the PFD/crypto layer are
//    ignored, but each file's secure flag ("*<name>" entries of PARAM.SFO) is kept so its file type
//    survives a reload.
//  - PARAM.SFO has no PARAMS/PARAMS2 blobs and no RPCS3_BLIST: files are listed alphabetically, and
//    every file of a committed save is rewritten, so unmodified files get a new modification time.
//  - The lv2_sleep delays that emulate the console's slowness, the auto-save indicator overlay and
//    the progress bar are omitted. A rename or write failure returns ACCESS_ERROR instead of
//    throwing.
//  - The SDK version of the executable is not available to the HLE modules; the checks that depend
//    on it follow the behaviour of an SDK newer than 4.0 (a name buffer of N bytes holds N-1
//    characters, an empty dirNamePrefix is rejected), which is what the shipped game's 4.30 has.
//  - Only user 00000001 exists; the user id variants of the functions are not implemented.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/vfs.h"

namespace tox2 {
namespace {

namespace stdfs = std::filesystem;

constexpr char kModule[] = "cellSysutil";

// CELL_SAVEDATA_ERROR_*
constexpr std::uint32_t kErrorCbResult = 0x8002b401;
constexpr std::uint32_t kErrorAccess = 0x8002b402;
constexpr std::uint32_t kErrorParam = 0x8002b404;
constexpr std::uint32_t kErrorBroken = 0x8002b406;
constexpr std::uint32_t kErrorFailure = 0x8002b407;
constexpr std::uint32_t kErrorBusy = 0x8002b408;
constexpr std::uint32_t kCancel = 1;  // CELL_CANCEL

// CELL_SAVEDATA_CBRESULT_*
constexpr std::int32_t kCbOkLastNoConfirm = 2;
constexpr std::int32_t kCbOkLast = 1;
constexpr std::int32_t kCbOkNext = 0;
constexpr std::int32_t kCbErrNoSpace = -1;
constexpr std::int32_t kCbErrFailure = -2;
constexpr std::int32_t kCbErrBroken = -3;
constexpr std::int32_t kCbErrNoData = -4;
constexpr std::int32_t kCbErrInvalid = -5;

// Sizes and limits (CellSaveDataParamSize and friends).
constexpr std::uint32_t kDirNameSize = 32;
constexpr std::uint32_t kFileNameSize = 13;
constexpr std::uint32_t kPrefixSize = 256;
constexpr std::uint32_t kListItemMax = 2048;
constexpr std::uint32_t kDirListMax = 2048;
constexpr std::uint32_t kTitleSize = 128;
constexpr std::uint32_t kSubTitleSize = 128;
constexpr std::uint32_t kDetailSize = 1024;
constexpr std::uint32_t kListParamSize = 8;
constexpr std::uint32_t kDirListSize = 48;   // sizeof(CellSaveDataDirList)
constexpr std::uint32_t kFileStatSize = 56;  // sizeof(CellSaveDataFileStat)

// Enumerations.
constexpr std::uint32_t kSortModifiedTime = 0;
constexpr std::uint32_t kSortSubtitle = 1;
constexpr std::uint32_t kOrderAscent = 1;
constexpr std::uint32_t kFocusDirName = 0;
constexpr std::uint32_t kFocusListHead = 1;
constexpr std::uint32_t kFocusListTail = 2;
constexpr std::uint32_t kFocusLatest = 3;
constexpr std::uint32_t kFocusOldest = 4;
constexpr std::uint32_t kFocusNewData = 5;
constexpr std::uint32_t kIconPosTail = 1;
constexpr std::uint32_t kFileOpRead = 0;
constexpr std::uint32_t kFileOpWrite = 1;
constexpr std::uint32_t kFileOpDelete = 2;
constexpr std::uint32_t kFileOpWriteNoTrunc = 3;
constexpr std::uint32_t kFileTypeSecure = 0;
constexpr std::uint32_t kFileTypeNormal = 1;
constexpr std::uint32_t kFileTypeIcon0 = 2;
constexpr std::uint32_t kFileTypeIcon1 = 3;
constexpr std::uint32_t kFileTypePic1 = 4;
constexpr std::uint32_t kFileTypeSnd0 = 5;
constexpr std::uint32_t kAttrNoDuplicate = 1;
constexpr std::uint32_t kRecreateNo = 0;
constexpr std::uint32_t kRecreateNoNoBroken = 1;
constexpr std::uint32_t kRecreateYes = 2;
constexpr std::uint32_t kRecreateYesResetOwner = 3;
constexpr std::uint32_t kRecreateMask = 0xfffeffff;
constexpr std::uint32_t kErrDialogNone = 0;
constexpr std::uint32_t kErrDialogAlways = 1;
constexpr std::uint32_t kErrDialogNoRepeat = 2;

// Free space RPCS3 reports (40 GiB minus 256 KiB, in KiB) and the fixed system size.
constexpr std::int32_t kHddFreeSizeKb = 40 * 1024 * 1024 - 256;
constexpr std::int32_t kSysSizeKb = 35;

// Guest structure layouts (all big-endian; extracted from `Modules/cellSaveData.h`).
// CellSaveDataSetList / CellSaveDataSetBuf
constexpr std::uint32_t kSetListSortType = 0;
constexpr std::uint32_t kSetListSortOrder = 4;
constexpr std::uint32_t kSetListPrefix = 8;
constexpr std::uint32_t kSetListReserved = 12;
constexpr std::uint32_t kSetBufDirListMax = 0;
constexpr std::uint32_t kSetBufFileListMax = 4;
constexpr std::uint32_t kSetBufReserved = 8;  // u32[6]
constexpr std::uint32_t kSetBufBufSize = 32;
constexpr std::uint32_t kSetBufBuf = 36;
// CellSaveDataCBResult
constexpr std::uint32_t kResultResult = 0;
constexpr std::uint32_t kResultErrNeedSizeKb = 8;
constexpr std::uint32_t kResultInvalidMsg = 12;
constexpr std::uint32_t kResultUserdata = 16;  // everything before it is cleared before a callback
// CellSaveDataListGet / CellSaveDataListSet / CellSaveDataListNewData / CellSaveDataDirList
constexpr std::uint32_t kListGetDirNum = 0;
constexpr std::uint32_t kListGetDirListNum = 4;
constexpr std::uint32_t kListGetDirList = 8;
constexpr std::uint32_t kListSetFocusPosition = 0;
constexpr std::uint32_t kListSetFocusDirName = 4;
constexpr std::uint32_t kListSetFixedListNum = 8;
constexpr std::uint32_t kListSetFixedList = 12;
constexpr std::uint32_t kListSetNewData = 16;
constexpr std::uint32_t kNewDataIconPosition = 0;
constexpr std::uint32_t kNewDataDirName = 4;
constexpr std::uint32_t kDirListDirName = 0;
constexpr std::uint32_t kDirListListParam = 32;
// CellSaveDataStatGet
constexpr std::uint32_t kStatGetHddFreeSizeKb = 0;
constexpr std::uint32_t kStatGetIsNewData = 4;
constexpr std::uint32_t kStatGetAtime = 8;
constexpr std::uint32_t kStatGetMtime = 16;
constexpr std::uint32_t kStatGetCtime = 24;
constexpr std::uint32_t kStatGetDirName = 32;
constexpr std::uint32_t kStatGetTitle = 64;
constexpr std::uint32_t kStatGetSubTitle = 192;
constexpr std::uint32_t kStatGetDetail = 320;
constexpr std::uint32_t kStatGetAttribute = 1344;
constexpr std::uint32_t kStatGetParentalLevel = 1348;
constexpr std::uint32_t kStatGetListParam = 1352;
constexpr std::uint32_t kStatGetSizeKb = 1620;
constexpr std::uint32_t kStatGetSysSizeKb = 1624;
constexpr std::uint32_t kStatGetFileNum = 1628;
constexpr std::uint32_t kStatGetFileListNum = 1632;
constexpr std::uint32_t kStatGetFileList = 1636;
// CellSaveDataStatSet / CellSaveDataSystemFileParam
constexpr std::uint32_t kStatSetSetParam = 0;
constexpr std::uint32_t kStatSetReCreateMode = 4;
constexpr std::uint32_t kParamTitle = 0;
constexpr std::uint32_t kParamSubTitle = 128;
constexpr std::uint32_t kParamDetail = 256;
constexpr std::uint32_t kParamAttribute = 1280;
constexpr std::uint32_t kParamParentalLevel = 1284;
constexpr std::uint32_t kParamListParam = 1288;
constexpr std::uint32_t kParamReserved = 1296;
constexpr std::uint32_t kParamReservedSize = 256;
// CellSaveDataFileStat
constexpr std::uint32_t kFileStatFileType = 0;
constexpr std::uint32_t kFileStatSizeOff = 8;
constexpr std::uint32_t kFileStatAtime = 16;
constexpr std::uint32_t kFileStatMtime = 24;
constexpr std::uint32_t kFileStatCtime = 32;
constexpr std::uint32_t kFileStatFileName = 40;
// CellSaveDataFileGet / CellSaveDataFileSet
constexpr std::uint32_t kFileGetExcSize = 0;
constexpr std::uint32_t kFileGetSize = 68;
constexpr std::uint32_t kFileSetOperation = 0;
constexpr std::uint32_t kFileSetFileType = 8;
constexpr std::uint32_t kFileSetFileName = 28;
constexpr std::uint32_t kFileSetFileOffset = 32;
constexpr std::uint32_t kFileSetFileSize = 36;
constexpr std::uint32_t kFileSetFileBufSize = 40;
constexpr std::uint32_t kFileSetFileBuf = 44;
constexpr std::uint32_t kFileSetSize = 48;

// Offsets of the structures inside the scratch block (RPCS3 `savedata_context`).
constexpr std::uint32_t kScratchResult = 0x000;
constexpr std::uint32_t kScratchListGet = 0x040;
constexpr std::uint32_t kScratchListSet = 0x0a0;
constexpr std::uint32_t kScratchStatGet = 0x100;
constexpr std::uint32_t kScratchStatSet = 0x800;
constexpr std::uint32_t kScratchFileGet = 0x820;
constexpr std::uint32_t kScratchFileSet = 0x880;
constexpr std::uint32_t kScratchUsed = 0x900;

constexpr std::string_view kSaveRoot = "/dev_hdd0/home/00000001/savedata";

enum class Op { AutoSave, AutoLoad, ListSave, ListLoad };

const char* opName(Op op) {
    switch (op) {
        case Op::AutoSave: return "AutoSave2";
        case Op::AutoLoad: return "AutoLoad2";
        case Op::ListSave: return "ListSave2";
        case Op::ListLoad: return "ListLoad2";
    }
    return "?";
}

bool isListOp(Op op) { return op == Op::ListSave || op == Op::ListLoad; }

std::uint32_t g_scratch = 0;
bool g_busy = false;
std::int32_t g_last_error_dialog = 0;  // last CBRESULT error that raised a dialog (NOREPEAT)

// sysutil_check_name_string on a guest or host string: 0 when valid, -1 for an invalid character
// (or a leading '_'), -2 for a bad length. `s` is cut at `maxlen` characters first, which is what
// the guest buffer's size does in RPCS3.
int checkNameString(std::string_view s, int min_len, int max_len) {
    std::string buf(s.substr(0, static_cast<std::size_t>(max_len)));
    buf.resize(static_cast<std::size_t>(max_len) + 1, '\0');
    const int last_pos = std::max(max_len - 1, 0);
    if (buf[0] == '_') return -1;
    for (int index = 0;; index++) {
        const char cur = buf[static_cast<std::size_t>(index)];
        if (cur == '\0' || index == max_len) {
            if (min_len > index || (max_len == index && buf[static_cast<std::size_t>(last_pos)] != '\0')) {
                return -2;
            }
            return 0;
        }
        if ((cur >= 'A' && cur <= 'Z') || (cur >= '0' && cur <= '9') || cur == '-' || cur == '_') continue;
        return -1;
    }
}

// RPCS3 `check_filename`: 0, or the parameter error number (70/71) the console logs.
int checkFilename(std::string_view path, bool disallow_system_files, bool account_sfo_pfd) {
    if (path.size() >= kFileNameSize) return 71;
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) dot = path.size();
    if (path.empty() || dot > 8 || path.size() - dot > 4) return 70;
    if (path == "." || (!account_sfo_pfd && (path == "PARAM.SFO" || path == "PARAM.PFD"))) return 70;

    if (dot != 0) {
        std::string name(path.substr(0, dot));
        std::replace(name.begin(), name.end(), '.', '-');
        if (name[0] == '_') name[0] = '-';
        if (disallow_system_files &&
            ((dot >= 5 && name.starts_with("PARAM")) || (dot >= 4 && name.starts_with("ICON")) ||
             (dot >= 3 && name.starts_with("PIC")) || (dot >= 3 && name.starts_with("SND")))) {
            return 70;
        }
        if (checkNameString(name, 1, 9) == -1) return 70;
    }
    if (path.size() > dot + 1) {
        std::string ext(path.substr(dot + 1));
        if (ext[0] == '_') ext[0] = '-';
        if (checkNameString(ext, 1, 4) == -1) return 70;
    }
    return 0;
}

bool isValidDirName(const std::string& name) {
    return !name.empty() && checkNameString(name, 1, static_cast<int>(kDirNameSize)) == 0;
}

// Copies `text` into a zero-filled guest char array of `capacity` bytes (RPCS3 strcpy_trunc).
void putText(std::uint8_t* base, std::uint32_t addr, std::uint32_t capacity, std::string_view text) {
    const std::size_t n = std::min<std::size_t>(text.size(), capacity - 1);
    std::memcpy(base + addr, text.data(), n);
    base[addr + n] = 0;
}

std::string getText(const std::uint8_t* base, std::uint32_t addr, std::uint32_t capacity) {
    return readGuestString(base, addr, capacity);
}

std::string sfoString(const ParamSfo& sfo, const std::string& key) {
    const auto it = sfo.strings.find(key);
    return it != sfo.strings.end() ? it->second : std::string();
}

std::uint32_t sfoInt(const ParamSfo& sfo, const std::string& key) {
    const auto it = sfo.ints.find(key);
    return it != sfo.ints.end() ? it->second : 0;
}

struct Times {
    std::int64_t atime = 0;
    std::int64_t mtime = 0;
    std::int64_t ctime = 0;
};

Times statTimes(const std::string& guest_path) {
    FsStat stat;
    if (fsStatPath(guest_path, stat) != kCellOk) return {};
    return {stat.atime, stat.mtime, stat.ctime};
}

// One save directory as the list and stat steps see it.
struct SaveEntry {
    std::string escaped;  // directory name on disk
    std::string dir_name;
    std::string list_param;
    std::string title;
    std::string subtitle;
    std::string details;
    std::uint64_t size = 0;
    Times times;
    bool is_new = false;
};

std::optional<std::vector<std::uint8_t>> readFile(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const stdfs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// The valid save directory names under `root`, sorted.
std::vector<std::string> listSaveDirs(const stdfs::path& root) {
    std::vector<std::string> names;
    std::error_code ec;
    for (stdfs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string name = it->path().filename().string();
        if (checkNameString(name, 1, static_cast<int>(kDirNameSize)) != 0) continue;
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Reads the entry of `escaped` from its PARAM.SFO; nullopt when the save has none or an invalid
// SAVEDATA_DIRECTORY (RPCS3 skips those).
std::optional<SaveEntry> loadEntry(const stdfs::path& root, const std::string& escaped) {
    const auto parsed = readParamSfoFile(root / escaped / "PARAM.SFO");
    if (!parsed || parsed->empty()) return std::nullopt;
    SaveEntry entry;
    entry.dir_name = sfoString(*parsed, "SAVEDATA_DIRECTORY");
    entry.list_param = sfoString(*parsed, "SAVEDATA_LIST_PARAM");
    entry.title = sfoString(*parsed, "TITLE");
    entry.subtitle = sfoString(*parsed, "SUB_TITLE");
    entry.details = sfoString(*parsed, "DETAIL");
    if (!isValidDirName(entry.dir_name)) {
        logError("tox2: savedata: '{}' has an invalid SAVEDATA_DIRECTORY entry ('{}')", escaped, entry.dir_name);
        return std::nullopt;
    }
    std::error_code ec;
    for (stdfs::directory_iterator it(root / escaped, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) continue;
        if (checkFilename(it->path().filename().string(), false, true) != 0) continue;
        entry.size += it->file_size(ec);
    }
    entry.times = statTimes(std::string(kSaveRoot) + "/" + escaped);
    entry.escaped = escaped;
    return entry;
}

// Everything one savedata_op call works with.
struct OpState {
    Tox2PpuContext* ctx = nullptr;
    std::uint8_t* base = nullptr;
    std::uint32_t result = 0, list_get = 0, list_set = 0, stat_get = 0, stat_set = 0, file_get = 0, file_set = 0;
};

std::int32_t read32s(const OpState& s, std::uint32_t addr) {
    return static_cast<std::int32_t>(tox2Read32(s.base, addr));
}

// Runs a (result, get, set) callback and returns the CBRESULT it stored. Only the part of the
// result before `userdata` is cleared, so the callback's userdata survives.
std::int32_t runCallback(OpState& s, std::uint32_t func, std::uint32_t get, std::uint32_t set) {
    std::memset(s.base + s.result, 0, kResultUserdata);
    callGuestFunction(s.ctx, s.base, func, {s.result, get, set});
    return read32s(s, s.result + kResultResult);
}

// RPCS3 `display_callback_result_error_message`, minus the blocking dialog (see the header).
std::uint32_t reportCallbackError(const OpState& s, std::int32_t result, std::uint32_t err_dialog) {
    std::string msg;
    switch (result) {
        case kCbErrNoSpace:
            msg = std::format("not enough space ({} KB needed)", read32s(s, s.result + kResultErrNeedSizeKb));
            break;
        case kCbErrFailure: msg = "the operation failed"; break;
        case kCbErrBroken: msg = "the save data is corrupted"; break;
        case kCbErrNoData: msg = "there is no save data"; break;
        case kCbErrInvalid: {
            const std::uint32_t text = tox2Read32(s.base, s.result + kResultInvalidMsg);
            msg = text != 0 ? getText(s.base, text, 256) : std::string("invalid");
            break;
        }
        default:
            logError("tox2: savedata: callback returned result {} (parameter error 22)", result);
            return kErrorParam;
    }
    const bool repeat = result == g_last_error_dialog;
    if (err_dialog == kErrDialogNoRepeat) g_last_error_dialog = result;
    if (err_dialog == kErrDialogNone || (err_dialog == kErrDialogNoRepeat && repeat)) {
        logInfo("tox2: savedata: callback result {}: {} (no dialog requested)", result, msg);
    } else {
        logWarn("tox2: savedata: error dialog (headless, not shown): {}", msg);
    }
    return kErrorCbResult;
}

std::uint32_t paramError(int number, std::string_view detail = {}) {
    logError("tox2: savedata: parameter error {}{}{}", number, detail.empty() ? "" : " ", detail);
    return kErrorParam;
}

// RPCS3 `savedata_check_args` for the four operations implemented here. 0 or the error number.
int checkArgs(const std::uint8_t* base, Op op, std::uint32_t version, std::uint32_t dir_name, std::uint32_t err_dialog,
              std::uint32_t set_list, std::uint32_t set_buf, std::uint32_t func_list, std::uint32_t func_stat) {
    if (version > 1) return 1;
    if (err_dialog > kErrDialogNoRepeat) return 5;

    if (!isListOp(op)) {
        if (dir_name == 0) return 2;
        switch (checkNameString(readGuestString(base, dir_name, kDirNameSize), 1, kDirNameSize)) {
            case -1: return 3;
            case -2: return 4;
            default: break;
        }
    } else {
        if (set_list == 0) return 11;
        if (tox2Read32(base, set_list + kSetListSortType) > kSortSubtitle) return 12;
        if (tox2Read32(base, set_list + kSetListSortOrder) > kOrderAscent) return 13;
        const std::uint32_t prefix = tox2Read32(base, set_list + kSetListPrefix);
        if (prefix == 0) return 15;
        if (std::memchr(base + prefix, 0, kPrefixSize) == nullptr || base[prefix] == 0) return 17;

        const std::string prefixes = readGuestString(base, prefix, kPrefixSize);
        std::size_t pos = 0;
        while (pos <= prefixes.size()) {
            std::size_t bar = prefixes.find('|', pos);
            if (bar == std::string::npos) bar = prefixes.size();
            const std::string_view piece = std::string_view(prefixes).substr(pos, bar - pos);
            if (!piece.empty()) {
                if (piece.size() > kDirNameSize) return 17;
                switch (checkNameString(piece, 1, kDirNameSize)) {
                    case -1: return 16;
                    case -2: return 17;
                    default: break;
                }
            }
            pos = bar + 1;
        }
        if (tox2Read32(base, set_list + kSetListReserved) != 0) return 14;
    }

    if (set_buf == 0) return 74;
    const std::uint32_t dir_list_max = tox2Read32(base, set_buf + kSetBufDirListMax);
    const std::uint32_t file_list_max = tox2Read32(base, set_buf + kSetBufFileListMax);
    const std::uint32_t buf_size = tox2Read32(base, set_buf + kSetBufBufSize);
    if (isListOp(op)) {
        if (dir_list_max > kDirListMax) return 8;
        if (static_cast<std::uint64_t>(dir_list_max) * kDirListSize > buf_size) return 7;
    }
    if (static_cast<std::uint64_t>(file_list_max) * kFileStatSize > buf_size) return 7;
    if (buf_size != 0 && tox2Read32(base, set_buf + kSetBufBuf) == 0) return 6;
    for (std::uint32_t i = 0; i < 6; i++) {
        if (tox2Read32(base, set_buf + kSetBufReserved + 4 * i) != 0) return 10;
    }
    if (isListOp(op) && func_list == 0) return 18;
    if (!isListOp(op) && func_stat == 0) return 20;
    return 0;
}

// Outcome of the list step: `finished` means the operation ends with `code` (kCellOk included);
// otherwise `dir_name` names the save to continue with.
struct ListResult {
    bool finished = false;
    std::uint32_t code = kCellOk;
    std::string dir_name;
};

ListResult runListStep(OpState& s, Op op, std::uint32_t err_dialog, std::uint32_t set_list, std::uint32_t set_buf,
                       std::uint32_t func_list, const stdfs::path& root) {
    std::uint8_t* base = s.base;
    const std::uint32_t dir_list_max = tox2Read32(base, set_buf + kSetBufDirListMax);
    const std::uint32_t buf_size = tox2Read32(base, set_buf + kSetBufBufSize);
    const std::uint32_t buf = tox2Read32(base, set_buf + kSetBufBuf);
    auto fail = [](std::uint32_t code) { return ListResult{true, code, {}}; };
    auto clearBuf = [&] {
        if (buf_size != 0) std::memset(base + buf, 0, buf_size);
    };

    tox2Write32(base, s.list_get + kListGetDirList, buf);

    std::vector<std::string> prefixes;
    {
        const std::string text = readGuestString(base, tox2Read32(base, set_list + kSetListPrefix), kPrefixSize);
        std::size_t pos = 0;
        while (pos <= text.size()) {
            std::size_t bar = text.find('|', pos);
            if (bar == std::string::npos) bar = text.size();
            if (bar > pos) prefixes.push_back(text.substr(pos, bar - pos));
            pos = bar + 1;
        }
    }
    if (prefixes.empty() && op == Op::ListLoad) prefixes = {""};

    std::vector<SaveEntry> entries;
    std::uint32_t dir_num = 0;
    std::uint32_t dir_list_num = 0;
    for (const std::string& name : listSaveDirs(root)) {
        for (const std::string& prefix : prefixes) {
            if (!name.starts_with(prefix)) continue;
            if (dir_num++ == 0) clearBuf();
            if (dir_list_num < dir_list_max) {
                dir_list_num++;
                if (auto entry = loadEntry(root, name)) entries.push_back(std::move(*entry));
            }
            break;
        }
    }
    tox2Write32(base, s.list_get + kListGetDirNum, dir_num);
    tox2Write32(base, s.list_get + kListGetDirListNum, dir_list_num);

    const std::uint32_t sort_type = tox2Read32(base, set_list + kSetListSortType);
    const auto less = [sort_type](const SaveEntry& a, const SaveEntry& b) {
        if (sort_type == kSortModifiedTime) {
            return a.times.mtime == b.times.mtime ? a.subtitle < b.subtitle : a.times.mtime < b.times.mtime;
        }
        return a.subtitle == b.subtitle ? a.times.mtime < b.times.mtime : a.subtitle < b.subtitle;
    };
    if (tox2Read32(base, set_list + kSetListSortOrder) == kOrderAscent) {
        std::sort(entries.begin(), entries.end(), less);
    } else {
        std::sort(entries.rbegin(), entries.rend(), less);
    }
    for (std::size_t i = 0; i < entries.size(); i++) {
        const std::uint32_t item = buf + static_cast<std::uint32_t>(i) * kDirListSize;
        putText(base, item + kDirListDirName, kDirNameSize, entries[i].dir_name);
        putText(base, item + kDirListListParam, kListParamSize, entries[i].list_param);
    }

    tox2Write32(base, s.list_set + kListSetFocusPosition, kFocusListHead);
    const std::int32_t cb = runCallback(s, func_list, s.list_get, s.list_set);
    if (cb != kCbOkNext) {
        logInfo("tox2: savedata: funcList returned result {}", cb);
        if (cb == kCbOkLast || cb == kCbOkLastNoConfirm) return fail(kCellOk);
        return fail(reportCallbackError(s, cb, err_dialog));
    }

    const std::uint32_t fixed_num = tox2Read32(base, s.list_set + kListSetFixedListNum);
    const std::uint32_t fixed_list = tox2Read32(base, s.list_set + kListSetFixedList);
    if (fixed_num > kListItemMax) return fail(paramError(38, std::format("(fixedListNum={})", fixed_num)));
    if (fixed_num != 0 && fixed_list == 0) return fail(paramError(39));
    std::set<std::string> selected_list;
    for (std::uint32_t i = 0; i < fixed_num; i++) {
        const std::string name = getText(base, fixed_list + i * kDirListSize + kDirListDirName, kDirNameSize);
        switch (checkNameString(name, 1, kDirNameSize)) {
            case -1: return fail(paramError(40));
            case -2:
                if (!name.empty()) return fail(paramError(41));
                break;
            default: break;
        }
        selected_list.insert(name);
    }
    std::erase_if(entries, [&](const SaveEntry& e) { return !selected_list.contains(e.dir_name); });

    const std::uint32_t new_data = tox2Read32(base, s.list_set + kListSetNewData);
    std::string new_dir;
    std::uint32_t new_icon_pos = 0;
    if (new_data != 0) {
        new_icon_pos = tox2Read32(base, new_data + kNewDataIconPosition);
        if (new_icon_pos > kIconPosTail) return fail(paramError(43, std::format("(iconPosition={:#x})", new_icon_pos)));
        const std::uint32_t name_addr = tox2Read32(base, new_data + kNewDataDirName);
        if (name_addr == 0) return fail(paramError(44));
        new_dir = getText(base, name_addr, kDirNameSize);
        switch (checkNameString(new_dir, 1, kDirNameSize)) {
            case -1: return fail(paramError(45));
            case -2:
                if (!new_dir.empty()) return fail(paramError(46));
                break;
            default: break;
        }
    }

    const std::uint32_t focus_pos = tox2Read32(base, s.list_set + kListSetFocusPosition);
    const auto count = static_cast<std::int32_t>(entries.size());
    std::int32_t focused = -1;
    switch (focus_pos) {
        case kFocusDirName: {
            const std::uint32_t name_addr = tox2Read32(base, s.list_set + kListSetFocusDirName);
            if (name_addr == 0) return fail(paramError(35));
            const std::string name = getText(base, name_addr, kDirNameSize);
            switch (checkNameString(name, 1, kDirNameSize)) {
                case -1: return fail(paramError(36));
                case -2:
                    if (!name.empty()) return fail(paramError(37));
                    break;
                default: break;
            }
            for (std::int32_t i = 0; i < count; i++) {
                if (entries[static_cast<std::size_t>(i)].dir_name == name) {
                    focused = i;
                    break;
                }
            }
            break;
        }
        case kFocusListHead: focused = count == 0 ? -1 : 0; break;
        case kFocusListTail: focused = count - 1; break;
        case kFocusLatest: {
            std::int64_t best = INT64_MIN;
            for (std::int32_t i = 0; i < count; i++) {
                if (entries[static_cast<std::size_t>(i)].times.mtime > best) {
                    focused = i;
                    best = entries[static_cast<std::size_t>(i)].times.mtime;
                }
            }
            break;
        }
        case kFocusOldest: {
            std::int64_t best = INT64_MAX;
            for (std::int32_t i = 0; i < count; i++) {
                if (entries[static_cast<std::size_t>(i)].times.mtime < best) {
                    focused = i;
                    best = entries[static_cast<std::size_t>(i)].times.mtime;
                }
            }
            break;
        }
        case kFocusNewData:
            if (new_data == 0) return fail(paramError(34, "(newData is null)"));
            focused = new_icon_pos == kIconPosTail ? count : 0;
            break;
        default: return fail(paramError(34, std::format("(focusPosition={:#x})", focus_pos)));
    }

    // Headless choice (see the header).
    std::int32_t selected = -2;
    const bool can_make_new = op == Op::ListSave && new_data != 0;
    if (can_make_new && focus_pos == kFocusNewData) {
        selected = -1;
    } else if (focused >= 0 && focused < count) {
        selected = focused;
    } else if (op == Op::ListLoad && count > 0) {
        selected = 0;
        for (std::int32_t i = 1; i < count; i++) {
            if (entries[static_cast<std::size_t>(i)].times.mtime >
                entries[static_cast<std::size_t>(selected)].times.mtime) {
                selected = i;
            }
        }
    } else if (can_make_new) {
        selected = -1;
    }
    if (selected == -2) {
        logInfo("tox2: savedata: {}: no entry to choose without a save list dialog, cancelling", opName(op));
        return fail(kCancel);
    }

    if (dir_num != 0) clearBuf();  // funcStat gets a clean buffer
    if (selected == -1) {
        logInfo("tox2: savedata: {}: new save '{}' chosen", opName(op), new_dir);
        return {false, kCellOk, new_dir};
    }
    const std::string& chosen = entries[static_cast<std::size_t>(selected)].dir_name;
    logInfo("tox2: savedata: {}: entry '{}' chosen of {}", opName(op), chosen, count);
    return {false, kCellOk, chosen};
}

// In-memory copy of a save's files while the callbacks run.
using FileMap = std::map<std::string, std::vector<std::uint8_t>>;

// Replaces the save directory with the new contents; false (with a log line) on failure.
bool commitSave(const stdfs::path& root, const std::string& escaped, const FileMap& files, const ParamSfo& sfo) {
    const stdfs::path dir = root / escaped;
    const stdfs::path old_dir = root / (".backup_" + escaped);
    const stdfs::path new_dir = root / (".working_" + escaped);
    std::error_code ec;
    stdfs::remove_all(new_dir, ec);
    if (!stdfs::create_directory(new_dir, ec) || ec) {
        logError("tox2: savedata: cannot create '{}' ({})", new_dir.string(), ec.message());
        return false;
    }
    for (const auto& [name, bytes] : files) {
        if (name == "PARAM.SFO") continue;
        if (!writeFile(new_dir / name, bytes)) {
            logError("tox2: savedata: cannot write '{}'", (new_dir / name).string());
            return false;
        }
    }
    if (!writeFile(new_dir / "PARAM.SFO", serializeParamSfo(sfo))) {
        logError("tox2: savedata: cannot write PARAM.SFO in '{}'", new_dir.string());
        return false;
    }
    stdfs::remove_all(old_dir, ec);
    if (stdfs::exists(dir, ec)) {
        stdfs::rename(dir, old_dir, ec);
        if (ec) {
            logError("tox2: savedata: cannot move '{}' ({})", dir.string(), ec.message());
            return false;
        }
    }
    stdfs::rename(new_dir, dir, ec);
    if (ec) {
        logError("tox2: savedata: cannot move '{}' ({})", new_dir.string(), ec.message());
        return false;
    }
    stdfs::remove_all(old_dir, ec);
    return true;
}

// RPCS3 `savedata_op` for the four operations.
std::uint32_t savedataOp(OpState& s, Op op, std::uint32_t err_dialog, std::uint32_t dir_name_addr,
                         std::uint32_t set_list, std::uint32_t set_buf, std::uint32_t func_list,
                         std::uint32_t func_stat, std::uint32_t func_file, std::uint32_t userdata) {
    std::uint8_t* base = s.base;
    const stdfs::path root = [] {
        const VfsResolution res = resolveVfs(kSaveRoot);
        return res.status == VfsStatus::Ok ? res.host : stdfs::path();
    }();
    std::error_code ec;
    if (root.empty() || (stdfs::create_directories(root, ec), ec)) {
        logError("tox2: savedata: {} is not available (is --hdd-dir set?)", kSaveRoot);
        return kErrorAccess;
    }

    tox2Write32(base, s.result + kResultUserdata, userdata);

    SaveEntry save;
    if (isListOp(op)) {
        ListResult list = runListStep(s, op, err_dialog, set_list, set_buf, func_list, root);
        if (list.finished) return list.code;
        save.dir_name = std::move(list.dir_name);
    } else {
        save.dir_name = getText(base, dir_name_addr, kDirNameSize);
    }
    if (!isValidDirName(save.dir_name)) {
        logError("tox2: savedata: invalid save directory name '{}'", save.dir_name);
        return kErrorBroken;
    }
    save.escaped = save.dir_name;
    const stdfs::path dir_path = root / save.escaped;
    const std::string guest_dir = std::string(kSaveRoot) + "/" + save.escaped;

    ParamSfo sfo;
    if (auto parsed = readParamSfoFile(dir_path / "PARAM.SFO")) sfo = std::move(*parsed);
    bool has_modified = false;
    bool recreated = false;

    // Stat step.
    {
        const std::uint32_t buf = tox2Read32(base, set_buf + kSetBufBuf);
        const std::uint32_t buf_size = tox2Read32(base, set_buf + kSetBufBufSize);
        const std::uint32_t file_list_max = tox2Read32(base, set_buf + kSetBufFileListMax);
        if (func_stat == 0) return paramError(20);

        const Times times = statTimes(guest_dir);
        save.is_new = sfo.empty();
        tox2Write32(base, s.stat_get + kStatGetHddFreeSizeKb, static_cast<std::uint32_t>(kHddFreeSizeKb));
        tox2Write32(base, s.stat_get + kStatGetIsNewData, save.is_new ? 1 : 0);
        tox2Write64(base, s.stat_get + kStatGetAtime, static_cast<std::uint64_t>(times.atime));
        tox2Write64(base, s.stat_get + kStatGetMtime, static_cast<std::uint64_t>(times.mtime));
        tox2Write64(base, s.stat_get + kStatGetCtime, static_cast<std::uint64_t>(times.ctime));
        putText(base, s.stat_get + kStatGetDirName, kDirNameSize, save.dir_name);
        if (!sfo.empty()) {
            tox2Write32(base, s.stat_get + kStatGetParentalLevel, sfoInt(sfo, "PARENTAL_LEVEL"));
            tox2Write32(base, s.stat_get + kStatGetAttribute, sfoInt(sfo, "ATTRIBUTE"));
            putText(base, s.stat_get + kStatGetTitle, kTitleSize, sfoString(sfo, "TITLE"));
            putText(base, s.stat_get + kStatGetSubTitle, kSubTitleSize, sfoString(sfo, "SUB_TITLE"));
            putText(base, s.stat_get + kStatGetDetail, kDetailSize, sfoString(sfo, "DETAIL"));
            putText(base, s.stat_get + kStatGetListParam, kListParamSize, sfoString(sfo, "SAVEDATA_LIST_PARAM"));
        }
        tox2Write32(base, s.stat_get + kStatGetFileList, buf);
        if (!save.is_new && buf_size != 0) std::memset(base + buf, 0, buf_size);

        std::vector<std::pair<std::string, std::uint64_t>> files;
        for (stdfs::directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entry_ec;
            if (it->is_directory(entry_ec)) continue;
            std::string name = it->path().filename().string();
            if (checkFilename(name, false, false) != 0) continue;  // system files are not listed
            files.emplace_back(std::move(name), it->file_size(entry_ec));
        }
        ec.clear();
        std::sort(files.begin(), files.end());

        std::uint32_t file_num = 0;
        std::uint32_t file_list_num = 0;
        std::uint64_t size_bytes = 0;
        for (const auto& [name, size] : files) {
            file_num++;
            size_bytes += (size + 1023) / 1024 * 1024;  // the firmware rounds this up
            if (file_list_num >= file_list_max) continue;
            const std::uint32_t item = buf + file_list_num * kFileStatSize;
            file_list_num++;
            const Times file_times = statTimes(guest_dir + "/" + name);
            std::uint32_t type = kFileTypeNormal;
            if (name == "ICON0.PNG")
                type = kFileTypeIcon0;
            else if (name == "ICON1.PAM")
                type = kFileTypeIcon1;
            else if (name == "PIC1.PNG")
                type = kFileTypePic1;
            else if (name == "SND0.AT3")
                type = kFileTypeSnd0;
            else if (sfoInt(sfo, "*" + name) != 0)
                type = kFileTypeSecure;
            tox2Write32(base, item + kFileStatFileType, type);
            tox2Write64(base, item + kFileStatSizeOff, size);
            tox2Write64(base, item + kFileStatAtime, static_cast<std::uint64_t>(file_times.atime));
            tox2Write64(base, item + kFileStatMtime, static_cast<std::uint64_t>(file_times.mtime));
            tox2Write64(base, item + kFileStatCtime, static_cast<std::uint64_t>(file_times.ctime));
            putText(base, item + kFileStatFileName, kFileNameSize, name);
        }
        tox2Write32(base, s.stat_get + kStatGetFileNum, file_num);
        tox2Write32(base, s.stat_get + kStatGetFileListNum, file_list_num);
        tox2Write32(base, s.stat_get + kStatGetSysSizeKb, static_cast<std::uint32_t>(kSysSizeKb));
        tox2Write32(base, s.stat_get + kStatGetSizeKb,
                    save.is_new ? 0u : static_cast<std::uint32_t>(size_bytes / 1024 + kSysSizeKb));

        logInfo("tox2: savedata: {} '{}': new {}, {} file(s), calling funcStat", opName(op), save.dir_name, save.is_new,
                file_num);
        const std::int32_t cb = runCallback(s, func_stat, s.stat_get, s.stat_set);
        if (cb != kCbOkNext) {
            logInfo("tox2: savedata: funcStat returned result {}", cb);
            if (cb == kCbOkLast) return kCellOk;
            return reportCallbackError(s, cb, err_dialog);
        }

        const std::uint32_t set_param = tox2Read32(base, s.stat_set + kStatSetSetParam);
        if (set_param != 0) {
            const std::uint32_t attribute = tox2Read32(base, set_param + kParamAttribute);
            const std::uint32_t parental = tox2Read32(base, set_param + kParamParentalLevel);
            if (attribute > kAttrNoDuplicate) return paramError(57, std::format("(attribute={:#x})", attribute));
            if (parental > 11) return paramError(58, std::format("(parental_level={})", parental));
            for (std::uint32_t i = 0;; i++) {
                const std::uint8_t c = base[set_param + kParamListParam + i];
                if (c == 0 || i >= kListParamSize - 1) {
                    if (c != 0) return paramError(76);
                    break;
                }
                if ((c < 'A' || c > 'Z') && (c < '0' || c > '9') && c != '-' && c != '_') return paramError(77);
            }
            for (std::uint32_t i = 0; i < kParamReservedSize; i++) {
                if (base[set_param + kParamReserved + i] != 0) return paramError(59);
            }
            sfo.strings["ACCOUNT_ID"] = "0000000000000000";
            sfo.ints["ATTRIBUTE"] = attribute;
            sfo.strings["CATEGORY"] = "SD";
            sfo.ints["PARENTAL_LEVEL"] = parental;
            sfo.strings["DETAIL"] = getText(base, set_param + kParamDetail, kDetailSize);
            sfo.strings["SAVEDATA_DIRECTORY"] = save.dir_name;
            sfo.strings["SAVEDATA_LIST_PARAM"] = getText(base, set_param + kParamListParam, kListParamSize);
            sfo.strings["SUB_TITLE"] = getText(base, set_param + kParamSubTitle, kSubTitleSize);
            sfo.strings["TITLE"] = getText(base, set_param + kParamTitle, kTitleSize);
            has_modified = true;
        } else if (save.is_new) {
            return paramError(50);
        }

        switch (tox2Read32(base, s.stat_set + kStatSetReCreateMode) & kRecreateMask) {
            case kRecreateNo:
            case kRecreateNoNoBroken: break;
            case kRecreateYes:
            case kRecreateYesResetOwner:
                if (set_param == 0) return paramError(50);
                logWarn("tox2: savedata: recreating '{}'", save.dir_name);
                std::erase_if(sfo.ints, [](const auto& kv) { return kv.first[0] == '*'; });
                recreated = true;
                has_modified = true;
                break;
            default:
                return paramError(
                        48, std::format("(reCreateMode={:#x})", tox2Read32(base, s.stat_set + kStatSetReCreateMode)));
        }
    }

    if (!sfo.empty() && save.is_new) {
        stdfs::create_directory(dir_path, ec);
        if (ec) {
            logWarn("tox2: savedata: cannot create '{}' ({})", dir_path.string(), ec.message());
            return kErrorAccess;
        }
    }

    // Preload the files so the callbacks work on a snapshot.
    FileMap files;
    if (!recreated) {
        for (stdfs::directory_iterator it(dir_path, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entry_ec;
            if (it->is_directory(entry_ec)) continue;
            std::string name = it->path().filename().string();
            if (checkFilename(name, false, true) != 0) continue;
            if (auto bytes = readFile(it->path())) files.emplace(std::move(name), std::move(*bytes));
        }
        ec.clear();
    }

    tox2Write32(base, s.file_get + kFileGetExcSize, 0);
    std::uint32_t savedata_result = kCellOk;
    std::int32_t last_cb = 0;

    while (func_file != 0) {
        std::memset(base + s.file_set, 0, kFileSetSize);
        std::memset(base + s.file_get + 4, 0, kFileGetSize - 4);
        const std::int32_t cb = runCallback(s, func_file, s.file_get, s.file_set);
        last_cb = cb;
        if (cb != kCbOkNext) {
            if (cb == kCbOkLast || cb == kCbOkLastNoConfirm) break;
            logInfo("tox2: savedata: funcFile returned result {}", cb);
            if (cb < kCbErrInvalid || cb > kCbOkLastNoConfirm) {
                savedata_result = paramError(22);
            } else {
                savedata_result = kErrorCbResult;
            }
            break;
        }

        const std::uint32_t operation = tox2Read32(base, s.file_set + kFileSetOperation);
        const std::uint32_t file_type = tox2Read32(base, s.file_set + kFileSetFileType);
        std::string path;
        switch (file_type) {
            case kFileTypeSecure:
            case kFileTypeNormal: {
                const std::uint32_t name_addr = tox2Read32(base, s.file_set + kFileSetFileName);
                if (name_addr == 0) {
                    savedata_result = paramError(69);
                    break;
                }
                if (std::memchr(base + name_addr, 0, kFileNameSize) == nullptr) {
                    savedata_result = paramError(71);
                    break;
                }
                path = getText(base, name_addr, kFileNameSize);
                if (const int error = checkFilename(path, true, false)) savedata_result = paramError(error);
                break;
            }
            case kFileTypeIcon0: path = "ICON0.PNG"; break;
            case kFileTypeIcon1: path = "ICON1.PAM"; break;
            case kFileTypePic1: path = "PIC1.PNG"; break;
            case kFileTypeSnd0: path = "SND0.AT3"; break;
            default: savedata_result = paramError(61, std::format("(fileType={:#x})", file_type)); break;
        }
        if (savedata_result != kCellOk) break;

        const std::uint32_t file_buf = tox2Read32(base, s.file_set + kFileSetFileBuf);
        const std::uint32_t file_buf_size = tox2Read32(base, s.file_set + kFileSetFileBufSize);
        const std::uint32_t file_size = tox2Read32(base, s.file_set + kFileSetFileSize);
        const std::uint32_t file_offset = tox2Read32(base, s.file_set + kFileSetFileOffset);
        logInfo("tox2: savedata: file op {} '{}' type {} offset {} size {} buffer {}", operation, path, file_type,
                file_offset, file_size, file_buf_size);

        const bool writes = operation == kFileOpWrite || operation == kFileOpWriteNoTrunc;
        if (operation == kFileOpRead || writes) {
            if (file_buf_size < file_size) {
                savedata_result = paramError(72);
                break;
            }
            if ((file_buf == 0 && file_buf_size != 0) ||
                (file_size != 0 && !process().memory.isCommitted(file_buf, file_size))) {
                savedata_result = paramError(73);
                break;
            }
        }

        switch (operation) {
            case kFileOpRead: {
                const auto file = files.find(path);
                if (file == files.end() || file->second.size() <= file_offset) {
                    logError("tox2: savedata: cannot read '{}' (size {}, offset {})", path,
                             file == files.end() ? -1 : static_cast<std::int64_t>(file->second.size()), file_offset);
                    savedata_result = kErrorFailure;
                    break;
                }
                const std::size_t n = std::min<std::size_t>(file_size, file->second.size() - file_offset);
                std::memcpy(base + file_buf, file->second.data() + file_offset, n);
                tox2Write32(base, s.file_get + kFileGetExcSize, static_cast<std::uint32_t>(n));
                break;
            }
            case kFileOpWrite:
            case kFileOpWriteNoTrunc: {
                std::vector<std::uint8_t>& bytes = files[path];
                const std::size_t end = static_cast<std::size_t>(file_offset) + file_size;
                if (bytes.size() < end) bytes.resize(end, 0);
                std::memcpy(bytes.data() + file_offset, base + file_buf, file_size);
                if (operation == kFileOpWrite) bytes.resize(end);
                tox2Write32(base, s.file_get + kFileGetExcSize, file_size);
                has_modified = true;
                break;
            }
            case kFileOpDelete:
                if (files.erase(path) == 0) {
                    logError("tox2: savedata: cannot delete '{}'", path);
                    savedata_result = kErrorFailure;
                    break;
                }
                sfo.ints.erase("*" + path);
                tox2Write32(base, s.file_get + kFileGetExcSize, 0);
                has_modified = true;
                break;
            default: savedata_result = paramError(60, std::format("(fileOperation={:#x})", operation)); break;
        }
        if (savedata_result != kCellOk) break;

        // emplace keeps the flag of a file that already has one, as RPCS3 does.
        if (operation != kFileOpDelete) sfo.ints.emplace("*" + path, file_type == kFileTypeSecure ? 1u : 0u);
    }

    if (!sfo.empty() && has_modified) {
        if (!commitSave(root, save.escaped, files, sfo)) return kErrorAccess;
        logInfo("tox2: savedata: committed '{}' ({} file(s))", save.dir_name, files.size());
    }

    if (savedata_result == kErrorCbResult) return reportCallbackError(s, last_cb, err_dialog);
    return savedata_result;
}

// Shared entry: argument check, busy flag, scratch block, then the operation.
void runOp(Tox2PpuContext* ctx, std::uint8_t* base, Op op, std::uint32_t version, std::uint32_t dir_name,
           std::uint32_t err_dialog, std::uint32_t set_list, std::uint32_t set_buf, std::uint32_t func_list,
           std::uint32_t func_stat, std::uint32_t func_file, std::uint32_t container, std::uint32_t userdata) {
    logInfo("tox2: savedata: cellSaveData{}(version {}, dirName \"{}\", errDialog {}, setBuf {:#x}, "
            "funcStat {:#x}, funcFile {:#x}, container {:#x}, userdata {:#x})",
            opName(op), version, dir_name != 0 && !isListOp(op) ? getText(base, dir_name, kDirNameSize) : "",
            err_dialog, set_buf, func_stat, func_file, container, userdata);

    if (const int error = checkArgs(base, op, version, dir_name, err_dialog, set_list, set_buf, func_list, func_stat)) {
        logError("tox2: savedata: {} parameter error {}", opName(op), error);
        ctx->gpr[3] = kErrorParam;
        return;
    }
    if (g_busy) {
        ctx->gpr[3] = kErrorBusy;
        return;
    }
    if (g_scratch == 0) {
        g_scratch = process().main.allocate(kScratchUsed, 16);
        if (g_scratch == 0) {
            logError("tox2: savedata: cannot allocate the callback structures");
            ctx->gpr[3] = kErrorFailure;
            return;
        }
    }

    g_busy = true;
    std::memset(base + g_scratch, 0, kScratchUsed);
    OpState state;
    state.ctx = ctx;
    state.base = base;
    state.result = g_scratch + kScratchResult;
    state.list_get = g_scratch + kScratchListGet;
    state.list_set = g_scratch + kScratchListSet;
    state.stat_get = g_scratch + kScratchStatGet;
    state.stat_set = g_scratch + kScratchStatSet;
    state.file_get = g_scratch + kScratchFileGet;
    state.file_set = g_scratch + kScratchFileSet;
    const std::uint32_t code =
            savedataOp(state, op, err_dialog, dir_name, set_list, set_buf, func_list, func_stat, func_file, userdata);
    g_busy = false;
    logInfo("tox2: savedata: {} returns {:#x}", opName(op), code);
    ctx->gpr[3] = code;
}

// cellSaveDataAutoLoad2(u32 version, const char* dirName, u32 errDialog, CellSaveDataSetBuf* setBuf,
//                       funcStat, funcFile, u32 container, void* userdata)
void autoLoad2(Tox2PpuContext* ctx, std::uint8_t* base) {
    runOp(ctx, base, Op::AutoLoad, argU32(ctx, 0), argU32(ctx, 1), argU32(ctx, 2), 0, argU32(ctx, 3), 0, argU32(ctx, 4),
          argU32(ctx, 5), argU32(ctx, 6), argU32(ctx, 7));
}

// cellSaveDataAutoSave2: same parameters as AutoLoad2.
void autoSave2(Tox2PpuContext* ctx, std::uint8_t* base) {
    runOp(ctx, base, Op::AutoSave, argU32(ctx, 0), argU32(ctx, 1), argU32(ctx, 2), 0, argU32(ctx, 3), 0, argU32(ctx, 4),
          argU32(ctx, 5), argU32(ctx, 6), argU32(ctx, 7));
}

// cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList, CellSaveDataSetBuf* setBuf,
//                       funcList, funcStat, funcFile, u32 container, void* userdata)
void listLoad2(Tox2PpuContext* ctx, std::uint8_t* base) {
    runOp(ctx, base, Op::ListLoad, argU32(ctx, 0), 0, kErrDialogAlways, argU32(ctx, 1), argU32(ctx, 2), argU32(ctx, 3),
          argU32(ctx, 4), argU32(ctx, 5), argU32(ctx, 6), argU32(ctx, 7));
}

// cellSaveDataListSave2: same parameters as ListLoad2.
void listSave2(Tox2PpuContext* ctx, std::uint8_t* base) {
    runOp(ctx, base, Op::ListSave, argU32(ctx, 0), 0, kErrDialogAlways, argU32(ctx, 1), argU32(ctx, 2), argU32(ctx, 3),
          argU32(ctx, 4), argU32(ctx, 5), argU32(ctx, 6), argU32(ctx, 7));
}

// cellSaveDataDelete2(u32 container): RPCS3 `select_and_delete` is a selection UI only.
void delete2(Tox2PpuContext* ctx, std::uint8_t*) {
    logInfo("tox2: savedata: cellSaveDataDelete2(container {:#x}): no selection UI, cancelling", argU32(ctx, 0));
    ctx->gpr[3] = kCancel;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x1dfbfdd6, "cellSaveDataListLoad2", listLoad2},
        {kModule, 0x2de0d663, "cellSaveDataListSave2", listSave2},
        {kModule, 0x8b7ed64b, "cellSaveDataAutoSave2", autoSave2},
        {kModule, 0xedadd797, "cellSaveDataDelete2", delete2},
        {kModule, 0xfbd5c856, "cellSaveDataAutoLoad2", autoLoad2},
};

}  // namespace

std::span<const HleEntry> cellSaveDataHandlers() { return kHandlers; }

void resetCellSaveData() {
    g_busy = false;
    g_last_error_dialog = 0;
    // The scratch block stays allocated; the process allocator is reset by tests that reserve anew.
    g_scratch = 0;
}

}  // namespace tox2

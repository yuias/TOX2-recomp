// HLE handlers of the sceNpTrophy module (the 10 imports the game uses). Behaviour follows RPCS3
// `Emu/Cell/Modules/sceNpTrophy.cpp` (function of the same name), with `Loader/TRP.cpp` for the
// trophy pack. Handlers run under the guest lock, so the tables need no mutex.
//
// Deviations from RPCS3:
//  - The trophy pack is read for its header, its trophy config (XML, scanned for the id, ttype and
//    pid attributes of each <trophy> tag) and nothing else; the checksum of version 2+ packs is not
//    verified and the icons are not extracted.
//  - The install step creates the trophy directory under /dev_hdd0/home/00000001/trophy with the
//    config and a plain-text unlock state (UNLOCKED.TXT: "<index> <rtc tick>" per line) instead of
//    TROPUSR.DAT.
//  - The status callbacks that RPCS3 delivers through cellSysutilCheckCallback, spread over two
//    seconds while sceNpTrophyRegisterContext blocks its caller until each one has been consumed,
//    run synchronously inside sceNpTrophyRegisterContext. This is deliberate: a caller that pumps
//    the queue itself would only get through RPCS3's wait by timeout, delaying every boot.
//  - The config language is fixed to English (RPCS3's default system language, index 1).
//  - Unlocking shows no notification.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/fatal.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/vfs.h"

namespace dal {
namespace {

constexpr char kModule[] = "sceNpTrophy";

constexpr std::uint32_t kErrorAlreadyInitialized = 0x80022901;
constexpr std::uint32_t kErrorNotInitialized = 0x80022902;
constexpr std::uint32_t kErrorNotSupported = 0x80022903;
constexpr std::uint32_t kErrorContextNotRegistered = 0x80022904;
constexpr std::uint32_t kErrorInvalidArgument = 0x80022906;
constexpr std::uint32_t kErrorExceedsMax = 0x80022907;
constexpr std::uint32_t kErrorUnknownContext = 0x8002290a;
constexpr std::uint32_t kErrorInvalidContext = 0x8002290e;
constexpr std::uint32_t kErrorProcessingAborted = 0x8002290f;
constexpr std::uint32_t kErrorUnknownHandle = 0x80022911;
constexpr std::uint32_t kErrorCannotUnlockPlatinum = 0x80022914;
constexpr std::uint32_t kErrorAlreadyUnlocked = 0x80022915;
constexpr std::uint32_t kErrorInvalidNpCommId = 0x80022918;
constexpr std::uint32_t kErrorConfDoesNotExist = 0x8002291b;
constexpr std::uint32_t kErrorInvalidTrophyId = 0x80022920;
constexpr std::uint32_t kErrorIllegalUpdate = 0x8002292a;

// Context and handle ids are 1..4 (id_base 1, id_count 4 in RPCS3).
constexpr std::uint32_t kIdBase = 1;
constexpr std::uint32_t kIdCount = 4;

constexpr std::uint32_t kStatusNotInstalled = 1;
constexpr std::uint32_t kStatusInstalled = 3;
constexpr std::uint32_t kStatusProcessingSetup = 5;
constexpr std::uint32_t kStatusProcessingProgress = 6;
constexpr std::uint32_t kStatusProcessingFinalize = 7;
constexpr std::uint32_t kStatusProcessingComplete = 8;

constexpr std::uint32_t kGradePlatinum = 1;
constexpr std::uint32_t kInvalidTrophyId = 0xffffffff;
constexpr std::uint64_t kOptionsReadOnly = 1;       // SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY
constexpr std::uint64_t kOptionsShowErrorExit = 1;  // SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT
constexpr std::uint32_t kMaxFlagTrophies = 128;     // SCE_NP_TROPHY_FLAG_SETSIZE

// SceNpCommunicationId / SceNpCommunicationSignature.
constexpr std::uint32_t kCommIdNameMax = 9;
constexpr std::uint32_t kCommIdNum = 10;
constexpr std::uint32_t kCommIdNumMax = 99;
constexpr std::uint32_t kCommSignMagic = 0xB9DDE13B;
constexpr std::uint32_t kCommSignVersion = 0x100;

// TROPHY.TRP: header and entry sizes, magic.
constexpr std::uint32_t kTrpMagic = 0xDCA24D00;
constexpr std::uint64_t kTrpHeaderSize = 64;
constexpr std::uint64_t kTrpEntrySize = 64;
constexpr std::uint32_t kTrpEntryNameSize = 32;
constexpr std::uint32_t kTrpMaxEntries = 0x10000;
constexpr std::uint32_t kLanguage = 1;  // English (US)

constexpr std::string_view kTropDir = "/dev_bdvd/PS3_GAME/TROPDIR/";
constexpr std::string_view kUserTrophyDir = "/dev_hdd0/home/00000001/trophy/";
constexpr std::string_view kConfigFile = "TROPCONF.SFM";
constexpr std::string_view kStateFile = "UNLOCKED.TXT";

// Microseconds from 0001-01-01 (CellRtcTick epoch) to the Unix epoch.
constexpr std::uint64_t kRtcUnixEpochUs = 62135596800ull * 1000000ull;

struct Trophy {
    std::uint32_t grade = 0;
    std::uint32_t pid = kInvalidTrophyId;  // platinum link id
    bool unlocked = false;
    std::uint64_t timestamp = 0;
};

struct TrophyContext {
    bool used = false;
    std::uint32_t serial = 0;  // tells a context apart from a later one that reused its id
    std::string name;
    bool read_only = false;
    bool registered = false;
    std::vector<Trophy> trophies;
};

struct TrophyHandle {
    bool used = false;
    std::uint32_t serial = 0;
};

bool g_initialized = false;
std::uint32_t g_next_serial = 1;
std::array<TrophyContext, kIdCount + 1> g_contexts;  // index = id
std::array<TrophyHandle, kIdCount + 1> g_handles;

void clearTables() {
    g_contexts = {};
    g_handles = {};
}

// RPCS3 sce_np_trophy_manager::get_context_ex. Returns 0 or an error; `*out` is set on success.
std::uint32_t getContext(std::uint32_t context, std::uint32_t handle, bool test_writeable, TrophyContext** out) {
    if (context < kIdBase || context >= kIdBase + kIdCount) return kErrorInvalidArgument;
    TrophyContext& ctxt = g_contexts[context];
    if (!ctxt.used) return kErrorUnknownContext;
    if (test_writeable && ctxt.read_only) return kErrorInvalidContext;
    if (handle < kIdBase || handle >= kIdBase + kIdCount) return kErrorInvalidArgument;
    if (!g_handles[handle].used) return kErrorUnknownHandle;
    *out = &ctxt;
    return 0;
}

std::filesystem::path hostPathOf(const std::string& guest) {
    const VfsResolution resolved = resolveVfs(guest);
    return resolved.status == VfsStatus::Ok ? resolved.host : std::filesystem::path();
}

std::filesystem::path trpHostPath(const std::string& name) {
    return hostPathOf(std::string(kTropDir) + name + "/TROPHY.TRP");
}

std::filesystem::path userDirHostPath(const std::string& name) {
    return hostPathOf(std::string(kUserTrophyDir) + name);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t{be32(p)} << 32) | be32(p + 4); }

struct TrpHeader {
    std::uint64_t file_size = 0;
    std::uint32_t version = 0;
    std::uint32_t files_count = 0;
    std::uint32_t element_size = 0;
};

struct TrpEntry {
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// RPCS3 TRPLoader::LoadHeader, minus the checksum of version 2+ packs.
bool readTrpHeader(std::ifstream& in, TrpHeader& header) {
    std::uint8_t raw[kTrpHeaderSize];
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(raw), sizeof(raw)) || be32(raw) != kTrpMagic) return false;
    header.version = be32(raw + 4);
    header.file_size = be64(raw + 8);
    header.files_count = be32(raw + 16);
    header.element_size = be32(raw + 20);
    return header.version < 2 || header.file_size >= kTrpHeaderSize;
}

bool readTrpEntries(std::ifstream& in, const TrpHeader& header, std::vector<TrpEntry>& entries) {
    if (header.files_count > kTrpMaxEntries) return false;
    in.clear();
    in.seekg(static_cast<std::streamoff>(kTrpHeaderSize));
    entries.clear();
    for (std::uint32_t i = 0; i < header.files_count; i++) {
        std::uint8_t raw[kTrpEntrySize];
        if (!in.read(reinterpret_cast<char*>(raw), sizeof(raw))) return false;
        const char* name = reinterpret_cast<const char*>(raw);
        entries.push_back({std::string(name, strnlen(name, kTrpEntryNameSize)), be64(raw + 32), be64(raw + 40)});
    }
    return true;
}

// RPCS3 TRPLoader::GetRequiredSpace.
std::uint64_t trpRequiredSpace(const TrpHeader& header) {
    return header.file_size - kTrpHeaderSize - std::uint64_t{header.files_count} * header.element_size;
}

// The entry holding the trophy config for `kLanguage`: TROP_xx.SFM, else TROP.SFM, else
// TROPCONF.SFM (RPCS3 renames the chosen one to TROPCONF.SFM on install).
const TrpEntry* findConfigEntry(const std::vector<TrpEntry>& entries) {
    const auto find = [&](std::string_view name) -> const TrpEntry* {
        for (const TrpEntry& entry : entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    if (const TrpEntry* localized = find(std::format("TROP_{:02}.SFM", kLanguage))) return localized;
    if (const TrpEntry* generic = find("TROP.SFM")) return generic;
    return find(kConfigFile);
}

// Value of ` name="..."` inside one tag. The leading space keeps "id" from matching inside "pid".
std::optional<std::string_view> tagAttribute(std::string_view tag, std::string_view name) {
    const std::string key = std::format(" {}=\"", name);
    const std::size_t at = tag.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    const std::size_t start = at + key.size();
    const std::size_t end = tag.find('"', start);
    if (end == std::string_view::npos) return std::nullopt;
    return tag.substr(start, end - start);
}

std::uint32_t parseNumberU32(std::optional<std::string_view> text) {
    int value = 0;
    if (text) std::from_chars(text->data(), text->data() + text->size(), value);
    return static_cast<std::uint32_t>(value);
}

// One Trophy per `<trophy ...>` tag, in file order (RPCS3 TROPUSRLoader::Generate assumes that
// trophy ids are ordered and indexes by position).
std::vector<Trophy> parseTrophyConfig(std::string_view xml) {
    std::vector<Trophy> trophies;
    constexpr std::string_view kTag = "<trophy ";
    for (std::size_t at = xml.find(kTag); at != std::string_view::npos; at = xml.find(kTag, at + 1)) {
        const std::size_t close = xml.find('>', at);
        if (close == std::string_view::npos) break;
        const std::string_view tag = xml.substr(at, close - at);
        Trophy trophy;
        const auto type = tagAttribute(tag, "ttype");
        switch (type && !type->empty() ? (*type)[0] : '\0') {
            case 'P': trophy.grade = 1; break;
            case 'G': trophy.grade = 2; break;
            case 'S': trophy.grade = 3; break;
            case 'B': trophy.grade = 4; break;
            default: trophy.grade = 0; break;
        }
        trophy.pid = parseNumberU32(tagAttribute(tag, "pid"));
        trophies.push_back(trophy);
    }
    return trophies;
}

std::uint64_t currentRtcTick() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return kRtcUnixEpochUs +
           static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void loadUnlockState(const std::filesystem::path& dir, std::vector<Trophy>& trophies) {
    std::ifstream in(dir / kStateFile);
    std::uint32_t index = 0;
    std::uint64_t timestamp = 0;
    while (in >> index >> timestamp) {
        if (index < trophies.size()) {
            trophies[index].unlocked = true;
            trophies[index].timestamp = timestamp;
        }
    }
}

bool saveUnlockState(const std::string& name, const std::vector<Trophy>& trophies) {
    const std::filesystem::path dir = userDirHostPath(name);
    if (dir.empty()) return false;
    std::ostringstream text;
    for (std::size_t i = 0; i < trophies.size(); i++) {
        if (trophies[i].unlocked) text << i << ' ' << trophies[i].timestamp << '\n';
    }
    std::ofstream out(dir / kStateFile, std::ios::binary | std::ios::trunc);
    out << text.str();
    return out.good();
}

// sceNpTrophyInit(void* pool, u32 poolSize, u32 containerId, u64 options)
void trophyInit(DalPpuContext* ctx, std::uint8_t*) {
    if (g_initialized) {
        ctx->gpr[3] = kErrorAlreadyInitialized;
        return;
    }
    if (ctx->gpr[6] > 0) {
        ctx->gpr[3] = kErrorNotSupported;
        return;
    }
    g_initialized = true;
    ctx->gpr[3] = kCellOk;
}

// sceNpTrophyTerm()
void trophyTerm(DalPpuContext* ctx, std::uint8_t*) {
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    clearTables();
    g_initialized = false;
    ctx->gpr[3] = kCellOk;
}

// sceNpTrophyCreateHandle(u32* handle)
void trophyCreateHandle(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t handle_addr = argU32(ctx, 0);
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    if (handle_addr == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    for (std::uint32_t id = kIdBase; id < kIdBase + kIdCount; id++) {
        if (g_handles[id].used) continue;
        g_handles[id] = {true, g_next_serial++};
        dalWrite32(base, handle_addr, id);
        ctx->gpr[3] = kCellOk;
        return;
    }
    ctx->gpr[3] = kErrorExceedsMax;
}

// sceNpTrophyDestroyHandle(u32 handle)
void trophyDestroyHandle(DalPpuContext* ctx, std::uint8_t*) {
    const std::uint32_t handle = argU32(ctx, 0);
    if (handle < kIdBase || handle >= kIdBase + kIdCount) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (!g_handles[handle].used) {
        ctx->gpr[3] = kErrorUnknownHandle;
        return;
    }
    g_handles[handle] = {};
    ctx->gpr[3] = kCellOk;
}

// sceNpTrophyCreateContext(u32* context, const SceNpCommunicationId* commId,
//                          const SceNpCommunicationSignature* commSign, u64 options)
void trophyCreateContext(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context_addr = argU32(ctx, 0);
    const std::uint32_t comm_id = argU32(ctx, 1);
    const std::uint32_t comm_sign = argU32(ctx, 2);
    const std::uint64_t options = ctx->gpr[6];
    if (comm_sign == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    if (context_addr == 0 || comm_id == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (options > kOptionsReadOnly) {
        ctx->gpr[3] = kErrorNotSupported;
        return;
    }
    const std::uint32_t num = dalRead8(base, comm_id + kCommIdNum);
    if (num > kCommIdNumMax) {
        ctx->gpr[3] = kErrorInvalidNpCommId;
        return;
    }

    // The signature must carry the magic and version 1.00, and 6 zero padding bytes after them.
    bool padding_zero = true;
    for (std::uint32_t i = 6; i < 12; i++) padding_zero &= dalRead8(base, comm_sign + i) == 0;
    if (dalRead32(base, comm_sign) != kCommSignMagic || !padding_zero ||
        dalRead16(base, comm_sign + 4) != kCommSignVersion) {
        ctx->gpr[3] = kErrorInvalidNpCommId;
        return;
    }

    const std::string name = std::format("{}_{:02}", readGuestString(base, comm_id, kCommIdNameMax), num);
    for (std::uint32_t id = kIdBase; id < kIdBase + kIdCount; id++) {
        if (g_contexts[id].used) continue;
        TrophyContext& created = g_contexts[id];
        created = {};
        created.used = true;
        created.serial = g_next_serial++;
        created.name = name;
        created.read_only = (options & kOptionsReadOnly) != 0;
        dalWrite32(base, context_addr, id);
        logInfo("dal: trophy: context {} for \"{}\"", id, name);
        ctx->gpr[3] = kCellOk;
        return;
    }
    ctx->gpr[3] = kErrorExceedsMax;
}

// sceNpTrophyDestroyContext(u32 context)
void trophyDestroyContext(DalPpuContext* ctx, std::uint8_t*) {
    const std::uint32_t context = argU32(ctx, 0);
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    if (context < kIdBase || context >= kIdBase + kIdCount) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (!g_contexts[context].used) {
        ctx->gpr[3] = kErrorUnknownContext;
        return;
    }
    g_contexts[context] = {};
    ctx->gpr[3] = kCellOk;
}

// sceNpTrophyGetRequiredDiskSpace(u32 context, u32 handle, u64* reqspace, u64 options)
void trophyGetRequiredDiskSpace(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    const std::uint32_t handle = argU32(ctx, 1);
    const std::uint32_t reqspace_addr = argU32(ctx, 2);
    if (reqspace_addr == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (ctx->gpr[6] > 0) {
        ctx->gpr[3] = kErrorNotSupported;
        return;
    }
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    TrophyContext* ctxt = nullptr;
    if (const std::uint32_t error = getContext(context, handle, false, &ctxt)) {
        ctx->gpr[3] = error;
        return;
    }

    std::uint64_t space = 0;
    const std::filesystem::path user_dir = userDirHostPath(ctxt->name);
    std::error_code ec;
    if (user_dir.empty() || !std::filesystem::is_directory(user_dir, ec)) {
        const std::filesystem::path trp_path = trpHostPath(ctxt->name);
        std::ifstream in(trp_path, std::ios::binary);
        if (trp_path.empty() || !in) {
            logError("dal: trophy: cannot open the trophy pack of \"{}\"", ctxt->name);
            ctx->gpr[3] = kErrorConfDoesNotExist;
            return;
        }
        TrpHeader header;
        if (readTrpHeader(in, header)) {
            space = trpRequiredSpace(header);
        } else {
            logError("dal: trophy: invalid trophy pack header (trp_name={})", ctxt->name);
        }
    }
    logInfo("dal: trophy: sceNpTrophyGetRequiredDiskSpace -> {:#x}", space);
    dalWrite64(base, reqspace_addr, space);
    ctx->gpr[3] = kCellOk;
}

// The "install" and state load of sceNpTrophyRegisterContext; returns an error code or 0.
std::uint32_t installTrophies(TrophyContext& ctxt) {
    const std::filesystem::path trp_path = trpHostPath(ctxt.name);
    std::ifstream in(trp_path, std::ios::binary);
    TrpHeader header;
    std::vector<TrpEntry> entries;
    if (trp_path.empty() || !in || !readTrpHeader(in, header) || !readTrpEntries(in, header, entries)) {
        logError("dal: trophy: failed to load the trophy config header of \"{}\"", ctxt.name);
        return kErrorIllegalUpdate;
    }
    const TrpEntry* config = findConfigEntry(entries);
    if (config == nullptr || config->size > (std::uint64_t{1} << 24)) {
        logError("dal: trophy: invalid or incomplete trophy config in \"{}\"", ctxt.name);
        return kErrorIllegalUpdate;
    }
    std::string xml(static_cast<std::size_t>(config->size), '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(config->offset));
    if (!in.read(xml.data(), static_cast<std::streamsize>(xml.size()))) {
        logError("dal: trophy: failed to read the trophy config of \"{}\"", ctxt.name);
        return kErrorIllegalUpdate;
    }

    const std::filesystem::path dir = userDirHostPath(ctxt.name);
    std::error_code ec;
    if (dir.empty() || (std::filesystem::create_directories(dir, ec), ec)) {
        logError("dal: trophy: failed to create the trophy directory of \"{}\" ({})", ctxt.name,
                 ec ? ec.message() : std::string("no hdd directory configured"));
        return kErrorIllegalUpdate;
    }
    {
        std::ofstream out(dir / kConfigFile, std::ios::binary | std::ios::trunc);
        out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
        if (!out.good()) {
            logError("dal: trophy: failed to write the trophy config of \"{}\"", ctxt.name);
            return kErrorIllegalUpdate;
        }
    }
    ctxt.trophies = parseTrophyConfig(xml);
    loadUnlockState(dir, ctxt.trophies);
    if (!saveUnlockState(ctxt.name, ctxt.trophies)) {
        logError("dal: trophy: failed to write the unlock state of \"{}\"", ctxt.name);
        return kErrorIllegalUpdate;
    }
    ctxt.registered = true;
    logInfo("dal: trophy: registered \"{}\": {} trophies", ctxt.name, ctxt.trophies.size());
    return 0;
}

// sceNpTrophyRegisterContext(u32 context, u32 handle, SceNpTrophyStatusCallback statusCb,
//                            void* arg, u64 options)
// The callback is s32(u32 context, u32 status, s32 completed, s32 total, void* arg).
void trophyRegisterContext(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    const std::uint32_t handle = argU32(ctx, 1);
    const std::uint32_t status_cb = argU32(ctx, 2);
    const std::uint32_t arg = argU32(ctx, 3);
    const std::uint64_t options = ctx->gpr[7];
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    TrophyContext* ctxt = nullptr;
    if (const std::uint32_t error = getContext(context, handle, true, &ctxt)) {
        ctx->gpr[3] = error;
        return;
    }
    if (status_cb == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (options > kOptionsShowErrorExit) {
        ctx->gpr[3] = kErrorNotSupported;
        return;
    }

    const auto on_error = [&] {
        if ((options & kOptionsShowErrorExit) != 0) {
            stopProcess(ExitCode::Fatal,
                        "dal: stop: sceNpTrophyRegisterContext failed: Error during trophy registration! "
                        "The game will now be terminated.");
        }
    };

    const std::filesystem::path trp_path = trpHostPath(ctxt->name);
    std::error_code ec;
    if (trp_path.empty() || !std::filesystem::exists(trp_path, ec)) {
        logError("dal: trophy: failed to open the trophy pack of \"{}\"", ctxt->name);
        ctx->gpr[3] = kErrorConfDoesNotExist;
        return;
    }
    const std::filesystem::path user_dir = userDirHostPath(ctxt->name);
    const std::uint32_t trp_status =
            !user_dir.empty() && std::filesystem::is_directory(user_dir, ec) ? kStatusInstalled : kStatusNotInstalled;

    // The first call asks whether to install; a negative result aborts the registration.
    const std::uint32_t context_serial = ctxt->serial;
    const std::uint32_t handle_serial = g_handles[handle].serial;
    logInfo("dal: trophy: register callback status {}", trp_status);
    const auto first =
            static_cast<std::int32_t>(callGuestFunction(ctx, base, status_cb, {context, trp_status, 0, 0, arg}));
    if (first < 0) {
        on_error();
        ctx->gpr[3] = kErrorProcessingAborted;
        return;
    }

    // The callback may have called back into this module; recheck what it could have changed.
    if (!g_initialized) {
        on_error();
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    TrophyContext* again = nullptr;
    if (const std::uint32_t error = getContext(context, handle, false, &again)) {
        ctx->gpr[3] = error;
        return;
    }
    if (again->serial != context_serial) {
        on_error();
        ctx->gpr[3] = kErrorUnknownContext;
        return;
    }
    if (g_handles[handle].serial != handle_serial) {
        on_error();
        ctx->gpr[3] = kErrorUnknownHandle;
        return;
    }

    if (const std::uint32_t error = installTrophies(*again)) {
        on_error();
        ctx->gpr[3] = error;
        return;
    }

    // The events the console's system software sends while it installs the data.
    const auto progress_count = static_cast<std::int32_t>(again->trophies.size()) - 1;
    struct Step {
        std::uint32_t status;
        std::int32_t total;
    };
    const Step steps[] = {
            {kStatusProcessingSetup, 3},
            {kStatusProcessingProgress, progress_count},
            {kStatusProcessingFinalize, std::max<std::int32_t>(progress_count, 9) - 5},
            {kStatusProcessingComplete, 0},
    };
    for (const Step& step : steps) {
        for (std::int32_t completed = 0; completed <= step.total; completed++) {
            callGuestFunction(ctx, base, status_cb,
                              {context, step.status, static_cast<std::uint32_t>(completed),
                               static_cast<std::uint32_t>(step.total), arg});
        }
    }
    ctx->gpr[3] = kCellOk;
}

// The platinum trophy that this unlock completes, or kInvalidTrophyId (RPCS3
// TROPUSRLoader::GetUnlockedPlatinumID).
std::uint32_t unlockedPlatinumId(const std::vector<Trophy>& trophies, std::uint32_t trophy_id) {
    const std::uint32_t pid = trophies[trophy_id].pid;
    if (pid == kInvalidTrophyId || (pid < trophies.size() && trophies[pid].unlocked)) {
        return kInvalidTrophyId;
    }
    for (const Trophy& trophy : trophies) {
        if (trophy.pid == pid && !trophy.unlocked) return kInvalidTrophyId;
    }
    return pid;
}

// sceNpTrophyUnlockTrophy(u32 context, u32 handle, s32 trophyId, u32* platinumId)
void trophyUnlockTrophy(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    const std::uint32_t handle = argU32(ctx, 1);
    const auto trophy_id = static_cast<std::int32_t>(argU32(ctx, 2));
    const std::uint32_t platinum_addr = argU32(ctx, 3);
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    TrophyContext* ctxt = nullptr;
    if (const std::uint32_t error = getContext(context, handle, true, &ctxt)) {
        ctx->gpr[3] = error;
        return;
    }
    if (!ctxt->registered) {
        ctx->gpr[3] = kErrorContextNotRegistered;
        return;
    }
    std::vector<Trophy>& trophies = ctxt->trophies;
    if (trophy_id < 0 || trophy_id >= static_cast<std::int32_t>(trophies.size())) {
        ctx->gpr[3] = kErrorInvalidTrophyId;
        return;
    }
    const auto index = static_cast<std::uint32_t>(trophy_id);
    if (trophies[index].grade == kGradePlatinum) {
        ctx->gpr[3] = kErrorCannotUnlockPlatinum;
        return;
    }
    if (trophies[index].unlocked) {
        ctx->gpr[3] = kErrorAlreadyUnlocked;
        return;
    }

    const std::uint64_t tick = currentRtcTick();
    trophies[index].unlocked = true;
    trophies[index].timestamp = tick;
    logInfo("dal: trophy: trophy {} unlocked", trophy_id);

    const std::uint32_t platinum = unlockedPlatinumId(trophies, index);
    if (platinum != kInvalidTrophyId && platinum < trophies.size()) {
        trophies[platinum].unlocked = true;
        trophies[platinum].timestamp = tick;
        logInfo("dal: trophy: platinum trophy {} unlocked", platinum);
    }
    if (platinum_addr != 0) dalWrite32(base, platinum_addr, platinum);

    if (!saveUnlockState(ctxt->name, trophies)) {
        logError("dal: trophy: failed to save the unlock state of \"{}\"", ctxt->name);
    }
    ctx->gpr[3] = kCellOk;
}

// sceNpTrophyGetTrophyUnlockState(u32 context, u32 handle, SceNpTrophyFlagArray* flags, u32* count)
void trophyGetTrophyUnlockState(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t context = argU32(ctx, 0);
    const std::uint32_t handle = argU32(ctx, 1);
    const std::uint32_t flags_addr = argU32(ctx, 2);
    const std::uint32_t count_addr = argU32(ctx, 3);
    if (flags_addr == 0 || count_addr == 0) {
        ctx->gpr[3] = kErrorInvalidArgument;
        return;
    }
    if (!g_initialized) {
        ctx->gpr[3] = kErrorNotInitialized;
        return;
    }
    TrophyContext* ctxt = nullptr;
    if (const std::uint32_t error = getContext(context, handle, false, &ctxt)) {
        ctx->gpr[3] = error;
        return;
    }

    // A read-only context has no registration of its own; it reads the installed state, if any.
    std::vector<Trophy> loaded;
    const std::vector<Trophy>* trophies = &ctxt->trophies;
    if (ctxt->read_only) {
        const std::filesystem::path dir = userDirHostPath(ctxt->name);
        std::ifstream config(dir.empty() ? std::filesystem::path() : dir / kConfigFile, std::ios::binary);
        if (config) {
            const std::string xml((std::istreambuf_iterator<char>(config)), std::istreambuf_iterator<char>());
            loaded = parseTrophyConfig(xml);
            loadUnlockState(dir, loaded);
            trophies = &loaded;
        } else {
            dalWrite32(base, count_addr, 0);
            for (std::uint32_t word = 0; word < kMaxFlagTrophies / 32; word++) {
                dalWrite32(base, flags_addr + 4 * word, 0);
            }
            ctx->gpr[3] = kCellOk;
            return;
        }
    } else if (!ctxt->registered) {
        ctx->gpr[3] = kErrorContextNotRegistered;
        return;
    }

    const auto count = static_cast<std::uint32_t>(trophies->size());
    dalWrite32(base, count_addr, count);
    std::uint32_t words[kMaxFlagTrophies / 32] = {};
    for (std::uint32_t id = 0; id < std::min(count, kMaxFlagTrophies); id++) {
        if ((*trophies)[id].unlocked) words[id / 32] |= 1u << (id % 32);
    }
    for (std::uint32_t word = 0; word < kMaxFlagTrophies / 32; word++) {
        dalWrite32(base, flags_addr + 4 * word, words[word]);
    }
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x1197b52c, "sceNpTrophyRegisterContext", trophyRegisterContext},
        {kModule, 0x1c25470d, "sceNpTrophyCreateHandle", trophyCreateHandle},
        {kModule, 0x370136fe, "sceNpTrophyGetRequiredDiskSpace", trophyGetRequiredDiskSpace},
        {kModule, 0x3741ecc7, "sceNpTrophyDestroyContext", trophyDestroyContext},
        {kModule, 0x39567781, "sceNpTrophyInit", trophyInit},
        {kModule, 0x623cd2dc, "sceNpTrophyDestroyHandle", trophyDestroyHandle},
        {kModule, 0x8ceedd21, "sceNpTrophyUnlockTrophy", trophyUnlockTrophy},
        {kModule, 0xa7fabf4d, "sceNpTrophyTerm", trophyTerm},
        {kModule, 0xb3ac3478, "sceNpTrophyGetTrophyUnlockState", trophyGetTrophyUnlockState},
        {kModule, 0xe3bf9a28, "sceNpTrophyCreateContext", trophyCreateContext},
};

}  // namespace

std::span<const HleEntry> sceNpTrophyHandlers() { return kHandlers; }

void resetSceNpTrophy() {
    g_initialized = false;
    g_next_serial = 1;
    clearTables();
}

}  // namespace dal

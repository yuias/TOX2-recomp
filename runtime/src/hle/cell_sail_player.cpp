// HLE handlers of the cellSail player: MemAllocatorInitialize, PlayerInitialize2, Finalize, the
// adapter and repeat-mode setters, Boot, OpenStream, Start, the descriptor calls (Create, Add,
// Destroy) and the descriptor's database and capability calls. Behaviour follows RPCS3
// `Emu/Cell/Modules/cellSail.cpp` (the function of the same name) for argument handling, error codes
// and the event pairing: Initialize2 reports PLAYER_STATE_CHANGED(INITIALIZED), Boot reports
// PLAYER_STATE_CHANGED(BOOT_TRANSITION) and then PLAYER_CALL_COMPLETED(BOOT), and CreateDescriptor
// accepts `x-cell-fs://` URIs and reads the PAMF header. RPCS3 implements nothing of OpenStream and
// Start, so their events come from the game's own notify callback, which stores the state and clears
// its busy byte and ignores the other majors: OpenStream reports OPEN_TRANSITION and OPENED and then
// CALL_COMPLETED(OPEN_STREAM), Start reports START_TRANSITION and RUNNING and then
// CALL_COMPLETED(START), and the end of the stream reports STOP_TRANSITION and OPENED. The game
// never stops the stream: it leaves RUNNING only because the state changes, so the end is detected by
// the wall clock when the game polls for frames.
//
// A stream plays on the host movie player (media/movie_player.h): Start launches its decode thread
// and frames are copied into the game's buffers when the game polls the graphics adapter. The
// clock is the host's steady clock, restarted when the first frame and the first PCM are decoded, so
// the decoder's start-up does not make the first frames late. The buffers come from the graphics
// adapter's pAlloc callback at OpenStream and go back through pFree at Finalize.
//
// Deviations from RPCS3:
// - Events are queued to the `_libsail-control` service thread instead of calling the callback inside
//   the HLE call, as the library's own thread would do. PlayerFinalize waits for the queue to drain
//   (bounded), so the game can free its callback argument once Finalize returns.
// - Initialize2 on an initialised player returns INVALID_STATE (RPCS3 overwrites the player).
// - Boot reports CLOSED after BOOT_TRANSITION: that is the state a booted player is in.
// - A file that cannot be read still gets a descriptor (RPCS3 does the same); CreateDatabase reports
//   the failure. The game ignores CreateDescriptor's result and needs the pointer for every later
//   call, and its Open/Start waits are only released by events, so OpenStream completes for such a
//   descriptor too (and Start refuses it).
// - SetRepeatMode returns CELL_OK on success (RPCS3 returns the mode). The mode is stored and not
//   acted on: the stream ends at its end, and the game restarts a looping movie by itself.
//
// Object state lives host-side, keyed by the address of the guest object; the game allocates the
// player and the adapters and the HLE never writes them. Only the descriptor block and a small
// scratch block (format copies, the frame pointer array and the sound frame buffer, which the
// callbacks need as guest memory; the decoded PCM is copied into the latter) are allocated here, through the game's own
// allocator callbacks. Everything runs under the guest lock. The descriptor's allocator callbacks and the scratch
// block's allocation run on the calling guest thread, which is how the game expects them to be called. The service
// thread runs the notify callback, both adapters' format-changed callbacks, the graphics adapter's frame allocation and
// release, and the release of the scratch block.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/census.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "tox2/ppu_context.h"
#include "hle/cell_sail_internal.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "hle/sail_service.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"
#include "media/movie_decoder.h"
#include "media/movie_player.h"
#include "media/pamf.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellSail";
constexpr char kUriScheme[] = "x-cell-fs://";
constexpr std::uint32_t kUriMaxLength = 1024;
// Same read the host movie player does: the sequence information and stream table are in the first
// 2048 bytes, and the EP table behind them is not needed for linear playback.
constexpr std::size_t kPamfHeaderReadSize = 2048;

// What the game's allocator callback receives for a descriptor. Its heap (the callback forwards to
// it with the boundary as the alignment) clamps the alignment to at least 16 bytes and rounds it up
// to a power of two, and takes no other boundary rule, so 16 is enough for a block nothing reads.
constexpr std::uint32_t kDescriptorBoundary = 0x10;

// Bound on PlayerFinalize's wait for the service thread, which may be inside an allocator callback.
constexpr auto kFinalizeTimeout = std::chrono::seconds(2);

// Upper bound on Start's wait for the decoders' first output (the decode thread's start-up). Past it
// the clock starts anyway and the first frames are late.
constexpr auto kPrerollTimeout = std::chrono::seconds(1);

// The game copies one frame out of its buffer within the poll that received it, so the buffer
// returned last is the only one it can still be reading; two more rotate behind it.
constexpr std::uint32_t kFrameBuffers = 3;

// Largest sound frame request: RPCS3's `cellSailSoundAdapterGetFrame` rejects more than 2048 samples.
constexpr std::uint32_t kMaxSoundFrames = 2048;
constexpr std::uint32_t kMaxAudioChannels = 8;

// Layout of the scratch block allocated at OpenStream. The callbacks and the sound info need guest
// memory: copies of the formats for pFormatChanged, the pointer array for pAlloc and pFree, and the
// sound frame buffer (valid until the next SoundAdapterGetFrame, as the game expects).
constexpr std::uint32_t kScratchVideoFormat = 0;
constexpr std::uint32_t kScratchSoundFormat = 32;
constexpr std::uint32_t kScratchFramePointers = 64;  // room for 8 pointers
constexpr std::uint32_t kScratchPcm = 96;
constexpr std::uint32_t kScratchBoundary = 0x10;
static_assert(kScratchSoundFormat >= kScratchVideoFormat + sail::kVideoFormatSize);
static_assert(kScratchFramePointers >= kScratchSoundFormat + sail::kAudioFormatSize);
static_assert(kScratchPcm >= kScratchFramePointers + 4 * kFrameBuffers);

constexpr std::uint32_t kDefaultWidth = 1280, kDefaultHeight = 720;
constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr double kTicksPerSecond = 90000.0;

// One opened stream: the host player and everything allocated in guest memory for it. Created by
// OpenStream, released by Finalize. It is shared with the service-thread calls that touch it, so a
// call that runs after Finalize still finds valid host state; guest addresses in it are only used
// while the call holds them (see the checks of isCurrent).
struct Stream {
    MoviePlayer movie;
    std::string guest_path;
    std::uint32_t session_id = 0;

    bool open_ok = false;  // the host player opened the file
    std::string open_error;
    bool started = false;  // Start launched the decode thread
    bool running = false;  // the clock runs and frames are handed out
    bool eos = false;      // the end was reported
    bool summarized = false;

    VideoOutputFormat video_format;
    AudioOutputFormat audio_format;
    std::uint32_t width = kDefaultWidth, height = kDefaultHeight;
    std::uint32_t frame_bytes = 0;
    std::uint32_t audio_frame_bytes = 0;

    std::uint32_t scratch = 0;
    std::uint32_t pcm_frames = 0;       // capacity of the sound buffer, 0 without a sound adapter
    std::vector<std::uint32_t> frames;  // what pAlloc returned
    bool frames_usable = false;         // pAlloc succeeded and every buffer is mapped
    std::uint32_t next_frame = 0;
    std::uint32_t video_tag = 0, sound_tag = 0;

    std::uint64_t get_frame_calls = 0, frames_rejected = 0;
    // Time of the frame copy into guest memory, summed and longest (the copy runs under the guest lock).
    std::uint64_t frame_copy_ns = 0, frame_copy_max_ns = 0;
    // Wall-clock window of the playback and the flips that happened in it.
    std::uint64_t start_us = 0, end_us = 0, start_flips = 0, end_flips = 0;
    std::uint64_t start_skipped = 0, end_skipped = 0;
    bool ended = false;
};

struct Player {
    std::uint64_t serial = 0;  // tells a queued event which incarnation of the address it was for
    std::uint32_t alloc_opd = 0, free_opd = 0, alloc_arg = 0;
    std::uint32_t notify_opd = 0, notify_arg = 0;
    std::uint32_t max_audio_streams = 0, max_video_streams = 0;
    sail::PlayerState state = sail::PlayerState::Initialized;
    std::int32_t repeat_mode = 0;
    std::array<std::uint8_t, sail::kStartCommandSize> repeat_command{};
    std::shared_ptr<Stream> stream;
};

struct Descriptor {
    std::uint32_t player = 0;  // 0 once the player is finalized
    bool registered = false;   // added to a live player
    std::uint32_t free_opd = 0, alloc_arg = 0;
    std::string guest_path;
    std::filesystem::path host_path;
    std::optional<PamfInfo> info;
    std::uint32_t error_code = kCellOk;  // why `info` is empty
};

std::map<std::uint32_t, Player> g_players;
std::map<std::uint32_t, Descriptor> g_descriptors;
std::uint64_t g_next_serial = 1;
std::uint32_t g_next_session = 1;

// Finished movies, for the census. The census reads them from another thread.
struct SummaryState {
    std::mutex mutex;
    std::vector<std::string> lines;
};

SummaryState& summaryState() {
    // Leaked: the census can print from a stop path that ends in _Exit.
    static SummaryState* state = new SummaryState;
    return *state;
}

using sail_hle::guestMapped;

struct Event {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint64_t arg0;
    std::uint64_t arg1;
};

bool isCurrent(std::uint32_t address, std::uint64_t serial) {
    const auto it = g_players.find(address);
    return it != g_players.end() && it->second.serial == serial;
}

// Where a player's events go: its notify callback, valid while the player incarnation is current.
struct Notifier {
    std::uint32_t address = 0;
    std::uint64_t serial = 0;
    std::uint32_t opd = 0, arg = 0;
};

Notifier notifierOf(std::uint32_t address, const Player& player) {
    return {address, player.serial, player.notify_opd, player.notify_arg};
}

// Calls the notify callback for each event, in order. Events for a player that was finalized
// meanwhile (also during an earlier callback) are dropped, so nothing reaches the game's callback
// argument after Finalize.
void deliverEvents(Tox2PpuContext* ctx, std::uint8_t* base, const Notifier& to, const std::vector<Event>& events) {
    for (const Event& event : events) {
        if (!isCurrent(to.address, to.serial)) return;
        callGuestFunction(ctx, base, to.opd,
                          {to.arg, sail::makeEvent(event.major, event.minor), event.arg0, event.arg1});
    }
}

// Delivers `events` on the service thread.
void queueEvents(std::uint32_t address, const Player& player, std::vector<Event> events) {
    queueSailCall([to = notifierOf(address, player), events = std::move(events)](
                          Tox2PpuContext* ctx, std::uint8_t* base) { deliverEvents(ctx, base, to, events); });
}

constexpr Event stateChanged(sail::PlayerState state) {
    return {sail::kEventPlayerStateChanged, 0, static_cast<std::uint64_t>(state), 0};
}

constexpr Event callCompleted(std::uint32_t call) { return {sail::kEventPlayerCallCompleted, call, 0, 0}; }

constexpr Event plainEvent(std::uint32_t major) { return {major, 0, 0, 0}; }

// Waits until every call queued before this one has run. False on timeout. Returns at once when the
// service thread cannot run at all, since nothing is pending then.
bool drainServiceQueue() {
    struct Marker {
        std::condition_variable_any done_cv;
        bool done = false;
    };
    auto marker = std::make_shared<Marker>();
    const bool queued = queueSailCall([marker](Tox2PpuContext*, std::uint8_t*) {
        marker->done = true;
        marker->done_cv.notify_all();
    });
    if (!queued) return true;
    return blockingWait(marker->done_cv, [&] { return marker->done; }, "sail finalize", kFinalizeTimeout);
}

Player* findPlayer(std::uint32_t address) {
    const auto it = g_players.find(address);
    return it == g_players.end() ? nullptr : &it->second;
}

Descriptor* findDescriptor(std::uint32_t address) {
    const auto it = g_descriptors.find(address);
    return it == g_descriptors.end() ? nullptr : &it->second;
}

// cellSailMemAllocatorInitialize(CellSailMemAllocator* self, CellSailMemAllocatorFuncs* callbacks,
//                                void* arg)
// The third argument is not in RPCS3's two-argument signature; the game passes one and the player
// passes it back to every allocator callback.
void sailMemAllocatorInitialize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    const std::uint32_t callbacks = argU32(ctx, 1);
    if (!guestMapped(self, sail::kAllocatorSize) || callbacks == 0) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    tox2Write32(base, self + sail::kAllocatorCallbacks, callbacks);
    tox2Write32(base, self + sail::kAllocatorArg, argU32(ctx, 2));
    setResultCode(ctx, kCellOk);
}

// cellSailPlayerInitialize2(CellSailPlayer* self, CellSailMemAllocator* allocator,
//                           CellSailPlayerFuncNotified callback, void* callback_arg,
//                           CellSailPlayerAttribute* attribute, CellSailPlayerResource* resource)
void playerInitialize2(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    const std::uint32_t allocator = argU32(ctx, 1);
    const std::uint32_t callback = argU32(ctx, 2);
    const std::uint32_t attribute = argU32(ctx, 4);
    if (self == 0 || callback == 0 || !guestMapped(allocator, sail::kAllocatorSize) ||
        !guestMapped(attribute, sail::kPlayerAttributeSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    const std::uint32_t funcs = tox2Read32(base, allocator + sail::kAllocatorCallbacks);
    if (!guestMapped(funcs, sail::kAllocatorFuncsSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (g_players.contains(self)) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }

    Player player;
    player.serial = g_next_serial++;
    player.alloc_opd = tox2Read32(base, funcs + sail::kAllocatorFuncsAlloc);
    player.free_opd = tox2Read32(base, funcs + sail::kAllocatorFuncsFree);
    player.alloc_arg = tox2Read32(base, allocator + sail::kAllocatorArg);
    player.notify_opd = callback;
    player.notify_arg = argU32(ctx, 3);
    player.max_audio_streams = tox2Read32(base, attribute + sail::kPlayerAttributeMaxAudioStreams);
    player.max_video_streams = tox2Read32(base, attribute + sail::kPlayerAttributeMaxVideoStreams);
    const auto it = g_players.emplace(self, player).first;
    logDebug("tox2: sail: player {:#010x} initialised (notify {:#010x}, arg {:#010x})", self, callback,
             player.notify_arg);

    queueEvents(self, it->second, {stateChanged(sail::PlayerState::Initialized)});
    setResultCode(ctx, kCellOk);
}

void retireStream(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t self, Player& player);

// cellSailPlayerFinalize(CellSailPlayer* self)
// The game calls it from whatever state the movie ended in and frees its callback argument right
// after, so queued events are delivered first and none follows. The decode thread stops and the
// frame buffers are handed back before the adapters are released.
void playerFinalize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    Player* player = findPlayer(self);
    if (player == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint64_t serial = player->serial;
    retireStream(ctx, base, self, *player);
    if (!drainServiceQueue()) {
        logWarn("tox2: sail: the service thread did not drain within {} s while finalizing player {:#010x}",
                kFinalizeTimeout.count(), self);
    }
    // The drain releases the guest lock; another thread may have finalized this player and reused the
    // address meanwhile, and its state is not ours to release.
    if (!isCurrent(self, serial)) {
        setResultCode(ctx, kCellOk);
        return;
    }
    sail_hle::unbindAdapters(self);
    for (auto& entry : g_descriptors) {
        if (entry.second.player == self) {
            entry.second.player = 0;
            entry.second.registered = false;
        }
    }
    g_players.erase(self);
    logDebug("tox2: sail: player {:#010x} finalized", self);
    setResultCode(ctx, kCellOk);
}

// cellSailPlayerSetSoundAdapter(CellSailPlayer* self, u32 index, CellSailSoundAdapter* adapter)
void playerSetSoundAdapter(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t self = argU32(ctx, 0);
    const Player* player = findPlayer(self);
    if (player == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    // RPCS3 compares with `>`, so index == max is accepted.
    if (argU32(ctx, 1) > player->max_audio_streams) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    setResultCode(ctx, sail_hle::bindAdapter(sail_hle::AdapterKind::Sound, argU32(ctx, 2), self));
}

// cellSailPlayerSetGraphicsAdapter(CellSailPlayer* self, u32 index, CellSailGraphicsAdapter* adapter)
void playerSetGraphicsAdapter(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t self = argU32(ctx, 0);
    const Player* player = findPlayer(self);
    if (player == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    if (argU32(ctx, 1) > player->max_video_streams) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    setResultCode(ctx, sail_hle::bindAdapter(sail_hle::AdapterKind::Graphics, argU32(ctx, 2), self));
}

// cellSailPlayerSetRepeatMode(CellSailPlayer* self, s32 repeat_mode, CellSailStartCommand* command)
void playerSetRepeatMode(Tox2PpuContext* ctx, std::uint8_t* base) {
    Player* player = findPlayer(argU32(ctx, 0));
    if (player == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    player->repeat_mode = static_cast<std::int32_t>(argU32(ctx, 1));
    const std::uint32_t command = argU32(ctx, 2);
    if (guestMapped(command, sail::kStartCommandSize)) {
        std::memcpy(player->repeat_command.data(), base + command, sail::kStartCommandSize);
    }
    setResultCode(ctx, kCellOk);
}

// cellSailPlayerBoot(CellSailPlayer* self, u64 user_param)
void playerBoot(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t self = argU32(ctx, 0);
    Player* player = findPlayer(self);
    if (player == nullptr || player->state != sail::PlayerState::Initialized) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    player->state = sail::PlayerState::Closed;
    queueEvents(self, *player,
                {stateChanged(sail::PlayerState::BootTransition), stateChanged(sail::PlayerState::Closed),
                 callCompleted(sail::kCallBoot)});
    setResultCode(ctx, kCellOk);
}

struct HeaderRead {
    std::optional<PamfInfo> info;
    std::filesystem::path host_path;
    std::uint32_t error_code = kCellOk;
    std::string error;
};

// Reads and parses the PAMF header of the guest file `guest_path`. Nothing here formats a host path:
// the file name can contain characters the narrow conversion rejects.
HeaderRead readPamfHeader(const std::string& guest_path) {
    HeaderRead result;
    try {
        bool read_only = false;
        const auto host = resolveGuestPath(guest_path, read_only);
        if (!host) {
            result.error_code = sail::kErrorInvalidFd;
            result.error = "the path is not mapped";
            return result;
        }
        result.host_path = *host;
        std::ifstream in(*host, std::ios::binary);
        if (!in) {
            result.error_code = sail::kErrorInvalidFd;
            result.error = "the file cannot be opened";
            return result;
        }
        std::vector<std::uint8_t> header(kPamfHeaderReadSize);
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        const auto got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
        auto parsed = parsePamfHeader(std::span<const std::uint8_t>(header).first(got));
        if (!parsed) {
            result.error_code = sail::kErrorUnsupportedStream;
            result.error = parsed.error();
            return result;
        }
        result.info = std::move(*parsed);
    } catch (const std::exception&) {
        result.info.reset();
        result.error_code = sail::kErrorInvalidFd;
        result.error = "file system error";
    }
    return result;
}

// cellSailPlayerCreateDescriptor(CellSailPlayer* self, s32 stream_type, void* media_info,
//                                const char* uri, CellSailDescriptor** desc)
void playerCreateDescriptor(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    const std::uint32_t uri_addr = argU32(ctx, 3);
    const std::uint32_t out = argU32(ctx, 4);
    const Player* player = findPlayer(self);
    if (player == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    if (!guestMapped(uri_addr, 1) || !guestMapped(out, 4)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    tox2Write32(base, out, 0);
    if (static_cast<std::int32_t>(argU32(ctx, 1)) != sail::kStreamPamf) {
        logWarn("tox2: sail: descriptor stream type {} is not supported", static_cast<std::int32_t>(argU32(ctx, 1)));
        setResultCode(ctx, sail::kErrorUnsupportedStream);
        return;
    }
    const std::string uri = readGuestString(base, uri_addr, kUriMaxLength);
    if (!uri.starts_with(kUriScheme)) {
        logWarn("tox2: sail: descriptor URI {} is not an {} URI", uri, kUriScheme);
        setResultCode(ctx, sail::kErrorUnsupportedStream);
        return;
    }

    if (player->alloc_opd == 0) {
        setResultCode(ctx, sail::kErrorMemory);
        return;
    }
    // The callback may wait inside the game's heap, so nothing is read through `player` afterwards.
    const std::uint32_t alloc_arg = player->alloc_arg;
    const std::uint32_t free_opd = player->free_opd;
    const std::uint64_t serial = player->serial;
    const auto block = static_cast<std::uint32_t>(callGuestFunction(
            ctx, base, player->alloc_opd, {alloc_arg, kDescriptorBoundary, sail::kDescriptorMaxSize}));
    if (!guestMapped(block, sail::kDescriptorMaxSize)) {
        logError("tox2: sail: the allocator callback returned {:#010x} for a descriptor", block);
        setResultCode(ctx, sail::kErrorMemory);
        return;
    }
    std::memset(base + block, 0, sail::kDescriptorMaxSize);

    Descriptor descriptor;
    // The callback can release the guest lock, so the player may have been finalized (or its address
    // reused) meanwhile. The descriptor is then left unlinked: the game still gets the block and can
    // destroy it, but no finalized player's state is attached to it.
    descriptor.player = isCurrent(self, serial) ? self : 0;
    descriptor.free_opd = free_opd;
    descriptor.alloc_arg = alloc_arg;
    descriptor.guest_path = uri.substr(sizeof(kUriScheme) - 1);
    HeaderRead header = readPamfHeader(descriptor.guest_path);
    descriptor.info = std::move(header.info);
    descriptor.host_path = std::move(header.host_path);
    descriptor.error_code = header.error_code;
    if (descriptor.info) {
        const PamfInfo& info = *descriptor.info;
        logInfo("tox2: sail: descriptor {:#010x} for {}: {} streams, {} .. {} (90 kHz)", block, descriptor.guest_path,
                info.streams.size(), info.start_pts, info.end_pts);
    } else {
        logError("tox2: sail: descriptor {:#010x} for {}: {}", block, descriptor.guest_path, header.error);
    }
    if (descriptor.player == 0) {
        logWarn("tox2: sail: player {:#010x} was finalized while its descriptor was allocated", self);
    }
    // A block that is already tracked means the game freed it without DestroyDescriptor and got the
    // address back; the new descriptor replaces the stale one.
    if (g_descriptors.contains(block)) {
        logWarn("tox2: sail: descriptor block {:#010x} was still tracked and is replaced", block);
    }
    g_descriptors.insert_or_assign(block, std::move(descriptor));
    tox2Write32(base, out, block);
    setResultCode(ctx, kCellOk);
}

// cellSailPlayerAddDescriptor(CellSailPlayer* self, CellSailDescriptor* desc)
void playerAddDescriptor(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t self = argU32(ctx, 0);
    if (findPlayer(self) == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    Descriptor* descriptor = findDescriptor(argU32(ctx, 1));
    if (descriptor == nullptr || descriptor->player != self) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (descriptor->registered) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    descriptor->registered = true;
    setResultCode(ctx, kCellOk);
}

// cellSailPlayerDestroyDescriptor(CellSailPlayer* self, CellSailDescriptor* desc)
// The game calls it after PlayerFinalize, which releases the descriptor; a descriptor still added to
// a live player is refused (INVALID_STATE, as in RPCS3).
void playerDestroyDescriptor(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    const std::uint32_t address = argU32(ctx, 1);
    const Descriptor* descriptor = findDescriptor(address);
    if (descriptor == nullptr || (descriptor->player != 0 && descriptor->player != self)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (descriptor->registered) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::uint32_t free_opd = descriptor->free_opd;
    const std::uint32_t alloc_arg = descriptor->alloc_arg;
    g_descriptors.erase(address);
    if (free_opd != 0) {
        callGuestFunction(ctx, base, free_opd, {alloc_arg, kDescriptorBoundary, address});
    }
    setResultCode(ctx, kCellOk);
}

// cellSailDescriptorCreateDatabase(CellSailDescriptor* self, void* database, u32 size, u64 arg)
// The block is a CellPamfReader-sized object only cellPamfReaderGetNumberOfSpecificStreams reads.
void descriptorCreateDatabase(Tox2PpuContext* ctx, std::uint8_t* base) {
    const Descriptor* descriptor = findDescriptor(argU32(ctx, 0));
    const std::uint32_t database = argU32(ctx, 1);
    if (descriptor == nullptr || argU32(ctx, 2) < sail::kPamfReaderSize ||
        !guestMapped(database, sail::kPamfReaderSize)) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    if (!descriptor->info) {
        setResultCode(ctx, descriptor->error_code);
        return;
    }
    sail_hle::writePamfDatabase(base, database, *descriptor->info);
    setResultCode(ctx, kCellOk);
}

// cellSailDescriptorDestroyDatabase(CellSailDescriptor* self, void* database)
void descriptorDestroyDatabase(Tox2PpuContext* ctx, std::uint8_t* base) {
    if (findDescriptor(argU32(ctx, 0)) == nullptr || !sail_hle::clearPamfDatabase(base, argU32(ctx, 1))) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    setResultCode(ctx, kCellOk);
}

// cellSailDescriptorGetCapabilities(CellSailDescriptor* self, u32* out0, u32* out1, u32* out2,
//                                   u32* out3)
// RPCS3 has no signature for this call. The game passes four word pointers and keeps bit 1 of the
// last one without ever reading it, so the capabilities are all reported as clear.
void descriptorGetCapabilities(Tox2PpuContext* ctx, std::uint8_t* base) {
    if (findDescriptor(argU32(ctx, 0)) == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidArg);
        return;
    }
    for (int i = 1; i <= 4; i++) {
        const std::uint32_t out = argU32(ctx, i);
        if (guestMapped(out, 4)) tox2Write32(base, out, 0);
    }
    setResultCode(ctx, kCellOk);
}

using VideoWire = std::array<std::uint8_t, sail::kVideoFormatSize>;
using AudioWire = std::array<std::uint8_t, sail::kAudioFormatSize>;

// Guest-layout format structs are big-endian byte arrays here; these read and write one field.
template <class Bytes>
std::int32_t readField(const Bytes& bytes, std::uint32_t offset, std::uint32_t size) {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < size; i++) value = (value << 8) | bytes[offset + i];
    // Sign-extend, since "unspecified" is -1 in every field.
    const std::uint32_t shift = 32 - 8 * size;
    return static_cast<std::int32_t>(value << shift) >> shift;
}

template <class Bytes>
void writeField(Bytes& bytes, std::uint32_t offset, std::uint32_t size, std::uint32_t value) {
    for (std::uint32_t i = 0; i < size; i++)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * (size - 1 - i)));
}

std::int32_t frameRateCode(std::uint32_t num, std::uint32_t den) {
    struct Rate {
        std::uint32_t num, den;
        std::int32_t code;
    };
    constexpr Rate kRates[] = {{24000, 1001, sail::kVideoFrameRate24000Over1001},
                               {24, 1, sail::kVideoFrameRate24},
                               {25, 1, sail::kVideoFrameRate25},
                               {30000, 1001, sail::kVideoFrameRate30000Over1001},
                               {30, 1, sail::kVideoFrameRate30},
                               {50, 1, sail::kVideoFrameRate50},
                               {60000, 1001, sail::kVideoFrameRate60000Over1001},
                               {60, 1, sail::kVideoFrameRate60}};
    for (const Rate& rate : kRates) {
        if (rate.num == num && rate.den == den) return rate.code;
    }
    return sail::kVideoFrameRate30000Over1001;
}

const PamfStream* firstStream(const PamfInfo* info, bool video) {
    if (info == nullptr) return nullptr;
    for (const PamfStream& stream : info->streams) {
        const bool is_video = stream.codec == PamfCodec::Avc || stream.codec == PamfCodec::M2v;
        const bool is_audio = stream.codec == PamfCodec::Atrac3plus || stream.codec == PamfCodec::Lpcm ||
                              stream.codec == PamfCodec::Ac3;
        if (video ? is_video : is_audio) return &stream;
    }
    return nullptr;
}

// Settles what the host player converts to, from what the game asked for (fields of -1 are
// unspecified and take the stream's values; the game asks for ARGB with alpha 255 and nothing else).
void chooseFormats(Stream& stream, const PamfInfo* info, const std::optional<sail_hle::GraphicsBinding>& gfx,
                   const std::optional<sail_hle::SoundBinding>& snd) {
    VideoWire video;
    video.fill(0xff);
    if (gfx) video = gfx->preferred;
    const PamfStream* video_stream = firstStream(info, true);

    VideoOutputFormat& out = stream.video_format;
    out.coding = VideoCoding::Argb;
    switch (readField(video, sail::kVideoFormatCoding, 1)) {
        case sail::kVideoCodingRgbaInterleaved: out.coding = VideoCoding::Rgba; break;
        case sail::kVideoCodingYuv420Planar: out.coding = VideoCoding::Yuv420Planar; break;
        default: break;
    }
    const std::int32_t wanted_width = readField(video, sail::kVideoFormatWidth, 2);
    const std::int32_t wanted_height = readField(video, sail::kVideoFormatHeight, 2);
    stream.width = wanted_width > 0                                      ? static_cast<std::uint32_t>(wanted_width)
                   : video_stream != nullptr && video_stream->width != 0 ? video_stream->width
                                                                         : kDefaultWidth;
    stream.height = wanted_height > 0                                      ? static_cast<std::uint32_t>(wanted_height)
                    : video_stream != nullptr && video_stream->height != 0 ? video_stream->height
                                                                           : kDefaultHeight;
    const bool planar = out.coding == VideoCoding::Yuv420Planar;
    const std::uint32_t row = planar ? stream.width : stream.width * 4;
    const std::int32_t wanted_pitch = readField(video, sail::kVideoFormatPitch, 4);
    const std::uint32_t pitch = wanted_pitch > 0 && static_cast<std::uint32_t>(wanted_pitch) >= row
                                        ? static_cast<std::uint32_t>(wanted_pitch)
                                        : row;
    const std::int32_t alpha = readField(video, sail::kVideoFormatAlpha, 4);
    out.width = stream.width;
    out.height = stream.height;
    out.pitch = pitch;
    out.alpha = alpha >= 0 && alpha <= 255 ? static_cast<std::uint8_t>(alpha) : 255;
    // The game's movies signal BT.709; an unspecified matrix follows the usual HD/SD split.
    switch (readField(video, sail::kVideoFormatColorMatrix, 1)) {
        case sail::kVideoColorMatrixBt601: out.bt709 = false; break;
        case sail::kVideoColorMatrixBt709: out.bt709 = true; break;
        default: out.bt709 = stream.height >= 720; break;
    }
    stream.frame_bytes =
            planar ? pitch * stream.height + 2 * ((pitch + 1) / 2) * ((stream.height + 1) / 2) : pitch * stream.height;

    AudioWire audio;
    audio.fill(0xff);
    if (snd) audio = snd->preferred;
    const PamfStream* audio_stream = firstStream(info, false);
    const std::int32_t wanted_channels = readField(audio, sail::kAudioFormatChNum, 1);
    const std::int32_t wanted_rate = readField(audio, sail::kAudioFormatFs, 4);
    std::uint32_t channels = wanted_channels > 0 ? static_cast<std::uint32_t>(wanted_channels)
                             : audio_stream != nullptr && audio_stream->channels != 0 ? audio_stream->channels
                                                                                      : 2;
    channels = std::clamp<std::uint32_t>(channels, 1, kMaxAudioChannels);
    stream.audio_format.channels = channels;
    stream.audio_format.sample_rate = wanted_rate > 0 ? static_cast<std::uint32_t>(wanted_rate)
                                      : audio_stream != nullptr && audio_stream->sample_rate != 0
                                              ? audio_stream->sample_rate
                                              : kDefaultSampleRate;
    // CELL_SAIL_AUDIO_CODING_LPCM_FLOAT32 is the only coding cellSail defines, so the coding field needs
    // no mapping: the decoder always delivers interleaved big-endian float, which is what the game reads.
    stream.audio_format.float32 = true;
    stream.audio_frame_bytes = channels * 4;
    // No resampling or remixing happens, so a stream that differs from the game's format plays as the
    // decoder produces it (counted in the decoder statistics).
    if (audio_stream != nullptr && snd &&
        ((audio_stream->sample_rate != 0 && audio_stream->sample_rate != stream.audio_format.sample_rate) ||
         (audio_stream->channels != 0 && audio_stream->channels != channels))) {
        logWarn("tox2: sail: the audio stream ({} Hz, {} ch) differs from the sound adapter's format ({} Hz, {} ch)",
                audio_stream->sample_rate, audio_stream->channels, stream.audio_format.sample_rate, channels);
    }
    stream.pcm_frames = snd ? kMaxSoundFrames : 0;
}

// The CellSailVideoFormat GetFormat and pFormatChanged report once the stream is open.
VideoWire videoWire(const Stream& stream, std::int32_t frame_rate) {
    VideoWire wire{};
    const bool planar = stream.video_format.coding == VideoCoding::Yuv420Planar;
    std::int32_t coding = sail::kVideoCodingArgbInterleaved;
    if (stream.video_format.coding == VideoCoding::Rgba) coding = sail::kVideoCodingRgbaInterleaved;
    if (planar) coding = sail::kVideoCodingYuv420Planar;
    writeField(wire, sail::kVideoFormatCoding, 1, static_cast<std::uint32_t>(coding));
    writeField(wire, sail::kVideoFormatScan, 1, sail::kVideoScanProgressive);
    writeField(wire, sail::kVideoFormatBitsPerColor, 1, 8);
    writeField(wire, sail::kVideoFormatFrameRate, 1, static_cast<std::uint32_t>(frame_rate));
    writeField(wire, sail::kVideoFormatWidth, 2, stream.width);
    writeField(wire, sail::kVideoFormatHeight, 2, stream.height);
    writeField(wire, sail::kVideoFormatPitch, 4, stream.video_format.pitch);
    writeField(wire, sail::kVideoFormatAlpha, 4, stream.video_format.alpha);
    writeField(wire, sail::kVideoFormatColorMatrix, 1,
               stream.video_format.bt709 ? sail::kVideoColorMatrixBt709 : sail::kVideoColorMatrixBt601);
    writeField(wire, sail::kVideoFormatAspectRatio, 1, sail::kVideoAspectRatio1To1);
    // RGB output is full range; planar output keeps the stream's limited range.
    writeField(wire, sail::kVideoFormatColorRange, 1,
               planar ? sail::kVideoColorRangeLimited : sail::kVideoColorRangeFull);
    return wire;
}

AudioWire audioWire(const Stream& stream) {
    AudioWire wire{};
    writeField(wire, sail::kAudioFormatCoding, 1, sail::kAudioCodingLpcmFloat32);
    writeField(wire, sail::kAudioFormatChNum, 1, stream.audio_format.channels);
    writeField(wire, sail::kAudioFormatSampleNum, 2, static_cast<std::uint32_t>(sail::kAudioUnspecified));
    writeField(wire, sail::kAudioFormatFs, 4, stream.audio_format.sample_rate);
    writeField(wire, sail::kAudioFormatChLayout, 4,
               stream.audio_format.channels == 2 ? static_cast<std::uint32_t>(sail::kAudioChLayout2ChLr)
                                                 : static_cast<std::uint32_t>(sail::kAudioUnspecified));
    writeField(wire, sail::kAudioFormatReserved0, 4, static_cast<std::uint32_t>(sail::kAudioUnspecified));
    writeField(wire, sail::kAudioFormatReserved1, 4, static_cast<std::uint32_t>(sail::kAudioUnspecified));
    writeField(wire, sail::kAudioFormatReserved1 + 4, 4, static_cast<std::uint32_t>(sail::kAudioUnspecified));
    return wire;
}

struct FlipSnapshot {
    std::uint64_t flips = 0, skipped = 0;
};

FlipSnapshot flipSnapshot() {
    const RsxCensus rsx = rsxCensusSnapshot();
    return {rsx.flips, rsx.draws_skipped};
}

void snapshotEnd(Stream& stream) {
    stream.ended = true;
    if (stream.start_us == 0) return;
    const FlipSnapshot flips = flipSnapshot();
    stream.end_us = systemTimeUs();
    stream.end_flips = flips.flips;
    stream.end_skipped = flips.skipped;
}

std::vector<std::string> sailCensusLines() {
    SummaryState& summaries = summaryState();
    std::lock_guard<std::mutex> lock(summaries.mutex);
    return summaries.lines;
}

// The per-movie lines of Design/Logging, once the stream is over and its counters are final.
void recordSummary(Stream& stream) {
    std::string line;
    if (!stream.open_ok) {
        line = std::format("movie {}: not played: {}", stream.guest_path, stream.open_error);
    } else {
        const MoviePlayerStats stats = stream.movie.stats();
        const double fps =
                stats.frame_rate_den != 0 ? static_cast<double>(stats.frame_rate_num) / stats.frame_rate_den : 0.0;
        line = std::format(
                "movie {}: {}x{} {:.2f} fps, {} decoded, {} delivered, {} dropped, audio {} samples ({} underruns), "
                "played {:.2f} s of {:.2f} s",
                stream.guest_path, stats.width, stats.height, fps, stats.video_decoded, stats.video_delivered,
                stats.video_dropped, stats.audio_delivered_frames, stats.audio_underruns,
                static_cast<double>(stats.played_ticks) / kTicksPerSecond,
                static_cast<double>(stats.duration_ticks) / kTicksPerSecond);
        logDebug(
                "tox2: sail: movie {} details: video {} rejected {}, audio decoded {} delivered {} silence {} stale {} "
                "overflow {} rejected {} bad {}, demux errors {}, read errors {}, GetFrame frames refused {}, codecs: {}, "
                "decode {:.2f} ms/frame (longest call {:.2f} ms), frame copy {:.2f} ms/frame (longest {:.2f} ms)",
                stream.guest_path, stats.video_decoder ? "decoded" : "black", stats.video_rejected,
                stats.audio_decoded_frames, stats.audio_delivered_frames, stats.audio_silence_frames,
                stats.audio_stale_frames, stats.audio_overflow_frames, stats.audio_rejected, stats.audio_bad_chunks,
                stats.demux_errors, stats.read_errors, stream.frames_rejected,
                stats.codec_error.empty() ? "ok" : stats.codec_error,
                stats.video_decoded != 0 ? static_cast<double>(stats.video_decode_ns) / 1e6 / stats.video_decoded : 0.0,
                static_cast<double>(stats.video_decode_max_ns) / 1e6,
                stats.video_delivered != 0 ? static_cast<double>(stream.frame_copy_ns) / 1e6 / stats.video_delivered
                                           : 0.0,
                static_cast<double>(stream.frame_copy_max_ns) / 1e6);
    }
    logInfo("tox2: sail: {}", line);
    if (stream.start_us != 0 && stream.ended && stream.end_us > stream.start_us) {
        const double seconds = static_cast<double>(stream.end_us - stream.start_us) / 1e6;
        const std::uint64_t flips = stream.end_flips - stream.start_flips;
        logInfo("tox2: sail: movie window {}: {:.3f} .. {:.3f} s ({:.2f} s), {} flips ({:.1f}/s), {} skipped draws, {} "
                "GetFrame calls ({:.1f}/s)",
                stream.guest_path, static_cast<double>(stream.start_us) / 1e6, static_cast<double>(stream.end_us) / 1e6,
                seconds, flips, static_cast<double>(flips) / seconds, stream.end_skipped - stream.start_skipped,
                stream.get_frame_calls, static_cast<double>(stream.get_frame_calls) / seconds);
    }
    {
        SummaryState& summaries = summaryState();
        std::lock_guard<std::mutex> lock(summaries.mutex);
        summaries.lines.push_back(std::move(line));
    }
    setSailCensusProvider(&sailCensusLines);
}

// Hands the frame buffers back through the graphics adapter's pFree and the scratch block through
// the allocator, in that order (the pointer array lives in the scratch block). Runs on the service
// thread, after every call that used them.
void releaseStreamMemory(Tox2PpuContext* ctx, std::uint8_t* base, Stream& stream,
                         const std::optional<sail_hle::GraphicsBinding>& gfx, std::uint32_t free_opd,
                         std::uint32_t alloc_arg) {
    if (stream.frames_usable && gfx && gfx->free != 0 && stream.scratch != 0) {
        const std::uint32_t pointers = stream.scratch + kScratchFramePointers;
        for (std::size_t i = 0; i < stream.frames.size(); i++) {
            tox2Write32(base, pointers + 4 * static_cast<std::uint32_t>(i), stream.frames[i]);
        }
        callGuestFunction(ctx, base, gfx->free, {gfx->arg, static_cast<std::uint32_t>(stream.frames.size()), pointers});
    }
    stream.frames_usable = false;
    stream.frames.clear();
    if (stream.scratch != 0 && free_opd != 0) {
        callGuestFunction(ctx, base, free_opd, {alloc_arg, kScratchBoundary, stream.scratch});
    }
    stream.scratch = 0;
}

// Stops the host decode, reports the movie and queues the release of its guest memory. The game
// may free its callback argument once Finalize returns, so the release is part of what Finalize
// waits for.
void retireStream(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t self, Player& player) {
    const std::shared_ptr<Stream> stream = std::move(player.stream);
    player.stream.reset();
    if (!stream) return;
    stream->movie.stop();
    stream->running = false;
    if (!stream->ended) snapshotEnd(*stream);
    stream->summarized = true;
    recordSummary(*stream);

    const auto gfx = sail_hle::graphicsBoundTo(self);
    const std::uint32_t free_opd = player.free_opd, alloc_arg = player.alloc_arg;
    const bool queued =
            queueSailCall([stream, gfx, free_opd, alloc_arg](Tox2PpuContext* service_ctx, std::uint8_t* service_base) {
                releaseStreamMemory(service_ctx, service_base, *stream, gfx, free_opd, alloc_arg);
            });
    if (!queued) releaseStreamMemory(ctx, base, *stream, gfx, free_opd, alloc_arg);
}

// The service-thread half of OpenStream: the graphics adapter's format and allocation callbacks,
// then the events. Both callbacks finish before CALL_COMPLETED so the game finds the format and the
// buffers when it resumes.
void completeOpenStream(Tox2PpuContext* ctx, std::uint8_t* base, const std::shared_ptr<Stream>& stream,
                        const Notifier& to, const std::optional<sail_hle::GraphicsBinding>& gfx,
                        const std::optional<sail_hle::SoundBinding>& snd) {
    if (stream->summarized || !isCurrent(to.address, to.serial)) return;
    if (stream->scratch != 0) {
        if (snd && snd->format_changed != 0) {
            callGuestFunction(ctx, base, snd->format_changed,
                              {snd->arg, stream->scratch + kScratchSoundFormat, stream->session_id});
        }
        if (gfx && gfx->format_changed != 0) {
            callGuestFunction(ctx, base, gfx->format_changed,
                              {gfx->arg, stream->scratch + kScratchVideoFormat, stream->session_id});
        }
        if (gfx && gfx->alloc != 0) {
            const std::uint32_t pointers = stream->scratch + kScratchFramePointers;
            const auto result = static_cast<std::uint32_t>(
                    callGuestFunction(ctx, base, gfx->alloc, {gfx->arg, stream->frame_bytes, kFrameBuffers, pointers}));
            stream->frames.assign(kFrameBuffers, 0);
            bool usable = result == kCellOk;
            for (std::uint32_t i = 0; i < kFrameBuffers; i++) {
                stream->frames[i] = tox2Read32(base, pointers + 4 * i);
                usable = usable && guestMapped(stream->frames[i], stream->frame_bytes);
            }
            stream->frames_usable = usable;
            if (!usable) {
                logError(
                        "tox2: sail: the frame allocation for {} returned {:#x} and unusable buffers; the movie plays "
                        "without video",
                        stream->guest_path, result);
            }
        }
    }
    deliverEvents(ctx, base, to,
                  {stateChanged(sail::PlayerState::OpenTransition), plainEvent(sail::kEventStreamOpened),
                   stateChanged(sail::PlayerState::Opened), callCompleted(sail::kCallOpenStream)});
}

// cellSailPlayerOpenStream(CellSailPlayer* self, u64 user_param)
// Opens the host player on the descriptor the game added, settles the frame format and has the
// game allocate the frame buffers. A descriptor whose file could not be read still completes: the
// game then fails at CreateDatabase and tears down, and it would wait for the completion forever
// otherwise.
void playerOpenStream(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t self = argU32(ctx, 0);
    const Player* player = findPlayer(self);
    if (player == nullptr || player->state != sail::PlayerState::Closed) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const Descriptor* descriptor = nullptr;
    for (const auto& entry : g_descriptors) {
        if (entry.second.player == self && entry.second.registered) {
            descriptor = &entry.second;
            break;
        }
    }
    if (descriptor == nullptr) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }

    // Everything below that follows the allocator callback works on copies: the callback can release
    // the guest lock, and the player may be finalized meanwhile.
    const std::uint64_t serial = player->serial;
    const std::uint32_t alloc_opd = player->alloc_opd, free_opd = player->free_opd, alloc_arg = player->alloc_arg;
    const Notifier to = notifierOf(self, *player);
    const auto gfx = sail_hle::graphicsBoundTo(self);
    const auto snd = sail_hle::soundBoundTo(self);

    auto stream = std::make_shared<Stream>();
    stream->session_id = g_next_session++;
    stream->guest_path = descriptor->guest_path;
    const PamfInfo* info = descriptor->info ? &*descriptor->info : nullptr;
    chooseFormats(*stream, info, gfx, snd);
    if (info == nullptr) {
        stream->open_error = std::format("no readable PAMF header (error {:#x})", descriptor->error_code);
    } else if (auto opened = stream->movie.open(descriptor->host_path, stream->video_format, stream->audio_format)) {
        stream->open_ok = true;
    } else {
        stream->open_error = opened.error();
    }
    if (!stream->open_ok) {
        logError("tox2: sail: {} cannot be played: {}", stream->guest_path, stream->open_error);
    }
    descriptor = nullptr;
    info = nullptr;

    if (stream->open_ok && alloc_opd != 0) {
        const std::uint32_t size = kScratchPcm + stream->pcm_frames * stream->audio_frame_bytes;
        const auto block = static_cast<std::uint32_t>(
                callGuestFunction(ctx, base, alloc_opd, {alloc_arg, kScratchBoundary, size}));
        const bool mapped = guestMapped(block, size);
        if (!isCurrent(self, serial)) {
            if (mapped && free_opd != 0) callGuestFunction(ctx, base, free_opd, {alloc_arg, kScratchBoundary, block});
            setResultCode(ctx, sail::kErrorInvalidState);
            return;
        }
        if (mapped) {
            stream->scratch = block;
            std::memset(base + block, 0, size);
        } else {
            logError("tox2: sail: the allocator callback returned {:#010x} for the stream's scratch block", block);
        }
    }
    player = findPlayer(self);
    if (player == nullptr || player->state != sail::PlayerState::Closed) {
        // Finalized (or reopened) while the allocator callback ran.
        if (stream->scratch != 0 && free_opd != 0) {
            callGuestFunction(ctx, base, free_opd, {alloc_arg, kScratchBoundary, stream->scratch});
        }
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }

    const MoviePlayerStats stats = stream->movie.stats();
    const std::int32_t frame_rate = frameRateCode(stats.frame_rate_num, stats.frame_rate_den);
    const VideoWire video_wire = videoWire(*stream, frame_rate);
    const AudioWire audio_wire = audioWire(*stream);
    if (stream->scratch != 0) {
        std::memcpy(base + stream->scratch + kScratchVideoFormat, video_wire.data(), video_wire.size());
        std::memcpy(base + stream->scratch + kScratchSoundFormat, audio_wire.data(), audio_wire.size());
    }
    // GetFormat must report the stream's format by the time the game sees the completion.
    sail_hle::setGraphicsFormat(self, video_wire);
    sail_hle::setSoundFormat(self, audio_wire);

    Player& live = g_players.at(self);
    live.stream = stream;
    live.state = sail::PlayerState::Opened;
    logDebug("tox2: sail: stream {} opened: {}x{} pitch {} frame {} bytes, session {}", stream->guest_path,
             stream->width, stream->height, stream->video_format.pitch, stream->frame_bytes, stream->session_id);
    queueSailCall([stream, to, gfx, snd](Tox2PpuContext* service_ctx, std::uint8_t* service_base) {
        completeOpenStream(service_ctx, service_base, stream, to, gfx, snd);
    });
    setResultCode(ctx, kCellOk);
}

// Waits, with the guest lock released, for the decoders' first output. The wait polls: the player's
// own wait is not one the guest lock can be released from.
void waitForPreroll(Stream& stream) {
    std::condition_variable_any idle;
    const auto deadline = std::chrono::steady_clock::now() + kPrerollTimeout;
    while (!stream.movie.waitPrerolled(std::chrono::milliseconds(0))) {
        if (stream.summarized) return;
        if (std::chrono::steady_clock::now() >= deadline) {
            logWarn("tox2: sail: {} produced no first frame within {} s; its clock starts anyway", stream.guest_path,
                    std::chrono::duration_cast<std::chrono::seconds>(kPrerollTimeout).count());
            return;
        }
        blockingWait(idle, [] { return false; }, "sail preroll", std::chrono::milliseconds(5));
    }
}

// The service-thread half of Start: the clock starts only after the decoders have output, so their
// start-up does not make the first frames late, and RUNNING is reported at that moment.
void completeStart(Tox2PpuContext* ctx, std::uint8_t* base, const std::shared_ptr<Stream>& stream, const Notifier& to) {
    if (!isCurrent(to.address, to.serial)) return;
    deliverEvents(ctx, base, to,
                  {stateChanged(sail::PlayerState::StartTransition), plainEvent(sail::kEventSessionStarted)});
    waitForPreroll(*stream);
    if (stream->summarized || !isCurrent(to.address, to.serial)) return;
    stream->movie.restartClock();
    const FlipSnapshot flips = flipSnapshot();
    stream->start_us = systemTimeUs();
    stream->start_flips = flips.flips;
    stream->start_skipped = flips.skipped;
    stream->running = true;
    logInfo("tox2: sail: movie start {} at {:.3f} s, flip {}", stream->guest_path,
            static_cast<double>(stream->start_us) / 1e6, flips.flips);
    deliverEvents(ctx, base, to, {stateChanged(sail::PlayerState::Running), callCompleted(sail::kCallStart)});
}

// cellSailPlayerStart(CellSailPlayer* self, CellSailStartCommand* command, u64 user_param)
// The game passes a zeroed command (play from the start to the end), which is the only one played.
void playerStart(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t self = argU32(ctx, 0);
    Player* player = findPlayer(self);
    if (player == nullptr || player->state != sail::PlayerState::Opened || !player->stream ||
        !player->stream->open_ok || player->stream->started) {
        setResultCode(ctx, sail::kErrorInvalidState);
        return;
    }
    const std::shared_ptr<Stream> stream = player->stream;
    if (!stream->movie.start()) {
        logError("tox2: sail: the decode thread for {} could not be started", stream->guest_path);
        setResultCode(ctx, sail::kErrorFatal);
        return;
    }
    stream->started = true;
    player->state = sail::PlayerState::Running;
    queueSailCall([stream, to = notifierOf(self, *player)](Tox2PpuContext* service_ctx, std::uint8_t* service_base) {
        completeStart(service_ctx, service_base, stream, to);
    });
    setResultCode(ctx, kCellOk);
}

// The player has to leave RUNNING on its own at the end of the stream, since the game never stops it.
// The wall clock decides, not the decoder: a slow decoder drops frames and the movie still ends on
// time. Called from the frame polls, which the game makes every tick while the state is RUNNING.
void checkStreamEnd(std::uint32_t self, Player& player, Stream& stream) {
    if (!stream.running || stream.eos || !stream.movie.atEnd(stream.movie.now())) return;
    stream.eos = true;
    stream.running = false;
    snapshotEnd(stream);
    player.state = sail::PlayerState::Opened;
    if (player.repeat_mode != 0) {
        logOnce(LogLevel::Info, "sail-repeat",
                "tox2: sail: repeat mode {} is set; the stream ends instead of looping (the game restarts a looping "
                "movie itself)",
                player.repeat_mode);
    }
    queueEvents(self, player,
                {plainEvent(sail::kEventSourceEos), stateChanged(sail::PlayerState::StopTransition),
                 stateChanged(sail::PlayerState::Opened)});
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x346ebba3, "cellSailMemAllocatorInitialize", sailMemAllocatorInitialize},
        {kModule, 0x23654375, "cellSailPlayerInitialize2", playerInitialize2},
        {kModule, 0x18b4629d, "cellSailPlayerFinalize", playerFinalize},
        {kModule, 0x1139a206, "cellSailPlayerSetSoundAdapter", playerSetSoundAdapter},
        {kModule, 0x18bcd21b, "cellSailPlayerSetGraphicsAdapter", playerSetGraphicsAdapter},
        {kModule, 0xfc5baf8a, "cellSailPlayerSetRepeatMode", playerSetRepeatMode},
        {kModule, 0xbdf21b0f, "cellSailPlayerBoot", playerBoot},
        {kModule, 0x34ecc1b9, "cellSailPlayerOpenStream", playerOpenStream},
        {kModule, 0xe535b0d3, "cellSailPlayerStart", playerStart},
        {kModule, 0xd7938b8d, "cellSailPlayerCreateDescriptor", playerCreateDescriptor},
        {kModule, 0x7c8dff3b, "cellSailPlayerAddDescriptor", playerAddDescriptor},
        {kModule, 0xfc839bd4, "cellSailPlayerDestroyDescriptor", playerDestroyDescriptor},
        {kModule, 0x0abb318b, "cellSailDescriptorCreateDatabase", descriptorCreateDatabase},
        {kModule, 0x28336e89, "cellSailDescriptorDestroyDatabase", descriptorDestroyDatabase},
        {kModule, 0xac9c3b1f, "cellSailDescriptorGetCapabilities", descriptorGetCapabilities},
};

}  // namespace

std::span<const HleEntry> cellSailPlayerHandlers() { return kHandlers; }

void resetCellSail() {
    g_players.clear();
    g_descriptors.clear();
    {
        SummaryState& summaries = summaryState();
        std::lock_guard<std::mutex> lock(summaries.mutex);
        summaries.lines.clear();
    }
    sail_hle::resetAdapters();
}

namespace sail_hle {

std::optional<VideoFrameResult> takeVideoFrame(std::uint8_t* base, std::uint32_t player_address) {
    Player* player = findPlayer(player_address);
    if (player == nullptr || !player->stream) return std::nullopt;
    Stream& stream = *player->stream;
    stream.get_frame_calls++;
    if (!stream.running) return std::nullopt;

    std::optional<VideoFrameResult> result;
    if (auto due = stream.movie.takeDueVideo(stream.movie.now())) {
        const VideoFrame& frame = due->frame;
        // A frame of another size would be copied with the wrong row layout.
        if (!stream.frames_usable || frame.width != stream.width || frame.height != stream.height ||
            frame.pitch != stream.video_format.pitch || frame.bytes.size() > stream.frame_bytes) {
            stream.frames_rejected++;
        } else {
            const std::uint32_t buffer = stream.frames[stream.next_frame];
            stream.next_frame = (stream.next_frame + 1) % static_cast<std::uint32_t>(stream.frames.size());
            const auto copy_begin = std::chrono::steady_clock::now();
            std::memcpy(base + buffer, frame.bytes.data(), frame.bytes.size());
            const auto copy_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copy_begin)
                            .count());
            stream.frame_copy_ns += copy_ns;
            stream.frame_copy_max_ns = std::max(stream.frame_copy_max_ns, copy_ns);
            result = VideoFrameResult{buffer, stream.session_id, ++stream.video_tag, frame.pts};
        }
    }
    checkStreamEnd(player_address, *player, stream);
    return result;
}

std::uint32_t takeSoundFrame(std::uint8_t* base, std::uint32_t player_address, std::uint32_t frames,
                             SoundFrameResult& out) {
    Player* player = findPlayer(player_address);
    if (player == nullptr || !player->stream) return sail::kErrorInvalidState;
    Stream& stream = *player->stream;
    if (frames == 0 || frames > kMaxSoundFrames) return sail::kErrorInvalidArg;
    if (!stream.running || stream.scratch == 0 || stream.pcm_frames == 0) {
        return static_cast<std::uint32_t>(sail::kSyncNoFrame);
    }
    const std::uint32_t buffer = stream.scratch + kScratchPcm;
    const std::uint32_t bytes = frames * stream.audio_frame_bytes;
    // Checked before the queue is touched, so a refused call does not consume PCM.
    if (frames > stream.pcm_frames || !guestMapped(buffer, bytes)) {
        return static_cast<std::uint32_t>(sail::kSyncNoFrame);
    }
    // The block is already in the game's format (interleaved big-endian float32) and padded with silence
    // when the decoder fell behind or failed; the sizes can only differ if the player were reconfigured.
    const MovieAudioBlock block = stream.movie.takeAudio(frames);
    if (block.bytes.size() == bytes) {
        std::memcpy(base + buffer, block.bytes.data(), bytes);
    } else {
        std::memset(base + buffer, 0, bytes);
    }
    out.buffer = buffer;
    out.session_id = stream.session_id;
    out.tag = ++stream.sound_tag;
    out.pts = block.pts;
    checkStreamEnd(player_address, *player, stream);
    return static_cast<std::uint32_t>(sail::kSyncNextOnTime);
}

}  // namespace sail_hle

}  // namespace tox2

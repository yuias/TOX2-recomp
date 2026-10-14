// HLE handlers of the cellSysutil callback pump and the BGM playback status. Behaviour follows
// RPCS3 `Emu/Cell/Modules/cellSysutil.cpp` (function of the same name). Handlers run under the
// kernel lock, so the tables need no mutex, except the ones marked kernel_free (cellSysutilGetBgmPlaybackStatus,
// cellVideoOutGetResolution, cellVideoOutSetGamma): they touch only guest memory, constants and the atomic BGM state,
// and log through the self-synchronised log.
//
// Deviations from RPCS3:
//  - The queue has no abort handles (RPCS3 uses them to drop queued savedata/dialog callbacks of a
//    closed object); nothing here needs them yet.
//  - CELL_SYSUTIL_REQUEST_EXITGAME does not arm RPCS3's termination watchdog.
//  - The BGM state is the status struct only; no system BGM is played, so the player state stays
//    "stop" and the fade ratio 0.
// Video and audio out (the imports below the callback pump) and cellVideoOutSetGamma of the
// cellSysutilAvconfExt module are served from here too; see the notes at their handlers.
// RPCS3 sends no system events on its own at startup; the first ones come from the HLE modules that
// emulate the system software (message dialog, save data, trophy installation, ...).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <format>
#include <span>
#include <utility>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellSysutil";
constexpr char kAvconfExtModule[] = "cellSysutilAvconfExt";

constexpr std::uint32_t kSlotCount = 4;

// CellSysutilBgmPlaybackStatus: u8 playerState, u8 enableState, char contentId[16],
// u8 currentFadeRatio, char reserved[13].
constexpr std::uint32_t kBgmStatusSize = 32;
constexpr std::uint8_t kBgmPlayerStop = 1;            // CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP
constexpr std::uint8_t kBgmEnable = 0;                // CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE
constexpr std::uint8_t kBgmDisable = 1;               // CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE
constexpr std::uint32_t kBgmErrorParam = 0x8002b101;  // CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM

struct Slot {
    std::uint32_t func = 0;  // OPD of void(u64 status, u64 param, void* userdata); 0 = free
    std::uint32_t userdata = 0;
};

std::array<Slot, kSlotCount> g_slots;
std::deque<SysutilCall> g_queue;
bool g_draw_started = false;
// Read by the kernel-free getBgmPlaybackStatus, so every access goes through a relaxed atomic_ref.
std::uint8_t g_bgm_enable_state = kBgmDisable;

void setBgmEnableState(std::uint8_t state) {
    std::atomic_ref(g_bgm_enable_state).store(state, std::memory_order_relaxed);
}

// cellSysutilRegisterCallback(u32 slot, CellSysutilCallback func, void* userdata)
void registerCallback(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t slot = argU32(ctx, 0);
    if (slot >= kSlotCount) {
        ctx->gpr[3] = kSysutilErrorValue;
        return;
    }
    g_slots[slot] = {argU32(ctx, 1), argU32(ctx, 2)};
    logInfo("tox2: sysutil: callback slot {} = {:#010x} (userdata {:#x})", slot, g_slots[slot].func,
            g_slots[slot].userdata);
    ctx->gpr[3] = kCellOk;
}

// cellSysutilUnregisterCallback(u32 slot)
void unregisterCallback(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t slot = argU32(ctx, 0);
    if (slot >= kSlotCount) {
        ctx->gpr[3] = kSysutilErrorValue;
        return;
    }
    g_slots[slot] = {};
    logInfo("tox2: sysutil: callback slot {} cleared", slot);
    ctx->gpr[3] = kCellOk;
}

// cellSysutilCheckCallback()
void checkCallback(Tox2PpuContext* ctx, std::uint8_t* base) {
    // Take the whole queue first: what a callback queues runs on the next check, as in RPCS3's
    // pop_all.
    std::deque<SysutilCall> batch;
    batch.swap(g_queue);
    for (const SysutilCall& call : batch) {
        call(ctx, base);
    }
    ctx->gpr[3] = kCellOk;
}

// cellSysutilEnableBgmPlayback()
void enableBgmPlayback(Tox2PpuContext* ctx, std::uint8_t*) {
    setBgmEnableState(kBgmEnable);
    ctx->gpr[3] = kCellOk;
}

// cellSysutilDisableBgmPlayback()
void disableBgmPlayback(Tox2PpuContext* ctx, std::uint8_t*) {
    setBgmEnableState(kBgmDisable);
    ctx->gpr[3] = kCellOk;
}

// cellSysutilGetBgmPlaybackStatus(CellSysutilBgmPlaybackStatus* status)
void getBgmPlaybackStatus(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t status_addr = argU32(ctx, 0);
    if (status_addr == 0) {
        ctx->gpr[3] = kBgmErrorParam;
        return;
    }
    for (std::uint32_t i = 0; i < kBgmStatusSize; i++) tox2Write8(base, status_addr + i, 0);
    tox2Write8(base, status_addr, kBgmPlayerStop);
    tox2Write8(base, status_addr + 1, std::atomic_ref(g_bgm_enable_state).load(std::memory_order_relaxed));
    ctx->gpr[3] = kCellOk;
}

// Video out. Behaviour follows RPCS3 `Emu/Cell/Modules/cellVideoOut.cpp`; layouts are
// CellVideoOutState {u8 state, u8 colorSpace, u8 reserved[6], CellVideoOutDisplayMode} and
// CellVideoOutConfiguration {u8 resolutionId, u8 format, u8 aspect, u8 reserved[9], u32 pitch}, with
// CellVideoOutDisplayMode {u8 resolutionId, scanMode, conversion, aspect, u8 reserved[2],
// u16 refreshRates}. The only deviation: RPCS3 derives availability from its user setting, here the
// game's PARAM.SFO RESOLUTION flags (bit 0 480, 1 576, 2 720, 3 1080, 4 480 16:9, 5 576 16:9) do.
constexpr std::uint32_t kVideoOutError = 0x8002b220;
constexpr std::uint32_t kVideoErrorIllegalConfiguration = kVideoOutError + 1;
constexpr std::uint32_t kVideoErrorIllegalParameter = kVideoOutError + 2;
constexpr std::uint32_t kVideoErrorOutOfRange = kVideoOutError + 3;
constexpr std::uint32_t kVideoErrorDeviceNotFound = kVideoOutError + 4;
constexpr std::uint32_t kVideoErrorUnsupportedVideoOut = kVideoOutError + 5;
constexpr std::uint32_t kVideoErrorUnsupportedDisplayMode = kVideoOutError + 6;

constexpr std::uint32_t kVideoOutPrimary = 0;
constexpr std::uint32_t kVideoOutSecondary = 1;
constexpr std::uint8_t kResolution1080 = 1, kResolution720 = 2, kResolution480 = 4, kResolution576 = 5;
constexpr std::uint8_t kAspectAuto = 0, kAspect4x3 = 1, kAspect16x9 = 2;
constexpr std::uint8_t kScanProgressive = 1;
constexpr std::uint8_t kColorSpaceRgb = 1;
constexpr std::uint16_t kRefreshRate5994 = 1;
constexpr std::uint32_t kSfoRes480 = 0x01, kSfoRes576 = 0x02, kSfoRes720 = 0x04, kSfoRes1080 = 0x08,
                        kSfoRes480Wide = 0x10, kSfoRes576Wide = 0x20;
constexpr std::uint32_t kVideoStateSize = 16;

struct VideoMode {
    std::uint8_t resolution_id = kResolution720;
    std::uint8_t aspect = kAspect16x9;
    std::uint8_t format = 0;
    std::uint32_t pitch = 0;
};

VideoMode g_video_mode;
bool g_video_configured = false;

std::uint32_t sfoFlags(const char* key) {
    const auto it = gameParamSfo().ints.find(key);
    return it == gameParamSfo().ints.end() ? 0x1 : it->second;
}

// The mode before the game configures one: 720p when the SFO allows it, else 480.
VideoMode defaultVideoMode() {
    const std::uint32_t flags = sfoFlags("RESOLUTION");
    VideoMode mode;
    if (flags & kSfoRes720) {
        mode.resolution_id = kResolution720;
        mode.aspect = kAspect16x9;
        mode.pitch = 1280 * 4;
    } else {
        mode.resolution_id = kResolution480;
        mode.aspect = (flags & kSfoRes480) || !(flags & kSfoRes480Wide) ? kAspect4x3 : kAspect16x9;
        mode.pitch = 720 * 4;
    }
    return mode;
}

// Ids that RPCS3's _IntGetResolutionInfo knows and that need no stereo support, with their pixel
// size. cellVideoOutConfigure accepts exactly these and cellVideoOutGetResolution reports them.
struct ResolutionSize {
    std::uint8_t id;
    std::uint16_t width, height;
};
constexpr ResolutionSize kResolutionSizes[] = {
        {kResolution1080, 1920, 1080},
        {kResolution720, 1280, 720},
        {kResolution480, 720, 480},
        {kResolution576, 720, 576},
        {0x0a, 1600, 1080},
        {0x0b, 1440, 1080},
        {0x0c, 1280, 1080},
        {0x0d, 960, 1080},
        {0x64, 1280, 768},
};

const ResolutionSize* findResolution(std::uint32_t id) {
    for (const ResolutionSize& size : kResolutionSizes) {
        if (size.id == id) return &size;
    }
    return nullptr;
}

bool knownResolution(std::uint8_t id) { return findResolution(id) != nullptr; }

bool sfoAllows(std::uint32_t resolution_id, std::uint32_t aspect) {
    const std::uint32_t flags = sfoFlags("RESOLUTION");
    const bool want_4x3 = aspect == kAspectAuto || aspect == kAspect4x3;
    const bool want_16x9 = aspect == kAspectAuto || aspect == kAspect16x9;
    switch (resolution_id) {
        case kResolution1080: return want_16x9 && (flags & kSfoRes1080);
        case kResolution720: return want_16x9 && (flags & kSfoRes720);
        case kResolution480: return (want_4x3 && (flags & kSfoRes480)) || (want_16x9 && (flags & kSfoRes480Wide));
        case kResolution576: return (want_4x3 && (flags & kSfoRes576)) || (want_16x9 && (flags & kSfoRes576Wide));
        default: return false;
    }
}

// cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state)
void videoOutGetState(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t video_out = argU32(ctx, 0);
    const std::uint32_t device_index = argU32(ctx, 1);
    const std::uint32_t state = argU32(ctx, 2);
    if (state == 0) {
        ctx->gpr[3] = kVideoErrorIllegalParameter;
        return;
    }
    // RPCS3 turns the unsupported-output error of its device count into DEVICE_NOT_FOUND here, and
    // the secondary output has no device, so only (0, 0) gets a state.
    if (video_out != kVideoOutPrimary || device_index != 0) {
        ctx->gpr[3] = kVideoErrorDeviceNotFound;
        return;
    }
    const VideoMode mode = g_video_configured ? g_video_mode : defaultVideoMode();
    for (std::uint32_t i = 0; i < kVideoStateSize; i++) tox2Write8(base, state + i, 0);
    tox2Write8(base, state + 0, 0);  // CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED
    tox2Write8(base, state + 1, kColorSpaceRgb);
    tox2Write8(base, state + 8, mode.resolution_id);
    tox2Write8(base, state + 9, kScanProgressive);
    tox2Write8(base, state + 10, 0);  // CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE
    tox2Write8(base, state + 11, mode.aspect);
    tox2Write16(base, state + 14, kRefreshRate5994);
    ctx->gpr[3] = kCellOk;
}

// cellVideoOutGetResolutionAvailability(u32 videoOut, u32 resolutionId, u32 aspect, u32 option)
// returns a boolean in r3.
void videoOutGetResolutionAvailability(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t video_out = argU32(ctx, 0);
    const std::uint32_t resolution_id = argU32(ctx, 1);
    const std::uint32_t aspect = argU32(ctx, 2);
    if (video_out == kVideoOutSecondary) {
        ctx->gpr[3] = 0;
        return;
    }
    if (video_out != kVideoOutPrimary) {
        ctx->gpr[3] = kVideoErrorUnsupportedVideoOut;
        return;
    }
    const bool available = sfoAllows(resolution_id, aspect);
    logOnce(std::format("videoout-avail-{}-{}", resolution_id, aspect),
            "tox2: videoout: availability of resolution {:#x} aspect {}: {}", resolution_id, aspect, available ? 1 : 0);
    ctx->gpr[3] = available ? 1 : 0;
}

// cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution): {u16 width, u16 height}.
// It describes the id, not the configured mode, and ignores the PARAM.SFO flags.
void videoOutGetResolution(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t resolution = argU32(ctx, 1);
    const ResolutionSize* size = findResolution(argU32(ctx, 0));
    if (resolution == 0 || size == nullptr) {
        ctx->gpr[3] = kVideoErrorIllegalParameter;
        return;
    }
    tox2Write16(base, resolution, size->width);
    tox2Write16(base, resolution + 2, size->height);
    ctx->gpr[3] = kCellOk;
}

// cellVideoOutSetGamma(u32 videoOut, float gamma): the SDK range is 0.8 to 1.8. The gamma is not applied,
// because the renderer presents the frame as it is.
void videoOutSetGamma(Tox2PpuContext* ctx, std::uint8_t*) {
    if (argU32(ctx, 0) != kVideoOutPrimary) {
        ctx->gpr[3] = kVideoErrorUnsupportedVideoOut;
        return;
    }
    const double gamma = ctx->fpr[1].f64;
    if (!(gamma >= 0.8 && gamma <= 1.8)) {
        ctx->gpr[3] = kVideoErrorIllegalParameter;
        return;
    }
    logOnce("videoout-gamma", "tox2: videoout: gamma {} ignored", gamma);
    ctx->gpr[3] = kCellOk;
}

// cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config, CellVideoOutOption* option,
//                       u32 waitForEvent)
void videoOutConfigure(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t config = argU32(ctx, 1);
    if (config == 0) {
        ctx->gpr[3] = kVideoErrorIllegalParameter;
        return;
    }
    const std::uint8_t resolution_id = tox2Read8(base, config);
    const std::uint8_t format = tox2Read8(base, config + 1);
    const std::uint8_t aspect = tox2Read8(base, config + 2);
    const std::uint32_t pitch = tox2Read32(base, config + 12);
    if (pitch == 0) {
        ctx->gpr[3] = kVideoErrorOutOfRange;
        return;
    }
    if (resolution_id == 0x92 || resolution_id == 0xa1) {
        ctx->gpr[3] = kVideoErrorIllegalConfiguration;
        return;
    }
    if (!knownResolution(resolution_id)) {
        logError("tox2: videoout: unsupported resolution {:#x} requested", resolution_id);
        ctx->gpr[3] = kVideoErrorUnsupportedDisplayMode;
        return;
    }
    VideoMode mode;
    mode.resolution_id = resolution_id;
    mode.format = format;
    mode.pitch = pitch;
    // AUTO and unknown aspects resolve to the display's own (16:9, RPCS3's default setting).
    mode.aspect = aspect == kAspect4x3 || aspect == kAspect16x9 ? aspect : kAspect16x9;
    g_video_mode = mode;
    g_video_configured = true;
    logInfo("tox2: videoout: configured resolution {:#x} aspect {}=>{} format {:#x} pitch {:#x}", resolution_id, aspect,
            mode.aspect, format, pitch);
    ctx->gpr[3] = kCellOk;
}

// Audio out. Behaviour follows RPCS3 `Emu/Cell/Modules/cellAudioOut.cpp` (function of the same name);
// layouts are CellAudioOutState {u8 state, u8 encoder, u8 reserved[6], u32 downMixer,
// CellAudioOutSoundMode {u8 type, u8 channel, u8 fs, u8 reserved, u32 layout}} and
// CellAudioOutConfiguration {u8 channel, u8 encoder, u8 reserved[10], u32 downMixer}. RPCS3 always
// offers LPCM 2 ch 48 kHz first and adds more modes only with its own surround setting, so the
// PARAM.SFO SOUND_FORMAT flags do not change the modes. The runtime mixes to stereo, so that is the
// only mode; kSoundModes is the one list that the state, the availability and the configuration
// share. Which values cellAudioOutConfigure refuses is the runtime's own reading of the SDK, not a
// copy of RPCS3.
constexpr std::uint32_t kAudioErrorIllegalParameter = 0x8002b242;
constexpr std::uint32_t kAudioErrorOutOfRange = 0x8002b243;
constexpr std::uint32_t kAudioErrorUnsupportedAudioOut = 0x8002b245;
constexpr std::uint32_t kAudioErrorUnsupportedSoundMode = 0x8002b246;
constexpr std::uint32_t kAudioStateSize = 20;
constexpr std::uint32_t kAudioConfigSize = 16;
constexpr std::uint8_t kAudioCodingLpcm = 0;
constexpr std::uint8_t kAudioFs48k = 0x04;
constexpr std::uint32_t kAudioLayout2ch = 0x00000001;
constexpr std::uint32_t kAudioDownMixerTypeB = 2;  // NONE = 0, TYPE_A = 1

struct SoundMode {
    std::uint8_t type, channel, fs;
    std::uint32_t layout;
};
constexpr SoundMode kSoundModes[] = {{kAudioCodingLpcm, 2, kAudioFs48k, kAudioLayout2ch}};

struct AudioOutSetting {
    std::uint8_t channel = kSoundModes[0].channel;
    std::uint8_t encoder = kAudioCodingLpcm;
    std::uint32_t down_mixer = 0;
};

AudioOutSetting g_audio_out;

const SoundMode* findSoundMode(std::uint8_t channel) {
    for (const SoundMode& mode : kSoundModes) {
        if (mode.channel == channel) return &mode;
    }
    return nullptr;
}

// cellAudioOutGetState(u32 audioOut, u32 deviceIndex, CellAudioOutState* state)
void audioOutGetState(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t audio_out = argU32(ctx, 0);
    const std::uint32_t device_index = argU32(ctx, 1);
    const std::uint32_t state = argU32(ctx, 2);
    if (state == 0) {
        ctx->gpr[3] = kAudioErrorIllegalParameter;
        return;
    }
    if (audio_out > 1) {
        ctx->gpr[3] = kAudioErrorIllegalParameter;
        return;
    }
    const std::uint32_t device_count = audio_out == 0 ? 1 : 0;
    for (std::uint32_t i = 0; i < kAudioStateSize; i++) tox2Write8(base, state + i, 0);
    if (device_index >= device_count) {
        if (audio_out == 1) {
            // RPCS3 returns what it saw on hardware: uninitialised stack data that was constant.
            tox2Write8(base, state, 0x10);
            tox2Write32(base, state + 16, 0xD00C1680);
            ctx->gpr[3] = kCellOk;
        } else {
            ctx->gpr[3] = kAudioErrorOutOfRange;
        }
        return;
    }
    const SoundMode& mode = *findSoundMode(g_audio_out.channel);
    tox2Write8(base, state + 0, 0);  // CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED
    tox2Write8(base, state + 1, g_audio_out.encoder);
    tox2Write32(base, state + 8, g_audio_out.down_mixer);
    tox2Write8(base, state + 12, mode.type);
    tox2Write8(base, state + 13, mode.channel);
    tox2Write8(base, state + 14, mode.fs);
    tox2Write32(base, state + 16, mode.layout);
    ctx->gpr[3] = kCellOk;
}

// cellAudioOutGetConfiguration(u32 audioOut, CellAudioOutConfiguration* config, CellAudioOutOption* option)
void audioOutGetConfiguration(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t audio_out = argU32(ctx, 0);
    const std::uint32_t config = argU32(ctx, 1);
    if (config == 0 || audio_out > 1) {
        ctx->gpr[3] = kAudioErrorIllegalParameter;
        return;
    }
    for (std::uint32_t i = 0; i < kAudioConfigSize; i++) tox2Write8(base, config + i, 0);
    // The secondary output has no device, so its configuration stays empty.
    if (audio_out == 0) {
        tox2Write8(base, config + 0, g_audio_out.channel);
        tox2Write8(base, config + 1, g_audio_out.encoder);
        tox2Write32(base, config + 12, g_audio_out.down_mixer);
    }
    ctx->gpr[3] = kCellOk;
}

// cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option): the largest channel count
// among the modes with that coding type and sampling rate, in r3 (0 when none). This is the runtime's
// reading of the SDK ("number of channels available"), not a copy of RPCS3.
void audioOutGetSoundAvailability(Tox2PpuContext* ctx, std::uint8_t*) {
    const std::uint32_t audio_out = argU32(ctx, 0);
    const std::uint32_t type = argU32(ctx, 1);
    const std::uint32_t fs = argU32(ctx, 2);
    if (audio_out > 1) {
        ctx->gpr[3] = kAudioErrorUnsupportedAudioOut;
        return;
    }
    std::uint32_t available = 0;
    if (audio_out == 0) {
        for (const SoundMode& mode : kSoundModes) {
            if (mode.type == type && mode.fs == fs) available = std::max<std::uint32_t>(available, mode.channel);
        }
    }
    logOnce(std::format("audioout-avail-{}-{}", type, fs), "tox2: audioout: availability of type {} fs {:#x}: {}", type,
            fs, available);
    ctx->gpr[3] = available;
}

// cellAudioOutConfigure(u32 audioOut, CellAudioOutConfiguration* config, CellAudioOutOption* option,
//                       u32 waitForEvent). A channel count of 0 keeps the current one. Nothing is
// stored when any field is refused. The mixer does not use the setting; GetState and GetConfiguration
// report it back.
void audioOutConfigure(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t audio_out = argU32(ctx, 0);
    const std::uint32_t config = argU32(ctx, 1);
    if (config == 0 || audio_out > 1) {
        ctx->gpr[3] = kAudioErrorIllegalParameter;
        return;
    }
    if (audio_out == 1) {
        ctx->gpr[3] = kCellOk;
        return;
    }
    AudioOutSetting setting = g_audio_out;
    const std::uint8_t channel = tox2Read8(base, config + 0);
    const std::uint8_t encoder = tox2Read8(base, config + 1);
    const std::uint32_t down_mixer = tox2Read32(base, config + 12);
    if (channel != 0) setting.channel = channel;
    if (findSoundMode(setting.channel) == nullptr || encoder != kAudioCodingLpcm) {
        logError("tox2: audioout: unsupported sound mode requested: {} ch, coding type {}", channel, encoder);
        ctx->gpr[3] = kAudioErrorUnsupportedSoundMode;
        return;
    }
    if (down_mixer > kAudioDownMixerTypeB) {
        ctx->gpr[3] = kAudioErrorIllegalParameter;
        return;
    }
    setting.encoder = encoder;
    setting.down_mixer = down_mixer;
    g_audio_out = setting;
    logInfo("tox2: audioout: configured {} ch, coding type {}, down mixer {}", setting.channel, setting.encoder,
            setting.down_mixer);
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x02ff3c1b, "cellSysutilUnregisterCallback", unregisterCallback},
        {kModule, 0x0bae8772, "cellVideoOutConfigure", videoOutConfigure},
        {kModule, 0x189a74da, "cellSysutilCheckCallback", checkCallback},
        {kModule, 0x220894e3, "cellSysutilEnableBgmPlayback", enableBgmPlayback},
        {kModule, 0x4692ab35, "cellAudioOutConfigure", audioOutConfigure},
        {kModule, 0x887572d5, "cellVideoOutGetState", videoOutGetState},
        {kModule, 0x9d98afa0, "cellSysutilRegisterCallback", registerCallback},
        {kModule, 0xa11552f6, "cellSysutilGetBgmPlaybackStatus", getBgmPlaybackStatus, true},
        {kModule, 0xa322db75, "cellVideoOutGetResolutionAvailability", videoOutGetResolutionAvailability},
        {kModule, 0xc01b4e7c, "cellAudioOutGetSoundAvailability", audioOutGetSoundAvailability},
        {kAvconfExtModule, 0xc7020f62, "cellVideoOutSetGamma", videoOutSetGamma, true},
        {kModule, 0xcfdd8e87, "cellSysutilDisableBgmPlayback", disableBgmPlayback},
        {kModule, 0xe558748d, "cellVideoOutGetResolution", videoOutGetResolution, true},
        {kModule, 0xed5d96af, "cellAudioOutGetConfiguration", audioOutGetConfiguration},
        {kModule, 0xf4e3caa0, "cellAudioOutGetState", audioOutGetState},
};

}  // namespace

std::span<const HleEntry> cellSysutilHandlers() { return kHandlers; }

void queueSysutilCall(SysutilCall call) { g_queue.push_back(std::move(call)); }

std::int32_t queueSysutilEvent(std::uint64_t status, std::uint64_t param) {
    if (status == kSysutilDrawingBegin) {
        if (g_draw_started) {
            logError("tox2: sysutil: a second DRAWING_BEGIN was queued");
            return static_cast<std::int32_t>(kSysutilErrorBusy);
        }
        g_draw_started = true;
    } else if (status == kSysutilDrawingEnd) {
        if (!g_draw_started) {
            logError("tox2: sysutil: DRAWING_END was queued without a DRAWING_BEGIN");
            return -1;
        }
        g_draw_started = false;
    }

    // The registered slots are captured now; a callback registered later does not see this event.
    std::int32_t count = 0;
    for (const Slot slot : g_slots) {
        if (slot.func == 0) continue;
        g_queue.push_back([slot, status, param](Tox2PpuContext* ctx, std::uint8_t* base) {
            callGuestFunction(ctx, base, slot.func, {status, param, slot.userdata});
        });
        count++;
    }
    return count;
}

void resetCellSysutil() {
    g_slots = {};
    g_queue.clear();
    g_draw_started = false;
    setBgmEnableState(kBgmDisable);
    g_video_mode = {};
    g_video_configured = false;
    g_audio_out = {};
}

}  // namespace tox2

#pragma once

// Guest-visible layouts, enums and error codes of cellSail (and the few cellPamf values the game
// uses). Guest memory is big-endian: the constants below are byte offsets and sizes for
// tox2Read*/tox2Write*, not host structs.
//
// The layouts follow the RPCS3 header `rpcs3/Emu/Cell/Modules/cellSail.h` (struct definitions and
// the enums and error codes next to them) and `rpcs3/Emu/Cell/Modules/cellPamf.h` (`CellPamfReader`,
// stream types and error codes). Offsets are the natural-alignment offsets of those field lists;
// RPCS3 asserts only upper bounds for most of these structs; where the game's own code clears or
// copies a whole struct, the comment says so.

#include <cstdint>

namespace tox2::sail {

// Error codes: `CellSailError` in cellSail.h.
inline constexpr std::uint32_t kErrorInvalidArg = 0x80610701;
inline constexpr std::uint32_t kErrorInvalidState = 0x80610702;
inline constexpr std::uint32_t kErrorUnsupportedStream = 0x80610703;
inline constexpr std::uint32_t kErrorIndexOutOfRange = 0x80610704;
inline constexpr std::uint32_t kErrorEmpty = 0x80610705;
inline constexpr std::uint32_t kErrorFulled = 0x80610706;
inline constexpr std::uint32_t kErrorUsing = 0x80610707;
inline constexpr std::uint32_t kErrorNotAvailable = 0x80610708;
inline constexpr std::uint32_t kErrorCancel = 0x80610709;
inline constexpr std::uint32_t kErrorMemory = 0x806107F0;
inline constexpr std::uint32_t kErrorInvalidFd = 0x806107F1;
inline constexpr std::uint32_t kErrorFatal = 0x806107FF;

// Event majors delivered to the player's notify callback (`CELL_SAIL_EVENT_*` in cellSail.h; the
// event value is `(major << 32) | minor`). Major 0 and majors >= 12 are invalid: the game reports
// them as errors, so they must never be emitted.
inline constexpr std::uint32_t kEventErrorOccured = 1, kEventPlayerCallCompleted = 2, kEventPlayerStateChanged = 3,
                               kEventStreamOpened = 4, kEventStreamClosed = 5, kEventSessionStarted = 6,
                               kEventPauseStateChanged = 7, kEventSourceEos = 8, kEventEsOpened = 9,
                               kEventEsClosed = 10, kEventMediaStateChanged = 11;

constexpr std::uint64_t makeEvent(std::uint32_t major, std::uint32_t minor) {
    return (static_cast<std::uint64_t>(major) << 32) | minor;
}

// `CELL_SAIL_PLAYER_STATE_*`. The game ends a movie when the state it last saw is not Running.
enum class PlayerState : std::uint32_t {
    Initialized,
    BootTransition,
    Closed,
    OpenTransition,
    Opened,
    StartTransition,
    Running,
    StopTransition,
    CloseTransition,
    Lost
};

// `CELL_SAIL_PLAYER_CALL_*`: the minor code of PLAYER_CALL_COMPLETED.
inline constexpr std::uint32_t kCallNone = 0, kCallBoot = 1, kCallOpenStream = 2, kCallCloseStream = 3,
                               kCallOpenEsAudio = 4, kCallOpenEsVideo = 5, kCallOpenEsUser = 6, kCallCloseEsAudio = 7,
                               kCallCloseEsVideo = 8, kCallCloseEsUser = 9, kCallStart = 10, kCallStop = 11,
                               kCallNext = 12, kCallReopenEsAudio = 13, kCallReopenEsVideo = 14, kCallReopenEsUser = 15;

// `CELL_SAIL_PLAYER_PRESET_*` (the game passes AvSyncAutoDetect).
inline constexpr std::int32_t kPresetAvSync = 0, kPresetAsIs = 1, kPresetAvSync5994Hz = 2, kPresetAvSync2997Hz = 3,
                              kPresetAvSync50Hz = 4, kPresetAvSync25Hz = 5, kPresetAvSyncAutoDetect = 6;

// `CELL_SAIL_STREAM_*` (CreateDescriptor's stream type).
inline constexpr std::int32_t kStreamPamf = 0, kStreamMp4 = 1, kStreamAvi = 2, kStreamUnspecified = -1;

// Frame status words returned by GraphicsAdapterGetFrame and SoundAdapterGetFrame
// (`CELL_SAIL_SYNC_*`): frame status in bits 8.., sync status in bits 2..5. The game consumes a
// frame only when the result is >= kSyncFrameConsumedMin; CELL_OK means "no frame".
inline constexpr std::int32_t kSyncNoFrame = 0, kSyncRepeated = 1 << 8, kSyncNext = 2 << 8, kSyncSkippedOne = 3 << 8;
inline constexpr std::int32_t kSyncOnTime = 1 << 2;
inline constexpr std::int32_t kSyncFrameConsumedMin = kSyncRepeated;
inline constexpr std::int32_t kSyncNextOnTime = kSyncNext | kSyncOnTime;  // 0x204

// Video format fields (`CELL_SAIL_VIDEO_*`). "Unspecified" is -1 in every field (the game fills
// its preferred format with 0xff bytes except the coding and alpha).
inline constexpr std::int32_t kVideoUnspecified = -1;
inline constexpr std::int32_t kVideoCodingArgbInterleaved = 0, kVideoCodingRgbaInterleaved = 1,
                              kVideoCodingYuv422UY0VY1 = 2, kVideoCodingYuv420Planar = 3;
inline constexpr std::int32_t kVideoColorMatrixBt601 = 0, kVideoColorMatrixBt709 = 1;
inline constexpr std::int32_t kVideoScanProgressive = 0, kVideoScanInterlace = 1;
inline constexpr std::int32_t kVideoFrameRate24000Over1001 = 0, kVideoFrameRate24 = 1, kVideoFrameRate25 = 2,
                              kVideoFrameRate30000Over1001 = 3, kVideoFrameRate30 = 4, kVideoFrameRate50 = 5,
                              kVideoFrameRate60000Over1001 = 6, kVideoFrameRate60 = 7;
inline constexpr std::int32_t kVideoAspectRatio1To1 = 1;  // 1920x1080 and 1280x720
// Note the order: limited is 1 and full is 0.
inline constexpr std::int32_t kVideoColorRangeFull = 0, kVideoColorRangeLimited = 1;

// Audio format fields (`CELL_SAIL_AUDIO_*`).
inline constexpr std::int32_t kAudioUnspecified = -1;
inline constexpr std::int32_t kAudioCodingLpcmFloat32 = 1;
inline constexpr std::int32_t kAudioChLayout2ChLr = 2;

// CellSailVideoFormat, 32 bytes. coding, scan, bitsPerColor, frameRate, colorMatrix, aspectRatio and
// colorRange are s8; width and height s16; pitch and alpha s32. The game reads width and height
// only (GraphicsAdapterGetFormat) and clears 0x20 bytes before the call.
inline constexpr std::uint32_t kVideoFormatCoding = 0, kVideoFormatScan = 1, kVideoFormatBitsPerColor = 2,
                               kVideoFormatFrameRate = 3, kVideoFormatWidth = 4, kVideoFormatHeight = 6,
                               kVideoFormatPitch = 8, kVideoFormatAlpha = 12, kVideoFormatColorMatrix = 16,
                               kVideoFormatAspectRatio = 17, kVideoFormatColorRange = 18, kVideoFormatReserved1 = 19,
                               kVideoFormatReserved2 = 20, kVideoFormatReserved3 = 24, kVideoFormatSize = 32;
static_assert(kVideoFormatSize == kVideoFormatReserved3 + 8);
static_assert(kVideoFormatHeight == kVideoFormatWidth + 2 && kVideoFormatPitch == kVideoFormatHeight + 2);

// CellSailAudioFormat, 24 bytes. coding and chNum are s8; sampleNum s16; fs, chLayout and
// reserved0 s32; reserved1 s64.
inline constexpr std::uint32_t kAudioFormatCoding = 0, kAudioFormatChNum = 1, kAudioFormatSampleNum = 2,
                               kAudioFormatFs = 4, kAudioFormatChLayout = 8, kAudioFormatReserved0 = 12,
                               kAudioFormatReserved1 = 16, kAudioFormatSize = 24;
static_assert(kAudioFormatSize == kAudioFormatReserved1 + 8);

// CellSailGraphicsFrameInfo and CellSailSoundFrameInfo (the same layout), 24 bytes: pBuffer,
// sessionId, tag u32, status s32, pts u64. The game reads pBuffer and tag of the video info and
// pBuffer, tag and status of the sound info.
inline constexpr std::uint32_t kFrameInfoBuffer = 0, kFrameInfoSessionId = 4, kFrameInfoTag = 8, kFrameInfoStatus = 12,
                               kFrameInfoPts = 16, kFrameInfoSize = 24;
static_assert(kFrameInfoSize == kFrameInfoPts + 8);

// CellSailMemAllocatorFuncs (8 bytes): pAlloc(pArg, boundary, size) -> pointer and
// pFree(pArg, boundary, pointer), both OPD addresses.
inline constexpr std::uint32_t kAllocatorFuncsAlloc = 0, kAllocatorFuncsFree = 4, kAllocatorFuncsSize = 8;
// CellSailMemAllocator (8 bytes). RPCS3's MemAllocatorInitialize takes two arguments; the game
// passes a third, pArg, which is stored here.
inline constexpr std::uint32_t kAllocatorCallbacks = 0, kAllocatorArg = 4, kAllocatorSize = 8;

// CellSailSoundAdapterFuncs (12 bytes): pMakeup(pArg), pCleanup(pArg),
// pFormatChanged(pArg, pFormat, sessionId).
inline constexpr std::uint32_t kSoundAdapterFuncsMakeup = 0, kSoundAdapterFuncsCleanup = 4,
                               kSoundAdapterFuncsFormatChanged = 8, kSoundAdapterFuncsSize = 12;

// CellSailGraphicsAdapterFuncs (20 bytes): pMakeup(pArg), pCleanup(pArg),
// pFormatChanged(pArg, pFormat, sessionId), pAlloc(pArg, size, num, ppFrame) -> s32 and
// pFree(pArg, num, ppFrame) -> s32.
inline constexpr std::uint32_t kGraphicsAdapterFuncsMakeup = 0, kGraphicsAdapterFuncsCleanup = 4,
                               kGraphicsAdapterFuncsFormatChanged = 8, kGraphicsAdapterFuncsAlloc = 12,
                               kGraphicsAdapterFuncsFree = 16, kGraphicsAdapterFuncsSize = 20;

// CellSailPlayerAttribute (32 bytes): preset s32, maxAudioStreamNum, maxVideoStreamNum,
// maxUserStreamNum, queueDepth u32, three reserved u32.
inline constexpr std::uint32_t kPlayerAttributePreset = 0, kPlayerAttributeMaxAudioStreams = 4,
                               kPlayerAttributeMaxVideoStreams = 8, kPlayerAttributeMaxUserStreams = 12,
                               kPlayerAttributeQueueDepth = 16, kPlayerAttributeSize = 32;
// CellSailPlayerResource (16 bytes): pSpurs, three reserved u32.
inline constexpr std::uint32_t kPlayerResourceSpurs = 0, kPlayerResourceSize = 16;

// CellSailStartCommand (40 bytes): startType, seekType, terminusType, flags, startArg, reserved
// u32, seekArg and terminusArg u64. The game passes 40 zero bytes.
inline constexpr std::uint32_t kStartCommandStartType = 0, kStartCommandSeekType = 4, kStartCommandTerminusType = 8,
                               kStartCommandFlags = 12, kStartCommandStartArg = 16, kStartCommandSeekArg = 24,
                               kStartCommandTerminusArg = 32, kStartCommandSize = 40;
static_assert(kStartCommandSize == kStartCommandTerminusArg + 8);

// Player callback: notify(pArg, event u64, arg0 u64, arg1 u64), void.
inline constexpr std::uint32_t kEventSize = 8;

// Upper bounds RPCS3 asserts for the guest objects (CHECK_MAX_SIZE in cellSail.h). Object state is
// kept host-side, keyed by the object's address, so nothing larger is ever written. The game's own
// slot for the player is 0x400 bytes; its adapter slots are 0x100.
inline constexpr std::uint32_t kPlayerMaxSize = 0x100, kSoundAdapterMaxSize = 0x100, kGraphicsAdapterMaxSize = 0x100,
                               kDescriptorMaxSize = 0x100;

// CellPamfReader (`cellPamf.h`): 128 bytes. headerSize u64 @0, dataSize u64 @8, attribute u32 @16,
// isPsmf u16 @20, version u16 @22, currentGroupingPeriodIndex @24, currentGroupIndex @28,
// currentStreamIndex @32, five header-related pointers @36..52, 18 reserved words up to 128. The
// database CreateDatabase fills has this size (the game passes 0x80).
inline constexpr std::uint32_t kPamfReaderSize = 128;

// Stream selectors of cellPamfReaderGetNumberOfSpecificStreams (`CellPamfStreamType`): the
// codec-specific types, the PSMF variants and two generic selectors.
inline constexpr std::int32_t kPamfStreamTypeAvc = 0, kPamfStreamTypeM2v = 1, kPamfStreamTypeAtrac3plus = 2,
                              kPamfStreamTypeLpcm = 3, kPamfStreamTypeAc3 = 4, kPamfStreamTypeUserData = 5,
                              kPamfStreamTypePsmfAvc = 6, kPamfStreamTypePsmfAtrac3plus = 7,
                              kPamfStreamTypePsmfLpcm = 8, kPamfStreamTypePsmfUserData = 9, kPamfStreamTypeVideo = 20,
                              kPamfStreamTypeAudio = 21, kPamfStreamTypeUnknown = 22;

// `CellPamfError`.
inline constexpr std::uint32_t kPamfErrorStreamNotFound = 0x80610501, kPamfErrorInvalidPamf = 0x80610502,
                               kPamfErrorInvalidArg = 0x80610503, kPamfErrorUnknownType = 0x80610504,
                               kPamfErrorUnsupportedVersion = 0x80610505, kPamfErrorUnknownStream = 0x80610506,
                               kPamfErrorEpNotFound = 0x80610507, kPamfErrorNotAvailable = 0x80610508;

}  // namespace tox2::sail

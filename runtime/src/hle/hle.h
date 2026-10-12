#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tox2/ppu_context.h"
#include "lv2/syscalls.h"

namespace tox2 {

// An HLE function, identified by the module that exports it and its FNID.
struct HleEntry {
    const char* module;
    uint32_t fnid;
    const char* name;
    GuestHandler handler;  // null: the default handler runs
};

// Handlers of the sysPrxForUser module.
std::span<const HleEntry> sysPrxForUserHandlers();

// String, memory and printf-family functions of the sysPrxForUser module, and the abort hook.
std::span<const HleEntry> sysPrxForUserLibcHandlers();

// Handlers of the cellSysmodule module.
std::span<const HleEntry> cellSysmoduleHandlers();

// Handlers of the cellGame module.
std::span<const HleEntry> cellGameHandlers();

// Forgets the outstanding cellGameDataCheck state. Intended for tests.
void resetCellGame();

// Handlers of the cellRtc imports: GetCurrentTick, ConvertUtcToLocalTime, SetTick and GetDayOfWeek.
std::span<const HleEntry> cellRtcHandlers();

// Replaces the host time zone as the offset (minutes east of UTC, daylight saving included) that
// cellRtcConvertUtcToLocalTime adds; nullopt restores the host time zone. Intended for tests.
void setRtcLocalOffsetMinutes(std::optional<std::int32_t> minutes);

// Handlers of the sceNp module.
std::span<const HleEntry> sceNpHandlers();

// Forgets the sceNp init state. Intended for tests.
void resetSceNp();

// Handlers of the sceNpTrophy module.
std::span<const HleEntry> sceNpTrophyHandlers();

// Forgets the trophy init state, contexts and handles. Intended for tests.
void resetSceNpTrophy();

// Handlers of the cellSysutil module that this file implements: the callback pump
// (Register/Unregister/CheckCallback) and the BGM playback status.
std::span<const HleEntry> cellSysutilHandlers();

// Status codes passed to cellSysutil callbacks (CELL_SYSUTIL_*) and the module's error codes.
inline constexpr std::uint64_t kSysutilRequestExitGame = 0x0101;
inline constexpr std::uint64_t kSysutilDrawingBegin = 0x0121;
inline constexpr std::uint64_t kSysutilDrawingEnd = 0x0122;
inline constexpr std::uint64_t kSysutilSystemMenuOpen = 0x0131;
inline constexpr std::uint64_t kSysutilSystemMenuClose = 0x0132;
inline constexpr std::uint64_t kSysutilBgmPlaybackPlay = 0x0141;
inline constexpr std::uint64_t kSysutilBgmPlaybackStop = 0x0142;
inline constexpr std::uint32_t kSysutilErrorValue = 0x8002b102;  // CELL_SYSUTIL_ERROR_VALUE
inline constexpr std::uint32_t kSysutilErrorBusy = 0x8002b105;   // CELL_SYSUTIL_ERROR_BUSY

// A unit of work that runs on the guest thread that calls cellSysutilCheckCallback, with that
// thread's context, and may call guest functions through callGuestFunction.
using SysutilCall = std::function<void(Tox2PpuContext*, std::uint8_t*)>;

// Queues `call` for the next cellSysutilCheckCallback (RPCS3 sysutil_register_cb). Call under the
// guest lock.
void queueSysutilCall(SysutilCall call);

// Queues func(status, param, userdata) for every callback registered right now, in slot order
// (RPCS3 sysutil_send_system_cmd). Returns how many were queued; DRAWING_BEGIN while one is open
// returns kSysutilErrorBusy and DRAWING_END without one returns -1, queueing nothing. Call under
// the guest lock.
std::int32_t queueSysutilEvent(std::uint64_t status, std::uint64_t param);

// Forgets the registered callbacks, the queue and the BGM state. Intended for tests.
void resetCellSysutil();

// Handlers of the cellMsgDialog imports (cellSysutil module): Open2, Close and the progress bar
// calls.
std::span<const HleEntry> cellMsgDialogHandlers();

// Sets what an auto-answered Yes/No dialog replies: "yes", "ok" (same as yes) or "no". Returns false
// for any other value. OK dialogs always answer OK; dialogs without buttons wait for
// cellMsgDialogClose.
bool setDialogAnswer(std::string_view answer);

// Delay before a dialog with buttons answers itself (default 200 ms).
void setDialogAnswerDelay(std::chrono::milliseconds delay);

// Forgets the open dialog and restores the answer and delay defaults. Intended for tests.
void resetCellMsgDialog();

// Handlers of the cellSaveData imports (cellSysutil module): AutoLoad2, AutoSave2, ListLoad2,
// ListSave2 and Delete2.
std::span<const HleEntry> cellSaveDataHandlers();

// Forgets the busy flag, the NOREPEAT error-dialog memory and the callback scratch block. Intended
// for tests; a test that reserves guest memory anew must call it.
void resetCellSaveData();

// Handlers of the sys_fs module (cellFs*).
std::span<const HleEntry> cellFsHandlers();

// Handlers of the cellPad imports (sys_io module).
std::span<const HleEntry> cellPadHandlers();

// Index of each word in CellPadData::button, as far as a standard pad fills it.
inline constexpr std::size_t kPadButtonDigital1 = 2;  // select, L3, R3, start, d-pad
inline constexpr std::size_t kPadButtonDigital2 = 3;  // L2, R2, L1, R1, triangle, circle, cross, square
inline constexpr std::size_t kPadButtonAnalogRightX = 4;
inline constexpr std::size_t kPadButtonAnalogRightY = 5;
inline constexpr std::size_t kPadButtonAnalogLeftX = 6;
inline constexpr std::size_t kPadButtonAnalogLeftY = 7;
inline constexpr std::size_t kPadButtonPressureFirst = 8;  // 12 pressure words, 8..19
inline constexpr std::size_t kPadButtonSensorFirst = 20;   // 4 sensor words, 20..23
inline constexpr std::size_t kPadButtonCount = 24;

// A pad port as a host input backend sees it right now.
struct PadPortState {
    bool connected = false;
    std::uint32_t device_capability = 0;                  // CELL_PAD_CAPABILITY_* bits
    std::uint32_t device_type = 0;                        // CELL_PAD_DEV_TYPE_*
    std::array<std::uint16_t, kPadButtonCount> button{};  // host order; words 0 and 1 are unused
};

// Source of pad input. cellPad calls it under the guest lock, so an implementation must return
// without blocking (read an atomic snapshot that its own input thread updates). Ports are 0..6.
class PadProvider {
public:
    virtual ~PadProvider() = default;

    // Current state of `port`. Return connected = false for a port without a pad.
    virtual PadPortState poll(std::uint32_t port) = 0;

    // cellPadSetActDirect: motor values 0..255 (the small motor is on/off in hardware). Optional.
    virtual void setVibration(std::uint32_t port, std::uint8_t small_motor, std::uint8_t large_motor) {
        (void)port;
        (void)small_motor;
        (void)large_motor;
    }
};

// Neutral standard pad: sticks centred, no button pressed, sensors at rest.
inline constexpr std::uint32_t kPadCapabilityStandard = 0x1f;  // conformity, pressure, sensor, HP stick, actuator
PadPortState neutralPadState();

// Replaces the pad input source (null: no pad connected on any port). Call before the guest runs
// or under the guest lock.
void setPadProvider(std::unique_ptr<PadProvider> provider);

// Selects a built-in provider by name: "none" (no pad connected) or "neutral" (port 0 holds a
// standard pad that never changes). Returns false for any other name.
bool setPadBackend(std::string_view name);

// Forgets the init state and restores the default provider. Intended for tests.
void resetCellPad();

// Handlers of the cellPngDec imports: Create, Destroy, ExtOpen, ExtReadHeader, ExtSetParameter,
// ExtDecodeData and Close.
std::span<const HleEntry> cellPngDecHandlers();

// Writes every image cellPngDec decodes to `<dir>/png_<n>.png` (re-encoded as 8-bit RGBA), for
// inspecting what the guest was given. nullopt (the default) writes nothing.
void setPngDumpDirectory(std::optional<std::filesystem::path> dir);

// Forgets the open handles and streams and the dump counter. Intended for tests.
void resetCellPngDec();

// Handlers of the cellSail module that build and run the player: the allocator, the player's init,
// boot, open, start and finalize calls, the descriptor calls and the descriptor's database and
// capabilities.
std::span<const HleEntry> cellSailPlayerHandlers();

// Handlers of the cellSail graphics and sound adapters (Initialize, Finalize, preferred and current
// format, GetFrame, UpdateAvSync).
std::span<const HleEntry> cellSailAdapterHandlers();

// Forgets every cellSail player, descriptor and adapter without calling into the guest. Intended for
// tests.
void resetCellSail();

// Handlers of the cellPamf module: cellPamfReaderGetNumberOfSpecificStreams.
std::span<const HleEntry> cellPamfHandlers();

// Handlers of the cellAudio module: Init, Quit, the port calls, GetPortConfig, GetPortBlockTag,
// GetPortTimestamp and the notify queue registration. Init starts the audio engine.
std::span<const HleEntry> cellAudioHandlers();

// Forgets the cellAudio state (ports, registrations, counters) without touching guest memory or the
// audio engine. Intended for tests.
void resetCellAudio();

// Handlers of the cellAtrac module: all 15 imports. Decode runs the decoder of the installed
// AtracDecoderFactory; without a decoder, or for a frame it rejects, it outputs silence of the right
// length.
std::span<const HleEntry> cellAtracHandlers();

// Totals over every handle since start-up (or the last resetCellAtrac). Safe to read from any thread.
struct CellAtracStats {
    std::uint64_t decoders = 0;         // CreateDecoder calls that succeeded
    std::uint64_t frames = 0;           // ATRAC3plus frames decoded
    std::uint64_t non_silent = 0;       // of those, frames with audible content
    std::uint64_t errors = 0;           // frames the decoder rejected
    std::uint64_t without_decoder = 0;  // frames passed through with no decoder
    std::uint64_t starved = 0;          // Decode calls that returned no samples for lack of buffered data
};
CellAtracStats cellAtracStats();

// Forgets every cellAtrac handle and the totals without touching guest memory. Intended for tests.
void resetCellAtrac();

// Handlers of the cellGcmSys module.
std::span<const HleEntry> cellGcmSysHandlers();

// The body of _cellGcmInitBody with a selectable local memory size (the import always passes the
// console's 0xF900000). Commits the RSX areas, maps the IO area and writes the context, control
// register and configuration. Returns kCellOk or a CellGcmError code. Intended for tests; the
// state is released with resetRsxState.
std::uint32_t gcmInitBody(std::uint8_t* base, std::uint32_t context_pp, std::uint32_t cmd_size, std::uint32_t io_size,
                          std::uint32_t io_address, std::uint32_t local_size);

// Function exports of a module whose code runs recompiled. An import of the main executable with the
// same (module, fnid) calls the export's descriptor instead of a handler.
struct GuestExports {
    const char* image;  // census tag and log name, e.g. "libsre"
    const Tox2ExportInfo* exports;
    std::uint32_t count;
};

// Main executable: (module, fnid) found in `guest` -> bound to that export; else a registered handler;
// else the default handler. A name that differs between the registry and the import table is a
// warning. Fatal when guestExportConflicts(guest) is not empty, or when an export's descriptor names
// an address tox2Lookup does not know. Replaces any earlier installation, including the module
// tables. Logs "tox2: hle: <n> imports, <h> with handlers", followed by ", <b> bound to <image>" for
// each entry of `guest`. Call before any guest thread runs.
void installImports(const Tox2ImportInfo* imports, uint32_t count, std::span<const GuestExports> guest = {});

// A module's own import table, used by tox2HleCallImport. Handlers or the default handler only. A
// second call with the same `image` replaces its table. Logs "tox2: hle: <image>: <n> imports, <h> with
// handlers". `imports` must stay alive: tox2HleCallImport finds the table by the pointer it is given.
void installModuleImports(const char* image, const Tox2ImportInfo* imports, uint32_t count);

// "module::name (fnid 0x…)" for every (module, fnid) that is both exported by `guest` and registered
// in a handler list, ordered by (module, fnid).
std::vector<std::string> guestExportConflicts(std::span<const GuestExports> guest);

// Silences the per-call log of the default handler (census counting stays on).
void setQuietHle(bool quiet);

// Calls the function described by the guest function descriptor at `opd_addr` (entry address and
// TOC) and returns when it does. lr and r2 are saved and restored.
void callGuestOpd(Tox2PpuContext* ctx, uint8_t* base, uint32_t opd_addr);

// Calls a guest function through its OPD with up to 8 arguments in r3..r10 and returns r3.
// Saves and restores lr, r2 and r1; lowers r1 by a 0x70-byte frame (16-aligned, back chain stored)
// so the callee's LR/CR saves cannot touch the HLE caller's frame.
std::uint64_t callGuestFunction(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t opd_addr,
                                std::initializer_list<std::uint64_t> args);

// Low 32 bits of argument register r(3 + n).
inline std::uint32_t argU32(const Tox2PpuContext* ctx, int n) { return static_cast<std::uint32_t>(ctx->gpr[3 + n]); }

// Reads a NUL-terminated guest string of at most `max` bytes (the terminator is not required).
std::string readGuestString(const std::uint8_t* base, std::uint32_t addr, std::uint32_t max);

}  // namespace tox2

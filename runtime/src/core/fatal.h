#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "tox2/ppu_context.h"

namespace tox2 {

enum class ExitCode : int {
    ProcessExit = 0,
    Usage = 2,
    Fatal = 3,
    LoadError = 4,
    Watchdog = 5,
    Stall = 6,
    NoThreads = 7,
    // The run reached something this runtime deliberately does not do yet.
    Unsupported = 8,
};

enum class FatalKind {
    UnhandledInstruction,
    Trap,
    GuestFault,
    UnknownTarget,
    Internal,
};

std::string_view fatalKindName(FatalKind kind);

// One caller frame recovered from the guest stack. `function` is 0 when no function owns `lr`.
struct GuestFrame {
    std::uint32_t lr;
    std::uint32_t function;
};

inline constexpr std::size_t kMaxGuestFrames = 16;

// Entry of the function that owns `addr`: the greatest registered entry <= addr in the span (one per
// image) that holds `addr`. Functions have no recorded size, so an address in padding or in
// unregistered code maps to the preceding function of its span. Returns 0 when `addr` is outside
// every span (a span ends at its last entry's slot, so addresses inside the last function's body are
// not found).
std::uint32_t findOwningFunction(std::uint32_t addr);

// Walks the PPC64 ELF back chain starting at ctx.gpr[1] and stores the saved LR of each caller
// frame (`*(back_chain + 16)`). Stops on a null, misaligned, non-ascending or non-committed link, or
// after `max_frames`. Guest memory is only read after checking that it is committed stack, so a
// corrupt chain cannot fault. Returns the number of frames written.
std::size_t walkGuestStack(const Tox2PpuContext& ctx, const std::uint8_t* base, GuestFrame* out,
                           std::size_t max_frames);

// Registered by the census code so this file does not depend on it. Called exactly once, on the
// way out of every stop path. Without a printer nothing is printed.
using CensusPrinterFn = void (*)();
void setCensusPrinter(CensusPrinterFn fn);

// Runs on the stopping thread on every stop path (fatal, stopProcess, supervisor verdicts), in registration
// order and before the census, so it sees the guest memory as the run left it. It must not block and must
// not call a stop function. Returns false when kMaxStopHooks hooks are already set. Call it before threads start.
using StopHookFn = void (*)();
inline constexpr std::size_t kMaxStopHooks = 8;
bool addStopHook(StopHookFn fn);

// Context of the guest thread running on the calling host thread (nullptr on other threads);
// fatal() dumps its registers and trace ring. The thread layer sets it when a guest thread starts.
void setFatalContext(const Tox2PpuContext* ctx);

// What fatal() prints for a thread that is not a PPU guest thread (an SPU thread): `subject` is whatever
// was passed to setFatalDump. The text may end in a newline. Takes precedence over the PPU context.
using FatalDumpFn = std::string (*)(const void* subject);

// Sets the dump of the calling host thread; a null function clears it. The SPU layer sets it where its host
// thread starts, so a fatal stop raised anywhere on that thread (for example a guest fault inside an MFC
// transfer) prints the SPU registers instead of a PPU dump.
void setFatalDump(FatalDumpFn fn, const void* subject);

// Logs the stop line, the registers and trace ring of the current guest thread (or its setFatalDump dump) and
// the census, then ends the process with ExitCode::Fatal. Only the first caller proceeds; others block forever.
[[noreturn]] void fatal(FatalKind kind, std::string_view message);

// Like fatal() without the register dump. `line` is the complete stop line, e.g.
// "tox2: stop: watchdog 20 s".
[[noreturn]] void stopProcess(ExitCode code, std::string_view line);

// Keeps failures from raising modal dialogs (CRT abort/assert/report boxes, critical-error and
// fault boxes) so that a headless run always ends with an exit code. Call first thing in main().
void installProcessSettings();

// Directory of the running executable; empty if Windows cannot report it.
std::filesystem::path executableDirectory();

}  // namespace tox2

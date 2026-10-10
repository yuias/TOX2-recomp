#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

// Entry of the function that owns `addr`: the greatest registered entry <= addr. Functions have no
// recorded size, so an address in padding or in unregistered code maps to the preceding function.
// Returns 0 when `addr` is outside the registered span (which ends at the last entry's slot, so
// addresses inside the last function's body are not found).
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

// Context of the guest thread running on the calling host thread (nullptr on other threads);
// fatal() dumps its registers and trace ring. The thread layer sets it when a guest thread starts.
void setFatalContext(const Tox2PpuContext* ctx);

// Logs the stop line, the registers and trace ring of the current guest thread, and the census,
// then ends the process with ExitCode::Fatal. Only the first caller proceeds; others block forever.
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

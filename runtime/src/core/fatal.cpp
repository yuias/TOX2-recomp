#include "core/fatal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#include "core/guest_memory.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>

namespace tox2 {
namespace {

std::atomic<CensusPrinterFn> g_census_printer{nullptr};
std::atomic<bool> g_stopping{false};
thread_local const Tox2PpuContext* t_fatal_context = nullptr;
thread_local FatalDumpFn t_fatal_dump = nullptr;
thread_local const void* t_fatal_subject = nullptr;
thread_local bool t_in_stop = false;

// Takes the single right to end the process. A second host thread parks until the first one
// calls _Exit. A thread that faults again while stopping must not wait for itself.
void enterStop() {
    if (t_in_stop) {
        std::_Exit(static_cast<int>(ExitCode::Fatal));
    }
    t_in_stop = true;
    if (g_stopping.exchange(true)) {
        for (;;) {
            Sleep(INFINITE);
        }
    }
}

[[noreturn]] void finishStop(ExitCode code) {
    if (CensusPrinterFn printer = g_census_printer.load()) {
        printer();
    }
    std::fflush(stdout);
    flushLog();
    std::fflush(stderr);
    std::_Exit(static_cast<int>(code));
}

// True when [addr, addr + 8) is 8-byte aligned, inside the stack region and committed readable
// host memory. An aligned 8-byte range never crosses a page, so one query is enough.
bool readableStackWord(const std::uint8_t* base, std::uint64_t addr) {
    if ((addr & 7) != 0 || addr < kStackRegion.begin || addr + 8 > kStackRegion.end) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(base + addr, &info, sizeof info) != sizeof info || info.State != MEM_COMMIT) {
        return false;
    }
    return (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

void dumpContext(const Tox2PpuContext& ctx) {
    logInfo("registers:");
    for (int row = 0; row < 32; row += 4) {
        logInfo("  r{:<2} {:016x}  r{:<2} {:016x}  r{:<2} {:016x}  r{:<2} {:016x}", row, ctx.gpr[row], row + 1,
                ctx.gpr[row + 1], row + 2, ctx.gpr[row + 2], row + 3, ctx.gpr[row + 3]);
    }
    logInfo("  lr {:016x}  ctr {:016x}  cr {:08x}  xer {:08x}", ctx.lr, ctx.ctr, tox2Mfcr(&ctx),
            static_cast<std::uint32_t>(tox2Mfxer(&ctx)));

    const std::uint32_t count = ctx.trace_pos < kTox2TraceSize ? ctx.trace_pos : kTox2TraceSize;
    logInfo("trace (oldest -> newest, {} entries):", count);
    std::string line;
    for (std::uint32_t i = 0; i < count; i++) {
        // Entry i of the logical sequence ending at trace_pos - 1.
        const std::uint32_t slot = (ctx.trace_pos - count + i) & (kTox2TraceSize - 1);
        line += std::format(" {:08x}", ctx.trace[slot]);
        if ((i & 7) == 7 || i + 1 == count) {
            logInfo("{}", line);
            line.clear();
        }
    }

    // The trace ring only holds function entries; the stack gives the call sites. The live LR is
    // frame 0 (it is the return address when the stop happens in a leaf or an HLE call).
    logInfo("guest stack:");
    logInfo("  frame 0: lr {:#010x} in fn {:#010x}", static_cast<std::uint32_t>(ctx.lr),
            findOwningFunction(static_cast<std::uint32_t>(ctx.lr)));
    GuestFrame frames[kMaxGuestFrames];
    const std::size_t n = walkGuestStack(ctx, process().memory.base(), frames, kMaxGuestFrames);
    for (std::size_t i = 0; i < n; i++) {
        logInfo("  frame {}: lr {:#010x} in fn {:#010x}", i + 1, frames[i].lr, frames[i].function);
    }
}

}  // namespace

std::size_t walkGuestStack(const Tox2PpuContext& ctx, const std::uint8_t* base, GuestFrame* out,
                           std::size_t max_frames) {
    if (base == nullptr) {
        return 0;
    }
    std::uint64_t sp = ctx.gpr[1];
    std::size_t count = 0;
    while (count < max_frames && readableStackWord(base, sp)) {
        const std::uint64_t back_chain = tox2Read64(base, static_cast<std::uint32_t>(sp));
        // The stack grows down, so a valid chain strictly ascends; this also rules out cycles.
        if (back_chain <= sp || !readableStackWord(base, back_chain) || !readableStackWord(base, back_chain + 16)) {
            break;
        }
        const std::uint64_t lr = tox2Read64(base, static_cast<std::uint32_t>(back_chain + 16));
        if (lr == 0 || lr > 0xFFFFFFFFull) {
            break;
        }
        out[count++] = {static_cast<std::uint32_t>(lr), findOwningFunction(static_cast<std::uint32_t>(lr))};
        sp = back_chain;
    }
    return count;
}

std::string_view fatalKindName(FatalKind kind) {
    switch (kind) {
        case FatalKind::UnhandledInstruction: return "unhandled instruction";
        case FatalKind::Trap: return "trap";
        case FatalKind::GuestFault: return "guest fault";
        case FatalKind::UnknownTarget: return "unknown target";
        case FatalKind::Internal: break;
    }
    return "internal error";
}

void setCensusPrinter(CensusPrinterFn fn) { g_census_printer.store(fn); }

void setFatalContext(const Tox2PpuContext* ctx) { t_fatal_context = ctx; }

void setFatalDump(FatalDumpFn fn, const void* subject) {
    t_fatal_dump = fn;
    t_fatal_subject = subject;
}

void fatal(FatalKind kind, std::string_view message) {
    enterStop();
    logInfo("tox2: stop: fatal {}: {}", fatalKindName(kind), message);
    if (t_fatal_dump != nullptr) {
        std::string text = t_fatal_dump(t_fatal_subject);
        // The logger adds its own newline.
        while (!text.empty() && text.back() == '\n') text.pop_back();
        logInfo("{}", text);
    } else if (t_fatal_context != nullptr) {
        dumpContext(*t_fatal_context);
    }
    finishStop(ExitCode::Fatal);
}

void stopProcess(ExitCode code, std::string_view line) {
    enterStop();
    logInfo("{}", line);
    finishStop(code);
}

void installProcessSettings() {
    // abort() would otherwise show the debug CRT dialog or hand the process to Windows Error
    // Reporting; with both off it just terminates.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    // The error-mode flags also cover faults in the loader and unhandled exceptions, which would
    // otherwise raise the "has stopped working" box.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
}

std::filesystem::path executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace tox2

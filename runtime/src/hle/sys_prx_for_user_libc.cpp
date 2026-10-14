// sysPrxForUser string, memory and printf functions, and the abort hook libsre calls after its own
// diagnostics. Strings are read straight from guest memory, so a bad pointer ends in the guest fault
// handler like it would on the console. Handlers run under the kernel lock, except the ones marked kernel_free, which
// touch only guest memory, the commit map and logging (own mutexes). Printing to stdout and abort stay under the lock.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/guest_printf.h"
#include "hle/hle.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "sysPrxForUser";

// The longest string abort() prints; libsre passes short literals.
constexpr std::uint32_t kAbortTextMax = 1024;

std::uint64_t signExtend32(std::int32_t value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

std::uint32_t guestStrlen(const std::uint8_t* base, std::uint32_t s) {
    return static_cast<std::uint32_t>(std::strlen(reinterpret_cast<const char*>(base + s)));
}

// _sys_strlen(s)
void sysStrlen(Tox2PpuContext* ctx, std::uint8_t* base) { ctx->gpr[3] = guestStrlen(base, argU32(ctx, 0)); }

// _sys_strcpy(dst, src): returns dst.
void sysStrcpy(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    const std::uint32_t src = argU32(ctx, 1);
    std::memmove(base + dst, base + src, guestStrlen(base, src) + 1);
    ctx->gpr[3] = dst;
}

// _sys_strcat(dst, src): returns dst.
void sysStrcat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    const std::uint32_t src = argU32(ctx, 1);
    std::memmove(base + dst + guestStrlen(base, dst), base + src, guestStrlen(base, src) + 1);
    ctx->gpr[3] = dst;
}

// _sys_strncpy(dst, src, n): copies up to the NUL and pads with NULs up to n.
void sysStrncpy(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    const std::uint32_t src = argU32(ctx, 1);
    const std::uint32_t n = argU32(ctx, 2);
    std::uint32_t i = 0;
    for (; i < n && base[src + i] != 0; i++) base[dst + i] = base[src + i];
    for (; i < n; i++) base[dst + i] = 0;
    ctx->gpr[3] = dst;
}

// _sys_strncat(dst, src, n): appends at most n characters, then a NUL.
void sysStrncat(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    const std::uint32_t src = argU32(ctx, 1);
    const std::uint32_t n = argU32(ctx, 2);
    const std::uint32_t end = dst + guestStrlen(base, dst);
    std::uint32_t i = 0;
    for (; i < n && base[src + i] != 0; i++) base[end + i] = base[src + i];
    base[end + i] = 0;
    ctx->gpr[3] = dst;
}

// _sys_strncmp(s1, s2, n): difference of the first differing bytes as unsigned char.
void sysStrncmp(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t a = argU32(ctx, 0);
    const std::uint32_t b = argU32(ctx, 1);
    const std::uint32_t n = argU32(ctx, 2);
    std::int32_t diff = 0;
    for (std::uint32_t i = 0; i < n; i++) {
        diff = static_cast<std::int32_t>(base[a + i]) - static_cast<std::int32_t>(base[b + i]);
        if (diff != 0 || base[a + i] == 0) break;
    }
    ctx->gpr[3] = signExtend32(diff);
}

// _sys_memcmp(a, b, n): as strncmp without stopping at NUL.
void sysMemcmp(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t a = argU32(ctx, 0);
    const std::uint32_t b = argU32(ctx, 1);
    const std::uint32_t n = argU32(ctx, 2);
    std::int32_t diff = 0;
    for (std::uint32_t i = 0; i < n && diff == 0; i++) {
        diff = static_cast<std::int32_t>(base[a + i]) - static_cast<std::int32_t>(base[b + i]);
    }
    ctx->gpr[3] = signExtend32(diff);
}

// _sys_memset(dst, c, n): the low byte of c is stored.
void sysMemset(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    std::memset(base + dst, static_cast<int>(ctx->gpr[4] & 0xFF), argU32(ctx, 2));
    ctx->gpr[3] = dst;
}

// _sys_memcpy(dst, src, n): memmove semantics, because callers do not always keep the ranges apart.
void sysMemcpy(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t dst = argU32(ctx, 0);
    std::memmove(base + dst, base + argU32(ctx, 1), argU32(ctx, 2));
    ctx->gpr[3] = dst;
}

// Formats `fmt` from `args` and reports an unsupported conversion once per format string.
std::string formatChecked(const std::uint8_t* base, std::uint32_t fmt, GuestVarArgs& args) {
    std::string error;
    std::string text = formatGuestPrintf(base, fmt, args, error);
    if (!error.empty() && firstUse(std::format("printf:{:08x}", fmt))) {
        logWarn("tox2: printf: {} in \"{}\"", error, readGuestString(base, fmt, 256));
    }
    return text;
}

void writeStdout(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// Stores `text` into a guest buffer of `size` bytes like snprintf and returns the full length.
void storeSnprintf(Tox2PpuContext* ctx, std::uint8_t* base, std::uint32_t buf, std::uint32_t size,
                   const std::string& text) {
    if (size > 0) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(text.size(), size - 1));
        std::memcpy(base + buf, text.data(), count);
        base[buf + count] = 0;
    }
    ctx->gpr[3] = text.size();
}

// _sys_printf(fmt, ...)
void sysPrintf(Tox2PpuContext* ctx, std::uint8_t* base) {
    GuestVarArgs args = GuestVarArgs::fromRegisters(ctx, 1);
    const std::string text = formatChecked(base, argU32(ctx, 0), args);
    writeStdout(text);
    ctx->gpr[3] = text.size();
}

// _sys_vprintf(fmt, va)
void sysVprintf(Tox2PpuContext* ctx, std::uint8_t* base) {
    GuestVarArgs args = GuestVarArgs::fromVaList(argU32(ctx, 1));
    const std::string text = formatChecked(base, argU32(ctx, 0), args);
    writeStdout(text);
    ctx->gpr[3] = text.size();
}

// _sys_snprintf(buf, size, fmt, ...)
void sysSnprintf(Tox2PpuContext* ctx, std::uint8_t* base) {
    GuestVarArgs args = GuestVarArgs::fromRegisters(ctx, 3);
    const std::string text = formatChecked(base, argU32(ctx, 2), args);
    storeSnprintf(ctx, base, argU32(ctx, 0), argU32(ctx, 1), text);
}

// _sys_vsnprintf(buf, size, fmt, va)
void sysVsnprintf(Tox2PpuContext* ctx, std::uint8_t* base) {
    GuestVarArgs args = GuestVarArgs::fromVaList(argU32(ctx, 3));
    const std::string text = formatChecked(base, argU32(ctx, 2), args);
    storeSnprintf(ctx, base, argU32(ctx, 0), argU32(ctx, 1), text);
}

// 0x9fb6228e: libsre prints its own diagnostic with _sys_printf and then calls this with one plain
// string, so the string is written as is.
void sysAbort(Tox2PpuContext* ctx, std::uint8_t* base) {
    writeStdout(readGuestString(base, argU32(ctx, 0), kAbortTextMax));
    stopProcess(ExitCode::Fatal, std::format("tox2: stop: abort from {:#010x} (sysPrxForUser 0x9fb6228e)",
                                             static_cast<std::uint32_t>(ctx->lr)));
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x04e83d2c, "_sys_strncmp", sysStrncmp, true},
        {kModule, 0x052d29a6, "_sys_strcat", sysStrcat, true},
        {kModule, 0x0618936b, "_sys_vsnprintf", sysVsnprintf, true},
        {kModule, 0x06574237, "_sys_snprintf", sysSnprintf, true},
        {kModule, 0x2d36462b, "_sys_strlen", sysStrlen, true},
        {kModule, 0x68b9b011, "_sys_memset", sysMemset, true},
        {kModule, 0x6bf66ea7, "_sys_memcpy", sysMemcpy, true},
        {kModule, 0x996f7cf8, "_sys_strncat", sysStrncat, true},
        {kModule, 0x99c88692, "_sys_strcpy", sysStrcpy, true},
        {kModule, 0x9f04f7af, "_sys_printf", sysPrintf},
        {kModule, 0x9fb6228e, "abort", sysAbort},
        {kModule, 0xd3039d4d, "_sys_strncpy", sysStrncpy, true},
        {kModule, 0xfa7f693d, "_sys_vprintf", sysVprintf},
        {kModule, 0xfb5db080, "_sys_memcmp", sysMemcmp, true},
};

}  // namespace

std::span<const HleEntry> sysPrxForUserLibcHandlers() { return kHandlers; }

}  // namespace tox2

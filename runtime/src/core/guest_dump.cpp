#include "core/guest_dump.h"

#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <vector>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>

namespace tox2 {
namespace {

// Filled before any guest thread runs and only read by the stop hook afterwards.
std::vector<GuestDumpSpec> g_dumps;

bool parseNumber(std::string_view text, std::uint64_t& value) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return !text.empty() && ec == std::errc{} && end == text.data() + text.size();
}

std::string sha256Hex(const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data),
                                       static_cast<ULONG>(size), digest.data(), static_cast<ULONG>(digest.size()));
    if (status < 0) return {};
    std::string hex;
    for (const std::uint8_t byte : digest) hex += std::format("{:02x}", byte);
    return hex;
}

void writeDump(const GuestDumpSpec& dump) {
    const std::uint64_t end = static_cast<std::uint64_t>(dump.addr) + dump.size;
    const std::string range = std::format("{:#010x}..{:#010x} -> {}", dump.addr, end, dump.file);
    const GuestMemory& memory = process().memory;
    if (!memory.isCommitted(dump.addr, dump.size)) {
        logInfo("tox2: dump: {} not committed", range);
        return;
    }
    // A copy, so that the hash and the file describe the same bytes while other threads still run.
    const std::vector<std::uint8_t> bytes(memory.base() + dump.addr, memory.base() + end);

    std::ofstream file(dump.file, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        logInfo("tox2: dump: {} write failed", range);
        return;
    }

    std::string text;
    if (dump.size <= 16) {
        text = " bytes";
        for (const std::uint8_t byte : bytes) text += std::format(" {:02x}", byte);
    }
    logInfo("tox2: dump: {} sha256 {}{}", range, sha256Hex(bytes.data(), bytes.size()), text);
}

void writeDumps() {
    for (const GuestDumpSpec& dump : g_dumps) writeDump(dump);
}

}  // namespace

std::optional<GuestDumpSpec> parseGuestDumpSpec(std::string_view text, std::string& error) {
    const std::size_t first = text.find(':');
    const std::size_t second = first == std::string_view::npos ? first : text.find(':', first + 1);
    if (second == std::string_view::npos) {
        error = "expected ADDR:SIZE:FILE";
        return std::nullopt;
    }
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    if (!parseNumber(text.substr(0, first), addr) || addr > 0xFFFFFFFFull) {
        error = "ADDR must be a 32-bit number";
        return std::nullopt;
    }
    if (!parseNumber(text.substr(first + 1, second - first - 1), size) || size == 0 || addr + size > 0x100000000ull) {
        error = "SIZE must be at least 1 and the range must end within the 32-bit guest space";
        return std::nullopt;
    }
    const std::string_view file = text.substr(second + 1);
    if (file.empty()) {
        error = "FILE is empty";
        return std::nullopt;
    }
    return GuestDumpSpec{static_cast<std::uint32_t>(addr), static_cast<std::uint32_t>(size), std::string(file)};
}

bool addGuestDump(GuestDumpSpec spec) {
    if (g_dumps.empty() && !addStopHook(&writeDumps)) return false;
    g_dumps.push_back(std::move(spec));
    return true;
}

}  // namespace tox2

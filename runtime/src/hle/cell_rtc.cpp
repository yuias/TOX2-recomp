// HLE of the four cellRtc imports the game uses: GetCurrentTick, ConvertUtcToLocalTime, SetTick and
// GetDayOfWeek. The game only seeds a random number generator from the local tick and formats dates
// with SetTick and GetDayOfWeek.
// Behaviour follows RPCS3 `Emu/Cell/Modules/cellRtc.cpp` (function of the same name).
//
// A CellRtcTick is a big-endian u64 of microseconds since 0001-01-01; a CellRtcDateTime is 16 bytes
// (u16 year, month, day, hour, minute, second, u32 microsecond). Pointers are checked for being
// committed guest memory, which is what RPCS3's page-attribute query amounts to.
//
// cellRtcGetCurrentTick and cellRtcConvertUtcToLocalTime are kernel_free: they touch only guest memory, the commit map,
// host clocks and the atomic local-offset override.
//
// Deviations from RPCS3: there is no lv2 time-zone syscall in this runtime, so the local offset is
// read from the host time zone (the same source RPCS3 uses on Windows: minus the bias, minus the
// standard or daylight bias while in effect). The clock is the host UTC clock with no scaling or
// console time offset.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "core/guest_memory.h"
#include "tox2/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellRtc";

constexpr std::uint32_t kErrorInvalidPointer = 0x80010602;  // CELL_RTC_ERROR_INVALID_POINTER

constexpr std::uint32_t kTickSize = 8;
constexpr std::uint32_t kDateTimeSize = 16;
constexpr std::uint64_t kUnixEpochTicks = 62135596800000000ull;  // 1970-01-01 in ticks
constexpr std::uint64_t kTicksPerMinute = 60ull * 1000000ull;
constexpr std::uint64_t kTicksPerSecond = 1000000ull;
constexpr std::uint64_t kTicksPerDay = 86400ull * kTicksPerSecond;
constexpr std::uint64_t kDaysPerCycle = 146097;  // days in 400 Gregorian years

// Test hook for the time zone; atomic because the kernel-free handlers read it without the kernel lock.
constexpr std::int64_t kNoOffsetOverride = std::numeric_limits<std::int64_t>::min();
std::atomic<std::int64_t> g_local_offset_override{kNoOffsetOverride};

bool mapped(std::uint32_t addr, std::uint32_t size) { return addr != 0 && process().memory.isCommitted(addr, size); }

// Minutes east of UTC including the daylight saving offset in effect (RPCS3 timezone + summertime).
std::int32_t localOffsetMinutes() {
    const std::int64_t override_minutes = g_local_offset_override.load(std::memory_order_relaxed);
    if (override_minutes != kNoOffsetOverride) return static_cast<std::int32_t>(override_minutes);
    TIME_ZONE_INFORMATION info{};
    const DWORD state = GetTimeZoneInformation(&info);
    if (state == TIME_ZONE_ID_INVALID) return 0;
    LONG summertime = 0;
    if (state == TIME_ZONE_ID_STANDARD) summertime = -info.StandardBias;
    if (state == TIME_ZONE_ID_DAYLIGHT) summertime = -info.DaylightBias;
    return static_cast<std::int32_t>(-info.Bias + summertime);
}

bool isLeapYear(std::uint64_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

// cellRtcGetCurrentTick(CellRtcTick* pTick)
void rtcGetCurrentTick(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t tick_addr = argU32(ctx, 0);
    if (!mapped(tick_addr, kTickSize)) {
        ctx->gpr[3] = kErrorInvalidPointer;
        return;
    }
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
    tox2Write64(base, tick_addr, kUnixEpochTicks + static_cast<std::uint64_t>(micros));
    ctx->gpr[3] = kCellOk;
}

// cellRtcConvertUtcToLocalTime(const CellRtcTick* pUtc, CellRtcTick* pLocalTime)
void rtcConvertUtcToLocalTime(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t utc_addr = argU32(ctx, 0);
    const std::uint32_t local_addr = argU32(ctx, 1);
    if (!mapped(utc_addr, kTickSize) || !mapped(local_addr, kTickSize)) {
        ctx->gpr[3] = kErrorInvalidPointer;
        return;
    }
    // Unsigned wrap-around matches the 64-bit tick arithmetic of the console.
    const std::uint64_t delta =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(localOffsetMinutes())) * kTicksPerMinute;
    tox2Write64(base, local_addr, tox2Read64(base, utc_addr) + delta);
    ctx->gpr[3] = kCellOk;
}

// cellRtcSetTick(CellRtcDateTime* pTime, const CellRtcTick* pTick)
void rtcSetTick(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t time_addr = argU32(ctx, 0);
    const std::uint32_t tick_addr = argU32(ctx, 1);
    if (!mapped(time_addr, kDateTimeSize) || !mapped(tick_addr, kTickSize)) {
        ctx->gpr[3] = kErrorInvalidPointer;
        return;
    }
    const std::uint64_t tick = tox2Read64(base, tick_addr);

    std::uint64_t days = tick / kTicksPerDay;
    const std::uint64_t in_day = tick % kTicksPerDay;
    const std::uint64_t cycles = days / kDaysPerCycle;
    days %= kDaysPerCycle;

    // Walk the years of the 400-year cycle. The cycle starts at year 1 (not a leap year), so the
    // calendar year of index i is cycles * 400 + i + 1 and its leap rule only depends on i + 1.
    std::uint64_t year_in_cycle = 0;
    for (;; year_in_cycle++) {
        const std::uint64_t length = isLeapYear(year_in_cycle + 1) ? 366 : 365;
        if (days < length) break;
        days -= length;
    }
    const std::uint64_t year = cycles * 400 + year_in_cycle + 1;

    constexpr std::uint16_t kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::uint32_t month = 0;
    for (; month < 11; month++) {
        const std::uint64_t length = kMonthDays[month] + (month == 1 && isLeapYear(year) ? 1 : 0);
        if (days < length) break;
        days -= length;
    }

    tox2Write16(base, time_addr + 0, static_cast<std::uint16_t>(year));
    tox2Write16(base, time_addr + 2, static_cast<std::uint16_t>(month + 1));
    tox2Write16(base, time_addr + 4, static_cast<std::uint16_t>(days + 1));
    tox2Write16(base, time_addr + 6, static_cast<std::uint16_t>(in_day / (3600 * kTicksPerSecond)));
    tox2Write16(base, time_addr + 8, static_cast<std::uint16_t>(in_day / (60 * kTicksPerSecond) % 60));
    tox2Write16(base, time_addr + 10, static_cast<std::uint16_t>(in_day / kTicksPerSecond % 60));
    tox2Write32(base, time_addr + 12, static_cast<std::uint32_t>(in_day % kTicksPerSecond));
    ctx->gpr[3] = kCellOk;
}

// s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day): 0 = Sunday. The arguments are not validated.
void rtcGetDayOfWeek(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    auto year = static_cast<std::int32_t>(argU32(ctx, 0));
    auto month = static_cast<std::int32_t>(argU32(ctx, 1));
    const auto day = static_cast<std::int32_t>(argU32(ctx, 2));
    // January and February count as months 13 and 14 of the previous year.
    if (month < 3) {
        month += 12;
        year -= 1;
    }
    const std::int32_t weekday = ((13 * month + 8) / 5 + year + year / 4 - year / 100 + year / 400 + day) % 7;
    ctx->gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(weekday));
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x99b13034, "cellRtcSetTick", rtcSetTick},
        {kModule, 0x9dafc0d9, "cellRtcGetCurrentTick", rtcGetCurrentTick, true},
        {kModule, 0xc2d8cf95, "cellRtcGetDayOfWeek", rtcGetDayOfWeek},
        {kModule, 0xc48d5002, "cellRtcConvertUtcToLocalTime", rtcConvertUtcToLocalTime, true},
};

}  // namespace

std::span<const HleEntry> cellRtcHandlers() { return kHandlers; }

void setRtcLocalOffsetMinutes(std::optional<std::int32_t> minutes) {
    g_local_offset_override.store(minutes ? *minutes : kNoOffsetOverride, std::memory_order_relaxed);
}

}  // namespace tox2

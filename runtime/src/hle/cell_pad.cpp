// HLE handlers of the cellPad imports (sys_io module): Init, End, SetPortSetting, GetData, GetInfo2
// and SetActDirect. Behaviour follows RPCS3 `Emu/Cell/Modules/cellPad.cpp` (function of the same
// name): error order, the CellPadInfo2 and CellPadData layouts, and a GetData that reports len 0
// while the input has not changed. Handlers run under the guest lock, so the state needs no mutex.
//
// Input comes from a PadProvider (hle.h). The built-in providers are "none" (nothing connected) and
// "neutral" (port 0 holds a standard pad that never changes); a host backend (keyboard, XInput)
// plugs in through setPadProvider without touching this file.
//
// Deviations from RPCS3: a pad that is connected when cellPadInit runs is reported at once, where
// RPCS3 learns of it through a sys_config event on another thread; the SDK-version check that
// guards the reserved bytes of cellPadSetActDirect always applies (a title with a missing or old
// SDK version would skip it); cellPadSetActDirect treats the motor values as raw bytes.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace dal {
namespace {

constexpr char kModule[] = "sys_io";

constexpr std::uint32_t kErrorInvalidParameter = 0x80121102;
constexpr std::uint32_t kErrorAlreadyInitialized = 0x80121103;
constexpr std::uint32_t kErrorUninitialized = 0x80121104;
constexpr std::uint32_t kErrorNoDevice = 0x80121107;
constexpr std::uint32_t kErrorUnsupportedGamepad = 0x80121108;

constexpr std::uint32_t kMaxPads = 127;  // CELL_MAX_PADS: range of the port argument
constexpr std::uint32_t kMaxPorts = 7;   // CELL_PAD_MAX_PORT_NUM: ports with state

constexpr std::uint32_t kStatusConnected = 1u << 0;
constexpr std::uint32_t kStatusAssignChanges = 1u << 1;
constexpr std::uint32_t kSettingPressOn = 1u << 1;
constexpr std::uint32_t kSettingSensorOn = 1u << 2;
constexpr std::uint32_t kCapabilityActuator = 1u << 4;

constexpr std::uint32_t kLenNoChange = 0;
constexpr std::uint32_t kLenChangeDefault = 8;
constexpr std::uint32_t kLenChangePressOn = 20;
constexpr std::uint32_t kLenChangeSensorOn = 24;

// With sensors on, a pad reports even unchanged data this often (a DualShock 3 does).
constexpr std::chrono::milliseconds kSensorInterval{10};

// CellPadInfo2: max_connect, now_connect, system_info, then four u32[7] arrays.
constexpr std::uint32_t kInfo2NowConnect = 4;
constexpr std::uint32_t kInfo2SystemInfo = 8;
constexpr std::uint32_t kInfo2PortStatus = 12;
constexpr std::uint32_t kInfo2PortSetting = kInfo2PortStatus + 4 * kMaxPorts;
constexpr std::uint32_t kInfo2DeviceCapability = kInfo2PortSetting + 4 * kMaxPorts;
constexpr std::uint32_t kInfo2DeviceType = kInfo2DeviceCapability + 4 * kMaxPorts;
constexpr std::uint32_t kInfo2Size = kInfo2DeviceType + 4 * kMaxPorts;

// CellPadData: s32 len, then u16 button[].
constexpr std::uint32_t kDataButtons = 4;

// CellPadActParam: u8 motor[2], u8 reserved[6].
constexpr std::uint32_t kActReserved = 2;
constexpr std::uint32_t kActReservedCount = 6;

class NonePadProvider final : public PadProvider {
public:
    PadPortState poll(std::uint32_t) override { return {}; }
};

class NeutralPadProvider final : public PadProvider {
public:
    PadPortState poll(std::uint32_t port) override { return port == 0 ? neutralPadState() : PadPortState{}; }
};

std::unique_ptr<PadProvider> makeProvider(std::string_view name) {
    if (name == "none") return std::make_unique<NonePadProvider>();
    if (name == "neutral") return std::make_unique<NeutralPadProvider>();
    return nullptr;
}

// Used until --pad or setPadProvider says otherwise.
constexpr std::string_view kDefaultBackend = "neutral";

struct Port {
    std::uint32_t port_status = 0;  // what the game was told: kStatusConnected | kStatusAssignChanges
    std::uint32_t setting = 0;      // kSettingPressOn | kSettingSensorOn
    bool buffer_cleared = true;     // the next GetData reports everything again
    std::array<std::uint16_t, kPadButtonCount> output{};  // button words the game has seen
    std::chrono::steady_clock::time_point last_sensor_update{};
};

struct PadModule {
    std::uint32_t max_connect = 0;  // 0: not initialised
    std::array<Port, kMaxPorts> ports;
    std::unique_ptr<PadProvider> provider;
};

PadModule g_pad;

PadProvider& provider() {
    if (!g_pad.provider) g_pad.provider = makeProvider(kDefaultBackend);
    return *g_pad.provider;
}

std::uint32_t portCount() { return std::min(g_pad.max_connect, kMaxPorts); }

// Polls the provider and brings the status the game sees in line with it (a plug or unplug sets
// the assign-changed flag, which cellPadGetInfo2 reports once).
PadPortState pollPort(std::uint32_t port) {
    PadPortState state = provider().poll(port);
    Port& entry = g_pad.ports[port];
    const bool reported = (entry.port_status & kStatusConnected) != 0;
    if (state.connected != reported) {
        entry.port_status = state.connected ? (kStatusConnected | kStatusAssignChanges) : kStatusAssignChanges;
        entry.buffer_cleared = true;
        logOnce(std::format("pad-port-{}-{}", port, state.connected), "dal: pad: port {} {}", port,
                state.connected ? "connected" : "disconnected");
    }
    return state;
}

// cellPadInit(u32 max_connect)
void padInit(DalPpuContext* ctx, std::uint8_t*) {
    const std::uint32_t max_connect = argU32(ctx, 0);
    if (g_pad.max_connect != 0) {
        ctx->gpr[3] = kErrorAlreadyInitialized;
        return;
    }
    if (max_connect == 0 || max_connect > kMaxPads) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    g_pad.max_connect = max_connect;
    g_pad.ports = {};
    for (std::uint32_t port = 0; port < portCount(); port++) {
        pollPort(port);
    }
    logInfo("dal: pad: init max_connect {}", max_connect);
    ctx->gpr[3] = kCellOk;
}

// cellPadEnd()
void padEnd(DalPpuContext* ctx, std::uint8_t*) {
    if (g_pad.max_connect == 0) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    g_pad.max_connect = 0;
    ctx->gpr[3] = kCellOk;
}

// cellPadSetPortSetting(u32 port_no, u32 port_setting)
void padSetPortSetting(DalPpuContext* ctx, std::uint8_t*) {
    const std::uint32_t port = argU32(ctx, 0);
    const std::uint32_t setting = argU32(ctx, 1);
    if (g_pad.max_connect == 0) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    if (port >= kMaxPads) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    // Not NO_DEVICE for ports without state.
    if (port < kMaxPorts) {
        const std::uint32_t masked = setting & (kSettingPressOn | kSettingSensorOn);
        g_pad.ports[port].setting = masked;
        // Press mode decides whether pressure words reach the game, which matters to input providers.
        logOnce(std::format("pad-setting-{}-{:#x}", port, masked),
                "dal: pad: port {} setting {:#x} (press {}, sensor {})", port, masked,
                (masked & kSettingPressOn) != 0 ? "on" : "off", (masked & kSettingSensorOn) != 0 ? "on" : "off");
    }
    ctx->gpr[3] = kCellOk;
}

// cellPadGetData(u32 port_no, CellPadData* data)
void padGetData(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t port = argU32(ctx, 0);
    const std::uint32_t data = argU32(ctx, 1);
    if (g_pad.max_connect == 0) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    if (port >= kMaxPads || data == 0) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    if (port >= portCount()) {
        ctx->gpr[3] = kErrorNoDevice;
        return;
    }
    const PadPortState state = pollPort(port);
    Port& entry = g_pad.ports[port];
    if (!state.connected) {
        ctx->gpr[3] = kErrorNoDevice;  // data stays untouched
        return;
    }

    const bool press_on = (entry.setting & kSettingPressOn) != 0;
    const bool sensor_on = (entry.setting & kSettingSensorOn) != 0;

    // Sensor words only count as input while sensors are on; they are copied below either way.
    const std::size_t compared_end = sensor_on ? kPadButtonCount : kPadButtonSensorFirst;
    bool changed = false;
    for (std::size_t i = kPadButtonDigital1; i < compared_end; i++) {
        if (entry.output[i] != state.button[i]) {
            entry.output[i] = state.button[i];
            changed = true;
        }
    }

    std::uint32_t len = kLenNoChange;
    if (sensor_on) {
        const auto now = std::chrono::steady_clock::now();
        if (changed || entry.buffer_cleared || now - entry.last_sensor_update >= kSensorInterval) {
            len = kLenChangeSensorOn;
            entry.last_sensor_update = now;
        }
    } else if (changed || entry.buffer_cleared) {
        len = press_on ? kLenChangePressOn : kLenChangeDefault;
    }
    entry.buffer_cleared = false;

    dalWrite32(base, data, len);
    if (len == kLenNoChange) {
        ctx->gpr[3] = kCellOk;
        return;
    }

    const auto put = [&](std::size_t index, std::uint16_t value) {
        dalWrite16(base, data + kDataButtons + static_cast<std::uint32_t>(index) * 2, value);
    };
    put(0, 0);
    put(1, static_cast<std::uint16_t>((0x7 << 4) | std::min<std::uint32_t>(len / 2, 15)));
    for (std::size_t i = kPadButtonDigital1; i < kPadButtonPressureFirst; i++) put(i, state.button[i]);
    // The pressure area is cleared when the game did not ask for pressure data.
    for (std::size_t i = kPadButtonPressureFirst; i < kPadButtonSensorFirst; i++) {
        put(i, press_on ? state.button[i] : 0);
    }
    if (len == kLenChangeSensorOn) {
        for (std::size_t i = kPadButtonSensorFirst; i < kPadButtonCount; i++) put(i, state.button[i]);
    }
    ctx->gpr[3] = kCellOk;
}

// cellPadGetInfo2(CellPadInfo2* info)
void padGetInfo2(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t info = argU32(ctx, 0);
    if (g_pad.max_connect == 0) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    if (info == 0) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    std::fill_n(base + info, kInfo2Size, std::uint8_t{0});
    dalWrite32(base, info, portCount());  // clamped, unlike the value passed to cellPadInit
    dalWrite32(base, info + kInfo2SystemInfo, 0);

    std::uint32_t now_connect = 0;
    for (std::uint32_t port = 0; port < portCount(); port++) {
        const PadPortState state = pollPort(port);
        Port& entry = g_pad.ports[port];
        dalWrite32(base, info + kInfo2PortStatus + 4 * port, entry.port_status);
        dalWrite32(base, info + kInfo2PortSetting + 4 * port, entry.setting);
        entry.port_status &= ~kStatusAssignChanges;
        if (!state.connected) continue;
        dalWrite32(base, info + kInfo2DeviceCapability + 4 * port, state.device_capability);
        dalWrite32(base, info + kInfo2DeviceType + 4 * port, state.device_type);
        now_connect++;
    }
    dalWrite32(base, info + kInfo2NowConnect, now_connect);
    ctx->gpr[3] = kCellOk;
}

// cellPadSetActDirect(u32 port_no, CellPadActParam* param)
void padSetActDirect(DalPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t port = argU32(ctx, 0);
    const std::uint32_t param = argU32(ctx, 1);
    if (g_pad.max_connect == 0) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    if (port >= kMaxPads || param == 0) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    for (std::uint32_t i = 0; i < kActReservedCount; i++) {
        if (base[param + kActReserved + i] != 0) {
            ctx->gpr[3] = kErrorInvalidParameter;
            return;
        }
    }
    if (port >= portCount()) {
        ctx->gpr[3] = kErrorNoDevice;
        return;
    }
    const PadPortState state = pollPort(port);
    if (!state.connected) {
        ctx->gpr[3] = kErrorNoDevice;
        return;
    }
    if ((state.device_capability & kCapabilityActuator) == 0) {
        ctx->gpr[3] = kErrorUnsupportedGamepad;
        return;
    }
    // motor[0] is the small motor, motor[1] the large one.
    provider().setVibration(port, base[param], base[param + 1]);
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x1cf98800, "cellPadInit", padInit},
        {kModule, 0x4d9b75d5, "cellPadEnd", padEnd},
        {kModule, 0x578e3c98, "cellPadSetPortSetting", padSetPortSetting},
        {kModule, 0x8b72cda1, "cellPadGetData", padGetData},
        {kModule, 0xa703a51d, "cellPadGetInfo2", padGetInfo2},
        {kModule, 0xf65544ee, "cellPadSetActDirect", padSetActDirect},
};

}  // namespace

std::span<const HleEntry> cellPadHandlers() { return kHandlers; }

PadPortState neutralPadState() {
    PadPortState state;
    state.connected = true;
    state.device_capability = kPadCapabilityStandard;
    state.device_type = 0;  // CELL_PAD_DEV_TYPE_STANDARD
    for (std::size_t i = kPadButtonAnalogRightX; i <= kPadButtonAnalogLeftY; i++) state.button[i] = 0x80;
    // Sensor values of a DualShock 3 lying flat.
    state.button[kPadButtonSensorFirst + 0] = 512;
    state.button[kPadButtonSensorFirst + 1] = 399;
    state.button[kPadButtonSensorFirst + 2] = 512;
    state.button[kPadButtonSensorFirst + 3] = 512;
    return state;
}

void setPadProvider(std::unique_ptr<PadProvider> provider) {
    g_pad.provider = provider ? std::move(provider) : makeProvider("none");
    g_pad.ports = {};
}

bool setPadBackend(std::string_view name) {
    std::unique_ptr<PadProvider> made = makeProvider(name);
    if (!made) return false;
    setPadProvider(std::move(made));
    return true;
}

void resetCellPad() { g_pad = {}; }

}  // namespace dal

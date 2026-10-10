#include "hle/cell_audio_ports.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include "audio/audio_output.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

static_assert(kCellAudioBlockFrames == kAudioBlockFrames);

constexpr std::uint32_t kAllocationUnit = 0x10000;
constexpr std::uint32_t kBytesPerSample = 4;

// CellAudioPortParam and CellAudioPortConfig field offsets.
constexpr std::uint32_t kParamChannels = 0x00;
constexpr std::uint32_t kParamBlocks = 0x08;
constexpr std::uint32_t kParamAttr = 0x10;
constexpr std::uint32_t kParamLevel = 0x18;
constexpr std::uint32_t kParamSize = 0x20;
constexpr std::uint32_t kConfigReadIndexAddr = 0x00;
constexpr std::uint32_t kConfigStatus = 0x04;
constexpr std::uint32_t kConfigChannels = 0x08;
constexpr std::uint32_t kConfigBlocks = 0x10;
constexpr std::uint32_t kConfigPortSize = 0x18;
constexpr std::uint32_t kConfigPortAddr = 0x1c;
constexpr std::uint32_t kConfigSize = 0x20;

// Notify event source: (process id 1 << 32) + event port id base + registration number * id step.
constexpr std::uint64_t kSourceBase = (std::uint64_t{1} << 32) + 0x0e000000;
constexpr std::uint64_t kSourceStep = 0x100;
constexpr std::uint64_t kSourceCounterWrap = 8192;

bool guestRange(std::uint32_t addr, std::uint32_t size) {
    return addr != 0 && process().memory.isCommitted(addr, size);
}

bool validChannels(std::uint64_t channels) { return channels == 0 || channels == 2 || channels == 8; }

bool validBlocks(std::uint64_t blocks) {
    return blocks == 2 || blocks == 4 || blocks == 8 || blocks == 16 || blocks == 32;
}

float readBigEndianFloat(const std::uint8_t* at) {
    std::uint32_t bits;
    std::memcpy(&bits, at, sizeof bits);
    return std::bit_cast<float>(std::byteswap(bits));
}

std::uint32_t blockBytes(std::uint64_t channels) {
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1, channels)) * kCellAudioBlockFrames * kBytesPerSample;
}

}  // namespace

std::uint32_t CellAudioPorts::init() {
    if (initialised_) return kCellAudioErrorAlreadyInit;
    const std::uint32_t area = process().main.allocate(kCellAudioIndexAreaBytes, kAllocationUnit);
    if (area == 0) return kCellEnomem;

    index_area_ = area;
    ports_ = {};
    // Port 0 is handed out first, then 1, 2, ...
    free_ports_.clear();
    for (std::uint32_t port = kCellAudioPortCount; port > 0; --port) free_ports_.push_back(port - 1);
    initialised_ = true;
    return kCellOk;
}

std::uint32_t CellAudioPorts::quit() {
    if (!initialised_) return kCellAudioErrorNotInit;
    for (Port& port : ports_) {
        if (port.state != PortState::Closed) {
            process().main.free(port.addr);
            port.state = PortState::Closed;
        }
    }
    process().main.free(index_area_);
    index_area_ = 0;
    free_ports_.clear();
    // Notify registrations stay, as in the library; the engine stops, so none is sent.
    initialised_ = false;
    return kCellOk;
}

std::uint32_t CellAudioPorts::portOpen(std::uint8_t* base, std::uint32_t param_addr, std::uint32_t port_out_addr) {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (!guestRange(param_addr, kParamSize) || !guestRange(port_out_addr, 4)) return kCellAudioErrorParam;

    const std::uint64_t channels = tox2Read64(base, param_addr + kParamChannels);
    const std::uint64_t blocks = tox2Read64(base, param_addr + kParamBlocks);
    const std::uint64_t attr = tox2Read64(base, param_addr + kParamAttr);
    if (!validChannels(channels) || !validBlocks(blocks)) return kCellAudioErrorParam;
    if (free_ports_.empty()) return kCellAudioErrorPortFull;

    const std::uint32_t size = blockBytes(channels) * static_cast<std::uint32_t>(blocks);
    const std::uint32_t allocation = std::max(kAllocationUnit, (size + kAllocationUnit - 1) & ~(kAllocationUnit - 1));
    const std::uint32_t addr = process().main.allocate(allocation, kAllocationUnit);
    if (addr == 0) return kCellAudioErrorSharedMemory;

    const std::uint32_t port_no = free_ports_.back();
    free_ports_.pop_back();

    Port& port = ports_[port_no];
    port.state = PortState::Opened;
    port.channels = channels;
    port.blocks = blocks;
    port.size = size;
    port.addr = addr;
    port.level = 1.0f;
    if ((attr & kCellAudioPortAttrInitLevel) != 0) {
        port.level = std::bit_cast<float>(tox2Read32(base, param_addr + kParamLevel));
    }
    port.position = 0;
    // The next period gets number period_, so the tags of this port continue from there.
    port.tag = period_;
    port.time_us = last_period_us_;

    // A reopened slot would otherwise keep its previous user's position until the first period.
    tox2Write64(base, cellAddress(port_no), 0);
    tox2Write32(base, port_out_addr, port_no);

    logInfo("tox2: audio: port {} open: {} ch, {} blocks, attr {:#x}, level {}", port_no, channels, blocks, attr,
            port.level);
    return kCellOk;
}

std::uint32_t CellAudioPorts::getPortConfig(std::uint8_t* base, std::uint32_t port_no,
                                            std::uint32_t config_addr) const {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount || !guestRange(config_addr, kConfigSize)) return kCellAudioErrorParam;

    const Port& port = ports_[port_no];
    std::uint32_t status = kCellAudioStatusClose;
    if (port.state == PortState::Opened) status = kCellAudioStatusReady;
    if (port.state == PortState::Started) status = kCellAudioStatusRun;

    tox2Write32(base, config_addr + kConfigReadIndexAddr, cellAddress(port_no));
    tox2Write32(base, config_addr + kConfigStatus, status);
    tox2Write64(base, config_addr + kConfigChannels, port.channels);
    tox2Write64(base, config_addr + kConfigBlocks, port.blocks);
    tox2Write32(base, config_addr + kConfigPortSize, port.size);
    tox2Write32(base, config_addr + kConfigPortAddr, port.addr);
    return kCellOk;
}

std::uint32_t CellAudioPorts::portStart(std::uint8_t* base, std::uint32_t port_no) {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount) return kCellAudioErrorParam;
    Port& port = ports_[port_no];
    if (port.state == PortState::Closed) return kCellAudioErrorPortNotOpen;
    if (port.state == PortState::Started) return kCellAudioErrorPortAlreadyRun;

    std::memset(base + port.addr, 0, port.size);
    port.state = PortState::Started;
    return kCellOk;
}

std::uint32_t CellAudioPorts::portStop(std::uint32_t port_no) {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount) return kCellAudioErrorParam;
    Port& port = ports_[port_no];
    if (port.state != PortState::Started) return kCellAudioErrorPortNotRun;

    port.state = PortState::Opened;
    return kCellOk;
}

std::uint32_t CellAudioPorts::portClose(std::uint32_t port_no) {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount) return kCellAudioErrorParam;
    Port& port = ports_[port_no];
    if (port.state == PortState::Closed) return kCellAudioErrorPortNotOpen;

    process().main.free(port.addr);
    port.state = PortState::Closed;
    free_ports_.push_back(port_no);
    return kCellOk;
}

std::uint32_t CellAudioPorts::getPortBlockTag(std::uint8_t* base, std::uint32_t port_no, std::uint64_t block_no,
                                              std::uint32_t tag_addr) const {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount) return kCellAudioErrorParam;
    const Port& port = ports_[port_no];
    if (port.state == PortState::Closed) return kCellAudioErrorPortNotOpen;
    if (block_no >= port.blocks) return kCellAudioErrorParam;
    if (tag_addr != 0 && !guestRange(tag_addr, 8)) return kCellAudioErrorParam;

    // Unsigned arithmetic without a modulo: the block the next period mixes carries the number of the
    // period that published it, the following blocks the numbers after it.
    if (tag_addr != 0) tox2Write64(base, tag_addr, port.tag + block_no - port.position);
    return kCellOk;
}

std::uint32_t CellAudioPorts::getPortTimestamp(std::uint8_t* base, std::uint32_t port_no, std::uint64_t tag,
                                               std::uint32_t stamp_addr) const {
    if (!initialised_) return kCellAudioErrorNotInit;
    if (port_no >= kCellAudioPortCount) return kCellAudioErrorParam;
    const Port& port = ports_[port_no];
    if (port.state == PortState::Closed) return kCellAudioErrorPortNotOpen;
    if (tag > port.tag) return kCellAudioErrorTagNotFound;
    if (stamp_addr != 0 && !guestRange(stamp_addr, 8)) return kCellAudioErrorParam;

    // No history: an older tag is extrapolated back from the latest period at the nominal length.
    const std::uint64_t back = (port.tag - tag) * kCellAudioPeriodUs;
    if (stamp_addr != 0) tox2Write64(base, stamp_addr, port.time_us > back ? port.time_us - back : 0);
    return kCellOk;
}

std::uint32_t CellAudioPorts::setNotifyEventQueue(std::uint64_t key) {
    if (!initialised_) return kCellAudioErrorNotInit;

    const std::uint32_t queue_id = eventQueueIdByKey(key);
    logInfo("tox2: audio: notify queue key {:#x} -> {}", key,
            queue_id != 0 ? std::format("{:#x}", queue_id) : std::string("not found"));
    if (queue_id == 0) return kCellAudioErrorTransEvent;

    // Registrations of destroyed queues are dropped here, as the library does while it looks for a
    // duplicate.
    std::erase_if(registrations_, [](const Registration& r) { return !eventQueueExists(r.queue_id); });
    const bool registered = std::any_of(registrations_.begin(), registrations_.end(),
                                        [queue_id](const Registration& r) { return r.queue_id == queue_id; });
    if (registered || registrations_.size() >= kCellAudioMaxNotifyQueues) return kCellAudioErrorTransEvent;

    const std::uint64_t source = kSourceBase + (registration_count_++ % kSourceCounterWrap) * kSourceStep;
    registrations_.push_back({queue_id, key, source});
    return kCellOk;
}

std::uint32_t CellAudioPorts::removeNotifyEventQueue(std::uint64_t key) {
    if (!initialised_) return kCellAudioErrorNotInit;

    const auto it = std::find_if(registrations_.begin(), registrations_.end(),
                                 [key](const Registration& r) { return r.key == key && eventQueueExists(r.queue_id); });
    if (it == registrations_.end()) return kCellAudioErrorTransEvent;
    registrations_.erase(it);
    return kCellOk;
}

void CellAudioPorts::mixPort(const std::uint8_t* base, const Port& port, std::span<float> out,
                             std::array<std::uint64_t, kCellAudioMaxChannels>& nonzero) {
    const std::uint32_t channels = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, port.channels));
    const std::uint8_t* block = base + port.addr + port.position * blockBytes(port.channels);
    const std::size_t frames = std::min<std::size_t>(kCellAudioBlockFrames, out.size() / 2);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* samples = block + frame * channels * kBytesPerSample;
        for (std::uint32_t c = 0; c < channels; ++c) {
            if (readBigEndianFloat(samples + c * kBytesPerSample) != 0.0f) nonzero[c]++;
        }
        // Input order of an 8-channel frame is L, R, C, LFE, SL, SR, RL, RR. The downmixer the game is
        // told about is NONE, which keeps the front pair and drops the rest.
        const float left = readBigEndianFloat(samples) * port.level;
        const float right = channels >= 2 ? readBigEndianFloat(samples + kBytesPerSample) * port.level : left;
        out[2 * frame] += left;
        out[2 * frame + 1] += right;
    }
}

void CellAudioPorts::notifyQueues() {
    for (const Registration& registration : registrations_) {
        // A full queue (the game's holds two events) or a destroyed one only loses the event.
        if (sendEventToQueue(registration.queue_id, registration.source, 0, 0, 0) == kCellOk) {
            notifies_sent_.fetch_add(1, std::memory_order_relaxed);
        } else {
            notifies_failed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void CellAudioPorts::producePeriod(std::uint8_t* base, std::uint64_t now_us, std::span<float> out) {
    if (!initialised_) return;

    std::array<std::uint64_t, kCellAudioMaxChannels> nonzero{};
    for (std::uint32_t port_no = 0; port_no < kCellAudioPortCount; ++port_no) {
        Port& port = ports_[port_no];
        if (port.state != PortState::Started) continue;

        mixPort(base, port, out, nonzero);
        // Zeroed after mixing: a block the game does not rewrite in time plays as silence next time round.
        std::memset(base + port.addr + port.position * blockBytes(port.channels), 0, blockBytes(port.channels));

        port.tag = period_;
        port.time_us = now_us;
        port.position = (port.position + 1) % port.blocks;
        tox2Write64(base, cellAddress(port_no), port.position);
    }

    for (std::uint32_t c = 0; c < kCellAudioMaxChannels; ++c) {
        if (nonzero[c] == 0) continue;
        nonzero_samples_[c].fetch_add(nonzero[c], std::memory_order_relaxed);
        if (c >= 2 && !dropped_logged_[c]) {
            dropped_logged_[c] = true;
            logInfo("tox2: audio: input channel {} carries signal; the stereo downmix drops it", c);
        }
    }

    // Clamped here, after the sum of all ports; a NaN from the guest becomes silence and an infinity
    // clamps to full scale.
    for (float& sample : out) {
        if (std::isnan(sample)) sample = 0.0f;
        sample = std::clamp(sample, -1.0f, 1.0f);
    }

    period_++;
    last_period_us_ = now_us;
    periods_.fetch_add(1, std::memory_order_relaxed);
    notifyQueues();
}

CellAudioStats CellAudioPorts::stats() const {
    CellAudioStats stats;
    stats.periods = periods_.load(std::memory_order_relaxed);
    stats.notifies_sent = notifies_sent_.load(std::memory_order_relaxed);
    stats.notifies_failed = notifies_failed_.load(std::memory_order_relaxed);
    for (std::size_t c = 0; c < kCellAudioMaxChannels; ++c) {
        stats.nonzero_samples[c] = nonzero_samples_[c].load(std::memory_order_relaxed);
    }
    return stats;
}

void CellAudioPorts::reset() {
    initialised_ = false;
    index_area_ = 0;
    ports_ = {};
    free_ports_.clear();
    period_ = 0;
    last_period_us_ = 0;
    registrations_.clear();
    registration_count_ = 0;
    dropped_logged_ = {};
    periods_ = 0;
    notifies_sent_ = 0;
    notifies_failed_ = 0;
    for (auto& count : nonzero_samples_) count = 0;
}

CellAudioPorts& cellAudioPorts() {
    static CellAudioPorts instance;
    return instance;
}

}  // namespace tox2

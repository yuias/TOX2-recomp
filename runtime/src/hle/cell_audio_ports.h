// The state of the cellAudio library: ports, the read-index area, tags, timestamps and the notify
// registrations. Everything here runs under the guest lock; only the statistics are atomics, so the
// census can read them from any thread.
//
// Behaviour follows RPCS3 `rpcs3/Emu/Cell/Modules/cellAudio.h` (`CellAudioError`, `CellAudioPortParam`,
// `CellAudioPortConfig`) and `cellAudio.cpp` (`cellAudioInit`, `cellAudioPortOpen`,
// `cellAudioGetPortConfig`, `cellAudioPortStart`, `cellAudioGetPortBlockTag`,
// `cellAudioGetPortTimestamp`, `AudioSetNotifyEventQueue`, `cell_audio_thread::mix` and `advance`).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace tox2 {

// CellAudioError.
inline constexpr std::uint32_t kCellAudioErrorAlreadyInit = 0x80310701;
inline constexpr std::uint32_t kCellAudioErrorNotInit = 0x80310703;
inline constexpr std::uint32_t kCellAudioErrorParam = 0x80310704;
inline constexpr std::uint32_t kCellAudioErrorPortFull = 0x80310705;
inline constexpr std::uint32_t kCellAudioErrorPortAlreadyRun = 0x80310706;
inline constexpr std::uint32_t kCellAudioErrorPortNotOpen = 0x80310707;
inline constexpr std::uint32_t kCellAudioErrorPortNotRun = 0x80310708;
inline constexpr std::uint32_t kCellAudioErrorTransEvent = 0x80310709;
inline constexpr std::uint32_t kCellAudioErrorSharedMemory = 0x8031070b;
inline constexpr std::uint32_t kCellAudioErrorTagNotFound = 0x8031070f;

// Port table size; every port-number range check uses it.
inline constexpr std::uint32_t kCellAudioPortCount = 8;
inline constexpr std::uint32_t kCellAudioMaxChannels = 8;
inline constexpr std::uint32_t kCellAudioMaxNotifyQueues = 64;

// CellAudioPortConfig::status.
inline constexpr std::uint32_t kCellAudioStatusClose = 0x1010;
inline constexpr std::uint32_t kCellAudioStatusReady = 1;
inline constexpr std::uint32_t kCellAudioStatusRun = 2;

// CellAudioPortParam::attr bit that makes `level` the initial port level.
inline constexpr std::uint64_t kCellAudioPortAttrInitLevel = 0x1000;

// Frames per port block, and one period in whole microseconds (256 * 1000000 / 48000, truncated).
inline constexpr std::uint32_t kCellAudioBlockFrames = 256;
inline constexpr std::uint64_t kCellAudioPeriodUs = 5333;

// Each port owns a 16-byte slot of the index area; slot numbers start at 1.
inline constexpr std::uint32_t kCellAudioIndexAreaBytes = 0x10000;
inline constexpr std::uint32_t kCellAudioIndexSlotBytes = 16;

struct CellAudioStats {
    std::uint64_t periods = 0;
    std::uint64_t notifies_sent = 0;
    std::uint64_t notifies_failed = 0;  // queue full or gone
    // Non-zero samples per input channel of every mixed port. With the downmix that drops channels
    // 2..7, a non-zero count there is content that does not reach the output.
    std::array<std::uint64_t, kCellAudioMaxChannels> nonzero_samples{};
};

class CellAudioPorts {
public:
    // The logic of the 11 imports, returning the import's result code. `base` is the guest memory base;
    // guest addresses are validated here, so a bad pointer is an error code, not a fault. All of them
    // run under the guest lock.
    std::uint32_t init();
    std::uint32_t quit();
    std::uint32_t portOpen(std::uint8_t* base, std::uint32_t param_addr, std::uint32_t port_out_addr);
    std::uint32_t getPortConfig(std::uint8_t* base, std::uint32_t port, std::uint32_t config_addr) const;
    std::uint32_t portStart(std::uint8_t* base, std::uint32_t port);
    std::uint32_t portStop(std::uint32_t port);
    std::uint32_t portClose(std::uint32_t port);
    std::uint32_t getPortBlockTag(std::uint8_t* base, std::uint32_t port, std::uint64_t block_no,
                                  std::uint32_t tag_addr) const;
    std::uint32_t getPortTimestamp(std::uint8_t* base, std::uint32_t port, std::uint64_t tag,
                                   std::uint32_t stamp_addr) const;
    std::uint32_t setNotifyEventQueue(std::uint64_t key);
    std::uint32_t removeNotifyEventQueue(std::uint64_t key);

    // One cellAudio period: mixes the current block of every started port into `out` (interleaved
    // stereo, summed onto what it holds and clamped to [-1, 1]), zeroes the blocks, advances the read
    // indices, tags and timestamps, then notifies every registered queue. Does nothing before init()
    // and after quit(). `now_us` is the guest system time at the start of the period.
    void producePeriod(std::uint8_t* base, std::uint64_t now_us, std::span<float> out);

    bool initialised() const { return initialised_; }

    CellAudioStats stats() const;  // any thread

    // Forgets every port, registration, counter and statistic without touching guest memory.
    // Intended for tests.
    void reset();

private:
    enum class PortState { Closed, Opened, Started };

    struct Port {
        PortState state = PortState::Closed;
        std::uint64_t channels = 0;  // as opened: 0, 2 or 8
        std::uint64_t blocks = 0;
        std::uint32_t size = 0;  // sample bytes: max(1, channels) * blocks * 256 * 4
        std::uint32_t addr = 0;  // block 0
        float level = 1.0f;
        std::uint64_t position = 0;  // the block the next period mixes (the published read index)
        std::uint64_t tag = 0;       // period number recorded at the last period the port was started
        std::uint64_t time_us = 0;   // start time of that period
    };

    struct Registration {
        std::uint32_t queue_id = 0;
        std::uint64_t key = 0;
        std::uint64_t source = 0;
    };

    std::uint32_t cellAddress(std::uint32_t port) const { return index_area_ + (port + 1) * kCellAudioIndexSlotBytes; }
    void mixPort(const std::uint8_t* base, const Port& port, std::span<float> out,
                 std::array<std::uint64_t, kCellAudioMaxChannels>& nonzero);
    void notifyQueues();

    bool initialised_ = false;
    std::uint32_t index_area_ = 0;
    std::array<Port, kCellAudioPortCount> ports_{};
    // Free port numbers; the back is handed out next, and a closed port goes back on top.
    std::vector<std::uint32_t> free_ports_;

    std::uint64_t period_ = 0;          // number the next period gets; survives quit()
    std::uint64_t last_period_us_ = 0;  // start time of the latest period
    std::vector<Registration> registrations_;
    std::uint64_t registration_count_ = 0;  // successful registrations since boot

    std::array<bool, kCellAudioMaxChannels> dropped_logged_{};

    std::atomic<std::uint64_t> periods_{0};
    std::atomic<std::uint64_t> notifies_sent_{0};
    std::atomic<std::uint64_t> notifies_failed_{0};
    std::array<std::atomic<std::uint64_t>, kCellAudioMaxChannels> nonzero_samples_{};
};

// The one cellAudio instance. Under the guest lock, except stats().
CellAudioPorts& cellAudioPorts();

}  // namespace tox2

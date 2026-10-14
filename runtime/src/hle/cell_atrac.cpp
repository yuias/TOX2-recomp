// HLE of the 15 cellAtrac imports the game uses, and of CreateDecoder, GetRemainFrame and GetSoundInfo,
// which it does not import. The game's "ATRAC3 Decode Thread" drives them in a fixed order:
// SetDataAndGetMemSize, CreateDecoder(Ext), the queries, then Decode and GetLoopInfo polls
// until Decode reports the end, and DeleteDecoder. Streamed sounds are fed through GetStreamDataInfo
// and AddStreamData from the file thread. All state is host side, keyed by the handle's guest
// address; nothing is written into the handle, which the game never reads.
//
// The stream rules are in cell_atrac_stream.cpp. Decode runs the frame decoder of the installed
// factory under the kernel lock (one ATRAC3plus frame takes tens of microseconds); without a decoder,
// or for a frame the decoder rejects, it outputs silence of the right length.
//
// Argument lists and error codes follow RPCS3 `rpcs3/Emu/Cell/Modules/cellAtrac.h` (`CellAtracError`)
// and `cellAtrac.cpp` (the registered functions). Every value written through an output pointer is
// a 4-byte big-endian cell.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/census.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "tox2/ppu_context.h"
#include "hle/cell_atrac_stream.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace tox2 {
namespace {

constexpr char kModule[] = "cellAtrac";

// Under the kernel lock, like every handler. Handle addresses are reused after DeleteDecoder.
std::unordered_map<std::uint32_t, AtracStream>& sounds() {
    static std::unordered_map<std::uint32_t, AtracStream> map;
    return map;
}

AtracStream* findSound(std::uint32_t handle) {
    const auto it = sounds().find(handle);
    return it != sounds().end() ? &it->second : nullptr;
}

// Handle addresses whose unusable data was already reported: the game may retry the same call.
std::unordered_set<std::uint32_t>& unusableReported() {
    static std::unordered_set<std::uint32_t> set;
    return set;
}

// Written under the kernel lock, read by the census at stop from another thread.
struct Totals {
    std::atomic<std::uint64_t> decoders{0}, frames{0}, non_silent{0}, errors{0}, without_decoder{0}, starved{0};
};

Totals& totals() {
    static Totals t;
    return t;
}

void add(std::atomic<std::uint64_t>& counter, std::uint64_t n) { counter.fetch_add(n, std::memory_order_relaxed); }

// One warning per run: every handle would fail the same way (missing codec, bad parameters).
std::atomic<bool> g_decoder_warned{false};

bool guestRange(std::uint32_t addr, std::uint32_t size) {
    return addr != 0 && process().memory.isCommitted(addr, size);
}

bool cellOk(std::uint32_t addr) { return guestRange(addr, 4); }

void writeS32(std::uint8_t* base, std::uint32_t addr, std::int32_t value) {
    tox2Write32(base, addr, std::bit_cast<std::uint32_t>(value));
}

// cellAtracSetDataAndGetMemSize(CellAtracHandle* handle, u8* buffer, u32 read_bytes, u32 buffer_bytes,
//                               u32* work_memory_bytes)
void atracSetDataAndGetMemSize(Tox2PpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t handle = argU32(ctx, 0);
    const std::uint32_t buffer = argU32(ctx, 1);
    const std::uint32_t read_bytes = argU32(ctx, 2);
    const std::uint32_t buffer_bytes = argU32(ctx, 3);
    const std::uint32_t work_out = argU32(ctx, 4);
    const std::uint32_t span_bytes = std::max(read_bytes, buffer_bytes);
    if (handle == 0 || !cellOk(work_out) || (span_bytes != 0 && !guestRange(buffer, span_bytes))) {
        setResultCode(ctx, kCellEfault);
        return;
    }

    // A repeat of a sound starts from scratch whether or not the old state was deleted. The mode is
    // decided by the data, not the sizes: only a read that covers every frame is "all on memory"; a
    // read that merely fills the buffer is the start of a stream.
    sounds().erase(handle);
    std::string error;
    auto stream =
            AtracStream::open(std::span<const std::uint8_t>(base + buffer, read_bytes), buffer, buffer_bytes, error);
    if (!stream) {
        if (unusableReported().insert(handle).second) {
            logWarn("tox2: atrac: handle {:#x}: unusable data: {}", handle, error);
        }
        setResultCode(ctx, stream.error());
        return;
    }
    unusableReported().erase(handle);
    const At3Info& info = stream->info();
    logDebug(
            "tox2: atrac: set data handle {:#x}: {}, read {:#x} of buffer {:#x}, data at {:#x} size {:#x}, block {:#x}",
            handle, stream->memoryMode() ? "all on memory" : "streaming", read_bytes, buffer_bytes, info.data_offset,
            info.data_size, info.block_align);
    sounds().insert_or_assign(handle, std::move(*stream));
    tox2Write32(base, work_out, kAtracWorkMemoryBytes);
    setResultCode(ctx, kCellOk);
}

// Shared by cellAtracCreateDecoder and cellAtracCreateDecoderExt. The work memory and priorities have
// no meaning here: the decoder state is host side. A handle without a decoder still plays (as
// silence), so a failure to make one is not an error to the game.
void createDecoderFor(Tox2PpuContext* ctx) {
    const std::uint32_t handle = argU32(ctx, 0);
    AtracStream* sound = findSound(handle);
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    std::unique_ptr<AtracFrameDecoder> decoder;
    // With no factory installed the process start-up has already said why.
    if (AtracDecoderFactory* factory = atracDecoderFactory(); factory != nullptr && !sound->decoderCreated()) {
        std::string error;
        decoder = factory->create(sound->info(), error);
        if (!decoder && !g_decoder_warned.exchange(true)) {
            logWarn("tox2: atrac: no decoder: {}", error);
        }
    }
    const std::uint32_t result = sound->createDecoder(std::move(decoder));
    if (result == kCellOk) {
        add(totals().decoders, 1);
        const At3Info& info = sound->info();
        const std::string loop =
                info.loop ? std::format("{}-{}", info.loop->start_sample, info.loop->end_sample) : std::string("none");
        logInfo("tox2: atrac: decoder {:#x}: {} ch {} Hz, {} frames, loop {}, {}", handle, info.channels,
                info.sample_rate, sound->frames(), loop, sound->memoryMode() ? "all on memory" : "streaming");
        sound->debugTiming().created_us = systemTimeUs();
        logDebug("tox2: atrac: create t {} ms handle {:#x} max_sample {}", sound->debugTiming().created_us / 1000,
                 handle, kAtracMaxSample);
    }
    setResultCode(ctx, result);
}

// cellAtracCreateDecoder(CellAtracHandle* handle, u8* work_memory, u32 ppu_priority, u32 spu_priority)
void atracCreateDecoder(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    createDecoderFor(ctx);
}

// cellAtracCreateDecoderExt(CellAtracHandle* handle, u8* work_memory, u32 ppu_priority,
//                           CellAtracExtRes* ext_res)
// ext_res is {u32 spurs; u8 priority[8]}: the firmware decoder ran on that SPURS instance, ours is a host
// decoder, so it is not read.
void atracCreateDecoderExt(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    createDecoderFor(ctx);
}

// cellAtracGetVacantSize(CellAtracHandle* handle, u32* vacant_bytes)
// The game sizes its next file read from the cell and stores it without looking at the result, so the
// cell is written whenever it is mapped, also in memory mode where there is nothing to add.
void atracGetVacantSize(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!cellOk(out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, out, sound->vacant());
    setResultCode(ctx, sound->memoryMode() ? kAtracErrorAllDataIsOnMemory : kCellOk);
}

// cellAtracGetInternalErrorInfo(CellAtracHandle* handle, s32* result)
// The game asks after any decode failure; the host decoder has no internal error to report.
void atracGetInternalErrorInfo(Tox2PpuContext* ctx, std::uint8_t* base) {
    if (findSound(argU32(ctx, 0)) == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!cellOk(out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    writeS32(base, out, 0);
    setResultCode(ctx, kCellOk);
}

// cellAtracDeleteDecoder(CellAtracHandle* handle)
void atracDeleteDecoder(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    const std::uint32_t handle = argU32(ctx, 0);
    if (AtracStream* sound = findSound(handle); sound != nullptr && logEnabled(LogLevel::Debug)) {
        const AtracStream::DebugTiming& timing = sound->debugTiming();
        const double audio_ms = static_cast<double>(timing.samples) * 1000.0 / std::max(sound->info().sample_rate, 1u);
        const double wall_ms = static_cast<double>(timing.last_decode_us - timing.first_decode_us) / 1000.0;
        logDebug(
                "tox2: atrac: delete t {} ms handle {:#x} samples {} decodes {} starved {} first_decode {} ms "
                "last_decode {} ms wall {:.1f} ms audio {:.1f} ms ({} Hz)",
                systemTimeUs() / 1000, handle, timing.samples, timing.decodes, timing.starved,
                timing.first_decode_us / 1000, timing.last_decode_us / 1000, wall_ms, audio_ms,
                sound->info().sample_rate);
    }
    setResultCode(ctx, sounds().erase(handle) != 0 ? kCellOk : kAtracErrorUnsetData);
}

// cellAtracDecode(CellAtracHandle* handle, float* out, u32* samples, u32* finish, s32* remain)
void atracDecode(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    if (!sound->decoderCreated()) {
        setResultCode(ctx, kAtracErrorNoDecoder);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    const std::uint32_t samples_out = argU32(ctx, 2);
    const std::uint32_t finish_out = argU32(ctx, 3);
    const std::uint32_t remain_out = argU32(ctx, 4);
    const std::uint32_t channels = sound->info().channels;
    if (!guestRange(out, kAtracMaxSample * channels * 4) || !cellOk(samples_out) || !cellOk(finish_out) ||
        !cellOk(remain_out) || !guestRange(sound->bufferAddr(), sound->bufferBytes())) {
        setResultCode(ctx, kCellEfault);
        return;
    }

    std::array<float, kAtracMaxSample * kAtracMaxChannels> pcm;
    const std::span<const std::uint8_t> ring(base + sound->bufferAddr(), sound->bufferBytes());
    const auto result = sound->decode(ring, pcm);
    if (!result) {
        setResultCode(ctx, result.error());
        return;
    }
    Totals& t = totals();
    add(t.frames, result->counts.decoded);
    add(t.non_silent, result->counts.non_silent);
    add(t.errors, result->counts.errors);
    add(t.without_decoder, result->counts.without_decoder);
    if (result->starved) {
        // The game waits for AddStreamData on this code and retries; success with no samples and
        // finish = 0 would make it queue an empty PCM buffer.
        add(t.starved, 1);
        sound->debugTiming().starved++;
        setResultCode(ctx, kAtracErrorNodataInBuffer);
        return;
    }
    AtracStream::DebugTiming& timing = sound->debugTiming();
    timing.last_decode_us = systemTimeUs();
    if (timing.decodes++ == 0) timing.first_decode_us = timing.last_decode_us;
    timing.samples += result->samples;
    if (const std::string reason = sound->takeFirstError(); !reason.empty()) {
        logWarn("tox2: atrac: handle {:#x}: decode error, playing silence for such frames: {}", argU32(ctx, 0), reason);
    }
    if (const std::string note = sound->takeTraceNote(); !note.empty()) {
        logDebug("tox2: atrac: handle {:#x}: {}", argU32(ctx, 0), note);
    }
    const std::size_t count = std::size_t{result->samples} * channels;
    for (std::size_t i = 0; i < count; ++i) {
        tox2Write32(base, out + static_cast<std::uint32_t>(i * 4), std::bit_cast<std::uint32_t>(pcm[i]));
    }
    tox2Write32(base, samples_out, result->samples);
    tox2Write32(base, finish_out, result->finish ? 1 : 0);
    writeS32(base, remain_out, result->remain);
    setResultCode(ctx, kCellOk);
}

// cellAtracGetStreamDataInfo(CellAtracHandle* handle, u8** write_ptr, u32* writable_bytes, u32* read_pos)
void atracGetStreamDataInfo(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t write_ptr_out = argU32(ctx, 1);
    const std::uint32_t writable_out = argU32(ctx, 2);
    const std::uint32_t read_pos_out = argU32(ctx, 3);
    if (!cellOk(write_ptr_out) || !cellOk(writable_out) || !cellOk(read_pos_out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const auto info = sound->streamDataInfo();
    if (!info) {
        setResultCode(ctx, info.error());
        return;
    }
    tox2Write32(base, write_ptr_out, info->write_addr);
    tox2Write32(base, writable_out, info->writable);
    tox2Write32(base, read_pos_out, info->read_pos);
    logDebug("tox2: atrac: stream info handle {:#x}: write at {:#x}, writable {:#x}, read pos {:#x}", argU32(ctx, 0),
             info->write_addr - sound->bufferAddr(), info->writable, info->read_pos);
    setResultCode(ctx, kCellOk);
}

// cellAtracAddStreamData(CellAtracHandle* handle, u32 bytes)
void atracAddStreamData(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t result = sound->addStreamData(argU32(ctx, 1));
    logDebug("tox2: atrac: add stream data handle {:#x}: {:#x} bytes, result {:#x}, buffered to {:#x}", argU32(ctx, 0),
             argU32(ctx, 1), result, sound->bufferedEnd());
    setResultCode(ctx, result);
}

// cellAtracGetRemainFrame(CellAtracHandle* handle, s32* remain)
void atracGetRemainFrame(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!cellOk(out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    writeS32(base, out, sound->remain());
    setResultCode(ctx, kCellOk);
}

// cellAtracGetChannel(CellAtracHandle* handle, u32* channels)
void atracGetChannel(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!cellOk(out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, out, sound->info().channels);
    setResultCode(ctx, kCellOk);
}

// cellAtracGetMaxSample(CellAtracHandle* handle, u32* max_samples)
void atracGetMaxSample(Tox2PpuContext* ctx, std::uint8_t* base) {
    if (findSound(argU32(ctx, 0)) == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t out = argU32(ctx, 1);
    if (!cellOk(out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, out, kAtracMaxSample);
    setResultCode(ctx, kCellOk);
}

// cellAtracSetLoopNum(CellAtracHandle* handle, s32 loop_num)
void atracSetLoopNum(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    sound->setLoopNum(static_cast<std::int32_t>(argU32(ctx, 1)));
    setResultCode(ctx, kCellOk);
}

// cellAtracGetLoopInfo(CellAtracHandle* handle, s32* loop_num, u32* status)
void atracGetLoopInfo(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t loops_out = argU32(ctx, 1);
    const std::uint32_t status_out = argU32(ctx, 2);
    if (!cellOk(loops_out) || !cellOk(status_out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    writeS32(base, loops_out, sound->loopNum());
    tox2Write32(base, status_out, sound->loopStatus());
    setResultCode(ctx, kCellOk);
}

// cellAtracGetSoundInfo(CellAtracHandle* handle, s32* end_sample, s32* loop_start, s32* loop_end)
void atracGetSoundInfo(Tox2PpuContext* ctx, std::uint8_t* base) {
    AtracStream* sound = findSound(argU32(ctx, 0));
    if (sound == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t end_out = argU32(ctx, 1);
    const std::uint32_t start_out = argU32(ctx, 2);
    const std::uint32_t loop_end_out = argU32(ctx, 3);
    if (!cellOk(end_out) || !cellOk(start_out) || !cellOk(loop_end_out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    const AtracSoundInfo info = sound->soundInfo();
    writeS32(base, end_out, info.end_sample);
    writeS32(base, start_out, info.loop_start);
    writeS32(base, loop_end_out, info.loop_end);
    setResultCode(ctx, kCellOk);
}

// cellAtracIsSecondBufferNeeded(CellAtracHandle* handle): the answer is the result itself, and this
// model never needs a second buffer.
void atracIsSecondBufferNeeded(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, findSound(argU32(ctx, 0)) != nullptr ? 0 : kAtracErrorUnsetData);
}

// cellAtracGetSecondBufferInfo(CellAtracHandle* handle, u32* read_pos, u32* data_bytes)
// Unreachable while IsSecondBufferNeeded answers 0.
void atracGetSecondBufferInfo(Tox2PpuContext* ctx, std::uint8_t* base) {
    if (findSound(argU32(ctx, 0)) == nullptr) {
        setResultCode(ctx, kAtracErrorUnsetData);
        return;
    }
    const std::uint32_t pos_out = argU32(ctx, 1);
    const std::uint32_t size_out = argU32(ctx, 2);
    if (!cellOk(pos_out) || !cellOk(size_out)) {
        setResultCode(ctx, kCellEfault);
        return;
    }
    tox2Write32(base, pos_out, 0);
    tox2Write32(base, size_out, 0);
    setResultCode(ctx, kCellOk);
}

// cellAtracSetSecondBuffer(CellAtracHandle* handle, u8* buffer, u32 bytes)
void atracSetSecondBuffer(Tox2PpuContext* ctx, std::uint8_t* base) {
    (void)base;
    setResultCode(ctx, findSound(argU32(ctx, 0)) != nullptr ? kCellOk : kAtracErrorUnsetData);
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x06ddb53e, "cellAtracSetSecondBuffer", atracSetSecondBuffer},
        {kModule, 0x0f9667b6, "cellAtracGetChannel", atracGetChannel},
        {kModule, 0x2642d4cc, "cellAtracCreateDecoderExt", atracCreateDecoderExt},
        {kModule, 0x2bfff084, "cellAtracGetStreamDataInfo", atracGetStreamDataInfo},
        {kModule, 0x46cfc013, "cellAtracAddStreamData", atracAddStreamData},
        {kModule, 0x5f62d546, "cellAtracGetMaxSample", atracGetMaxSample},
        {kModule, 0x66afc68e, "cellAtracSetDataAndGetMemSize", atracSetDataAndGetMemSize},
        {kModule, 0x761cb9be, "cellAtracDeleteDecoder", atracDeleteDecoder},
        {kModule, 0x78ba5c41, "cellAtracSetLoopNum", atracSetLoopNum},
        {kModule, 0x8eb0e65f, "cellAtracDecode", atracDecode},
        {kModule, 0x99efe171, "cellAtracIsSecondBufferNeeded", atracIsSecondBufferNeeded},
        {kModule, 0xab6b6dbf, "cellAtracGetLoopInfo", atracGetLoopInfo},
        {kModule, 0xb5c11938, "cellAtracGetInternalErrorInfo", atracGetInternalErrorInfo},
        {kModule, 0xbe07f05e, "cellAtracGetSecondBufferInfo", atracGetSecondBufferInfo},
        {kModule, 0xc9a95fcb, "cellAtracGetVacantSize", atracGetVacantSize},
        {kModule, 0xcf01d5d4, "cellAtracGetSoundInfo", atracGetSoundInfo},
        {kModule, 0xdfab73aa, "cellAtracGetRemainFrame", atracGetRemainFrame},
        {kModule, 0xfa293e88, "cellAtracCreateDecoder", atracCreateDecoder},
};

}  // namespace

std::span<const HleEntry> cellAtracHandlers() { return kHandlers; }

CellAtracStats cellAtracStats() {
    const Totals& t = totals();
    CellAtracStats stats;
    stats.decoders = t.decoders.load(std::memory_order_relaxed);
    stats.frames = t.frames.load(std::memory_order_relaxed);
    stats.non_silent = t.non_silent.load(std::memory_order_relaxed);
    stats.errors = t.errors.load(std::memory_order_relaxed);
    stats.without_decoder = t.without_decoder.load(std::memory_order_relaxed);
    stats.starved = t.starved.load(std::memory_order_relaxed);
    return stats;
}

void resetCellAtrac() {
    sounds().clear();
    unusableReported().clear();
    Totals& t = totals();
    for (std::atomic<std::uint64_t>* counter :
         {&t.decoders, &t.frames, &t.non_silent, &t.errors, &t.without_decoder, &t.starved}) {
        counter->store(0, std::memory_order_relaxed);
    }
    g_decoder_warned.store(false);
}

}  // namespace tox2

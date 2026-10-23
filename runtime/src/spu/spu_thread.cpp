#include "spu/spu_thread.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <new>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_priority.h"
#include "core/log.h"
#include "core/reservation.h"
#include "core/thread_cpu.h"
#include "core/threads.h"
#include "spu/spu_census.h"
#include "spu/spu_channels.h"

namespace tox2 {
namespace {

thread_local SpuThread* t_current = nullptr;

constexpr std::uint32_t kLsMask = kTox2SpuLsSize - 1u;

// What fatal() prints on an SPU host thread.
std::string dumpForFatal(const void* thread) { return describeSpuThread(*static_cast<const SpuThread*>(thread)); }

// Sets the MXCSR of the generated SPU code (round toward zero, FTZ, DAZ) for its lifetime.
class SpuFpEnvironment {
public:
    SpuFpEnvironment() : saved_(tox2SpuMxcsrGet()) { tox2SpuMxcsrSet(kTox2SpuMxcsrSpu); }
    ~SpuFpEnvironment() { tox2SpuMxcsrSet(saved_); }
    SpuFpEnvironment(const SpuFpEnvironment&) = delete;
    SpuFpEnvironment& operator=(const SpuFpEnvironment&) = delete;

private:
    std::uint32_t saved_;
};

std::uint32_t roundDown(std::uint32_t value) { return value & ~(kSpuTagUnit - 1u); }
std::uint32_t roundUp(std::uint64_t value) {
    return static_cast<std::uint32_t>((value + kSpuTagUnit - 1u) & ~(std::uint64_t{kSpuTagUnit} - 1u));
}

}  // namespace

void SpuThread::LsDeleter::operator()(std::uint8_t* p) const { ::operator delete[](p, std::align_val_t(128)); }

SpuThread::SpuThread(std::uint32_t id, std::string name, SpuOwner* owner)
    : id_(id), name_(std::move(name)), owner_(owner) {
    auto* ls = static_cast<std::uint8_t*>(::operator new[](kTox2SpuLsSize, std::align_val_t(128)));
    std::memset(ls, 0, kTox2SpuLsSize);
    ls_.reset(ls);
    channels_ = std::make_unique<SpuChannels>(*this);
    ctx_.exit_ls = kTox2SpuNoExit;
    ctx_.runtime = this;
    spuCensusAdd(*this);
}

SpuThread::~SpuThread() {
    if (thread_.joinable()) {
        requestHalt();
        thread_.join();
    }
    // After the join, so the counters are final when the census totals take them over.
    spuCensusRemove(*this);
}

bool SpuThread::start(std::uint32_t entry_ls) {
    if (thread_.joinable() || state_.load() != SpuState::Created) {
        return false;
    }
    // Running before the host thread exists, so the supervisor never sees a started thread as idle.
    state_.store(SpuState::Running);
    spuCensusStarted();
    thread_ = std::thread([this, entry_ls] { threadMain(entry_ls); });
    return true;
}

void SpuThread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SpuThread::requestHalt() {
    __atomic_store_n(&ctx_.halt, 1u, __ATOMIC_RELEASE);
    // A thread that waits for a reservation event only sees the flag after this wake-up.
    reservations().wake();
    channels_->wake();
}

void SpuThread::threadMain(std::uint32_t entry_ls) {
    registerHostThreadCpu(std::format("spu.{:08x}", id_));
    // First, so the whole run is raised. Every wait this thread makes blocks (channels, reservation table, guest
    // lock); the reservation stripe lock is a bounded spin that ends in a sleep.
    host_priority_.store(raise_host_priority_ ? applyHostThreadPriority(HostThreadRole::HighPrioritySpu)
                                              : currentHostThreadPriority(),
                         std::memory_order_release);
    reserveFaultStack();
    setGuestLockClass(GuestLockClass::Spu);
    t_current = this;
    // The identity of the host thread, for log lines and for a fatal stop raised anywhere on it.
    setLogSpuThread(id_);
    setFatalDump(&dumpForFatal, this);
    const SpuLeave how = runSpu(*this, entry_ls);
    state_.store(SpuState::Finished);
    t_current = nullptr;
    if (owner_ != nullptr) {
        owner_->onFinished(*this, how);
    }
    setFatalDump(nullptr, nullptr);
    setLogSpuThread(std::nullopt);
}

void SpuThread::countStop(std::uint32_t code) {
    std::lock_guard lock(stop_mutex_);
    stops_[code]++;
}

std::map<std::uint32_t, std::uint64_t> SpuThread::stopCounts() const {
    std::lock_guard lock(stop_mutex_);
    return stops_;
}

std::uint16_t SpuThread::recordTag(std::uint32_t image, std::uint32_t segment, std::int32_t delta) {
    std::lock_guard lock(record_mutex_);
    for (std::uint32_t i = 0; i < record_count_; i++) {
        const SpuLoadRecord& r = records_[i];
        if (r.image == image && r.segment == segment && r.delta == delta) {
            return static_cast<std::uint16_t>(i + 1);
        }
    }
    if (record_count_ == kSpuMaxLoadRecords) {
        spuStop(*this, std::format("tox2: stop: spu: more than {} distinct image loads on thread {:#010x}",
                                   kSpuMaxLoadRecords, id_));
    }
    records_[record_count_] = SpuLoadRecord{image, segment, delta};
    return static_cast<std::uint16_t>(++record_count_);
}

namespace {

// Calls apply(word, mask) for the summary bits of blocks [first_block, last_block).
template <typename Apply>
void forBlockMasks(std::uint32_t first_block, std::uint32_t last_block, Apply apply) {
    for (std::uint32_t b = first_block; b < last_block;) {
        const std::uint32_t word = b / 64;
        const std::uint32_t end = std::min(last_block, (word + 1) * 64);
        const std::uint32_t count = end - b;
        const std::uint64_t mask = (count == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << count) - 1) << (b % 64);
        apply(word, mask);
        b = end;
    }
}

}  // namespace

void SpuThread::setTags(std::uint32_t first_unit, std::uint32_t last_unit, std::uint16_t tag) {
    if (first_unit >= last_unit) return;
    if (tag != 0) {
        // The bits go first: a reader that sees a tag must also see its block marked.
        forBlockMasks(first_unit / kSpuTagBlockUnits, (last_unit - 1) / kSpuTagBlockUnits + 1,
                      [&](std::uint32_t word, std::uint64_t mask) {
                          tagged_blocks_[word].fetch_or(mask, std::memory_order_release);
                      });
    } else {
        // A block that is only partly covered may still hold tagged units outside the range.
        forBlockMasks((first_unit + kSpuTagBlockUnits - 1) / kSpuTagBlockUnits, last_unit / kSpuTagBlockUnits,
                      [&](std::uint32_t word, std::uint64_t mask) {
                          tagged_blocks_[word].fetch_and(~mask, std::memory_order_release);
                      });
    }
    for (std::uint32_t u = first_unit; u < last_unit; u++) {
        tags_[u].store(tag, std::memory_order_release);
    }
}

std::array<std::uint64_t, kSpuTagBlockWords> SpuThread::taggedBlocks() const {
    std::array<std::uint64_t, kSpuTagBlockWords> out{};
    for (std::uint32_t i = 0; i < kSpuTagBlockWords; i++) out[i] = tagged_blocks_[i].load(std::memory_order_acquire);
    return out;
}

SpuThread* currentSpuThread() { return t_current; }

SpuLeave runSpu(SpuThread& thread, std::uint32_t entry_ls) {
    // The generated float code relies on the SPU environment; restoring the caller's MXCSR on every exit
    // keeps tests that call runSpu on their own thread unaffected. Host code reached from SPU code (channels,
    // MFC, lv2 callbacks, logging) also runs under it, so it must not depend on float rounding.
    const SpuFpEnvironment fp_environment;
    Tox2SpuContext& ctx = thread.context();
    ctx.runtime = &thread;
    ctx.jump_site = entry_ls;
    ctx.jump_target = entry_ls;
    tox2SpuDispatch(&ctx, thread.ls());
    // exit_ls is stored masked (kTox2SpuNoExit never matches), and the dispatch check is the only way to
    // leave with jump_target equal to it.
    if ((ctx.jump_target & (kTox2SpuLsSize - 4u)) == ctx.exit_ls) {
        return SpuLeave::Exit;
    }
    return thread.haltRequested() ? SpuLeave::Halt : SpuLeave::Returned;
}

void untagLs(SpuThread& thread, std::uint32_t lsa, std::uint32_t size) {
    std::uint64_t pos = lsa & kLsMask;
    std::uint64_t left = size;
    while (left > 0) {
        const std::uint64_t n = std::min<std::uint64_t>(left, kTox2SpuLsSize - pos);
        thread.setTags(static_cast<std::uint32_t>(pos) / kSpuTagUnit, roundUp(pos + n) / kSpuTagUnit, 0);
        left -= n;
        pos = 0;
    }
}

namespace {

// Untags the tracked units whose bytes the copy of [lsa, lsa + size) from `src` would change. A tag says
// "these LS bytes are image X at its link address", so overwriting a unit with the bytes it already holds
// keeps that true (the SPURS policy module restores a task's LS pattern from a save area this way). Edge
// units are partial: only the bytes the copy writes into them are compared.
//
// Most copies land in blocks without code, which the summary skips without reading a tag. In the other blocks a run
// of tagged units is compared with one memcmp, and only a run that differs is compared unit by unit: a unit the
// copy covers wholly with a fixed 16-byte compare, the at most two edge units with a bounded one. Consecutive
// differing units are untagged with one setTags call.
void untagChangedUnits(SpuThread& thread, const std::uint8_t* src, std::uint32_t lsa, std::uint32_t size) {
    const std::uint32_t end = lsa + size;
    const std::uint32_t first_unit = lsa / kSpuTagUnit;
    const std::uint32_t last_unit = roundUp(end) / kSpuTagUnit;
    const auto tagged = [&](std::uint32_t unit) { return thread.unitTagged(unit); };
    // Same bytes in LS and in the copy for the part of units [from, to) that the copy writes.
    const auto same = [&](std::uint32_t from, std::uint32_t to) {
        const std::uint32_t lo = std::max(lsa, from * kSpuTagUnit);
        const std::uint32_t hi = std::min(end, to * kSpuTagUnit);
        return std::memcmp(thread.ls() + lo, src + (lo - lsa), hi - lo) == 0;
    };
    // `same` for one unit. A fixed size lets the compiler inline the compare as two 8-byte loads.
    const auto sameUnit = [&](std::uint32_t unit) {
        const std::uint32_t lo = unit * kSpuTagUnit;
        if (lo < lsa || lo + kSpuTagUnit > end) return same(unit, unit + 1);
        std::uint64_t a[2], b[2];
        std::memcpy(a, thread.ls() + lo, kSpuTagUnit);
        std::memcpy(b, src + (lo - lsa), kSpuTagUnit);
        return ((a[0] ^ b[0]) | (a[1] ^ b[1])) == 0;
    };
    for (std::uint32_t block = first_unit / kSpuTagBlockUnits; block * kSpuTagBlockUnits < last_unit; block++) {
        if (!thread.blockMayBeTagged(block)) continue;
        const std::uint32_t block_first = block * kSpuTagBlockUnits;
        const std::uint32_t from = std::max(first_unit, block_first);
        const std::uint32_t to = std::min(last_unit, block_first + kSpuTagBlockUnits);
        bool remaining = false;  // a unit of the block that stays tagged
        for (std::uint32_t u = from; u < to;) {
            if (!tagged(u)) {
                u++;
                continue;
            }
            std::uint32_t run_end = u + 1;
            while (run_end < to && tagged(run_end)) run_end++;
            if (same(u, run_end)) {
                remaining = true;
            } else {
                std::uint32_t diff_begin = run_end;  // start of the current run of differing units, if any
                for (std::uint32_t v = u; v < run_end; v++) {
                    if (sameUnit(v)) {
                        remaining = true;
                        if (diff_begin != run_end) thread.setTags(diff_begin, v, 0);
                        diff_begin = run_end;
                    } else if (diff_begin == run_end) {
                        diff_begin = v;
                    }
                }
                if (diff_begin != run_end) thread.setTags(diff_begin, run_end, 0);
            }
            u = run_end;
        }
        if (!remaining && from == block_first && to == block_first + kSpuTagBlockUnits) {
            thread.clearBlockMayBeTagged(block);
        }
    }
}

// One piece of a copy that does not wrap.
void copyPiece(SpuThread& thread, const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size) {
    untagChangedUnits(thread, base + ea, lsa, size);
    std::memcpy(thread.ls() + lsa, base + ea, size);

    // Copies from outside every code segment (most data DMA) tag nothing.
    if (!spuCodeRangeOverlaps(ea, size)) return;

    const std::uint64_t ea_end = std::uint64_t{ea} + size;
    const std::uint32_t count = spuImageCount();
    for (std::uint32_t i = 0; i < count; i++) {
        const Tox2SpuImageInfo& image = spuImage(i);
        std::uint32_t first_tagged = 0, end_tagged = 0;
        for (std::uint32_t j = 0; j < image.segment_count; j++) {
            const Tox2SpuSegment& s = image.segments[j];
            if (s.code == 0) continue;
            const std::uint64_t a = std::max<std::uint64_t>(ea, s.ea);
            const std::uint64_t b = std::min<std::uint64_t>(ea_end, std::uint64_t{s.ea} + s.size);
            if (a >= b) continue;
            // Only whole 16-byte units are tagged: a unit that is partly outside the segment could hold
            // bytes of something else.
            const std::uint32_t lo = roundUp(lsa + (a - ea));
            const std::uint32_t hi = roundDown(static_cast<std::uint32_t>(lsa + (b - ea)));
            if (lo >= hi) continue;
            // Where the segment's first byte sits in LS, minus where it should sit.
            const std::int64_t delta = std::int64_t{lsa} - (std::int64_t{ea} - s.ea) - s.ls;
            const std::uint16_t tag = thread.recordTag(i, j, static_cast<std::int32_t>(delta));
            thread.setTags(lo / kSpuTagUnit, hi / kSpuTagUnit, tag);
            if (end_tagged == 0) first_tagged = lo;
            end_tagged = std::max(end_tagged, hi);
        }
        if (end_tagged != 0 && spuMarkImageSeen(i)) {
            logInfo("tox2: spu: first image {} at ls {:#07x}..{:#07x} on thread {:#010x}", image.name, first_tagged,
                    end_tagged, thread.id());
        }
    }
}

}  // namespace

void copyToLs(SpuThread& thread, const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size) {
    std::uint32_t pos = lsa & kLsMask;
    while (size > 0) {
        const std::uint32_t n = std::min<std::uint32_t>(size, kTox2SpuLsSize - pos);
        copyPiece(thread, base, ea, pos, n);
        ea += n;
        size -= n;
        pos = 0;
    }
}

std::optional<SpuTracked> trackedImageAt(const SpuThread& thread, std::uint32_t lsa) {
    const SpuLoadRecord* r = thread.recordAt(lsa);
    if (r == nullptr) return std::nullopt;
    return SpuTracked{&spuImage(r->image), r->image, r->segment, r->delta};
}

std::string describeSpuThread(const SpuThread& thread) {
    const Tox2SpuContext& ctx = thread.context();
    std::string out = std::format(
            "  spu thread {:#010x} \"{}\": jump_target {:#07x} jump_site {:#07x} srr0 {:#07x} "
            "int_enabled {} halt {}\n",
            thread.id(), thread.name(), ctx.jump_target, ctx.jump_site, ctx.srr0,
            static_cast<unsigned>(ctx.int_enabled), ctx.halt);

    out += "  trace (oldest first):";
    const std::uint32_t shown = std::min<std::uint32_t>(ctx.trace_pos, kTox2SpuTraceSize);
    for (std::uint32_t i = 0; i < shown; i++) {
        out += std::format(" {:05x}", ctx.trace[(ctx.trace_pos - shown + i) & (kTox2SpuTraceSize - 1)]);
    }
    out += "\n";

    out += "  registers (non-zero, words 0..3):\n";
    for (std::uint32_t r = 0; r < 128; r++) {
        const Tox2Vr& v = ctx.gpr[r];
        const std::uint32_t w0 = TOX2_VR_W(v, 0), w1 = TOX2_VR_W(v, 1), w2 = TOX2_VR_W(v, 2), w3 = TOX2_VR_W(v, 3);
        if ((w0 | w1 | w2 | w3) != 0) {
            out += std::format("    ${:<3} {:08x} {:08x} {:08x} {:08x}\n", r, w0, w1, w2, w3);
        }
    }

    out += "  ls map:\n";
    bool any = false;
    for (std::uint32_t u = 0; u < kSpuTagCount;) {
        const SpuLoadRecord* r = thread.recordAt(u * kSpuTagUnit);
        std::uint32_t end = u + 1;
        while (end < kSpuTagCount && thread.recordAt(end * kSpuTagUnit) == r) end++;
        if (r != nullptr) {
            out += std::format("    ls {:#07x}..{:#07x} {} segment {} delta {}\n", u * kSpuTagUnit, end * kSpuTagUnit,
                               spuImage(r->image).name, r->segment, r->delta);
            any = true;
        }
        u = end;
    }
    if (!any) out += "    (nothing tracked)\n";
    return out;
}

void spuStop(const SpuThread& thread, std::string_view line, std::string_view detail) {
    std::string text(line);
    if (!detail.empty()) {
        text += "\n  ";
        text += detail;
    }
    text += "\n";
    text += describeSpuThread(thread);
    // The dump ends with a newline; the logger adds its own.
    if (text.back() == '\n') text.pop_back();
    stopProcess(ExitCode::Fatal, text);
}

}  // namespace tox2

#include "spu/spu_thread.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <new>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/reservation.h"

namespace tox2 {
namespace {

thread_local SpuThread* t_current = nullptr;

constexpr std::uint32_t kLsMask = kTox2SpuLsSize - 1u;

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
    ctx_.exit_ls = kTox2SpuNoExit;
    ctx_.runtime = this;
}

SpuThread::~SpuThread() {
    if (thread_.joinable()) {
        requestHalt();
        thread_.join();
    }
}

bool SpuThread::start(std::uint32_t entry_ls) {
    if (thread_.joinable() || state_.load() != SpuState::Created) {
        return false;
    }
    // Running before the host thread exists, so the supervisor never sees a started thread as idle.
    state_.store(SpuState::Running);
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
}

void SpuThread::threadMain(std::uint32_t entry_ls) {
    reserveFaultStack();
    t_current = this;
    const SpuLeave how = runSpu(*this, entry_ls);
    state_.store(SpuState::Finished);
    t_current = nullptr;
    if (owner_ != nullptr) {
        owner_->onFinished(*this, how);
    }
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

void SpuThread::setTags(std::uint32_t first_unit, std::uint32_t last_unit, std::uint16_t tag) {
    for (std::uint32_t u = first_unit; u < last_unit; u++) {
        tags_[u].store(tag, std::memory_order_release);
    }
}

SpuThread* currentSpuThread() { return t_current; }

SpuLeave runSpu(SpuThread& thread, std::uint32_t entry_ls) {
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

// One piece of a copy that does not wrap.
void copyPiece(SpuThread& thread, const std::uint8_t* base, std::uint32_t ea, std::uint32_t lsa, std::uint32_t size) {
    std::memcpy(thread.ls() + lsa, base + ea, size);
    untagLs(thread, lsa, size);

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

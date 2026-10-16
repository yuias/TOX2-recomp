#include "spu/spu_capture.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <memory>
#include <utility>

#include "core/log.h"
#include "core/thread_cpu.h"
#include "spu/spu_channels.h"
#include "spu/spu_mfc.h"
#include "spu/spu_thread.h"

namespace tox2 {
namespace {

constexpr char kMagic[8] = {'T', 'O', 'X', '2', 'S', 'P', 'U', 'C'};
constexpr std::uint32_t kGprBytes = 128 * sizeof(Tox2Vr);
constexpr std::uint32_t kLsBytes = kTox2SpuLsSize;

static_assert(kGprBytes + 16 + 8 <= kSpuCaptureStateSize && kSpuCaptureStateSize % 16 == 0);
static_assert(sizeof(Tox2Vr) == 16);

// --- File format --------------------------------------------------------------------------------

class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    template <typename T>
    void put(T value) {
        bytes(&value, sizeof value);
    }
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out_.insert(out_.end(), p, p + size);
    }
    // `text` NUL-padded to `size` bytes.
    void padded(const std::string& text, std::size_t size) {
        bytes(text.data(), text.size());
        out_.resize(out_.size() + size - text.size(), 0);
    }
    void alignTo(std::size_t start, std::size_t size) { out_.resize(start + size, 0); }
    std::size_t pos() const { return out_.size(); }

private:
    std::vector<std::uint8_t>& out_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    template <typename T>
    T get() {
        T value;
        std::memcpy(&value, bytes_.data() + pos_, sizeof value);
        pos_ += sizeof value;
        return value;
    }
    void bytes(void* out, std::size_t size) {
        std::memcpy(out, bytes_.data() + pos_, size);
        pos_ += size;
    }
    std::string padded(std::size_t size) {
        const char* p = reinterpret_cast<const char*>(bytes_.data() + pos_);
        pos_ += size;
        return std::string(p, strnlen(p, size));
    }
    void seek(std::size_t pos) { pos_ = pos; }
    std::size_t pos() const { return pos_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_ = 0;
};

// --- Selector parsing -----------------------------------------------------------------------------

bool parseHex32(std::string_view text, std::uint32_t& out) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, 16);
    return ec == std::errc() && end == text.data() + text.size();
}

std::vector<std::string_view> split(std::string_view text, char sep) {
    std::vector<std::string_view> parts;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = text.find(sep, from);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(from));
            return parts;
        }
        parts.push_back(text.substr(from, at - from));
        from = at + 1;
    }
}

// --- Capture state machine ------------------------------------------------------------------------

enum Reason : std::uint32_t { kReads, kMfc, kStops, kDispatches, kReasonCount };
constexpr const char* kReasonNames[kReasonCount] = {"channel reads", "mfc commands", "stops", "dispatches"};

// What the purity check compares between S and E: anything that moved means the span consumed an input that
// the capture does not record.
struct Totals {
    std::uint64_t reads = 0;
    std::uint64_t stops = 0;
    std::array<std::uint64_t, 9> mfc{};
};

Totals totalsOf(SpuThread& thread) {
    Totals t;
    SpuChannels& channels = thread.channels();
    for (std::uint32_t ch = 0; ch < chan::kCount; ch++) t.reads += channels.readCount(ch);
    for (const auto& [code, n] : thread.stopCounts()) t.stops += n;
    const SpuMfcCounters& c = channels.mfc().counters();
    const std::atomic<std::uint64_t>* fields[] = {&c.get,       &c.put,           &c.get_list, &c.put_list, &c.getllar,
                                                  &c.putllc_ok, &c.putllc_failed, &c.putlluc,  &c.fences};
    for (std::size_t i = 0; i < t.mfc.size(); i++) t.mfc[i] = fields[i]->load(std::memory_order_relaxed);
    return t;
}

struct SelectorState {
    SpuCaptureSelector spec;
    std::uint32_t image_index = 0;
    std::atomic<std::uint32_t> accepted{0};
    std::atomic<std::uint32_t> written{0};
    std::array<std::atomic<std::uint64_t>, kReasonCount> rejected{};
};

struct Use {
    std::uint32_t selector;
    bool is_start;
};

struct SlotInfo {
    std::uint32_t image_index;
    std::uint32_t ls;
    std::vector<Use> uses;  // spans that end here first, then those that start here
};

struct Installation {
    std::uint64_t generation = 0;
    SpuCaptureSettings settings;
    std::chrono::steady_clock::time_point origin;
    std::vector<Tox2SpuImageInfo> images;
    std::vector<std::vector<Tox2SpuEntry>> entries;  // the patched entries of the images that have a slot
    std::deque<SelectorState> selectors;
    std::vector<SlotInfo> slots;
};

// The live installation and the original host function of each slot. Written by installSpuCapture before any SPU
// thread runs the table.
Installation* g_active = nullptr;
std::vector<std::unique_ptr<Installation>> g_installations;  // kept: a registered table points into one
std::array<Tox2SpuFunc*, kSpuCaptureMaxSlots> g_hosts{};
std::uint64_t g_generation = 0;

struct Pending {
    bool valid = false;
    Totals totals;
    std::uint64_t dispatches = 0;
    std::uint64_t cycles = 0;
    SpuCapture capture;  // holds the pre state while the span is open
};

struct ThreadState {
    std::uint64_t generation = 0;
    std::vector<Pending> pending;  // per selector
};
thread_local ThreadState t_state;

std::uint64_t msSince(std::chrono::steady_clock::time_point origin) {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count());
}

// Runs of consecutive 16-byte units that share one load record.
void collectTagRuns(const SpuThread& thread, std::vector<SpuCaptureTagRun>& runs) {
    runs.clear();
    const SpuLoadRecord* open = nullptr;
    for (std::uint32_t block = 0; block < kSpuTagBlockCount; block++) {
        if (!thread.blockMayBeTagged(block)) {
            open = nullptr;
            continue;
        }
        for (std::uint32_t u = block * kSpuTagBlockUnits; u < (block + 1) * kSpuTagBlockUnits; u++) {
            const SpuLoadRecord* r = thread.recordAt(u * kSpuTagUnit);
            if (r == nullptr) {
                open = nullptr;
                continue;
            }
            if (open != nullptr && open->image == r->image && open->segment == r->segment && open->delta == r->delta) {
                runs.back().hi = (u + 1) * kSpuTagUnit;
                continue;
            }
            open = r;
            runs.push_back({spuImage(r->image).name, r->segment, r->delta, u * kSpuTagUnit, (u + 1) * kSpuTagUnit});
        }
    }
}

void startSpan(Installation& inst, std::uint32_t selector, std::uint32_t start, SpuThread& thread,
               const Tox2SpuContext* ctx, const std::uint8_t* ls) {
    SelectorState& sel = inst.selectors[selector];
    if (sel.written.load(std::memory_order_relaxed) >= inst.settings.count) return;
    if (static_cast<double>(msSince(inst.origin)) < inst.settings.after_seconds * 1000.0) return;

    Pending& p = t_state.pending[selector];
    SpuCapture& c = p.capture;
    std::memcpy(c.pre_gpr.data(), ctx->gpr, kGprBytes);
    c.pre_fpscr = ctx->fpscr;
    c.pre_srr0 = ctx->srr0;
    c.pre_int_enabled = ctx->int_enabled;
    c.pre_ls.assign(ls, ls + kLsBytes);
    collectTagRuns(thread, c.tag_runs);
    c.start = start;
    c.sp_start = TOX2_VR_W(ctx->gpr[1], 0);
    p.totals = totalsOf(thread);
    p.dispatches = thread.dispatchCount();
    // Last, so the copies above are not part of the span's cycles.
    p.cycles = currentThreadCycles();
    p.valid = true;
}

void logRejections(const SelectorState& sel) {
    const SpuCaptureSelector& spec = sel.spec;
    logInfo("tox2: spu capture: image {} end {:#07x}: rejected spans: {} {}, {} {}, {} {}, {} {}", spec.image, spec.end,
            kReasonNames[kReads], sel.rejected[kReads].load(), kReasonNames[kMfc], sel.rejected[kMfc].load(),
            kReasonNames[kStops], sel.rejected[kStops].load(), kReasonNames[kDispatches],
            sel.rejected[kDispatches].load());
}

void endSpan(Installation& inst, std::uint32_t selector, std::uint32_t end, SpuThread& thread,
             const Tox2SpuContext* ctx, const std::uint8_t* ls) {
    Pending& p = t_state.pending[selector];
    if (!p.valid) return;
    // First, as at the start: the span ends at this dispatch whatever the verdict.
    const std::uint64_t cycles = currentThreadCycles();
    p.valid = false;
    SelectorState& sel = inst.selectors[selector];
    if (sel.written.load(std::memory_order_relaxed) >= inst.settings.count) return;

    const Totals now = totalsOf(thread);
    const std::uint64_t dispatches = thread.dispatchCount() - p.dispatches;
    std::optional<Reason> reason;
    if (now.reads != p.totals.reads) {
        reason = kReads;
    } else if (now.mfc != p.totals.mfc) {
        reason = kMfc;
    } else if (now.stops != p.totals.stops) {
        reason = kStops;
    } else if (sel.spec.max_dispatches != 0 && dispatches > sel.spec.max_dispatches) {
        reason = kDispatches;
    }
    if (reason) {
        sel.rejected[*reason].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uint32_t accepted = sel.accepted.fetch_add(1, std::memory_order_relaxed) + 1;
    if (accepted % inst.settings.every != 0) return;
    const std::uint32_t n = sel.written.fetch_add(1, std::memory_order_relaxed);
    if (n >= inst.settings.count) return;

    const Tox2SpuImageInfo& image = inst.images[sel.image_index];
    SpuCapture& c = p.capture;
    c.image = image.name;
    c.sha256 = image.sha256 != nullptr ? image.sha256 : "";
    c.end = end;
    c.thread_id = thread.id();
    c.span_cycles = cycles - p.cycles;
    c.ms_since_boot = msSince(inst.origin);
    c.dispatches = static_cast<std::uint32_t>(dispatches);
    c.sp_end = TOX2_VR_W(ctx->gpr[1], 0);
    c.flags = 0;
    std::memcpy(c.post_gpr.data(), ctx->gpr, kGprBytes);
    c.post_fpscr = ctx->fpscr;
    c.post_jump_target = ctx->jump_target;
    c.post_ls.assign(ls, ls + kLsBytes);

    const std::filesystem::path path = std::filesystem::path(inst.settings.dir) /
                                       std::format("{}_{:05x}_{:05x}_{}.spucap", c.image, c.start, c.end, n);
    std::string error;
    if (!writeSpuCapture(path, c, error)) {
        logError("tox2: spu capture: cannot write {}: {}", path.string(), error);
        return;
    }
    logInfo("tox2: spu capture: wrote {} image {} start {:#07x} end {:#07x} thread {:#010x} span cycles {} "
            "dispatches {} sp {:#x}/{:#x}",
            path.string(), c.image, c.start, c.end, c.thread_id, c.span_cycles, c.dispatches, c.sp_start, c.sp_end);
    logRejections(sel);
    if (n + 1 == inst.settings.count) {
        logInfo("tox2: spu capture: image {} end {:#07x} complete, {} files", c.image, c.end, inst.settings.count);
    }
}

// All the work of a hit; the trampolines only call it, so their frames stay free of locals with destructors and the
// tail call to the original host function remains a tail call.
TOX2_NOINLINE void onCaptureHit(std::uint32_t slot, Tox2SpuContext* ctx, std::uint8_t* ls) {
    Installation& inst = *g_active;
    if (t_state.generation != inst.generation) {
        t_state.pending.clear();
        t_state.pending.resize(inst.selectors.size());
        t_state.generation = inst.generation;
    }
    SpuThread& thread = *spuThreadOf(ctx);
    const SlotInfo& info = inst.slots[slot];
    for (const Use& use : info.uses) {
        if (use.is_start) {
            startSpan(inst, use.selector, info.ls, thread, ctx, ls);
        } else {
            endSpan(inst, use.selector, info.ls, thread, ctx, ls);
        }
    }
}

template <int Slot>
void captureTrampoline(Tox2SpuContext* __restrict ctx, std::uint8_t* __restrict ls) {
    onCaptureHit(Slot, ctx, ls);
    TOX2_MUSTTAIL return g_hosts[Slot](ctx, ls);
}

template <std::size_t... I>
constexpr std::array<Tox2SpuFunc*, sizeof...(I)> makeTrampolines(std::index_sequence<I...>) {
    return {&captureTrampoline<static_cast<int>(I)>...};
}
constexpr std::array<Tox2SpuFunc*, kSpuCaptureMaxSlots> kTrampolines =
        makeTrampolines(std::make_index_sequence<kSpuCaptureMaxSlots>{});

}  // namespace

// --- File format ---------------------------------------------------------------------------------

std::uint64_t spuCaptureFileSize(std::uint32_t tag_runs) {
    return std::uint64_t{kSpuCaptureHeaderSize} + 2 * (std::uint64_t{kSpuCaptureStateSize} + kLsBytes) +
           std::uint64_t{kSpuCaptureTagRunSize} * tag_runs;
}

bool serializeSpuCapture(const SpuCapture& c, std::vector<std::uint8_t>& out, std::string& error) {
    if (c.pre_ls.size() != kLsBytes || c.post_ls.size() != kLsBytes) {
        error = "the local store images must be 256 KiB";
        return false;
    }
    if (c.image.size() > kSpuCaptureNameSize || c.sha256.size() > kSpuCaptureHashSize) {
        error = "image name or hash is too long";
        return false;
    }
    for (const SpuCaptureTagRun& r : c.tag_runs) {
        if (r.image.size() > kSpuCaptureNameSize) {
            error = "tag run image name is too long";
            return false;
        }
    }
    out.clear();
    out.reserve(spuCaptureFileSize(static_cast<std::uint32_t>(c.tag_runs.size())));
    ByteWriter w(out);
    w.bytes(kMagic, sizeof kMagic);
    w.put(kSpuCaptureVersion);
    w.put(kSpuCaptureHeaderSize);
    w.padded(c.image, kSpuCaptureNameSize);
    w.padded(c.sha256, kSpuCaptureHashSize);
    w.put(c.start);
    w.put(c.end);
    w.put(c.thread_id);
    w.put(static_cast<std::uint32_t>(c.tag_runs.size()));
    w.put(c.span_cycles);
    w.put(c.ms_since_boot);
    w.put(c.dispatches);
    w.put(c.sp_start);
    w.put(c.sp_end);
    w.put(c.flags);

    std::size_t block = w.pos();
    w.bytes(c.pre_gpr.data(), kGprBytes);
    w.bytes(&c.pre_fpscr, sizeof(Tox2Vr));
    w.put(c.pre_srr0);
    w.put(c.pre_int_enabled);
    w.alignTo(block, kSpuCaptureStateSize);
    w.bytes(c.pre_ls.data(), kLsBytes);

    block = w.pos();
    w.bytes(c.post_gpr.data(), kGprBytes);
    w.bytes(&c.post_fpscr, sizeof(Tox2Vr));
    w.put(c.post_jump_target);
    w.alignTo(block, kSpuCaptureStateSize);
    w.bytes(c.post_ls.data(), kLsBytes);

    for (const SpuCaptureTagRun& r : c.tag_runs) {
        w.padded(r.image, kSpuCaptureNameSize);
        w.put(r.segment);
        w.put(r.delta);
        w.put(r.lo);
        w.put(r.hi);
    }
    return true;
}

std::optional<SpuCapture> parseSpuCapture(std::span<const std::uint8_t> bytes, std::string& error) {
    if (bytes.size() < kSpuCaptureHeaderSize || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0) {
        error = "not a span capture (bad magic)";
        return std::nullopt;
    }
    ByteReader r(bytes);
    r.seek(sizeof kMagic);
    const auto version = r.get<std::uint32_t>();
    const auto header_size = r.get<std::uint32_t>();
    if (version != kSpuCaptureVersion || header_size != kSpuCaptureHeaderSize) {
        error = std::format("unsupported capture version {} (header {} bytes)", version, header_size);
        return std::nullopt;
    }
    SpuCapture c;
    c.image = r.padded(kSpuCaptureNameSize);
    c.sha256 = r.padded(kSpuCaptureHashSize);
    c.start = r.get<std::uint32_t>();
    c.end = r.get<std::uint32_t>();
    c.thread_id = r.get<std::uint32_t>();
    const auto tag_runs = r.get<std::uint32_t>();
    if (bytes.size() != spuCaptureFileSize(tag_runs)) {
        error = std::format("file size {} does not match {} tag runs ({} expected)", bytes.size(), tag_runs,
                            spuCaptureFileSize(tag_runs));
        return std::nullopt;
    }
    c.span_cycles = r.get<std::uint64_t>();
    c.ms_since_boot = r.get<std::uint64_t>();
    c.dispatches = r.get<std::uint32_t>();
    c.sp_start = r.get<std::uint32_t>();
    c.sp_end = r.get<std::uint32_t>();
    c.flags = r.get<std::uint32_t>();

    std::size_t block = r.pos();
    r.bytes(c.pre_gpr.data(), kGprBytes);
    r.bytes(&c.pre_fpscr, sizeof(Tox2Vr));
    c.pre_srr0 = r.get<std::uint32_t>();
    c.pre_int_enabled = r.get<std::uint32_t>();
    r.seek(block + kSpuCaptureStateSize);
    c.pre_ls.resize(kLsBytes);
    r.bytes(c.pre_ls.data(), kLsBytes);

    block = r.pos();
    r.bytes(c.post_gpr.data(), kGprBytes);
    r.bytes(&c.post_fpscr, sizeof(Tox2Vr));
    c.post_jump_target = r.get<std::uint32_t>();
    r.seek(block + kSpuCaptureStateSize);
    c.post_ls.resize(kLsBytes);
    r.bytes(c.post_ls.data(), kLsBytes);

    c.tag_runs.resize(tag_runs);
    for (SpuCaptureTagRun& run : c.tag_runs) {
        run.image = r.padded(kSpuCaptureNameSize);
        run.segment = r.get<std::uint32_t>();
        run.delta = r.get<std::int32_t>();
        run.lo = r.get<std::uint32_t>();
        run.hi = r.get<std::uint32_t>();
    }
    return c;
}

bool validateSpuCapture(const SpuCapture& c, const Tox2SpuImageInfo* images, std::uint32_t count, std::string& error) {
    const auto find = [&](std::string_view name) -> const Tox2SpuImageInfo* {
        for (std::uint32_t i = 0; i < count; i++) {
            if (name == images[i].name) return &images[i];
        }
        return nullptr;
    };
    const Tox2SpuImageInfo* image = find(c.image);
    if (image == nullptr) {
        error = std::format("image {} is not in the table", c.image);
        return false;
    }
    if (c.sha256 != (image->sha256 != nullptr ? image->sha256 : "")) {
        error = std::format("image {} has another hash than the table", c.image);
        return false;
    }
    for (const SpuCaptureTagRun& run : c.tag_runs) {
        const Tox2SpuImageInfo* tagged = find(run.image);
        if (tagged == nullptr) {
            error = std::format("tag run image {} is not in the table", run.image);
            return false;
        }
        if (run.segment >= tagged->segment_count || tagged->segments[run.segment].code == 0) {
            error = std::format("tag run names segment {} of {}, which has no code", run.segment, run.image);
            return false;
        }
        if (run.lo >= run.hi || run.hi > kLsBytes || run.lo % kSpuTagUnit != 0 || run.hi % kSpuTagUnit != 0) {
            error = std::format("tag run {:#x}..{:#x} is not a range of whole units in the LS", run.lo, run.hi);
            return false;
        }
    }
    return true;
}

bool writeSpuCapture(const std::filesystem::path& path, const SpuCapture& capture, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!serializeSpuCapture(capture, bytes, error)) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) {
        error = std::format("cannot write {}", path.string());
        return false;
    }
    return true;
}

std::optional<SpuCapture> readSpuCapture(const std::filesystem::path& path, const Tox2SpuImageInfo* images,
                                         std::uint32_t count, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = std::format("cannot open {}", path.string());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = std::format("cannot read {}", path.string());
        return std::nullopt;
    }
    std::optional<SpuCapture> c = parseSpuCapture(bytes, error);
    if (c && !validateSpuCapture(*c, images, count, error)) return std::nullopt;
    return c;
}

// --- Selection -----------------------------------------------------------------------------------

std::optional<SpuCaptureSelector> parseSpuCaptureSelector(std::string_view text, std::string& error) {
    const std::vector<std::string_view> fields = split(text, ':');
    if (fields.size() < 3 || fields.size() > 4) {
        error = "expected IMAGE:S[+S...]:E[:dispatches=N]";
        return std::nullopt;
    }
    SpuCaptureSelector sel;
    if (fields[0].empty()) {
        error = "the image name is empty";
        return std::nullopt;
    }
    sel.image = fields[0];
    for (const std::string_view s : split(fields[1], '+')) {
        std::uint32_t start;
        if (!parseHex32(s, start)) {
            error = std::format("'{}' is not a hex start address", s);
            return std::nullopt;
        }
        sel.starts.push_back(start);
    }
    if (!parseHex32(fields[2], sel.end)) {
        error = std::format("'{}' is not a hex end address", fields[2]);
        return std::nullopt;
    }
    if (fields.size() == 4) {
        constexpr std::string_view kKey = "dispatches=";
        const std::string_view option = fields[3];
        const std::string_view number = option.starts_with(kKey) ? option.substr(kKey.size()) : std::string_view();
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), sel.max_dispatches);
        if (number.empty() || ec != std::errc() || end != number.data() + number.size()) {
            error = std::format("'{}' is not dispatches=N", option);
            return std::nullopt;
        }
    }
    return sel;
}

const Tox2SpuImageInfo* installSpuCapture(const Tox2SpuImageInfo* images, std::uint32_t count,
                                          std::span<const SpuCaptureSelector> selectors,
                                          const SpuCaptureSettings& settings, std::string& error) {
    if (selectors.empty()) return images;
    if (settings.dir.empty() || settings.count < 1 || settings.every < 1 || !(settings.after_seconds >= 0)) {
        error = "a capture needs a directory, a count and an interval of at least 1, and a start time of 0 or more";
        return nullptr;
    }

    auto inst = std::make_unique<Installation>();
    inst->settings = settings;
    inst->images.assign(images, images + count);
    inst->entries.resize(count);

    // (image, entry index) -> slot
    std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>> slot_of;
    const auto findEntry = [&](std::uint32_t image_index, std::uint32_t ls) -> std::optional<std::uint32_t> {
        const Tox2SpuImageInfo& image = images[image_index];
        const Tox2SpuEntry* first = image.entries;
        const Tox2SpuEntry* it = std::lower_bound(first, first + image.entry_count, ls,
                                                  [](const Tox2SpuEntry& e, std::uint32_t v) { return e.ls < v; });
        if (it == first + image.entry_count || it->ls != ls) return std::nullopt;
        return static_cast<std::uint32_t>(it - first);
    };
    const auto slotFor = [&](std::uint32_t image_index, std::uint32_t entry_index) -> std::uint32_t {
        for (const auto& [key, slot] : slot_of) {
            if (key.first == image_index && key.second == entry_index) return slot;
        }
        const auto slot = static_cast<std::uint32_t>(inst->slots.size());
        slot_of.push_back({{image_index, entry_index}, slot});
        inst->slots.push_back({image_index, images[image_index].entries[entry_index].ls, {}});
        return slot;
    };

    // The ends first, so a hit that closes one span and opens another does it in that order.
    std::vector<std::pair<std::uint32_t, Use>> starts;
    for (std::uint32_t s = 0; s < selectors.size(); s++) {
        const SpuCaptureSelector& spec = selectors[s];
        std::optional<std::uint32_t> image_index;
        for (std::uint32_t i = 0; i < count; i++) {
            if (spec.image == images[i].name) image_index = i;
        }
        if (!image_index) {
            error = std::format("unknown SPU image '{}'", spec.image);
            return nullptr;
        }
        if (spec.starts.empty()) {
            error = std::format("image {}: no start address", spec.image);
            return nullptr;
        }
        const auto end_entry = findEntry(*image_index, spec.end);
        if (!end_entry) {
            error = std::format("image {}: {:#x} is not an entry", spec.image, spec.end);
            return nullptr;
        }
        SelectorState& state = inst->selectors.emplace_back();
        state.spec = spec;
        state.image_index = *image_index;
        const std::uint32_t end_slot = slotFor(*image_index, *end_entry);
        inst->slots[end_slot].uses.push_back({s, false});
        for (const std::uint32_t start : spec.starts) {
            const auto start_entry = findEntry(*image_index, start);
            if (!start_entry) {
                error = std::format("image {}: {:#x} is not an entry", spec.image, start);
                return nullptr;
            }
            if (start == spec.end) {
                error = std::format("image {}: {:#x} is both start and end", spec.image, start);
                return nullptr;
            }
            const std::uint32_t slot = slotFor(*image_index, *start_entry);
            const auto same = [&](const std::pair<std::uint32_t, Use>& p) {
                return p.first == slot && p.second.selector == s;
            };
            if (std::none_of(starts.begin(), starts.end(), same)) starts.push_back({slot, {s, true}});
        }
        if (inst->slots.size() > kSpuCaptureMaxSlots) {
            error = std::format("the selectors need {} distinct entries, at most {} are supported", inst->slots.size(),
                                kSpuCaptureMaxSlots);
            return nullptr;
        }
    }
    for (const auto& [slot, use] : starts) inst->slots[slot].uses.push_back(use);

    std::error_code ec;
    std::filesystem::create_directories(settings.dir, ec);
    if (ec) {
        error = std::format("cannot create {}: {}", settings.dir, ec.message());
        return nullptr;
    }

    for (std::uint32_t slot = 0; slot < inst->slots.size(); slot++) {
        const SlotInfo& info = inst->slots[slot];
        std::vector<Tox2SpuEntry>& patched = inst->entries[info.image_index];
        const Tox2SpuImageInfo& image = images[info.image_index];
        if (patched.empty()) patched.assign(image.entries, image.entries + image.entry_count);
        const std::uint32_t entry_index = *findEntry(info.image_index, info.ls);
        g_hosts[slot] = patched[entry_index].host;
        patched[entry_index].host = kTrampolines[slot];
    }
    for (std::uint32_t i = 0; i < count; i++) {
        if (!inst->entries[i].empty()) inst->images[i].entries = inst->entries[i].data();
    }

    inst->generation = ++g_generation;
    inst->origin = std::chrono::steady_clock::now();
    g_active = inst.get();
    g_installations.push_back(std::move(inst));
    return g_active->images.data();
}

}  // namespace tox2

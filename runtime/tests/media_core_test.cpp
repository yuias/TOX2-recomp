// Self-checking tests for the movie pipeline's core: cellSail guest layouts, codec interfaces and the
// service thread, and the PAMF parser and demuxer. No FFmpeg is involved. Only the PAMF game-file
// test reads game data; it is optional and runs only when DAL_GAME_DIR is set.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "hle/sail_service.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"
#include "media/movie_decoder.h"
#include "media/movie_player.h"
#include "media/pamf.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

void testGuestLayouts() {
    namespace sail = dal::sail;

    // Sizes and offsets of the structs the HLE reads and writes.
    CHECK(sail::kVideoFormatSize == 32 && sail::kVideoFormatWidth == 4 && sail::kVideoFormatHeight == 6 &&
          sail::kVideoFormatPitch == 8 && sail::kVideoFormatAlpha == 12 && sail::kVideoFormatColorMatrix == 16 &&
          sail::kVideoFormatReserved3 == 24);
    CHECK(sail::kAudioFormatSize == 24 && sail::kAudioFormatSampleNum == 2 && sail::kAudioFormatFs == 4 &&
          sail::kAudioFormatChLayout == 8 && sail::kAudioFormatReserved1 == 16);
    CHECK(sail::kFrameInfoSize == 24 && sail::kFrameInfoTag == 8 && sail::kFrameInfoStatus == 12 &&
          sail::kFrameInfoPts == 16);
    CHECK(sail::kAllocatorFuncsSize == 8 && sail::kAllocatorSize == 8 && sail::kAllocatorArg == 4);
    CHECK(sail::kSoundAdapterFuncsSize == 12 && sail::kGraphicsAdapterFuncsSize == 20 &&
          sail::kGraphicsAdapterFuncsAlloc == 12 && sail::kGraphicsAdapterFuncsFree == 16);
    CHECK(sail::kPlayerAttributeSize == 32 && sail::kPlayerResourceSize == 16 && sail::kStartCommandSize == 40 &&
          sail::kEventSize == 8);
    CHECK(sail::kPamfReaderSize == 128);

    // The event value is (major << 32) | minor.
    CHECK(sail::makeEvent(sail::kEventPlayerCallCompleted, sail::kCallStart) == 0x000000020000000aull);
    CHECK(sail::makeEvent(sail::kEventPlayerStateChanged, 0) == 0x0000000300000000ull);

    // Event majors, states and call ids the game's callback and the HLE agree on.
    CHECK(sail::kEventErrorOccured == 1 && sail::kEventPlayerCallCompleted == 2 &&
          sail::kEventPlayerStateChanged == 3 && sail::kEventMediaStateChanged == 11);
    CHECK(static_cast<std::uint32_t>(sail::PlayerState::Running) == 6 &&
          static_cast<std::uint32_t>(sail::PlayerState::Lost) == 9);
    CHECK(sail::kCallBoot == 1 && sail::kCallOpenStream == 2 && sail::kCallStart == 10 &&
          sail::kCallReopenEsUser == 15);

    // The game consumes a frame only for a status word >= 0x100.
    CHECK(sail::kSyncFrameConsumedMin == 0x100 && sail::kSyncNextOnTime == 0x204);
    CHECK(sail::kSyncNoFrame < sail::kSyncFrameConsumedMin);

    CHECK(sail::kVideoCodingArgbInterleaved == 0 && sail::kVideoCodingYuv420Planar == 3);
    CHECK(sail::kVideoColorRangeFull == 0 && sail::kVideoColorRangeLimited == 1);
    CHECK(sail::kAudioCodingLpcmFloat32 == 1 && sail::kAudioChLayout2ChLr == 2);

    CHECK(sail::kErrorInvalidArg == 0x80610701 && sail::kErrorInvalidState == 0x80610702 &&
          sail::kErrorFatal == 0x806107FF);
    CHECK(sail::kPamfErrorStreamNotFound == 0x80610501 && sail::kPamfErrorNotAvailable == 0x80610508);
    CHECK(sail::kPamfStreamTypeVideo == 20 && sail::kPamfStreamTypeAudio == 21);
    CHECK(static_cast<int>(dal::PamfCodec::Avc) == sail::kPamfStreamTypeAvc &&
          static_cast<int>(dal::PamfCodec::Atrac3plus) == sail::kPamfStreamTypeAtrac3plus &&
          static_cast<int>(dal::PamfCodec::UserData) == sail::kPamfStreamTypeUserData);
}

class CountingFactory : public dal::MediaCodecFactory {
public:
    explicit CountingFactory(int* destroyed) : destroyed_(destroyed) {}
    ~CountingFactory() override { ++*destroyed_; }
    std::unique_ptr<dal::VideoDecoder> createVideo(const dal::PamfStream&, const dal::VideoOutputFormat&,
                                                   std::string& error) override {
        error = "no video";
        return nullptr;
    }
    std::unique_ptr<dal::AudioDecoder> createAudio(const dal::PamfStream&, const dal::AudioOutputFormat&,
                                                   std::string& error) override {
        error = "no audio";
        return nullptr;
    }

private:
    int* destroyed_;
};

void testCodecFactory() {
    CHECK(dal::mediaCodecFactory() == nullptr);

    int destroyed_a = 0, destroyed_b = 0;
    auto a = std::make_unique<CountingFactory>(&destroyed_a);
    dal::MediaCodecFactory* raw_a = a.get();
    dal::setMediaCodecFactory(std::move(a));
    CHECK(dal::mediaCodecFactory() == raw_a);

    std::string error;
    CHECK(dal::mediaCodecFactory()->createVideo({}, {}, error) == nullptr && error == "no video");
    CHECK(dal::mediaCodecFactory()->createAudio({}, {}, error) == nullptr && error == "no audio");

    // Replacing destroys the previous factory.
    auto b = std::make_unique<CountingFactory>(&destroyed_b);
    dal::MediaCodecFactory* raw_b = b.get();
    dal::setMediaCodecFactory(std::move(b));
    CHECK(dal::mediaCodecFactory() == raw_b && destroyed_a == 1 && destroyed_b == 0);

    dal::setMediaCodecFactory(nullptr);
    CHECK(dal::mediaCodecFactory() == nullptr && destroyed_b == 1);
}

struct ServiceCall {
    int id;
    bool on_service_thread;  // name, service flag and a TLS pointer of its own
    std::uint64_t guest_result;
    std::uint32_t thread_id;
};
std::vector<ServiceCall> g_calls;  // written under the guest lock
std::condition_variable_any g_calls_cv;

DAL_PPU_FUNC(sailFakeGuestFunction) {
    (void)base;
    ctx->gpr[3] = ctx->gpr[3] * 2;
}

template <class Pred>
bool waitUntil(Pred done) {
    return dal::blockingWait(g_calls_cv, done, "test wait", std::chrono::seconds(5));
}

void pauseUnlocked() {
    std::condition_variable_any cv;
    dal::blockingWait(cv, [] { return false; }, "test pause", std::chrono::milliseconds(2));
}

dal::SailCall recordingCall(int id, std::uint32_t guest_opd, std::function<void()> then = {}) {
    return [id, guest_opd, then = std::move(then)](DalPpuContext* ctx, std::uint8_t* base) {
        const dal::GuestThread* thread = dal::currentThread();
        const bool on_service = thread != nullptr && thread->service() && thread->name() == "_libsail-control" &&
                                ctx->gpr[13] == thread->tlsR13() && ctx->gpr[13] != 0;
        // The service thread can call guest functions through an OPD, as the cellSail callbacks do.
        const std::uint64_t result = dal::callGuestFunction(ctx, base, guest_opd, {static_cast<std::uint64_t>(id)});
        g_calls.push_back({id, on_service, result, thread != nullptr ? thread->id() : 0});
        if (then) then();
        g_calls_cv.notify_all();
    };
}

void testSailService() {
    dal::setLogSink([](std::string_view) {});
    dal::resetCensus();
    dal::resetLogOnce();
    dal::resetSyncObjects();
    dal::resetHostFunctions();
    dal::Process& p = dal::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    const std::uint32_t tls_image = scratch + 0x100;
    g_calls.clear();

    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    auto boot_ctx = std::make_unique<DalPpuContext>();
    dalWrite32(base, tls_image, 0xCAFEF00D);
    CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
    const std::uint32_t guest_opd = dal::registerHostFunction("sailFakeGuestFunction", sailFakeGuestFunction);
    CHECK(guest_opd != 0);

    // The thread cannot run while this test holds the guest lock, so all of these queue up. The first
    // call queues one more from the service thread itself, which runs after the three already queued.
    dal::queueSailCall(recordingCall(1, guest_opd, [guest_opd] { dal::queueSailCall(recordingCall(4, guest_opd)); }));
    dal::queueSailCall(recordingCall(2, guest_opd));
    dal::queueSailCall(recordingCall(3, guest_opd));
    CHECK(g_calls.empty());
    CHECK(waitUntil([] { return g_calls.size() >= 4; }));
    CHECK(g_calls.size() == 4);
    if (g_calls.size() == 4) {
        for (int i = 0; i < 4; i++) {
            CHECK(g_calls[i].id == i + 1);
            CHECK(g_calls[i].on_service_thread);
            CHECK(g_calls[i].guest_result == static_cast<std::uint64_t>(2 * (i + 1)));
        }
    }

    // Idle, the thread is a service thread: the census says so and the supervisor does not count it.
    const dal::GuestThread* thread = g_calls.empty() ? nullptr : dal::findThread(g_calls[0].thread_id);
    CHECK(thread != nullptr);
    if (thread == nullptr) return;
    for (int i = 0; i < 2500 && thread->state() != dal::ThreadState::Blocked; i++) pauseUnlocked();
    CHECK(thread->state() == dal::ThreadState::Blocked && thread->service());
    CHECK(dal::formatCensus().find("_libsail-control: blocked in sail control wait [service]") != std::string::npos);
    const dal::ThreadHealth service_health{thread->state(), thread->indefiniteWait(), thread->blockedSinceMs(),
                                           thread->service()};
    const std::int64_t later = service_health.blocked_since_ms + 10000;
    CHECK(dal::judgeThreads({service_health}, later) == dal::ThreadVerdict::NoThreads);
    const dal::ThreadHealth running_game{dal::ThreadState::Running, false, 0, false};
    CHECK(dal::judgeThreads({service_health, running_game}, later) == dal::ThreadVerdict::Ok);

    // stopSailService drops calls that have not run and joins the thread.
    const std::size_t before_stop = g_calls.size();
    dal::queueSailCall(recordingCall(5, guest_opd));
    dal::queueSailCall(recordingCall(6, guest_opd));
    dal::stopSailService();
    CHECK(thread->state() == dal::ThreadState::Exited);
    CHECK(g_calls.size() == before_stop);

    // A later call starts a new thread.
    dal::queueSailCall(recordingCall(7, guest_opd));
    CHECK(waitUntil([&] { return g_calls.size() > before_stop; }));
    CHECK(g_calls.size() == before_stop + 1 && g_calls.back().id == 7 && g_calls.back().on_service_thread);
    dal::stopSailService();

    dal::resetHostFunctions();
    dal::resetSyncObjects();
    p.main.free(scratch);
    dal::setLogSink({});
    dal::resetCensus();
    dal::resetLogOnce();
}

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& v, std::size_t off, std::uint32_t x) {
    v[off] = static_cast<std::uint8_t>(x >> 8);
    v[off + 1] = static_cast<std::uint8_t>(x);
}
void put32(Bytes& v, std::size_t off, std::uint32_t x) {
    put16(v, off, x >> 16);
    put16(v, off + 2, x & 0xFFFF);
}

// A two-stream header laid out like the game's movie headers: AVC 1280x720 on 0xE0 and
// ATRAC3plus 2 channels 48 kHz on private stream 1 / sub-stream 0.
Bytes makePamfHeader(std::uint32_t data_units) {
    Bytes h(2048, 0);
    std::memcpy(h.data(), "PAMF0041", 8);
    put32(h, 0x08, 1);
    put32(h, 0x0C, data_units);
    put32(h, 0x50, 0x94);
    put32(h, 0x58, 89310);
    put32(h, 0x5E, 722910);
    put32(h, 0x62, 120000);
    put32(h, 0x66, 67500);
    put32(h, 0x6A, 2);
    h[0x6F] = 1;
    put32(h, 0x70, 0x74);
    put32(h, 0x76, 89310);
    put32(h, 0x7C, 722910);
    h[0x81] = 1;
    put32(h, 0x82, 0x62);
    h[0x87] = 2;
    const std::uint8_t video[] = {0x1B, 0, 0, 0, 0xE0, 0, 0x33, 0x3B};
    std::memcpy(&h[0x88], video, sizeof(video));
    h[0x88 + 0x10 + 0x09] = 0x50;
    h[0x88 + 0x10 + 0x0B] = 0x2D;
    const std::uint8_t audio[] = {0xDC, 0, 0, 0, 0xBD, 0x00, 0x20, 0x14};
    std::memcpy(&h[0xB8], audio, sizeof(audio));
    h[0xB8 + 0x10 + 0x02] = 2;
    h[0xB8 + 0x10 + 0x03] = 1;
    return h;
}

void testPamfHeader() {
    const Bytes h = makePamfHeader(7);
    auto info = dal::parsePamfHeader(h);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->header_size == 2048 && info->data_offset == 2048 && info->data_size == 7 * 2048);
    CHECK(info->start_pts == 89310 && info->end_pts == 722910 && info->mux_rate_bound == 120000);
    CHECK(info->streams.size() == 2);
    if (info->streams.size() != 2) return;
    const dal::PamfStream& v = info->streams[0];
    CHECK(v.codec == dal::PamfCodec::Avc && v.stream_id == 0xE0 && v.channel == 0 && v.width == 1280 &&
          v.height == 720 && v.codec_info.size() == 32);
    const dal::PamfStream& a = info->streams[1];
    CHECK(a.codec == dal::PamfCodec::Atrac3plus && a.stream_id == 0xBD && a.private_stream_id == 0 && a.channels == 2 &&
          a.sample_rate == 48000);

    // Only the sequence information is needed, not the whole 2048-byte header.
    CHECK(dal::parsePamfHeader(std::span<const std::uint8_t>(h).first(0xE8)).has_value());

    auto fails = [&](auto mutate) {
        Bytes bad = h;
        mutate(bad);
        return !dal::parsePamfHeader(bad).has_value();
    };
    CHECK(!dal::parsePamfHeader(std::span<const std::uint8_t>(h).first(0xE0)).has_value());
    CHECK(!dal::parsePamfHeader({}).has_value());
    CHECK(fails([](Bytes& b) { b[0] = 'X'; }));
    CHECK(fails([](Bytes& b) { b[7] = '9'; }));
    CHECK(fails([](Bytes& b) { put32(b, 0x08, 0); }));
    CHECK(fails([](Bytes& b) { put32(b, 0x50, 0xFFFFFF); }));
    CHECK(fails([](Bytes& b) { put32(b, 0x6A, 3); }));
    CHECK(fails([](Bytes& b) { put32(b, 0x5E, 80000); }));
    CHECK(fails([](Bytes& b) { b[0xB8] = 0x55; }));
    CHECK(fails([](Bytes& b) { b[0xB8 + 5] = 0x40; }));  // LPCM sub-stream on an ATRAC3plus entry
    CHECK(fails([](Bytes& b) {
        b[0xB8] = 0x1B;
        b[0xB8 + 4] = 0xE0;
    }));  // second AVC stream on 0xE0
    CHECK(fails([](Bytes& b) {
        b[0xB8] = 0x02;
        b[0xB8 + 4] = 0xE0;
    }));                                                    // MPEG-2 video on the same stream id
    CHECK(fails([](Bytes& b) { put32(b, 0x76, 89311); }));  // grouping period start differs from the start PTS
    CHECK(fails([](Bytes& b) { put32(b, 0x70, 0x75); }));   // period does not fit in the sequence info
    CHECK(fails([](Bytes& b) { put32(b, 0x82, 0x63); }));   // group does not fit in the period
}

struct NotifyEvent {
    std::uint64_t arg, event, arg0, arg1;
    bool on_service;
};
std::vector<NotifyEvent> g_notify_events;  // written under the guest lock

struct AllocatorCall {
    std::uint64_t arg, boundary, value;  // size for pAlloc, pointer for pFree
};
std::vector<AllocatorCall> g_alloc_calls, g_free_calls;
std::uint32_t g_alloc_next = 0;

// The player's notify callback: (arg, event, arg0, arg1).
DAL_PPU_FUNC(sailFakeNotify) {
    (void)base;
    const dal::GuestThread* thread = dal::currentThread();
    const bool on_service = thread != nullptr && thread->service() && thread->name() == "_libsail-control";
    g_notify_events.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], on_service});
    g_calls_cv.notify_all();
}

// The game's allocator callbacks: pAlloc(arg, boundary, size) and pFree(arg, boundary, pointer).
DAL_PPU_FUNC(sailFakeAlloc) {
    (void)base;
    g_alloc_calls.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]});
    ctx->gpr[3] = g_alloc_next;
    g_alloc_next += 0x100;
}

DAL_PPU_FUNC(sailFakeFree) {
    (void)base;
    g_free_calls.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]});
    ctx->gpr[3] = 0;
}

dal::GuestHandler sailHandlerNamed(std::string_view name) {
    for (const auto& list : {dal::cellSailPlayerHandlers(), dal::cellSailAdapterHandlers(), dal::cellPamfHandlers()}) {
        for (const dal::HleEntry& entry : list) {
            if (name == entry.name) return entry.handler;
        }
    }
    return nullptr;
}

void testSailPlayerHandlers() {
    namespace sail = dal::sail;
    dal::setLogSink([](std::string_view) {});
    dal::resetCensus();
    dal::resetLogOnce();
    dal::resetSyncObjects();
    dal::resetHostFunctions();
    dal::resetCellSail();
    dal::Process& p = dal::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
    CHECK(scratch != 0);
    g_notify_events.clear();
    g_alloc_calls.clear();
    g_free_calls.clear();
    g_alloc_next = scratch + 0x4000;

    // Guest objects the handlers read or write.
    const std::uint32_t tls_image = scratch + 0x100, allocator = scratch + 0x1000, funcs = scratch + 0x1010,
                        attribute = scratch + 0x1020, resource = scratch + 0x1040, sound_funcs = scratch + 0x1060,
                        gfx_funcs = scratch + 0x1070, video_format = scratch + 0x1100, audio_format = scratch + 0x1140,
                        format_out = scratch + 0x1180, uri = scratch + 0x1200, uri_bad = scratch + 0x1300,
                        uri_missing = scratch + 0x1380, pp_desc = scratch + 0x1480, database = scratch + 0x1500,
                        caps = scratch + 0x1600, command = scratch + 0x1640, player = scratch + 0x2000,
                        player_b = scratch + 0x2400, sound = scratch + 0x2800, gfx = scratch + 0x2900,
                        stack = scratch + 0x9000;
    constexpr std::uint64_t kAllocArg = 0xA11C, kNotifyArg = 0xCB;

    std::error_code ec;
    const std::filesystem::path movie_path = std::filesystem::temp_directory_path(ec) / "dal_sail_player_test.pam";
    {
        const Bytes header = makePamfHeader(0);
        std::ofstream out(movie_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    }
    const std::string movie_guest = "/dev_bdvd/PS3_GAME/USRDIR/Movie/dal_sail_player_test.pam";
    dal::VfsLayout layout;
    layout.files.push_back({movie_guest, movie_path, true});
    dal::configureVfs(layout);
    auto putString = [&](std::uint32_t addr, const std::string& text) {
        std::memcpy(base + addr, text.c_str(), text.size() + 1);
    };
    putString(uri, "x-cell-fs://" + movie_guest);
    putString(uri_bad, "http://example.invalid/movie.pam");
    putString(uri_missing, "x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/Movie/dal_sail_missing.pam");

    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    auto boot_ctx = std::make_unique<DalPpuContext>();
    dalWrite32(base, tls_image, 0xCAFEF00D);
    CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
    DalPpuContext* ctx = boot_ctx.get();
    ctx->gpr[1] = stack;
    const std::uint32_t notify_opd = dal::registerHostFunction("sailFakeNotify", sailFakeNotify);
    const std::uint32_t alloc_opd = dal::registerHostFunction("sailFakeAlloc", sailFakeAlloc);
    const std::uint32_t free_opd = dal::registerHostFunction("sailFakeFree", sailFakeFree);
    CHECK(notify_opd != 0 && alloc_opd != 0 && free_opd != 0);

    const auto call = [&](std::string_view name, std::initializer_list<std::uint64_t> args) {
        const dal::GuestHandler handler = sailHandlerNamed(name);
        CHECK(handler != nullptr);
        if (handler == nullptr) return std::uint32_t{0xdeadbeef};
        int reg = 3;
        for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
        handler(ctx, base);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto waitForEvents = [&](std::size_t count) {
        return waitUntil([&] { return g_notify_events.size() >= count; });
    };
    const auto stateEvent = [](std::uint32_t state) { return std::pair{sail::makeEvent(3, 0), std::uint64_t{state}}; };
    const auto eventIs = [&](std::size_t index, std::pair<std::uint64_t, std::uint64_t> expected) {
        return index < g_notify_events.size() && g_notify_events[index].event == expected.first &&
               g_notify_events[index].arg0 == expected.second && g_notify_events[index].arg == kNotifyArg &&
               g_notify_events[index].on_service;
    };

    // Allocator object, then Initialize2: the allocator is copied and the INITIALIZED state is
    // reported from the service thread.
    dalWrite32(base, funcs, alloc_opd);
    dalWrite32(base, funcs + 4, free_opd);
    CHECK(call("cellSailMemAllocatorInitialize", {allocator, funcs, kAllocArg}) == 0);
    CHECK(dalRead32(base, allocator) == funcs && dalRead32(base, allocator + 4) == kAllocArg);
    CHECK(call("cellSailMemAllocatorInitialize", {0, funcs, kAllocArg}) == sail::kErrorInvalidArg);
    dalWrite32(base, attribute, 6);
    dalWrite32(base, attribute + 4, 1);
    dalWrite32(base, attribute + 8, 1);
    CHECK(call("cellSailPlayerInitialize2", {player, allocator, notify_opd, kNotifyArg, attribute, resource}) == 0);
    CHECK(g_notify_events.empty());  // delivery is asynchronous
    CHECK(waitForEvents(1) && eventIs(0, stateEvent(0)));
    CHECK(call("cellSailPlayerInitialize2", {player, allocator, notify_opd, kNotifyArg, attribute, resource}) ==
          sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerInitialize2", {0, allocator, notify_opd, kNotifyArg, attribute, resource}) ==
          sail::kErrorInvalidArg);

    // Adapters: initialise once, preferred format round trip, registration with the player.
    for (std::uint32_t i = 0; i < 3; i++) dalWrite32(base, sound_funcs + 4 * i, 0x1000 + i);
    for (std::uint32_t i = 0; i < 5; i++) dalWrite32(base, gfx_funcs + 4 * i, 0x2000 + i);
    CHECK(call("cellSailSoundAdapterInitialize", {sound, sound_funcs, kNotifyArg}) == 0);
    CHECK(call("cellSailSoundAdapterInitialize", {sound, sound_funcs, kNotifyArg}) == sail::kErrorInvalidState);
    CHECK(call("cellSailGraphicsAdapterInitialize", {gfx, gfx_funcs, kNotifyArg}) == 0);
    CHECK(call("cellSailGraphicsAdapterInitialize", {gfx, gfx_funcs, kNotifyArg}) == sail::kErrorInvalidState);
    CHECK(call("cellSailGraphicsAdapterInitialize", {gfx + 0x100, 0, kNotifyArg}) == sail::kErrorInvalidArg);
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailGraphicsAdapterGetFormat", {gfx, format_out}) == 0);
    CHECK(dalRead8(base, format_out) == 0xff);  // nothing preferred yet: unspecified
    std::memset(base + video_format, 0xff, sail::kVideoFormatSize);
    dalWrite8(base, video_format, sail::kVideoCodingArgbInterleaved);
    dalWrite32(base, video_format + sail::kVideoFormatAlpha, 255);
    CHECK(call("cellSailGraphicsAdapterSetPreferredFormat", {gfx, video_format}) == 0);
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailGraphicsAdapterGetFormat", {gfx, format_out}) == 0);
    CHECK(std::memcmp(base + format_out, base + video_format, sail::kVideoFormatSize) == 0);
    std::memset(base + audio_format, 0xff, sail::kAudioFormatSize);
    dalWrite8(base, audio_format, sail::kAudioCodingLpcmFloat32);
    dalWrite8(base, audio_format + 1, 2);
    dalWrite32(base, audio_format + sail::kAudioFormatFs, 48000);
    CHECK(call("cellSailSoundAdapterSetPreferredFormat", {sound, audio_format}) == 0);
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailSoundAdapterGetFormat", {sound, format_out}) == 0);
    CHECK(std::memcmp(base + format_out, base + audio_format, sail::kAudioFormatSize) == 0);
    CHECK(call("cellSailSoundAdapterSetPreferredFormat", {sound + 0x100, audio_format}) == sail::kErrorInvalidState);

    CHECK(call("cellSailPlayerSetSoundAdapter", {player, 0, sound}) == 0);
    CHECK(call("cellSailPlayerSetGraphicsAdapter", {player, 0, gfx}) == 0);
    CHECK(call("cellSailPlayerSetGraphicsAdapter", {player, 2, gfx}) == sail::kErrorInvalidArg);
    CHECK(call("cellSailPlayerSetGraphicsAdapter", {player, 0, gfx + 0x100}) == sail::kErrorInvalidState);
    CHECK(call("cellSailSoundAdapterFinalize", {sound}) == sail::kErrorInvalidState);  // registered
    CHECK(call("cellSailGraphicsAdapterFinalize", {gfx}) == sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerSetRepeatMode", {player, 1, command}) == 0);
    CHECK(call("cellSailPlayerSetRepeatMode", {player_b, 1, command}) == sail::kErrorInvalidState);

    // Boot: BOOT_TRANSITION, CLOSED, then CALL_COMPLETED(BOOT), in order and only from INITIALIZED.
    CHECK(call("cellSailPlayerBoot", {player, 0}) == 0);
    CHECK(waitForEvents(4));
    CHECK(g_notify_events.size() == 4 && eventIs(1, stateEvent(1)) && eventIs(2, stateEvent(2)) &&
          eventIs(3, {sail::makeEvent(2, sail::kCallBoot), 0}));
    CHECK(call("cellSailPlayerBoot", {player, 0}) == sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerBoot", {player_b, 0}) == sail::kErrorInvalidState);

    // Descriptor: the block comes from the game's allocator and the header is read through the VFS.
    CHECK(call("cellSailPlayerCreateDescriptor", {player, 0, 0, uri_bad, pp_desc}) == sail::kErrorUnsupportedStream);
    CHECK(dalRead32(base, pp_desc) == 0 && g_alloc_calls.empty());
    CHECK(call("cellSailPlayerCreateDescriptor", {player, 1, 0, uri, pp_desc}) == sail::kErrorUnsupportedStream);
    CHECK(call("cellSailPlayerCreateDescriptor", {player, 0, 0, uri, pp_desc}) == 0);
    const std::uint32_t desc = dalRead32(base, pp_desc);
    CHECK(desc == scratch + 0x4000 && g_alloc_calls.size() == 1);
    if (g_alloc_calls.size() == 1) {
        CHECK(g_alloc_calls[0].arg == kAllocArg && g_alloc_calls[0].boundary == 0x10 &&
              g_alloc_calls[0].value == sail::kDescriptorMaxSize);
    }
    CHECK(call("cellSailPlayerAddDescriptor", {player, desc}) == 0);
    CHECK(call("cellSailPlayerAddDescriptor", {player, desc}) == sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerAddDescriptor", {player, desc + 0x10}) == sail::kErrorInvalidArg);
    CHECK(call("cellSailPlayerDestroyDescriptor", {player, desc}) == sail::kErrorInvalidState);  // still added

    // Database: one video and one audio stream for the synthetic header.
    std::memset(base + database, 0xaa, sail::kPamfReaderSize);
    CHECK(call("cellSailDescriptorCreateDatabase", {desc, database, 0x40, 0}) == sail::kErrorInvalidArg);
    CHECK(call("cellSailDescriptorCreateDatabase", {desc, database, 0x80, 0}) == 0);
    const auto streams = [&](std::uint32_t type) {
        return call("cellPamfReaderGetNumberOfSpecificStreams", {database, type});
    };
    CHECK(streams(sail::kPamfStreamTypeVideo) == 1 && streams(sail::kPamfStreamTypeAudio) == 1);
    CHECK(streams(sail::kPamfStreamTypeAvc) == 1 && streams(sail::kPamfStreamTypeAtrac3plus) == 1);
    CHECK(streams(sail::kPamfStreamTypeM2v) == 0 && streams(sail::kPamfStreamTypeLpcm) == 0 &&
          streams(sail::kPamfStreamTypeAc3) == 0 && streams(sail::kPamfStreamTypeUserData) == 0);
    CHECK(streams(sail::kPamfStreamTypePsmfAvc) == 0 && streams(sail::kPamfStreamTypeUnknown) == 0 && streams(99) == 0);
    // The selector is a u8: higher bits of the register are ignored.
    CHECK(call("cellPamfReaderGetNumberOfSpecificStreams", {database, 0x100000000ull | 21}) == 1);
    CHECK(call("cellPamfReaderGetNumberOfSpecificStreams", {format_out, 21}) == 0);  // not a database

    // Capabilities: four word outputs, all reported clear.
    for (std::uint32_t i = 0; i < 4; i++) dalWrite32(base, caps + 4 * i, 0xdeadbeef);
    CHECK(call("cellSailDescriptorGetCapabilities", {desc, caps, caps + 4, caps + 8, caps + 12}) == 0);
    CHECK(dalRead32(base, caps) == 0 && dalRead32(base, caps + 12) == 0);
    CHECK(call("cellSailDescriptorGetCapabilities", {desc, 0, 0, 0, 0}) == 0);

    CHECK(call("cellSailDescriptorDestroyDatabase", {desc, database}) == 0);
    CHECK(call("cellSailDescriptorDestroyDatabase", {desc, database}) == sail::kErrorInvalidArg);
    CHECK(streams(sail::kPamfStreamTypeVideo) == 0);

    // A file that is missing still gets a descriptor; the database call reports why.
    CHECK(call("cellSailPlayerCreateDescriptor", {player, 0, 0, uri_missing, pp_desc}) == 0);
    const std::uint32_t missing = dalRead32(base, pp_desc);
    CHECK(missing != 0 && missing != desc);
    CHECK(call("cellSailDescriptorCreateDatabase", {missing, database, 0x80, 0}) == sail::kErrorInvalidFd);
    CHECK(call("cellSailPlayerDestroyDescriptor", {player, missing}) == 0);  // never added
    CHECK(g_free_calls.size() == 1 && g_free_calls[0].value == missing && g_free_calls[0].boundary == 0x10 &&
          g_free_calls[0].arg == kAllocArg);

    // Finalize waits for the queued events: the INITIALIZED report of a second player is delivered
    // before it returns, and nothing follows.
    const std::size_t before_b = g_notify_events.size();
    CHECK(call("cellSailPlayerInitialize2", {player_b, allocator, notify_opd, kNotifyArg, attribute, resource}) == 0);
    CHECK(call("cellSailPlayerFinalize", {player_b}) == 0);
    CHECK(g_notify_events.size() == before_b + 1);
    CHECK(call("cellSailPlayerFinalize", {player_b}) == sail::kErrorInvalidState);

    // Finalize of the booted player releases its adapters and descriptors.
    const std::size_t before_finalize = g_notify_events.size();
    CHECK(call("cellSailPlayerFinalize", {player}) == 0);
    CHECK(g_notify_events.size() == before_finalize);
    CHECK(call("cellSailPlayerBoot", {player, 0}) == sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerDestroyDescriptor", {player, desc}) == 0);
    CHECK(g_free_calls.size() == 2 && g_free_calls[1].value == desc);
    CHECK(call("cellSailPlayerDestroyDescriptor", {player, desc}) == sail::kErrorInvalidArg);
    CHECK(call("cellSailSoundAdapterFinalize", {sound}) == 0);
    CHECK(call("cellSailGraphicsAdapterFinalize", {gfx}) == 0);
    CHECK(call("cellSailGraphicsAdapterFinalize", {gfx}) == sail::kErrorInvalidState);
    // The address can be initialised again once the old state is gone.
    CHECK(call("cellSailPlayerInitialize2", {player, allocator, notify_opd, kNotifyArg, attribute, resource}) == 0);
    CHECK(call("cellSailPlayerFinalize", {player}) == 0);

    dal::stopSailService();
    dal::resetCellSail();
    dal::configureVfs(dal::VfsLayout{});
    std::filesystem::remove(movie_path, ec);
    dal::resetHostFunctions();
    dal::resetSyncObjects();
    p.main.free(scratch);
    dal::setLogSink({});
    dal::resetCensus();
    dal::resetLogOnce();
}

struct FrameAllocCall {
    std::uint64_t arg, size, num, pointers;
};
struct FrameFreeCall {
    std::uint64_t arg, num;
    std::vector<std::uint32_t> pointers;
};
struct FormatChangedCall {
    std::uint64_t arg, format, session;
};
std::vector<FrameAllocCall> g_frame_allocs;
std::vector<FrameFreeCall> g_frame_frees;
std::vector<FormatChangedCall> g_format_changes;
std::vector<std::uint32_t> g_frame_buffers;  // what pAlloc returned
std::uint32_t g_bump_next = 0;

std::uint32_t bumpAllocate(std::uint64_t size) {
    const std::uint32_t result = g_bump_next;
    g_bump_next += static_cast<std::uint32_t>((size + 0xff) & ~0xffull);
    return result;
}

// The game's allocator callback: pAlloc(arg, boundary, size).
DAL_PPU_FUNC(sailFakeBumpAlloc) {
    (void)base;
    g_alloc_calls.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]});
    ctx->gpr[3] = bumpAllocate(ctx->gpr[5]);
}

// The graphics adapter's frame callbacks: pAlloc(arg, size, num, ppFrame) and pFree(arg, num, ppFrame).
DAL_PPU_FUNC(sailFakeFrameAlloc) {
    g_frame_allocs.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]});
    for (std::uint32_t i = 0; i < ctx->gpr[5]; i++) {
        const std::uint32_t buffer = bumpAllocate(ctx->gpr[4]);
        g_frame_buffers.push_back(buffer);
        dalWrite32(base, static_cast<std::uint32_t>(ctx->gpr[6]) + 4 * i, buffer);
    }
    ctx->gpr[3] = 0;
}

DAL_PPU_FUNC(sailFakeFrameFree) {
    FrameFreeCall call{ctx->gpr[3], ctx->gpr[4], {}};
    for (std::uint32_t i = 0; i < ctx->gpr[4]; i++) {
        call.pointers.push_back(dalRead32(base, static_cast<std::uint32_t>(ctx->gpr[5]) + 4 * i));
    }
    g_frame_frees.push_back(std::move(call));
    ctx->gpr[3] = 0;
}

// pFormatChanged(arg, format, sessionId) of either adapter.
DAL_PPU_FUNC(sailFakeFormatChanged) {
    (void)base;
    g_format_changes.push_back({ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]});
    ctx->gpr[3] = 0;
}

// A header like makePamfHeader's whose movie lasts 0.3 s, so the wall-clock end arrives quickly.
Bytes makeShortPamfHeader() {
    Bytes h = makePamfHeader(0);
    put32(h, 0x5E, 89310 + 27000);
    put32(h, 0x7C, 89310 + 27000);
    return h;
}

void testSailPlayback() {
    namespace sail = dal::sail;
    dal::setLogSink([](std::string_view) {});
    dal::resetCensus();
    dal::resetLogOnce();
    dal::resetSyncObjects();
    dal::resetHostFunctions();
    dal::resetCellSail();
    dal::setMediaCodecFactory(nullptr);  // black frames: no FFmpeg here
    dal::Process& p = dal::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    constexpr std::uint32_t kArena = 0x2000000;  // two streams of three frames of 3,686,400 bytes, and the small blocks
    const std::uint32_t scratch = p.main.allocate(kArena, 0x10000);
    CHECK(scratch != 0);
    g_notify_events.clear();
    g_alloc_calls.clear();
    g_free_calls.clear();
    g_frame_allocs.clear();
    g_frame_frees.clear();
    g_format_changes.clear();
    g_frame_buffers.clear();
    g_bump_next = scratch + 0x20000;

    const std::uint32_t tls_image = scratch + 0x100, allocator = scratch + 0x1000, funcs = scratch + 0x1010,
                        attribute = scratch + 0x1020, resource = scratch + 0x1040, sound_funcs = scratch + 0x1060,
                        gfx_funcs = scratch + 0x1070, video_format = scratch + 0x1100, audio_format = scratch + 0x1140,
                        format_out = scratch + 0x1180, uri = scratch + 0x1200, uri_missing = scratch + 0x1380,
                        pp_desc = scratch + 0x1480, database = scratch + 0x1500, command = scratch + 0x1640,
                        info = scratch + 0x1700, player = scratch + 0x2000, player_b = scratch + 0x2400,
                        sound = scratch + 0x2800, gfx = scratch + 0x2900, sound_b = scratch + 0x2a00,
                        gfx_b = scratch + 0x2b00, stack = scratch + 0x9000;
    constexpr std::uint64_t kAllocArg = 0xA11C, kNotifyArg = 0xCB, kSoundArg = 0x5D, kGfxArg = 0x6F;

    std::error_code ec;
    const std::filesystem::path movie_path = std::filesystem::temp_directory_path(ec) / "dal_sail_playback_test.pam";
    {
        const Bytes header = makeShortPamfHeader();
        std::ofstream out(movie_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    }
    const std::string movie_guest = "/dev_bdvd/PS3_GAME/USRDIR/Movie/dal_sail_playback_test.pam";
    dal::VfsLayout layout;
    layout.files.push_back({movie_guest, movie_path, true});
    dal::configureVfs(layout);
    auto putString = [&](std::uint32_t addr, const std::string& text) {
        std::memcpy(base + addr, text.c_str(), text.size() + 1);
    };
    putString(uri, "x-cell-fs://" + movie_guest);
    putString(uri_missing, "x-cell-fs:///dev_bdvd/PS3_GAME/USRDIR/Movie/dal_sail_playback_missing.pam");

    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    auto boot_ctx = std::make_unique<DalPpuContext>();
    dalWrite32(base, tls_image, 0xCAFEF00D);
    CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
    DalPpuContext* ctx = boot_ctx.get();
    ctx->gpr[1] = stack;
    const std::uint32_t notify_opd = dal::registerHostFunction("sailFakeNotify", sailFakeNotify);
    const std::uint32_t alloc_opd = dal::registerHostFunction("sailFakeBumpAlloc", sailFakeBumpAlloc);
    const std::uint32_t free_opd = dal::registerHostFunction("sailFakeFree", sailFakeFree);
    const std::uint32_t frame_alloc_opd = dal::registerHostFunction("sailFakeFrameAlloc", sailFakeFrameAlloc);
    const std::uint32_t frame_free_opd = dal::registerHostFunction("sailFakeFrameFree", sailFakeFrameFree);
    const std::uint32_t changed_opd = dal::registerHostFunction("sailFakeFormatChanged", sailFakeFormatChanged);
    CHECK(notify_opd != 0 && alloc_opd != 0 && free_opd != 0 && frame_alloc_opd != 0 && frame_free_opd != 0 &&
          changed_opd != 0);

    const auto call = [&](std::string_view name, std::initializer_list<std::uint64_t> args) {
        const dal::GuestHandler handler = sailHandlerNamed(name);
        CHECK(handler != nullptr);
        if (handler == nullptr) return std::uint32_t{0xdeadbeef};
        int reg = 3;
        for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
        handler(ctx, base);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto waitForEvents = [&](std::size_t count) {
        return waitUntil([&] { return g_notify_events.size() >= count; });
    };
    const auto stateEvent = [](std::uint32_t state) { return std::pair{sail::makeEvent(3, 0), std::uint64_t{state}}; };
    const auto eventIs = [&](std::size_t index, std::pair<std::uint64_t, std::uint64_t> expected) {
        return index < g_notify_events.size() && g_notify_events[index].event == expected.first &&
               g_notify_events[index].arg0 == expected.second && g_notify_events[index].arg == kNotifyArg &&
               g_notify_events[index].on_service;
    };

    // The objects the game builds before it opens a stream, for the player at `player_address`.
    const auto bootPlayer = [&](std::uint32_t player_address, std::uint32_t sound_address, std::uint32_t gfx_address,
                                std::uint32_t uri_address) {
        CHECK(call("cellSailPlayerInitialize2",
                   {player_address, allocator, notify_opd, kNotifyArg, attribute, resource}) == 0);
        CHECK(call("cellSailSoundAdapterInitialize", {sound_address, sound_funcs, kSoundArg}) == 0);
        CHECK(call("cellSailSoundAdapterSetPreferredFormat", {sound_address, audio_format}) == 0);
        CHECK(call("cellSailPlayerSetSoundAdapter", {player_address, 0, sound_address}) == 0);
        CHECK(call("cellSailGraphicsAdapterInitialize", {gfx_address, gfx_funcs, kGfxArg}) == 0);
        CHECK(call("cellSailGraphicsAdapterSetPreferredFormat", {gfx_address, video_format}) == 0);
        CHECK(call("cellSailPlayerSetGraphicsAdapter", {player_address, 0, gfx_address}) == 0);
        CHECK(call("cellSailPlayerBoot", {player_address, 0}) == 0);
        CHECK(call("cellSailPlayerCreateDescriptor", {player_address, 0, 0, uri_address, pp_desc}) == 0);
        const std::uint32_t desc = dalRead32(base, pp_desc);
        CHECK(call("cellSailPlayerAddDescriptor", {player_address, desc}) == 0);
        return desc;
    };

    dalWrite32(base, funcs, alloc_opd);
    dalWrite32(base, funcs + 4, free_opd);
    CHECK(call("cellSailMemAllocatorInitialize", {allocator, funcs, kAllocArg}) == 0);
    dalWrite32(base, attribute, 6);
    dalWrite32(base, attribute + 4, 1);
    dalWrite32(base, attribute + 8, 1);
    // The game's callbacks: only the sound format change and the graphics format and frame callbacks do
    // anything here.
    dalWrite32(base, sound_funcs + 0, 0);
    dalWrite32(base, sound_funcs + 4, 0);
    dalWrite32(base, sound_funcs + 8, changed_opd);
    dalWrite32(base, gfx_funcs + 0, 0);
    dalWrite32(base, gfx_funcs + 4, 0);
    dalWrite32(base, gfx_funcs + 8, changed_opd);
    dalWrite32(base, gfx_funcs + 12, frame_alloc_opd);
    dalWrite32(base, gfx_funcs + 16, frame_free_opd);
    std::memset(base + video_format, 0xff, sail::kVideoFormatSize);  // as the game: ARGB, alpha 255
    dalWrite8(base, video_format, sail::kVideoCodingArgbInterleaved);
    dalWrite32(base, video_format + sail::kVideoFormatAlpha, 255);
    std::memset(base + audio_format, 0xff, sail::kAudioFormatSize);
    dalWrite8(base, audio_format, sail::kAudioCodingLpcmFloat32);
    dalWrite8(base, audio_format + 1, 2);
    dalWrite32(base, audio_format + sail::kAudioFormatFs, 48000);

    const std::uint32_t desc = bootPlayer(player, sound, gfx, uri);
    CHECK(waitForEvents(4));  // INITIALIZED and the three Boot events
    const std::size_t booted = g_notify_events.size();

    // Calls out of order are refused.
    CHECK(call("cellSailPlayerStart", {player, command, 0}) == sail::kErrorInvalidState);
    CHECK(call("cellSailGraphicsAdapterGetFrame", {gfx, info}) == static_cast<std::uint32_t>(sail::kSyncNoFrame));
    CHECK(call("cellSailPlayerOpenStream", {player_b, 0}) == sail::kErrorInvalidState);

    // OpenStream: the format and the frame buffers are there before the completion arrives.
    CHECK(call("cellSailPlayerOpenStream", {player, 0}) == 0);
    CHECK(call("cellSailPlayerOpenStream", {player, 0}) == sail::kErrorInvalidState);  // not Closed any more
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailGraphicsAdapterGetFormat", {gfx, format_out}) == 0);
    CHECK(dalRead8(base, format_out + sail::kVideoFormatCoding) == sail::kVideoCodingArgbInterleaved);
    CHECK(dalRead16(base, format_out + sail::kVideoFormatWidth) == 1280 &&
          dalRead16(base, format_out + sail::kVideoFormatHeight) == 720 &&
          dalRead32(base, format_out + sail::kVideoFormatPitch) == 5120 &&
          dalRead32(base, format_out + sail::kVideoFormatAlpha) == 255);
    CHECK(dalRead8(base, format_out + sail::kVideoFormatFrameRate) == sail::kVideoFrameRate30000Over1001);
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailSoundAdapterGetFormat", {sound, format_out}) == 0);
    CHECK(dalRead8(base, format_out + sail::kAudioFormatChNum) == 2 &&
          dalRead32(base, format_out + sail::kAudioFormatFs) == 48000);
    CHECK(waitForEvents(booted + 4));
    CHECK(g_notify_events.size() == booted + 4 && eventIs(booted, stateEvent(3)) &&
          eventIs(booted + 1, {sail::makeEvent(4, 0), 0}) && eventIs(booted + 2, stateEvent(4)) &&
          eventIs(booted + 3, {sail::makeEvent(2, sail::kCallOpenStream), 0}));
    // Both format callbacks ran before the completion and the frames were requested once: 3 buffers
    // of one tightly packed 1280x720 ARGB frame, pitch 5120.
    CHECK(g_format_changes.size() == 2 && g_format_changes[0].arg == kSoundArg && g_format_changes[1].arg == kGfxArg);
    CHECK(g_frame_allocs.size() == 1 && g_frame_buffers.size() == 3);
    if (g_frame_allocs.size() == 1) {
        CHECK(g_frame_allocs[0].arg == kGfxArg && g_frame_allocs[0].size == 5120 * 720 && g_frame_allocs[0].num == 3);
    }
    // One scratch block for the formats, the pointer array and the sound frame, from the game's allocator
    // (after the descriptor's block).
    CHECK(g_alloc_calls.size() == 2 && g_alloc_calls[1].arg == kAllocArg && g_alloc_calls[1].boundary == 0x10 &&
          g_alloc_calls[1].value == 96 + 2048 * 8);

    // Nothing is delivered before Start.
    CHECK(call("cellSailGraphicsAdapterGetFrame", {gfx, info}) == static_cast<std::uint32_t>(sail::kSyncNoFrame));
    CHECK(call("cellSailSoundAdapterGetFrame", {sound, 256, info}) == static_cast<std::uint32_t>(sail::kSyncNoFrame));

    // Start: START_TRANSITION, SESSION_STARTED, RUNNING, then the completion, and RUNNING first.
    const std::size_t opened = g_notify_events.size();
    CHECK(call("cellSailPlayerStart", {player, command, 0}) == 0);
    CHECK(call("cellSailPlayerStart", {player, command, 0}) == sail::kErrorInvalidState);
    CHECK(waitForEvents(opened + 4));
    CHECK(g_notify_events.size() == opened + 4 && eventIs(opened, stateEvent(5)) &&
          eventIs(opened + 1, {sail::makeEvent(6, 0), 0}) && eventIs(opened + 2, stateEvent(6)) &&
          eventIs(opened + 3, {sail::makeEvent(2, sail::kCallStart), 0}));

    // The first frame: the black frame of a movie without codecs, in one of the game's buffers.
    const std::uint32_t frame_taken = call("cellSailGraphicsAdapterGetFrame", {gfx, info});
    CHECK(frame_taken == static_cast<std::uint32_t>(sail::kSyncNextOnTime));
    const std::uint32_t buffer = dalRead32(base, info + sail::kFrameInfoBuffer);
    CHECK(std::ranges::find(g_frame_buffers, buffer) != g_frame_buffers.end());
    CHECK(dalRead32(base, info + sail::kFrameInfoTag) == 1 && dalRead32(base, info + sail::kFrameInfoStatus) == 0 &&
          dalRead32(base, info + sail::kFrameInfoSessionId) != 0);
    CHECK(dalRead64(base, info + sail::kFrameInfoPts) == 89310);
    // ARGB black with alpha 255, all the way down to the last pixel.
    CHECK(dalRead8(base, buffer) == 255 && dalRead8(base, buffer + 1) == 0 && dalRead8(base, buffer + 3) == 0 &&
          dalRead8(base, buffer + 5120 * 720 - 4) == 255);
    CHECK(call("cellSailGraphicsAdapterGetFrame", {gfx, info}) == static_cast<std::uint32_t>(sail::kSyncNoFrame));
    CHECK(dalRead32(base, info + sail::kFrameInfoBuffer) == 0);  // every field written
    CHECK(call("cellSailGraphicsAdapterUpdateAvSync", {gfx, 1, 12345}) == 0);

    // Sound frames: silence of the requested length, timestamped along the clock.
    std::memset(base + info, 0xee, sail::kFrameInfoSize);  // the game does not clear this one
    CHECK(call("cellSailSoundAdapterGetFrame", {sound, 256, info}) ==
          static_cast<std::uint32_t>(sail::kSyncNextOnTime));
    const std::uint32_t pcm = dalRead32(base, info + sail::kFrameInfoBuffer);
    CHECK(pcm != 0 && dalRead32(base, info + sail::kFrameInfoStatus) == 0 &&
          dalRead32(base, info + sail::kFrameInfoTag) == 1 && dalRead64(base, info + sail::kFrameInfoPts) == 89310);
    bool silent = true;
    for (std::uint32_t i = 0; i < 256 * 8; i++) silent = silent && dalRead8(base, pcm + i) == 0;
    CHECK(silent);
    CHECK(call("cellSailSoundAdapterGetFrame", {sound, 256, info}) ==
          static_cast<std::uint32_t>(sail::kSyncNextOnTime));
    CHECK(dalRead64(base, info + sail::kFrameInfoPts) == 89310 + 256 * 90000 / 48000);
    CHECK(call("cellSailSoundAdapterGetFrame", {sound, 4096, info}) == sail::kErrorInvalidArg);
    CHECK(call("cellSailSoundAdapterUpdateAvSync", {sound, 2, 99}) == 0);

    // The end comes from the wall clock, seen by a frame poll: SOURCE_EOS, STOP_TRANSITION, OPENED.
    const std::size_t running = g_notify_events.size();
    for (int i = 0; i < 400 && g_notify_events.size() < running + 3; i++) {
        call("cellSailGraphicsAdapterGetFrame", {gfx, info});
        pauseUnlocked();
    }
    CHECK(waitForEvents(running + 3));
    CHECK(g_notify_events.size() == running + 3 && eventIs(running, {sail::makeEvent(8, 0), 0}) &&
          eventIs(running + 1, stateEvent(7)) && eventIs(running + 2, stateEvent(4)));
    CHECK(call("cellSailGraphicsAdapterGetFrame", {gfx, info}) == static_cast<std::uint32_t>(sail::kSyncNoFrame));
    CHECK(g_notify_events.size() == running + 3);  // the end is reported once

    // Finalize hands every buffer back: the frames through the adapter, the scratch block through the
    // allocator, and it reports the movie.
    const std::size_t frees = g_free_calls.size();
    CHECK(call("cellSailPlayerFinalize", {player}) == 0);
    CHECK(g_frame_frees.size() == 1);
    if (g_frame_frees.size() == 1) {
        CHECK(g_frame_frees[0].arg == kGfxArg && g_frame_frees[0].num == 3 &&
              g_frame_frees[0].pointers == g_frame_buffers);
    }
    // The scratch block is the one allocated after the descriptor's 0x100-byte block.
    CHECK(g_free_calls.size() == frees + 1 && g_free_calls[frees].arg == kAllocArg &&
          g_free_calls[frees].value == scratch + 0x20000 + 0x100);
    const std::string census = dal::formatCensus();
    CHECK(census.find("census: sail 1 movies") != std::string::npos);
    CHECK(census.find("movie " + movie_guest + ": 1280x720 29.97 fps") != std::string::npos);
    // No codecs here: both sound pulls were silence, counted as underruns.
    CHECK(census.find("audio 0 samples (2 underruns), played 0.30 s of 0.30 s") != std::string::npos);
    CHECK(call("cellSailPlayerFinalize", {player}) == sail::kErrorInvalidState);
    CHECK(call("cellSailGraphicsAdapterUpdateAvSync", {gfx, 1, 1}) == 0);  // the adapter outlives the player
    CHECK(call("cellSailPlayerDestroyDescriptor", {player, desc}) == 0);
    CHECK(call("cellSailSoundAdapterFinalize", {sound}) == 0);
    CHECK(call("cellSailGraphicsAdapterFinalize", {gfx}) == 0);

    // A descriptor whose file is missing: OpenStream completes anyway, with the default format and no
    // buffers; the game then fails at CreateDatabase. Start refuses the stream.
    const std::size_t before_missing = g_notify_events.size();
    const std::size_t allocs = g_frame_allocs.size();
    const std::uint32_t missing = bootPlayer(player_b, sound_b, gfx_b, uri_missing);
    CHECK(waitForEvents(before_missing + 4));
    const std::size_t booted_b = g_notify_events.size();
    CHECK(call("cellSailPlayerOpenStream", {player_b, 0}) == 0);
    CHECK(waitForEvents(booted_b + 4));
    CHECK(eventIs(booted_b + 3, {sail::makeEvent(2, sail::kCallOpenStream), 0}));
    CHECK(g_frame_allocs.size() == allocs);
    std::memset(base + format_out, 0, 32);
    CHECK(call("cellSailGraphicsAdapterGetFormat", {gfx_b, format_out}) == 0);
    CHECK(dalRead16(base, format_out + sail::kVideoFormatWidth) == 1280 &&
          dalRead32(base, format_out + sail::kVideoFormatPitch) == 5120);
    CHECK(call("cellSailDescriptorCreateDatabase", {missing, database, 0x80, 0}) == sail::kErrorInvalidFd);
    CHECK(call("cellSailPlayerStart", {player_b, command, 0}) == sail::kErrorInvalidState);
    CHECK(call("cellSailPlayerFinalize", {player_b}) == 0);
    CHECK(dal::formatCensus().find("census: sail 2 movies") != std::string::npos);
    CHECK(dal::formatCensus().find("dal_sail_playback_missing.pam: not played") != std::string::npos);

    // Finalize right after Start, before the service thread has drained: whatever was queued before it
    // is delivered, nothing follows, and the frames and the scratch block are handed back exactly once.
    CHECK(call("cellSailPlayerDestroyDescriptor", {player_b, missing}) == 0);
    CHECK(call("cellSailSoundAdapterFinalize", {sound_b}) == 0);
    CHECK(call("cellSailGraphicsAdapterFinalize", {gfx_b}) == 0);
    const std::size_t frames_before = g_frame_buffers.size();
    const std::size_t frame_frees_before = g_frame_frees.size();
    const std::size_t scratch_frees_before = g_free_calls.size();
    const std::size_t events_before_c = g_notify_events.size();
    bootPlayer(player_b, sound_b, gfx_b, uri);
    CHECK(waitForEvents(events_before_c + 4));
    const std::size_t booted_c = g_notify_events.size();
    const std::uint32_t expected_scratch = g_bump_next;  // the next block the game's allocator hands out
    CHECK(call("cellSailPlayerOpenStream", {player_b, 0}) == 0);
    CHECK(waitForEvents(booted_c + 4));
    CHECK(g_frame_buffers.size() == frames_before + 3);
    CHECK(call("cellSailPlayerStart", {player_b, command, 0}) == 0);
    CHECK(call("cellSailPlayerFinalize", {player_b}) == 0);
    const std::size_t events_after_finalize = g_notify_events.size();
    for (int i = 0; i < 50; i++) pauseUnlocked();
    CHECK(g_notify_events.size() == events_after_finalize);
    CHECK(g_frame_frees.size() == frame_frees_before + 1);
    if (g_frame_frees.size() == frame_frees_before + 1 && g_frame_buffers.size() == frames_before + 3) {
        const std::vector<std::uint32_t> second(g_frame_buffers.end() - 3, g_frame_buffers.end());
        CHECK(g_frame_frees.back().arg == kGfxArg && g_frame_frees.back().pointers == second);
    }
    CHECK(g_free_calls.size() == scratch_frees_before + 1 && g_free_calls.back().value == expected_scratch);
    CHECK(call("cellSailGraphicsAdapterGetFrame", {gfx_b, info}) == sail::kErrorInvalidState);  // unbound
    CHECK(dal::formatCensus().find("census: sail 3 movies") != std::string::npos);

    dal::stopSailService();
    dal::resetCellSail();
    dal::configureVfs(dal::VfsLayout{});
    std::filesystem::remove(movie_path, ec);
    dal::resetHostFunctions();
    dal::resetSyncObjects();
    p.main.free(scratch);
    dal::setLogSink({});
    dal::resetCensus();
    dal::resetLogOnce();
}

void appendTimestamp(Bytes& v, int prefix, std::int64_t ts) {
    v.push_back(static_cast<std::uint8_t>((prefix << 4) | (((ts >> 30) & 7) << 1) | 1));
    v.push_back(static_cast<std::uint8_t>(ts >> 22));
    v.push_back(static_cast<std::uint8_t>((((ts >> 15) & 0x7F) << 1) | 1));
    v.push_back(static_cast<std::uint8_t>(ts >> 7));
    v.push_back(static_cast<std::uint8_t>(((ts & 0x7F) << 1) | 1));
}

Bytes makePes(std::uint8_t id, const Bytes& payload, std::int64_t pts = -1, std::int64_t dts = -1) {
    Bytes ts;
    if (pts >= 0) {
        appendTimestamp(ts, dts >= 0 ? 3 : 2, pts);
        if (dts >= 0) appendTimestamp(ts, 1, dts);
    }
    const unsigned flags = pts < 0 ? 0 : dts < 0 ? 2 : 3;
    const std::size_t length = 3 + ts.size() + payload.size();
    Bytes v = {0,
               0,
               1,
               id,
               static_cast<std::uint8_t>(length >> 8),
               static_cast<std::uint8_t>(length),
               0x80,
               static_cast<std::uint8_t>(flags << 6),
               static_cast<std::uint8_t>(ts.size())};
    v.insert(v.end(), ts.begin(), ts.end());
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

Bytes makeStreamPacket(std::uint8_t id, const Bytes& payload) {
    Bytes v = {0, 0, 1, id, static_cast<std::uint8_t>(payload.size() >> 8), static_cast<std::uint8_t>(payload.size())};
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

// One 2048-byte pack: pack header, the packets, then a padding packet up to the pack end.
Bytes makePack(const std::vector<Bytes>& packets, bool pad = true, std::size_t stuffing = 0) {
    Bytes v = {0,    0,    1,    0xBA, 0x44, 0x00, 0x0F,
               0x25, 0xC4, 0xC1, 0x07, 0x53, 0x03, static_cast<std::uint8_t>(0xF8 | stuffing)};
    v.insert(v.end(), stuffing, 0xFF);
    for (const Bytes& p : packets) v.insert(v.end(), p.begin(), p.end());
    if (pad && v.size() + 6 <= 2048) {
        const Bytes padding = makeStreamPacket(0xBE, Bytes(2048 - v.size() - 6, 0xFF));
        v.insert(v.end(), padding.begin(), padding.end());
    }
    v.resize(2048, 0);
    return v;
}

// `count` ATRAC3plus frames of 24 bytes (size code 1, 48 kHz, stereo); no sync pattern inside a payload.
Bytes makeAtracFrames(int count) {
    Bytes v;
    for (int i = 0; i < count; i++) {
        const Bytes header = {0x0F, 0xD0, 0x48, 0x01, 0, 0, 0, 0};
        v.insert(v.end(), header.begin(), header.end());
        v.insert(v.end(), 16, static_cast<std::uint8_t>(0x20 + i));
    }
    return v;
}

// Private stream 1 payload: sub-stream 0, a zero byte, then the continuation length.
Bytes makeAudioPayload(const Bytes& es, std::size_t from, std::size_t to, std::uint32_t continuation) {
    Bytes v = {0, 0, static_cast<std::uint8_t>(continuation >> 8), static_cast<std::uint8_t>(continuation)};
    v.insert(v.end(), es.begin() + static_cast<std::ptrdiff_t>(from), es.begin() + static_cast<std::ptrdiff_t>(to));
    return v;
}

struct DemuxResult {
    std::vector<std::vector<dal::PamfAccessUnit>> aus;  // per stream
    std::uint64_t errors = 0;
    std::size_t audio_before_end = 0;
};

void drainAll(dal::PamfDemuxer& demux, DemuxResult& r) {
    for (std::uint32_t i = 0; i < r.aus.size(); i++) {
        while (auto au = demux.next(i)) r.aus[i].push_back(std::move(*au));
    }
}

// Feeds `data` cut at the given offsets, or in pieces of `chunk` bytes when `cuts` is empty.
DemuxResult runDemux(const dal::PamfInfo& info, const Bytes& data, std::vector<std::size_t> cuts, std::size_t chunk = 0,
                     bool end_of_stream = true) {
    dal::PamfDemuxer demux(info);
    DemuxResult r;
    r.aus.resize(info.streams.size());
    cuts.push_back(data.size());
    std::size_t pos = 0, cut = 0;
    while (pos < data.size()) {
        const std::size_t stop = chunk != 0 ? std::min(data.size(), pos + chunk) : cuts[cut++];
        demux.feed(std::span<const std::uint8_t>(data).subspan(pos, stop - pos));
        pos = stop;
        drainAll(demux, r);
    }
    r.audio_before_end = r.aus.size() > 1 ? r.aus[1].size() : 0;
    if (end_of_stream) demux.endOfStream();
    drainAll(demux, r);
    r.errors = demux.errors();
    return r;
}

bool sameAus(const DemuxResult& a, const DemuxResult& b) {
    if (a.errors != b.errors || a.aus.size() != b.aus.size() || a.audio_before_end != b.audio_before_end) return false;
    for (std::size_t s = 0; s < a.aus.size(); s++) {
        if (a.aus[s].size() != b.aus[s].size()) return false;
        for (std::size_t i = 0; i < a.aus[s].size(); i++) {
            const dal::PamfAccessUnit &x = a.aus[s][i], &y = b.aus[s][i];
            if (x.stream_index != y.stream_index || x.pts != y.pts || x.dts != y.dts || x.bytes != y.bytes)
                return false;
        }
    }
    return true;
}

const Bytes kVideoPayload1 = {0, 0, 0, 1, 0x09, 0x10, 0xAA, 0xBB};
const Bytes kVideoPayload2 = {0xCC, 0xDD, 0xEE};
const Bytes kVideoPayload3 = {0, 0, 1, 0x09, 0x30};

// Video and ATRAC3plus packs as in the game files: a random access pack with system header and
// private stream 2, audio split across packets, and a final audio packet without a timestamp.
Bytes makeSyntheticStream(const Bytes& es) {
    const Bytes system_header = makeStreamPacket(0xBB, Bytes(6, 0x80));
    const Bytes private_stream_2 = makeStreamPacket(0xBF, {0x01, 0xE0, 0x00, 0x80, 0x01, 0x00});
    Bytes data;
    auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
    add(makePack({system_header, private_stream_2, makePes(0xE0, kVideoPayload1, 90000, 86997)}));
    add(makePack({makePes(0xBD, makeAudioPayload(es, 0, 60, 0), 1000)}));
    add(makePack({makePes(0xE0, kVideoPayload2)}));
    // Frame 2 continues for 12 bytes here, frame 3 starts at byte 72, frame 4 at byte 96.
    add(makePack({makePes(0xBD, makeAudioPayload(es, 60, 100, 12), 20000)}));
    add(makePack({makePes(0xBD, makeAudioPayload(es, 100, es.size(), 20))}));
    add(makePack({makePes(0xE0, kVideoPayload3, 93003)}));
    return data;
}

void testPamfDemuxSynthetic() {
    const Bytes header = makePamfHeader(6);
    const auto info = dal::parsePamfHeader(header);
    CHECK(info.has_value());
    if (!info) return;
    const Bytes es = makeAtracFrames(6);
    const Bytes data = makeSyntheticStream(es);
    CHECK(data.size() == 6 * 2048);

    // Three feeds that cut through the middle of packs.
    const DemuxResult r = runDemux(*info, data, {1000, 5000});
    CHECK(r.errors == 0);
    CHECK(r.aus.size() == 2 && r.aus[0].size() == 3);
    if (r.aus.size() == 2 && r.aus[0].size() == 3) {
        CHECK(r.aus[0][0].pts == 90000 && r.aus[0][0].dts == 86997 && r.aus[0][0].bytes == kVideoPayload1);
        CHECK(r.aus[0][1].pts == -1 && r.aus[0][1].dts == -1 && r.aus[0][1].bytes == kVideoPayload2);
        CHECK(r.aus[0][2].pts == 93003 && r.aus[0][2].dts == -1 && r.aus[0][2].bytes == kVideoPayload3);
        CHECK(r.aus[0][0].stream_index == 0);
    }

    // Frame 5 ends exactly at the end of the data, so it is delivered only by endOfStream.
    CHECK(r.audio_before_end == 5);
    CHECK(r.aus[1].size() == 6);
    if (r.aus[1].size() == 6) {
        // Frames 0, 3: PES timestamps. Frames 1, 2, 4, 5: previous plus 3840 ticks.
        const std::int64_t expected[] = {1000, 4840, 8680, 20000, 23840, 27680};
        for (int i = 0; i < 6; i++) {
            CHECK(r.aus[1][i].pts == expected[i] && r.aus[1][i].dts == -1 && r.aus[1][i].stream_index == 1);
            CHECK(r.aus[1][i].bytes == Bytes(es.begin() + i * 24, es.begin() + (i + 1) * 24));
        }
    }

    // The result does not depend on how the input is chunked.
    for (std::size_t chunk : {1, 7, 100, 2047, 2048, 2049, 4096}) {
        CHECK(sameAus(runDemux(*info, data, {}, chunk), r));
    }
    CHECK(sameAus(runDemux(*info, data, {}, data.size()), r));
}

void testPamfDemuxDamage() {
    const auto info = dal::parsePamfHeader(makePamfHeader(8));
    CHECK(info.has_value());
    if (!info) return;
    const Bytes es = makeAtracFrames(6);
    const Bytes video_ok = makePes(0xE0, kVideoPayload1, 90000, 86997);

    // A PES length that runs past the pack end, a section without a pack start code and a pack with a
    // bad packet prefix are each counted once and skipped; the packs around them still demux.
    Bytes overrun = makePes(0xE0, kVideoPayload2);
    overrun[4] = 0x7F;
    Bytes bad_prefix = makePack({Bytes{0, 0, 2, 0xE0, 0, 3, 0x80, 0, 0}});
    Bytes garbage(2048, 0x5A);

    Bytes data;
    auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
    add(makePack({video_ok}));
    add(makePack({overrun}, false));
    add(garbage);
    add(bad_prefix);
    add(makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500)}));
    add(makePack({makePes(0xE0, kVideoPayload3)}));

    for (std::size_t chunk : {0, 1, 1000, 2048}) {
        const DemuxResult r = runDemux(*info, data, {}, chunk != 0 ? chunk : data.size());
        CHECK(r.errors == 3);
        CHECK(r.aus[0].size() == 2 && r.aus[0][0].pts == 90000 && r.aus[0][1].bytes == kVideoPayload3);
        CHECK(r.aus[1].size() == 6 && r.aus[1][5].pts == 500 + 5 * 3840);
    }

    // A program end code delivers the pending last frame without endOfStream and is not an error.
    Bytes program_end = {0, 0, 1, 0xB9};
    program_end.resize(2048, 0);
    Bytes with_end;
    Bytes audio_pack = makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500)});
    with_end.insert(with_end.end(), audio_pack.begin(), audio_pack.end());
    with_end.insert(with_end.end(), program_end.begin(), program_end.end());
    const DemuxResult ended = runDemux(*info, with_end, {2048}, 0, false);
    CHECK(ended.errors == 0 && ended.aus[1].size() == 6);

    // A short last section is parsed as far as it goes and counts as one error, wherever it is cut off.
    Bytes truncated = makePack({video_ok});
    Bytes second = makePack({makePes(0xE0, kVideoPayload2)});
    truncated.insert(truncated.end(), second.begin(), second.begin() + 20);
    const DemuxResult cut = runDemux(*info, truncated, {2060});
    CHECK(cut.errors == 1 && cut.aus[0].size() == 1);
    Bytes header_only = makePack({video_ok});
    header_only.insert(header_only.end(), second.begin(), second.begin() + 14);  // ends between packets
    const DemuxResult cut_between = runDemux(*info, header_only, {});
    CHECK(cut_between.errors == 1 && cut_between.aus[0].size() == 1);

    // A damaged frame header costs one error, however long the resynchronisation takes. Frame 2 claims
    // 56 bytes instead of 24, so no sync word follows it; frames 0, 1, 3, 4 and 5 are still delivered.
    Bytes bad_es = es;
    bad_es[2 * 24 + 3] = 0x05;
    const Bytes bad_pack = makePack({makePes(0xBD, makeAudioPayload(bad_es, 0, bad_es.size(), 0), 500)});
    for (std::size_t chunk : {0, 1, 5, 100}) {
        const DemuxResult r = runDemux(*info, bad_pack, {}, chunk != 0 ? chunk : bad_pack.size());
        CHECK(r.errors == 1 && r.aus[1].size() == 5);
        if (r.aus[1].size() != 5) continue;
        const int expected_frames[] = {0, 1, 3, 4, 5};
        for (int i = 0; i < 5; i++) {
            const int f = expected_frames[i];
            CHECK(r.aus[1][i].bytes == Bytes(es.begin() + f * 24, es.begin() + (f + 1) * 24));
        }
    }

    // pack_stuffing_length is honoured, padding may come first, and a program end code may follow a PES
    // inside a pack: what came before it is delivered, the next section continues, and nothing is an error.
    for (std::size_t stuffing : {2, 7}) {
        const DemuxResult stuffed = runDemux(*info, makePack({video_ok}, true, stuffing), {});
        CHECK(stuffed.errors == 0 && stuffed.aus[0].size() == 1 && stuffed.aus[0][0].pts == 90000);
    }
    const DemuxResult padding_first =
            runDemux(*info, makePack({makeStreamPacket(0xBE, Bytes(10, 0xFF)), video_ok}), {});
    CHECK(padding_first.errors == 0 && padding_first.aus[0].size() == 1 && padding_first.aus[0][0].pts == 90000);

    Bytes in_pack = makePack(
            {video_ok, makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 500), Bytes{0, 0, 1, 0xB9}}, false);
    const Bytes after = makePack({makePes(0xE0, kVideoPayload3)});
    in_pack.insert(in_pack.end(), after.begin(), after.end());
    const DemuxResult program_end_in_pack = runDemux(*info, in_pack, {1500}, 0, false);
    CHECK(program_end_in_pack.errors == 0 && program_end_in_pack.aus[0].size() == 2 &&
          program_end_in_pack.aus[1].size() == 6);

    // A stream that is not in the table is skipped silently, a bad out-of-range index returns nothing.
    Bytes other = makePack({makePes(0xE3, kVideoPayload2), makePes(0xBD, Bytes{0x31, 0, 0, 0, 1, 2})});
    const DemuxResult skipped = runDemux(*info, other, {});
    CHECK(skipped.errors == 0 && skipped.aus[0].empty() && skipped.aus[1].empty());
    dal::PamfDemuxer demux(*info);
    CHECK(!demux.next(2).has_value() && demux.errors() == 0);

    // A PES with the MPEG-1 style header byte is damage.
    Bytes mpeg1 = makePes(0xE0, kVideoPayload2);
    mpeg1[6] = 0x0F;
    const DemuxResult bad_header = runDemux(*info, makePack({mpeg1}), {});
    CHECK(bad_header.errors == 1 && bad_header.aus[0].empty());
}

// Fake codecs for the player: one frame or chunk comes out per access unit, marked with its index, so
// the tests see exactly what the player did with it.
struct FakeShared {
    std::atomic<int> video_sent{0}, audio_sent{0};
    std::atomic<int> video_delay_ms{0};
    std::atomic<std::int64_t> format_start_pts{-12345};
    bool fail_video = false, fail_audio = false;
    // With `audio_pattern` the audio chunks carry the known PCM of fakePatternPcm in the format the
    // player asked for (recorded in audio_channels and audio_rate); without it they are filled with
    // the chunk's index.
    std::atomic<bool> audio_pattern{false};
    std::atomic<std::uint32_t> audio_channels{0}, audio_rate{0};
};

constexpr std::uint32_t kFakeChunkFrames = 2048;  // sample frames per fake ATRAC3plus chunk
constexpr std::uint32_t kFakeFrameBytes = 8;      // stereo float32

// Interleaved big-endian float32 samples whose value identifies the sample frame and the channel, so
// a misplaced, swapped or repeated sample shows up in a byte comparison.
void fakePatternPcm(std::uint8_t* out, std::uint64_t first_frame, std::uint32_t frames, std::uint32_t channels) {
    for (std::uint32_t i = 0; i < frames; i++) {
        for (std::uint32_t c = 0; c < channels; c++) {
            const float value = static_cast<float>(first_frame + i) * 0.5f + static_cast<float>(c) * 0.25f;
            std::uint32_t bits;
            std::memcpy(&bits, &value, sizeof bits);
            for (int b = 0; b < 4; b++) *out++ = static_cast<std::uint8_t>(bits >> (24 - 8 * b));
        }
    }
}

class FakeVideoDecoder : public dal::VideoDecoder {
public:
    explicit FakeVideoDecoder(FakeShared* shared) : shared_(shared) {}
    bool send(std::span<const std::uint8_t>, std::int64_t pts, std::int64_t) override {
        if (const int delay = shared_->video_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const int index = shared_->video_sent++;
        // Like the real decoder: a frame without a PTS continues after the previous one.
        last_pts_ = pts >= 0 ? pts : last_pts_ + 3003;
        dal::VideoFrame frame;
        frame.pts = last_pts_;
        frame.width = 4;
        frame.height = 2;
        frame.pitch = 16;
        frame.bytes.assign(32, static_cast<std::uint8_t>(index));
        pending_.push_back(std::move(frame));
        return true;
    }
    bool receive(dal::VideoFrame& out) override {
        if (pending_.empty()) return false;
        out = std::move(pending_.front());
        pending_.erase(pending_.begin());
        return true;
    }
    void flush() override {}

private:
    FakeShared* shared_;
    std::int64_t last_pts_ = 0;
    std::vector<dal::VideoFrame> pending_;
};

class FakeAudioDecoder : public dal::AudioDecoder {
public:
    FakeAudioDecoder(FakeShared* shared, std::uint32_t channels) : shared_(shared), channels_(channels) {}
    bool send(std::span<const std::uint8_t>, std::int64_t pts) override {
        const int index = shared_->audio_sent++;
        dal::AudioChunk chunk;
        chunk.pts = pts;
        chunk.frames = kFakeChunkFrames;
        if (shared_->audio_pattern) {
            chunk.bytes.resize(std::size_t{kFakeChunkFrames} * channels_ * 4);
            fakePatternPcm(chunk.bytes.data(), std::uint64_t{kFakeChunkFrames} * static_cast<std::uint32_t>(index),
                           kFakeChunkFrames, channels_);
        } else {
            chunk.bytes.assign(std::size_t{kFakeChunkFrames} * kFakeFrameBytes, static_cast<std::uint8_t>(index + 1));
        }
        pending_.push_back(std::move(chunk));
        return true;
    }
    bool receive(dal::AudioChunk& out) override {
        if (pending_.empty()) return false;
        out = std::move(pending_.front());
        pending_.erase(pending_.begin());
        return true;
    }
    void flush() override {}

private:
    FakeShared* shared_;
    std::uint32_t channels_;
    std::vector<dal::AudioChunk> pending_;
};

class FakeFactory : public dal::MediaCodecFactory {
public:
    explicit FakeFactory(FakeShared* shared) : shared_(shared) {}
    std::unique_ptr<dal::VideoDecoder> createVideo(const dal::PamfStream&, const dal::VideoOutputFormat& format,
                                                   std::string& error) override {
        shared_->format_start_pts = format.start_pts;
        if (shared_->fail_video) {
            error = "fake video failure";
            return nullptr;
        }
        return std::make_unique<FakeVideoDecoder>(shared_);
    }
    std::unique_ptr<dal::AudioDecoder> createAudio(const dal::PamfStream&, const dal::AudioOutputFormat& format,
                                                   std::string& error) override {
        shared_->audio_channels = format.channels;
        shared_->audio_rate = format.sample_rate;
        if (shared_->fail_audio) {
            error = "fake audio failure";
            return nullptr;
        }
        return std::make_unique<FakeAudioDecoder>(shared_, format.channels);
    }

private:
    FakeShared* shared_;
};

// Video PES i has PTS 89310 + 3003 i (the header's start PTS is 89310). The first `audio_packs`
// packs are each followed by an audio pack with two ATRAC3plus frames; the first frame starts at
// 85470, one frame before the start PTS, as in the game's movies.
Bytes makePlayerStream(int video_packs, int audio_packs) {
    Bytes data;
    auto add = [&](const Bytes& pack) { data.insert(data.end(), pack.begin(), pack.end()); };
    for (int i = 0; i < video_packs; i++) {
        add(makePack({makePes(0xE0, Bytes{0, 0, 0, 1, 0x09, 0x10, static_cast<std::uint8_t>(i)}, 89310 + 3003 * i)}));
        if (i < audio_packs) {
            const Bytes es = makeAtracFrames(2);
            add(makePack({makePes(0xBD, makeAudioPayload(es, 0, es.size(), 0), 85470 + 7680 * i)}));
        }
    }
    return data;
}

struct TempMovie {
    std::filesystem::path path;
    explicit TempMovie(const Bytes& data, const char* name = "dal_movie_player_test.pam") {
        std::error_code ec;
        path = std::filesystem::temp_directory_path(ec) / name;
        Bytes file = makePamfHeader(static_cast<std::uint32_t>(data.size() / 2048));
        file.insert(file.end(), data.begin(), data.end());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    ~TempMovie() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

template <class Pred>
bool pollFor(Pred done, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr std::int64_t kPlayerStartPts = 89310;
constexpr std::int64_t kPlayerDuration = 722910 - 89310;

struct PlayerFixture {
    FakeShared shared;
    std::atomic<std::int64_t> clock{1000};
    dal::MoviePlayerOptions options;
    PlayerFixture() {
        options.clock = [this] { return clock.load(); };
        dal::setMediaCodecFactory(std::make_unique<FakeFactory>(&shared));
    }
    ~PlayerFixture() { dal::setMediaCodecFactory(nullptr); }
};

bool decodeFinished(const dal::MoviePlayer& player) { return player.stats().decode_finished; }

void testMoviePlayerOrderAndTiming() {
    PlayerFixture fx;
    fx.options.video_queue_frames = 64;  // everything fits, so decoding ends without a consumer
    const TempMovie movie(makePlayerStream(10, 4));
    dal::MoviePlayer player(fx.options);

    CHECK(!player.start());
    CHECK(!player.open(std::filesystem::temp_directory_path() / "dal_movie_player_missing.pam", {}, {}).has_value());
    CHECK(!player.start());
    const auto info = player.open(movie.path, {}, {});
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->streams.size() == 2 && static_cast<std::int64_t>(info->start_pts) == kPlayerStartPts);
    CHECK(player.start());
    CHECK(!player.start());
    CHECK(player.waitPrerolled(std::chrono::seconds(5)));
    CHECK(pollFor([&] { return decodeFinished(player); }));
    // The decoder is told the file's start PTS even though the caller passed none.
    CHECK(fx.shared.format_start_pts == kPlayerStartPts);

    dal::MoviePlayerStats stats = player.stats();
    CHECK(stats.video_decoder && stats.audio_decoder && stats.codec_error.empty());
    CHECK(stats.video_decoded == 10 && stats.audio_decoded_frames == 8 * kFakeChunkFrames);
    CHECK(stats.demux_errors == 0 && stats.read_errors == 0);
    CHECK(stats.width == 1280 && stats.height == 720 && stats.duration_ticks == kPlayerDuration);
    CHECK(stats.frame_rate_num == 30000 && stats.frame_rate_den == 1001);  // the header states no rate

    const std::int64_t origin = 1000;
    std::int64_t last_pts = -1;
    int delivered = 0;
    std::uint32_t dropped = 0;
    auto expectFrame = [&](std::int64_t now, int index, std::uint32_t expected_dropped) {
        auto due = player.takeDueVideo(now);
        CHECK(due.has_value());
        if (!due) return;
        CHECK(due->frame.pts == kPlayerStartPts + 3003 * index);
        CHECK(!due->frame.bytes.empty() && due->frame.bytes[0] == index);
        CHECK(due->dropped == expected_dropped);
        CHECK(due->frame.pts > last_pts);
        last_pts = due->frame.pts;
        delivered++;
        dropped += due->dropped;
    };
    // Frame i is due 3003 i ticks after the origin; nothing before that.
    expectFrame(origin, 0, 0);
    CHECK(!player.takeDueVideo(origin + 3002).has_value());
    expectFrame(origin + 3003, 1, 0);
    CHECK(!player.takeDueVideo(origin + 3003 + 3002).has_value());
    // A jump over frames 2, 3 and 4 delivers the newest due frame and counts the skipped ones.
    expectFrame(origin + 5 * 3003 + 10, 5, 3);
    expectFrame(origin + 1000000, 9, 3);
    CHECK(!player.takeDueVideo(origin + 2000000).has_value());
    stats = player.stats();
    CHECK(stats.video_delivered == 4 && stats.video_dropped == 6 && delivered == 4 && dropped == 6);

    // The movie ends when the clock has covered the interval, whatever the queues hold.
    CHECK(!player.atEnd(origin + kPlayerDuration - 1));
    CHECK(player.atEnd(origin + kPlayerDuration));
    CHECK(player.stats().played_ticks == kPlayerDuration);

    // Audio keeps its own PTS on the video's origin, including the frame before the start PTS.
    const dal::MovieAudioBlock a = player.takeAudio(100);
    CHECK(a.pts == 85470 && a.pts < kPlayerStartPts && a.frames == 100 && a.real_frames == 100);
    CHECK(a.bytes.size() == 100 * kFakeFrameBytes && a.bytes[0] == 1);
    // A partial chunk advances the PTS by the samples taken (100 frames = 187.5 ticks, rounded down).
    const dal::MovieAudioBlock b = player.takeAudio(100);
    CHECK(b.pts == 85470 + 187 && b.real_frames == 100);
    // A block that spans three chunks: 1848 frames of chunk 0, all of chunk 1, 1104 frames of chunk 2.
    const dal::MovieAudioBlock c = player.takeAudio(5000);
    CHECK(c.pts == 85470 + 200 * 90000 / 48000 && c.frames == 5000 && c.real_frames == 5000);
    CHECK(c.bytes.size() == 5000 * kFakeFrameBytes);
    if (c.bytes.size() == 5000 * kFakeFrameBytes) {
        CHECK(c.bytes[0] == 1 && c.bytes[1847 * kFakeFrameBytes] == 1);
        CHECK(c.bytes[1848 * kFakeFrameBytes] == 2 && c.bytes[3895 * kFakeFrameBytes] == 2);
        CHECK(c.bytes[3896 * kFakeFrameBytes] == 3 && c.bytes[4999 * kFakeFrameBytes + 7] == 3);
    }
    // Asking for more than is left gives the rest and then zero-filled silence.
    const dal::MovieAudioBlock d = player.takeAudio(20000);
    const std::uint32_t left = 8 * kFakeChunkFrames - 5200;
    CHECK(d.frames == 20000 && d.real_frames == left);
    CHECK(d.bytes.size() == 20000 * kFakeFrameBytes && d.bytes[left * kFakeFrameBytes - 1] == 8);
    CHECK(d.bytes[left * kFakeFrameBytes] == 0 && d.bytes.back() == 0);
    // Silence continues where the previous block ended.
    const dal::MovieAudioBlock e = player.takeAudio(64);
    CHECK(e.real_frames == 0 && e.pts == d.pts + 20000 * 90000 / 48000);
    CHECK(e.bytes == Bytes(64 * kFakeFrameBytes, 0));
    stats = player.stats();
    CHECK(stats.audio_delivered_frames == 8 * kFakeChunkFrames);
    CHECK(stats.audio_silence_frames == 20000 - left + 64);

    // Re-opening resets the player.
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.stats().video_decoded == 0 && !player.stats().decode_finished);
    CHECK(!player.takeDueVideo(origin + 1000000).has_value());
}

void testMoviePlayerDecodeTime() {
    PlayerFixture fx;
    fx.options.video_queue_frames = 64;
    fx.shared.video_delay_ms = 3;
    const TempMovie movie(makePlayerStream(6, 0));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.stats().video_decode_ns == 0 && player.stats().video_decode_max_ns == 0);
    CHECK(player.start());
    CHECK(pollFor([&] { return decodeFinished(player); }));
    // Six sends of at least 3 ms each; the longest call is one of them and cannot exceed the sum.
    const dal::MoviePlayerStats stats = player.stats();
    CHECK(stats.video_decoded == 6);
    CHECK(stats.video_decode_ns >= 6 * 3'000'000ull);
    CHECK(stats.video_decode_max_ns >= 3'000'000ull && stats.video_decode_max_ns <= stats.video_decode_ns);
    // Re-opening resets the counters.
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.stats().video_decode_ns == 0 && player.stats().video_decode_max_ns == 0);
}

void testMoviePlayerBackpressure() {
    PlayerFixture fx;
    const TempMovie movie(makePlayerStream(40, 0));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());

    // Nobody takes frames: the queue holds 4 and the decode thread holds one more, then it waits.
    CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(fx.shared.video_sent <= 6);
    CHECK(!decodeFinished(player));

    // The consumer is not held up by the blocked decode thread.
    const auto before = std::chrono::steady_clock::now();
    auto first = player.takeDueVideo(1000);
    const auto took = std::chrono::steady_clock::now() - before;
    CHECK(first.has_value() && first->frame.bytes[0] == 0);
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(!decodeFinished(player));

    // Taking frames lets the thread go on until the end of the file.
    std::uint64_t seen = first ? 1 + first->dropped : 0;
    const std::int64_t far = 1000 + 100000000;
    CHECK(pollFor([&] {
        if (auto due = player.takeDueVideo(far)) seen += 1 + due->dropped;
        return decodeFinished(player);
    }));
    while (auto due = player.takeDueVideo(far)) seen += 1 + due->dropped;
    CHECK(seen == 40 && fx.shared.video_sent == 40);
    CHECK(player.stats().video_decoded == 40);
}

// Nobody pulls audio (the default cellAudio never does): the video decode must still reach the end.
void testMoviePlayerUnpulledAudio() {
    PlayerFixture fx;
    fx.options.read_chunk_bytes = 2048;  // many small reads, so a stalled read shows up
    fx.options.audio_queue_frames = 4096;
    const TempMovie movie(makePlayerStream(40, 20));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());

    // The clock runs past the whole movie, so every queued chunk is stale once the queue is over its soft
    // limit.
    const std::int64_t far = 1000 + 100000000;
    fx.clock = far;
    CHECK(pollFor([&] {
        player.takeDueVideo(far);
        return decodeFinished(player);
    }));
    const dal::MoviePlayerStats stats = player.stats();
    CHECK(stats.video_decoded == 40 && stats.audio_decoded_frames == 40 * kFakeChunkFrames);
    CHECK(stats.audio_stale_frames > 0 && stats.audio_overflow_frames == 0);

    // What is left is the newest PCM, in increasing PTS order.
    std::int64_t last_pts = -1;
    std::uint64_t real = 0;
    bool increasing = true;
    for (;;) {
        const dal::MovieAudioBlock block = player.takeAudio(1000);
        if (block.real_frames == 0) break;
        if (block.pts <= last_pts) increasing = false;
        last_pts = block.pts;
        real += block.real_frames;
    }
    CHECK(real > 0 && increasing);
    CHECK(real + stats.audio_stale_frames == stats.audio_decoded_frames);
}

// A stalled clock: nothing is stale, so only the hard cap bounds the queue.
void testMoviePlayerAudioHardCap() {
    PlayerFixture fx;
    fx.options.read_chunk_bytes = 2048;
    fx.options.audio_queue_frames = 4096;
    fx.options.audio_queue_cap_frames = 8192;
    const TempMovie movie(makePlayerStream(40, 20));
    dal::MoviePlayer player(fx.options);
    CHECK(player.open(movie.path, {}, {}).has_value());
    CHECK(player.start());
    CHECK(pollFor([&] {
        player.takeDueVideo(1000 + 100000000);  // frames only; the player's clock stays at the origin
        return decodeFinished(player);
    }));
    const dal::MoviePlayerStats stats = player.stats();
    CHECK(stats.audio_decoded_frames == 40 * kFakeChunkFrames);
    CHECK(stats.audio_stale_frames == 0 && stats.audio_overflow_frames > 0);
    // The lead is measured against the stalled clock: all of the audio ends beyond it.
    CHECK(stats.audio_max_lead_ticks > 0 && stats.audio_max_lead_at_ticks == 0);

    std::uint64_t real = 0;
    std::int64_t last_pts = -1;
    bool increasing = true;
    for (;;) {
        const dal::MovieAudioBlock block = player.takeAudio(1000);
        if (block.real_frames == 0) break;
        if (block.pts <= last_pts) increasing = false;
        last_pts = block.pts;
        real += block.real_frames;
    }
    CHECK(real > 0 && real <= 8192 && increasing);
    CHECK(real + stats.audio_overflow_frames == stats.audio_decoded_frames);
}

void testMoviePlayerStop() {
    const auto elapsedMs = [](std::chrono::steady_clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
    };

    // Blocked on a full video queue.
    {
        PlayerFixture fx;
        const TempMovie movie(makePlayerStream(40, 0));
        dal::MoviePlayer player(fx.options);
        CHECK(player.open(movie.path, {}, {}).has_value());
        CHECK(player.start());
        CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
        const auto before = std::chrono::steady_clock::now();
        player.stop();
        CHECK(elapsedMs(before) < 1000);
        CHECK(decodeFinished(player));
        CHECK(player.takeDueVideo(1000 + 100000000).has_value());  // what was queued stays readable
        player.stop();                                             // idempotent
    }

    // Busy in a slow decoder.
    {
        PlayerFixture fx;
        fx.options.video_queue_frames = 1000;
        fx.shared.video_delay_ms = 20;
        const TempMovie movie(makePlayerStream(200, 0));
        dal::MoviePlayer player(fx.options);
        CHECK(player.open(movie.path, {}, {}).has_value());
        CHECK(player.start());
        CHECK(pollFor([&] { return fx.shared.video_sent >= 2; }));
        const auto before = std::chrono::steady_clock::now();
        player.stop();
        CHECK(elapsedMs(before) < 500);
        CHECK(fx.shared.video_sent < 200);
    }

    // Destroyed without stop(), with the thread waiting on a full queue.
    {
        PlayerFixture fx;
        const TempMovie movie(makePlayerStream(40, 0));
        const auto before = std::chrono::steady_clock::now();
        {
            dal::MoviePlayer player(fx.options);
            CHECK(player.open(movie.path, {}, {}).has_value());
            CHECK(player.start());
            CHECK(pollFor([&] { return fx.shared.video_sent >= 5; }));
        }
        CHECK(elapsedMs(before) < 1500);
    }
}

void testMoviePlayerWithoutCodecs() {
    dal::setMediaCodecFactory(nullptr);
    const TempMovie movie(makePlayerStream(5, 2));
    std::atomic<std::int64_t> clock{100};
    dal::MoviePlayerOptions options;
    options.clock = [&] { return clock.load(); };
    dal::MoviePlayer player(options);

    dal::VideoOutputFormat video;
    video.alpha = 0x80;
    CHECK(player.open(movie.path, video, {}).has_value());
    CHECK(player.start());
    CHECK(player.waitPrerolled(std::chrono::seconds(5)));
    CHECK(pollFor([&] { return decodeFinished(player); }));
    dal::MoviePlayerStats stats = player.stats();
    CHECK(!stats.video_decoder && !stats.audio_decoder && stats.video_decoded == 0);
    CHECK(stats.codec_error.find("no codec factory") != std::string::npos);

    // One black frame at the start PTS, in the stream's size: ARGB with the format's alpha.
    auto black = player.takeDueVideo(100);
    CHECK(black.has_value());
    if (black) {
        const dal::VideoFrame& f = black->frame;
        CHECK(f.pts == kPlayerStartPts && f.width == 1280 && f.height == 720 && f.pitch == 5120);
        CHECK(f.bytes.size() == 5120u * 720u);
        if (f.bytes.size() == 5120u * 720u) {
            CHECK(f.bytes[0] == 0x80 && f.bytes[1] == 0 && f.bytes[2] == 0 && f.bytes[3] == 0);
            CHECK(f.bytes[4] == 0x80 && f.bytes[f.bytes.size() - 4] == 0x80 && f.bytes.back() == 0);
        }
    }
    CHECK(!player.takeDueVideo(100 + 10000000).has_value());

    // Silence for the whole movie: the PTS runs on from the start PTS.
    const dal::MovieAudioBlock a = player.takeAudio(480);
    CHECK(a.pts == kPlayerStartPts && a.frames == 480 && a.real_frames == 0);
    CHECK(a.bytes == Bytes(480 * kFakeFrameBytes, 0));
    CHECK(player.takeAudio(480).pts == kPlayerStartPts + 900);

    // The movie ends after its duration on the clock. Moving the origin later moves the end.
    CHECK(!player.atEnd(100 + kPlayerDuration - 1));
    CHECK(player.atEnd(100 + kPlayerDuration));
    clock = 200;
    player.restartClock();
    CHECK(!player.atEnd(100 + kPlayerDuration));
    CHECK(player.atEnd(200 + kPlayerDuration));

    // Other pixel formats; the same player opens another movie.
    video.coding = dal::VideoCoding::Rgba;
    CHECK(player.open(movie.path, video, {}).has_value());
    CHECK(player.start());
    CHECK(player.waitPrerolled(std::chrono::seconds(5)));
    black = player.takeDueVideo(clock);
    CHECK(black.has_value() && black->frame.bytes.size() == 5120u * 720u);
    if (black && black->frame.bytes.size() == 5120u * 720u) {
        CHECK(black->frame.bytes[0] == 0 && black->frame.bytes[3] == 0x80);
    }
    video.coding = dal::VideoCoding::Yuv420Planar;
    video.width = 16;
    video.height = 8;
    CHECK(player.open(movie.path, video, {}).has_value());
    CHECK(player.start());
    CHECK(player.waitPrerolled(std::chrono::seconds(5)));
    black = player.takeDueVideo(clock);
    CHECK(black.has_value() && black->frame.bytes.size() == 16u * 8u + 2u * 8u * 4u);
    if (black && black->frame.bytes.size() == 16u * 8u + 2u * 8u * 4u) {
        CHECK(black->frame.pitch == 16 && black->frame.bytes[0] == 16 && black->frame.bytes[127] == 16);
        CHECK(black->frame.bytes[128] == 128 && black->frame.bytes.back() == 128);
    }
}

void testMoviePlayerPartialCodecs() {
    // The video decoder cannot be created: black frame, but the audio still plays.
    {
        PlayerFixture fx;
        fx.shared.fail_video = true;
        const TempMovie movie(makePlayerStream(5, 2));
        dal::MoviePlayer player(fx.options);
        CHECK(player.open(movie.path, {}, {}).has_value());
        CHECK(player.start());
        CHECK(pollFor([&] { return decodeFinished(player); }));
        // Nothing takes audio, so decoding of the 4 chunks stays within the queue and finishes.
        const dal::MoviePlayerStats stats = player.stats();
        CHECK(!stats.video_decoder && stats.audio_decoder);
        CHECK(stats.codec_error.find("fake video failure") != std::string::npos);
        auto black = player.takeDueVideo(1000);
        CHECK(black.has_value() && black->frame.pts == kPlayerStartPts);
        const dal::MovieAudioBlock a = player.takeAudio(300);
        CHECK(a.real_frames == 300 && a.pts == 85470);
    }
    // The audio decoder cannot be created: video plays, audio is silence.
    {
        PlayerFixture fx;
        fx.shared.fail_audio = true;
        fx.options.video_queue_frames = 64;  // no consumer here
        const TempMovie movie(makePlayerStream(5, 2));
        dal::MoviePlayer player(fx.options);
        CHECK(player.open(movie.path, {}, {}).has_value());
        CHECK(player.start());
        CHECK(pollFor([&] { return decodeFinished(player); }));
        const dal::MoviePlayerStats stats = player.stats();
        CHECK(stats.video_decoder && !stats.audio_decoder && stats.video_decoded == 5);
        const dal::MovieAudioBlock a = player.takeAudio(300);
        CHECK(a.real_frames == 0 && a.frames == 300);
    }
    // A file that is not PAMF is refused by open().
    {
        const TempMovie garbage(Bytes(4096, 0x5A), "dal_movie_player_garbage.pam");
        dal::MoviePlayer player;
        std::ofstream(garbage.path, std::ios::binary | std::ios::trunc) << "not a movie";
        const auto info = player.open(garbage.path, {}, {});
        CHECK(!info.has_value() && !info.error().empty());
    }
}

// SoundAdapterGetFrame with a fake ATRAC3plus decoder: the PCM reaches the game's buffer byte for byte
// in the format the game asked for, silence pads what the decoder did not deliver, and the summary
// counts delivered samples and underruns. Video is black (no decoder), so the audio decode waits for
// this test's pulls instead of running ahead.
void testSailSoundDelivery() {
    namespace sail = dal::sail;
    dal::setLogSink([](std::string_view) {});
    dal::resetCensus();
    dal::resetLogOnce();
    dal::resetSyncObjects();
    dal::resetHostFunctions();
    dal::resetCellSail();
    FakeShared shared;
    shared.fail_video = true;
    shared.audio_pattern = true;
    dal::setMediaCodecFactory(std::make_unique<FakeFactory>(&shared));
    dal::Process& p = dal::process();
    CHECK(p.memory.reserve());
    std::uint8_t* base = p.memory.base();
    const std::uint32_t scratch = p.main.allocate(0x100000, 0x10000);
    CHECK(scratch != 0);
    g_notify_events.clear();
    g_alloc_calls.clear();
    g_free_calls.clear();
    g_bump_next = scratch + 0x20000;

    const std::uint32_t tls_image = scratch + 0x100, allocator = scratch + 0x1000, funcs = scratch + 0x1010,
                        attribute = scratch + 0x1020, resource = scratch + 0x1040, sound_funcs = scratch + 0x1060,
                        audio_format = scratch + 0x1140, uri = scratch + 0x1200, pp_desc = scratch + 0x1480,
                        command = scratch + 0x1640, info = scratch + 0x1700, player = scratch + 0x2000,
                        sound = scratch + 0x2800, stack = scratch + 0x9000;
    constexpr std::uint64_t kAllocArg = 0xA11C, kNotifyArg = 0xCB, kSoundArg = 0x5D;

    const TempMovie movie(makePlayerStream(8, 5), "dal_sail_sound_test.pam");
    const std::string movie_guest = "/dev_bdvd/PS3_GAME/USRDIR/Movie/dal_sail_sound_test.pam";
    dal::VfsLayout layout;
    layout.files.push_back({movie_guest, movie.path, true});
    dal::configureVfs(layout);
    std::memcpy(base + uri, ("x-cell-fs://" + movie_guest).c_str(), movie_guest.size() + 13);

    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    auto boot_ctx = std::make_unique<DalPpuContext>();
    dalWrite32(base, tls_image, 0xCAFEF00D);
    CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
    DalPpuContext* ctx = boot_ctx.get();
    ctx->gpr[1] = stack;
    const std::uint32_t notify_opd = dal::registerHostFunction("sailFakeNotify", sailFakeNotify);
    const std::uint32_t alloc_opd = dal::registerHostFunction("sailFakeBumpAlloc", sailFakeBumpAlloc);
    const std::uint32_t free_opd = dal::registerHostFunction("sailFakeFree", sailFakeFree);
    CHECK(notify_opd != 0 && alloc_opd != 0 && free_opd != 0);
    const auto call = [&](std::string_view name, std::initializer_list<std::uint64_t> args) {
        const dal::GuestHandler handler = sailHandlerNamed(name);
        CHECK(handler != nullptr);
        if (handler == nullptr) return std::uint32_t{0xdeadbeef};
        int reg = 3;
        for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
        handler(ctx, base);
        return static_cast<std::uint32_t>(ctx->gpr[3]);
    };
    const auto waitForEvents = [&](std::size_t count) {
        return waitUntil([&] { return g_notify_events.size() >= count; });
    };

    dalWrite32(base, funcs, alloc_opd);
    dalWrite32(base, funcs + 4, free_opd);
    CHECK(call("cellSailMemAllocatorInitialize", {allocator, funcs, kAllocArg}) == 0);
    dalWrite32(base, attribute, 6);
    dalWrite32(base, attribute + 4, 1);
    dalWrite32(base, attribute + 8, 1);
    std::memset(base + sound_funcs, 0, sail::kSoundAdapterFuncsSize);

    // One movie played to the end of its decoded audio, for a sound adapter asking for `channels` channels
    // at `rate` Hz. The stream's own PTS arithmetic holds only for 48 kHz, so the PTS is compared there.
    const auto playStream = [&](std::int32_t channels, std::int32_t rate) {
        const std::uint32_t frame_bytes = static_cast<std::uint32_t>(channels) * 4;
        const bool check_pts = rate == 48000;
        shared.audio_sent = 0;
        shared.audio_channels = 0;
        shared.audio_rate = 0;
        std::memset(base + audio_format, 0xff, sail::kAudioFormatSize);
        dalWrite8(base, audio_format, sail::kAudioCodingLpcmFloat32);
        dalWrite8(base, audio_format + 1, static_cast<std::uint8_t>(channels));
        dalWrite32(base, audio_format + sail::kAudioFormatFs, static_cast<std::uint32_t>(rate));

        std::size_t seen = g_notify_events.size();
        CHECK(call("cellSailPlayerInitialize2", {player, allocator, notify_opd, kNotifyArg, attribute, resource}) == 0);
        CHECK(call("cellSailSoundAdapterInitialize", {sound, sound_funcs, kSoundArg}) == 0);
        CHECK(call("cellSailSoundAdapterSetPreferredFormat", {sound, audio_format}) == 0);
        CHECK(call("cellSailPlayerSetSoundAdapter", {player, 0, sound}) == 0);
        CHECK(call("cellSailPlayerBoot", {player, 0}) == 0);
        CHECK(call("cellSailPlayerCreateDescriptor", {player, 0, 0, uri, pp_desc}) == 0);
        const std::uint32_t desc = dalRead32(base, pp_desc);
        CHECK(call("cellSailPlayerAddDescriptor", {player, desc}) == 0);
        CHECK(waitForEvents(seen += 4));
        CHECK(call("cellSailPlayerOpenStream", {player, 0}) == 0);
        CHECK(waitForEvents(seen += 4));
        CHECK(call("cellSailPlayerStart", {player, command, 0}) == 0);
        CHECK(waitForEvents(seen += 4));

        // The decoder was asked for the game's format.
        CHECK(shared.audio_channels == static_cast<std::uint32_t>(channels) &&
              shared.audio_rate == static_cast<std::uint32_t>(rate));
        // Let the decode thread queue every chunk (five packs of two chunks), so that the pulls below do
        // not race with it.
        constexpr int kChunks = 10;
        for (int i = 0; i < 2000 && shared.audio_sent < kChunks; i++) pauseUnlocked();
        for (int i = 0; i < 50; i++) pauseUnlocked();
        CHECK(shared.audio_sent == kChunks);
        const std::uint64_t total = std::uint64_t{kChunks} * kFakeChunkFrames;

        // A request over the buffer's capacity (or of nothing) is refused and takes nothing from the queue.
        CHECK(call("cellSailSoundAdapterGetFrame", {sound, 2049, info}) == sail::kErrorInvalidArg);
        CHECK(call("cellSailSoundAdapterGetFrame", {sound, 0, info}) == sail::kErrorInvalidArg);

        // Pulls of mixed sizes cross chunk boundaries, then run past the end of the decoded audio.
        constexpr std::uint32_t kSizes[] = {1000, 256, 1500, 2048};
        std::uint64_t consumed = 0, position = 0, underruns = 0;
        bool bytes_exact = true, pts_exact = true, info_ok = true;
        for (std::uint32_t pull = 0; pull < 30; pull++) {
            const std::uint32_t n = kSizes[pull % 4];
            const auto real = static_cast<std::uint32_t>(std::min<std::uint64_t>(n, total - consumed));
            std::vector<std::uint8_t> expected(std::size_t{n} * frame_bytes, 0);
            fakePatternPcm(expected.data(), consumed, real, static_cast<std::uint32_t>(channels));
            std::memset(base + info, 0xee, sail::kFrameInfoSize);  // the game does not clear it
            const std::uint32_t result = call("cellSailSoundAdapterGetFrame", {sound, n, info});
            const std::uint32_t buffer = dalRead32(base, info + sail::kFrameInfoBuffer);
            info_ok = info_ok && result == static_cast<std::uint32_t>(sail::kSyncNextOnTime) && buffer != 0 &&
                      dalRead32(base, info + sail::kFrameInfoStatus) == 0 &&
                      dalRead32(base, info + sail::kFrameInfoTag) == pull + 1 &&
                      dalRead32(base, info + sail::kFrameInfoSessionId) != 0;
            bytes_exact = bytes_exact && p.memory.isCommitted(buffer, n * frame_bytes) &&
                          std::memcmp(base + buffer, expected.data(), expected.size()) == 0;
            pts_exact =
                    pts_exact && dalRead64(base, info + sail::kFrameInfoPts) ==
                                         static_cast<std::uint64_t>(kPlayerStartPts - 3840 + position * 90000 / 48000);
            consumed += real;
            position += n;
            if (real < n) underruns++;
        }
        CHECK(info_ok);
        CHECK(bytes_exact);
        if (check_pts) CHECK(pts_exact);
        CHECK(consumed == total && underruns >= 2);

        // The summary reports the samples that were real PCM and the pulls that needed silence.
        CHECK(call("cellSailPlayerFinalize", {player}) == 0);
        const std::string census = dal::formatCensus();
        CHECK(census.find(std::format("audio {} samples ({} underruns), played", total, underruns)) !=
              std::string::npos);
        CHECK(call("cellSailPlayerDestroyDescriptor", {player, desc}) == 0);
        CHECK(call("cellSailSoundAdapterFinalize", {sound}) == 0);
        dal::resetCellSail();  // forgets the summary, so the next run's line is the only one
    };

    playStream(2, 48000);  // what the game asks for
    playStream(1, 44100);  // a format the decoder has to be configured for

    dal::stopSailService();
    dal::resetCellSail();
    dal::configureVfs(dal::VfsLayout{});
    dal::setMediaCodecFactory(nullptr);
    dal::resetHostFunctions();
    dal::resetSyncObjects();
    p.main.free(scratch);
    dal::setLogSink({});
    dal::resetCensus();
    dal::resetLogOnce();
}

struct GameFile {
    const char* name;
    // The first three counts are from the whole-file statistics of the format description;
    // `video_pts_only` (PTS without DTS) was found by running the demuxer.
    std::uint32_t packs, video_pes, video_pts, video_pts_only;
};

const GameFile kGameFiles[] = {
        {"ch_logo.pam", 4037, 3979, 17, 0},  {"if_logo.pam", 4169, 4110, 17, 0},  {"op.pam", 113633, 112864, 241, 2},
        {"world.pam", 51111, 50434, 195, 0}, {"end01.pam", 78080, 77059, 293, 0}, {"end02.pam", 77956, 76935, 293, 0},
        {"end03.pam", 78023, 77002, 293, 0}, {"end04.pam", 77670, 76649, 292, 0}, {"end05.pam", 78067, 77046, 293, 0},
        {"end06.pam", 79860, 78813, 299, 2},
};

void testPamfGameFiles() {
    // _dupenv_s: the CRT marks getenv as deprecated.
    char* dir = nullptr;
    std::size_t dir_len = 0;
    if (_dupenv_s(&dir, &dir_len, "DAL_GAME_DIR") != 0 || dir == nullptr || *dir == '\0') {
        std::free(dir);
        std::printf("pamf game files: skipped (DAL_GAME_DIR is not set)\n");
        return;
    }
    const std::filesystem::path movie_dir = std::filesystem::path(dir) / "PS3_GAME" / "USRDIR" / "Movie";
    std::free(dir);

    for (const GameFile& file : kGameFiles) {
        const std::filesystem::path path = movie_dir / file.name;
        std::ifstream in(path, std::ios::binary);
        CHECK(in.is_open());
        if (!in.is_open()) {
            std::fprintf(stderr, "cannot open %s\n", file.name);
            continue;
        }
        Bytes header(2048);
        in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        const auto info = dal::parsePamfHeader(header);
        CHECK(info.has_value());
        if (!info) {
            std::fprintf(stderr, "%s: %s\n", file.name, info.error().c_str());
            continue;
        }
        const bool is_ch_logo = std::string_view(file.name) == "ch_logo.pam";
        CHECK(info->streams.size() == 2 && info->streams[0].codec == dal::PamfCodec::Avc &&
              info->streams[1].codec == dal::PamfCodec::Atrac3plus);
        CHECK(info->data_offset == 2048 && info->data_size / 2048 == file.packs);
        CHECK(std::filesystem::file_size(path) == std::uint64_t{info->data_offset} + info->data_size);
        if (info->streams.size() != 2) continue;
        CHECK(info->streams[0].width == 1280 && info->streams[0].height == 720);
        CHECK(info->streams[1].channels == 2 && info->streams[1].sample_rate == 48000);
        if (is_ch_logo) {
            CHECK(info->start_pts == 89310 && info->end_pts == 722910 && info->mux_rate_bound == 120000);
            CHECK(info->streams[0].stream_id == 0xE0 && info->streams[1].stream_id == 0xBD &&
                  info->streams[1].private_stream_id == 0);
        }

        // The whole data area, fed in odd-sized pieces so that packs and packets straddle feeds.
        dal::PamfDemuxer demux(*info);
        in.seekg(info->data_offset);
        Bytes chunk((1u << 20) + 13);
        std::uint64_t video_aus = 0, video_pts = 0, video_dts = 0, audio_frames = 0, audio_bytes = 0;
        std::int64_t last_video_pts = -1, last_video_dts = -1, last_audio_pts = -1, first_audio_pts = -1;
        std::int64_t first_video_pts = -1, first_video_dts = -1;
        std::uint64_t video_pts_only = 0;
        bool video_order_ok = true, audio_order_ok = true, audio_all_stamped = true;
        bool frames_ok = true;
        std::size_t first_frame_size = 0;
        auto pull = [&] {
            while (auto au = demux.next(0)) {
                video_aus++;
                if (au->pts >= 0) {
                    video_pts++;
                    if (au->pts <= last_video_pts) video_order_ok = false;
                    last_video_pts = au->pts;
                    if (first_video_pts < 0) {
                        first_video_pts = au->pts;
                        first_video_dts = au->dts;
                    }
                    if (au->dts < 0) {
                        // The description lists a DTS for every PTS, but some op and end06 packets carry only a PTS.
                        if (video_pts_only++ < 3) {
                            std::printf("pamf %s: video AU %llu has PTS %lld and no DTS (last DTS %lld)\n", file.name,
                                        static_cast<unsigned long long>(video_aus), static_cast<long long>(au->pts),
                                        static_cast<long long>(last_video_dts));
                        }
                    }
                }
                if (au->dts >= 0) {
                    video_dts++;
                    if (au->dts <= last_video_dts) video_order_ok = false;
                    last_video_dts = au->dts;
                }
            }
            while (auto au = demux.next(1)) {
                audio_frames++;
                audio_bytes += au->bytes.size();
                if (au->pts < 0)
                    audio_all_stamped = false;
                else if (au->pts <= last_audio_pts)
                    audio_order_ok = false;
                if (au->pts >= 0) last_audio_pts = au->pts;
                if (first_audio_pts < 0) {
                    first_audio_pts = au->pts;
                    first_frame_size = au->bytes.size();
                }
                if (au->bytes.size() < 8 || au->bytes[0] != 0x0F || au->bytes[1] != 0xD0) frames_ok = false;
            }
        };
        while (in) {
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            demux.feed(std::span<const std::uint8_t>(chunk).first(static_cast<std::size_t>(got)));
            pull();
        }
        demux.endOfStream();
        pull();

        std::printf(
                "pamf %s: packs %u, video PES/AUs %llu (PTS %llu, DTS %llu), audio frames %llu (%llu bytes), errors %llu, "
                "first video PTS/DTS %lld/%lld, audio PTS %lld..%lld\n",
                file.name, file.packs, static_cast<unsigned long long>(video_aus),
                static_cast<unsigned long long>(video_pts), static_cast<unsigned long long>(video_dts),
                static_cast<unsigned long long>(audio_frames), static_cast<unsigned long long>(audio_bytes),
                static_cast<unsigned long long>(demux.errors()), static_cast<long long>(first_video_pts),
                static_cast<long long>(first_video_dts), static_cast<long long>(first_audio_pts),
                static_cast<long long>(last_audio_pts));

        CHECK(demux.errors() == 0);
        CHECK(video_aus == file.video_pes && video_pts == file.video_pts);
        CHECK(video_pts_only == file.video_pts_only && video_dts == file.video_pts - file.video_pts_only);
        CHECK(video_order_ok && audio_order_ok && audio_all_stamped && frames_ok);
        CHECK(audio_frames > 0 && audio_bytes % first_frame_size == 0);
        if (is_ch_logo || std::string_view(file.name) == "if_logo.pam" || std::string_view(file.name) == "op.pam") {
            CHECK(first_video_pts == 90000 && first_video_dts == 86997);
        }
        if (is_ch_logo) {
            // One frame before the header start PTS, 165 inside the interval, and one starting at its end.
            CHECK(audio_frames == 167 && first_audio_pts == 85470 && last_audio_pts == 722910 &&
                  first_frame_size == 696);
        }
        if (std::string_view(file.name) == "op.pam") CHECK(last_audio_pts == 8629470);
    }
}

}  // namespace

int main() {
    dal::installProcessSettings();
    testGuestLayouts();
    testCodecFactory();
    testSailService();
    testPamfHeader();
    testSailPlayerHandlers();
    testSailPlayback();
    testPamfDemuxSynthetic();
    testPamfDemuxDamage();
    testMoviePlayerOrderAndTiming();
    testMoviePlayerDecodeTime();
    testMoviePlayerBackpressure();
    testMoviePlayerUnpulledAudio();
    testMoviePlayerAudioHardCap();
    testMoviePlayerStop();
    testMoviePlayerWithoutCodecs();
    testMoviePlayerPartialCodecs();
    testSailSoundDelivery();
    testPamfGameFiles();
    std::printf("media core tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

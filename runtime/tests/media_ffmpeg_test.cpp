// Self-checking tests for the run-time FFmpeg loader.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

extern "C" {
#include <libswresample/version_major.h>
}

#include "core/log.h"
#include "media/ffmpeg_api.h"
#include "media/ffmpeg_codecs.h"

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

// Runs first: once the real libraries are mapped, a later load by the same file name could be
// answered from the already loaded modules and no longer test the directory.
void testEmptyDirectory() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "tox2_ffmpeg_empty_dir_test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const auto result = tox2::loadFfmpeg(dir);
    CHECK(!result.has_value());
    if (!result) {
        std::printf("empty directory: unavailable: %s\n", result.error().c_str());
        CHECK(result.error().find(tox2::ffmpegLibraryFileName("avcodec", LIBAVCODEC_VERSION_MAJOR)) !=
              std::string::npos);
    }

    const auto absent = tox2::loadFfmpeg(dir / "does_not_exist");
    CHECK(!absent.has_value());
    std::filesystem::remove_all(dir, ec);
}

// A load that fails after some libraries are mapped must unmap them again. Copies of the real
// libraries sit in a temporary directory next to an avcodec that is not a valid library; if the
// loader kept the copies mapped, Windows would refuse to delete them. Runs before testLoad for the
// same reason as testEmptyDirectory.
void testPartialLoadIsUnmapped(const std::filesystem::path& real_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "tox2_ffmpeg_partial_dir_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const std::string avcodec = tox2::ffmpegLibraryFileName("avcodec", LIBAVCODEC_VERSION_MAJOR);
    const std::string dependencies[] = {tox2::ffmpegLibraryFileName("avutil", LIBAVUTIL_VERSION_MAJOR),
                                        tox2::ffmpegLibraryFileName("swresample", LIBSWRESAMPLE_VERSION_MAJOR),
                                        tox2::ffmpegLibraryFileName("swscale", LIBSWSCALE_VERSION_MAJOR)};
    int copied = 0;
    for (const std::string& name : dependencies) {
        fs::copy_file(real_dir / name, dir / name, fs::copy_options::overwrite_existing, ec);
        if (!ec) copied++;
    }
    CHECK(copied == 3);
    {
        std::ofstream bogus(dir / avcodec, std::ios::binary);
        bogus << "not a library";
    }

    const auto result = tox2::loadFfmpeg(dir);
    CHECK(!result.has_value());
    if (!result) {
        std::printf("partial directory: unavailable: %s\n", result.error().c_str());
        CHECK(result.error().find("cannot load " + avcodec) != std::string::npos);
    }
    fs::remove_all(dir, ec);
    CHECK(!ec);
    CHECK(!fs::exists(dir));
}

void testLoad() {
    const auto& loaded = tox2::defaultFfmpeg();
    if (!loaded) std::fprintf(stderr, "load failed: %s\n", loaded.error().c_str());
    CHECK(loaded.has_value());
    if (!loaded) return;
    const tox2::FfmpegApi& api = *loaded;

    std::printf("avutil %u.%u.%u, avcodec %u.%u.%u, swscale %u.%u.%u\n", api.avutil_version() >> 16,
                (api.avutil_version() >> 8) & 0xff, api.avutil_version() & 0xff, api.avcodec_version() >> 16,
                (api.avcodec_version() >> 8) & 0xff, api.avcodec_version() & 0xff, api.swscale_version() >> 16,
                (api.swscale_version() >> 8) & 0xff, api.swscale_version() & 0xff);
    CHECK((api.avcodec_version() >> 16) == LIBAVCODEC_VERSION_MAJOR);

    const AVCodec* h264 = api.avcodec_find_decoder(AV_CODEC_ID_H264);
    CHECK(h264 != nullptr);
    if (h264) CHECK(std::string(h264->name) == "h264");
    const AVCodec* atrac3p = api.avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    CHECK(atrac3p != nullptr);
    // The codec id is ATRAC3P; the decoder itself is named atrac3plus.
    if (atrac3p) CHECK(std::string(atrac3p->name) == "atrac3plus");
    CHECK(api.avcodec_find_decoder_by_name("atrac3plus") == atrac3p);
    CHECK(api.avcodec_find_decoder_by_name("h264") == h264);

    SwsContext* sws = api.sws_getCachedContext(nullptr, 16, 16, AV_PIX_FMT_YUV420P, 16, 16, AV_PIX_FMT_ARGB,
                                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    CHECK(sws != nullptr);
    if (sws) api.sws_freeContext(sws);

    // Exercises the frame and packet allocators through FFmpeg's own free functions.
    AVFrame* frame = api.av_frame_alloc();
    CHECK(frame != nullptr);
    api.av_frame_free(&frame);
    CHECK(frame == nullptr);
    AVPacket* packet = api.av_packet_alloc();
    CHECK(packet != nullptr);
    api.av_packet_free(&packet);
    CHECK(packet == nullptr);

    // Opening ATRAC3plus without stream parameters fails and makes FFmpeg log, which must arrive
    // through the project logger.
    std::vector<std::string> lines;
    tox2::setLogLevel(tox2::LogLevel::Debug);
    tox2::setLogSink([&lines](std::string_view line) { lines.emplace_back(line); });
    AVCodecContext* context = atrac3p ? api.avcodec_alloc_context3(atrac3p) : nullptr;
    CHECK(context != nullptr);
    if (context) {
        CHECK(api.avcodec_open2(context, atrac3p, nullptr) < 0);
        api.avcodec_free_context(&context);
    }
    tox2::setLogSink({});
    tox2::setLogLevel(tox2::LogLevel::Info);
    bool bridged = false;
    for (const std::string& line : lines) {
        if (line.find("ffmpeg: ") != std::string::npos) bridged = true;
    }
    CHECK(bridged);

    // The same result comes back on every call.
    CHECK(&tox2::defaultFfmpeg() == &loaded);
}

int clampByte(double value) { return value < 0 ? 0 : value > 255 ? 255 : static_cast<int>(value + 0.5); }

// Limited-range YUV to full-range RGB from the BT.601 and BT.709 definitions, independent of swscale.
void expectedRgb(bool bt709, int y, int u, int v, int rgb[3]) {
    const double luma = 1.164383 * (y - 16);
    const double cb = u - 128, cr = v - 128;
    if (bt709) {
        rgb[0] = clampByte(luma + 1.792741 * cr);
        rgb[1] = clampByte(luma - 0.213249 * cb - 0.532909 * cr);
        rgb[2] = clampByte(luma + 2.112402 * cb);
    } else {
        rgb[0] = clampByte(luma + 1.596027 * cr);
        rgb[1] = clampByte(luma - 0.391762 * cb - 0.812968 * cr);
        rgb[2] = clampByte(luma + 2.017232 * cb);
    }
}

// A 16x16 picture made of four flat 8x8 blocks, so chroma interpolation at the block edges does
// not enter the comparison: only pixels inside each block are checked.
void testYuvToRgb() {
    const auto& loaded = tox2::defaultFfmpeg();
    CHECK(loaded.has_value());
    if (!loaded) return;

    struct Block {
        int y, u, v;
    };
    constexpr Block kBlocks[4] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {200, 100, 160}};
    constexpr int kSize = 16;
    std::vector<std::uint8_t> luma(kSize * kSize), cb(kSize * kSize / 4), cr(kSize * kSize / 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const Block& block = kBlocks[(y / 8) * 2 + x / 8];
            luma[static_cast<std::size_t>(y * kSize + x)] = static_cast<std::uint8_t>(block.y);
            if (y % 2 == 0 && x % 2 == 0) {
                cb[static_cast<std::size_t>(y / 2 * (kSize / 2) + x / 2)] = static_cast<std::uint8_t>(block.u);
                cr[static_cast<std::size_t>(y / 2 * (kSize / 2) + x / 2)] = static_cast<std::uint8_t>(block.v);
            }
        }
    }

    AVFrame* frame = loaded->av_frame_alloc();
    CHECK(frame != nullptr);
    if (!frame) return;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = frame->height = kSize;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->data[0] = luma.data();
    frame->data[1] = cb.data();
    frame->data[2] = cr.data();
    frame->linesize[0] = kSize;
    frame->linesize[1] = frame->linesize[2] = kSize / 2;

    tox2::FfmpegVideoConverter converter(*loaded);
    for (const bool bt709 : {false, true}) {
        for (const tox2::VideoCoding coding : {tox2::VideoCoding::Argb, tox2::VideoCoding::Rgba}) {
            tox2::VideoOutputFormat format;
            format.coding = coding;
            format.bt709 = bt709;
            format.alpha = 0x80;
            format.pitch = kSize * 4 + 16;  // a padded pitch must be honoured
            tox2::VideoFrame out;
            std::string error;
            CHECK(converter.convert(*frame, format, out, error));
            CHECK(out.width == kSize && out.height == kSize && out.pitch == format.pitch);
            CHECK(out.bytes.size() == std::size_t{format.pitch} * kSize);
            if (out.bytes.size() != std::size_t{format.pitch} * kSize) continue;

            const std::size_t alpha_at = coding == tox2::VideoCoding::Argb ? 0 : 3;
            const std::size_t red_at = coding == tox2::VideoCoding::Argb ? 1 : 0;
            int worst = 0;
            bool alpha_ok = true;
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    const Block& block = kBlocks[by * 2 + bx];
                    int want[3];
                    expectedRgb(bt709, block.y, block.u, block.v, want);
                    for (int y = by * 8 + 2; y < by * 8 + 6; ++y) {
                        for (int x = bx * 8 + 2; x < bx * 8 + 6; ++x) {
                            const std::uint8_t* pixel = out.bytes.data() +
                                                        std::size_t{format.pitch} * static_cast<std::size_t>(y) +
                                                        static_cast<std::size_t>(x) * 4;
                            if (pixel[alpha_at] != 0x80) alpha_ok = false;
                            for (int c = 0; c < 3; ++c) {
                                worst = std::max(worst,
                                                 std::abs(int{pixel[red_at + static_cast<std::size_t>(c)]} - want[c]));
                            }
                        }
                    }
                }
            }
            std::printf("yuv->%s %s: largest difference %d\n", coding == tox2::VideoCoding::Argb ? "argb" : "rgba",
                        bt709 ? "bt709" : "bt601", worst);
            CHECK(worst <= 2);
            CHECK(alpha_ok);
        }
    }

    // The two matrices must give visibly different colours for these blocks.
    int a[3], b[3];
    expectedRgb(false, kBlocks[0].y, kBlocks[0].u, kBlocks[0].v, a);
    expectedRgb(true, kBlocks[0].y, kBlocks[0].u, kBlocks[0].v, b);
    CHECK(std::abs(a[1] - b[1]) > 8);

    // Planar output keeps the decoder's planes: Y, then U, then V, without padding at pitch 0.
    tox2::VideoOutputFormat planar;
    planar.coding = tox2::VideoCoding::Yuv420Planar;
    tox2::VideoFrame out;
    std::string error;
    CHECK(converter.convert(*frame, planar, out, error));
    CHECK(out.pitch == kSize && out.bytes.size() == luma.size() + cb.size() + cr.size());
    if (out.bytes.size() == luma.size() + cb.size() + cr.size()) {
        CHECK(std::equal(luma.begin(), luma.end(), out.bytes.begin()));
        CHECK(std::equal(cb.begin(), cb.end(), out.bytes.begin() + static_cast<std::ptrdiff_t>(luma.size())));
        CHECK(std::equal(cr.begin(), cr.end(),
                         out.bytes.begin() + static_cast<std::ptrdiff_t>(luma.size() + cb.size())));
    }

    // A pitch below the row size is refused.
    tox2::VideoOutputFormat narrow;
    narrow.pitch = kSize * 4 - 1;
    CHECK(!converter.convert(*frame, narrow, out, error));

    // The format comes from guest memory: absurd values are refused before any allocation.
    tox2::VideoOutputFormat huge;
    huge.width = 100000;
    CHECK(!converter.convert(*frame, huge, out, error));
    huge = {};
    huge.height = 0x7fffffff;
    CHECK(!converter.convert(*frame, huge, out, error));
    huge = {};
    huge.pitch = 1u << 20;
    CHECK(!converter.convert(*frame, huge, out, error));
    huge = {};
    huge.pitch = 0xffffffffu;
    huge.coding = tox2::VideoCoding::Yuv420Planar;
    CHECK(!converter.convert(*frame, huge, out, error));
    huge = {};
    huge.coding = tox2::VideoCoding::Yuv420Planar;
    huge.width = huge.height = 8192;  // within the limits per axis, but the frame is over the byte cap
    huge.pitch = 4 * 8192;
    CHECK(!converter.convert(*frame, huge, out, error));
    // The converter is still usable after refusals.
    CHECK(converter.convert(*frame, planar, out, error));

    loaded->av_frame_free(&frame);
}

// The decoder must survive end of stream and be reusable, with or without data in it.
void testVideoDecoderLifecycle() {
    const auto& loaded = tox2::defaultFfmpeg();
    CHECK(loaded.has_value());
    if (!loaded) return;

    tox2::FfmpegCodecFactory factory(*loaded);
    tox2::PamfStream stream;
    stream.codec = tox2::PamfCodec::Avc;
    stream.codec_info.assign(32, 0);
    stream.codec_info[2] = 4;  // frame-rate code 3 (30000/1001) plus one
    std::string error;
    std::unique_ptr<tox2::VideoDecoder> decoder = factory.createVideo(stream, {}, error);
    CHECK(decoder != nullptr);
    if (!decoder) return;

    tox2::VideoFrame frame;
    CHECK(!decoder->receive(frame));
    decoder->flush();
    const std::uint8_t plain[] = {1, 2, 3, 4};  // no start code: the parser only buffers it
    CHECK(!decoder->send(plain, -1, -1));       // refused while draining
    CHECK(!decoder->receive(frame));            // fully drained
    CHECK(decoder->send(plain, -1, -1));        // and usable again

    // Garbage with start codes reaches the decoder; errors must not leave it stuck.
    const std::uint8_t junk[] = {0,    0, 1, 0x09, 0xf0, 0,    0, 1, 0x65, 0x88, 0x84, 0x00, 0xff,
                                 0xee, 0, 0, 1,    0x09, 0xf0, 0, 0, 1,    0x41, 0x9a, 0x11, 0x22};
    decoder->send(junk, 90000, 87000);
    decoder->flush();
    for (int i = 0; i < 100 && decoder->receive(frame); ++i) {
    }
    CHECK(decoder->send(plain, -1, -1));
    decoder->flush();
    CHECK(!decoder->receive(frame));
    CHECK(decoder->send(plain, -1, -1));

    // The audio factory serves ATRAC3plus only.
    CHECK(factory.createAudio(stream, {}, error) == nullptr);
}

std::uint32_t loadBe32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

std::int16_t loadBe16(const std::uint8_t* p) { return static_cast<std::int16_t>((p[0] << 8) | p[1]); }

// Planar float in, interleaved big-endian out: byte order, channel order, rounding and clamping.
void testAudioConversion() {
    const auto& loaded = tox2::defaultFfmpeg();
    CHECK(loaded.has_value());
    if (!loaded) return;

    constexpr int kSamples = 6;
    float left[kSamples] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f};
    float right[kSamples] = {0.0f, 0.25f, std::nanf(""), 1e-6f, -1e-6f, 0.999f};
    AVFrame* frame = loaded->av_frame_alloc();
    CHECK(frame != nullptr);
    if (!frame) return;
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->nb_samples = kSamples;
    loaded->av_channel_layout_default(&frame->ch_layout, 2);
    frame->data[0] = reinterpret_cast<std::uint8_t*>(left);
    frame->data[1] = reinterpret_cast<std::uint8_t*>(right);
    frame->extended_data = frame->data;

    tox2::AudioChunk chunk;
    std::string error;
    CHECK(tox2::convertAudioFrame(*frame, true, chunk, error));
    CHECK(chunk.frames == kSamples && chunk.bytes.size() == kSamples * 2 * 4);
    if (chunk.bytes.size() == kSamples * 2 * 4) {
        const std::uint8_t* p = chunk.bytes.data();
        CHECK(loadBe32(p) == 0x3F000000);       // 0.5f, left of sample 0
        CHECK(loadBe32(p + 4) == 0x00000000);   // 0.0f, right of sample 0
        CHECK(loadBe32(p + 8) == 0xBF000000);   // -0.5f, left of sample 1
        CHECK(loadBe32(p + 12) == 0x3E800000);  // 0.25f
        CHECK(loadBe32(p + 32) == 0x40000000);  // 2.0f is passed through unclamped
    }

    CHECK(tox2::convertAudioFrame(*frame, false, chunk, error));
    CHECK(chunk.frames == kSamples && chunk.bytes.size() == kSamples * 2 * 2);
    if (chunk.bytes.size() == kSamples * 2 * 2) {
        const std::uint8_t* p = chunk.bytes.data();
        const std::int16_t want_left[kSamples] = {16384, -16384, 32767, -32767, 32767, -32768};
        const std::int16_t want_right[kSamples] = {0, 8192, 0, 0, 0, 32734};  // NaN is silence
        bool ok = true;
        for (int i = 0; i < kSamples; ++i) {
            ok = ok && loadBe16(p + i * 4) == want_left[i] && loadBe16(p + i * 4 + 2) == want_right[i];
        }
        CHECK(ok);
    }

    // Anything the decoder could not legitimately produce is refused.
    frame->format = AV_SAMPLE_FMT_S16P;
    CHECK(!tox2::convertAudioFrame(*frame, true, chunk, error));
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->nb_samples = 0;
    CHECK(!tox2::convertAudioFrame(*frame, true, chunk, error));
    frame->nb_samples = 1 << 20;
    CHECK(!tox2::convertAudioFrame(*frame, true, chunk, error));
    frame->nb_samples = kSamples;
    loaded->av_channel_layout_default(&frame->ch_layout, 9);
    CHECK(!tox2::convertAudioFrame(*frame, true, chunk, error));
    loaded->av_channel_layout_default(&frame->ch_layout, 2);
    CHECK(tox2::convertAudioFrame(*frame, true, chunk, error));
    frame->data[0] = frame->data[1] = nullptr;  // not owned by the frame
    loaded->av_frame_free(&frame);
}

// An ATRAC3plus frame as the demuxer delivers it: the 8-byte header, then the payload.
std::vector<std::uint8_t> atracFrame(unsigned rate_index, unsigned channel_index, unsigned size_code,
                                     std::size_t total_size = 0) {
    const std::size_t payload = (size_code + 1) * 8;
    std::vector<std::uint8_t> frame(total_size != 0 ? total_size : 8 + payload, 0x55);
    const unsigned params = (rate_index << 13) | (channel_index << 10) | size_code;
    frame[0] = 0x0F;
    frame[1] = 0xD0;
    frame[2] = static_cast<std::uint8_t>(params >> 8);
    frame[3] = static_cast<std::uint8_t>(params);
    return frame;
}

void testAudioDecoderLifecycle() {
    const auto& loaded = tox2::defaultFfmpeg();
    CHECK(loaded.has_value());
    if (!loaded) return;

    tox2::FfmpegCodecFactory factory(*loaded);
    tox2::PamfStream stream;
    stream.codec = tox2::PamfCodec::Atrac3plus;
    stream.sample_rate = 48000;
    stream.channels = 2;
    std::string error;
    tox2::AudioOutputFormat format;

    // The requested format comes from guest memory.
    tox2::AudioOutputFormat too_many = format;
    too_many.channels = 1000;
    CHECK(factory.createAudio(stream, too_many, error) == nullptr);
    tox2::PamfStream other = stream;
    other.codec = tox2::PamfCodec::Avc;
    CHECK(factory.createAudio(other, format, error) == nullptr);

    std::unique_ptr<tox2::AudioDecoder> decoder = factory.createAudio(stream, format, error);
    CHECK(decoder != nullptr);
    if (!decoder) return;
    const auto* ffmpeg_decoder = dynamic_cast<tox2::FfmpegAudioDecoder*>(decoder.get());
    CHECK(ffmpeg_decoder != nullptr);
    if (!ffmpeg_decoder) return;
    const tox2::FfmpegAudioStats& stats = ffmpeg_decoder->stats();
    CHECK(ffmpeg_decoder->sampleRate() == 0);  // not configured before the first frame

    tox2::AudioChunk chunk;
    CHECK(!decoder->receive(chunk));  // nothing sent yet
    decoder->flush();                 // and nothing to flush
    CHECK(!decoder->receive(chunk));

    // Frames that cannot be configured from are refused and counted.
    const std::uint8_t noise[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(!decoder->send(noise, 0));
    CHECK(!decoder->send({}, 0));
    CHECK(!decoder->send(atracFrame(3, 2, 0x7F), 0));       // rate index 3 does not exist
    CHECK(!decoder->send(atracFrame(2, 0, 0x7F), 0));       // channel configuration 0 is invalid
    CHECK(!decoder->send(atracFrame(2, 2, 0x7F, 100), 0));  // shorter than the header announces
    CHECK(stats.rejected_frames == 5);

    // A well-formed header with a payload that is not ATRAC3plus data: the outcome is up to the
    // decoder, but it must come back with an answer and stay usable.
    const std::vector<std::uint8_t> valid = atracFrame(2, 2, 0x7F);
    decoder->send(valid, 3840);
    for (int i = 0; i < 4 && decoder->receive(chunk); ++i) {
        CHECK(chunk.frames == 2048);
    }
    decoder->send(valid, 7680);
    decoder->flush();
    const std::uint64_t errors = stats.decode_errors;
    CHECK(!decoder->send(valid, 11520));  // refused while draining, without being counted
    CHECK(stats.decode_errors == errors);
    for (int i = 0; i < 4 && decoder->receive(chunk); ++i) {
    }
    CHECK(!decoder->receive(chunk));  // fully drained
    const std::uint64_t before = stats.decode_errors;
    const bool accepted = decoder->send(valid, 0);  // usable again: accepted or counted, not refused
    CHECK(accepted || stats.decode_errors > before);

    // A change of parameters in the middle of a stream reopens the decoder.
    decoder->send(atracFrame(1, 1, 0x3F), 0);
    decoder->flush();
    for (int i = 0; i < 4 && decoder->receive(chunk); ++i) {
    }
    CHECK(!decoder->receive(chunk));
}

tox2::At3Info syntheticAt3(std::uint32_t channels, std::uint32_t block_align) {
    tox2::At3Info info;
    info.channels = channels;
    info.sample_rate = 48000;
    info.block_align = block_align;
    return info;
}

// The raw-frame decoder behind the cellAtrac HLE: construction limits, frame and output size checks,
// and a decoder that survives bad data and a reset.
void testAtracFrameDecoder() {
    std::string error;
    auto factory = tox2::createFfmpegAtracDecoderFactory(error);
    CHECK(factory != nullptr);
    if (!factory) return;

    for (const std::uint32_t channels : {1u, 2u}) {
        auto decoder = factory->create(syntheticAt3(channels, 512), error);
        CHECK(decoder != nullptr);
        if (!decoder) continue;

        std::vector<float> out(tox2::kAtrac3plusFrameSamples * channels, 1.0f);
        const std::vector<std::uint8_t> frame(512, 0x55);
        CHECK(!decoder->decode(std::span<const std::uint8_t>(frame).first(511), out, error));  // short frame
        CHECK(!decoder->decode(frame, std::span<float>(out).first(out.size() - 1), error));    // short output
        // Bytes that are not ATRAC3plus data: the outcome is up to the decoder, but it must answer, leave
        // the decoder usable, and report an error rather than a half-written success.
        const bool garbage_ok = decoder->decode(frame, out, error);
        CHECK(garbage_ok || !error.empty());
        decoder->reset();
        decoder->decode(frame, out, error);
    }

    CHECK(factory->create(syntheticAt3(2, 0), error) == nullptr);  // block_align is required
    CHECK(factory->create(syntheticAt3(0, 512), error) == nullptr);
    CHECK(factory->create(syntheticAt3(9, 512), error) == nullptr);
    tox2::At3Info no_rate = syntheticAt3(2, 512);
    no_rate.sample_rate = 0;
    CHECK(factory->create(no_rate, error) == nullptr);
}

}  // namespace

int main() {
    testEmptyDirectory();
    testPartialLoadIsUnmapped(tox2::defaultFfmpegDirectory());
    testLoad();
    testYuvToRgb();
    testVideoDecoderLifecycle();
    testAudioConversion();
    testAudioDecoderLifecycle();
    testAtracFrameDecoder();
    std::printf("media ffmpeg tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

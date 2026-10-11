// Self-checking tests for the Vulkan renderer library. They skip when no Vulkan device exists.

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "render/fp_translator.h"
#include "render/frame_dump.h"
#include "render/shader_compiler.h"
#include "render/vertex_convert.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_texture_layout.h"
#include "rsx/rsx_unimplemented.h"
#include "rsx_synthetic.h"

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

constexpr const char* kTrivialVertex = R"(#version 450
layout(location = 0) in vec4 position;
void main() { gl_Position = position; }
)";

constexpr const char* kTrivialFragment = R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

// Needs no Vulkan device, so it runs before the context is created.
void testShaderCompiler() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "tox2_shader_compiler_test";
    std::filesystem::remove_all(dir, ec);
    tox2::setShaderCacheDir(dir);

    const tox2::ShaderCompilerStats start = tox2::shaderCompilerStats();
    auto vert = tox2::compileGlsl(tox2::ShaderStage::Vertex, kTrivialVertex, 0x1111);
    auto frag = tox2::compileGlsl(tox2::ShaderStage::Fragment, kTrivialFragment, 0x2222);
    CHECK(vert.has_value() && vert->size() > 5 && vert->front() == 0x07230203);
    CHECK(frag.has_value() && frag->size() > 5 && frag->front() == 0x07230203);
    if (!vert) std::fprintf(stderr, "vertex: %s\n", vert.error().c_str());
    if (!frag) std::fprintf(stderr, "fragment: %s\n", frag.error().c_str());

    // Not cached, and the shaderc message reaches the caller.
    auto bad = tox2::compileGlsl(tox2::ShaderStage::Fragment, "#version 450\nvoid main() { oops }\n", 0x3333);
    CHECK(!bad.has_value());
    if (!bad) {
        std::printf("shader syntax error message: %s", bad.error().c_str());
        CHECK(bad.error().find("error") != std::string::npos);
    }

    const tox2::ShaderCompilerStats compiled = tox2::shaderCompilerStats();
    CHECK(compiled.compiles - start.compiles == 3);
    CHECK(compiled.cache_hits == start.cache_hits);

    // Second round: both good shaders come from disk and match the first result.
    auto vert2 = tox2::compileGlsl(tox2::ShaderStage::Vertex, kTrivialVertex, 0x1111);
    auto frag2 = tox2::compileGlsl(tox2::ShaderStage::Fragment, kTrivialFragment, 0x2222);
    const tox2::ShaderCompilerStats cached = tox2::shaderCompilerStats();
    CHECK(vert2.has_value() && vert && *vert2 == *vert);
    CHECK(frag2.has_value() && frag && *frag2 == *frag);
    CHECK(cached.cache_hits - compiled.cache_hits == 2);
    CHECK(cached.compiles == compiled.compiles);

    // A truncated file is recompiled and rewritten.
    std::filesystem::path file;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename().string().starts_with("vert_0000000000001111_")) {
            file = entry.path();
        }
    }
    CHECK(!file.empty() && std::filesystem::exists(file, ec));
    std::filesystem::resize_file(file, 10, ec);
    auto vert3 = tox2::compileGlsl(tox2::ShaderStage::Vertex, kTrivialVertex, 0x1111);
    const tox2::ShaderCompilerStats repaired = tox2::shaderCompilerStats();
    CHECK(vert3.has_value() && vert && *vert3 == *vert);
    CHECK(repaired.compiles - cached.compiles == 1);
    CHECK(std::filesystem::file_size(file, ec) == vert->size() * 4);

    // Same cache_key, different GLSL: two compiles, no hit, and the SPIR-V differs.
    const char* variant_vertex = R"(#version 450
layout(location = 0) in vec4 position;
void main() { gl_Position = position * 2.0; }
)";
    auto vert4 = tox2::compileGlsl(tox2::ShaderStage::Vertex, variant_vertex, 0x1111);
    const tox2::ShaderCompilerStats variant = tox2::shaderCompilerStats();
    CHECK(vert4.has_value() && vert && *vert4 != *vert);
    CHECK(variant.compiles - repaired.compiles == 1);
    CHECK(variant.cache_hits == repaired.cache_hits);
    auto vert5 = tox2::compileGlsl(tox2::ShaderStage::Vertex, variant_vertex, 0x1111);
    auto vert6 = tox2::compileGlsl(tox2::ShaderStage::Vertex, kTrivialVertex, 0x1111);
    const tox2::ShaderCompilerStats variant_cached = tox2::shaderCompilerStats();
    CHECK(vert5.has_value() && vert4 && *vert5 == *vert4);
    CHECK(vert6.has_value() && vert && *vert6 == *vert);
    CHECK(variant_cached.cache_hits - variant.cache_hits == 2);
    CHECK(variant_cached.compiles == variant.compiles);

    std::printf("shader cache: %llu hits, %llu compiles\n",
                static_cast<unsigned long long>(variant_cached.cache_hits - start.cache_hits),
                static_cast<unsigned long long>(variant_cached.compiles - start.compiles));
    tox2::setShaderCacheDir({});
    std::filesystem::remove_all(dir, ec);
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Clears a 4x4 B8G8R8A8 image to ARGB 0xFF102030 and reads it back through a host buffer.
void testClearAndReadback(tox2::VkContext& ctx) {
    constexpr std::uint32_t kSize = 4;
    constexpr VkDeviceSize kBytes = kSize * kSize * 4;
    VkDevice device = ctx.device();

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_info.extent = {kSize, kSize, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo image_alloc_info{};
    image_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_alloc = nullptr;
    CHECK(vmaCreateImage(ctx.allocator(), &image_info, &image_alloc_info, &image, &image_alloc, nullptr) == VK_SUCCESS);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = kBytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo buffer_alloc_info{};
    buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc = nullptr;
    VmaAllocationInfo buffer_mapped{};
    CHECK(vmaCreateBuffer(ctx.allocator(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc, &buffer_mapped) ==
          VK_SUCCESS);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
    CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    CHECK(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

    const bool ready = image != VK_NULL_HANDLE && buffer != VK_NULL_HANDLE && pool != VK_NULL_HANDLE &&
                       fence != VK_NULL_HANDLE && buffer_mapped.pMappedData != nullptr;
    if (ready) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear{};
        clear.float32[0] = 0x10 / 255.0f;
        clear.float32[1] = 0x20 / 255.0f;
        clear.float32[2] = 0x30 / 255.0f;
        clear.float32[3] = 1.0f;
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        VkMemoryBarrier2 host_barrier{};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host_barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);

        VkCommandBufferSubmitInfo cmd_submit{};
        cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_submit;
        CHECK(vkQueueSubmit2(ctx.graphicsQueue(), 1, &submit, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS);

        vmaInvalidateAllocation(ctx.allocator(), buffer_alloc, 0, VK_WHOLE_SIZE);
        const auto* bytes = static_cast<const std::uint8_t*>(buffer_mapped.pMappedData);
        bool all_match = true;
        for (std::uint32_t i = 0; i < kSize * kSize; i++) {
            const std::uint8_t expected[4] = {0x30, 0x20, 0x10, 0xFF};
            if (std::memcmp(bytes + i * 4, expected, 4) != 0) all_match = false;
        }
        CHECK(all_match);
    }

    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx.allocator(), buffer, buffer_alloc);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(ctx.allocator(), image, image_alloc);
}

std::uint64_t unimplementedCount(const std::string& kind) {
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        if (name == kind) return count;
    }
    return 0;
}

std::vector<tox2::RsxCommand> replaySynthetic(const std::vector<std::uint8_t>& file) {
    tox2::RsxCaptureReader reader;
    std::string error;
    CHECK(reader.parse(file, error));
    tox2::ReplayRsxMemoryView view;
    tox2::RsxFrameBuilder builder(view);
    tox2::replayCapture(reader, view, builder);
    return builder.takeCommands();
}

bool displayIsUniform(const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t height,
                      const std::uint8_t (&expected)[4]) {
    if (bgra.size() != static_cast<std::size_t>(width) * height * 4) return false;
    for (std::size_t i = 0; i < bgra.size(); i += 4) {
        if (std::memcmp(bgra.data() + i, expected, 4) != 0) return false;
    }
    return true;
}

// Needs no Vulkan device.
void testFrameDump() {
    tox2::RgbImage image;
    image.width = 5;
    image.height = 3;
    image.rgb.resize(5 * 3 * 3);
    for (std::size_t i = 0; i < image.rgb.size(); i++) image.rgb[i] = static_cast<std::uint8_t>(i * 7);

    // BGRA to RGB drops alpha and swaps the first and third channel.
    const std::uint8_t bgra[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const tox2::RgbImage converted = tox2::rgbFromBgra(bgra, 2, 1);
    CHECK(converted.rgb == std::vector<std::uint8_t>({3, 2, 1, 7, 6, 5}));

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "tox2_frame_dump_test";
    std::filesystem::remove_all(dir, ec);
    const std::filesystem::path path = dir / "nested" / "image.png";
    std::string error;
    CHECK(tox2::writePngRgb(path, image, error));
    tox2::RgbImage read;
    CHECK(tox2::readPngRgb(path, read, error));
    CHECK(read.width == 5 && read.height == 3 && read.rgb == image.rgb);

    tox2::ImageDiff diff;
    CHECK(tox2::diffImages(image, read, diff, error));
    CHECK(diff.max == 0 && diff.mean == 0.0 && diff.differing_percent == 0.0);

    // One pixel off by 10 in one channel, another by 2 (not counted as differing).
    tox2::RgbImage other = image;
    other.rgb[0] = static_cast<std::uint8_t>(other.rgb[0] + 10);
    other.rgb[3 * 4 + 1] = static_cast<std::uint8_t>(other.rgb[3 * 4 + 1] + 2);
    CHECK(tox2::diffImages(image, other, diff, error));
    CHECK(diff.max == 10);
    CHECK(diff.mean > 12.0 / 45.0 - 1e-9 && diff.mean < 12.0 / 45.0 + 1e-9);
    CHECK(diff.differing_percent > 100.0 / 15.0 - 1e-9 && diff.differing_percent < 100.0 / 15.0 + 1e-9);

    tox2::RgbImage small = image;
    small.width = 4;
    small.rgb.resize(4 * 3 * 3);
    CHECK(!tox2::diffImages(image, small, diff, error));
    CHECK(!tox2::readPngRgb(dir / "missing.png", read, error));

    // The dump worker writes the same bytes as converting and encoding on the calling thread.
    {
        constexpr std::uint32_t kW = 7;
        constexpr std::uint32_t kH = 5;
        std::vector<std::uint8_t> pixels(kW * kH * 4);
        for (std::size_t i = 0; i < pixels.size(); i++) pixels[i] = static_cast<std::uint8_t>(i * 13 + 5);
        CHECK(tox2::writePngRgb(dir / "direct.png", tox2::rgbFromBgra(pixels, kW, kH), error));

        tox2::FrameDumper dumper(2);
        for (int i = 0; i < 3; i++) {
            tox2::DumpFrame frame;
            frame.bgra = dumper.takeBuffer();  // recycled after the first frame
            frame.bgra.assign(pixels.begin(), pixels.end());
            frame.width = kW;
            frame.height = kH;
            frame.path = dir / "dumper" / std::format("{}.png", i);
            CHECK(dumper.hasRoom() && dumper.submit(std::move(frame)));
            CHECK(dumper.flush(std::chrono::seconds(10)) == 0);
        }
        CHECK(dumper.counts().written == 3 && dumper.counts().failed == 0 && dumper.counts().skipped == 0);

        // flush waits for the frames submitted before the call, failed ones included, and reports what is
        // still pending when the timeout passes.
        CHECK(dumper.flush(std::chrono::milliseconds(0)) == 0);  // nothing submitted since the last flush
        tox2::DumpFrame bad;                                     // no pixels: the encoder refuses it
        bad.path = dir / "dumper" / "bad.png";
        CHECK(dumper.submit(std::move(bad)));
        tox2::DumpFrame good;
        good.bgra = pixels;
        good.width = kW;
        good.height = kH;
        good.path = dir / "dumper" / "after_bad.png";
        CHECK(dumper.submit(std::move(good)));
        CHECK(dumper.flush(std::chrono::milliseconds(0)) <= 2);
        CHECK(dumper.flush(std::chrono::seconds(10)) == 0);
        CHECK(dumper.counts().written == 4 && dumper.counts().failed == 1);
        CHECK(std::filesystem::exists(dir / "dumper" / "after_bad.png") &&
              !std::filesystem::exists(dir / "dumper" / "bad.png"));
        const auto bytesOf = [](const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::vector<char> direct = bytesOf(dir / "direct.png");
        CHECK(!direct.empty());
        for (int i = 0; i < 3; i++) CHECK(bytesOf(dir / "dumper" / std::format("{}.png", i)) == direct);

        // A full queue drops the frame and counts it; capacity 0 is always full.
        tox2::FrameDumper full(0);
        tox2::DumpFrame dropped;
        dropped.bgra = pixels;
        dropped.width = kW;
        dropped.height = kH;
        dropped.path = dir / "dropped.png";
        CHECK(!full.hasRoom() && !full.submit(std::move(dropped)));
        CHECK(full.counts().skipped == 1 && full.counts().written == 0);
        CHECK(!std::filesystem::exists(dir / "dropped.png"));
    }
    std::filesystem::remove_all(dir, ec);
}

tox2::RenderOptions readbackAllOptions() {
    tox2::RenderOptions options;
    options.readback_all = true;
    return options;
}

// Clear-only captures through the renderer: the display equals the clear color, across more frames
// than there are frames in flight, and a flip of an unseen buffer reads back black.
void testRendererClear(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());

    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CHECK(!renderer.readDisplay(bgra, width, height));

    constexpr std::uint32_t kColors[] = {0xFF102030, 0xFF405060, 0x80FF0000, 0xFF000000};
    for (const std::uint32_t argb : kColors) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(argb));
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        CHECK(width == tox2::rsx_synthetic::kSurfaceSize && height == tox2::rsx_synthetic::kSurfaceSize);
        const std::uint8_t expected[4] = {static_cast<std::uint8_t>(argb), static_cast<std::uint8_t>(argb >> 8),
                                          static_cast<std::uint8_t>(argb >> 16), static_cast<std::uint8_t>(argb >> 24)};
        CHECK(displayIsUniform(bgra, width, height, expected));
    }
    CHECK(renderer.stats().clears == std::size(kColors) && renderer.stats().flips == std::size(kColors));
    CHECK(unimplementedCount("state surface resize") == 0);

    // A buffer no clear or draw touched: black, and counted.
    std::vector<tox2::RsxCommand> flip_only;
    tox2::RsxFlip flip;
    flip.offset = 0x40000;
    flip.width = 8;
    flip.height = 4;
    flip_only.emplace_back(flip);
    renderer.execute(flip_only);
    CHECK(renderer.readDisplay(bgra, width, height));
    CHECK(width == 8 && height == 4);
    CHECK(displayIsUniform(bgra, width, height, {0, 0, 0, 0xFF}));
    CHECK(unimplementedCount("state flip of non-render-target") == 1);
    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// Without readback_all the display is only copied for dumped flips: readDisplay reports false for the
// others, and the dumped ones land in the directory as frame_<flip number>.png.
void testRendererReadbackGate(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    std::error_code ec;
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            std::format("tox2_render_test_dump_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());  // unique under parallel runs
    std::filesystem::remove_all(dir, ec);

    tox2::RenderOptions options;
    options.dump_dir = dir;
    options.dump_every = 2;
    tox2::RsxRenderer renderer(ctx, options);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::uint32_t flip = 1; flip <= 4; flip++) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(0xFF102030));
        renderer.execute(commands);
        CHECK(renderer.valid());
        // The dumped flips read back too; an odd one was gated off.
        CHECK(renderer.readDisplay(bgra, width, height) == (flip % 2 == 0));
    }
    // The dump worker writes in the background.
    CHECK(renderer.flushDumps(std::chrono::seconds(10)) == 0);
    const tox2::DumpCounts dumps = renderer.dumpCounts();
    CHECK(dumps.written == 2 && dumps.failed == 0 && dumps.skipped == 0);
    CHECK(std::filesystem::exists(dir / "frame_000002.png") && std::filesystem::exists(dir / "frame_000004.png"));
    CHECK(!std::filesystem::exists(dir / "frame_000001.png") && !std::filesystem::exists(dir / "frame_000003.png"));
    tox2::RgbImage image;
    std::string error;
    CHECK(tox2::readPngRgb(dir / "frame_000002.png", image, error) && image.width == width && image.height == height);
    CHECK(image.rgb.size() >= 3 && image.rgb[0] == 0x10 && image.rgb[1] == 0x20 && image.rgb[2] == 0x30);
    std::filesystem::remove_all(dir, ec);
    tox2::rsxUnimplementedReset();
}

std::uint32_t pixelArgb(const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
}

// Clear to `first`, then clear with `mask` to `second` inside the scissor rectangle (x, y, w, h).
std::vector<std::uint8_t> makeScissoredClearCapture(std::uint32_t first, std::uint32_t second, std::uint32_t mask,
                                                    std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                                    std::uint32_t h) {
    tox2::rsx_synthetic::SceneBuilder scene;
    scene.setSurface();
    scene.clear(first);
    scene.write(tox2::kMethodNv4097SetScissorHorizontal, x | (w << 16));
    scene.write(tox2::kMethodNv4097SetScissorVertical, y | (h << 16));
    scene.write(tox2::kMethodNv4097SetColorClearValue, second);
    scene.write(tox2::kMethodNv4097ClearSurface, mask);
    return scene.finish();
}

// A clear inside a sub-rectangle changes only that rectangle; a partial color mask changes only the
// masked channels. Depth and stencil values are not checked: the renderer has no depth readback.
void testRendererScissorAndMask(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    constexpr std::uint32_t kA = 0xFF102030;
    constexpr std::uint32_t kB = 0xFF405060;
    std::vector<tox2::RsxCommand> commands = replaySynthetic(makeScissoredClearCapture(kA, kB, 0xF3, 8, 16, 16, 8));
    renderer.execute(commands);
    CHECK(renderer.readDisplay(bgra, width, height) && width == 64 && height == 64);
    if (bgra.size() == 64 * 64 * 4) {
        CHECK(pixelArgb(bgra, width, 8, 16) == kB);
        CHECK(pixelArgb(bgra, width, 23, 23) == kB);
        CHECK(pixelArgb(bgra, width, 15, 20) == kB);
        CHECK(pixelArgb(bgra, width, 7, 16) == kA);
        CHECK(pixelArgb(bgra, width, 24, 16) == kA);
        CHECK(pixelArgb(bgra, width, 8, 15) == kA);
        CHECK(pixelArgb(bgra, width, 8, 24) == kA);
        CHECK(pixelArgb(bgra, width, 0, 0) == kA && pixelArgb(bgra, width, 63, 63) == kA);
    }
    CHECK(unimplementedCount("state partial color clear mask") == 0);

    // Red and green only (0x30): they take color B's values, blue and alpha keep color A's.
    commands = replaySynthetic(makeScissoredClearCapture(kA, kB, 0x30, 0, 0, 64, 64));
    renderer.execute(commands);
    CHECK(renderer.readDisplay(bgra, width, height));
    const std::uint8_t expected_rg[4] = {0x30, 0x50, 0x40, 0xFF};  // B, G, R, A
    CHECK(displayIsUniform(bgra, width, height, expected_rg));

    // Alpha only (0x80), inside a rectangle: the clear color's alpha lands, the rest is untouched.
    constexpr std::uint32_t kHalfAlpha = 0x80405060;
    commands = replaySynthetic(makeScissoredClearCapture(kA, kHalfAlpha, 0x80, 8, 16, 16, 8));
    renderer.execute(commands);
    CHECK(renderer.readDisplay(bgra, width, height) && width == 64 && height == 64);
    if (bgra.size() == 64 * 64 * 4) {
        CHECK(pixelArgb(bgra, width, 8, 16) == 0x80102030);
        CHECK(pixelArgb(bgra, width, 23, 23) == 0x80102030);
        CHECK(pixelArgb(bgra, width, 7, 16) == kA);
        CHECK(pixelArgb(bgra, width, 24, 16) == kA);
        CHECK(pixelArgb(bgra, width, 8, 24) == kA);
    }
    CHECK(unimplementedCount("state partial color clear mask") == 0);
    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// ---- vertex_convert (no Vulkan device needed) ----

void testVertexConvert() {
    using tox2::vertexAttribFormat;

    // Formats and sizes. Three-component 16-bit and 8-bit attributes are padded to four components.
    const auto f3 = vertexAttribFormat(tox2::kVertexTypeFloat, 3);
    CHECK(f3 && f3->format == VK_FORMAT_R32G32B32_SFLOAT && f3->guest_size == 12 && f3->host_size == 12);
    const auto ub4 = vertexAttribFormat(tox2::kVertexTypeUnorm8, 4);
    CHECK(ub4 && ub4->format == VK_FORMAT_R8G8B8A8_UNORM && ub4->guest_size == 4 && ub4->host_size == 4);
    const auto ub3 = vertexAttribFormat(tox2::kVertexTypeUnorm8, 3);
    CHECK(ub3 && ub3->guest_size == 3 && ub3->host_size == 4);
    const auto sf3 = vertexAttribFormat(tox2::kVertexTypeHalf, 3);
    CHECK(sf3 && sf3->format == VK_FORMAT_R16G16B16A16_SFLOAT && sf3->guest_size == 6 && sf3->host_size == 8);
    const auto s2 = vertexAttribFormat(tox2::kVertexTypeSnorm16, 2);
    CHECK(s2 && s2->format == VK_FORMAT_R16G16_SNORM && s2->host_size == 4);
    const auto ub256 = vertexAttribFormat(tox2::kVertexTypeUint8, 4);
    CHECK(ub256 && ub256->format == VK_FORMAT_R8G8B8A8_USCALED);
    CHECK(!vertexAttribFormat(tox2::kVertexTypeUint8, 3));
    CHECK(!vertexAttribFormat(tox2::kVertexTypeCmp, 1));
    CHECK(!vertexAttribFormat(0, 4));
    CHECK(!vertexAttribFormat(tox2::kVertexTypeFloat, 0) && !vertexAttribFormat(tox2::kVertexTypeFloat, 5));

    // f3, stride 16: two big-endian vertices become little-endian floats, tightly packed.
    const std::uint8_t f3_source[32] = {0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x40, 0x00,
                                        0x00, 0xAA, 0xAA, 0xAA, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x80,
                                        0x00, 0x00, 0x41, 0x20, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA};
    std::uint8_t out[24] = {};
    CHECK(tox2::convertVertexElements(*f3, f3_source, 16, 2, out) == 2);
    float floats[6];
    std::memcpy(floats, out, sizeof(floats));
    CHECK(floats[0] == 1.0f && floats[1] == 2.0f && floats[2] == -3.0f);
    CHECK(floats[3] == 0.0f && floats[4] == 0.25f && floats[5] == 10.0f);

    // The source ends before the third vertex: two elements are written, the rest of dst is untouched.
    std::uint8_t out3[36];
    std::memset(out3, 0x55, sizeof(out3));
    CHECK(tox2::convertVertexElements(*f3, f3_source, 16, 3, out3) == 2);
    CHECK(out3[24] == 0x55);
    CHECK(tox2::vertexElementsAvailable(*f3, 16, sizeof(f3_source)) == 2);
    CHECK(tox2::vertexElementsAvailable(*f3, 16, 11) == 0);
    CHECK(tox2::vertexElementsAvailable(*f3, 0, 12) == 1);

    // Stride 0: one shared element.
    std::uint8_t shared[12] = {};
    CHECK(tox2::convertVertexElements(*f3, f3_source, 0, 5, shared) == 1);
    std::memcpy(floats, shared, 12);
    CHECK(floats[0] == 1.0f && floats[1] == 2.0f && floats[2] == -3.0f);

    // ub4: bytes stay in stored order.
    const std::uint8_t colors[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::uint8_t ub_out[8] = {};
    CHECK(tox2::convertVertexElements(*ub4, colors, 4, 2, ub_out) == 2);
    CHECK(std::memcmp(ub_out, colors, 8) == 0);

    // ub3: padded with an opaque fourth byte.
    const std::uint8_t rgb[3] = {1, 2, 3};
    std::uint8_t ub3_out[4] = {};
    CHECK(tox2::convertVertexElements(*ub3, rgb, 3, 1, ub3_out) == 1);
    CHECK(ub3_out[0] == 1 && ub3_out[1] == 2 && ub3_out[2] == 3 && ub3_out[3] == 0xFF);

    // sf3: 16-bit components are swapped and the fourth is half-float 1.0 (0x3C00).
    const std::uint8_t halves[6] = {0x3C, 0x00, 0x40, 0x00, 0x44, 0x00};
    std::uint8_t half_out[8] = {};
    CHECK(tox2::convertVertexElements(*sf3, halves, 6, 1, half_out) == 1);
    const std::uint8_t expected_halves[8] = {0x00, 0x3C, 0x00, 0x40, 0x00, 0x44, 0x00, 0x3C};
    CHECK(std::memcmp(half_out, expected_halves, 8) == 0);

    // Indices.
    const std::uint8_t u16_source[6] = {0x01, 0x02, 0x00, 0x0A, 0xFF, 0xFE};
    std::uint8_t u16_out[6] = {};
    tox2::convertIndices(true, u16_source, 3, u16_out);
    const std::uint8_t expected_u16[6] = {0x02, 0x01, 0x0A, 0x00, 0xFE, 0xFF};
    CHECK(std::memcmp(u16_out, expected_u16, 6) == 0);
    const std::uint8_t u32_source[8] = {0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x07};
    std::uint8_t u32_out[8] = {};
    tox2::convertIndices(false, u32_source, 2, u32_out);
    const std::uint8_t expected_u32[8] = {0x02, 0x01, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00};
    CHECK(std::memcmp(u32_out, expected_u32, 8) == 0);

    std::uint32_t low = 0, high = 0;
    CHECK(tox2::indexRange(true, u16_source, 3, low, high) && low == 0x0A && high == 0xFFFE);
    CHECK(tox2::indexRange(false, u32_source, 2, low, high) && low == 7 && high == 0x102);
    CHECK(!tox2::indexRange(true, u16_source, 4, low, high) && !tox2::indexRange(true, u16_source, 0, low, high));

    // Range check with a restart index: 0x0102 and 0xFFFE are outside [10, 20].
    CHECK(!tox2::indicesInRange(true, u16_source, 3, 10, 20, false, 0));
    CHECK(!tox2::indicesInRange(true, u16_source, 3, 10, 20, true, 0xFFFF));
    const std::uint8_t restart_u32[12] = {0, 0, 0, 5, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 7};
    CHECK(tox2::indicesInRange(false, restart_u32, 3, 5, 7, true, 0xFFFFFFFF));
    CHECK(!tox2::indicesInRange(false, restart_u32, 3, 5, 7, false, 0xFFFFFFFF));
    CHECK(!tox2::indicesInRange(false, restart_u32, 4, 5, 7, true, 0xFFFFFFFF));  // source too short

    // Scaled-integer types can be widened to floats: s32k is signed, ub256 is not normalized.
    const auto s32k3 = vertexAttribFormat(tox2::kVertexTypeSint16, 3);
    CHECK(s32k3 && s32k3->format == VK_FORMAT_R16G16B16A16_SSCALED);
    const tox2::VertexAttribFormat wide = tox2::widenedFormat(*s32k3);
    CHECK(wide.widen && wide.format == VK_FORMAT_R32G32B32A32_SFLOAT && wide.host_size == 16);
    const std::uint8_t shorts[6] = {0x00, 0x07, 0xFF, 0xFE, 0x01, 0x00};  // 7, -2, 256
    float wide_out[4] = {};
    CHECK(tox2::convertVertexElements(wide, shorts, 6, 1,
                                      std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(wide_out), 16)) == 1);
    CHECK(wide_out[0] == 7.0f && wide_out[1] == -2.0f && wide_out[2] == 256.0f && wide_out[3] == 1.0f);
    const tox2::VertexAttribFormat wide_bytes = tox2::widenedFormat(*ub256);
    const std::uint8_t bytes4[4] = {0, 1, 200, 255};
    float bytes_out[4] = {};
    CHECK(tox2::convertVertexElements(wide_bytes, bytes4, 4, 1,
                                      std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(bytes_out), 16)) == 1);
    CHECK(bytes_out[0] == 0.0f && bytes_out[1] == 1.0f && bytes_out[2] == 200.0f && bytes_out[3] == 255.0f);
    CHECK(!tox2::widenedFormat(*f3).widen);

    // Restart support: strips and fans, and only the all-ones value of the index type.
    using tox2::RsxPrimitive;
    CHECK(tox2::rsxPrimitiveRestartSupported(RsxPrimitive::TriangleStrip, tox2::kRsxIndexTypeU32, 0xFFFFFFFF));
    CHECK(tox2::rsxPrimitiveRestartSupported(RsxPrimitive::TriangleFan, tox2::kRsxIndexTypeU16, 0xFFFF));
    CHECK(!tox2::rsxPrimitiveRestartSupported(RsxPrimitive::TriangleStrip, tox2::kRsxIndexTypeU16, 0xFFFFFFFF));
    CHECK(!tox2::rsxPrimitiveRestartSupported(RsxPrimitive::Triangles, tox2::kRsxIndexTypeU32, 0xFFFFFFFF));
    CHECK(!tox2::rsxPrimitiveRestartSupported(RsxPrimitive::TriangleStrip, tox2::kRsxIndexTypeU32, 5));
}

// ---- Drawing ----

// One vertex program instruction: MOV o[slot] = v[input].swizzle. Field layout as in the vertex program
// translator test's assembler; the end flag marks the last instruction. `write_mask` holds the destination
// lanes with w in bit 0 up to x in bit 3 (0x4 is y only).
std::array<std::uint32_t, 4> vpMove(std::uint32_t slot, std::uint32_t input, const char* swizzle, bool end,
                                    std::uint32_t write_mask = 0xF) {
    const auto lane = [&](int i) { return static_cast<std::uint32_t>(std::string("xyzw").find(swizzle[i])); };
    const std::uint32_t src0 =
            2u | (lane(3) << 8) | (lane(2) << 10) | (lane(1) << 12) | (lane(0) << 14);  // input source
    const std::uint32_t src12 = 3u | (3u << 8) | (2u << 10) | (1u << 12);               // unused constant source
    const std::uint32_t w0 = (3u << 2) | (2u << 4) | (1u << 6) | (7u << 10) | (63u << 15) | (1u << 30);
    const std::uint32_t w1 = (src0 >> 9) | (input << 8) | (1u << 22);
    const std::uint32_t w2 = (src12 >> 11) | (src12 << 6) | ((src0 & 0x1ff) << 23);
    // The write mask bits run w, z, y, x from low to high.
    const std::uint32_t w3 =
            (end ? 1u : 0u) | (slot << 2) | (63u << 7) | ((src12 & 0x7ff) << 21) | ((write_mask & 0xFu) << 13);
    return {w0, w1, w2, w3};
}

// The fragment program MOV R0 = COL0, as stored (16-bit halves swapped).
std::vector<std::uint32_t> fpMoveColor() {
    constexpr std::uint32_t kSwizzle = 0xE4u << 9;  // xyzw
    const std::uint32_t src_input = 1u | kSwizzle, src_temp = kSwizzle;
    const std::uint32_t w0 = 1u /* end */ | (0u << 1) | (0xFu << 9) | (1u << 13) /* COL0 */ | (1u << 24) /* MOV */;
    const std::uint32_t w1 = src_input | (7u << 18) | (0xE4u << 21);
    return {tox2::fpFixupWord(w0), tox2::fpFixupWord(w1), tox2::fpFixupWord(src_temp), tox2::fpFixupWord(src_temp)};
}

// Position in slot 0, color bytes (A, R, G, B in memory) in slot 3: the program moves the position to
// the output and the color, rotated to R, G, B, A, to the front diffuse output.
tox2::rsx_synthetic::Programs quadPrograms() {
    tox2::rsx_synthetic::Programs programs;
    programs.vertex.clear();
    for (const auto& instruction : {vpMove(0, 0, "xyzw", false), vpMove(1, 3, "yzwx", true)}) {
        programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
    }
    programs.fragment = fpMoveColor();
    return programs;
}

void setQuadRenderState(tox2::rsx_synthetic::SceneBuilder& scene) {
    constexpr float kScale[4] = {32.0f, 32.0f, 0.5f, 0.0f};
    constexpr float kOffset[4] = {32.0f, 32.0f, 0.5f, 0.0f};
    for (std::uint32_t i = 0; i < 4; i++) {
        scene.write(tox2::kMethodNv4097SetViewportScale + i * 4, std::bit_cast<std::uint32_t>(kScale[i]));
        scene.write(tox2::kMethodNv4097SetViewportOffset + i * 4, std::bit_cast<std::uint32_t>(kOffset[i]));
    }
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 0x1);  // front diffuse only
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x40);          // color from R0
}

enum class QuadDraw {
    Strip,              // array draw, 4 vertices
    IndexedU16,         // indices 10..13 over vertex arrays that start at element 10
    Fan,                // array draw, corners in fan order
    IndexedU32Restart,  // strip with the all-ones restart index between two triangles, minimum index 0
    ListRestart,        // the same indices as a triangle list: a restart the renderer cannot express
};

struct QuadOptions {
    QuadDraw draw = QuadDraw::Strip;
    std::uint32_t clear = 0xFF000000;
    bool cull = false;
    std::uint32_t cull_face = 0x405;   // back
    std::uint32_t front_face = 0x901;  // counter-clockwise
    bool blend = false;
    std::uint32_t uniform_color = 0;  // nonzero: every vertex carries this word (A, R, G, B in memory)
    // Color comes from slot 5, which has no array: -1 leaves the default register unwritten, 0 writes four
    // zeros, 1 writes (1, 0, 0, 1).
    int default_color = -2;  // -2: color comes from the array in slot 3
};

constexpr float kQuadCorners[4][3] = {
        {-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {-0.5f, 0.5f, 0}, {0.5f, 0.5f, 0}};            // strip order
constexpr std::uint32_t kQuadColors[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};  // red, green, blue, white

// Clear, then the quad drawn the way `options` says, flip.
std::vector<std::uint8_t> makeRenderedQuadCapture(const QuadOptions& options) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(options.clear);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    Programs programs = quadPrograms();
    if (options.default_color == -2) {
        scene.setColorAttrib(3, kColorsOffset);
    } else {
        programs.vertex.clear();
        for (const auto& instruction : {vpMove(0, 0, "xyzw", false), vpMove(1, 5, "xyzw", true)}) {
            programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
        }
        if (options.default_color >= 0) {
            const float value[4] = {options.default_color == 1 ? 1.0f : 0.0f, 0.0f, 0.0f,
                                    options.default_color == 1 ? 1.0f : 0.0f};
            for (std::uint32_t i = 0; i < 4; i++) {
                scene.write(tox2::kMethodNv4097SetVertexData4fM + 5 * 16 + i * 4,
                            std::bit_cast<std::uint32_t>(value[i]));
            }
        }
    }
    scene.setPrograms(programs);
    scene.write(tox2::kMethodNv4097SetCullFaceEnable, options.cull ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetCullFace, options.cull_face);
    scene.write(tox2::kMethodNv4097SetFrontFace, options.front_face);
    if (options.blend) {
        scene.write(tox2::kMethodNv4097SetBlendEnable, 1);
        scene.write(tox2::kMethodNv4097SetBlendFuncSfactor, 0x302 | (0x302u << 16));  // SRC_ALPHA
        scene.write(tox2::kMethodNv4097SetBlendFuncDfactor, 0x303 | (0x303u << 16));  // ONE_MINUS_SRC_ALPHA
        scene.write(tox2::kMethodNv4097SetBlendEquation, 0x8006 | (0x8006u << 16));   // ADD
    }

    const auto putVertex = [&](std::uint32_t element, const float (&corner)[3], std::uint32_t color) {
        for (std::uint32_t c = 0; c < 3; c++)
            scene.memory().putFloat(1, kPositionsOffset + element * 12 + c * 4, corner[c]);
        scene.memory().putWord(1, kColorsOffset + element * 4,
                               options.uniform_color != 0 ? options.uniform_color : color);
    };
    const auto beginDraw = [&](tox2::RsxPrimitive primitive) {
        scene.write(tox2::kMethodNv4097SetBeginEnd, static_cast<std::uint32_t>(primitive));
    };
    constexpr std::uint32_t kIndexOffset = 0x1300;

    switch (options.draw) {
        case QuadDraw::Strip:
            for (std::uint32_t v = 0; v < 4; v++) putVertex(v, kQuadCorners[v], kQuadColors[v]);
            scene.drawStrip(4);
            break;
        case QuadDraw::Fan: {
            // top left, top right, bottom right, bottom left
            constexpr std::uint32_t kOrder[4] = {0, 1, 3, 2};
            for (std::uint32_t v = 0; v < 4; v++) putVertex(v, kQuadCorners[kOrder[v]], kQuadColors[kOrder[v]]);
            beginDraw(tox2::RsxPrimitive::TriangleFan);
            scene.write(tox2::kMethodNv4097DrawArrays, 3u << 24);
            scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
            break;
        }
        case QuadDraw::IndexedU16: {
            constexpr std::uint32_t kFirst = 10;
            for (std::uint32_t v = 0; v < 4; v++) putVertex(kFirst + v, kQuadCorners[v], kQuadColors[v]);
            scene.memory().putWord(1, kIndexOffset, (kFirst << 16) | (kFirst + 1));
            scene.memory().putWord(1, kIndexOffset + 4, ((kFirst + 2) << 16) | (kFirst + 3));
            scene.write(tox2::kMethodNv4097SetIndexArrayAddress, kIndexOffset);
            scene.write(tox2::kMethodNv4097SetIndexArrayDma, 1u | (tox2::kRsxIndexTypeU16 << 4));  // main memory, u16
            beginDraw(tox2::RsxPrimitive::TriangleStrip);
            scene.write(tox2::kMethodNv4097DrawIndexArray, 3u << 24);
            scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
            break;
        }
        case QuadDraw::IndexedU32Restart:
        case QuadDraw::ListRestart: {
            for (std::uint32_t v = 0; v < 4; v++) putVertex(v, kQuadCorners[v], kQuadColors[v]);
            constexpr std::uint32_t kIndices[7] = {0, 1, 2, 0xFFFFFFFF, 1, 3, 2};
            for (std::uint32_t i = 0; i < 7; i++) scene.memory().putWord(1, kIndexOffset + i * 4, kIndices[i]);
            scene.write(tox2::kMethodNv4097SetIndexArrayAddress, kIndexOffset);
            scene.write(tox2::kMethodNv4097SetIndexArrayDma, 1u | (tox2::kRsxIndexTypeU32 << 4));  // main memory, u32
            scene.write(tox2::kMethodNv4097SetRestartIndexEnable, 1);
            scene.write(tox2::kMethodNv4097SetRestartIndex, 0xFFFFFFFF);
            beginDraw(options.draw == QuadDraw::ListRestart ? tox2::RsxPrimitive::Triangles
                                                            : tox2::RsxPrimitive::TriangleStrip);
            scene.write(tox2::kMethodNv4097DrawIndexArray, 6u << 24);
            scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
            break;
        }
    }
    return scene.finish();
}

bool channelsNear(std::uint32_t argb, int r, int g, int b, int tolerance = 12) {
    const auto within = [tolerance](std::uint32_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= tolerance;
    };
    return within((argb >> 16) & 0xFF, r) && within((argb >> 8) & 0xFF, g) && within(argb & 0xFF, b) &&
           (argb >> 24) == 0xFF;
}

// The quad covers pixels 16..48; each corner carries one color, interpolated linearly. Probes sit three
// pixels inside the corners, where the nearest vertex weighs about 0.84 and its neighbours 0.08.
void checkQuadPixels(const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t height,
                     int tolerance = 12) {
    CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
    if (bgra.size() != 64 * 64 * 4) return;
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    CHECK(pixelArgb(bgra, width, 61, 61) == 0xFF000000);
    CHECK(pixelArgb(bgra, width, 14, 32) == 0xFF000000);
    CHECK(channelsNear(pixelArgb(bgra, width, 18, 18), 215, 20, 20, tolerance));    // top left: red vertex
    CHECK(channelsNear(pixelArgb(bgra, width, 45, 18), 0, 235, 20, tolerance));     // top right: green vertex
    CHECK(channelsNear(pixelArgb(bgra, width, 18, 45), 0, 20, 235, tolerance));     // bottom left: blue vertex
    CHECK(channelsNear(pixelArgb(bgra, width, 45, 45), 215, 235, 235, tolerance));  // bottom right: white vertex
}

// A quad with big-endian float positions and ub4 colors and real vertex and fragment programs: strip, fan,
// indexed with a nonzero minimum index, indexed with a restart index, culling, blending and default
// attribute values.
void testRendererQuad(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    // Renders one scene and returns the display; renderer-side notes are left in the census.
    const auto render = [&](const QuadOptions& options) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeRenderedQuadCapture(options));
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
        if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
    };
    const auto expectNoNotes = [] {
        for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
            std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
        }
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
    };

    for (const QuadDraw draw : {QuadDraw::Strip, QuadDraw::IndexedU16, QuadDraw::Fan, QuadDraw::IndexedU32Restart}) {
        QuadOptions options;
        options.draw = draw;
        render(options);
        // A fan splits the quad along the other diagonal, so the corner probes mix in a little more of the
        // neighbouring colors.
        checkQuadPixels(bgra, width, height, draw == QuadDraw::Fan ? 45 : 12);
        CHECK(renderer.stats().draws_skipped == 0);
        expectNoNotes();
    }
    CHECK(renderer.stats().draws == 4);

    // A restart index in a triangle list cannot be expressed: the draw is skipped, nothing is drawn.
    QuadOptions list_restart;
    list_restart.draw = QuadDraw::ListRestart;
    render(list_restart);
    CHECK(renderer.stats().draws_skipped == 1 && pixelArgb(bgra, width, 32, 32) == 0xFF000000);

    // Culling. The quad's triangles run clockwise on screen, so they are front faces when the front face
    // is clockwise (0x900) and back faces when it is counter-clockwise (0x901).
    const struct {
        std::uint32_t front_face, cull_face;
        bool drawn;
    } kCullCases[] = {{0x900, 0x405, true}, {0x901, 0x405, false}, {0x900, 0x404, false}, {0x901, 0x404, true}};
    for (const auto& test : kCullCases) {
        QuadOptions options;
        options.cull = true;
        options.front_face = test.front_face;
        options.cull_face = test.cull_face;
        render(options);
        CHECK((pixelArgb(bgra, width, 32, 32) != 0xFF000000) == test.drawn);
    }
    QuadOptions no_cull;
    no_cull.front_face = 0x900;
    no_cull.cull_face = 0x404;  // ignored while culling is off
    render(no_cull);
    CHECK(pixelArgb(bgra, width, 32, 32) != 0xFF000000);

    // Blending: white at alpha 0.5 over blue gives (0.5, 0.5, 1).
    QuadOptions blend;
    blend.blend = true;
    blend.clear = 0xFF0000FF;
    blend.uniform_color = 0x80FFFFFF;
    render(blend);
    const std::uint32_t blended = pixelArgb(bgra, width, 32, 32);
    CHECK(std::abs(static_cast<int>((blended >> 16) & 0xFF) - 128) <= 3 &&
          std::abs(static_cast<int>((blended >> 8) & 0xFF) - 128) <= 3 && (blended & 0xFF) == 0xFF);

    // Default attribute values: an unwritten register reads (0, 0, 0, 1); a register written with four zeros
    // stays (0, 0, 0, 0); a written value is used as it is.
    QuadOptions defaults;
    defaults.clear = 0xFF404040;
    defaults.default_color = -1;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFF000000);
    defaults.default_color = 0;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0x00000000);
    defaults.default_color = 1;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFFFF0000);
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF404040);

    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// A vertex program instruction with a flow-control opcode: the target, the boolean index and its sense
// overlap the source 2 fields of `words`, so those are replaced.
std::array<std::uint32_t, 4> vpWithBranch(std::array<std::uint32_t, 4> words, std::uint32_t sca_opcode,
                                          std::uint32_t target, std::uint32_t bit, bool taken_when_set) {
    words[1] |= sca_opcode << 27;
    words[0] |= ((target >> 9) & 1u) << 23;
    words[2] = (words[2] & ~0x3Fu) | ((target >> 3) & 0x3Fu);
    words[3] = (words[3] & 0x007FFFFFu) | (bit << 23) | (taken_when_set ? 1u << 28 : 0u) | ((target & 7u) << 29);
    return words;
}

// BRB reads its boolean constant from SET_TRANSFORM_BRANCH_BITS at draw time: the program writes red to
// the front diffuse output, branches over the instruction that overwrites it with green, and the bit
// decides which color the quad has.
void testRendererBranchBits(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());

    const auto render = [&](std::uint32_t bit, bool taken_when_set, std::uint32_t branch_bits) {
        using namespace tox2::rsx_synthetic;
        SceneBuilder scene;
        scene.setSurface();
        scene.clear(0xFF404040);
        setQuadRenderState(scene);
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        for (std::uint32_t slot = 5; slot <= 6; slot++) {
            // Slot 5 is red, slot 6 green; both opaque.
            const float value[4] = {slot == 5 ? 1.0f : 0.0f, slot == 6 ? 1.0f : 0.0f, 0.0f, 1.0f};
            for (std::uint32_t i = 0; i < 4; i++) {
                scene.write(tox2::kMethodNv4097SetVertexData4fM + slot * 16 + i * 4,
                            std::bit_cast<std::uint32_t>(value[i]));
            }
        }
        Programs programs = quadPrograms();
        programs.vertex.clear();
        const std::array<std::array<std::uint32_t, 4>, 4> instructions = {
                vpMove(0, 0, "xyzw", false),
                vpWithBranch(vpMove(1, 5, "xyzw", false), 0x11 /* BRB */, 3, bit, taken_when_set),
                vpMove(1, 6, "xyzw", false),
                vpMove(0, 0, "xyzw", true),
        };
        for (const auto& instruction : instructions) {
            programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
        }
        scene.setPrograms(programs);
        scene.write(tox2::kMethodNv4097SetTransformBranchBits, branch_bits);
        for (std::uint32_t v = 0; v < 4; v++) {
            for (std::uint32_t c = 0; c < 3; c++)
                scene.memory().putFloat(1, kPositionsOffset + v * 12 + c * 4, kQuadCorners[v][c]);
        }
        scene.drawStrip(4);
        std::vector<tox2::RsxCommand> commands = replaySynthetic(scene.finish());
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        if (bgra.size() != 64 * 64 * 4) return std::uint32_t{0};
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
        return pixelArgb(bgra, width, 32, 32);
    };

    constexpr std::uint32_t kRed = 0xFFFF0000, kGreen = 0xFF00FF00;
    // Taken when the bit is clear: the red written first survives.
    CHECK(render(3, false, 0x00) == kRed);
    CHECK(render(3, false, 0x08) == kGreen);
    CHECK(render(3, false, 0xF7) == kRed);  // only bit 3 counts
    // Taken when the bit is set.
    CHECK(render(3, true, 0x00) == kGreen);
    CHECK(render(3, true, 0x08) == kRed);
    CHECK(render(3, true, 0xF7) == kGreen);
    // The highest bit of the register.
    CHECK(render(31, true, 0x80000000u) == kRed);
    CHECK(render(31, true, 0x7FFFFFFFu) == kGreen);

    CHECK(renderer.stats().draws_skipped == 0);
    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// ---- Textures ----

// Needs no Vulkan device.
void testTextureMapping() {
    constexpr VkComponentSwizzle kR = VK_COMPONENT_SWIZZLE_R, kG = VK_COMPONENT_SWIZZLE_G, kB = VK_COMPONENT_SWIZZLE_B,
                                 kA = VK_COMPONENT_SWIZZLE_A, kZero = VK_COMPONENT_SWIZZLE_ZERO,
                                 kOne = VK_COMPONENT_SWIZZLE_ONE;
    const auto same = [](VkComponentMapping m, VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b,
                         VkComponentSwizzle a) { return m.r == r && m.g == g && m.b == b && m.a == a; };

    // Every sampled color image is B8G8R8A8, so the identity remap gives an identity view.
    CHECK(same(tox2::textureComponentMapping(0xAAE4), kR, kG, kB, kA));
    // Red and blue exchanged (R reads B, B reads R).
    CHECK(same(tox2::textureComponentMapping(0xAA6C), kB, kG, kR, kA));
    // Alpha forced to one, then every channel forced to zero.
    CHECK(same(tox2::textureComponentMapping(0xA9E4), kR, kG, kB, kOne));
    CHECK(same(tox2::textureComponentMapping(0x0000), kZero, kZero, kZero, kZero));
    // The high half (override bits) does not change the mapping.
    CHECK(same(tox2::textureComponentMapping(0x1234AAE4), kR, kG, kB, kA));

    // A single-channel float image reads its red channel everywhere; only the forced zero or one of a channel
    // matters, whatever channel the remap selects.
    CHECK(same(tox2::singleChannelComponentMapping(0xAAE4), kR, kR, kR, kR));
    CHECK(same(tox2::singleChannelComponentMapping(0xAA6C), kR, kR, kR, kR));
    CHECK(same(tox2::singleChannelComponentMapping(0xA9E4), kR, kR, kR, kOne));
    CHECK(same(tox2::singleChannelComponentMapping(0x0000), kZero, kZero, kZero, kZero));

    // The border word (ARGB) goes through the same remap.
    CHECK(tox2::remapBorderColor(0x80402010, 0xAAE4) == 0x80402010);
    CHECK(tox2::remapBorderColor(0x80402010, 0xAA6C) == 0x80102040);  // R and B exchanged
    CHECK(tox2::remapBorderColor(0x80402010, 0xA9E4) == 0xFF402010);  // alpha forced to one
    CHECK(tox2::remapBorderColor(0x80402010, 0x0000) == 0);

    tox2::RsxTextureDesc texture;
    texture.filter = (3u << 16) | (4u << 24);  // NEAREST_NEAREST, CONVOLUTION_MAG
    texture.address = 1u | (4u << 8) | (3u << 16);
    texture.border = 0xFF000000;
    texture.remap = 0xAAE4;
    tox2::rsxUnimplementedReset();
    tox2::SamplerSettings sampler = tox2::samplerSettings(texture);
    CHECK(sampler.min_filter == VK_FILTER_NEAREST && sampler.mag_filter == VK_FILTER_LINEAR);
    CHECK(sampler.wrap_s == VK_SAMPLER_ADDRESS_MODE_REPEAT &&
          sampler.wrap_t == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER &&
          sampler.wrap_r == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(sampler.border == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    CHECK(tox2::rsxUnimplementedSnapshot().empty());

    texture.filter = (6u << 16) | (2u << 24);  // LINEAR_LINEAR, LINEAR
    texture.address = 2u | (5u << 8) | (1u << 16);
    sampler = tox2::samplerSettings(texture);
    CHECK(sampler.min_filter == VK_FILTER_LINEAR && sampler.mag_filter == VK_FILTER_LINEAR);
    CHECK(sampler.wrap_s == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT &&
          sampler.wrap_t == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CHECK(sampler.border == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);  // no wrap uses the border

    // A mirror-once wrap and a custom border color are replaced and counted.
    texture.address = 6u | (4u << 8);
    texture.border = 0xFF204060;
    sampler = tox2::samplerSettings(texture);
    CHECK(sampler.wrap_s == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
          sampler.border == VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
    CHECK(unimplementedCount("texture sampler mirror once wrap") == 1 &&
          unimplementedCount("texture sampler custom border color") == 1);
    tox2::rsxUnimplementedReset();
}

// Position in slot 0, 2D texture coordinates in slot 8 (TEX0): the vertex program moves both to its
// outputs; the fragment program is TEX H0, TEX0.
tox2::rsx_synthetic::Programs texturedPrograms() {
    tox2::rsx_synthetic::Programs programs;
    programs.vertex.clear();
    for (const auto& instruction : {vpMove(0, 0, "xyzw", false), vpMove(7, 8, "xyzw", true)}) {
        programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
    }
    constexpr std::array<std::uint32_t, 4> kTex = {0x17009e81, 0x1c9dc801, 0x0001c800, 0x0001c800};
    programs.fragment.clear();
    for (const std::uint32_t word : kTex) programs.fragment.push_back(tox2::fpFixupWord(word));
    return programs;
}

enum class TextureScene {
    Memory,    // the 2x2 texture in guest memory
    Alias,     // a cleared render target at another offset
    Feedback,  // the render target the draw writes to
    Chain,     // a 4x4 texture with three mip levels, swizzled or linear
};

struct TextureOptions {
    TextureScene scene = TextureScene::Memory;
    std::uint32_t remap = 0xAAE4;
    std::uint32_t wrap = 1;  // WRAP in S and T
    std::uint32_t border = 0;
    float coord_scale = 1.0f;   // texture coordinates run 0..coord_scale over the quad
    std::uint32_t pitch = 0;    // Memory scene: row pitch in bytes (0: no padding); padding holds junk
    bool unnormalized = false;  // _UN format: coordinates are in texels
    std::uint32_t dxt = 0;      // Memory scene: base format 0x86/0x87/0x88 for a 4x4 block-compressed texture
    bool zero_height = false;   // Memory scene: the image rect says 0 rows, so no bytes are copied
    // Chain scene: the swizzled layout (else linear with a 32-byte pitch), and the min filter, NEAREST_NEAREST
    // by default; 1 is NEAREST, which samples level 0 only.
    bool swizzled = false;
    std::uint32_t min_filter = 3;
};

// Chain scene: level 0 texel (x, y) has red 85 * x, green 85 * y and blue 0x40; level 1 (2x2) is green and
// level 2 (1x1) is blue.
constexpr std::uint32_t chainTexel(std::uint32_t x, std::uint32_t y) {
    return 0xFF000040u | ((x * 85u) << 16) | ((y * 85u) << 8);
}

// Guest bytes of the chain: swizzled levels follow each other (64, 16 and 4 bytes, Morton order inside a
// level); linear levels keep the 32-byte pitch of level 0 (4 rows, 2 rows, 1 row).
void putChainTexture(tox2::rsx_synthetic::SyntheticMemory& memory, bool swizzled) {
    using tox2::rsx_synthetic::kTextureOffset;
    constexpr std::uint32_t kGreen = 0xFF00FF00, kBlue = 0xFF0000FF;
    for (std::uint32_t y = 0; y < 4; y++) {
        for (std::uint32_t x = 0; x < 4; x++) {
            const std::uint32_t at = swizzled ? tox2::rsxSwizzleIndex(x, y, 2, 2) * 4 : y * 32 + x * 4;
            memory.putWord(0, kTextureOffset + at, chainTexel(x, y));
        }
    }
    for (std::uint32_t y = 0; y < 2; y++) {
        for (std::uint32_t x = 0; x < 2; x++) {
            memory.putWord(0, kTextureOffset + (swizzled ? 64 + (y * 2 + x) * 4 : 128 + y * 32 + x * 4), kGreen);
        }
    }
    memory.putWord(0, kTextureOffset + (swizzled ? 80 : 192), kBlue);
}

constexpr std::uint32_t kAliasOffset = 0x50000;
constexpr std::uint32_t kAliasColor = 0xFF336699;

// A quad covering pixels 16..48 whose corners carry the texture coordinates (0, 0), (1, 0), (0, 1), (1, 1)
// times coord_scale, in strip order top left, top right, bottom left, bottom right.
std::vector<std::uint8_t> makeTexturedCapture(const TextureOptions& options) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    if (options.scene == TextureScene::Alias) {
        scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kAliasOffset);
        scene.clear(kAliasColor);
        scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kSurfaceOffset);
        scene.clear(0xFF000000);
        scene.bindTexture(kAliasOffset, kSurfaceSize, kSurfaceSize);
    } else if (options.scene == TextureScene::Feedback) {
        scene.clear(kAliasColor);
        scene.bindTexture(kSurfaceOffset, kSurfaceSize, kSurfaceSize);
    } else if (options.scene == TextureScene::Chain) {
        scene.clear(0xFF000000);
        putChainTexture(scene.memory(), options.swizzled);
        scene.bindTexture(kTextureOffset, 4, 4);
        scene.write(tox2::kMethodNv4097SetTextureFormat,
                    1u | (2u << 4) | ((options.swizzled ? 0x85u : 0xA5u) << 8) | (3u << 16));
        scene.write(tox2::kMethodNv4097SetTextureControl0, 0x80000000u | (0xC00u << 7));  // levels 0..12
        scene.write(tox2::kMethodNv4097SetTextureControl3, options.swizzled ? 0 : 32);
        scene.write(tox2::kMethodNv4097SetTextureFilter, (options.min_filter << 16) | (1u << 24));
    } else {
        scene.clear(0xFF000000);
        const std::uint32_t pitch = options.pitch != 0 ? options.pitch : kTextureSize * 4;
        for (std::uint32_t y = 0; y < kTextureSize; y++) {
            for (std::uint32_t x = 0; x < pitch / 4; x++) {
                scene.memory().putWord(0, kTextureOffset + y * pitch + x * 4,
                                       x < kTextureSize ? kTexturePixels[y * kTextureSize + x] : 0xDEADBEEF);
            }
        }
        scene.bindTexture(kTextureOffset, kTextureSize, kTextureSize, pitch, options.unnormalized ? 0x40u : 0u);
        if (options.dxt != 0) {
            // One 4x4 block (alpha bytes first for DXT23 and DXT45). Colors: red 0xF800 and blue 0x001F as
            // little-endian halves, then one row of 2-bit indices per byte: color0, color1, color2, color3.
            std::uint32_t at = kTextureOffset;
            if (options.dxt == 0x87) {
                scene.memory().putWord(0, at, 0x88888888);  // explicit 4-bit alpha 8 for every texel
                scene.memory().putWord(0, at + 4, 0x88888888);
                at += 8;
            } else if (options.dxt == 0x88) {
                scene.memory().putWord(0, at, 0x80800000);  // both alpha endpoints 0x80, all indices 0
                scene.memory().putWord(0, at + 4, 0);
                at += 8;
            }
            scene.memory().putWord(0, at, 0x00F81F00);
            scene.memory().putWord(0, at + 4, 0x0055AAFF);
            // The pitch register of a compressed texture is ignored; give it a value that is not the row size.
            scene.write(tox2::kMethodNv4097SetTextureFormat,
                        1u | (2u << 4) | ((options.dxt | 0x20u) << 8) | (1u << 16));
            scene.write(tox2::kMethodNv4097SetTextureImageRect, (4u << 16) | 4u);
            scene.write(tox2::kMethodNv4097SetTextureControl3, 0);
        }
        if (options.zero_height) scene.write(tox2::kMethodNv4097SetTextureImageRect, kTextureSize << 16);
    }
    scene.write(tox2::kMethodNv4097SetTextureControl1, options.remap);
    scene.write(tox2::kMethodNv4097SetTextureAddress, options.wrap | (options.wrap << 8) | (1u << 16));
    scene.write(tox2::kMethodNv4097SetTextureBorderColor, options.border);

    setQuadRenderState(scene);
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);  // TEX0
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);              // color from H0
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 1);                // 2D coordinate
    for (std::uint32_t v = 0; v < 4; v++) {
        for (std::uint32_t c = 0; c < 3; c++)
            scene.memory().putFloat(1, kPositionsOffset + v * 12 + c * 4, kQuadCorners[v][c]);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8, (v & 1) != 0 ? options.coord_scale : 0.0f);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8 + 4, (v & 2) != 0 ? options.coord_scale : 0.0f);
    }
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setFloatAttrib(8, 2, kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());
    scene.drawStrip(4);
    return scene.finish();
}

// Two quads (pixels 4..28 and 36..60) textured by the same 2x2 texture; the texel words change between
// the draws, so the second quad must show yellow, cyan, magenta and grey instead. A semaphore acquire sits
// between them, the kind of synchronization a game does before it rewrites texture memory: the frame builder
// only looks at guest memory again after one.
std::vector<std::uint8_t> makeChangedTextureCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(0xFF000000);
    scene.setTexture();
    setQuadRenderState(scene);
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 1);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setFloatAttrib(8, 2, kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());

    const auto quad = [&](float x0, float x1) {
        const float xs[4] = {x0, x1, x0, x1};
        for (std::uint32_t v = 0; v < 4; v++) {
            scene.memory().putFloat(1, kPositionsOffset + v * 12, xs[v]);
            scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, kQuadCorners[v][1]);
            scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
            scene.memory().putFloat(1, kTexCoordsOffset + v * 8, (v & 1) != 0 ? 1.0f : 0.0f);
            scene.memory().putFloat(1, kTexCoordsOffset + v * 8 + 4, (v & 2) != 0 ? 1.0f : 0.0f);
        }
        scene.drawStrip(4);
    };
    quad(-0.875f, -0.125f);
    constexpr std::uint32_t kSecond[4] = {0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF, 0xFF808080};
    for (std::uint32_t i = 0; i < 4; i++) {
        scene.memory().putWord(0, kTextureOffset + (i / kTextureSize) * (kTextureSize * 4) + (i % kTextureSize) * 4,
                               kSecond[i]);
    }
    scene.write(tox2::kMethodNv406eSemaphoreAcquire, 0);
    quad(0.125f, 0.875f);
    return scene.finish();
}

void testRendererTextures(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    const auto render = [&](const TextureOptions& options) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeTexturedCapture(options));
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
        if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
    };
    const auto expectNoNotes = [] {
        for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
            std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
        }
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
    };

    // The 2x2 texture magnified 16 times with NEAREST filters: each texel fills a quarter of the quad, and
    // the pixels outside it keep the clear color.
    render({});
    CHECK(pixelArgb(bgra, width, 24, 24) == 0xFFFF0000);
    CHECK(pixelArgb(bgra, width, 40, 24) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 24, 40) == 0xFF0000FF);
    CHECK(pixelArgb(bgra, width, 40, 40) == 0x80FFFFFF);
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000 && pixelArgb(bgra, width, 61, 61) == 0xFF000000);
    CHECK(renderer.stats().draws_skipped == 0 && renderer.stats().texture_uploads == 1);
    expectNoNotes();

    // The remap exchanges red and blue.
    TextureOptions swapped;
    swapped.remap = 0xAA6C;
    render(swapped);
    CHECK(pixelArgb(bgra, width, 24, 24) == 0xFF0000FF);
    CHECK(pixelArgb(bgra, width, 40, 24) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 24, 40) == 0xFFFF0000);
    expectNoNotes();

    // The remap forces alpha to one: the half-transparent texel becomes opaque.
    TextureOptions opaque;
    opaque.remap = 0xA9E4;
    render(opaque);
    CHECK(pixelArgb(bgra, width, 40, 40) == 0xFFFFFFFF);
    CHECK(pixelArgb(bgra, width, 24, 24) == 0xFFFF0000);
    expectNoNotes();

    // Coordinates run 0..2 over the quad. WRAP repeats the texture; BORDER returns the border color past
    // 1. The quad's pixels 16..32 hold the texture once (texels switch at 24), 32..48 the second period.
    TextureOptions repeat;
    repeat.coord_scale = 2.0f;
    render(repeat);
    CHECK(pixelArgb(bgra, width, 20, 20) == 0xFFFF0000 && pixelArgb(bgra, width, 28, 20) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 36, 20) == 0xFFFF0000 && pixelArgb(bgra, width, 44, 20) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 20, 44) == 0xFF0000FF);
    expectNoNotes();

    TextureOptions border = repeat;
    border.wrap = 4;
    border.border = 0xFFFFFFFF;
    render(border);
    CHECK(pixelArgb(bgra, width, 20, 20) == 0xFFFF0000 && pixelArgb(bgra, width, 28, 20) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 20, 28) == 0xFF0000FF);
    CHECK(pixelArgb(bgra, width, 44, 20) == 0xFFFFFFFF && pixelArgb(bgra, width, 20, 44) == 0xFFFFFFFF &&
          pixelArgb(bgra, width, 44, 44) == 0xFFFFFFFF);
    expectNoNotes();

    // An opaque black border reads as opaque black (not as transparent black) past the texture, and the
    // remap applies to the border: transparent black with alpha forced to one is opaque black.
    border.border = 0xFF000000;
    render(border);
    CHECK(pixelArgb(bgra, width, 20, 20) == 0xFFFF0000);
    CHECK(pixelArgb(bgra, width, 44, 20) == 0xFF000000 && pixelArgb(bgra, width, 44, 44) == 0xFF000000);
    expectNoNotes();
    border.border = 0x00000000;
    border.remap = 0xA9E4;
    render(border);
    CHECK(pixelArgb(bgra, width, 44, 20) == 0xFF000000 && pixelArgb(bgra, width, 20, 44) == 0xFF000000);
    expectNoNotes();

    // A row pitch of 16 bytes for 2-texel rows: the junk after each row is not sampled.
    TextureOptions padded;
    padded.pitch = 16;
    render(padded);
    CHECK(pixelArgb(bgra, width, 24, 24) == 0xFFFF0000 && pixelArgb(bgra, width, 40, 24) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 24, 40) == 0xFF0000FF && pixelArgb(bgra, width, 40, 40) == 0x80FFFFFF);
    expectNoNotes();

    // Unnormalized coordinates run 0..2 (texels) over the quad and give the same picture.
    TextureOptions unnormalized;
    unnormalized.unnormalized = true;
    unnormalized.coord_scale = 2.0f;
    render(unnormalized);
    CHECK(pixelArgb(bgra, width, 24, 24) == 0xFFFF0000 && pixelArgb(bgra, width, 40, 24) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 24, 40) == 0xFF0000FF && pixelArgb(bgra, width, 40, 40) == 0x80FFFFFF);
    expectNoNotes();

    // Two draws of one texture (same key) with the guest bytes changed in between: each quad shows the
    // bytes of its own draw.
    {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeChangedTextureCapture());
        tox2::rsxUnimplementedReset();
        const std::uint64_t uploads_before = renderer.stats().texture_uploads;
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
        CHECK(pixelArgb(bgra, width, 10, 24) == 0xFFFF0000 && pixelArgb(bgra, width, 22, 40) == 0x80FFFFFF);
        CHECK(pixelArgb(bgra, width, 42, 24) == 0xFFFFFF00 && pixelArgb(bgra, width, 54, 40) == 0xFF808080);
        CHECK(renderer.stats().draws_skipped == 0);
        // The two draws carry different content ids, so the one image is uploaded for each of them.
        CHECK(renderer.stats().texture_uploads == uploads_before + 2);
        expectNoNotes();
    }

    // An image is uploaded from a blob once: the same commands again carry the same content ids and upload
    // nothing, and a new content id (the guest bytes changed) uploads again. A blob with no id is always new.
    {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeTexturedCapture({}));
        tox2::rsxUnimplementedReset();
        const auto texture = [&]() -> tox2::RsxTextureDesc& {
            for (tox2::RsxCommand& command : commands) {
                if (auto* draw = std::get_if<tox2::RsxDraw>(&command)) return draw->textures.at(0);
            }
            std::abort();
        };
        CHECK(texture().data != nullptr && texture().content_id != 0);
        const std::uint64_t uploads_before = renderer.stats().texture_uploads;
        renderer.execute(commands);
        CHECK(renderer.stats().texture_uploads == uploads_before + 1);
        renderer.execute(commands);
        CHECK(renderer.stats().texture_uploads == uploads_before + 1);
        texture().content_id = tox2::RsxTextureCache::nextContentId();
        renderer.execute(commands);
        CHECK(renderer.stats().texture_uploads == uploads_before + 2);
        texture().content_id = 0;
        renderer.execute(commands);
        renderer.execute(commands);
        CHECK(renderer.stats().texture_uploads == uploads_before + 4);
        CHECK(renderer.valid() && renderer.stats().draws_skipped == 0);
        expectNoNotes();
    }

    // A texture at another render target samples that target's cleared contents, with no upload.
    const std::uint64_t uploads = renderer.stats().texture_uploads;
    TextureOptions alias;
    alias.scene = TextureScene::Alias;
    render(alias);
    CHECK(pixelArgb(bgra, width, 32, 32) == kAliasColor && pixelArgb(bgra, width, 18, 45) == kAliasColor);
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    CHECK(renderer.stats().texture_uploads == uploads && renderer.stats().feedback_copies == 0);
    expectNoNotes();

    // The remap applies to an alias too: 0x336699 with red and blue exchanged is 0x996633.
    alias.remap = 0xAA6C;
    render(alias);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFF996633);
    expectNoNotes();

    // Sampling the target the draw renders to reads a copy taken before the draw: the quad shows the
    // exchanged colors and the rest of the target keeps its clear color.
    TextureOptions feedback;
    feedback.scene = TextureScene::Feedback;
    feedback.remap = 0xAA6C;
    render(feedback);
    CHECK(renderer.stats().feedback_copies == 1);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFF996633 && pixelArgb(bgra, width, 2, 2) == kAliasColor);
    expectNoNotes();

    // 4x4 block-compressed textures magnified 8 times: texel rows 0 and 1 hold color0 (red) and color1
    // (blue) exactly, row 2 and 3 their blends. DXT1 is opaque, DXT23 carries alpha 0x88 and DXT45 0x80.
    if (ctx.hasTextureBC()) {
        const std::uint64_t uploads_before = renderer.stats().texture_uploads;
        const auto within = [](std::uint32_t argb, std::uint32_t expected) {
            for (std::uint32_t shift = 0; shift < 32; shift += 8) {
                const int a = static_cast<int>((argb >> shift) & 0xFF);
                const int b = static_cast<int>((expected >> shift) & 0xFF);
                if (std::abs(a - b) > 2) return false;
            }
            return true;
        };
        for (const std::uint32_t dxt : {0x86u, 0x87u, 0x88u}) {
            TextureOptions block;
            block.dxt = dxt;
            render(block);
            const std::uint32_t alpha = dxt == 0x86 ? 0xFF000000u : dxt == 0x87 ? 0x88000000u : 0x80000000u;
            CHECK(pixelArgb(bgra, width, 20, 20) == (alpha | 0xFF0000));
            CHECK(pixelArgb(bgra, width, 44, 28) == (alpha | 0x0000FF));
            CHECK(within(pixelArgb(bgra, width, 30, 36), alpha | 0xAA0055));
            CHECK(within(pixelArgb(bgra, width, 30, 44), alpha | 0x5500AA));
            CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
            expectNoNotes();
        }
        CHECK(renderer.stats().texture_uploads == uploads_before + 3);

        // The remap applies to compressed images as to A8R8G8B8 ones.
        TextureOptions swapped_block;
        swapped_block.dxt = 0x86;
        swapped_block.remap = 0xAA6C;
        render(swapped_block);
        CHECK(pixelArgb(bgra, width, 20, 20) == 0xFF0000FF && pixelArgb(bgra, width, 44, 28) == 0xFFFF0000);
        expectNoNotes();
    }

    // A swizzled 4x4 texture with three levels, magnified 8 times so that level 0 is sampled: each texel fills
    // 8x8 pixels of the quad at 16..48 and comes from its Morton position in the guest bytes.
    {
        const std::uint64_t uploads_before = renderer.stats().texture_uploads;
        TextureOptions chain;
        chain.scene = TextureScene::Chain;
        chain.swizzled = true;
        render(chain);
        bool all = true;
        for (std::uint32_t y = 0; y < 4; y++) {
            for (std::uint32_t x = 0; x < 4; x++) {
                all = all && pixelArgb(bgra, width, 20 + 8 * x, 20 + 8 * y) == chainTexel(x, y);
            }
        }
        CHECK(all);
        CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
        CHECK(renderer.stats().draws_skipped == 0 && renderer.stats().texture_uploads == uploads_before + 1);
        expectNoNotes();

        // The same bytes minified: coordinates run 0..16 over the quad, 2 texels per pixel, which picks level 1
        // (green); 0..32 is 4 texels per pixel and picks level 2 (blue). With a NEAREST min filter only
        // level 0 is sampled whatever the coordinates are. Linear levels keep the pitch of level 0.
        for (const bool swizzled : {true, false}) {
            chain.swizzled = swizzled;
            chain.min_filter = 3;
            chain.coord_scale = 16.0f;
            render(chain);
            CHECK(pixelArgb(bgra, width, 24, 24) == 0xFF00FF00 && pixelArgb(bgra, width, 41, 37) == 0xFF00FF00);
            chain.coord_scale = 32.0f;
            render(chain);
            CHECK(pixelArgb(bgra, width, 24, 24) == 0xFF0000FF && pixelArgb(bgra, width, 41, 37) == 0xFF0000FF);
            chain.min_filter = 1;
            render(chain);
            CHECK(pixelArgb(bgra, width, 24, 24) != 0xFF0000FF && pixelArgb(bgra, width, 24, 24) != 0xFF00FF00);
            expectNoNotes();
        }
    }

    // A texture without bytes samples the magenta placeholder, and that is counted.
    TextureOptions empty;
    empty.zero_height = true;
    render(empty);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFFFF00FF);
    CHECK(unimplementedCount("texture placeholder: no texture data") == 1);

    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// ---- 3D and cube textures ----

// Two quads (pixels 4..28 and 36..60) that sample unit 0 with three coordinates, TEX H0, TEX0 as in
// texturedPrograms(). The unit holds a 2x2x2 linear A8R8G8B8 3D texture: slice 0 is the 2x2 texture
// (red, green / blue, half-transparent white), slice 1 yellow, cyan / magenta, grey. Or it holds a cube map
// of 2x2 faces, each face one color (+X red, -X green, +Y blue, -Y yellow, +Z cyan, -Z magenta). The
// texture coordinates run 0..1 in u and v over a quad; w (3D) or the direction (cube) is constant per quad.
// Without `bytes` the image rect has no rows, so no texture bytes are copied and the draw reads a placeholder.
struct VolumeOptions {
    bool cube = false;
    bool bytes = true;
    float left[3] = {0.0f, 0.0f, 0.25f};   // 3D: w of the left quad is left[2]; cube: its direction
    float right[3] = {0.0f, 0.0f, 0.75f};  // the same for the right quad
    bool nearest = true;                   // else LINEAR filters
    bool texel_center = false;             // 3D: u = v = 0.25 over the whole quad, the center of texel (0, 0)
    std::uint32_t slices = 2;              // 3D: depth register; one slice is still a 3D texture
};

constexpr std::uint32_t kVolumeSlice1[4] = {0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF, 0xFF808080};
constexpr std::uint32_t kCubeFaces[6] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF};

std::vector<std::uint8_t> makeVolumeCapture(const VolumeOptions& options) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(0xFF000000);
    scene.bindTexture(kTextureOffset, 2, 2);
    if (options.cube) {
        for (std::uint32_t face = 0; face < 6; face++) {
            for (std::uint32_t texel = 0; texel < 4; texel++) {
                scene.memory().putWord(0, kTextureOffset + face * 16 + (texel / 2) * 8 + (texel % 2) * 4,
                                       kCubeFaces[face]);
            }
        }
        scene.write(tox2::kMethodNv4097SetTextureFormat, 1u | (1u << 2) | (2u << 4) | (0xA5u << 8) | (1u << 16));
        scene.write(tox2::kMethodNv4097SetTextureControl3, 8);
    } else {
        for (std::uint32_t texel = 0; texel < 4; texel++) {
            scene.memory().putWord(0, kTextureOffset + (texel / 2) * 8 + (texel % 2) * 4, kTexturePixels[texel]);
            scene.memory().putWord(0, kTextureOffset + 16 + (texel / 2) * 8 + (texel % 2) * 4, kVolumeSlice1[texel]);
        }
        scene.write(tox2::kMethodNv4097SetTextureFormat, 1u | (3u << 4) | (0xA5u << 8) | (1u << 16));
        scene.write(tox2::kMethodNv4097SetTextureControl3, (options.slices << 20) | 8);
    }
    if (!options.bytes) scene.write(tox2::kMethodNv4097SetTextureImageRect, 2u << 16);
    if (!options.nearest) scene.write(tox2::kMethodNv4097SetTextureFilter, (2u << 16) | (2u << 24));

    setQuadRenderState(scene);
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);  // TEX0
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);              // color from H0
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 0);                // 3D coordinate
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setFloatAttrib(8, 3, kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());

    const auto quad = [&](float x0, float x1, const float (&third)[3]) {
        const float xs[4] = {x0, x1, x0, x1};
        for (std::uint32_t v = 0; v < 4; v++) {
            scene.memory().putFloat(1, kPositionsOffset + v * 12, xs[v]);
            scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, kQuadCorners[v][1]);
            scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
            // A cube lookup takes the direction as it is; a 3D lookup takes u and v from the corner.
            const float u = options.texel_center ? 0.25f : (v & 1) != 0 ? 1.0f : 0.0f;
            const float t = options.texel_center ? 0.25f : (v & 2) != 0 ? 1.0f : 0.0f;
            scene.memory().putFloat(1, kTexCoordsOffset + v * 12, options.cube ? third[0] : u);
            scene.memory().putFloat(1, kTexCoordsOffset + v * 12 + 4, options.cube ? third[1] : t);
            scene.memory().putFloat(1, kTexCoordsOffset + v * 12 + 8, third[2]);
        }
        scene.drawStrip(4);
    };
    quad(-0.875f, -0.125f, options.left);
    quad(0.125f, 0.875f, options.right);
    return scene.finish();
}

void testRendererVolumeTextures(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const auto render = [&](const VolumeOptions& options) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeVolumeCapture(options));
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height));
        if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
    };
    const auto expectNoNotes = [] {
        for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
            std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
        }
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
    };

    // The left quad reads the slice at w = 0.25 (slice 0), the right one the slice at w = 0.75 (slice 1); u
    // and v pick the texel as in 2D.
    render({});
    CHECK(pixelArgb(bgra, width, 10, 24) == 0xFFFF0000 && pixelArgb(bgra, width, 22, 24) == 0xFF00FF00);
    CHECK(pixelArgb(bgra, width, 10, 40) == 0xFF0000FF && pixelArgb(bgra, width, 22, 40) == 0x80FFFFFF);
    CHECK(pixelArgb(bgra, width, 42, 24) == 0xFFFFFF00 && pixelArgb(bgra, width, 54, 24) == 0xFF00FFFF);
    CHECK(pixelArgb(bgra, width, 42, 40) == 0xFFFF00FF && pixelArgb(bgra, width, 54, 40) == 0xFF808080);
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    // Both draws sample the same unchanged texture: one blob, one upload.
    CHECK(renderer.stats().draws_skipped == 0 && renderer.stats().texture_uploads == 1);
    expectNoNotes();

    // A single slice is still a 3D texture: both quads read it (w repeats).
    VolumeOptions flat;
    flat.slices = 1;
    render(flat);
    CHECK(pixelArgb(bgra, width, 10, 24) == 0xFFFF0000 && pixelArgb(bgra, width, 42, 24) == 0xFFFF0000);
    CHECK(pixelArgb(bgra, width, 22, 40) == 0x80FFFFFF && pixelArgb(bgra, width, 54, 40) == 0x80FFFFFF);
    expectNoNotes();

    // With a linear filter, w = 0.5 is halfway between the slices: at the center of texel (0, 0) red and
    // yellow average to (255, 128, 0).
    VolumeOptions halfway;
    halfway.nearest = false;
    halfway.texel_center = true;
    halfway.left[2] = halfway.right[2] = 0.5f;
    render(halfway);
    const std::uint32_t mixed = pixelArgb(bgra, width, 16, 32);
    CHECK(((mixed >> 16) & 0xFF) >= 250 && std::abs(static_cast<int>((mixed >> 8) & 0xFF) - 128) <= 3 &&
          (mixed & 0xFF) <= 3);
    expectNoNotes();

    // Each direction selects its face.
    VolumeOptions faces;
    faces.cube = true;
    faces.left[0] = 1.0f;
    faces.left[1] = 0.0f;
    faces.left[2] = 0.0f;
    faces.right[0] = 0.0f;
    faces.right[1] = -1.0f;
    faces.right[2] = 0.0f;
    render(faces);
    CHECK(pixelArgb(bgra, width, 10, 24) == kCubeFaces[0] && pixelArgb(bgra, width, 22, 40) == kCubeFaces[0]);
    CHECK(pixelArgb(bgra, width, 42, 24) == kCubeFaces[3] && pixelArgb(bgra, width, 54, 40) == kCubeFaces[3]);
    CHECK(renderer.stats().draws_skipped == 0);
    expectNoNotes();
    faces.left[0] = -0.2f;
    faces.left[1] = 0.1f;
    faces.left[2] = 1.0f;  // +Z is the major axis
    faces.right[0] = 0.0f;
    faces.right[1] = 0.3f;
    faces.right[2] = -2.0f;  // -Z
    render(faces);
    CHECK(pixelArgb(bgra, width, 16, 32) == kCubeFaces[4] && pixelArgb(bgra, width, 48, 32) == kCubeFaces[5]);
    expectNoNotes();

    // A 3D or cube texture without bytes reads the placeholder of its type (a placeholder of another view type
    // would be a validation error) and is counted.
    for (const bool cube : {false, true}) {
        VolumeOptions empty;
        empty.cube = cube;
        empty.bytes = false;
        if (cube) empty.left[2] = empty.right[2] = 1.0f;
        render(empty);
        CHECK(pixelArgb(bgra, width, 10, 24) == 0xFFFF00FF && pixelArgb(bgra, width, 54, 40) == 0xFFFF00FF);
        CHECK(unimplementedCount("texture placeholder: no texture data") == 2);
        CHECK(renderer.stats().draws_skipped == 0);
    }
    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// ---- Float and depth surfaces as textures ----

constexpr std::uint32_t kFloatOffset = 0x60000;  // the F_X32 target; never read, only addressed
constexpr std::uint32_t kDepthOffset = tox2::rsx_synthetic::kSurfaceOffset + 0x10000;  // SceneBuilder::setSurface

// A quad over pixels x0..x1 by 16..48 of the 64x64 target. Its texture coordinates select the same texels of a
// 64x64 texture, so a surface drawn at the same pixels shows up where it was written.
void putSurfaceQuad(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t x0, std::uint32_t x1) {
    using namespace tox2::rsx_synthetic;
    for (std::uint32_t v = 0; v < 4; v++) {
        const std::uint32_t x = (v & 1) != 0 ? x1 : x0;
        scene.memory().putFloat(1, kPositionsOffset + v * 12, (static_cast<float>(x) - 32.0f) / 32.0f);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, kQuadCorners[v][1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8, static_cast<float>(x) / 64.0f);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8 + 4, (v & 2) != 0 ? 0.25f : 0.75f);
        scene.memory().putWord(1, kColorsOffset + v * 4, 0xFF400000);  // red 0x40
    }
}

// The textured pass of both scenes: unit 0 reads `offset` as a 64x64 linear texture of the base format `format`.
void setSurfaceTextureState(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t offset, std::uint32_t format) {
    scene.bindTexture(offset, 64, 64);
    scene.write(tox2::kMethodNv4097SetTextureFormat, 1u | (2u << 4) | ((format | 0x20u) << 8) | (1u << 16));
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);  // TEX0
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);              // color from H0
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 1);                // 2D coordinate
    scene.setFloatAttrib(8, 2, tox2::rsx_synthetic::kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());
}

// Pass 1 draws a quad with red 0x40 over pixels 16..32 into an F_X32 target, with an alpha test that never
// passes (a float target has no alpha test). Pass 2 clears the display target and draws a quad that reads the
// float target through an X32_FLOAT texture over pixels 16..48.
std::vector<std::uint8_t> makeFloatSurfaceCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetSurfaceFormat, 13u | (2u << 5) | (1u << 8));
    scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kFloatOffset);
    scene.write(tox2::kMethodNv4097SetAlphaTestEnable, 1);
    scene.write(tox2::kMethodNv4097SetAlphaFunc, 0x200);  // NEVER
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    putSurfaceQuad(scene, 16, 32);
    scene.drawStrip(4);

    scene.write(tox2::kMethodNv4097SetSurfaceFormat, 8u | (2u << 5) | (1u << 8));
    scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kSurfaceOffset);
    scene.write(tox2::kMethodNv4097SetAlphaTestEnable, 0);
    scene.clear(0xFF000000);
    setSurfaceTextureState(scene, kFloatOffset, 0x9C);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
    return scene.finish();
}

void testRendererFloatSurface(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<tox2::RsxCommand> commands = replaySynthetic(makeFloatSurfaceCapture());
    tox2::rsxUnimplementedReset();
    renderer.execute(commands);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CHECK(renderer.readDisplay(bgra, width, height));
    CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
    if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
    // The float target holds 0x40 / 255 where the first quad was drawn and zero elsewhere. A float texture reads
    // as (x, x, x, x), so the display pixel has every channel, alpha included, at that value.
    CHECK(pixelArgb(bgra, width, 24, 32) == 0x40404040);
    CHECK(pixelArgb(bgra, width, 40, 32) == 0x00000000);
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    CHECK(renderer.stats().draws == 2 && renderer.stats().draws_skipped == 0);
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();
}

// Two 24-bit depth values with their stencil: the Z24S8 word read as A8R8G8B8 is A = depth bits 23..16,
// R = 15..8, G = 7..0, B = stencil.
constexpr std::uint32_t kDepthFirst = 0x123456, kStencilFirst = 0x78;
constexpr std::uint32_t kDepthSecond = 0x6789A0, kStencilSecond = 0x9A;
constexpr std::uint32_t depthWordArgb(std::uint32_t depth, std::uint32_t stencil) {
    return ((depth >> 16) << 24) | (((depth >> 8) & 0xFF) << 16) | ((depth & 0xFF) << 8) | stencil;
}

// The depth is stored as a float or a 24-bit integer, so the low byte of the 24-bit value may be one off.
bool depthWordNear(std::uint32_t argb, std::uint32_t depth, std::uint32_t stencil) {
    const std::uint32_t expected = depthWordArgb(depth, stencil);
    const int low = static_cast<int>((argb >> 8) & 0xFF) - static_cast<int>((expected >> 8) & 0xFF);
    return (argb & 0xFFFF00FF) == (expected & 0xFFFF00FF) && std::abs(low) <= 1;
}

// The depth surface is cleared to the first value, two quads (pixels 16..32 and 32..48) read it as an A8R8G8B8
// texture, then a clear of depth and stencil alone sets the second value and a small quad (28..36) reads it.
std::vector<std::uint8_t> makeDepthTextureCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetColorClearValue, 0xFF000000);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, (kDepthFirst << 8) | kStencilFirst);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF3);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    setSurfaceTextureState(scene, kDepthOffset, 0x85);
    putSurfaceQuad(scene, 16, 32);
    scene.drawStrip(4);
    putSurfaceQuad(scene, 32, 48);
    scene.drawStrip(4);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, (kDepthSecond << 8) | kStencilSecond);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    putSurfaceQuad(scene, 28, 36);
    scene.drawStrip(4);
    return scene.finish();
}

void testRendererDepthTexture(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<tox2::RsxCommand> commands = replaySynthetic(makeDepthTextureCapture());
    tox2::rsxUnimplementedReset();
    renderer.execute(commands);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CHECK(renderer.readDisplay(bgra, width, height));
    CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
    if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
    CHECK(depthWordNear(pixelArgb(bgra, width, 20, 32), kDepthFirst, kStencilFirst));
    CHECK(depthWordNear(pixelArgb(bgra, width, 44, 32), kDepthFirst, kStencilFirst));
    CHECK(depthWordNear(pixelArgb(bgra, width, 32, 32), kDepthSecond, kStencilSecond));
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    // The surface is rewritten once for the two draws that read it unchanged, and again after the clear.
    CHECK(renderer.stats().depth_conversions == 2);
    CHECK(renderer.stats().draws == 3 && renderer.stats().draws_skipped == 0);
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();
}

// A clear that includes stencil under a write mask other than 0 or 0xFF is counted by the frame
// builder (the renderer clears the whole byte).
void testPartialStencilNote() {
    const auto clearWithStencilMask = [](std::uint32_t stencil_mask) {
        tox2::rsx_synthetic::SceneBuilder scene;
        scene.setSurface();
        scene.write(tox2::kMethodNv4097SetStencilMask, stencil_mask);
        scene.clear(0xFF000000);
        // Recording runs a frame builder of its own; only the replay's notes are counted.
        const std::vector<std::uint8_t> file = scene.finish();
        tox2::rsxUnimplementedReset();
        return replaySynthetic(file);
    };
    clearWithStencilMask(0xFF);
    CHECK(unimplementedCount("state partial stencil clear mask") == 0);
    clearWithStencilMask(0x00);
    CHECK(unimplementedCount("state partial stencil clear mask") == 0);
    const std::vector<tox2::RsxCommand> commands = clearWithStencilMask(0x0F);
    CHECK(commands.size() == 2 && std::get<tox2::RsxClear>(commands[0]).mask == 0xF3);
    CHECK(unimplementedCount("state partial stencil clear mask") == 1);
    tox2::rsxUnimplementedReset();
}

// True when the last replay counted no stencil func or op word.
bool noStencilWordNotes() {
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        if (name.starts_with("state stencil func") || name.starts_with("state stencil op")) return false;
    }
    return true;
}

struct StencilFaceWords {
    std::uint32_t func, ref, func_mask, write_mask, op_fail, op_zfail, op_zpass;
};

bool sameFace(const tox2::RsxStencilFace& face, const StencilFaceWords& words) {
    return face.func == words.func && face.ref == (words.ref & 0xFF) && face.func_mask == (words.func_mask & 0xFF) &&
           face.write_mask == (words.write_mask & 0xFF) && face.op_fail == words.op_fail &&
           face.op_zfail == words.op_zfail && face.op_zpass == words.op_zpass;
}

// One draw with the given front and back stencil registers; the replay's notes are the only ones counted.
tox2::RsxDraw replayStencilDraw(bool test, bool two_sided, const StencilFaceWords& front,
                                const StencilFaceWords& back) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, test ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetTwoSidedStencilTestEnable, two_sided ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetStencilFunc, front.func);
    scene.write(tox2::kMethodNv4097SetStencilFuncRef, front.ref);
    scene.write(tox2::kMethodNv4097SetStencilFuncMask, front.func_mask);
    scene.write(tox2::kMethodNv4097SetStencilMask, front.write_mask);
    scene.write(tox2::kMethodNv4097SetStencilOpFail, front.op_fail);
    scene.write(tox2::kMethodNv4097SetStencilOpZfail, front.op_zfail);
    scene.write(tox2::kMethodNv4097SetStencilOpZpass, front.op_zpass);
    scene.write(tox2::kMethodNv4097SetBackStencilFunc, back.func);
    scene.write(tox2::kMethodNv4097SetBackStencilFuncRef, back.ref);
    scene.write(tox2::kMethodNv4097SetBackStencilFuncMask, back.func_mask);
    scene.write(tox2::kMethodNv4097SetBackStencilMask, back.write_mask);
    scene.write(tox2::kMethodNv4097SetBackStencilOpFail, back.op_fail);
    scene.write(tox2::kMethodNv4097SetBackStencilOpZfail, back.op_zfail);
    scene.write(tox2::kMethodNv4097SetBackStencilOpZpass, back.op_zpass);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.putQuadPositions();
    scene.drawStrip(4);
    const std::vector<std::uint8_t> file = scene.finish();
    // Recording runs a frame builder of its own, which decodes the same draw.
    tox2::rsxUnimplementedReset();
    for (const tox2::RsxCommand& command : replaySynthetic(file)) {
        if (const auto* draw = std::get_if<tox2::RsxDraw>(&command)) return *draw;
    }
    CHECK(false);  // no draw was built
    return {};
}

// Stencil registers reach RsxDraw::raster: the back face follows the front one while two-sided stencil is off,
// raw op words are kept, and words outside the GCM sets are counted only while the test (and the face) is in use.
void testStencilDecode() {
    constexpr std::uint32_t kReplace = 0x1E01, kIncr = 0x1E02, kDecr = 0x1E03, kKeep = 0x1E00, kInvertOp = 0x150A;
    // Reference and masks carry junk above bit 7 to check that only the low byte is used.
    const StencilFaceWords front = {0x204, 0x1A5, 0x30F, 0x2F0, kReplace, kIncr, kDecr};
    const StencilFaceWords back = {0x201, 0x109, 0x1FF, 0x10F, kInvertOp, kKeep, 0x8507};

    // (a) two-sided off: the back registers are ignored and the back face equals the front one.
    tox2::RsxDraw draw = replayStencilDraw(true, false, front, back);
    CHECK(draw.raster.stencil_test && !draw.raster.two_sided_stencil);
    CHECK(sameFace(draw.raster.stencil_front, front));
    CHECK(sameFace(draw.raster.stencil_back, front));
    CHECK(draw.raster.stencil_front.ref == 0xA5 && draw.raster.stencil_front.func_mask == 0x0F &&
          draw.raster.stencil_front.write_mask == 0xF0);
    CHECK(noStencilWordNotes());

    // (b) two-sided on: the back face comes from the BACK registers.
    draw = replayStencilDraw(true, true, front, back);
    CHECK(draw.raster.two_sided_stencil);
    CHECK(sameFace(draw.raster.stencil_front, front));
    CHECK(sameFace(draw.raster.stencil_back, back));
    CHECK(noStencilWordNotes());

    // The test flag alone is decoded; the registers are kept with the test off.
    draw = replayStencilDraw(false, false, front, back);
    CHECK(!draw.raster.stencil_test && sameFace(draw.raster.stencil_front, front));
    CHECK(noStencilWordNotes());

    // (c) unknown op words are counted with the test on; the word is compared in full, not truncated to 16 bits.
    StencilFaceWords bad = front;
    bad.op_zpass = 0x1234;
    draw = replayStencilDraw(true, false, bad, back);
    CHECK(draw.raster.stencil_front.op_zpass == 0x1234);
    CHECK(unimplementedCount("state stencil op ?0x1234") == 1);
    bad.op_zpass = kIncr | 0x10000;
    replayStencilDraw(true, false, bad, back);
    CHECK(unimplementedCount("state stencil op ?0x11e02") == 1);
    CHECK(unimplementedCount("state stencil op ?0x1234") == 0);

    // (d) the same words with the test off are not counted.
    bad.op_zpass = 0x1234;
    replayStencilDraw(false, false, bad, back);
    CHECK(noStencilWordNotes());
    bad.op_zpass = kIncr | 0x10000;
    replayStencilDraw(false, false, bad, back);
    CHECK(noStencilWordNotes());

    // (e) compare words: 0x000..0x007 are accepted like 0x200..0x207, 0x208 and words with other bits are not.
    StencilFaceWords func = front;
    func.func = 0x003;
    draw = replayStencilDraw(true, false, func, back);
    CHECK(draw.raster.stencil_front.func == 0x003);
    CHECK(noStencilWordNotes());
    func.func = 0x207;
    replayStencilDraw(true, false, func, back);
    CHECK(noStencilWordNotes());
    func.func = 0x208;
    replayStencilDraw(true, false, func, back);
    CHECK(unimplementedCount("state stencil func ?0x208") == 1);
    func.func = 0x10201;
    replayStencilDraw(true, false, func, back);
    CHECK(unimplementedCount("state stencil func ?0x10201") == 1);
    func.func = 0x208;
    replayStencilDraw(false, false, func, back);  // test off: no further note
    CHECK(unimplementedCount("state stencil func ?0x208") == 0);

    // (f) a bad back word is counted only while two-sided stencil is on.
    bad = back;
    bad.op_fail = 0x4321;
    bad.func = 0x209;
    replayStencilDraw(true, false, front, bad);
    CHECK(noStencilWordNotes());
    draw = replayStencilDraw(true, true, front, bad);
    CHECK(draw.raster.stencil_back.op_fail == 0x4321);
    CHECK(unimplementedCount("state stencil op ?0x4321") == 1);
    CHECK(unimplementedCount("state stencil func ?0x209") == 1);
    tox2::rsxUnimplementedReset();
}

// One draw with SET_USER_CLIP_PLANE_CONTROL written as `control`; the replay's notes are the only ones counted.
tox2::RsxDraw replayUserClipDraw(std::uint32_t control) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetUserClipPlaneControl, control);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.putQuadPositions();
    scene.drawStrip(4);
    const std::vector<std::uint8_t> file = scene.finish();
    tox2::rsxUnimplementedReset();
    for (const tox2::RsxCommand& command : replaySynthetic(file)) {
        if (const auto* draw = std::get_if<tox2::RsxDraw>(&command)) return *draw;
    }
    CHECK(false);  // no draw was built
    return {};
}

// True when the last replay counted no user clip plane word.
bool noUserClipNotes() {
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        if (name.starts_with("state user clip plane")) return false;
    }
    return true;
}

// The control word reaches RsxDraw::raster raw; a nibble above 2 is counted once per plane.
void testUserClipDecode() {
    CHECK(replayUserClipDraw(0).raster.user_clip == 0);
    CHECK(noUserClipNotes());
    // Planes 0..5 set to 1, 2, 1, 2, 1, 2.
    CHECK(replayUserClipDraw(0x212121).raster.user_clip == 0x212121);
    CHECK(noUserClipNotes());
    // Plane 4 with the nibble 3.
    CHECK(replayUserClipDraw(0x030000).raster.user_clip == 0x030000);
    CHECK(unimplementedCount("state user clip plane op ?0x3") == 1);
    // Bits above plane 5 are not planes.
    replayUserClipDraw(0xF0000000);
    CHECK(noUserClipNotes());
    // Two bad planes with different nibbles are counted separately.
    replayUserClipDraw(0x0000F0A0);
    CHECK(unimplementedCount("state user clip plane op ?0xa") == 1);
    CHECK(unimplementedCount("state user clip plane op ?0xf") == 1);
    tox2::rsxUnimplementedReset();
}

// A raster state with the stencil test on and the depth test and write off; `front` and `back` give the write
// mask and the ZPASS op (REPLACE or KEEP, the other ops stay KEEP) of each face.
struct ZetaFace {
    std::uint8_t write_mask;
    bool writing_op;
};

tox2::RsxRasterState zetaRaster(bool two_sided, const ZetaFace& front, const ZetaFace& back) {
    constexpr std::uint32_t kReplace = 0x1E01, kKeep = 0x1E00;
    tox2::RsxRasterState raster;
    raster.stencil_test = true;
    raster.two_sided_stencil = two_sided;
    raster.stencil_front.write_mask = front.write_mask;
    raster.stencil_front.op_zpass = front.writing_op ? kReplace : kKeep;
    // Mirrors the decoder: the back face equals the front one while two-sided stencil is off.
    raster.stencil_back = raster.stencil_front;
    if (two_sided) {
        raster.stencil_back.write_mask = back.write_mask;
        raster.stencil_back.op_zpass = back.writing_op ? kReplace : kKeep;
    }
    return raster;
}

// A depth-test-off draw with the stencil state `raster` and stencil only (no depth) writes, followed by a draw
// that reads the depth surface as an A8R8G8B8 texture. True when the second draw sees the depth surface as a
// render target alias.
bool stencilPassMakesDepthKnown(std::uint32_t op_zpass) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    // Colour-only clear: the depth surface must not become known any other way.
    scene.write(tox2::kMethodNv4097SetColorClearValue, 0xFF000000);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    scene.putQuadPositions();
    scene.putQuadColors();
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
    scene.write(tox2::kMethodNv4097SetDepthMask, 0);
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, 1);
    scene.write(tox2::kMethodNv4097SetStencilMask, 0xFF);
    scene.write(tox2::kMethodNv4097SetStencilOpZpass, op_zpass);
    scene.drawStrip(4);

    // The reading draw does not use the stencil, or it would register the depth surface itself.
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, 0);
    setSurfaceTextureState(scene, kDepthOffset, 0x85);
    putSurfaceQuad(scene, 16, 32);
    scene.drawStrip(4);
    const std::vector<std::uint8_t> file = scene.finish();
    tox2::rsxUnimplementedReset();
    int draws = 0;
    bool known = false;
    for (const tox2::RsxCommand& command : replaySynthetic(file)) {
        const auto* draw = std::get_if<tox2::RsxDraw>(&command);
        if (draw == nullptr || ++draws != 2) continue;
        CHECK(draw->textures.size() == 1);
        known = !draw->textures.empty() && draw->textures[0].rt_alias >= 0 && draw->textures[0].rt_depth;
    }
    CHECK(draws == 2);
    return known;
}

// The zeta write rule: a draw registers the depth surface and rebuilds its A8R8G8B8 copy when the depth or the
// stencil can write it.
void testZetaWriteRule() {
    using tox2::RsxDepthFormat;
    using tox2::RsxRasterState;
    constexpr ZetaFace kWrite = {0xFF, true}, kKeepOnly = {0xFF, false}, kNoMask = {0x00, true};
    constexpr ZetaFace kNoMaskKeep = {0x00, false};
    const auto zeta = [](const RsxRasterState& raster, RsxDepthFormat format = RsxDepthFormat::Z24S8) {
        return tox2::rsxZetaWriteEnabled(raster, format);
    };

    CHECK(tox2::rsxStencilOpWrites(0x1E01) && tox2::rsxStencilOpWrites(0x0000) && tox2::rsxStencilOpWrites(0x1E02) &&
          tox2::rsxStencilOpWrites(0x1E03) && tox2::rsxStencilOpWrites(0x150A) && tox2::rsxStencilOpWrites(0x8507) &&
          tox2::rsxStencilOpWrites(0x8508));
    CHECK(!tox2::rsxStencilOpWrites(0x1E00) && !tox2::rsxStencilOpWrites(0x1234) && !tox2::rsxStencilOpWrites(0x11E01));

    // 1. depth test and depth write, stencil off.
    RsxRasterState depth_only;
    depth_only.depth_test = true;
    depth_only.depth_write = true;
    CHECK(zeta(depth_only));
    depth_only.depth_write = false;
    CHECK(!zeta(depth_only));

    // 2. 3. 4. one face.
    CHECK(zeta(zetaRaster(false, kWrite, kWrite)));
    CHECK(!zeta(zetaRaster(false, kKeepOnly, kKeepOnly)));
    CHECK(!zeta(zetaRaster(false, kNoMask, kNoMask)));

    // 5. an unknown op word counts as KEEP.
    RsxRasterState unknown = zetaRaster(false, kWrite, kWrite);
    unknown.stencil_front.op_zpass = 0x1234;
    unknown.stencil_back = unknown.stencil_front;
    CHECK(!zeta(unknown));
    unknown.stencil_front.op_fail = 0x1E03;  // a real op in another slot writes
    CHECK(zeta(unknown));

    // 6. front mask 0, back mask 0xFF with a back REPLACE: the back face counts only with two-sided stencil on.
    CHECK(!zeta(zetaRaster(false, kNoMask, kWrite)));
    CHECK(zeta(zetaRaster(true, kNoMask, kWrite)));

    // 7. masks and ops are combined separately: a front mask with KEEP ops plus a back op with mask 0.
    CHECK(zeta(zetaRaster(true, kKeepOnly, kNoMask)));
    CHECK(!zeta(zetaRaster(false, kKeepOnly, kNoMask)));

    // 8. front REPLACE with mask 0, back mask 0xFF with KEEP ops.
    CHECK(zeta(zetaRaster(true, kNoMask, kKeepOnly)));

    // Neither face has a mask or an op: nothing writes even with two-sided stencil on.
    CHECK(!zeta(zetaRaster(true, kNoMaskKeep, kNoMaskKeep)));
    // A front face that can write is not combined with the back one (same result either way here).
    CHECK(zeta(zetaRaster(true, kWrite, kNoMaskKeep)));

    // 9. Z16 and no depth surface have no stencil; the depth write still counts.
    RsxRasterState z16 = zetaRaster(false, kWrite, kWrite);
    CHECK(!zeta(z16, RsxDepthFormat::Z16));
    CHECK(!zeta(z16, RsxDepthFormat::None));
    z16.depth_test = true;
    z16.depth_write = true;
    CHECK(zeta(z16, RsxDepthFormat::Z16));

    // 10. a stencil that is off writes nothing whatever its masks and ops are.
    RsxRasterState off = zetaRaster(true, kWrite, kWrite);
    off.stencil_test = false;
    CHECK(!zeta(off));

    // A stencil-only REPLACE pass makes the depth surface known to a later texture read; with KEEP ops it does not.
    CHECK(stencilPassMakesDepthKnown(0x1E01));
    CHECK(!stencilPassMakesDepthKnown(0x1E00));
    tox2::rsxUnimplementedReset();
}

// ---- Stencil test in the renderer ----

constexpr std::uint32_t kOpKeep = 0x1E00, kOpZero = 0x0000, kOpReplace = 0x1E01, kOpIncr = 0x1E02, kOpDecr = 0x1E03;
constexpr std::uint32_t kOpInvert = 0x150A, kOpIncrWrap = 0x8507, kOpDecrWrap = 0x8508;
constexpr std::uint32_t kCmpNever = 0x200, kCmpEqual = 0x202, kCmpGreater = 0x204, kCmpAlways = 0x207;
constexpr std::uint32_t kQuadRed = 0xFF400000;  // what putSurfaceQuad draws
constexpr std::uint32_t kBlack = 0xFF000000;

StencilFaceWords stencilFace(std::uint32_t func, std::uint32_t ref, std::uint32_t op_zpass,
                             std::uint32_t write_mask = 0xFF, std::uint32_t func_mask = 0xFF) {
    return {func, ref, func_mask, write_mask, kOpKeep, kOpKeep, op_zpass};
}

void writeStencilFace(tox2::rsx_synthetic::SceneBuilder& scene, bool back, const StencilFaceWords& face) {
    using namespace tox2;
    scene.write(back ? kMethodNv4097SetBackStencilFunc : kMethodNv4097SetStencilFunc, face.func);
    scene.write(back ? kMethodNv4097SetBackStencilFuncRef : kMethodNv4097SetStencilFuncRef, face.ref);
    scene.write(back ? kMethodNv4097SetBackStencilFuncMask : kMethodNv4097SetStencilFuncMask, face.func_mask);
    scene.write(back ? kMethodNv4097SetBackStencilMask : kMethodNv4097SetStencilMask, face.write_mask);
    scene.write(back ? kMethodNv4097SetBackStencilOpFail : kMethodNv4097SetStencilOpFail, face.op_fail);
    scene.write(back ? kMethodNv4097SetBackStencilOpZfail : kMethodNv4097SetStencilOpZfail, face.op_zfail);
    scene.write(back ? kMethodNv4097SetBackStencilOpZpass : kMethodNv4097SetStencilOpZpass, face.op_zpass);
}

// Black color, depth 1.0 and the given stencil, the quad programs, and depth test and write off. depth_code is
// the SET_SURFACE_FORMAT depth field: 2 Z24S8, 1 Z16, 0 none.
void beginStencilScene(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t stencil, std::uint32_t depth_code = 2) {
    using namespace tox2::rsx_synthetic;
    scene.setSurface();
    if (depth_code != 2) scene.write(tox2::kMethodNv4097SetSurfaceFormat, 8u | (depth_code << 5) | (1u << 8));
    scene.write(tox2::kMethodNv4097SetColorClearValue, kBlack);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, 0xFFFFFF00u | stencil);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF3);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
    scene.write(tox2::kMethodNv4097SetDepthMask, 0);
}

// One draw of the quad over pixels 16..48, with the stencil test on and the given front face. The back face
// follows it unless the caller wrote the BACK registers and `two_sided`.
void stencilQuad(tox2::rsx_synthetic::SceneBuilder& scene, const StencilFaceWords& front, bool color_writes = true,
                 bool two_sided = false) {
    writeStencilFace(scene, false, front);
    scene.write(tox2::kMethodNv4097SetTwoSidedStencilTestEnable, two_sided ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, 1);
    scene.write(tox2::kMethodNv4097SetColorMask, color_writes ? 0x01010101u : 0u);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
}

struct StencilRender {
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0, height = 0;
    tox2::RendererStats stats;
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const { return pixelArgb(bgra, width, x, y); }
};

// A fresh renderer per scene so the statistics belong to it; the census holds the renderer's notes afterwards.
StencilRender renderStencilScene(tox2::VkContext& ctx, tox2::rsx_synthetic::SceneBuilder& scene) {
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    std::vector<tox2::RsxCommand> commands = replaySynthetic(scene.finish());
    tox2::rsxUnimplementedReset();
    renderer.execute(commands);
    CHECK(renderer.valid());
    StencilRender result;
    CHECK(renderer.readDisplay(result.bgra, result.width, result.height));
    CHECK(result.width == 64 && result.height == 64 && result.bgra.size() == 64 * 64 * 4);
    if (result.bgra.size() != 64 * 64 * 4) result.bgra.assign(64 * 64 * 4, 0);
    result.stats = renderer.stats();
    return result;
}

void expectNoRendererNotes() {
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
}

// The quad is drawn (or not) at the middle of the target, the corner stays black, and nothing was skipped.
void expectStencilDrawn(const StencilRender& render, bool drawn) {
    CHECK(render.pixel(32, 32) == (drawn ? kQuadRed : kBlack));
    CHECK(render.pixel(2, 2) == kBlack);
    CHECK(render.stats.draws_skipped == 0);
}

// Stencil bits read back through a depth-as-texture quad: the stencil is cleared to 0x78, a quad reads the
// depth surface as an A8R8G8B8 texture, a stencil-only REPLACE pass writes 0x9A (depth test off, color mask 0),
// and a second quad reads it again. The first quad shows the cleared stencil in its B channel, the second the
// new one, and the surface is converted twice.
void testStencilWriteReachesDepthTexture(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetColorClearValue, kBlack);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, (kDepthFirst << 8) | kStencilFirst);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF3);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    setSurfaceTextureState(scene, kDepthOffset, 0x85);
    putSurfaceQuad(scene, 16, 32);
    scene.drawStrip(4);

    setQuadRenderState(scene);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
    scene.write(tox2::kMethodNv4097SetDepthMask, 0);
    stencilQuad(scene, stencilFace(kCmpAlways, 0x9A, kOpReplace), false);

    scene.write(tox2::kMethodNv4097SetStencilTestEnable, 0);
    scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101);
    setSurfaceTextureState(scene, kDepthOffset, 0x85);
    putSurfaceQuad(scene, 32, 48);
    scene.drawStrip(4);

    const StencilRender render = renderStencilScene(ctx, scene);
    CHECK(depthWordNear(render.pixel(20, 32), kDepthFirst, kStencilFirst));
    CHECK(depthWordNear(render.pixel(40, 32), kDepthFirst, 0x9A));
    CHECK(render.stats.depth_conversions == 2);
    CHECK(render.stats.draws == 3 && render.stats.draws_skipped == 0);
    expectNoRendererNotes();
}

// The stencil test, its ops, masks, reference and two-sided mode on a Z24S8 target (D24S8 or D32S8 by device),
// and the draw without a stencil aspect.
void testRendererStencil(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    // 1. The reference is compared with the cleared stencil.
    for (const std::uint32_t ref : {5u, 6u}) {
        SceneBuilder scene;
        beginStencilScene(scene, 0x05);
        stencilQuad(scene, stencilFace(kCmpEqual, ref, kOpKeep));
        const StencilRender render = renderStencilScene(ctx, scene);
        expectStencilDrawn(render, ref == 5);
        expectNoRendererNotes();
    }

    // 2. A colour-less pass writes the stencil over x 16..32, a full-width pass draws where it reads 7.
    {
        SceneBuilder scene;
        beginStencilScene(scene, 0x00);
        writeStencilFace(scene, false, stencilFace(kCmpAlways, 7, kOpReplace));
        scene.write(tox2::kMethodNv4097SetStencilTestEnable, 1);
        scene.write(tox2::kMethodNv4097SetColorMask, 0);
        putSurfaceQuad(scene, 16, 32);
        scene.drawStrip(4);
        stencilQuad(scene, stencilFace(kCmpEqual, 7, kOpKeep, 0x00));
        const StencilRender render = renderStencilScene(ctx, scene);
        CHECK(render.pixel(24, 32) == kQuadRed);
        CHECK(render.pixel(40, 32) == kBlack);
        CHECK(render.pixel(2, 2) == kBlack);
        CHECK(render.stats.draws == 2 && render.stats.draws_skipped == 0);
        expectNoRendererNotes();
    }

    // 3. The compare mask applies to both sides: 0x5A & 0xF0 equals 0x50 & 0xF0, but not without the mask.
    for (const std::uint32_t func_mask : {0xF0u, 0xFFu}) {
        SceneBuilder scene;
        beginStencilScene(scene, 0x5A);
        stencilQuad(scene, stencilFace(kCmpEqual, 0x50, kOpKeep, 0xFF, func_mask));
        expectStencilDrawn(renderStencilScene(ctx, scene), func_mask == 0xF0);
        expectNoRendererNotes();
    }
    // The write mask keeps the other bits: REPLACE of 0xFF under mask 0x0F turns 0x50 into 0x5F.
    for (const std::uint32_t check_ref : {0x5Fu, 0x50u, 0xFFu}) {
        SceneBuilder scene;
        beginStencilScene(scene, 0x50);
        stencilQuad(scene, stencilFace(kCmpAlways, 0xFF, kOpReplace, 0x0F), false);
        stencilQuad(scene, stencilFace(kCmpEqual, check_ref, kOpKeep, 0x00));
        expectStencilDrawn(renderStencilScene(ctx, scene), check_ref == 0x5F);
        expectNoRendererNotes();
    }

    // Ops of the three outcomes (stencil fail, depth fail, pass): each case runs one colour-less pass over a
    // cleared stencil and checks the result with an EQUAL draw. A failing depth test is GREATER against the
    // cleared depth 1.0 (the quad lies at 0.5).
    struct OpCase {
        const char* name;
        std::uint32_t initial;
        StencilFaceWords face;
        bool depth_fails;
        std::uint32_t expected;
    };
    StencilFaceWords fail_face = stencilFace(kCmpNever, 0x42, kOpZero);
    fail_face.op_fail = kOpReplace;
    StencilFaceWords zfail_face = stencilFace(kCmpAlways, 0x42, kOpZero);
    zfail_face.op_zfail = kOpReplace;
    const OpCase kOpCases[] = {
            {"KEEP", 0x35, stencilFace(kCmpAlways, 0x42, kOpKeep), false, 0x35},
            {"ZERO", 0x35, stencilFace(kCmpAlways, 0x42, kOpZero), false, 0x00},
            {"REPLACE", 0x35, stencilFace(kCmpAlways, 0x42, kOpReplace), false, 0x42},
            {"INCR", 0x35, stencilFace(kCmpAlways, 0x42, kOpIncr), false, 0x36},
            {"INCR clamps", 0xFF, stencilFace(kCmpAlways, 0x42, kOpIncr), false, 0xFF},
            {"DECR", 0x35, stencilFace(kCmpAlways, 0x42, kOpDecr), false, 0x34},
            {"DECR clamps", 0x00, stencilFace(kCmpAlways, 0x42, kOpDecr), false, 0x00},
            {"INVERT", 0x35, stencilFace(kCmpAlways, 0x42, kOpInvert), false, 0xCA},
            {"INCR_WRAP", 0xFF, stencilFace(kCmpAlways, 0x42, kOpIncrWrap), false, 0x00},
            {"DECR_WRAP", 0x00, stencilFace(kCmpAlways, 0x42, kOpDecrWrap), false, 0xFF},
            {"unknown op is KEEP", 0x35, stencilFace(kCmpAlways, 0x42, 0x1234), false, 0x35},
            {"OP_FAIL", 0x35, fail_face, false, 0x42},
            {"OP_ZFAIL", 0x35, zfail_face, true, 0x42},
            {"OP_ZPASS when the depth test passes", 0x35, zfail_face, false, 0x00},
    };
    for (const OpCase& test : kOpCases) {
        SceneBuilder scene;
        beginStencilScene(scene, test.initial);
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, test.depth_fails ? 1 : 0);
        scene.write(tox2::kMethodNv4097SetDepthFunc, kCmpGreater);
        stencilQuad(scene, test.face, false);
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
        stencilQuad(scene, stencilFace(kCmpEqual, test.expected, kOpKeep, 0x00));
        const StencilRender render = renderStencilScene(ctx, scene);
        if (render.pixel(32, 32) != kQuadRed) std::fprintf(stderr, "stencil op case failed: %s\n", test.name);
        expectStencilDrawn(render, true);
        expectNoRendererNotes();
    }

    // 4. Two-sided: the quad's faces are back faces with front face 0x901 and front faces with 0x900. The front
    // registers say NEVER and the back ones ALWAYS; with two-sided stencil off the back face uses the front ones.
    for (const std::uint32_t front_face : {0x901u, 0x900u}) {
        for (const bool two_sided : {true, false}) {
            SceneBuilder scene;
            beginStencilScene(scene, 0x00);
            scene.write(tox2::kMethodNv4097SetFrontFace, front_face);
            writeStencilFace(scene, true, stencilFace(kCmpAlways, 0, kOpKeep));
            stencilQuad(scene, stencilFace(kCmpNever, 0, kOpKeep), true, two_sided);
            const bool back_face = front_face == 0x901;
            expectStencilDrawn(renderStencilScene(ctx, scene), back_face && two_sided);
            expectNoRendererNotes();
        }
    }

    // 5. A stencil-only write is visible when the depth surface is read as a texture.
    testStencilWriteReachesDepthTexture(ctx);

    // 6. Z16 and no depth surface have no stencil: the draw is drawn without the test and counted once.
    for (const std::uint32_t depth_code : {1u, 0u}) {
        SceneBuilder scene;
        beginStencilScene(scene, 0x00, depth_code);
        stencilQuad(scene, stencilFace(kCmpNever, 0, kOpKeep));
        expectStencilDrawn(renderStencilScene(ctx, scene), true);
        CHECK(unimplementedCount("state stencil test without a stencil surface") == 1);
        CHECK(tox2::rsxUnimplementedSnapshot().size() == 1);
    }
    tox2::rsxUnimplementedReset();
}

// The quad programs plus MOV o[5].y = v0.x (the model-space x, -0.5 on the left half and +0.5 on the right),
// and a second write of the same value to o[6].y when `second_slot` is set. The colour move keeps the quad
// distinguishable from the clear colour.
tox2::rsx_synthetic::Programs userClipPrograms(bool second_slot) {
    tox2::rsx_synthetic::Programs programs = quadPrograms();
    programs.vertex.clear();
    std::vector<std::array<std::uint32_t, 4>> instructions = {vpMove(0, 0, "xyzw", false), vpMove(1, 3, "yzwx", false)};
    if (second_slot) instructions.push_back(vpMove(6, 0, "xxxx", false, 0x4));
    instructions.push_back(vpMove(5, 0, "xxxx", true, 0x4));
    for (const auto& instruction : instructions) {
        programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
    }
    return programs;
}

// User clip planes: the program writes a distance per plane and the control word and the output mask decide
// which planes cut the quad (pixels 16..48, red where drawn).
void testRendererUserClip(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    const bool clipping = ctx.hasClipDistance();
    if (!clipping) std::printf("user clip: device has no shaderClipDistance, checking the unclipped path\n");

    struct ClipCase {
        const char* name;
        std::uint32_t output_mask;
        std::uint32_t control;
        bool second_slot;
        bool left_drawn;   // pixel (24, 32), model x < 0
        bool right_drawn;  // pixel (40, 32), model x >= 0
    };
    constexpr std::uint32_t kMaskPlane0 = 0x41, kMaskPlane2 = 0x101, kMaskPlane0And3 = 0x241;
    const ClipCase kCases[] = {
            {"plane 0 off", kMaskPlane0, 0x0, false, true, true},
            {"plane 0 greater or equal", kMaskPlane0, 0x2, false, false, true},
            {"plane 0 less than", kMaskPlane0, 0x1, false, true, false},
            // Bit 6 of the output mask is clear: the plane is off whatever the control word says.
            {"plane 0 without the mask bit", 0x1, 0x2, false, true, true},
            // Plane 2 reads the unwritten .w lane of o[5], which holds 1.
            {"plane 2 greater or equal on .w", kMaskPlane2, 0x200, false, true, true},
            {"plane 2 less than on .w", kMaskPlane2, 0x100, false, false, false},
            // The program writes only o[5] here: plane 3 has no source and keeps everything.
            {"plane 3 without a source slot", kMaskPlane0And3, 0x1002, false, false, true},
            // With o[6] written, plane 3 reads o[6].y = x: less than keeps the left half.
            {"plane 3 with a source slot", kMaskPlane0And3, 0x1000, true, true, false},
            {"planes 0 and 3 together", kMaskPlane0And3, 0x1002, true, false, false},
    };
    for (const ClipCase& test : kCases) {
        SceneBuilder scene;
        scene.setSurface();
        scene.write(tox2::kMethodNv4097SetColorClearValue, kBlack);
        scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
        setQuadRenderState(scene);
        scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, test.output_mask);
        scene.write(tox2::kMethodNv4097SetUserClipPlaneControl, test.control);
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        scene.setColorAttrib(3, kColorsOffset);
        scene.setPrograms(userClipPrograms(test.second_slot));
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
        scene.write(tox2::kMethodNv4097SetDepthMask, 0);
        scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
        putSurfaceQuad(scene, 16, 48);
        scene.drawStrip(4);
        const StencilRender render = renderStencilScene(ctx, scene);

        // Without the device feature nothing is clipped; the planes the control word enables are counted.
        const bool left = clipping ? test.left_drawn : true, right = clipping ? test.right_drawn : true;
        if (render.pixel(24, 32) != (left ? kQuadRed : kBlack) || render.pixel(40, 32) != (right ? kQuadRed : kBlack)) {
            std::fprintf(stderr, "user clip case failed: %s\n", test.name);
        }
        CHECK(render.pixel(24, 32) == (left ? kQuadRed : kBlack));
        CHECK(render.pixel(40, 32) == (right ? kQuadRed : kBlack));
        CHECK(render.pixel(2, 2) == kBlack);
        CHECK(render.stats.draws == 1 && render.stats.draws_skipped == 0);
        if (clipping) {
            expectNoRendererNotes();
        } else {
            std::uint32_t enabled = 0;
            for (std::uint32_t plane = 0; plane < 6; plane++) {
                const std::uint32_t op = (test.control >> (4 * plane)) & 0xFu;
                if ((op == 1 || op == 2) && (test.output_mask & (1u << (6 + plane))) != 0) enabled = 1;
            }
            CHECK(unimplementedCount("state user clip unavailable") == enabled);
            CHECK(tox2::rsxUnimplementedSnapshot().size() == enabled);
        }
    }
    tox2::rsxUnimplementedReset();
}

bool sameRect(const tox2::PixelRect& rect, std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

void testLetterbox() {
    CHECK(sameRect(tox2::letterboxRect(1280, 720, 16, 9), 0, 0, 1280, 720));
    CHECK(sameRect(tox2::letterboxRect(1920, 1080, 16, 9), 0, 0, 1920, 1080));
    // Wider than 16:9: pillarbox bars left and right.
    CHECK(sameRect(tox2::letterboxRect(1600, 720, 16, 9), 160, 0, 1280, 720));
    // Taller than 16:9: letterbox bars above and below.
    CHECK(sameRect(tox2::letterboxRect(1280, 1024, 16, 9), 0, 152, 1280, 720));
    CHECK(sameRect(tox2::letterboxRect(800, 800, 16, 9), 0, 175, 800, 450));
    // Odd sizes round down; the margins differ by at most one pixel.
    CHECK(sameRect(tox2::letterboxRect(641, 361, 16, 9), 0, 0, 641, 360));
    // Empty areas and ratios give an empty rectangle.
    CHECK(sameRect(tox2::letterboxRect(0, 720, 16, 9), 0, 0, 0, 0));
    CHECK(sameRect(tox2::letterboxRect(1280, 0, 16, 9), 0, 0, 0, 0));
    CHECK(sameRect(tox2::letterboxRect(1280, 720, 0, 9), 0, 0, 0, 0));
    CHECK(sameRect(tox2::letterboxRect(1, 1, 16, 9), 0, 0, 0, 0));  // 1 x 0 after rounding
    // A very large area does not overflow.
    CHECK(sameRect(tox2::letterboxRect(0xFFFFFFFFu, 0xFFFFFFFFu, 16, 9), 0, 939524096, 0xFFFFFFFFu, 2415919103u));
}

// lParam of a key message: repeat count, scan code (bits 16..23), extended (24), previous state (30).
std::uint64_t keyLparam(std::uint32_t scan, bool extended, bool was_down) {
    return 1u | (static_cast<std::uint64_t>(scan) << 16) | (extended ? 1u << 24 : 0u) | (was_down ? 1u << 30 : 0u);
}

void testKeyMessages() {
    constexpr std::uint32_t kKeyDown = 0x100, kKeyUp = 0x101, kChar = 0x102, kSysKeyDown = 0x104, kSysKeyUp = 0x105;
    constexpr std::uint32_t kSize = 0x5, kKillFocus = 0x8;
    constexpr std::uint64_t kVkZ = 0x5A, kVkUp = 0x26, kVkV = 0x56;

    auto key = tox2::keyMessageFromWin32(kKeyDown, kVkZ, keyLparam(0x2C, false, false));
    CHECK(key && key->scan_code == 0x2C && key->down && !key->repeat);

    key = tox2::keyMessageFromWin32(kKeyDown, kVkZ, keyLparam(0x2C, false, true));
    CHECK(key && key->scan_code == 0x2C && key->down && key->repeat);

    key = tox2::keyMessageFromWin32(kKeyDown, kVkUp, keyLparam(0x48, true, false));
    CHECK(key && key->scan_code == 0xE048 && key->down);
    // The same scan code without the extended bit is numpad 8.
    key = tox2::keyMessageFromWin32(kKeyDown, kVkUp, keyLparam(0x48, false, false));
    CHECK(key && key->scan_code == 0x48);

    // A key-up always carries the previous-state bit; it is not a repeat.
    key = tox2::keyMessageFromWin32(kKeyUp, kVkZ, keyLparam(0x2C, false, true) | (1ull << 31));
    CHECK(key && key->scan_code == 0x2C && !key->down && !key->repeat);

    // Alt held: bit 29 is set and the message is the SYS variant.
    key = tox2::keyMessageFromWin32(kSysKeyDown, kVkV, keyLparam(0x2F, false, false) | (1ull << 29));
    CHECK(key && key->scan_code == 0x2F && key->down && !key->repeat);
    key = tox2::keyMessageFromWin32(kSysKeyUp, kVkV, keyLparam(0x2F, false, true) | (1ull << 29) | (1ull << 31));
    CHECK(key && key->scan_code == 0x2F && !key->down);

    // A zero scan code falls back to the virtual key; the extended bit still tells the arrow from numpad 8.
    // (Arrow keys only: letter keys map through the active layout.)
    key = tox2::keyMessageFromWin32(kKeyDown, kVkUp, keyLparam(0, true, false));
    CHECK(key && key->scan_code == 0xE048 && key->down);
    key = tox2::keyMessageFromWin32(kKeyDown, kVkUp, keyLparam(0, false, false));
    CHECK(key && key->scan_code == 0x48);

    CHECK(!tox2::keyMessageFromWin32(kChar, kVkZ, keyLparam(0x2C, false, false)));
    CHECK(!tox2::keyMessageFromWin32(kSize, 0, 0x02D00500));
    CHECK(!tox2::keyMessageFromWin32(kKillFocus, 0, 0));
}

}  // namespace

int main() {
    const bool layer = tox2::vkValidationLayerAvailable();
    std::printf("validation layer: %s\n", layer ? "installed" : "not installed");
    testLetterbox();
    testKeyMessages();
    testShaderCompiler();
    testTextureMapping();
    testFrameDump();
    testPartialStencilNote();
    testStencilDecode();
    testUserClipDecode();
    testZetaWriteRule();
    testVertexConvert();

    tox2::VkContextOptions options;
    options.validation = true;
    std::string error;
    std::unique_ptr<tox2::VkContext> ctx = tox2::createVkContext(options, error);
    if (!ctx) {
        std::printf("vulkan: %s\n", error.c_str());
        std::printf("render tests: skipped: no Vulkan device\n");
        return g_failed != 0 ? 1 : 0;
    }

    const std::uint32_t api = ctx->deviceProperties().apiVersion;
    std::printf("device: %s (Vulkan %u.%u.%u), D24S8 %s, clip distance %s, validation %s\n", ctx->deviceName().data(),
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                ctx->hasD24S8() ? "supported" : "unsupported (D32S8 fallback)",
                ctx->hasClipDistance() ? "supported" : "unsupported", ctx->validationEnabled() ? "active" : "inactive");
    CHECK(layer == ctx->validationEnabled());

    testClearAndReadback(*ctx);
    testRendererClear(*ctx);
    testRendererReadbackGate(*ctx);
    testRendererScissorAndMask(*ctx);
    testRendererQuad(*ctx);
    testRendererBranchBits(*ctx);
    testRendererTextures(*ctx);
    testRendererVolumeTextures(*ctx);
    testRendererFloatSurface(*ctx);
    testRendererDepthTexture(*ctx);
    testRendererStencil(*ctx);
    testRendererUserClip(*ctx);
    CHECK(ctx->validationErrorCount() == 0);

    ctx.reset();
    std::printf("render tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

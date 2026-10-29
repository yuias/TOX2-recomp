// Self-checking tests for the Vulkan renderer library. They skip when no Vulkan device exists.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "core/log.h"
#include "render/dlaa_math.h"
#include "render/dlaa_pass.h"
#include "render/fg_motion.h"
#include "render/fp_translator.h"
#include "render/frame_dump.h"
#include "render/image_pool.h"
#include "render/object_motion_history.h"
#include "render/render_scale.h"
#include "render/shader_compiler.h"
#include "render/vertex_convert.h"
#include "render/vk_device_choice.h"
#include "render/window_size.h"
#include "render/vk_context.h"
#include "render/vk_pipelines.h"
#include "render/vk_renderer.h"
#include "render/vk_textures.h"
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

    // Box downsampling: a 4x2 image with distinct alpha, so every channel is averaged.
    {
        const std::uint8_t src[4 * 2 * 4] = {
                0,   0,   0,   0,   10, 20, 30, 40, 100, 100, 100, 100, 1, 2, 3, 4,  // row 0
                255, 255, 255, 255, 11, 22, 33, 44, 101, 102, 103, 104, 2, 2, 2, 2,  // row 1
        };
        std::vector<std::uint8_t> out;
        // (2, 2): blue {0,10,255,11} -> 69 (68.75); green 74.25 -> 74; red 79.5 rounds up to 80; alpha 84.75 -> 85.
        CHECK(tox2::downsampleBoxBgra(src, 4, 2, 2, 2, out, error));
        CHECK(out == std::vector<std::uint8_t>({69, 74, 80, 85, 51, 52, 52, 53}));
        // (2, 1): rows stay separate; {0,10} -> 5, {100,1} -> 51 (50.5 rounds up).
        CHECK(tox2::downsampleBoxBgra(src, 4, 2, 2, 1, out, error));
        CHECK(out == std::vector<std::uint8_t>({5, 10, 15, 20, 51, 51, 52, 52, 133, 139, 144, 150, 52, 52, 53, 53}));
        // (1, 1) copies.
        CHECK(tox2::downsampleBoxBgra(src, 4, 2, 1, 1, out, error));
        CHECK(out == std::vector<std::uint8_t>(src, src + sizeof(src)));
        CHECK(!tox2::downsampleBoxBgra(src, 4, 2, 0, 1, out, error));
        CHECK(!tox2::downsampleBoxBgra(src, 4, 2, 1, 0, out, error));
        CHECK(!tox2::downsampleBoxBgra(src, 4, 2, 3, 1, out, error));
        CHECK(!tox2::downsampleBoxBgra(src, 4, 2, 1, 3, out, error));
        CHECK(!tox2::downsampleBoxBgra(std::span<const std::uint8_t>(src, 8), 4, 2, 2, 2, out, error));

        const tox2::RgbImage rgb = tox2::rgbFromBgra(src, 4, 2);
        tox2::RgbImage rgb_out;
        CHECK(tox2::downsampleBox(rgb, 2, 2, rgb_out, error));
        CHECK(rgb_out.width == 2 && rgb_out.height == 1);
        CHECK(rgb_out.rgb == std::vector<std::uint8_t>({80, 74, 69, 52, 52, 51}));
        CHECK(tox2::downsampleBox(rgb, 1, 1, rgb_out, error) && rgb_out.rgb == rgb.rgb);
        CHECK(!tox2::downsampleBox(rgb, 0, 2, rgb_out, error));
        CHECK(!tox2::downsampleBox(rgb, 3, 1, rgb_out, error));
    }

    // Cropping: a 4x3 image whose pixel (x, y) is {10 * y + x, 100 + 10 * y + x, 200 + 10 * y + x}.
    {
        tox2::RgbImage grid;
        grid.width = 4;
        grid.height = 3;
        for (std::uint32_t y = 0; y < 3; y++) {
            for (std::uint32_t x = 0; x < 4; x++) {
                const std::uint8_t base = static_cast<std::uint8_t>(10 * y + x);
                grid.rgb.insert(grid.rgb.end(),
                                {base, static_cast<std::uint8_t>(100 + base), static_cast<std::uint8_t>(200 + base)});
            }
        }
        tox2::RgbImage cropped;
        CHECK(tox2::cropRgb(grid, 1, 1, 2, 2, cropped, error));
        CHECK(cropped.width == 2 && cropped.height == 2);
        CHECK(cropped.rgb == std::vector<std::uint8_t>({11, 111, 211, 12, 112, 212, 21, 121, 221, 22, 122, 222}));
        // The whole image, and the last pixel alone, are inside.
        CHECK(tox2::cropRgb(grid, 0, 0, 4, 3, cropped, error) && cropped.rgb == grid.rgb);
        CHECK(tox2::cropRgb(grid, 3, 2, 1, 1, cropped, error) &&
              cropped.rgb == std::vector<std::uint8_t>({23, 123, 223}));
        // Cropping in place works.
        tox2::RgbImage in_place = grid;
        CHECK(tox2::cropRgb(in_place, 2, 0, 2, 1, in_place, error) &&
              in_place.rgb == std::vector<std::uint8_t>({2, 102, 202, 3, 103, 203}));
        const tox2::RgbImage before = cropped;
        CHECK(!tox2::cropRgb(grid, 0, 0, 0, 2, cropped, error));
        CHECK(!tox2::cropRgb(grid, 0, 0, 2, 0, cropped, error));
        CHECK(!tox2::cropRgb(grid, 3, 0, 2, 1, cropped, error));
        CHECK(!tox2::cropRgb(grid, 0, 2, 1, 2, cropped, error));
        CHECK(!tox2::cropRgb(grid, 0xFFFFFFFFu, 0, 2, 1, cropped, error));
        CHECK(!error.empty() && cropped.rgb == before.rgb);
    }

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

// Needs no Vulkan device.
void testAaModeNames() {
    for (const tox2::AaMode mode : {tox2::AaMode::Supersample, tox2::AaMode::Msaa, tox2::AaMode::Dlaa}) {
        const std::optional<tox2::AaMode> parsed = tox2::parseAaMode(tox2::aaModeName(mode));
        CHECK(parsed.has_value() && *parsed == mode);
    }
    CHECK(tox2::aaModeName(tox2::AaMode::Supersample) == "supersample");
    CHECK(tox2::aaModeName(tox2::AaMode::Msaa) == "msaa");
    CHECK(tox2::aaModeName(tox2::AaMode::Dlaa) == "dlaa");
    CHECK(!tox2::parseAaMode("").has_value());
    CHECK(!tox2::parseAaMode("MSAA2").has_value());
    CHECK(!tox2::parseAaMode("msaa ").has_value());
    CHECK(!tox2::parseAaMode("DLAA").has_value());
    CHECK(!tox2::parseAaMode("dlaa ").has_value());
}

bool nearlyEqual(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

// Needs no Vulkan device.
void testDlaaJitter() {
    constexpr float kX[8] = {0.0f, -0.25f, 0.25f, -0.375f, 0.125f, -0.125f, 0.375f, -0.4375f};
    constexpr float kY[8] = {-1.0f / 6, 1.0f / 6, -7.0f / 18, -1.0f / 18, 5.0f / 18, -5.0f / 18, 1.0f / 18, 7.0f / 18};
    for (std::uint32_t frame = 0; frame < tox2::kDlaaJitterPhases; ++frame) {
        const std::array<float, 2> jitter = tox2::dlaaJitterPixels(frame);
        CHECK(nearlyEqual(jitter[0], kX[frame], 1e-6f));
        CHECK(nearlyEqual(jitter[1], kY[frame], 1e-6f));
    }
    // The sequence repeats, including far past 32 bits.
    for (const std::uint64_t frame : {std::uint64_t{8}, std::uint64_t{13}, std::uint64_t{1} << 40}) {
        const std::array<float, 2> wrapped = tox2::dlaaJitterPixels(frame);
        const std::array<float, 2> base = tox2::dlaaJitterPixels(frame % tox2::kDlaaJitterPhases);
        CHECK(wrapped[0] == base[0] && wrapped[1] == base[1]);
    }
    CHECK(tox2::dlaaJitterPixels(8) == tox2::dlaaJitterPixels(0));
    // Every offset stays inside the pixel.
    for (std::uint32_t frame = 0; frame < tox2::kDlaaJitterPhases; ++frame) {
        const std::array<float, 2> jitter = tox2::dlaaJitterPixels(frame);
        CHECK(jitter[0] >= -0.5f && jitter[0] < 0.5f && jitter[1] >= -0.5f && jitter[1] < 0.5f);
    }
    CHECK(nearlyEqual(tox2::halton(1, 2), 0.5f, 1e-7f));
    CHECK(nearlyEqual(tox2::halton(3, 2), 0.75f, 1e-7f));
    CHECK(nearlyEqual(tox2::halton(4, 3), 4.0f / 9, 1e-6f));
}

// c0..c3 as the 16 vertex-constant words of a draw.
std::array<std::uint32_t, 16> constantWords(const tox2::Mat4& m) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < 16; ++i) words[i] = std::bit_cast<std::uint32_t>(m[i]);
    return words;
}

// Needs no Vulkan device.
void testDlaaPerspective() {
    // A 2D screen-space matrix: x, y scale and offset, no w contribution from x, y or z.
    const tox2::Mat4 ortho = {2.0f / 1280, 0, 0, 0, 0, -2.0f / 720, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    const std::array<std::uint32_t, 16> ortho_words = constantWords(ortho);
    CHECK(!tox2::dlaaPerspective(ortho_words));
    CHECK(!tox2::dlaaPerspective(constantWords(tox2::mat4Identity())));
    CHECK(!tox2::dlaaPerspective(std::span<const std::uint32_t>(ortho_words).first(0)));

    // clip.w = z: c2.w = 1.
    const tox2::Mat4 perspective = {1.2f, 0, 0, 0, 0, 2.1f, 0, 0, 0, 0, 1.01f, 1, 0, 0, -0.1f, 0};
    const std::array<std::uint32_t, 16> perspective_words = constantWords(perspective);
    CHECK(tox2::dlaaPerspective(perspective_words));
    // Any of the three rows that scale an input component into w is enough.
    for (const std::size_t word : {std::size_t{3}, std::size_t{7}, std::size_t{11}}) {
        tox2::Mat4 m = ortho;
        m[word] = 0.5f;
        CHECK(tox2::dlaaPerspective(constantWords(m)));
    }
    // Only c3.w (a constant w) is not a perspective divide.
    tox2::Mat4 constant_w = ortho;
    constant_w[15] = 2.0f;
    CHECK(!tox2::dlaaPerspective(constantWords(constant_w)));
    // Fewer than 16 words never qualifies, even when the words present would.
    CHECK(!tox2::dlaaPerspective(std::span<const std::uint32_t>(perspective_words).first(13)));
}

// Row-vector perspective projection: clip.w = view z.
tox2::Mat4 perspectiveMatrix(float fx, float fy, float near_plane, float far_plane) {
    const float a = far_plane / (far_plane - near_plane);
    const float b = -near_plane * far_plane / (far_plane - near_plane);
    return {fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, a, 1, 0, 0, b, 0};
}

tox2::Mat4 translationMatrix(float x, float y, float z) {
    tox2::Mat4 m = tox2::mat4Identity();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

// Needs no Vulkan device.
void testDlaaMatrices() {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-2.0f, 2.0f);

    // p * M is the guest's clip position x*c0 + y*c1 + z*c2 + w*c3.
    tox2::Mat4 random_matrix{};
    for (float& value : random_matrix) value = dist(rng);
    const std::array<float, 4> point = {0.5f, -1.5f, 2.0f, 1.0f};
    const std::array<float, 4> clip = tox2::mat4Transform(point, random_matrix);
    for (std::size_t lane = 0; lane < 4; ++lane) {
        const float expected = point[0] * random_matrix[lane] + point[1] * random_matrix[4 + lane] +
                               point[2] * random_matrix[8 + lane] + point[3] * random_matrix[12 + lane];
        CHECK(nearlyEqual(clip[lane], expected, 1e-5f));
    }

    // mat4Multiply applies the left matrix first.
    const tox2::Mat4 move = translationMatrix(1.0f, 2.0f, 3.0f);
    tox2::Mat4 scale = tox2::mat4Identity();
    scale[0] = 2.0f;
    const std::array<float, 4> moved_then_scaled = tox2::mat4Transform({1, 1, 1, 1}, tox2::mat4Multiply(move, scale));
    CHECK(nearlyEqual(moved_then_scaled[0], 4.0f, 1e-6f) && nearlyEqual(moved_then_scaled[1], 3.0f, 1e-6f) &&
          nearlyEqual(moved_then_scaled[2], 4.0f, 1e-6f));

    // Inverse round trip on well-conditioned random matrices (a dominant diagonal keeps them invertible).
    for (int trial = 0; trial < 8; ++trial) {
        tox2::Mat4 m{};
        for (float& value : m) value = dist(rng);
        for (std::size_t i = 0; i < 4; ++i) m[i * 5] += 6.0f;
        tox2::Mat4 inverse{};
        CHECK(tox2::mat4Inverse(m, inverse));
        const tox2::Mat4 product = tox2::mat4Multiply(m, inverse);
        const tox2::Mat4 identity = tox2::mat4Identity();
        for (std::size_t i = 0; i < 16; ++i) CHECK(nearlyEqual(product[i], identity[i], 1e-5f));
    }
    // Singular and non-finite matrices fail and leave the output alone.
    tox2::Mat4 untouched = tox2::mat4Identity();
    untouched[5] = 7.0f;
    tox2::Mat4 out = untouched;
    tox2::Mat4 singular = tox2::mat4Identity();
    singular[10] = 0.0f;
    CHECK(!tox2::mat4Inverse(singular, out) && out == untouched);
    CHECK(!tox2::mat4Inverse(tox2::Mat4{}, out) && out == untouched);
    tox2::Mat4 not_finite = tox2::mat4Identity();
    not_finite[3] = std::numeric_limits<float>::infinity();
    CHECK(!tox2::mat4Inverse(not_finite, out) && out == untouched);

    // Two cameras differing by a translation: the current clip position carried through clip_to_prev_clip lands
    // where the previous camera projects the same world point.
    tox2::DlaaCameraKey current;
    tox2::DlaaCameraKey previous;
    const tox2::Mat4 projection = perspectiveMatrix(1.2f, 2.1f, 0.5f, 100.0f);
    current.mvp = tox2::mat4Multiply(translationMatrix(-0.3f, 0.2f, 1.0f), projection);
    previous.mvp = tox2::mat4Multiply(translationMatrix(-0.5f, 0.25f, 0.5f), projection);
    for (tox2::DlaaCameraKey* key : {&current, &previous}) {
        key->viewport_scale[0] = 640.0f;
        key->viewport_scale[1] = -360.0f;
        key->viewport_scale[2] = 0.5f;
        key->viewport_offset[0] = 640.0f;
        key->viewport_offset[1] = 360.0f;
        key->viewport_offset[2] = 0.5f;
        key->clip_w = 1280.0f;
        key->clip_h = 720.0f;
    }
    // Both the raw MVPs and the clip matrices (viewport mapping included) must satisfy the identity.
    const std::pair<tox2::Mat4, tox2::Mat4> pairs[] = {{current.mvp, previous.mvp},
                                                       {tox2::dlaaClipMatrix(current), tox2::dlaaClipMatrix(previous)}};
    for (const auto& [m_current, m_previous] : pairs) {
        tox2::Mat4 current_inverse{}, previous_inverse{};
        CHECK(tox2::mat4Inverse(m_current, current_inverse));
        CHECK(tox2::mat4Inverse(m_previous, previous_inverse));
        const tox2::Mat4 clip_to_prev_clip = tox2::mat4Multiply(current_inverse, m_previous);
        const tox2::Mat4 prev_clip_to_clip = tox2::mat4Multiply(previous_inverse, m_current);
        for (const std::array<float, 4>& world :
             {std::array<float, 4>{1.0f, 2.0f, 10.0f, 1.0f}, std::array<float, 4>{-3.0f, 0.5f, 25.0f, 1.0f}}) {
            const std::array<float, 4> p_current = tox2::mat4Transform(world, m_current);
            const std::array<float, 4> p_previous = tox2::mat4Transform(world, m_previous);
            const std::array<float, 4> carried = tox2::mat4Transform(p_current, clip_to_prev_clip);
            CHECK(p_previous[3] > 0.0f && carried[3] > 0.0f);
            for (std::size_t lane = 0; lane < 3; ++lane) {
                CHECK(nearlyEqual(carried[lane] / carried[3], p_previous[lane] / p_previous[3], 1e-4f));
            }
            // And back again.
            const std::array<float, 4> back = tox2::mat4Transform(p_previous, prev_clip_to_clip);
            for (std::size_t lane = 0; lane < 3; ++lane) {
                CHECK(nearlyEqual(back[lane] / back[3], p_current[lane] / p_current[3], 1e-4f));
            }
        }
    }

    // dlaaClipMatrix reproduces the vertex epilogue's arithmetic (vp_translator.cpp): viewport transform
    // normalised to the clip size, then the depth range remap.
    tox2::DlaaCameraKey key = current;
    key.viewport_scale[0] = 600.0f;
    key.viewport_scale[1] = -340.0f;
    key.viewport_scale[2] = 0.45f;
    key.viewport_offset[0] = 650.0f;
    key.viewport_offset[1] = 350.0f;
    key.viewport_offset[2] = 0.55f;
    key.clip_min = 0.1f;
    key.clip_max = 0.9f;
    const tox2::Mat4 clip_matrix = tox2::dlaaClipMatrix(key);
    for (const std::array<float, 4>& world :
         {std::array<float, 4>{0.2f, -0.1f, 4.0f, 1.0f}, std::array<float, 4>{-1.0f, 0.8f, 9.0f, 1.0f},
          std::array<float, 4>{2.5f, 1.5f, 2.0f, 1.0f}}) {
        const std::array<float, 4> guest = tox2::mat4Transform(world, key.mvp);
        const float half_w = 0.5f * key.clip_w;
        const float half_h = 0.5f * key.clip_h;
        float x = guest[0] * (key.viewport_scale[0] / half_w) + guest[3] * ((key.viewport_offset[0] - half_w) / half_w);
        float y = guest[1] * (key.viewport_scale[1] / half_h) + guest[3] * ((key.viewport_offset[1] - half_h) / half_h);
        float z = guest[2] * key.viewport_scale[2] + guest[3] * key.viewport_offset[2];
        const float depth_near = std::min(key.clip_min, key.clip_max);
        const float range = std::max(key.clip_min, key.clip_max) - depth_near;
        z = ((z - depth_near * guest[3]) / (range * guest[3])) * guest[3];
        const std::array<float, 4> got = tox2::mat4Transform(world, clip_matrix);
        CHECK(nearlyEqual(got[0], x, 1e-5f * std::max(1.0f, std::abs(x))));
        CHECK(nearlyEqual(got[1], y, 1e-5f * std::max(1.0f, std::abs(y))));
        CHECK(nearlyEqual(got[2], z, 1e-5f * std::max(1.0f, std::abs(z))));
        CHECK(nearlyEqual(got[3], guest[3], 1e-5f * std::max(1.0f, std::abs(guest[3]))));
    }
    // The identity range leaves depth as viewport_scale.z * z + viewport_offset.z * w.
    key.clip_min = 0.0f;
    key.clip_max = 1.0f;
    const std::array<float, 4> world = {0.2f, -0.1f, 4.0f, 1.0f};
    const std::array<float, 4> guest = tox2::mat4Transform(world, key.mvp);
    const std::array<float, 4> identity_range = tox2::mat4Transform(world, tox2::dlaaClipMatrix(key));
    CHECK(nearlyEqual(identity_range[2], guest[2] * key.viewport_scale[2] + guest[3] * key.viewport_offset[2], 1e-5f));

    // F * m * F flips the sign of the y row and column pairs, and is its own inverse.
    const tox2::Mat4 flipped = tox2::dlaaFlipY(random_matrix);
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const float sign = ((row == 1) != (col == 1)) ? -1.0f : 1.0f;
            CHECK(flipped[row * 4 + col] == sign * random_matrix[row * 4 + col]);
        }
    }
    CHECK(tox2::dlaaFlipY(flipped) == random_matrix);
}

// Needs no Vulkan device.
void testDlaaCameraCut() {
    CHECK(!tox2::dlaaCameraCut(tox2::mat4Identity(), tox2::kDlaaCameraCutProbeZ));
    // The centre moves by 0.6 NDC: a cut. By 0.3: the history is still usable.
    CHECK(tox2::dlaaCameraCut(translationMatrix(0.6f, 0.0f, 0.0f), tox2::kDlaaCameraCutProbeZ));
    CHECK(tox2::dlaaCameraCut(translationMatrix(0.0f, -0.6f, 0.0f), tox2::kDlaaCameraCutProbeZ));
    CHECK(!tox2::dlaaCameraCut(translationMatrix(0.3f, 0.0f, 0.0f), tox2::kDlaaCameraCutProbeZ));
    CHECK(!tox2::dlaaCameraCut(translationMatrix(0.0f, 0.2f, 0.2f), tox2::kDlaaCameraCutProbeZ));
    // A matrix that sends the test points behind the camera (w <= 0).
    tox2::Mat4 behind = tox2::mat4Identity();
    behind[15] = -1.0f;
    CHECK(tox2::dlaaCameraCut(behind, tox2::kDlaaCameraCutProbeZ));
    tox2::Mat4 at_camera = tox2::mat4Identity();
    at_camera[15] = 0.0f;
    CHECK(tox2::dlaaCameraCut(at_camera, tox2::kDlaaCameraCutProbeZ));
    tox2::Mat4 not_a_number = tox2::mat4Identity();
    not_a_number[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(tox2::dlaaCameraCut(not_a_number, tox2::kDlaaCameraCutProbeZ));

    // A camera that walks 8 near-plane distances forward in one frame: the DLAA test points (about 2 near-plane
    // distances away) jump, the frame generation ones (about 100 away) barely move.
    const tox2::Mat4 projection = perspectiveMatrix(1.2f, 2.1f, 0.5f, 1000.0f);
    const tox2::Mat4 walked = tox2::mat4Multiply(translationMatrix(0.0f, 0.0f, -4.0f), projection);
    tox2::Mat4 walked_inverse{};
    CHECK(tox2::mat4Inverse(walked, walked_inverse));
    const tox2::Mat4 walk = tox2::mat4Multiply(walked_inverse, projection);
    CHECK(tox2::dlaaCameraCut(walk, tox2::kDlaaCameraCutProbeZ));
    CHECK(!tox2::dlaaCameraCut(walk, tox2::kFgCameraCutProbeZ));
    // A quarter turn is a cut at every depth.
    const tox2::Mat4 quarter_turn = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
    const tox2::Mat4 turned = tox2::mat4Multiply(quarter_turn, projection);
    tox2::Mat4 turned_inverse{};
    CHECK(tox2::mat4Inverse(turned, turned_inverse));
    const tox2::Mat4 turn = tox2::mat4Multiply(turned_inverse, projection);
    CHECK(tox2::dlaaCameraCut(turn, tox2::kDlaaCameraCutProbeZ));
    CHECK(tox2::dlaaCameraCut(turn, tox2::kFgCameraCutProbeZ));
}

// A camera that differs from the others by `shift`: a perspective projection behind a translation.
tox2::DlaaCameraKey trackerKey(float shift) {
    tox2::DlaaCameraKey key;
    key.mvp = tox2::mat4Multiply(translationMatrix(shift, 0.2f, 1.0f), perspectiveMatrix(1.2f, 2.1f, 0.5f, 100.0f));
    key.viewport_scale[0] = 640.0f;
    key.viewport_scale[1] = -360.0f;
    key.viewport_scale[2] = 0.5f;
    key.viewport_offset[0] = 640.0f;
    key.viewport_offset[1] = 360.0f;
    key.viewport_offset[2] = 0.5f;
    key.clip_w = 1280.0f;
    key.clip_h = 720.0f;
    return key;
}

// One frame whose camera is `key`, ended without an evaluation.
void trackerFrame(tox2::DlaaCameraTracker& tracker, const tox2::DlaaCameraKey& key, int draws = 1) {
    for (int i = 0; i < draws; ++i) tracker.record(key);
    tracker.endFrame();
}

bool isIdentityResult(const tox2::DlaaCameraTracker::Result& result) {
    const tox2::DlaaMatrices& m = result.matrices;
    const tox2::Mat4 identity = tox2::mat4Identity();
    return m.view_to_clip == identity && m.clip_to_view == identity && m.clip_to_prev_clip == identity &&
           m.prev_clip_to_clip == identity;
}

// The clip-to-clip transforms the tracker must report for a current and a previous camera.
void expectTransitions(const tox2::DlaaCameraTracker::Result& result, const tox2::DlaaCameraKey& current,
                       const tox2::DlaaCameraKey& previous) {
    const tox2::Mat4 clip_current = tox2::dlaaClipMatrix(current);
    const tox2::Mat4 clip_previous = tox2::dlaaClipMatrix(previous);
    tox2::Mat4 inverse_current{}, inverse_previous{};
    CHECK(tox2::mat4Inverse(clip_current, inverse_current) && tox2::mat4Inverse(clip_previous, inverse_previous));
    CHECK(result.matrices.view_to_clip == clip_current);
    CHECK(result.matrices.clip_to_prev_clip == tox2::mat4Multiply(inverse_current, clip_previous));
    CHECK(result.matrices.prev_clip_to_clip == tox2::mat4Multiply(inverse_previous, clip_current));
}

// Needs no Vulkan device.
void testDlaaCameraTracker() {
    const tox2::DlaaCameraKey a = trackerKey(-0.30f), b = trackerKey(-0.31f), c = trackerKey(-0.32f);

    // No previous camera: identity matrices and a reset, whatever the current frame drew.
    {
        tox2::DlaaCameraTracker tracker;
        tracker.record(a);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(result.reset && isIdentityResult(result));
    }
    // A previous camera but none in the current frame: the same.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(result.reset && isIdentityResult(result));
    }
    // Two consecutive cameras: the transforms between them, no reset, and the y flip mirrors both.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        tracker.record(b);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(!result.reset);
        expectTransitions(result, b, a);
        const tox2::DlaaCameraTracker::Result flipped = tracker.resolve(true);
        CHECK(!flipped.reset);
        CHECK(flipped.matrices.view_to_clip == result.matrices.view_to_clip);
        CHECK(flipped.matrices.clip_to_prev_clip == tox2::dlaaFlipY(result.matrices.clip_to_prev_clip));
        CHECK(flipped.matrices.prev_clip_to_clip == tox2::dlaaFlipY(result.matrices.prev_clip_to_clip));
    }

    // A singular camera, as the current or the previous one, resets with identity matrices.
    tox2::DlaaCameraKey singular = a;
    singular.mvp = tox2::Mat4{};
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        tracker.record(singular);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(result.reset && isIdentityResult(result));
    }
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, singular);
        tracker.record(a);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(result.reset && isIdentityResult(result));
    }

    // The most drawn camera wins; on a tie the first one seen does.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        tracker.record(b);
        tracker.record(c);
        tracker.record(c);
        expectTransitions(tracker.resolve(false), c, a);
    }
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        tracker.record(b);
        tracker.record(c);
        tracker.record(b);
        tracker.record(c);
        expectTransitions(tracker.resolve(false), b, a);
    }

    // Only kMaxKeys distinct cameras are counted: a further one is ignored however often it is drawn, while the
    // counted ones keep counting.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        std::vector<tox2::DlaaCameraKey> counted;
        for (std::size_t i = 0; i < tox2::DlaaCameraTracker::kMaxKeys; ++i) {
            counted.push_back(trackerKey(-0.5f - 0.01f * static_cast<float>(i)));
            tracker.record(counted.back());
        }
        const tox2::DlaaCameraKey extra = trackerKey(-0.9f);
        for (int i = 0; i < 5; ++i) tracker.record(extra);
        expectTransitions(tracker.resolve(false), counted.front(), a);
        tracker.record(counted.back());
        expectTransitions(tracker.resolve(false), counted.back(), a);
    }

    // The camera the evaluation used, not the whole frame's, is the next frame's previous one.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        tracker.record(b);
        CHECK(!tracker.resolve(false).reset);           // the evaluation: b is all it has seen
        for (int i = 0; i < 3; ++i) tracker.record(c);  // drawn after the evaluation, and the more drawn
        tracker.endFrame();
        tracker.record(a);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(!result.reset);
        expectTransitions(result, a, b);
        tracker.endFrame();
        // A frame without an evaluation hands on its own dominant camera.
        tracker.record(b);
        tracker.record(c);
        tracker.record(c);
        tracker.endFrame();
        tracker.record(a);
        expectTransitions(tracker.resolve(false), a, c);
    }
    // An evaluation that saw no camera leaves none, even if one is drawn later in the frame.
    {
        tox2::DlaaCameraTracker tracker;
        trackerFrame(tracker, a);
        CHECK(tracker.resolve(false).reset);
        tracker.record(b);
        tracker.endFrame();
        tracker.record(c);
        const tox2::DlaaCameraTracker::Result result = tracker.resolve(false);
        CHECK(result.reset && isIdentityResult(result));
    }
}

// Needs no Vulkan device.
void testWindowSizePresets() {
    for (const tox2::WindowSizePreset preset : tox2::kWindowSizePresets) {
        const std::optional<tox2::WindowSizePreset> parsed = tox2::parseWindowSize(tox2::windowSizeName(preset));
        CHECK(parsed.has_value() && *parsed == preset);
    }
    CHECK(tox2::windowSizeName(tox2::WindowSizePreset::Size720p) == "720p");
    CHECK(tox2::windowSizeName(tox2::WindowSizePreset::Size2160p) == "2160p");
    CHECK(!tox2::parseWindowSize("").has_value());
    CHECK(!tox2::parseWindowSize("1080P").has_value());
    CHECK(!tox2::parseWindowSize("1080p ").has_value());
    CHECK(!tox2::parseWindowSize("900p").has_value());
    static_assert(tox2::presetClientSize(tox2::WindowSizePreset::Size720p) == tox2::ClientSize{1280, 720});
    static_assert(tox2::presetClientSize(tox2::WindowSizePreset::Size1080p) == tox2::ClientSize{1920, 1080});
    static_assert(tox2::presetClientSize(tox2::WindowSizePreset::Size1440p) == tox2::ClientSize{2560, 1440});
    static_assert(tox2::presetClientSize(tox2::WindowSizePreset::Size2160p) == tox2::ClientSize{3840, 2160});

    CHECK(tox2::fitClientSize({1920, 1080}, {1906, 1009}) == (tox2::ClientSize{1792, 1008}));
    CHECK(tox2::fitClientSize({1280, 720}, {2000, 2000}) == (tox2::ClientSize{1280, 720}));
    CHECK(tox2::fitClientSize({1280, 720}, {1280, 720}) == (tox2::ClientSize{1280, 720}));
    CHECK(tox2::fitClientSize({3840, 2160}, {8, 8}) == (tox2::ClientSize{16, 9}));
    // Width is the limiting side here, so the result is a multiple of 16 below the available width.
    CHECK(tox2::fitClientSize({3840, 2160}, {2500, 2000}) == (tox2::ClientSize{2496, 1404}));
}

// Needs no window.
void testPlaceInWorkArea() {
    using tox2::ScreenRect;
    const ScreenRect work{0, 0, 1920, 1040};
    // A rectangle that fits stays where it is.
    CHECK(tox2::placeInWorkArea({100, 50, 900, 650}, work) == (ScreenRect{100, 50, 900, 650}));
    CHECK(tox2::placeInWorkArea({0, 0, 1920, 1040}, work) == (ScreenRect{0, 0, 1920, 1040}));
    // Each side that sticks out is pulled in by the least distance, keeping the size.
    CHECK(tox2::placeInWorkArea({-30, 50, 770, 650}, work) == (ScreenRect{0, 50, 800, 650}));
    CHECK(tox2::placeInWorkArea({1500, 50, 2300, 650}, work) == (ScreenRect{1120, 50, 1920, 650}));
    CHECK(tox2::placeInWorkArea({100, -20, 900, 580}, work) == (ScreenRect{100, 0, 900, 600}));
    CHECK(tox2::placeInWorkArea({100, 700, 900, 1300}, work) == (ScreenRect{100, 440, 900, 1040}));
    CHECK(tox2::placeInWorkArea({1500, 700, 2300, 1300}, work) == (ScreenRect{1120, 440, 1920, 1040}));
    // A work area that does not start at the origin (a monitor left of or above the primary one).
    const ScreenRect second{-1920, -200, 0, 880};
    CHECK(tox2::placeInWorkArea({-100, 500, 700, 1100}, second) == (ScreenRect{-800, 280, 0, 880}));
    CHECK(tox2::placeInWorkArea({200, 0, 1000, 600}, second) == (ScreenRect{-800, 0, 0, 600}));
    // Larger than the work area: aligned to the left and top edges.
    CHECK(tox2::placeInWorkArea({300, 200, 2300, 1400}, work) == (ScreenRect{0, 0, 2000, 1200}));
    CHECK(tox2::placeInWorkArea({-50, -50, 1950, 1150}, work) == (ScreenRect{0, 0, 2000, 1200}));
    // Wider only, or taller only.
    CHECK(tox2::placeInWorkArea({300, 200, 2300, 800}, work) == (ScreenRect{0, 200, 2000, 800}));
    CHECK(tox2::placeInWorkArea({300, 200, 900, 1400}, work) == (ScreenRect{300, 0, 900, 1200}));
}

// Needs no Vulkan device.
void testAnisotropyNames() {
    for (const std::uint32_t level : tox2::kAnisotropyLevels) {
        const std::optional<std::uint32_t> parsed = tox2::parseAnisotropy(tox2::anisotropyName(level));
        CHECK(parsed.has_value() && *parsed == level);
    }
    CHECK(tox2::anisotropyName(1) == "off");
    CHECK(tox2::anisotropyName(16) == "16");
    CHECK(tox2::anisotropyName(0).empty() && tox2::anisotropyName(3).empty() && tox2::anisotropyName(32).empty());
    CHECK(*tox2::parseAnisotropy("off") == 1);
    CHECK(*tox2::parseAnisotropy("8") == 8);
    CHECK(!tox2::parseAnisotropy("").has_value());
    CHECK(!tox2::parseAnisotropy("1").has_value());
    CHECK(!tox2::parseAnisotropy("3").has_value());
    CHECK(!tox2::parseAnisotropy("Off").has_value());
    CHECK(!tox2::parseAnisotropy("16x").has_value());
}

// Needs no Vulkan device.
void testForcedAnisotropy() {
    tox2::SamplerSettings mipped;
    mipped.min_filter = VK_FILTER_LINEAR;
    mipped.max_lod = 3.0f;
    CHECK(tox2::forcedAnisotropy(mipped, 16, 16.0f) == 16.0f);
    CHECK(tox2::forcedAnisotropy(mipped, 4, 16.0f) == 4.0f);
    CHECK(tox2::forcedAnisotropy(mipped, 16, 8.0f) == 8.0f);  // clamped to the device maximum
    CHECK(tox2::forcedAnisotropy(mipped, 1, 16.0f) == 1.0f);  // off
    CHECK(tox2::forcedAnisotropy(mipped, 0, 16.0f) == 1.0f);
    CHECK(tox2::forcedAnisotropy(mipped, 16, 0.0f) == 1.0f);  // no samplerAnisotropy feature
    CHECK(tox2::forcedAnisotropy(mipped, 16, 1.0f) == 1.0f);

    tox2::SamplerSettings nearest = mipped;
    nearest.min_filter = VK_FILTER_NEAREST;
    CHECK(tox2::forcedAnisotropy(nearest, 16, 16.0f) == 1.0f);

    tox2::SamplerSettings single_level = mipped;
    single_level.max_lod = 0.0f;
    CHECK(tox2::forcedAnisotropy(single_level, 16, 16.0f) == 1.0f);

    // The magnification filter does not matter; only a linear minification that reads mip levels qualifies.
    tox2::SamplerSettings nearest_mag = mipped;
    nearest_mag.mag_filter = VK_FILTER_NEAREST;
    CHECK(tox2::forcedAnisotropy(nearest_mag, 8, 16.0f) == 8.0f);
}

// Needs no Vulkan device: texel x of a layout row comes from pixel x / 2, alternating between the two samples.
void testLayoutSource() {
    constexpr std::uint32_t kExpectedPixel[8] = {0, 0, 1, 1, 2, 2, 3, 3};
    constexpr std::uint32_t kExpectedSample[8] = {1, 0, 1, 0, 1, 0, 1, 0};
    for (std::uint32_t x = 0; x < 8; x++) {
        const tox2::LayoutSource source = tox2::layoutSource(x, 3);
        CHECK(source.x == kExpectedPixel[x] && source.y == 3 && source.sample == kExpectedSample[x]);
    }
    static_assert(tox2::layoutSource(1, 0).x == 0 && tox2::layoutSource(1, 0).sample == tox2::kLayoutColumnSample[1]);
    const tox2::LayoutSource wide = tox2::layoutSource(2 * 1280 * 4 - 1, 719);
    CHECK(wide.x == 1280 * 4 - 1 && wide.y == 719 && wide.sample == tox2::kLayoutColumnSample[1]);
}

// Prints the 2-sample support verdict and checks that an empty gap implies every sample-count limit has 2.
void testTwoSampleSupport(tox2::VkContext& ctx) {
    const VkPhysicalDeviceLimits& limits = ctx.deviceProperties().limits;
    const std::string& gap = ctx.twoSampleSupportGap();
    if (gap.empty()) {
        std::printf("2-sample multisampling available, standardSampleLocations %s\n",
                    limits.standardSampleLocations != VK_FALSE ? "true" : "false");
    } else {
        std::printf("2-sample multisampling unavailable (%s), standardSampleLocations %s\n", gap.c_str(),
                    limits.standardSampleLocations != VK_FALSE ? "true" : "false");
    }
    if (gap.empty()) {
        const VkSampleCountFlags all[] = {
                limits.framebufferColorSampleCounts,   limits.framebufferDepthSampleCounts,
                limits.framebufferStencilSampleCounts, limits.sampledImageColorSampleCounts,
                limits.sampledImageDepthSampleCounts,  limits.sampledImageStencilSampleCounts,
        };
        for (const VkSampleCountFlags counts : all) CHECK((counts & VK_SAMPLE_COUNT_2_BIT) != 0);
    }
}

// The pipelines of the 2-sample passes build without validation errors. Skipped on a device without 2 samples.
void testLayoutPassPipelines(tox2::VkContext& ctx) {
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("layout pass pipelines: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    tox2::DrawPipelines pipelines(ctx);
    CHECK(pipelines.valid());

    CHECK(pipelines.layoutPassLayout() == VK_NULL_HANDLE);  // created with the first layout pass pipeline
    CHECK(pipelines.depthConvertPipeline(false) != VK_NULL_HANDLE);
    CHECK(pipelines.layoutPassLayout() != VK_NULL_HANDLE);
    const VkPipeline convert_ms = pipelines.depthConvertPipeline(true);
    CHECK(convert_ms != VK_NULL_HANDLE && convert_ms != pipelines.depthConvertPipeline(false));
    CHECK(pipelines.depthConvertPipeline(true) == convert_ms);

    for (const VkFormat format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R32_SFLOAT}) {
        const VkPipeline pipeline = pipelines.layoutColorPipeline(format);
        CHECK(pipeline != VK_NULL_HANDLE && pipelines.layoutColorPipeline(format) == pipeline);
    }
    CHECK(pipelines.layoutColorPipeline(VK_FORMAT_B8G8R8A8_UNORM) !=
          pipelines.layoutColorPipeline(VK_FORMAT_R32_SFLOAT));
    for (const VkFormat format : {ctx.depthStencilFormat(), VK_FORMAT_D16_UNORM}) {
        const VkPipeline pipeline = pipelines.layoutDepthPipeline(format);
        CHECK(pipeline != VK_NULL_HANDLE && pipelines.layoutDepthPipeline(format) == pipeline);
    }

    // The same program drawn into 1-sample and 2-sample attachments gets two pipelines.
    std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeQuadCapture());
    const tox2::RsxDraw* draw = nullptr;
    for (const tox2::RsxCommand& command : commands) {
        if (const auto* candidate = std::get_if<tox2::RsxDraw>(&command)) {
            draw = candidate;
            break;
        }
    }
    CHECK(draw != nullptr);
    if (draw != nullptr) {
        tox2::DrawShaders shaders;
        CHECK(pipelines.shaders(*draw, shaders));
        tox2::PipelineKey key;
        key.vp_hash = draw->vp_hash;
        key.fp_hash = draw->fp_hash;
        key.fp_control = draw->fp_control;
        key.texcoord_control = draw->texcoord_control;
        key.color_format = VK_FORMAT_B8G8R8A8_UNORM;
        key.depth_format = ctx.depthStencilFormat();
        for (std::uint32_t slot = 0; slot < tox2::kPipelineVertexSlots; slot++) {
            if ((shaders.inputs_used & (1u << slot)) == 0) continue;
            key.vertex_formats[slot] = VK_FORMAT_R32G32B32A32_SFLOAT;
            key.vertex_strides[slot] = 16;
        }
        const VkPipeline single = pipelines.pipeline(key, shaders);
        key.samples = VK_SAMPLE_COUNT_2_BIT;
        const VkPipeline multi = pipelines.pipeline(key, shaders);
        CHECK(single != VK_NULL_HANDLE && multi != VK_NULL_HANDLE && single != multi);
    }

    const VkPipeline clear_single = pipelines.maskedClearPipeline(VK_FORMAT_B8G8R8A8_UNORM,
                                                                  VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT);
    const VkPipeline clear_multi = pipelines.maskedClearPipeline(
            VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT, VK_SAMPLE_COUNT_2_BIT);
    CHECK(clear_single != VK_NULL_HANDLE && clear_multi != VK_NULL_HANDLE && clear_single != clear_multi);

    CHECK(pipelines.valid());
    CHECK(ctx.validationErrorCount() == errors_before);
}

// The object motion variant of a draw's shaders and pipeline build without validation errors and differ from the plain
// ones. Skipped on a device that lacks a limit or independentBlend.
void testObjectMotionPipeline(tox2::VkContext& ctx) {
    const VkPhysicalDeviceLimits& limits = ctx.deviceProperties().limits;
    const bool limits_fit = limits.maxVertexInputAttributes >= 32 && limits.maxVertexInputBindings >= 32 &&
                            limits.maxVertexOutputComponents >= 84 && limits.maxFragmentInputComponents >= 72 &&
                            limits.maxColorAttachments >= 2;
    CHECK(ctx.objectMotionSupported() == ctx.objectMotionGap().empty());
    CHECK(!ctx.objectMotionSupported() || limits_fit);
    if (!ctx.objectMotionSupported()) {
        std::printf("object motion pipeline: skipped (%s)\n", ctx.objectMotionGap().c_str());
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    tox2::DrawPipelines pipelines(ctx);
    CHECK(pipelines.valid());

    std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeQuadCapture());
    const tox2::RsxDraw* draw = nullptr;
    for (const tox2::RsxCommand& command : commands) {
        if (const auto* candidate = std::get_if<tox2::RsxDraw>(&command)) {
            draw = candidate;
            break;
        }
    }
    CHECK(draw != nullptr);
    if (draw == nullptr) return;

    tox2::DrawShaders plain, motion;
    CHECK(pipelines.shaders(*draw, plain));
    CHECK(pipelines.shaders(*draw, motion, true));
    CHECK(motion.vertex != plain.vertex && motion.fragment != plain.fragment);
    CHECK(motion.inputs_used == plain.inputs_used && motion.texture_units == plain.texture_units);
    tox2::DrawShaders again;
    CHECK(pipelines.shaders(*draw, again, true));
    CHECK(again.vertex == motion.vertex && again.fragment == motion.fragment);

    tox2::PipelineKey key;
    key.vp_hash = draw->vp_hash;
    key.fp_hash = draw->fp_hash;
    key.fp_control = draw->fp_control;
    key.texcoord_control = draw->texcoord_control;
    key.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    key.depth_format = ctx.depthStencilFormat();
    key.blend = true;
    key.src_color = key.src_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
    key.dst_color = key.dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    for (std::uint32_t slot = 0; slot < tox2::kPipelineVertexSlots; slot++) {
        if ((plain.inputs_used & (1u << slot)) == 0) continue;
        key.vertex_formats[slot] = VK_FORMAT_R32G32B32A32_SFLOAT;
        key.vertex_strides[slot] = 16;
    }
    const VkPipeline plain_pipeline = pipelines.pipeline(key, plain);
    key.object_motion = true;
    const VkPipeline motion_pipeline = pipelines.pipeline(key, motion);
    CHECK(plain_pipeline != VK_NULL_HANDLE && motion_pipeline != VK_NULL_HANDLE);
    CHECK(plain_pipeline != motion_pipeline && pipelines.pipeline(key, motion) == motion_pipeline);

    CHECK(pipelines.valid());
    CHECK(ctx.validationErrorCount() == errors_before);
}

tox2::RenderOptions readbackAllOptions(std::uint32_t resolution_scale = 1, tox2::AaMode aa_mode = tox2::AaMode::Msaa) {
    tox2::RenderOptions options;
    options.readback_all = true;
    options.resolution_scale = resolution_scale;
    options.aa_mode = aa_mode;
    // The scene-image tests read the layout path; the evaluation has its own test.
    options.dlaa_evaluate = false;
    return options;
}

// The effective anti-aliasing mode follows the request, except that Msaa falls back on a device without 2 samples.
void testRendererAaMode(tox2::VkContext& ctx) {
    {
        tox2::rsxUnimplementedReset();
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(1, tox2::AaMode::Supersample));
        CHECK(renderer.valid() && renderer.aaMode() == tox2::AaMode::Supersample);
        CHECK(unimplementedCount("state 2-sample multisampling unavailable") == 0);
    }
    {
        tox2::rsxUnimplementedReset();
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(1, tox2::AaMode::Msaa));
        CHECK(renderer.valid());
        CHECK(renderer.msaaAvailable() == ctx.twoSampleSupportGap().empty());
        if (renderer.msaaAvailable()) {
            CHECK(renderer.aaMode() == tox2::AaMode::Msaa);
            CHECK(unimplementedCount("state 2-sample multisampling unavailable") == 0);
        } else {
            CHECK(renderer.aaMode() == tox2::AaMode::Supersample);
            CHECK(unimplementedCount("state 2-sample multisampling unavailable") == 1);
        }
    }
    {
        tox2::rsxUnimplementedReset();
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(1, tox2::AaMode::Dlaa));
        CHECK(renderer.valid());
        CHECK(renderer.dlaaAvailable() == renderer.dlaaUnavailableReason().empty());
        if (renderer.dlaaAvailable()) {
            CHECK(renderer.aaMode() == tox2::AaMode::Dlaa);
            CHECK(unimplementedCount("state DLAA unavailable") == 0);
        } else {
            CHECK(renderer.aaMode() == (renderer.msaaAvailable() ? tox2::AaMode::Msaa : tox2::AaMode::Supersample));
            CHECK(unimplementedCount("state DLAA unavailable") == 1);
        }
        // A switch to Dlaa succeeds exactly when it is available and leaves the mode alone otherwise.
        const tox2::AaMode before = renderer.aaMode();
        CHECK(renderer.setAaMode(tox2::AaMode::Dlaa) == renderer.dlaaAvailable());
        CHECK(renderer.aaMode() == (renderer.dlaaAvailable() ? tox2::AaMode::Dlaa : before));
        std::printf("render: DLAA %s\n",
                    renderer.dlaaAvailable() ? "available" : renderer.dlaaUnavailableReason().c_str());
    }
    tox2::rsxUnimplementedReset();
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

// A requested dump needs no dump_dir: only the asked flip is read back and written, into
// requested_dump_dir.
void testRendererRequestedDump(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    std::error_code ec;
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            std::format("tox2_render_test_requested_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());  // unique under parallel runs
    std::filesystem::remove_all(dir, ec);

    tox2::RenderOptions options;
    options.requested_dump_dir = dir;
    tox2::RsxRenderer renderer(ctx, options);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (std::uint32_t flip = 1; flip <= 3; flip++) {
        if (flip == 2) renderer.requestDisplayDump();
        std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(0xFF102030));
        renderer.execute(commands);
        CHECK(renderer.valid());
        CHECK(renderer.readDisplay(bgra, width, height) == (flip == 2));
    }
    CHECK(renderer.flushDumps(std::chrono::seconds(10)) == 0);
    const tox2::DumpCounts dumps = renderer.dumpCounts();
    CHECK(dumps.written == 1 && dumps.failed == 0 && dumps.skipped == 0);
    CHECK(std::filesystem::exists(dir / "frame_000002.png"));
    CHECK(!std::filesystem::exists(dir / "frame_000001.png") && !std::filesystem::exists(dir / "frame_000003.png"));
    tox2::RgbImage image;
    std::string error;
    CHECK(tox2::readPngRgb(dir / "frame_000002.png", image, error) && image.width == width && image.height == height);
    CHECK(image.rgb.size() >= 3 && image.rgb[0] == 0x10 && image.rgb[1] == 0x20 && image.rgb[2] == 0x30);
    std::filesystem::remove_all(dir, ec);
    tox2::rsxUnimplementedReset();
}

// A flip that dump_dir writes anyway is not written again into requested_dump_dir.
void testRendererRequestedDumpSharesDumpDir(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    std::error_code ec;
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() /
            std::format("tox2_render_test_shared_{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::remove_all(base, ec);

    tox2::RenderOptions options;
    options.dump_dir = base / "dumped";
    options.dump_every = 2;
    options.requested_dump_dir = base / "requested";
    tox2::RsxRenderer renderer(ctx, options);
    CHECK(renderer.valid());
    for (std::uint32_t flip = 1; flip <= 3; flip++) {
        if (flip == 2 || flip == 3) renderer.requestDisplayDump();
        std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(0xFF102030));
        renderer.execute(commands);
        CHECK(renderer.valid());
    }
    CHECK(renderer.flushDumps(std::chrono::seconds(10)) == 0);
    CHECK(renderer.dumpCounts().written == 2);
    CHECK(std::filesystem::exists(base / "dumped" / "frame_000002.png"));
    CHECK(!std::filesystem::exists(base / "requested" / "frame_000002.png"));
    CHECK(std::filesystem::exists(base / "requested" / "frame_000003.png"));
    std::filesystem::remove_all(base, ec);
    tox2::rsxUnimplementedReset();
}

class CountingOverlay final : public tox2::PresentOverlay {
public:
    tox2::PresentOverlayRequests onFlip(const tox2::PresentFrameInfo&) override {
        on_flip++;
        return {};
    }
    bool wantsDraw() const override { return true; }
    bool wantsIdlePresent() const override { return true; }
    void record(VkCommandBuffer, const tox2::Presenter::Target&) override { records++; }

    int on_flip = 0;
    int records = 0;
};

void testIdlePresentDue() {
    using Clock = tox2::RsxRenderer::Clock;
    using std::chrono::milliseconds;
    const Clock::time_point flip = Clock::now();
    const auto due = [](Clock::time_point last_flip, Clock::time_point last_idle, Clock::time_point now) {
        return tox2::RsxRenderer::idlePresentDue(last_flip, last_idle, now);
    };
    // No idle present until the game has not flipped for 100 ms, however old the last idle present is.
    CHECK(!due(flip, flip - std::chrono::seconds(1), flip));
    CHECK(!due(flip, flip - std::chrono::seconds(1), flip + milliseconds(99)));
    CHECK(due(flip, flip - std::chrono::seconds(1), flip + milliseconds(100)));
    // The next one follows 33 ms after the previous idle present, not after the flip.
    const Clock::time_point idle = flip + milliseconds(100);
    CHECK(!due(flip, idle, idle));
    CHECK(!due(flip, idle, idle + milliseconds(32)));
    CHECK(due(flip, idle, idle + milliseconds(33)));
    // A new flip restarts the stall wait even when the last idle present is long ago.
    const Clock::time_point next_flip = idle + milliseconds(500);
    CHECK(!due(next_flip, idle, next_flip + milliseconds(99)));
    CHECK(due(next_flip, idle, next_flip + milliseconds(100)));
}

// Without a presenter nothing can be shown, so an idle present does nothing, before and after a flip.
void testPresentIdleNeedsPresenter(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    CountingOverlay overlay;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    renderer.setPresentOverlay(&overlay);
    renderer.presentIdle();
    CHECK(renderer.valid() && renderer.stats().flips == 0);
    std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(0xFF102030));
    renderer.execute(commands);
    CHECK(renderer.valid() && renderer.stats().flips == 1);
    renderer.presentIdle();
    CHECK(renderer.valid() && renderer.stats().flips == 1);
    CHECK(overlay.on_flip == 0 && overlay.records == 0);
    tox2::rsxUnimplementedReset();
}

// The overlay hook belongs to the present path: a renderer without a presenter never calls it.
void testPresentOverlayNeedsPresenter(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    CountingOverlay overlay;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    renderer.setPresentOverlay(&overlay);
    for (int flip = 0; flip < 3; flip++) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(tox2::rsx_synthetic::makeClearCapture(0xFF102030));
        renderer.execute(commands);
        CHECK(renderer.valid());
    }
    CHECK(renderer.stats().flips == 3);
    CHECK(overlay.on_flip == 0 && overlay.records == 0);
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

    // Triangle fans as lists.
    CHECK((tox2::triangleFanToList(2, 5) == std::vector<std::uint32_t>{2, 3, 4, 2, 4, 5, 2, 5, 6}));
    CHECK(tox2::triangleFanToList(0, 2).empty() && tox2::triangleFanToList(7, 0).empty());
    const std::uint32_t fan_indices[4] = {9, 8, 7, 6};
    CHECK((tox2::triangleFanIndicesToList(fan_indices) == std::vector<std::uint32_t>{9, 8, 7, 9, 7, 6}));
    CHECK(tox2::triangleFanIndicesToList(std::span<const std::uint32_t>(fan_indices, 2)).empty());
    CHECK(tox2::triangleFanIndicesToList({}).empty());

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

// The conversion before it was specialised per format, component at a time: the reference the fast loops must match
// byte for byte.
std::uint32_t referenceComponentBytes(std::uint8_t type) {
    switch (type) {
        case tox2::kVertexTypeFloat: return 4;
        case tox2::kVertexTypeSnorm16:
        case tox2::kVertexTypeHalf:
        case tox2::kVertexTypeSint16: return 2;
        default: return 1;
    }
}

std::uint32_t referencePadded(std::uint8_t type) {
    switch (type) {
        case tox2::kVertexTypeHalf: return 0x3C00;
        case tox2::kVertexTypeSnorm16: return 0x7FFF;
        case tox2::kVertexTypeSint16: return 1;
        default: return 0xFF;
    }
}

std::uint32_t referenceWiden(const tox2::VertexAttribFormat& format, std::span<const std::uint8_t> src,
                             std::uint32_t stride, std::uint32_t n, std::span<std::uint8_t> dst) {
    const std::uint32_t component = referenceComponentBytes(format.type);
    std::uint32_t written = 0;
    for (std::uint32_t k = 0; k < n; k++) {
        const std::uint64_t offset = static_cast<std::uint64_t>(k) * stride;
        if (offset + format.guest_size > src.size()) break;
        const std::uint8_t* in = src.data() + offset;
        float values[4];
        for (std::uint32_t c = 0; c < 4; c++) {
            if (c >= format.size) {
                values[c] = c == 3 ? 1.0f : 0.0f;
            } else if (component == 2) {
                values[c] = static_cast<float>(
                        static_cast<std::int16_t>(static_cast<std::uint16_t>(in[c * 2] << 8 | in[c * 2 + 1])));
            } else {
                values[c] = static_cast<float>(in[c]);
            }
        }
        std::memcpy(dst.data() + static_cast<std::size_t>(k) * 16, values, 16);
        written++;
    }
    return written;
}

std::uint32_t referenceConvert(const tox2::VertexAttribFormat& format, std::span<const std::uint8_t> src,
                               std::uint32_t stride, std::uint32_t count, std::span<std::uint8_t> dst) {
    if (format.widen)
        return referenceWiden(format, src, stride, stride == 0 ? std::min<std::uint32_t>(count, 1) : count, dst);
    const std::uint32_t component = referenceComponentBytes(format.type);
    const std::uint32_t out_components = format.host_size / component;
    const std::uint32_t n = stride == 0 ? std::min<std::uint32_t>(count, 1) : count;
    std::uint32_t written = 0;
    for (std::uint32_t k = 0; k < n; k++) {
        const std::uint64_t offset = static_cast<std::uint64_t>(k) * stride;
        if (offset + format.guest_size > src.size()) break;
        const std::uint8_t* in = src.data() + offset;
        std::uint8_t* out = dst.data() + static_cast<std::size_t>(k) * format.host_size;
        for (std::uint32_t c = 0; c < out_components; c++) {
            std::uint8_t* o = out + c * component;
            if (c >= format.size) {
                const std::uint32_t value = referencePadded(format.type);
                if (component == 2) {
                    o[0] = static_cast<std::uint8_t>(value);
                    o[1] = static_cast<std::uint8_t>(value >> 8);
                } else {
                    o[0] = static_cast<std::uint8_t>(value);
                }
                continue;
            }
            const std::uint8_t* i = in + c * component;
            if (component == 4) {
                o[0] = i[3];
                o[1] = i[2];
                o[2] = i[1];
                o[3] = i[0];
            } else if (component == 2) {
                o[0] = i[1];
                o[1] = i[0];
            } else {
                o[0] = i[0];
            }
        }
        written++;
    }
    return written;
}

// A format the way vertexAttribFormat sizes it, also for the types and sizes it refuses.
tox2::VertexAttribFormat sizedFormat(std::uint8_t type, std::uint8_t size) {
    tox2::VertexAttribFormat format;
    format.type = type;
    format.size = size;
    format.guest_size = referenceComponentBytes(type) * size;
    format.host_size = referenceComponentBytes(type) * (size == 3 && type != tox2::kVertexTypeFloat ? 4 : size);
    if (const auto real = tox2::vertexAttribFormat(type, size)) {
        CHECK(real->guest_size == format.guest_size && real->host_size == format.host_size);
        format.format = real->format;
    }
    return format;
}

// The one-pass conversion agrees with the separate conversion and range check on random data, with and without a
// restart value, and notices an out-of-range index at either end.
void testConvertIndicesChecked() {
    std::mt19937 rng(0x1D1CE5);
    constexpr std::uint32_t kLow = 100, kHigh = 5000;
    constexpr std::uint32_t kCount = 257;  // not a multiple of any vector width
    for (const bool is_u16 : {true, false}) {
        for (const bool restart : {false, true}) {
            const std::uint32_t size = is_u16 ? 2 : 4;
            const std::uint32_t restart_index = is_u16 ? 0xFFFF : 0xFFFFFFFF;
            std::vector<std::uint32_t> values(kCount);
            for (std::uint32_t& value : values) {
                value = kLow + static_cast<std::uint32_t>(rng() % (kHigh - kLow + 1));
                if (restart && rng() % 8 == 0) value = restart_index;
            }
            values.front() = kLow;
            values[1] = kHigh;
            const auto pack = [&](const std::vector<std::uint32_t>& list) {
                std::vector<std::uint8_t> bytes(list.size() * size);
                for (std::size_t i = 0; i < list.size(); i++) {
                    for (std::uint32_t b = 0; b < size; b++) {
                        bytes[i * size + b] = static_cast<std::uint8_t>(list[i] >> (8 * (size - 1 - b)));
                    }
                }
                return bytes;
            };
            const auto check = [&](const std::vector<std::uint32_t>& list, bool expect) {
                const std::vector<std::uint8_t> src = pack(list);
                const std::uint32_t count = static_cast<std::uint32_t>(list.size());
                std::vector<std::uint8_t> expected(src.size()), got(src.size(), 0xCD);
                tox2::convertIndices(is_u16, src, count, expected);
                const bool in_range = tox2::indicesInRange(is_u16, src, count, kLow, kHigh, restart, restart_index);
                const bool checked =
                        tox2::convertIndicesChecked(is_u16, src, count, kLow, kHigh, restart, restart_index, got);
                CHECK(in_range == expect);
                CHECK(checked == in_range);
                if (checked) CHECK(got == expected);
            };
            check(values, true);

            std::vector<std::uint32_t> first_out = values, last_out = values;
            first_out.front() = kLow - 1;
            last_out.back() = kHigh + 1;
            check(first_out, false);
            check(last_out, false);
            first_out.front() = kHigh + 1;
            last_out.back() = kLow - 1;
            check(first_out, false);
            check(last_out, false);
        }
    }

    // A u16 index never matches a restart value wider than 16 bits, so 0xFFFF is out of range there.
    const std::uint8_t u16_ffff[4] = {0x00, 0x0A, 0xFF, 0xFF};
    std::uint8_t out[4] = {};
    CHECK(!tox2::convertIndicesChecked(true, u16_ffff, 2, 10, 20, true, 0xFFFFFFFF, out));
    CHECK(tox2::convertIndicesChecked(true, u16_ffff, 2, 10, 20, true, 0xFFFF, out) && out[0] == 0x0A && out[1] == 0 &&
          out[2] == 0xFF && out[3] == 0xFF);

    // A source or destination shorter than count indices fails, like indicesInRange on a short source.
    CHECK(!tox2::convertIndicesChecked(true, std::span<const std::uint8_t>(u16_ffff, 3), 2, 0, 0xFFFF, false, 0, out));
    CHECK(!tox2::convertIndicesChecked(true, u16_ffff, 2, 0, 0xFFFF, false, 0, std::span<std::uint8_t>(out, 3)));
    CHECK(tox2::convertIndicesChecked(false, {}, 0, 5, 7, false, 0, {}));
}

void testVertexConvertMatchesReference() {
    std::uint32_t state = 0x12345678u;  // xorshift32, fixed seed
    auto nextByte = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<std::uint8_t>(state >> 11);
    };

    std::vector<tox2::VertexAttribFormat> formats;
    for (std::uint8_t type = 1; type <= 7; type++) {
        for (std::uint8_t size = 1; size <= 4; size++) {
            const tox2::VertexAttribFormat format = sizedFormat(type, size);
            formats.push_back(format);
            // widenedFormat returns the types it does not widen unchanged; force the flag on every type too, which
            // a hand-made format may carry.
            formats.push_back(tox2::widenedFormat(format));
            tox2::VertexAttribFormat forced = format;
            forced.widen = true;
            forced.host_size = 16;
            formats.push_back(forced);
        }
    }
    // Sizes no real format has, which take the unspecialised path.
    formats.push_back(tox2::VertexAttribFormat{});
    tox2::VertexAttribFormat odd = sizedFormat(tox2::kVertexTypeFloat, 3);
    odd.host_size = 16;
    formats.push_back(odd);

    // Counts around the vector paths' read-ahead and unroll boundaries; strides tight, padded, and wider than a
    // 16 byte read.
    const std::uint32_t counts[] = {0, 1, 2, 3, 5, 7, 16, 17, 33, 100};
    std::size_t cases = 0;
    for (const tox2::VertexAttribFormat& format : formats) {
        const std::uint32_t strides[] = {0, format.guest_size, format.guest_size + 1, format.guest_size + 3, 16, 20,
                                         64};
        for (const std::uint32_t stride : strides) {
            for (const std::uint32_t count : counts) {
                const std::uint32_t elements = stride == 0 ? std::min<std::uint32_t>(count, 1) : count;
                const std::size_t exact =
                        elements == 0 ? 0 : static_cast<std::size_t>(elements - 1) * stride + format.guest_size;
                // Exact, one byte short (a missing last element), and empty; the source starts at an odd address.
                const std::size_t sizes[] = {exact, exact == 0 ? 0 : exact - 1, 0};
                for (const std::size_t src_size : sizes) {
                    std::vector<std::uint8_t> backing(src_size + 1);
                    for (std::uint8_t& b : backing) b = nextByte();
                    const std::span<const std::uint8_t> src(backing.data() + 1, src_size);
                    // The destination starts at an offset that is not a multiple of 16, with and without slack
                    // after the converted elements; the bytes around the elements must stay untouched.
                    for (const std::size_t dst_offset : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
                        for (const std::size_t slack : {std::size_t{0}, std::size_t{16}}) {
                            const std::size_t dst_size = static_cast<std::size_t>(count) * format.host_size + slack;
                            std::vector<std::uint8_t> expected(dst_size + dst_offset, 0xCD);
                            std::vector<std::uint8_t> actual(dst_size + dst_offset, 0xCD);
                            const std::uint32_t expected_n = referenceConvert(
                                    format, src, stride, count, std::span<std::uint8_t>(expected).subspan(dst_offset));
                            const std::uint32_t actual_n = tox2::convertVertexElements(
                                    format, src, stride, count, std::span<std::uint8_t>(actual).subspan(dst_offset));
                            CHECK(actual_n == expected_n);
                            CHECK(actual == expected);
                            cases++;
                        }
                    }
                }
            }
        }
    }
    CHECK(cases > 2000);
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

// The viewport covers a square surface of 2 * `half_size` pixels (the 64x64 default surface).
void setQuadRenderState(tox2::rsx_synthetic::SceneBuilder& scene, float half_size = 32.0f) {
    const float scale[4] = {half_size, half_size, 0.5f, 0.0f};
    const float offset[4] = {half_size, half_size, 0.5f, 0.0f};
    for (std::uint32_t i = 0; i < 4; i++) {
        scene.write(tox2::kMethodNv4097SetViewportScale + i * 4, std::bit_cast<std::uint32_t>(scale[i]));
        scene.write(tox2::kMethodNv4097SetViewportOffset + i * 4, std::bit_cast<std::uint32_t>(offset[i]));
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
    ArrayElement,       // the elements of IndexedU16, but with the indices written through the FIFO
    Inline,             // array draw whose vertices (position floats, then the color word) come through the FIFO
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
    // zeros, 1 writes (1, 0, 0, 1), 2 writes 4UB_M, 3 writes 2S_M, 4 writes SCALED4S_M.
    int default_color = -2;  // -2: color comes from the array in slot 3
    int alpha_func = -1;     // -1: alpha test disabled; otherwise the GCM compare enum (0x200 never .. 0x207 always)
    std::uint32_t alpha_ref = 0;
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
        if (options.default_color == 2) {
            scene.write(tox2::kMethodNv4097SetVertexData4ubM + 5 * 4, 0xFF0080FF);
        } else if (options.default_color == 3) {
            scene.write(tox2::kMethodNv4097SetVertexData2sM + 5 * 4, 0x00000001);
        } else if (options.default_color == 4) {
            scene.write(tox2::kMethodNv4097SetVertexDataScaled4sM + 5 * 8, 0x80007FFF);
            scene.write(tox2::kMethodNv4097SetVertexDataScaled4sM + 5 * 8 + 4, 0x7FFF0000);
        } else if (options.default_color >= 0) {
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
    if (options.alpha_func >= 0) {
        scene.write(tox2::kMethodNv4097SetAlphaTestEnable, 1);
        scene.write(tox2::kMethodNv4097SetAlphaFunc, static_cast<std::uint32_t>(options.alpha_func));
        scene.write(tox2::kMethodNv4097SetAlphaRef, options.alpha_ref);
    }
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
        case QuadDraw::ArrayElement: {
            constexpr std::uint32_t kFirst = 10;
            for (std::uint32_t v = 0; v < 4; v++) putVertex(kFirst + v, kQuadCorners[v], kQuadColors[v]);
            beginDraw(tox2::RsxPrimitive::TriangleStrip);
            scene.write(tox2::kMethodNv4097ArrayElement16, ((kFirst + 1) << 16) | kFirst);
            scene.write(tox2::kMethodNv4097ArrayElement32, kFirst + 2);
            scene.write(tox2::kMethodNv4097ArrayElement32, kFirst + 3);
            scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
            break;
        }
        case QuadDraw::Inline:
            beginDraw(tox2::RsxPrimitive::TriangleStrip);
            for (std::uint32_t v = 0; v < 4; v++) {
                for (std::uint32_t c = 0; c < 3; c++) {
                    scene.write(tox2::kMethodNv4097InlineArray, std::bit_cast<std::uint32_t>(kQuadCorners[v][c]));
                }
                scene.write(tox2::kMethodNv4097InlineArray,
                            options.uniform_color != 0 ? options.uniform_color : kQuadColors[v]);
            }
            scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
            break;
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

    for (const QuadDraw draw : {QuadDraw::Strip, QuadDraw::IndexedU16, QuadDraw::Fan, QuadDraw::IndexedU32Restart,
                                QuadDraw::ArrayElement, QuadDraw::Inline}) {
        QuadOptions options;
        options.draw = draw;
        render(options);
        // A fan splits the quad along the other diagonal, so the corner probes mix in a little more of the
        // neighbouring colors.
        checkQuadPixels(bgra, width, height, draw == QuadDraw::Fan ? 45 : 12);
        CHECK(renderer.stats().draws_skipped == 0);
        expectNoNotes();
    }
    CHECK(renderer.stats().draws == 6);

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
    // The integer forms: the first component sits in the low bits of the word.
    defaults.default_color = 2;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFFFF8000);
    defaults.default_color = 3;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFFFF0000);
    defaults.default_color = 4;
    render(defaults);
    CHECK(pixelArgb(bgra, width, 32, 32) == 0xFFFF0000);

    CHECK(renderer.valid());
    tox2::rsxUnimplementedReset();
}

// The alpha test is a specialization constant of the fragment stage, so a draw with the test off and one with it
// on use two pipelines of one shader module. The quad's alpha is 0x80; the 8-bit rounding makes the compare against
// the reference exact.
void testRendererAlphaTest(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    constexpr std::uint32_t kQuadColor = 0x80FFFFFF;
    constexpr std::uint32_t kDrawn = 0x80FFFFFF;  // alpha comes through unblended
    constexpr std::uint32_t kBackground = 0xFF004000;

    const auto render = [&](int alpha_func, std::uint32_t alpha_ref) {
        QuadOptions options;
        options.clear = kBackground;
        options.uniform_color = kQuadColor;
        options.alpha_func = alpha_func;
        options.alpha_ref = alpha_ref;
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeRenderedQuadCapture(options));
        tox2::rsxUnimplementedReset();
        const std::uint64_t created = renderer.pipelineCacheStats().created;
        renderer.execute(commands);
        CHECK(renderer.valid());
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(bgra, width, height));
        CHECK(width == 64 && height == 64 && bgra.size() == 64 * 64 * 4);
        if (bgra.size() != 64 * 64 * 4) bgra.assign(64 * 64 * 4, 0);
        CHECK(pixelArgb(bgra, width, 2, 2) == kBackground);
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
        struct Result {
            bool drawn;
            std::uint64_t pipelines;
        };
        const std::uint32_t center = pixelArgb(bgra, width, 32, 32);
        CHECK(center == kDrawn || center == kBackground);
        return Result{center == kDrawn, renderer.pipelineCacheStats().created - created};
    };

    // The first scene also creates the pipelines shared by all scenes; later ones add only what differs.
    CHECK(render(-1, 0).drawn);
    CHECK(render(-1, 0).pipelines == 0);
    // "Always" is no test, whatever the enable register says: same pipeline, quad drawn.
    const auto always = render(0x207, 0);
    CHECK(always.drawn && always.pipelines == 0);

    const struct {
        std::uint32_t func, ref;
        bool drawn;
    } kCases[] = {
            {0x200, 0x00, false},                                             // never
            {0x200, 0x80, false}, {0x201, 0x81, true},                        // less
            {0x201, 0x80, false}, {0x201, 0x40, false}, {0x202, 0x80, true},  // equal
            {0x202, 0x7F, false}, {0x203, 0x80, true},                        // lequal
            {0x203, 0x7F, false}, {0x204, 0x7F, true},                        // greater
            {0x204, 0x80, false}, {0x205, 0x7F, true},                        // notequal
            {0x205, 0x80, false}, {0x206, 0x80, true},                        // gequal
            {0x206, 0x81, false}, {0x207, 0xFF, true},                        // always
    };
    bool first_test = true;
    for (const auto& test : kCases) {
        const auto result = render(static_cast<int>(test.func), test.ref);
        CHECK(result.drawn == test.drawn);
        if (result.drawn != test.drawn) {
            std::fprintf(stderr, "alpha test func %#x ref %#x\n", test.func, test.ref);
        }
        // The first draw with the test on adds the pipeline of that variant; every other function shares it.
        if (test.func != 0x207) {
            CHECK(result.pipelines == (first_test ? 1u : 0u));
            first_test = false;
        }
    }
    // Back to the test off: its pipeline is still there.
    CHECK(render(-1, 0).pipelines == 0);
    tox2::rsxUnimplementedReset();
}

// An indexed draw whose index lies outside the range the frame builder recorded is skipped and counted. The builder
// derives the range from the same indices, so the draw is edited by hand.
void testIndexOutsideRange(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    QuadOptions options;
    options.draw = QuadDraw::IndexedU16;
    std::vector<tox2::RsxCommand> commands = replaySynthetic(makeRenderedQuadCapture(options));
    tox2::RsxDraw* draw = nullptr;
    for (tox2::RsxCommand& command : commands) {
        if (auto* candidate = std::get_if<tox2::RsxDraw>(&command)) draw = candidate;
    }
    CHECK(draw != nullptr && draw->indexed && draw->count == 4 && draw->index_data.size() == 8);
    if (draw == nullptr || draw->index_data.size() != 8) return;
    CHECK(draw->min_index == 10 && draw->max_index == 13);
    draw->index_data[7] = 14;  // big-endian 0x000E past the recorded maximum
    renderer.execute(commands);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0, height = 0;
    CHECK(renderer.readDisplay(bgra, width, height));
    CHECK(renderer.stats().draws_skipped == 1);
    CHECK(unimplementedCount("state index outside the vertex range") == 1);
    CHECK(bgra.size() == 64 * 64 * 4 && pixelArgb(bgra, width, 32, 32) == 0xFF000000);
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

// Draws that carry one non-zero vertex constants id share one upload within a frame; id 0 and different ids
// upload separately, and a new frame starts without a cached upload.
void testVertexConstantUploadReuse(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());

    // Clear, one quad draw, flip.
    const std::vector<tox2::RsxCommand> scene = replaySynthetic(makeRenderedQuadCapture(QuadOptions{}));
    CHECK(scene.size() == 3);
    if (scene.size() != 3) return;
    const auto* base = std::get_if<tox2::RsxDraw>(&scene[1]);
    CHECK(base != nullptr);
    if (base == nullptr) return;

    // The scene's draw once per id, between the clear and the flip.
    const auto frame = [&](std::initializer_list<std::uint64_t> ids) {
        std::vector<tox2::RsxCommand> commands{scene[0]};
        for (const std::uint64_t id : ids) {
            tox2::RsxDraw draw = *base;
            draw.vp_constants_id = id;
            commands.emplace_back(std::move(draw));
        }
        commands.push_back(scene[2]);
        return commands;
    };
    const auto reusedBy = [&](std::initializer_list<std::uint64_t> ids) {
        std::vector<tox2::RsxCommand> commands = frame(ids);
        const std::uint64_t before = renderer.stats().vp_constant_uploads_reused;
        renderer.execute(commands);
        CHECK(renderer.valid());
        return renderer.stats().vp_constant_uploads_reused - before;
    };

    CHECK(reusedBy({7, 7}) == 1);
    CHECK(reusedBy({7, 7, 7}) == 2);
    CHECK(reusedBy({7, 8}) == 0);
    CHECK(reusedBy({0, 0}) == 0);
    CHECK(reusedBy({7, 8, 7}) == 0);  // only the previous draw's upload is kept
    // The last draw of the previous frame had this id; its upload memory is recycled.
    CHECK(reusedBy({7}) == 0);
    CHECK(reusedBy({7, 7}) == 1);
    CHECK(renderer.stats().draws_skipped == 0);

    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CHECK(renderer.readDisplay(bgra, width, height));
    checkQuadPixels(bgra, width, height);
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
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

    // A B8 image (R8_UNORM) has the base channels {1, byte, byte, byte}: the identity remap reads (R, R, R, ONE).
    CHECK(tox2::textureVkFormat(0x81 | 0x20) == VK_FORMAT_R8_UNORM);
    CHECK(same(tox2::b8ComponentMapping(0xAAE4), kR, kR, kR, kOne));
    // The remap applies on top: alpha reads the byte, green is forced to zero, alpha is forced to zero.
    CHECK(same(tox2::b8ComponentMapping(0xAAE5), kR, kR, kR, kR));
    CHECK(same(tox2::b8ComponentMapping(0x8AE4), kR, kZero, kR, kOne));
    CHECK(same(tox2::b8ComponentMapping(0xA8E4), kR, kR, kR, kZero));
    CHECK(same(tox2::b8ComponentMapping(0x0000), kZero, kZero, kZero, kZero));
    // A colour channel that selects the A source reads the base alpha, which is one.
    CHECK(same(tox2::b8ComponentMapping(0xAAE0), kOne, kR, kR, kOne));

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
    float coord_scale = 1.0f;                  // texture coordinates run 0..coord_scale over the quad
    std::uint32_t pitch = 0;                   // Memory scene: row pitch in bytes (0: no padding); padding holds junk
    bool unnormalized = false;                 // _UN format: coordinates are in texels
    std::uint32_t display_clear = 0xFF000000;  // Alias scene: what the display target is cleared to
    bool split_alias = false;                  // Alias scene: the left half of the aliased target gets kAliasSplitColor
    std::uint32_t dxt = 0;     // Memory scene: base format 0x86/0x87/0x88 for a 4x4 block-compressed texture
    bool zero_height = false;  // Memory scene: the image rect says 0 rows, so no bytes are copied
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
constexpr std::uint32_t kAliasSplitColor = 0xFF99CC33;

// A quad covering pixels 16..48 whose corners carry the texture coordinates (0, 0), (1, 0), (0, 1), (1, 1)
// times coord_scale, in strip order top left, top right, bottom left, bottom right.
std::vector<std::uint8_t> makeTexturedCapture(const TextureOptions& options) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    if (options.scene == TextureScene::Alias) {
        scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kAliasOffset);
        scene.clear(kAliasColor);
        if (options.split_alias) {
            scene.write(tox2::kMethodNv4097SetScissorHorizontal, (kSurfaceSize / 2) << 16);
            scene.write(tox2::kMethodNv4097SetColorClearValue, kAliasSplitColor);
            scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
            scene.write(tox2::kMethodNv4097SetScissorHorizontal, kSurfaceSize << 16);
        }
        scene.write(tox2::kMethodNv4097SetSurfaceColorAOffset, kSurfaceOffset);
        scene.clear(options.display_clear);
        scene.bindTexture(kAliasOffset, kSurfaceSize, kSurfaceSize, 0, options.unnormalized ? 0x40u : 0u);
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

// Draws that sample one render target keep it in shader-read layout: only the first one records a barrier for that
// image, and the draws' own attachment barriers stay.
void testReadBarrierSkipped(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    TextureOptions alias;
    alias.scene = TextureScene::Alias;
    const std::vector<tox2::RsxCommand> scene = replaySynthetic(makeTexturedCapture(alias));
    CHECK(scene.size() == 4);  // clear of the aliased target, clear of the display, the draw, the flip
    if (scene.size() != 4 || std::get_if<tox2::RsxDraw>(&scene[2]) == nullptr) return;

    struct Run {
        tox2::RendererStats stats;
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
    };
    const auto run = [&](std::size_t draws) {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions());
        CHECK(renderer.valid());
        std::vector<tox2::RsxCommand> commands(scene.begin(), scene.begin() + 3);
        for (std::size_t i = 1; i < draws; i++) commands.push_back(scene[2]);
        commands.push_back(scene[3]);
        renderer.execute(commands);
        CHECK(renderer.valid());
        Run result;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(result.bgra, result.width, height));
        result.stats = renderer.stats();
        return result;
    };
    const Run one = run(1);
    const Run two = run(2);
    const Run three = run(3);

    CHECK(one.stats.barriers_skipped == 0);
    CHECK(two.stats.barriers_skipped == 1 && three.stats.barriers_skipped == 2);
    // The later draws continue the first one's rendering instance: no barrier for their attachments, and none for the
    // sampled image either.
    CHECK(two.stats.barriers == one.stats.barriers && three.stats.barriers == one.stats.barriers);
    for (const Run* r : {&one, &two, &three}) {
        CHECK(pixelArgb(r->bgra, r->width, 32, 32) == kAliasColor && pixelArgb(r->bgra, r->width, 2, 2) == 0xFF000000);
        CHECK(r->stats.draws_skipped == 0);
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();
}

// Consecutive draws with the same attachments share one rendering instance; anything recorded outside an instance
// (another target, a clear, a texture upload, a copy of the target the draw samples) ends it first. The instance
// counts are relative to runs with fewer draws, so they do not depend on how clears are recorded (the clear before a
// draw leaves its instance for the draw: testClearPassMerging).
void testDrawPassMerging(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    TextureOptions alias;
    alias.scene = TextureScene::Alias;
    const std::vector<tox2::RsxCommand> scene = replaySynthetic(makeTexturedCapture(alias));
    CHECK(scene.size() == 4);  // clear of the aliased target, clear of the display, the draw, the flip
    if (scene.size() != 4 || std::get_if<tox2::RsxDraw>(&scene[2]) == nullptr) return;
    const tox2::RsxCommand& clear_alias = scene[0];
    const tox2::RsxCommand& clear_display = scene[1];
    const tox2::RsxCommand& sampling_draw = scene[2];  // draws to the display, samples the aliased target
    const tox2::RsxCommand& flip = scene[3];

    struct Run {
        tox2::RendererStats stats;
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
    };
    const auto run = [&](const std::vector<tox2::RsxCommand>& commands) {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions());
        CHECK(renderer.valid());
        std::vector<tox2::RsxCommand> copy = commands;
        renderer.execute(copy);
        CHECK(renderer.valid());
        Run result;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(result.bgra, result.width, height));
        result.stats = renderer.stats();
        CHECK(result.stats.draws_skipped == 0);
        return result;
    };
    // The same draw drawing into the aliased target instead of the display.
    const auto retarget = [](tox2::RsxCommand command) {
        std::get<tox2::RsxDraw>(command).surfaces.color.offset = kAliasOffset;
        return command;
    };

    // Draws to one target add one instance in all, and no barrier after the first.
    {
        const auto draws = [&](std::size_t count) {
            std::vector<tox2::RsxCommand> commands = {clear_alias, clear_display};
            for (std::size_t i = 0; i < count; i++) commands.push_back(sampling_draw);
            commands.push_back(flip);
            return run(commands);
        };
        const Run one = draws(1);
        const Run two = draws(2);
        const Run three = draws(3);
        CHECK(two.stats.rendering_instances == one.stats.rendering_instances);
        CHECK(three.stats.rendering_instances == one.stats.rendering_instances);
        CHECK(two.stats.barriers == one.stats.barriers && three.stats.barriers == one.stats.barriers);
        for (const Run* r : {&one, &two, &three}) {
            CHECK(pixelArgb(r->bgra, r->width, 32, 32) == kAliasColor &&
                  pixelArgb(r->bgra, r->width, 2, 2) == 0xFF000000);
        }
    }

    // A draw to another target is another instance, and so is the draw back to the first one.
    {
        const std::vector<tox2::RsxCommand> memory_scene = replaySynthetic(makeTexturedCapture({}));
        CHECK(memory_scene.size() == 3);  // clear, the draw, the flip
        if (memory_scene.size() != 3 || std::get_if<tox2::RsxDraw>(&memory_scene[1]) == nullptr) return;
        const tox2::RsxCommand& draw = memory_scene[1];
        const tox2::RsxCommand other = retarget(draw);
        const auto sequence = [&](std::initializer_list<const tox2::RsxCommand*> draws) {
            std::vector<tox2::RsxCommand> commands = {clear_alias, clear_display};
            for (const tox2::RsxCommand* d : draws) commands.push_back(*d);
            commands.push_back(flip);
            return run(commands);
        };
        const Run same = sequence({&draw, &draw});
        const Run switched = sequence({&draw, &draw, &other});
        const Run back = sequence({&draw, &other, &draw});
        CHECK(switched.stats.rendering_instances == same.stats.rendering_instances + 1);
        CHECK(back.stats.rendering_instances == same.stats.rendering_instances + 2);
        // The display shows the textured quad of the draws that went to it, whatever the order.
        for (const Run* r : {&same, &switched, &back}) CHECK(pixelArgb(r->bgra, r->width, 2, 2) != 0);
        CHECK(switched.bgra == same.bgra && back.bgra == same.bgra);
    }

    // A clear between two draws to the same target ends the first instance and begins one for the second draw.
    {
        const Run split = run({clear_alias, clear_display, sampling_draw, clear_display, sampling_draw, flip});
        const Run merged = run({clear_alias, clear_display, sampling_draw, sampling_draw, flip});
        // The clear before the second draw begins an instance with the depth attached. The draw tests no depth and
        // binds none, so it begins an instance of its own: two more than without the clear.
        CHECK(split.stats.rendering_instances == merged.stats.rendering_instances + 2);
        CHECK(pixelArgb(split.bgra, split.width, 32, 32) == kAliasColor &&
              pixelArgb(split.bgra, split.width, 2, 2) == 0xFF000000);
    }

    // A draw that samples its own target copies it first, which cannot happen inside the instance: each such draw
    // gets an instance of its own, and the draws after them still read what they wrote.
    {
        const tox2::RsxCommand feedback = retarget(sampling_draw);
        const Run base = run({clear_alias, clear_display, sampling_draw, flip});
        const Run twice = run({clear_alias, clear_display, feedback, feedback, sampling_draw, flip});
        CHECK(twice.stats.feedback_copies == 2);
        CHECK(twice.stats.rendering_instances == base.stats.rendering_instances + 2);
        CHECK(pixelArgb(twice.bgra, twice.width, 32, 32) == kAliasColor &&
              pixelArgb(twice.bgra, twice.width, 2, 2) == 0xFF000000);
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();

    // A texture upload between two draws to one target ends the instance: the second draw has new texels.
    {
        const std::vector<tox2::RsxCommand> changed = replaySynthetic(makeChangedTextureCapture());
        CHECK(changed.size() == 4);  // clear, two draws, the flip
        if (changed.size() != 4) return;
        const Run result = run(changed);
        CHECK(result.stats.texture_uploads == 2);
        CHECK(result.stats.rendering_instances ==
              run({changed[0], changed[1], changed[3]}).stats.rendering_instances + 1);
        CHECK(pixelArgb(result.bgra, result.width, 10, 24) == 0xFFFF0000 &&
              pixelArgb(result.bgra, result.width, 22, 40) == 0x80FFFFFF);
        CHECK(pixelArgb(result.bgra, result.width, 42, 24) == 0xFFFFFF00 &&
              pixelArgb(result.bgra, result.width, 54, 40) == 0xFF808080);
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();
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

// The textured pass of both scenes: unit 0 reads `offset` as a `size` x `size` linear texture of the base format
// `format`.
void setSurfaceTextureState(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t offset, std::uint32_t format,
                            std::uint32_t size = 64) {
    scene.bindTexture(offset, size, size);
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
constexpr std::uint32_t kDepthFar = 0xFFFFFF, kStencilFar = 0x3C;  // depth 1.0 must not round up to 2^24
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
// texture, then a clear of depth and stencil alone sets the second value and a small quad (28..36) reads it, and a
// third such clear sets the far plane, read by a quad at 52..60.
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
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, (kDepthFar << 8) | kStencilFar);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    putSurfaceQuad(scene, 52, 60);
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
    CHECK(pixelArgb(bgra, width, 56, 32) == depthWordArgb(kDepthFar, kStencilFar));
    CHECK(pixelArgb(bgra, width, 2, 2) == 0xFF000000);
    // The surface is rewritten once for the two draws that read it unchanged, and again after each clear.
    CHECK(renderer.stats().depth_conversions == 3);
    CHECK(renderer.stats().draws == 4 && renderer.stats().draws_skipped == 0);
    for (const auto& [name, count] : tox2::rsxUnimplementedSnapshot()) {
        std::fprintf(stderr, "unimplemented: %s: %llu\n", name.c_str(), static_cast<unsigned long long>(count));
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    tox2::rsxUnimplementedReset();
}

// One depth surface bound at 64x64 and then at 128x128 by depth-only clears, every frame: each binding replaces the
// target the other one left at the same address, so two targets are created per frame.
std::vector<std::uint8_t> makeDepthResizeCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface(0, 64);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, 0xFFFFFF00u);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    scene.setSurface(0, 128);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    return scene.finish();
}

// ImagePool: FIFO reuse, the per-key and byte caps, the idle trim and the reasons a take misses.
void testImagePool(tox2::VkContext& ctx) {
    using tox2::ImagePoolMiss;
    using tox2::PooledImageKey;
    constexpr std::uint64_t kBytes = 1024;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    struct Made {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
    };
    auto makeKey = [](std::uint32_t width) {
        return PooledImageKey{VK_FORMAT_B8G8R8A8_UNORM, width, 4, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    };
    auto make = [&](const PooledImageKey& key) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = key.format;
        image_info.extent = {key.width, key.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = key.samples;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = key.usage;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        Made made;
        CHECK(vmaCreateImage(ctx.allocator(), &image_info, &alloc_info, &made.image, &made.allocation, nullptr) ==
              VK_SUCCESS);
        return made;
    };

    tox2::ImagePool pool(ctx, tox2::ImagePoolLimits{2, 3 * kBytes, 5});
    const PooledImageKey key_a = makeKey(4);
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    // An empty pool: nothing was ever given.
    tox2::ImagePoolResult result = pool.take(key_a, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::NoEntry);

    // Three gives of one key under a cap of two: the first is destroyed, the rest leave oldest first.
    const Made a1 = make(key_a), a2 = make(key_a), a3 = make(key_a);
    pool.give(key_a, kBytes, a1.image, a1.allocation);
    pool.give(key_a, kBytes, a2.image, a2.allocation);
    pool.give(key_a, kBytes, a3.image, a3.allocation);
    CHECK(pool.discards().key_cap == 1 && pool.discards().byte_cap == 0 && pool.discards().idle == 0);
    CHECK(pool.entries() == 2 && pool.bytes() == 2 * kBytes);
    result = pool.take(key_a, image, allocation);
    CHECK(result.hit && image == a2.image && allocation == a2.allocation);
    result = pool.take(key_a, image, allocation);
    CHECK(result.hit && image == a3.image);
    result = pool.take(key_a, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::KeyCap);
    result = pool.take(key_a, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::NoEntry);
    CHECK(pool.entries() == 0 && pool.bytes() == 0);
    vmaDestroyImage(ctx.allocator(), a2.image, a2.allocation);
    vmaDestroyImage(ctx.allocator(), a3.image, a3.allocation);

    // Four keys of one image each against a budget of three: the oldest (B) goes.
    const PooledImageKey key_b = makeKey(5), key_c = makeKey(6), key_d = makeKey(7), key_e = makeKey(8);
    const PooledImageKey keys[] = {key_b, key_c, key_d, key_e};
    Made made[4];
    for (int i = 0; i < 4; i++) {
        made[i] = make(keys[i]);
        pool.give(keys[i], kBytes, made[i].image, made[i].allocation);
    }
    CHECK(pool.discards().byte_cap == 1 && pool.discards().key_cap == 1);
    CHECK(pool.entries() == 3 && pool.bytes() == 3 * kBytes);
    result = pool.take(key_b, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::ByteCap);
    result = pool.take(key_c, image, allocation);
    CHECK(result.hit && image == made[1].image);
    // Put it back: the pool holds D, E, C again, stamped with the flip of the last trim.
    pool.give(key_c, kBytes, made[1].image, made[1].allocation);
    CHECK(pool.entries() == 3);

    // Entries given at flip 0 are kept through flip 5 and destroyed at flip 6.
    pool.trim(0);
    pool.trim(5);
    CHECK(pool.entries() == 3);
    pool.trim(6);
    CHECK(pool.entries() == 0 && pool.bytes() == 0);
    CHECK(pool.discards().idle == 3);
    result = pool.take(key_d, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::Idle);
    result = pool.take(key_d, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::NoEntry);

    // An image larger than the whole budget is not kept and does not evict the others.
    const Made kept = make(key_a);
    pool.give(key_a, kBytes, kept.image, kept.allocation);
    const Made huge = make(key_b);
    pool.give(key_b, 4 * kBytes, huge.image, huge.allocation);
    CHECK(pool.entries() == 1 && pool.bytes() == kBytes && pool.discards().byte_cap == 2);
    result = pool.take(key_b, image, allocation);
    CHECK(!result.hit && result.miss == ImagePoolMiss::ByteCap);

    // The destructor destroys what is left (a leak would show in the allocator on context teardown).
    CHECK(ctx.validationErrorCount() == errors_before);
}

// A mode-3 colour surface bound at 32x32 and then, at the same address, as a plain 64x64 one by colour-only clears,
// every frame: two targets and one 2-sample sidecar are needed per frame.
std::vector<std::uint8_t> makeSidecarResizeCapture() {
    using namespace tox2::rsx_synthetic;
    constexpr std::uint32_t kOffset = 0x40000;
    SceneBuilder scene;
    scene.setSurface(3, 32, kOffset);
    scene.write(tox2::kMethodNv4097SetColorClearValue, 0xFF000000);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
    scene.setSurface(0, 64, kOffset);
    scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
    return scene.finish();
}

// Retired images come back after their slot's fence (two frames later), so creations stop growing after the first
// frames and the pool serves every later request.
void testRendererImageCreationCounters(tox2::VkContext& ctx) {
    using tox2::RenderImageKind;
    constexpr std::uint64_t kFrames = 12, kSteadyFrom = 7;
    const auto target = static_cast<std::size_t>(RenderImageKind::Target);
    const auto sidecar = static_cast<std::size_t>(RenderImageKind::Sidecar);
    const std::uint32_t errors_before = ctx.validationErrorCount();

    const auto run = [&](const std::vector<std::uint8_t>& capture, std::size_t pooled_kind,
                         std::uint64_t hits_per_frame) {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions());
        CHECK(renderer.valid());
        tox2::RendererStats before_steady;
        for (std::uint64_t frame = 1; frame <= kFrames; frame++) {
            std::vector<tox2::RsxCommand> commands = replaySynthetic(capture);
            tox2::rsxUnimplementedReset();
            renderer.execute(commands);
            CHECK(renderer.valid());
            const tox2::RendererStats& stats = renderer.stats();
            std::uint64_t misses = 0;
            for (const std::uint64_t count : stats.image_pool_misses) misses += count;
            CHECK(misses == stats.images_created[target] + stats.images_created[sidecar]);
            for (std::size_t kind = 0; kind < tox2::kRenderImageKindCount; kind++) {
                if (kind != target && kind != sidecar) {
                    CHECK(stats.images_created[kind] == 0 && stats.image_bytes_created[kind] == 0);
                    CHECK(stats.image_pool_hits[kind] == 0);
                }
            }
            if (frame == kSteadyFrom) before_steady = stats;
            if (frame > kSteadyFrom) {
                // The last 5 frames: nothing new is created and each frame's requests are all hits.
                CHECK(stats.images_created[pooled_kind] == before_steady.images_created[pooled_kind]);
                CHECK(stats.image_pool_hits[pooled_kind] ==
                      before_steady.image_pool_hits[pooled_kind] + hits_per_frame * (frame - kSteadyFrom));
            }
        }
        CHECK(renderer.stats().flips == kFrames);
        CHECK(renderer.stats().image_pool_discards.key_cap == 0 && renderer.stats().image_pool_discards.byte_cap == 0);
        return renderer.stats();
    };

    const std::uint64_t texel = ctx.depthStencilFormat() == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8 : 4;
    const tox2::RendererStats depth = run(makeDepthResizeCapture(), target, 2);
    CHECK(depth.image_bytes_created[target] == depth.images_created[target] / 2 * (64 * 64 + 128 * 128) * texel);
    CHECK(depth.image_pool_hits[target] + depth.images_created[target] == 2 * kFrames);

    if (ctx.twoSampleSupportGap().empty()) {
        const tox2::RendererStats msaa = run(makeSidecarResizeCapture(), sidecar, 1);
        CHECK(msaa.images_created[sidecar] > 0 && msaa.image_pool_hits[sidecar] > 0);
        CHECK(msaa.image_pool_hits[target] > 0);
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// The depth surface of makeDepthResizeCapture, with a draw at 128x128 that reads it as an A8R8G8B8 texture: each frame
// needs one depth-as-ARGB image, and the image of the target the 64x64 binding replaced goes back to the pool.
std::vector<std::uint8_t> makeDepthArgbResizeCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface(0, 64);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, 0xFFFFFF00u);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    scene.setSurface(0, 128);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
    setQuadRenderState(scene, 64.0f);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    setSurfaceTextureState(scene, kDepthOffset, 0x85, 128);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
    return scene.finish();
}

void testRendererDepthArgbPool(tox2::VkContext& ctx) {
    using tox2::RenderImageKind;
    constexpr std::uint64_t kFrames = 12, kSteadyFrom = 7;
    const auto argb = static_cast<std::size_t>(RenderImageKind::DepthArgb);
    const std::uint32_t errors_before = ctx.validationErrorCount();

    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    tox2::RendererStats before_steady;
    for (std::uint64_t frame = 1; frame <= kFrames; frame++) {
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeDepthArgbResizeCapture());
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        CHECK(renderer.valid());
        const tox2::RendererStats& stats = renderer.stats();
        CHECK(stats.depth_conversions == frame);
        std::uint64_t misses = 0;
        for (const std::uint64_t count : stats.image_pool_misses) misses += count;
        CHECK(misses ==
              stats.images_created[static_cast<std::size_t>(RenderImageKind::Target)] + stats.images_created[argb]);
        if (frame == kSteadyFrom) before_steady = stats;
        if (frame > kSteadyFrom) {
            // The last 5 frames: the one image each frame needs comes from the pool.
            CHECK(stats.images_created[argb] == before_steady.images_created[argb]);
            CHECK(stats.image_pool_hits[argb] == before_steady.image_pool_hits[argb] + (frame - kSteadyFrom));
        }
    }
    CHECK(renderer.stats().images_created[argb] > 0);
    CHECK(renderer.stats().image_bytes_created[argb] == renderer.stats().images_created[argb] * 128 * 128 * 4);
    CHECK(renderer.stats().image_pool_hits[argb] + renderer.stats().images_created[argb] == kFrames);
    CHECK(ctx.validationErrorCount() == errors_before);
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

// The AA field of SET_SURFACE_FORMAT selects the samples per pixel of the colour and depth layouts, and
// SET_ANTI_ALIASING_CONTROL decodes into the raster state.
void testAntiAliasDecode() {
    tox2::rsxUnimplementedReset();
    auto regs = std::make_unique<tox2::RsxRegisters>();
    regs->reset();
    regs->apply(tox2::kMethodNv4097SetSurfaceClipHorizontal, 64u << 16);
    regs->apply(tox2::kMethodNv4097SetSurfaceClipVertical, 32u << 16);
    regs->apply(tox2::kMethodNv4097SetSurfaceColorTarget, 1);  // target A

    const auto decodeMode = [&](std::uint32_t mode) {
        regs->apply(tox2::kMethodNv4097SetSurfaceFormat, 8u | (2u << 5) | (1u << 8) | (mode << 12));
        return tox2::decodeSurfaceState(*regs);
    };
    const struct {
        std::uint32_t mode;
        std::uint8_t samples_x, samples_y;
    } cases[] = {{0, 1, 1}, {3, 2, 1}, {4, 2, 2}, {5, 2, 2}, {1, 1, 1}};
    for (const auto& c : cases) {
        const tox2::RsxSurfaceState state = decodeMode(c.mode);
        CHECK(state.aa_mode == c.mode);
        CHECK(state.color.samples_x == c.samples_x && state.color.samples_y == c.samples_y);
        CHECK(state.depth.samples_x == c.samples_x && state.depth.samples_y == c.samples_y);
        // The clip size stays in pixels.
        CHECK(state.color.width == 64 && state.color.height == 32 && state.depth.width == 64);
    }

    // The modes the renderer draws are not counted; the others are.
    CHECK(unimplementedCount("state antialiasing 0x1") == 1);
    CHECK(unimplementedCount("state antialiasing 0x3") == 0 && unimplementedCount("state antialiasing 0x4") == 0 &&
          unimplementedCount("state antialiasing 0x5") == 0);

    regs->apply(tox2::kMethodNv4097SetAntiAliasingControl, 0xFFFF0001);
    tox2::RsxRasterState raster = tox2::decodeRasterState(*regs);
    CHECK(raster.msaa && !raster.alpha_to_coverage && !raster.alpha_to_one && raster.sample_mask == 0xFFFF);
    regs->apply(tox2::kMethodNv4097SetAntiAliasingControl, 0xFFFF0000);
    raster = tox2::decodeRasterState(*regs);
    CHECK(!raster.msaa && raster.sample_mask == 0xFFFF);
    regs->apply(tox2::kMethodNv4097SetAntiAliasingControl, 0x00A50111);
    raster = tox2::decodeRasterState(*regs);
    CHECK(raster.msaa && raster.alpha_to_coverage && raster.alpha_to_one && raster.sample_mask == 0x00A5);
    tox2::rsxUnimplementedReset();
}

// At scale 1 scaleRect is the clamp the renderer applied to scissors and clear rectangles before the
// resolution scale existed; larger scales multiply the guest rectangle and clamp it to the image area.
void testScaleRect() {
    const auto oldClamp = [](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t limit_w,
                             std::uint32_t limit_h) {
        tox2::ImageRect rect;
        rect.x = std::min(x, limit_w);
        rect.y = std::min(y, limit_h);
        rect.width = std::min(w, limit_w - rect.x);
        rect.height = std::min(h, limit_h - rect.y);
        return rect;
    };
    const auto same = [](const tox2::ImageRect& a, const tox2::ImageRect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    constexpr std::uint32_t kMax = 0xFFFFFFFFu;
    const std::uint32_t values[] = {0, 1, 7, 64, 100, 1279, 1280, 1281, 4096, kMax};
    for (const std::uint32_t x : values) {
        for (const std::uint32_t w : values) {
            const tox2::ImageRect got = tox2::scaleRect(x, x, w, w, {1, 1}, 1280, 720);
            CHECK(same(got, oldClamp(x, x, w, w, 1280, 720)));
        }
    }

    // Inside, crossing the edge, starting outside, zero size, at scale 1.
    CHECK(same(tox2::scaleRect(10, 20, 30, 40, {1, 1}, 100, 100), {10, 20, 30, 40}));
    CHECK(same(tox2::scaleRect(80, 90, 50, 50, {1, 1}, 100, 100), {80, 90, 20, 10}));
    CHECK(same(tox2::scaleRect(150, 120, 10, 10, {1, 1}, 100, 100), {100, 100, 0, 0}));
    CHECK(same(tox2::scaleRect(10, 10, 0, 0, {1, 1}, 100, 100), {10, 10, 0, 0}));

    // Scales 2 and 3 on the same cases; the limits are image pixels.
    CHECK(same(tox2::scaleRect(10, 20, 30, 40, {2, 2}, 200, 200), {20, 40, 60, 80}));
    CHECK(same(tox2::scaleRect(10, 20, 30, 40, {3, 3}, 300, 300), {30, 60, 90, 120}));
    CHECK(same(tox2::scaleRect(80, 90, 50, 50, {2, 2}, 200, 200), {160, 180, 40, 20}));
    CHECK(same(tox2::scaleRect(80, 90, 50, 50, {3, 3}, 300, 300), {240, 270, 60, 30}));
    CHECK(same(tox2::scaleRect(150, 120, 10, 10, {2, 2}, 200, 200), {200, 200, 0, 0}));
    CHECK(same(tox2::scaleRect(10, 10, 0, 0, {3, 3}, 300, 300), {30, 30, 0, 0}));
    // The factors are per axis (an anti-aliased layout is wider than tall).
    CHECK(same(tox2::scaleRect(5, 5, 10, 10, {2, 1}, 100, 100), {10, 5, 20, 10}));

    // Products that exceed 32 bits clamp instead of wrapping.
    CHECK(same(tox2::scaleRect(kMax, kMax, kMax, kMax, {4, 4}, 1280, 720), {1280, 720, 0, 0}));
    CHECK(same(tox2::scaleRect(0, 0, 0x80000000u, 0x80000000u, {4, 4}, 1280, 720), {0, 0, 1280, 720}));
    CHECK(same(tox2::scaleRect(0x40000000u, 1, 0x40000000u, 1, {4, 4}, 1280, 720), {1280, 4, 0, 4}));
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
    std::uint32_t scale = 1;  // the renderer's resolution scale; width and height are in image pixels
    // The pixel at guest position (x, y): the centre of its block of image pixels.
    std::uint32_t pixel(std::uint32_t x, std::uint32_t y) const {
        return pixelArgb(bgra, width, x * scale + scale / 2, y * scale + scale / 2);
    }
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

// renderStencilScene on a renderer that has already drawn once: the placeholder textures a renderer creates for its
// first draw are recorded outside an instance and would end the one a clear left open. `stats.rendering_instances` is
// the count of the scene alone.
StencilRender renderStencilSceneWarm(tox2::VkContext& ctx, tox2::rsx_synthetic::SceneBuilder& scene) {
    tox2::RsxRenderer renderer(ctx, readbackAllOptions());
    CHECK(renderer.valid());
    tox2::rsx_synthetic::SceneBuilder warm_up;
    beginStencilScene(warm_up, 0);
    stencilQuad(warm_up, stencilFace(kCmpAlways, 0, kOpKeep));
    std::vector<tox2::RsxCommand> warm_commands = replaySynthetic(warm_up.finish());
    renderer.execute(warm_commands);
    const std::uint64_t instances_before = renderer.stats().rendering_instances;
    std::vector<tox2::RsxCommand> commands = replaySynthetic(scene.finish());
    tox2::rsxUnimplementedReset();
    renderer.execute(commands);
    CHECK(renderer.valid());
    StencilRender result;
    CHECK(renderer.readDisplay(result.bgra, result.width, result.height));
    CHECK(result.width == 64 && result.height == 64 && result.bgra.size() == 64 * 64 * 4);
    if (result.bgra.size() != 64 * 64 * 4) result.bgra.assign(64 * 64 * 4, 0);
    result.stats = renderer.stats();
    result.stats.rendering_instances -= instances_before;
    return result;
}

// A clear over the whole render area clears through the attachments' load operations and leaves its instance open for
// the draws that follow; a clear of a rectangle, or of only some aspects, keeps loading what it does not replace.
// Instance counts are relative to the same scene without the draw, which does not add one.
void testClearPassMerging(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    constexpr std::uint32_t kClearA = 0xFF102030;
    constexpr std::uint32_t kClearB = 0xFF405060;
    const auto fullScissor = [](SceneBuilder& scene) {
        scene.write(tox2::kMethodNv4097SetScissorHorizontal, kSurfaceSize << 16);
        scene.write(tox2::kMethodNv4097SetScissorVertical, kSurfaceSize << 16);
    };
    const StencilFaceWords always = stencilFace(kCmpAlways, 0, kOpKeep);

    // A full clear of colour, depth and stencil, then a draw to the same target: one instance in all.
    {
        SceneBuilder with_draw;
        beginStencilScene(with_draw, 0);
        stencilQuad(with_draw, always);
        SceneBuilder clear_only;
        beginStencilScene(clear_only, 0);
        const StencilRender merged = renderStencilSceneWarm(ctx, with_draw);
        const StencilRender base = renderStencilSceneWarm(ctx, clear_only);
        CHECK(merged.stats.rendering_instances == base.stats.rendering_instances);
        expectStencilDrawn(merged, true);
        CHECK(base.pixel(32, 32) == kBlack);
        expectNoRendererNotes();
    }

    // A clear of a rectangle after a full one loads the target and clears the rectangle; the draw continues it.
    {
        const auto scene_of = [&](bool draw) {
            SceneBuilder scene;
            beginStencilScene(scene, 0);
            scene.write(tox2::kMethodNv4097SetScissorHorizontal, 4u | (8u << 16));
            scene.write(tox2::kMethodNv4097SetScissorVertical, 4u | (8u << 16));
            scene.write(tox2::kMethodNv4097SetColorClearValue, kClearB);
            scene.write(tox2::kMethodNv4097ClearSurface, 0xF3);
            fullScissor(scene);
            if (draw) stencilQuad(scene, always);
            return scene;
        };
        SceneBuilder with_draw = scene_of(true);
        SceneBuilder clear_only = scene_of(false);
        const StencilRender merged = renderStencilSceneWarm(ctx, with_draw);
        const StencilRender base = renderStencilSceneWarm(ctx, clear_only);
        CHECK(merged.stats.rendering_instances == base.stats.rendering_instances);
        CHECK(merged.pixel(6, 6) == kClearB && merged.pixel(2, 2) == kBlack && merged.pixel(12, 6) == kBlack);
        CHECK(merged.pixel(32, 32) == kQuadRed);
        CHECK(base.pixel(6, 6) == kClearB && base.pixel(32, 32) == kBlack);
        expectNoRendererNotes();
    }

    // A full clear of the depth only keeps the stencil of a Z24S8 target (the stencil test passes where the old
    // value is) and replaces the depth (a greater-than test against the cleared 0.25 passes where the old 1.0 would
    // fail). The stencil-only clear is the same the other way round.
    for (const bool depth_only : {true, false}) {
        for (const bool expect_drawn : {true, false}) {
            SceneBuilder scene;
            beginStencilScene(scene, 0x5A);
            scene.write(tox2::kMethodNv4097SetZstencilClearValue, depth_only ? 0x40000077u : 0x40000000u | 0x77u);
            scene.write(tox2::kMethodNv4097ClearSurface, depth_only ? 0x01u : 0x02u);
            scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);
            // The quad is at depth 0.5. Depth cleared to 0.25: GREATER passes; depth kept at 1.0: LESS passes.
            scene.write(tox2::kMethodNv4097SetDepthFunc, depth_only ? 0x204u : 0x201u);
            const std::uint32_t kept_stencil = depth_only ? 0x5Au : 0x77u;
            stencilQuad(scene, stencilFace(kCmpEqual, expect_drawn ? kept_stencil : kept_stencil ^ 1u, kOpKeep));
            const StencilRender render = renderStencilSceneWarm(ctx, scene);
            expectStencilDrawn(render, expect_drawn);
            expectNoRendererNotes();
        }
    }

    // A clear, then a draw to another target: the clear is stored, and the draw is an instance of its own.
    {
        const auto scene_of = [&](int draws) {
            SceneBuilder scene;
            beginStencilScene(scene, 0);
            scene.write(tox2::kMethodNv4097SetColorClearValue, kClearA);
            scene.write(tox2::kMethodNv4097ClearSurface, 0xF3);
            if (draws >= 1) {
                scene.setSurface(0, kSurfaceSize, 0x50000);  // another colour target (and depth target)
                stencilQuad(scene, always);
            }
            if (draws >= 2) {
                scene.setSurface();  // back to the display target
                stencilQuad(scene, always);
            }
            return scene;
        };
        SceneBuilder none = scene_of(0), other = scene_of(1), back = scene_of(2);
        const StencilRender base = renderStencilSceneWarm(ctx, none);
        const StencilRender elsewhere = renderStencilSceneWarm(ctx, other);
        const StencilRender returned = renderStencilSceneWarm(ctx, back);
        CHECK(elsewhere.stats.rendering_instances == base.stats.rendering_instances + 1);
        CHECK(returned.stats.rendering_instances == base.stats.rendering_instances + 2);
        for (const StencilRender* r : {&base, &elsewhere}) {
            CHECK(r->pixel(2, 2) == kClearA && r->pixel(32, 32) == kClearA && r->pixel(63, 63) == kClearA);
        }
        CHECK(returned.pixel(32, 32) == kQuadRed && returned.pixel(2, 2) == kClearA);
        CHECK(returned.stats.draws_skipped == 0 && elsewhere.stats.draws_skipped == 0);
        expectNoRendererNotes();
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
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

// ---- Vertex program condition codes ----

// Patches of an instruction built by vpMove: the condition and the test enable (bit 13) of a conditional
// write, and the update enable (bit 14) together with a temp destination in place of the default "no temp".
std::array<std::uint32_t, 4> vpWithCondition(std::array<std::uint32_t, 4> words, std::uint32_t condition) {
    words[0] = (words[0] & ~(7u << 10)) | (condition << 10) | (1u << 13);
    return words;
}

std::array<std::uint32_t, 4> vpUpdatesCondition(std::array<std::uint32_t, 4> words, std::uint32_t temp) {
    words[0] = (words[0] & ~(63u << 15)) | (temp << 15) | (1u << 14);
    return words;
}

// The quad's position, an unconditional red front diffuse, then a green one written only where the condition
// register is greater than zero. The register is set by a MOV of default attribute v5 (all lanes `value`) that
// has no output: with no temp it writes the register (bit 14), with a temp it writes the temp and the register
// stays at its initial zero.
void testRendererConditionCode(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    const auto render = [&](float value, std::uint32_t update_temp) {
        SceneBuilder scene;
        scene.setSurface();
        scene.write(tox2::kMethodNv4097SetColorClearValue, kBlack);
        scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
        setQuadRenderState(scene);
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        const float colors[3][4] = {{value, value, value, value}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
        for (std::uint32_t slot = 5; slot <= 7; slot++) {
            for (std::uint32_t i = 0; i < 4; i++) {
                scene.write(tox2::kMethodNv4097SetVertexData4fM + slot * 16 + i * 4,
                            std::bit_cast<std::uint32_t>(colors[slot - 5][i]));
            }
        }
        Programs programs = quadPrograms();
        programs.vertex.clear();
        const std::array<std::array<std::uint32_t, 4>, 4> instructions = {
                vpMove(0, 0, "xyzw", false),
                vpUpdatesCondition(vpMove(31, 5, "xyzw", false), update_temp),
                vpMove(1, 6, "xyzw", false),
                vpWithCondition(vpMove(1, 7, "xyzw", true), 4 /* GT */),
        };
        for (const auto& instruction : instructions) {
            programs.vertex.insert(programs.vertex.end(), instruction.begin(), instruction.end());
        }
        scene.setPrograms(programs);
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
        scene.write(tox2::kMethodNv4097SetDepthMask, 0);
        scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
        putSurfaceQuad(scene, 16, 48);
        scene.drawStrip(4);
        const StencilRender result = renderStencilScene(ctx, scene);
        CHECK(result.stats.draws == 1 && result.stats.draws_skipped == 0);
        CHECK(result.pixel(2, 2) == kBlack);
        expectNoRendererNotes();
        return result.pixel(32, 32);
    };

    constexpr std::uint32_t kRed = 0xFFFF0000, kGreen = 0xFF00FF00;
    // The register is written: the second write lands for a positive value and is skipped for a negative one.
    CHECK(render(1.0f, 63) == kGreen);
    CHECK(render(-1.0f, 63) == kRed);
    // The MOV has a temp, so RPCS3's rule writes the temp and leaves the register at zero: never greater.
    CHECK(render(1.0f, 0) == kRed);
    CHECK(render(-1.0f, 0) == kRed);
    tox2::rsxUnimplementedReset();
}

// ---- Surface resize ----

constexpr std::uint32_t kResizeClear = 0xFF102030;

// A 64x64 target (and Z24S8 unless depth_code is 0 or 1, as in beginStencilScene), cleared, with the quad
// programs and depth test off.
void beginResizeScene(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t depth_code = 0) {
    using namespace tox2::rsx_synthetic;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetSurfaceFormat, 8u | (depth_code << 5) | (1u << 8));
    scene.write(tox2::kMethodNv4097SetColorClearValue, kResizeClear);
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, 0xFFFFFF00u);
    scene.write(tox2::kMethodNv4097ClearSurface, depth_code == 0 ? 0xF0 : 0xF3);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
    scene.write(tox2::kMethodNv4097SetDepthMask, 0);
}

// Binds the same offsets as a square surface of `size` pixels; `pitch` is the colour row pitch.
void rebindSurface(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t size, std::uint32_t pitch) {
    scene.write(tox2::kMethodNv4097SetSurfaceClipHorizontal, size << 16);
    scene.write(tox2::kMethodNv4097SetSurfaceClipVertical, size << 16);
    scene.write(tox2::kMethodNv4097SetSurfacePitchA, pitch);
}

// A draw that writes nothing: the renderer still acquires the colour target the draw binds, and the depth target when
// the draw tests depth.
void bindingDraw(tox2::rsx_synthetic::SceneBuilder& scene) {
    scene.write(tox2::kMethodNv4097SetColorMask, 0);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
}

void colorDraw(tox2::rsx_synthetic::SceneBuilder& scene) {
    scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
}

// The scene on a fresh renderer (so the statistics belong to it) with the display of any size.
StencilRender renderResizeScene(tox2::VkContext& ctx, tox2::rsx_synthetic::SceneBuilder& scene,
                                std::uint32_t scale = 1) {
    tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale));
    CHECK(renderer.valid());
    std::vector<tox2::RsxCommand> commands = replaySynthetic(scene.finish());
    tox2::rsxUnimplementedReset();
    renderer.execute(commands);
    CHECK(renderer.valid());
    StencilRender result;
    result.scale = scale;
    CHECK(renderer.readDisplay(result.bgra, result.width, result.height));
    result.stats = renderer.stats();
    if (result.bgra.size() != static_cast<std::size_t>(result.width) * result.height * 4) result.bgra.clear();
    return result;
}

// A target bound again at the same address with another size keeps the overlap of the old one when the pitch
// and the format allow it, unless the first command on it overwrites everything; a target seen for the first
// time starts as opaque black.
void testRendererSurfaceResize(tox2::VkContext& ctx, std::uint32_t scale = 1) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    // 1. The quad drawn into 64x64, then the target is bound as 32x32: its top-left part survives.
    {
        SceneBuilder scene;
        beginResizeScene(scene);
        colorDraw(scene);
        rebindSurface(scene, 32, kSurfacePitch);
        bindingDraw(scene);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.width == 32 * scale && render.height == 32 * scale && !render.bgra.empty());
        if (!render.bgra.empty()) {
            CHECK(render.pixel(20, 20) == kQuadRed);
            CHECK(render.pixel(28, 28) == kQuadRed);
            CHECK(render.pixel(2, 2) == kResizeClear);
            CHECK(render.pixel(30, 10) == kResizeClear);
        }
        CHECK(render.stats.surface_inherits == 1);
        CHECK(render.stats.draws == 2 && render.stats.draws_skipped == 0);
        expectNoRendererNotes();
    }

    // 2. The first command on the resized target is a full colour clear: nothing is copied.
    {
        SceneBuilder scene;
        beginResizeScene(scene);
        colorDraw(scene);
        rebindSurface(scene, 32, kSurfacePitch);
        scene.write(tox2::kMethodNv4097SetColorClearValue, 0xFF405060);
        scene.write(tox2::kMethodNv4097ClearSurface, 0xF0);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.width == 32 * scale && render.height == 32 * scale && !render.bgra.empty());
        if (!render.bgra.empty()) {
            CHECK(render.pixel(20, 20) == 0xFF405060);
            CHECK(render.pixel(2, 2) == 0xFF405060);
        }
        CHECK(render.stats.surface_inherits == 0);
        expectNoRendererNotes();
    }

    // 3. Another pitch: not inherited, the new target is opaque black, counted once.
    {
        SceneBuilder scene;
        beginResizeScene(scene);
        colorDraw(scene);
        rebindSurface(scene, 32, kSurfacePitch * 2);
        bindingDraw(scene);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.width == 32 * scale && render.height == 32 * scale && !render.bgra.empty());
        if (!render.bgra.empty()) {
            CHECK(render.pixel(20, 20) == kBlack);
            CHECK(render.pixel(2, 2) == kBlack);
        }
        CHECK(render.stats.surface_inherits == 0);
        CHECK(unimplementedCount("state surface resize with another pitch") == 1);
        CHECK(tox2::rsxUnimplementedSnapshot().size() == 1);
    }

    // 4. A target seen for the first time is opaque black, alpha included.
    {
        // Nothing is cleared: the target the draw creates holds the initial contents.
        SceneBuilder scene;
        scene.setSurface();
        scene.write(tox2::kMethodNv4097SetSurfaceFormat, 8u | (1u << 8));
        setQuadRenderState(scene);
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        scene.setColorAttrib(3, kColorsOffset);
        scene.setPrograms(quadPrograms());
        bindingDraw(scene);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.width == 64 * scale && render.height == 64 * scale && !render.bgra.empty());
        if (!render.bgra.empty()) {
            CHECK(render.pixel(20, 20) == kBlack);
            CHECK(render.pixel(2, 2) == kBlack);
        }
        expectNoRendererNotes();
    }

    // 5. Depth: both targets are resized (the size is the clip) and a depth and stencil clear is the first
    // command that touches the depth target. Under a stencil write mask of 0xFF it overwrites both aspects; under
    // 0 the stencil bit is dropped, only the depth is overwritten, and the old stencil is copied.
    for (const std::uint32_t stencil_mask : {0xFFu, 0u}) {
        SceneBuilder scene;
        beginResizeScene(scene, 2);
        rebindSurface(scene, 32, kSurfacePitch);
        scene.write(tox2::kMethodNv4097SetStencilMask, stencil_mask);
        scene.write(tox2::kMethodNv4097ClearSurface, 0x03);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.stats.surface_inherits == (stencil_mask == 0xFF ? 0u : 1u));
        expectNoRendererNotes();
    }

    // 6. Z24S8 to Z16 at one address cannot be copied; counted once and the draw still works.
    {
        SceneBuilder scene;
        beginResizeScene(scene, 2);
        scene.write(tox2::kMethodNv4097SetSurfaceFormat, 8u | (1u << 5) | (1u << 8));
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);  // an unused depth surface is not acquired
        bindingDraw(scene);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.stats.surface_inherits == 0 && render.stats.draws_skipped == 0);
        CHECK(unimplementedCount("state surface resize to an incompatible format") == 1);
        CHECK(tox2::rsxUnimplementedSnapshot().size() == 1);
    }

    // 7. A8R8G8B8 to F_X32 at one address, same size and pitch: the words are copied as they are.
    {
        SceneBuilder scene;
        beginResizeScene(scene);
        colorDraw(scene);
        scene.write(tox2::kMethodNv4097SetSurfaceFormat, 13u | (1u << 8));
        bindingDraw(scene);
        const StencilRender render = renderResizeScene(ctx, scene, scale);
        CHECK(render.stats.surface_inherits == 1 && render.stats.draws_skipped == 0);
        CHECK(unimplementedCount("state surface resize with another pitch") == 0);
        CHECK(unimplementedCount("state surface resize to an incompatible format") == 0);
    }
    tox2::rsxUnimplementedReset();
}

// ---- Resolution scale ----

// The last display of a run on a fresh renderer, box-downsampled to the guest size, with the figures a scale must
// not change.
struct ScaledFrame {
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0, height = 0;              // guest size
    std::uint32_t image_width = 0, image_height = 0;  // as read back
    tox2::RendererStats stats;
    std::vector<std::pair<std::string, std::uint64_t>> notes;
    bool ok = false;
};

// Runs the captures in order (one flip each) at `scale`. `reduce` is the factor to downsample the display by,
// normally the scale itself.
ScaledFrame renderAtScale(tox2::VkContext& ctx, const std::vector<std::vector<std::uint8_t>>& files,
                          std::uint32_t scale, std::uint32_t reduce = 0, std::size_t drop_first_commands = 0,
                          tox2::AaMode aa_mode = tox2::AaMode::Msaa) {
    if (reduce == 0) reduce = scale;
    ScaledFrame frame;
    tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale, aa_mode));
    CHECK(renderer.valid() && renderer.resolutionScale() == scale);
    std::vector<std::vector<tox2::RsxCommand>> frames;
    for (const std::vector<std::uint8_t>& file : files) frames.push_back(replaySynthetic(file));
    // Commands the renderer never sees, while the frame builder has already resolved what they drew.
    if (!frames.empty() && frames[0].size() >= drop_first_commands) {
        frames[0].erase(frames[0].begin(), frames[0].begin() + static_cast<std::ptrdiff_t>(drop_first_commands));
    }
    tox2::rsxUnimplementedReset();
    for (std::vector<tox2::RsxCommand>& commands : frames) renderer.execute(commands);
    CHECK(renderer.valid());
    std::vector<std::uint8_t> image;
    if (!renderer.readDisplay(image, frame.image_width, frame.image_height)) {
        CHECK(false);
        return frame;
    }
    std::string error;
    const bool reduced =
            tox2::downsampleBoxBgra(image, frame.image_width, frame.image_height, reduce, reduce, frame.bgra, error);
    if (!reduced) std::fprintf(stderr, "downsample failed: %s\n", error.c_str());
    CHECK(reduced);
    frame.width = frame.image_width / reduce;
    frame.height = frame.image_height / reduce;
    frame.stats = renderer.stats();
    frame.notes = tox2::rsxUnimplementedSnapshot();
    frame.ok = reduced;
    return frame;
}

bool sameStats(const tox2::RendererStats& a, const tox2::RendererStats& b) {
    return a.clears == b.clears && a.draws == b.draws && a.draws_skipped == b.draws_skipped && a.flips == b.flips &&
           a.feedback_copies == b.feedback_copies && a.depth_conversions == b.depth_conversions &&
           a.surface_inherits == b.surface_inherits;
}

// Renders the captures at scales 1, 2 and 3. The scaled displays are 2x and 3x the guest size, their box
// downsample matches the scale-1 display within `tolerance` per channel (alpha included), and the statistics and
// the census are the same. Returns the scale-1 frame for probes.
ScaledFrame expectScaleInvariant(tox2::VkContext& ctx, const char* name,
                                 const std::vector<std::vector<std::uint8_t>>& files, int tolerance,
                                 std::size_t drop_first_commands = 0) {
    const ScaledFrame base = renderAtScale(ctx, files, 1, 0, drop_first_commands);
    CHECK(base.ok);
    for (const std::uint32_t scale : {2u, 3u}) {
        const ScaledFrame scaled = renderAtScale(ctx, files, scale, 0, drop_first_commands);
        CHECK(scaled.ok && base.ok);
        if (!scaled.ok || !base.ok) continue;
        CHECK(scaled.image_width == base.image_width * scale && scaled.image_height == base.image_height * scale);
        if (scaled.bgra.size() != base.bgra.size()) {
            CHECK(false);
            continue;
        }
        int max_diff = 0;
        for (std::size_t i = 0; i < base.bgra.size(); i++) {
            max_diff = std::max(max_diff, std::abs(static_cast<int>(base.bgra[i]) - static_cast<int>(scaled.bgra[i])));
        }
        if (max_diff > tolerance) {
            std::fprintf(stderr, "%s at scale %u: max channel difference %d (limit %d)\n", name, scale, max_diff,
                         tolerance);
        }
        CHECK(max_diff <= tolerance);
        CHECK(sameStats(scaled.stats, base.stats));
        CHECK(scaled.notes == base.notes);
    }
    return base;
}

// A depth-only clear of a rectangle (to 0.25) under a depth-tested quad at depth 0.5: the quad shows everywhere but
// in that rectangle.
std::vector<std::uint8_t> makeDepthOnlyClearCapture() {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    beginStencilScene(scene, 0);
    scene.write(tox2::kMethodNv4097SetScissorHorizontal, 24u | (16u << 16));
    scene.write(tox2::kMethodNv4097SetScissorVertical, 24u | (16u << 16));
    scene.write(tox2::kMethodNv4097SetZstencilClearValue, 0x40000000u);
    scene.write(tox2::kMethodNv4097ClearSurface, 0x01);
    scene.write(tox2::kMethodNv4097SetScissorHorizontal, kSurfaceSize << 16);
    scene.write(tox2::kMethodNv4097SetScissorVertical, kSurfaceSize << 16);
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);
    scene.write(tox2::kMethodNv4097SetDepthFunc, 0x201);  // LESS
    scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
    putSurfaceQuad(scene, 16, 48);
    scene.drawStrip(4);
    return scene.finish();
}

// A surface of `width` x 2 pixels, cleared: the clear covers the whole target and is followed by a flip.
std::vector<std::uint8_t> makeWideSurfaceCapture(std::uint32_t width, std::uint32_t color) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.write(tox2::kMethodNv4097SetSurfaceClipHorizontal, width << 16);
    scene.write(tox2::kMethodNv4097SetSurfaceClipVertical, 2u << 16);
    scene.write(tox2::kMethodNv4097SetSurfacePitchA, width * 4);
    scene.write(tox2::kMethodNv4097SetSurfacePitchZ, width * 4);
    scene.write(tox2::kMethodNv4097SetScissorHorizontal, width << 16);
    scene.write(tox2::kMethodNv4097SetScissorVertical, 2u << 16);
    scene.clear(color);
    return scene.finish();
}

// A surface wider than the device allows at the requested scale is made at the largest scale that fits (here the
// colour and depth targets have the same size, so they share it), and the fallback is counted.
void checkSurfaceTooLarge(tox2::VkContext& ctx, std::uint32_t scale, std::uint32_t width) {
    const std::uint32_t max_dimension = ctx.deviceProperties().limits.maxImageDimension2D;
    if (width > 0xFFFF || width > max_dimension) {
        std::printf("skipping the size limit case for scale %u: the device limit is %u\n", scale, max_dimension);
        return;
    }
    const std::uint32_t fitted = std::min(scale, max_dimension / width);
    CHECK(fitted >= 1 && fitted < scale);
    // The readback is at the scale the target was made at, so the display is reduced by that factor.
    const ScaledFrame frame = renderAtScale(ctx, {makeWideSurfaceCapture(width, 0xFF102030)}, scale, fitted);
    CHECK(frame.ok);
    if (!frame.ok) return;
    CHECK(frame.image_width == width * fitted && frame.image_height == 2 * fitted);
    CHECK(frame.width == width && frame.height == 2 && pixelArgb(frame.bgra, frame.width, width - 1, 1) == 0xFF102030);
    CHECK(unimplementedCount("state surface too large for the resolution scale") >= 1);
    CHECK(unimplementedCount(std::format("state surface size {}x2", width)) == 0);
    CHECK(frame.stats.clears == 1 && frame.stats.draws_skipped == 0);
}

void testRendererScaledPlain(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    // The scale is validated at construction.
    for (const std::uint32_t bad : {0u, tox2::kMaxResolutionScale + 1}) {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(bad));
        CHECK(!renderer.valid() && renderer.error().find("resolution scale") != std::string::npos);
    }
    for (std::uint32_t scale = 1; scale <= tox2::kMaxResolutionScale; scale++) {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale));
        CHECK(renderer.valid() && renderer.resolutionScale() == scale);
    }

    constexpr std::uint32_t kA = 0xFF102030;
    constexpr std::uint32_t kB = 0xFF405060;

    // Flat and half-transparent clears.
    ScaledFrame frame = expectScaleInvariant(ctx, "clear", {makeClearCapture(kA)}, 0);
    CHECK(frame.ok && frame.width == 64 && pixelArgb(frame.bgra, frame.width, 10, 10) == kA);
    frame = expectScaleInvariant(ctx, "half-transparent clear", {makeClearCapture(0x80FF0000)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 10, 10) == 0x80FF0000);

    // A clear inside a rectangle, a partial colour mask over everything, and alpha alone inside a rectangle.
    frame = expectScaleInvariant(ctx, "scissored clear", {makeScissoredClearCapture(kA, kB, 0xF3, 8, 16, 16, 8)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 8, 16) == kB &&
          pixelArgb(frame.bgra, frame.width, 7, 16) == kA);
    frame = expectScaleInvariant(ctx, "masked clear", {makeScissoredClearCapture(kA, kB, 0x30, 0, 0, 64, 64)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 10, 10) == 0xFF405030);
    frame = expectScaleInvariant(ctx, "masked clear in a rectangle",
                                 {makeScissoredClearCapture(kA, 0x80405060, 0x80, 8, 16, 16, 8)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 8, 16) == 0x80102030 &&
          pixelArgb(frame.bgra, frame.width, 7, 16) == kA);

    // A vertex-colour quad: each sample is rounded to 8 bits before the downsample.
    frame = expectScaleInvariant(ctx, "vertex-colour quad", {makeRenderedQuadCapture({})}, 2);
    checkQuadPixels(frame.bgra, frame.width, frame.height);

    // A 2x2 NEAREST texture, whose texel edges fall on pixel edges: four different colours, none black.
    frame = expectScaleInvariant(ctx, "textured quad", {makeTexturedCapture({})}, 0);
    if (frame.ok) {
        const std::uint32_t probes[4] = {
                pixelArgb(frame.bgra, frame.width, 24, 24), pixelArgb(frame.bgra, frame.width, 40, 24),
                pixelArgb(frame.bgra, frame.width, 24, 40), pixelArgb(frame.bgra, frame.width, 40, 40)};
        for (std::uint32_t i = 0; i < 4; i++) {
            CHECK(probes[i] != kBlack);
            for (std::uint32_t j = i + 1; j < 4; j++) CHECK(probes[i] != probes[j]);
        }
    }

    // A float surface read back through an X32_FLOAT texture.
    frame = expectScaleInvariant(ctx, "float surface", {makeFloatSurfaceCapture()}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 24, 32) == 0x40404040);

    // A depth-only clear: the depth target alone sets the rectangle's scale.
    frame = expectScaleInvariant(ctx, "depth-only clear", {makeDepthOnlyClearCapture()}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 20, 20) == kQuadRed &&
          pixelArgb(frame.bgra, frame.width, 32, 32) == kBlack);

    // A flip of a buffer that was never a render target is black at the flip's size times the scale.
    {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(3));
        std::vector<tox2::RsxCommand> flip_only;
        tox2::RsxFlip flip;
        flip.offset = 0x40000;
        flip.width = 8;
        flip.height = 4;
        flip_only.emplace_back(flip);
        renderer.execute(flip_only);
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(bgra, width, height) && width == 24 && height == 12);
        CHECK(displayIsUniform(bgra, width, height, {0, 0, 0, 0xFF}));
    }

    // Two sizes of surface that do not fit at the requested scale.
    const std::uint32_t max_dimension = ctx.deviceProperties().limits.maxImageDimension2D;
    checkSurfaceTooLarge(ctx, 2, max_dimension / 2 + 1);
    checkSurfaceTooLarge(ctx, 4, max_dimension / 3 + 1);
    tox2::rsxUnimplementedReset();
}

// Targets read as textures, at scales 2 and 3 against scale 1.
void testRendererScaledTargetsAsTextures(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    // A render target sampled by a second pass; its left half differs, so a wrong scale of the lookup shows.
    TextureOptions alias;
    alias.scene = TextureScene::Alias;
    alias.split_alias = true;
    ScaledFrame frame = expectScaleInvariant(ctx, "target alias", {makeTexturedCapture(alias)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 20, 32) == kAliasSplitColor &&
          pixelArgb(frame.bgra, frame.width, 44, 32) == kAliasColor);

    // The same read with texel coordinates: the texture scale is the guest size, not the image size.
    TextureOptions unnormalized = alias;
    unnormalized.unnormalized = true;
    unnormalized.coord_scale = 64.0f;
    frame = expectScaleInvariant(ctx, "unnormalized target alias", {makeTexturedCapture(unnormalized)}, 0);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 20, 32) == kAliasSplitColor &&
          pixelArgb(frame.bgra, frame.width, 44, 32) == kAliasColor);

    // A draw that samples its own target goes through the feedback copy.
    TextureOptions feedback;
    feedback.scene = TextureScene::Feedback;
    frame = expectScaleInvariant(ctx, "feedback copy", {makeTexturedCapture(feedback)}, 0);
    CHECK(frame.ok && frame.stats.feedback_copies == 1);

    // A target the renderer never saw drawn (its clear is dropped here) is created at the current scale and reads
    // as opaque black over the display's own clear colour.
    TextureOptions unknown = alias;
    unknown.split_alias = false;
    unknown.display_clear = kAliasColor;
    frame = expectScaleInvariant(ctx, "alias of an unknown target", {makeTexturedCapture(unknown)}, 0, 1);
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 32, 32) == kBlack &&
          pixelArgb(frame.bgra, frame.width, 2, 2) == kAliasColor);
    CHECK(frame.ok && unimplementedCount("texture alias of unknown render target") == 1);

    // Depth cleared, then read as A8R8G8B8, including after later clears and at the far plane.
    frame = expectScaleInvariant(ctx, "depth as A8R8G8B8", {makeDepthTextureCapture()}, 0);
    CHECK(frame.ok && frame.stats.depth_conversions == 3);
    CHECK(frame.ok && depthWordNear(pixelArgb(frame.bgra, frame.width, 32, 32), kDepthSecond, kStencilSecond));
    CHECK(frame.ok && pixelArgb(frame.bgra, frame.width, 56, 32) == depthWordArgb(kDepthFar, kStencilFar));

    // The resize scenes (inherit copies, pitch and format rules) with the expectations of the scale-1 test.
    testRendererSurfaceResize(ctx, 2);
    testRendererSurfaceResize(ctx, 3);
    tox2::rsxUnimplementedReset();
}

// ---- Changing the scale at run time ----

constexpr std::uint32_t kScaleChangeGreen = 0xFF00FF00;  // ARGB of the vertex colour word 0xFF00FF00
constexpr std::uint32_t kScaleChangeBlue = 0xFF0000FF;

void setQuadColor(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t word) {
    for (std::uint32_t v = 0; v < 4; v++) {
        scene.memory().putWord(1, tox2::rsx_synthetic::kColorsOffset + v * 4, word);
    }
}

// Frame 0 clears to black, draws a flat rectangle over pixels 8..24 and a depth-writing quad (depth 0.5) over 40..56.
// Frame 1 keeps everything and draws a rectangle over 32..48 under a LESS depth test: it shows left of the depth quad
// and is rejected where the quad's depth is still there.
std::vector<std::uint8_t> makeScaleChangeFrame(std::uint32_t frame) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    if (frame == 0) {
        beginStencilScene(scene, 0);
    } else {
        scene.setSurface();
        setQuadRenderState(scene);
        scene.setFloatAttrib(0, 3, kPositionsOffset);
        scene.setColorAttrib(3, kColorsOffset);
        scene.setPrograms(quadPrograms());
    }
    scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
    scene.write(tox2::kMethodNv4097SetDepthFunc, 0x201);  // LESS
    if (frame == 0) {
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 0);
        putSurfaceQuad(scene, 8, 24);
        scene.drawStrip(4);
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);
        scene.write(tox2::kMethodNv4097SetDepthMask, 1);
        putSurfaceQuad(scene, 40, 56);
        setQuadColor(scene, kScaleChangeGreen);
    } else {
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);
        scene.write(tox2::kMethodNv4097SetDepthMask, 0);
        putSurfaceQuad(scene, 32, 48);
        setQuadColor(scene, kScaleChangeBlue);
    }
    scene.drawStrip(4);
    return scene.finish();
}

// A scale change copies depth targets with a blit, which some devices do not offer for the depth/stencil format
// (Apple GPUs). The renderer then keeps the new target at its initial contents and notes it, so the comparisons that
// depend on the rescaled depth do not apply there.
bool depthRescaleBlittable(tox2::VkContext& ctx) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), ctx.depthStencilFormat(), &props);
    constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (props.optimalTilingFeatures & kBlit) == kBlit;
}

// Two frames on one renderer whose scale is changed from `from` to `to` between them. The second display is `to`
// times the guest size and its box downsample equals the scale-1 rendering of both frames: the colour contents and
// the depth buffer both survive the change.
void expectScaleChange(tox2::VkContext& ctx, const ScaledFrame& reference, std::uint32_t from, std::uint32_t to) {
    tox2::RsxRenderer renderer(ctx, readbackAllOptions(from));
    CHECK(renderer.valid());
    std::vector<tox2::RsxCommand> first = replaySynthetic(makeScaleChangeFrame(0));
    std::vector<tox2::RsxCommand> second = replaySynthetic(makeScaleChangeFrame(1));
    tox2::rsxUnimplementedReset();

    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    renderer.execute(first);
    CHECK(renderer.readDisplay(bgra, width, height) && width == 64 * from && height == 64 * from);

    CHECK(renderer.setResolutionScale(to) && renderer.resolutionScale() == to);
    CHECK(!renderer.setResolutionScale(0) && renderer.resolutionScale() == to);
    CHECK(!renderer.setResolutionScale(tox2::kMaxResolutionScale + 1) && renderer.resolutionScale() == to);
    renderer.execute(second);
    CHECK(renderer.valid());
    CHECK(renderer.readDisplay(bgra, width, height) && width == 64 * to && height == 64 * to);

    std::vector<std::uint8_t> reduced;
    std::string error;
    CHECK(tox2::downsampleBoxBgra(bgra, width, height, to, to, reduced, error));
    CHECK(reduced.size() == reference.bgra.size());
    if (reduced.size() != reference.bgra.size()) return;
    int max_diff = 0;
    for (std::size_t i = 0; i < reduced.size(); i++) {
        max_diff = std::max(max_diff, std::abs(static_cast<int>(reduced[i]) - static_cast<int>(reference.bgra[i])));
    }
    if (max_diff != 0) std::fprintf(stderr, "scale %u to %u: max channel difference %d\n", from, to, max_diff);
    CHECK(max_diff == 0);
    CHECK(sameStats(renderer.stats(), reference.stats));
    CHECK(tox2::rsxUnimplementedSnapshot() == reference.notes);
}

void testRendererScaleChange(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    const ScaledFrame reference = renderAtScale(ctx, {makeScaleChangeFrame(0), makeScaleChangeFrame(1)}, 1);
    CHECK(reference.ok && reference.width == 64);
    if (!reference.ok) return;
    CHECK(pixelArgb(reference.bgra, reference.width, 12, 32) == kQuadRed);
    CHECK(pixelArgb(reference.bgra, reference.width, 36, 32) == kScaleChangeBlue);
    CHECK(pixelArgb(reference.bgra, reference.width, 44, 32) == kScaleChangeGreen);
    CHECK(pixelArgb(reference.bgra, reference.width, 60, 32) == kBlack);
    CHECK(reference.notes.empty());

    if (!depthRescaleBlittable(ctx)) {
        std::printf("render tests: scale change skipped: the depth format cannot be blitted\n");
        tox2::rsxUnimplementedReset();
        return;
    }
    expectScaleChange(ctx, reference, 1, 2);
    expectScaleChange(ctx, reference, 2, 1);
    expectScaleChange(ctx, reference, 1, 3);
    expectScaleChange(ctx, reference, 3, 1);
    tox2::rsxUnimplementedReset();
}

// ---- Anti-aliased surface layouts ----

constexpr std::uint32_t kAaSize = 32;  // clip of the anti-aliased surface; its layout is 64x32
constexpr std::uint32_t kAaOffset = 0x40000;

// A red triangle with a 45 degree hypotenuse on black, drawn into a kAaSize x kAaSize surface in the AA layout
// `aa_mode` (mode 0: a plain surface at the display offset). With `resolve` a second pass reads the surface the
// way the game does, as a 2W x H linear texture with a LINEAR filter, into a plain target at the display offset.
std::vector<std::uint8_t> makeAntiAliasedCapture(std::uint32_t aa_mode, bool resolve) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface(aa_mode, kAaSize, resolve ? kAaOffset : kSurfaceOffset);
    scene.clear(0xFF000000);
    setQuadRenderState(scene, kAaSize / 2.0f);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    constexpr float kCorners[3][2] = {{-0.75f, -0.75f}, {0.75f, -0.75f}, {-0.75f, 0.75f}};
    for (std::uint32_t v = 0; v < 3; v++) {
        scene.memory().putFloat(1, kPositionsOffset + v * 12, kCorners[v][0]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, kCorners[v][1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
        scene.memory().putWord(1, kColorsOffset + v * 4, 0xFFFF0000);  // red
    }
    scene.drawStrip(3);
    if (!resolve) return scene.finish();

    scene.setSurface(0, kAaSize, kSurfaceOffset);
    scene.clear(0xFF000000);
    constexpr float kQuad[4][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {1, 1, 1, 1}};  // x, y, u, v
    for (std::uint32_t v = 0; v < 4; v++) {
        scene.memory().putFloat(1, kPositionsOffset + v * 12, kQuad[v][0]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, kQuad[v][1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8, kQuad[v][2]);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8 + 4, kQuad[v][3]);
    }
    scene.bindTexture(kAaOffset, kAaSize * 2, kAaSize);
    scene.write(tox2::kMethodNv4097SetTextureFilter, (2u << 16) | (2u << 24));  // LINEAR
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);        // TEX0
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);                    // color from H0
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 1);                      // 2D coordinate
    scene.setFloatAttrib(8, 2, kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());
    scene.drawStrip(4);
    return scene.finish();
}

// The 45 degree hypotenuse is parallel to the diagonal of the 2-sample pattern, so this scene only shows partial pixels
// when it is supersampled; the 2-sample path is tested by testRendererMsaaLayout.
void testRendererAntiAliasedSurface(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();

    // The plain render of the same triangle is the reference: pure red and black, no partial pixels.
    const ScaledFrame plain = renderAtScale(ctx, {makeAntiAliasedCapture(0, false)}, 1);
    CHECK(plain.ok && plain.width == kAaSize && plain.height == kAaSize);
    if (!plain.ok) return;
    const auto redOf = [](const ScaledFrame& frame, std::uint32_t x, std::uint32_t y) {
        return (pixelArgb(frame.bgra, frame.width, x, y) >> 16) & 0xFF;
    };
    const auto countEdges = [&](const ScaledFrame& frame) {
        std::uint32_t edges = 0;
        for (std::uint32_t y = 0; y < frame.height; y++) {
            for (std::uint32_t x = 0; x < frame.width; x++) {
                const std::uint32_t argb = pixelArgb(frame.bgra, frame.width, x, y);
                const std::uint32_t red = (argb >> 16) & 0xFF;
                if (red > 0 && red < 255 && (argb & 0xFFFF) == 0) edges++;
            }
        }
        return edges;
    };
    CHECK(countEdges(plain) == 0);
    CHECK(redOf(plain, 8, 20) == 255 && redOf(plain, 24, 8) == 0);

    for (const std::uint32_t scale : {1u, 2u}) {
        // The frame builder counted no anti-aliasing mode for the layout.
        const std::vector<std::uint8_t> file = makeAntiAliasedCapture(3, true);
        tox2::rsxUnimplementedReset();
        replaySynthetic(file);
        CHECK(unimplementedCount("state antialiasing 0x3") == 0);

        const ScaledFrame aa = renderAtScale(ctx, {file}, scale, 0, 0, tox2::AaMode::Supersample);
        CHECK(aa.ok && aa.width == kAaSize && aa.height == kAaSize && aa.image_width == kAaSize * scale);
        if (!aa.ok) continue;
        // The surface is drawn in its layout, so the resolve texture is the same size and nothing is approximated.
        CHECK(aa.notes.empty());
        CHECK(aa.stats.draws == 2 && aa.stats.draws_skipped == 0);

        // Away from the edges the resolve equals the plain render, and the diagonal has partial pixels.
        std::uint32_t compared = 0;
        for (std::uint32_t y = 1; y + 1 < kAaSize; y++) {
            for (std::uint32_t x = 1; x + 1 < kAaSize; x++) {
                bool interior = true;
                for (std::uint32_t dy = 0; dy < 3; dy++) {
                    for (std::uint32_t dx = 0; dx < 3; dx++) {
                        interior = interior && pixelArgb(plain.bgra, plain.width, x + dx - 1, y + dy - 1) ==
                                                       pixelArgb(plain.bgra, plain.width, x, y);
                    }
                }
                if (!interior) continue;
                compared++;
                CHECK(pixelArgb(aa.bgra, aa.width, x, y) == pixelArgb(plain.bgra, plain.width, x, y));
            }
        }
        CHECK(compared > 400);
        const std::uint32_t edges = countEdges(aa);
        if (edges < 8) std::fprintf(stderr, "anti-aliased surface at scale %u: %u edge pixels\n", scale, edges);
        CHECK(edges >= 8);
    }
    tox2::rsxUnimplementedReset();
}

// ---- Multisampled anti-aliased surfaces ----

// Where the red rectangle drawn into the 2-sample surface lies, in guest pixels. The right and bottom edges fall
// between the two samples of a pixel, so both are anti-aliased, and no edge passes within 0.05 of a sample position.
constexpr float kMsRectLeft = 4.0f;
constexpr float kMsRectTop = 4.0f;
constexpr float kMsRectRight = 20.3f;
constexpr float kMsRectBottom = 12.3f;

enum class MsaaRead {
    Texels,      // the surface as a 64x32 NEAREST texture into a 64x32 display: one display texel per layout texel
    Linear,      // the game's resolve: a LINEAR read into a display of the clip size
    SelfSample,  // a pass into the surface samples it (at layout column 2x + 1), then the Texels read
    LinearHeld,  // Linear, but the rectangle is a degenerate draw that covers nothing and the surface is not cleared
    Unread,      // the rectangle only; the display is a cleared plain surface and nothing reads the AA surface
};

void setViewportHalfSize(tox2::rsx_synthetic::SceneBuilder& scene, float half_width, float half_height) {
    const float scale[4] = {half_width, half_height, 0.5f, 0.0f};
    const float offset[4] = {half_width, half_height, 0.5f, 0.0f};
    for (std::uint32_t i = 0; i < 4; i++) {
        scene.write(tox2::kMethodNv4097SetViewportScale + i * 4, std::bit_cast<std::uint32_t>(scale[i]));
        scene.write(tox2::kMethodNv4097SetViewportOffset + i * 4, std::bit_cast<std::uint32_t>(offset[i]));
    }
}

// A textured full-target quad: positions (-1..1), texture coordinates u0..u1 across and 0..1 down.
void putTexturedFullQuad(tox2::rsx_synthetic::SceneBuilder& scene, float u0, float u1) {
    using namespace tox2::rsx_synthetic;
    const float quad[4][4] = {{-1, -1, u0, 0}, {1, -1, u1, 0}, {-1, 1, u0, 1}, {1, 1, u1, 1}};  // x, y, u, v
    for (std::uint32_t v = 0; v < 4; v++) {
        scene.memory().putFloat(1, kPositionsOffset + v * 12, quad[v][0]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, quad[v][1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8, quad[v][2]);
        scene.memory().putFloat(1, kTexCoordsOffset + v * 8 + 4, quad[v][3]);
    }
}

// Unit 0 reads `offset` as an A8R8G8B8 texture of `width` x kAaSize (the 2W layout of the AA surface by default).
void setUpTexturedDraw(tox2::rsx_synthetic::SceneBuilder& scene, std::uint32_t filter, std::uint32_t offset = kAaOffset,
                       std::uint32_t width = kAaSize * 2) {
    using namespace tox2::rsx_synthetic;
    scene.bindTexture(offset, width, kAaSize);
    scene.write(tox2::kMethodNv4097SetTextureFilter, (filter << 16) | (filter << 24));
    scene.write(tox2::kMethodNv4097SetVertexAttribOutputMask, 1u << 14);  // TEX0
    scene.write(tox2::kMethodNv4097SetShaderControl, 0x400);              // color from H0
    scene.write(tox2::kMethodNv4097SetTexCoordControl, 1);                // 2D coordinate
    scene.setFloatAttrib(8, 2, kTexCoordsOffset);
    scene.setPrograms(texturedPrograms());
}

// Draws the red rectangle (kMsRect*) into the surface bound now, a kAaSize square, at depth 0.5. A `degenerate`
// rectangle has no width: the draw is recorded (and registers the surface with the frame builder) but writes nothing.
void drawMsaaRect(tox2::rsx_synthetic::SceneBuilder& scene, bool degenerate = false) {
    using namespace tox2::rsx_synthetic;
    setQuadRenderState(scene, kAaSize / 2.0f);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    const auto ndc = [](float v) { return v / (kAaSize / 2.0f) - 1.0f; };
    const float xs[2] = {ndc(kMsRectLeft), degenerate ? ndc(kMsRectLeft) : ndc(kMsRectRight)};
    const float ys[2] = {ndc(kMsRectTop), ndc(kMsRectBottom)};
    for (std::uint32_t v = 0; v < 4; v++) {  // strip order: left/right, then top/bottom
        scene.memory().putFloat(1, kPositionsOffset + v * 12, xs[v & 1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 4, ys[v >> 1]);
        scene.memory().putFloat(1, kPositionsOffset + v * 12 + 8, 0.0f);
        scene.memory().putWord(1, kColorsOffset + v * 4, 0xFFFF0000);  // red
    }
    scene.drawStrip(4);
}

// A 32x32 mode-3 surface cleared black with a red rectangle (kMsRect*) drawn into it, then read the way `read` says.
std::vector<std::uint8_t> makeMsaaLayoutCapture(MsaaRead read) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface(3, kAaSize, kAaOffset);
    if (read != MsaaRead::LinearHeld) scene.clear(0xFF000000);
    drawMsaaRect(scene, read == MsaaRead::LinearHeld);

    if (read == MsaaRead::Unread) {
        scene.setSurface(0, kAaSize, kSurfaceOffset);
        scene.clear(0xFF000000);
        return scene.finish();
    }

    if (read == MsaaRead::SelfSample) {
        // The pixel centre falls on the boundary between layout texels 2X and 2X + 1; the u range is shifted by an
        // eighth of a pixel, which moves the lookup into texel 2X + 1 (by 0.25 texel at scale 1 and 0.5 at scale 2).
        scene.setSurface(3, kAaSize, kAaOffset);
        setQuadRenderState(scene, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.125f / kAaSize, 1.0f + 0.125f / kAaSize);
        setUpTexturedDraw(scene, 1);
        scene.drawStrip(4);
    }

    if (read == MsaaRead::Linear || read == MsaaRead::LinearHeld) {
        scene.setSurface(0, kAaSize, kSurfaceOffset);
        scene.clear(0xFF000000);
        setViewportHalfSize(scene, kAaSize / 2.0f, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.0f, 1.0f);
        setUpTexturedDraw(scene, 2);  // LINEAR
    } else {
        scene.setSurface(0, kAaSize * 2, kSurfaceOffset, kAaSize);
        scene.clear(0xFF000000);
        setViewportHalfSize(scene, kAaSize, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.0f, 1.0f);
        setUpTexturedDraw(scene, 1);  // NEAREST
    }
    scene.drawStrip(4);
    return scene.finish();
}

// Whether layout texel (x, y) of the rectangle's surface is red at resolution scale `scale`: sample
// kLayoutColumnSample[x & 1] of multisampled pixel (x >> 1, y) lies inside the rectangle. The standard 2x positions
// (y down) are sample 0 at (0.75, 0.75) and sample 1 at (0.25, 0.25).
bool msaaTexelCovered(std::uint32_t x, std::uint32_t y, std::uint32_t scale) {
    const tox2::LayoutSource source = tox2::layoutSource(x, y);
    const float position = source.sample == 0 ? 0.75f : 0.25f;
    const float guest_x = (static_cast<float>(source.x) + position) / static_cast<float>(scale);
    const float guest_y = (static_cast<float>(source.y) + position) / static_cast<float>(scale);
    return guest_x >= kMsRectLeft && guest_x < kMsRectRight && guest_y >= kMsRectTop && guest_y < kMsRectBottom;
}

// A mode-3 surface drawn as 2 samples and read through its layout: the layout holds sample kLayoutColumnSample[x & 1]
// of pixel x / 2 at texel x, at every resolution scale, and edges are anti-aliased along both axes.
void testRendererMsaaLayout(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("multisampled layout: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    if (ctx.deviceProperties().limits.standardSampleLocations == VK_FALSE) {
        std::printf("multisampled layout: skipped (no standard sample locations)\n");
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    constexpr std::uint32_t kRed = 0xFFFF0000, kBlack = 0xFF000000;

    for (const std::uint32_t scale : {1u, 2u}) {
        // The surface read texel by texel.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Texels)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0, red = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t expected = msaaTexelCovered(x, y, scale) ? kRed : kBlack;
                    red += expected == kRed;
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled layout at scale %u: %u texels differ\n", scale, mismatches);
            CHECK(mismatches == 0 && red > 0);

            // Along the bottom edge the upper sample is inside and the lower one outside, which a surface shaded per
            // layout texel cannot produce: the two texels of a pixel differ.
            std::uint32_t vertical_pairs = 0;
            const std::uint32_t edge_row = static_cast<std::uint32_t>(kMsRectBottom * static_cast<float>(scale));
            for (std::uint32_t x = 0; x + 1 < frame.width; x += 2) {
                vertical_pairs += pixelArgb(frame.bgra, frame.width, x, edge_row) == kRed &&
                                  pixelArgb(frame.bgra, frame.width, x + 1, edge_row) == kBlack;
            }
            CHECK(vertical_pairs >= 1);

            CHECK(frame.stats.layout_copies >= 1 && frame.stats.draws_skipped == 0);
            CHECK(frame.notes.empty());
        }

        // The resolve of the game: a LINEAR read of the layout averages the two samples of each pixel.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Linear)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t partial = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t argb = pixelArgb(frame.bgra, frame.width, x, y);
                    const std::uint32_t value = (argb >> 16) & 0xFF;
                    partial += value > 0 && value < 255 && (argb & 0xFFFF) == 0;
                }
            }
            if (partial < 16)
                std::fprintf(stderr, "multisampled resolve at scale %u: %u partial pixels\n", scale, partial);
            CHECK(partial >= 16);
            CHECK(frame.notes.empty());
        }

        // A pass that samples the surface it draws into: the layout is brought up to date and read directly (no
        // scratch copy), the pass shades once per pixel and writes the shade to both samples.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::SelfSample)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    // Both texels of a pixel hold what texel 2X + 1 held before the pass.
                    const std::uint32_t expected = msaaTexelCovered(x | 1, y, scale) ? kRed : kBlack;
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled self-sample at scale %u: %u texels differ\n", scale, mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.draws == 3 && frame.stats.draws_skipped == 0);
            CHECK(frame.stats.feedback_copies == 0 && frame.stats.layout_copies >= 2);
            CHECK(frame.notes.empty());
        }
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// ---- Switching the anti-aliasing mode at run time ----

// The raw display (image size) after each frame of a sequence on one renderer.
struct RawDisplay {
    std::vector<std::uint8_t> bgra;
    std::uint32_t width = 0, height = 0;
};

RawDisplay runFrame(tox2::RsxRenderer& renderer, std::vector<tox2::RsxCommand> commands) {
    RawDisplay display;
    renderer.execute(commands);
    CHECK(renderer.valid());
    CHECK(renderer.readDisplay(display.bgra, display.width, display.height));
    return display;
}

bool sameDisplay(const RawDisplay& a, const RawDisplay& b) {
    return a.width == b.width && a.height == b.height && !a.bgra.empty() && a.bgra == b.bgra;
}

// A renderer's mode follows setAaMode, and the picture follows the mode without any target being touched at the
// switch: a read finds the memory layout brought up to date from the 2-sample image, and a full clear makes the old
// contents of either representation irrelevant.
void testRendererAaModeSwitch(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    {
        tox2::RsxRenderer renderer(ctx, readbackAllOptions(1, tox2::AaMode::Supersample));
        CHECK(renderer.valid() && renderer.aaMode() == tox2::AaMode::Supersample);
        CHECK(renderer.setAaMode(tox2::AaMode::Msaa) == renderer.msaaAvailable());
        CHECK(renderer.aaMode() == (renderer.msaaAvailable() ? tox2::AaMode::Msaa : tox2::AaMode::Supersample));
        CHECK(renderer.setAaMode(tox2::AaMode::Supersample) && renderer.aaMode() == tox2::AaMode::Supersample);
    }
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("anti-aliasing mode switch: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    if (ctx.deviceProperties().limits.standardSampleLocations == VK_FALSE) {
        std::printf("anti-aliasing mode switch: skipped (no standard sample locations)\n");
        return;
    }
    const std::uint32_t errors_before = ctx.validationErrorCount();

    for (const std::uint32_t scale : {1u, 2u}) {
        // The references: one frame of the scene on a fresh renderer in each mode.
        const auto reference = [&](tox2::AaMode mode) {
            RawDisplay display;
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Linear)}, scale, 1, 0, mode);
            CHECK(frame.ok && frame.notes.empty());
            display.bgra = frame.bgra;
            display.width = frame.width;
            display.height = frame.height;
            return display;
        };
        const RawDisplay msaa_reference = reference(tox2::AaMode::Msaa);
        const RawDisplay super_reference = reference(tox2::AaMode::Supersample);
        CHECK(!msaa_reference.bgra.empty() && !super_reference.bgra.empty());
        // The modes must be told apart by the picture, or the equalities below would prove nothing.
        CHECK(!sameDisplay(msaa_reference, super_reference));

        const std::vector<tox2::RsxCommand> drawn = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::Linear));
        const std::vector<tox2::RsxCommand> held = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::LinearHeld));
        const std::vector<tox2::RsxCommand> unread = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::Unread));
        tox2::rsxUnimplementedReset();

        // The full sequence: msaa, supersample read, supersample redraw after a full clear, msaa again.
        {
            tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale, tox2::AaMode::Msaa));
            CHECK(renderer.valid() && renderer.aaMode() == tox2::AaMode::Msaa);

            const RawDisplay first = runFrame(renderer, drawn);
            CHECK(sameDisplay(first, msaa_reference));

            CHECK(renderer.setAaMode(tox2::AaMode::Supersample) && renderer.aaMode() == tox2::AaMode::Supersample);
            const RawDisplay second = runFrame(renderer, held);
            CHECK(sameDisplay(second, first));

            const RawDisplay third = runFrame(renderer, drawn);
            CHECK(sameDisplay(third, super_reference));

            CHECK(renderer.setAaMode(tox2::AaMode::Msaa) && renderer.aaMode() == tox2::AaMode::Msaa);
            const RawDisplay fourth = runFrame(renderer, drawn);
            CHECK(sameDisplay(fourth, first));

            CHECK(renderer.stats().draws_skipped == 0);
            CHECK(tox2::rsxUnimplementedSnapshot().empty());
        }

        // The layout is stale at the switch (nothing read the surface in msaa mode): the first supersample pass over
        // the surface copies it, so the picture is the msaa one; going back without a clear draws over layout data that
        // the sidecar lacks, which is counted, and the picture is still the same.
        {
            tox2::rsxUnimplementedReset();
            tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale, tox2::AaMode::Msaa));
            CHECK(renderer.valid());
            runFrame(renderer, unread);
            CHECK(renderer.stats().layout_copies == 0);

            CHECK(renderer.setAaMode(tox2::AaMode::Supersample));
            const RawDisplay synced = runFrame(renderer, held);
            CHECK(sameDisplay(synced, msaa_reference));
            CHECK(renderer.stats().layout_copies >= 1);
            CHECK(tox2::rsxUnimplementedSnapshot().empty());

            CHECK(renderer.setAaMode(tox2::AaMode::Msaa));
            const RawDisplay back = runFrame(renderer, held);
            CHECK(sameDisplay(back, msaa_reference));
            const auto notes = tox2::rsxUnimplementedSnapshot();
            CHECK(notes.size() == 1 && notes[0].first == "state anti-aliased pass over layout data");
            CHECK(renderer.stats().draws_skipped == 0);
        }
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// Dynamic state a draw has already recorded is not recorded again; anything that binds another pipeline or sets
// state on the command buffer, and a new frame, makes the next draw record all of it.
void testDrawStateShadow(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    constexpr std::uint64_t kDrawStateCalls = 21;  // pipeline bind, 19 dynamic state commands, the jitter push
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    TextureOptions alias;
    alias.scene = TextureScene::Alias;
    const std::vector<tox2::RsxCommand> scene = replaySynthetic(makeTexturedCapture(alias));
    CHECK(scene.size() == 4);  // clear of the aliased target, clear of the display, the draw, the flip
    if (scene.size() != 4 || std::get_if<tox2::RsxDraw>(&scene[2]) == nullptr) return;
    const tox2::RsxCommand& clear_alias = scene[0];
    const tox2::RsxCommand& clear_display = scene[1];
    const tox2::RsxCommand& draw = scene[2];
    const tox2::RsxCommand& flip = scene[3];

    struct Run {
        tox2::RendererStats stats;
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
    };
    const auto run = [&](const tox2::RenderOptions& options, std::vector<tox2::RsxCommand> commands) {
        tox2::RsxRenderer renderer(ctx, options);
        CHECK(renderer.valid());
        renderer.execute(commands);
        CHECK(renderer.valid());
        Run result;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(result.bgra, result.width, height));
        result.stats = renderer.stats();
        CHECK(result.stats.draws_skipped == 0);
        return result;
    };

    // Equal draws in one instance: the first records everything, the others nothing.
    for (const std::size_t count : {1u, 2u, 3u}) {
        std::vector<tox2::RsxCommand> commands = {clear_alias, clear_display};
        for (std::size_t i = 0; i < count; i++) commands.push_back(draw);
        commands.push_back(flip);
        const Run r = run(readbackAllOptions(), commands);
        CHECK(r.stats.state_calls == kDrawStateCalls);
        CHECK(r.stats.state_calls_skipped == kDrawStateCalls * (count - 1));
        CHECK(pixelArgb(r.bgra, r.width, 32, 32) == kAliasColor && pixelArgb(r.bgra, r.width, 2, 2) == 0xFF000000);
    }

    // A masked colour clear binds its own pipeline: the next draw records its state again, and the pixels are those
    // of the draws.
    {
        const std::vector<tox2::RsxCommand> masked =
                replaySynthetic(makeScissoredClearCapture(0xFF000000, 0xFF405060, 0x30, 0, 0, 64, 64));
        const tox2::RsxClear* masked_clear = nullptr;
        for (const tox2::RsxCommand& command : masked) {
            if (const auto* clear = std::get_if<tox2::RsxClear>(&command)) masked_clear = clear;
        }
        CHECK(masked_clear != nullptr);
        if (masked_clear == nullptr) return;
        const Run split = run(readbackAllOptions(), {clear_alias, clear_display, draw, *masked_clear, draw, flip});
        CHECK(split.stats.state_calls == 2 * kDrawStateCalls && split.stats.state_calls_skipped == 0);
        CHECK(pixelArgb(split.bgra, split.width, 32, 32) == kAliasColor);
        // A plain clear records no pipeline or state, so the second draw skips all of it.
        const Run plain = run(readbackAllOptions(), {clear_alias, clear_display, draw, clear_display, draw, flip});
        CHECK(plain.stats.state_calls == kDrawStateCalls && plain.stats.state_calls_skipped == kDrawStateCalls);
    }

    // The state does not carry over into the next frame's command buffer.
    {
        const Run frames = run(readbackAllOptions(), {clear_alias, clear_display, draw, flip, draw, flip});
        CHECK(frames.stats.state_calls == 2 * kDrawStateCalls && frames.stats.state_calls_skipped == 0);
        CHECK(pixelArgb(frames.bgra, frames.width, 32, 32) == kAliasColor);
    }

    // A multisampled surface read by a single-sample pass is synchronised with a pass of its own first.
    if (ctx.twoSampleSupportGap().empty() && ctx.deviceProperties().limits.standardSampleLocations != VK_FALSE) {
        const std::vector<tox2::RsxCommand> layout = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::Linear));
        std::vector<std::size_t> draws;
        for (std::size_t i = 0; i < layout.size(); i++) {
            if (std::get_if<tox2::RsxDraw>(&layout[i]) != nullptr) draws.push_back(i);
        }
        CHECK(draws.size() == 2);
        if (draws.size() == 2) {
            const Run synced = run(readbackAllOptions(1, tox2::AaMode::Msaa), layout);
            CHECK(synced.stats.layout_copies >= 1);
            CHECK(synced.stats.state_calls == 2 * kDrawStateCalls && synced.stats.state_calls_skipped == 0);
            // The read draw again, with the layout up to date: nothing to record.
            std::vector<tox2::RsxCommand> again = layout;
            again.insert(again.begin() + static_cast<std::ptrdiff_t>(draws[1]) + 1, layout[draws[1]]);
            const Run twice = run(readbackAllOptions(1, tox2::AaMode::Msaa), again);
            CHECK(twice.stats.state_calls == 2 * kDrawStateCalls && twice.stats.state_calls_skipped == kDrawStateCalls);
            CHECK(twice.bgra == synced.bgra);
        }
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// ---- Depth of a multisampled surface read as A8R8G8B8 ----

constexpr std::uint32_t kMsStencil = 0x5A;
constexpr std::uint32_t kMsDepthOffset = kAaOffset + 0x10000;  // SceneBuilder::setSurface

enum class MsaaDepthRead {
    Texels,      // the depth as a 64x32 NEAREST texture into a 64x32 display
    EdgeEffect,  // a pass into the AA colour samples the depth over the clip, then the colour is read texel by texel
    Reference,   // the same rectangle into a plain 32x32 surface, whose depth is read as a 32x32 texture
};

// Depth test ALWAYS with depth write and stencil REPLACE of `stencil` when `on`; all three off otherwise.
void setMsaaDepthState(tox2::rsx_synthetic::SceneBuilder& scene, bool on, std::uint32_t stencil = kMsStencil) {
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, on ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetDepthFunc, kCmpAlways);
    scene.write(tox2::kMethodNv4097SetDepthMask, on ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, on ? 1 : 0);
    if (on) writeStencilFace(scene, false, stencilFace(kCmpAlways, stencil, kOpReplace));
}

// The red rectangle at depth 0.5 with stencil `stencil`, drawn into the 2-sample surface (or, for Reference, a
// plain one), then the depth read the way `read` says.
std::vector<std::uint8_t> makeMsaaDepthCapture(MsaaDepthRead read, std::uint32_t stencil = kMsStencil) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    const bool plain = read == MsaaDepthRead::Reference;
    scene.setSurface(plain ? 0 : 3, kAaSize, kAaOffset);
    scene.clear(0xFF000000);
    setMsaaDepthState(scene, true, stencil);
    drawMsaaRect(scene);
    setMsaaDepthState(scene, false);

    if (read == MsaaDepthRead::EdgeEffect) {
        // The edge effect of the game: a pass into the AA colour with the AA depth attached samples the depth. It
        // shades once per pixel, so pixel X reads layout column 2X + 1 (the u range is shifted by an eighth of a
        // pixel, off the boundary between columns 2X and 2X + 1) and writes that one shade into both samples.
        scene.setSurface(3, kAaSize, kAaOffset);
        setQuadRenderState(scene, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.125f / kAaSize, 1.0f + 0.125f / kAaSize);
        setUpTexturedDraw(scene, 1, kMsDepthOffset);
        scene.drawStrip(4);
    }

    if (plain) {
        scene.setSurface(0, kAaSize, kSurfaceOffset);
        scene.clear(0xFF000000);
        setViewportHalfSize(scene, kAaSize / 2.0f, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.0f, 1.0f);
        setUpTexturedDraw(scene, 1, kMsDepthOffset, kAaSize);
    } else {
        scene.setSurface(0, kAaSize * 2, kSurfaceOffset, kAaSize);
        scene.clear(0xFF000000);
        setViewportHalfSize(scene, kAaSize, kAaSize / 2.0f);
        putTexturedFullQuad(scene, 0.0f, 1.0f);
        setUpTexturedDraw(scene, 1, read == MsaaDepthRead::Texels ? kMsDepthOffset : kAaOffset);
    }
    scene.drawStrip(4);
    return scene.finish();
}

// The depth of a mode-3 surface read as A8R8G8B8 (the game's edge effect and F_X32 conversions) holds, per layout
// texel, the word of the sample the layout takes there, converted without a layout copy of the depth.
void testRendererMsaaDepthAsArgb(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("multisampled depth as A8R8G8B8: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    if (ctx.deviceProperties().limits.standardSampleLocations == VK_FALSE) {
        std::printf("multisampled depth as A8R8G8B8: skipped (no standard sample locations)\n");
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();

    for (const std::uint32_t scale : {1u, 2u}) {
        // Both words come from a plain surface: 0.5 sits on a 24-bit rounding boundary, and the cleared depth 1.0
        // reads as the conversion shader gives it (its float rounding carries into the top byte), not as 0xFFFFFF00.
        const ScaledFrame reference =
                renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::Reference)}, scale, 1, 0, tox2::AaMode::Msaa);
        CHECK(reference.ok && reference.width == kAaSize * scale);
        if (!reference.ok) continue;
        const std::uint32_t covered_word = pixelArgb(reference.bgra, reference.width, 10 * scale, 8 * scale);
        const std::uint32_t uncovered_word = pixelArgb(reference.bgra, reference.width, 0, 0);
        CHECK((covered_word & 0xFF) == kMsStencil && (uncovered_word & 0xFF) == 0 && covered_word != uncovered_word);
        CHECK(reference.notes.empty());

        const auto expectedWord = [&](std::uint32_t x, std::uint32_t y) {
            return msaaTexelCovered(x, y, scale) ? covered_word : uncovered_word;
        };

        // Every layout texel of the depth.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::Texels)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0, covered = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    covered += msaaTexelCovered(x, y, scale);
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expectedWord(x, y)) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled depth at scale %u: %u texels differ\n", scale, mismatches);
            CHECK(mismatches == 0 && covered > 0);
            CHECK(frame.stats.depth_conversions >= 1 && frame.stats.draws_skipped == 0);
            CHECK(frame.notes.empty());
        }

        // The edge effect: both texels of pixel X hold the word of layout column 2X + 1.
        {
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::EdgeEffect)}, scale, 1, 0,
                                                    tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expectedWord(x | 1, y)) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled depth edge effect at scale %u: %u texels differ\n", scale,
                             mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.depth_conversions >= 1 && frame.stats.draws_skipped == 0);
            CHECK(frame.notes.empty());
        }
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// ---- Layout copies of a multisampled surface for inherit, rescale and single-sample passes ----

constexpr std::uint32_t kMsInheritStencil = 0xA5;  // bits 0, 2, 5 and 7: every pass of the stencil copy is exercised

enum class MsaaInherit {
    PlainSquare,       // the pair rebound as plain 32x32 surfaces (inherit), the depth read as 32x32
    PlainSquareColor,  // the same inherit, the colour read as 32x32
    PlainWide,    // the pair rebound as plain 64x32 surfaces (the same memory, no inherit), the depth read as 64x32
    RescaleAa,    // second frame of the rescale case: the AA pair bound again by a draw
    RescaleWide,  // second frame of the rescale case: the pair bound as plain 64x32 surfaces, the depth read
};

// A draw that writes no colour and tests depth without writing it, so only the binding of the surfaces matters. With
// `registers_depth` the test never passes and the depth write is on: nothing changes, but the frame builder then knows
// the depth as a render target in a frame that has not drawn to it before (it registers a surface a draw can write).
void drawWithoutWrites(tox2::rsx_synthetic::SceneBuilder& scene, bool registers_depth = false) {
    scene.write(tox2::kMethodNv4097SetColorMask, 0);
    scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);
    scene.write(tox2::kMethodNv4097SetDepthFunc, registers_depth ? kCmpNever : kCmpAlways);
    scene.write(tox2::kMethodNv4097SetDepthMask, registers_depth ? 1 : 0);
    scene.write(tox2::kMethodNv4097SetStencilTestEnable, 0);
    drawMsaaRect(scene);
    scene.write(tox2::kMethodNv4097SetColorMask, 0x01010101u);
    setMsaaDepthState(scene, false);
}

// The rectangle scene of makeMsaaDepthCapture with stencil kMsInheritStencil in the 2-sample surface (omitted for the
// second frame of the rescale case), then the rebinding and the read that `kind` says. The rebinding keeps the rows of
// the 2W x H layout, pitch 2 x kAaSize x 4.
std::vector<std::uint8_t> makeMsaaInheritCapture(MsaaInherit kind, std::uint32_t stencil = kMsInheritStencil) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    const bool second_frame = kind == MsaaInherit::RescaleAa || kind == MsaaInherit::RescaleWide;
    if (!second_frame) {
        scene.setSurface(3, kAaSize, kAaOffset);
        scene.clear(0xFF000000);
        setMsaaDepthState(scene, true, stencil);
        drawMsaaRect(scene);
        setMsaaDepthState(scene, false);
    }
    if (kind == MsaaInherit::RescaleAa) {
        scene.setSurface(3, kAaSize, kAaOffset);
        scene.write(tox2::kMethodNv4097SetDepthTestEnable, 1);  // an unused depth surface is not acquired
        drawMsaaRect(scene);
        return scene.finish();
    }

    const bool wide = kind == MsaaInherit::PlainWide || kind == MsaaInherit::RescaleWide;
    scene.setSurface(0, wide ? kAaSize * 2 : kAaSize, kAaOffset, kAaSize, kAaSize * 2 * 4);
    drawWithoutWrites(scene, second_frame);

    const std::uint32_t read_width = wide ? kAaSize * 2 : kAaSize;
    scene.setSurface(0, read_width, kSurfaceOffset, kAaSize);
    scene.clear(0xFF000000);
    setViewportHalfSize(scene, read_width / 2.0f, kAaSize / 2.0f);
    putTexturedFullQuad(scene, 0.0f, 1.0f);
    setUpTexturedDraw(scene, 1, kind == MsaaInherit::PlainSquareColor ? kAaOffset : kMsDepthOffset, read_width);
    scene.drawStrip(4);
    return scene.finish();
}

// The rectangle scene of makeMsaaDepthCapture with stencil kMsInheritStencil in the 2-sample surface, then the same
// rectangle drawn into a separate plain 32x32 colour surface that binds the pair's depth address as a 32x32 surface
// with the depth test, the depth write and the stencil all off. That draw does not use the depth, so it must not
// resize or copy the 2-sample target. The read then is the colour of the plain surface (`read_depth` false) or, after
// a draw that does test the depth through a plain 64x32 binding of the same memory, the depth as 64x32 (as
// MsaaInherit::PlainWide).
constexpr std::uint32_t kMsUnusedColorOffset = 0x80000;

std::vector<std::uint8_t> makeMsaaUnusedDepthCapture(bool read_depth) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface(3, kAaSize, kAaOffset);
    scene.clear(0xFF000000);
    setMsaaDepthState(scene, true, kMsInheritStencil);
    drawMsaaRect(scene);
    setMsaaDepthState(scene, false);

    scene.setSurface(0, kAaSize, kMsUnusedColorOffset);
    scene.write(tox2::kMethodNv4097SetSurfaceZetaOffset, kMsDepthOffset);
    scene.write(tox2::kMethodNv4097SetSurfacePitchZ, kAaSize * 2 * 4);
    drawMsaaRect(scene);

    if (read_depth) {
        scene.setSurface(0, kAaSize * 2, kAaOffset, kAaSize, kAaSize * 2 * 4);
        drawWithoutWrites(scene);
    }
    const std::uint32_t read_width = read_depth ? kAaSize * 2 : kAaSize;
    scene.setSurface(0, read_width, kSurfaceOffset, kAaSize);
    scene.clear(0xFF000000);
    setViewportHalfSize(scene, read_width / 2.0f, kAaSize / 2.0f);
    putTexturedFullQuad(scene, 0.0f, 1.0f);
    setUpTexturedDraw(scene, 1, read_depth ? kMsDepthOffset : kMsUnusedColorOffset, read_width);
    scene.drawStrip(4);
    return scene.finish();
}

// A mode-3 surface drawn as 2 samples and then bound as a plain surface: the layout copy of the depth and stencil
// feeds the inherited contents of a smaller plain surface, a single-sample pass over the same memory, and the
// rescale of the target.
void testRendererMsaaInherit(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("multisampled inherit: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    if (ctx.deviceProperties().limits.standardSampleLocations == VK_FALSE) {
        std::printf("multisampled inherit: skipped (no standard sample locations)\n");
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    constexpr std::uint32_t kRed = 0xFFFF0000, kBlack = 0xFF000000;

    // The depth words come from a plain surface with the same rectangle, depth and stencil (see the depth test).
    struct Words {
        std::uint32_t covered = 0, uncovered = 0;
    };
    const auto referenceWords = [&](std::uint32_t scale, Words& words) {
        const ScaledFrame reference =
                renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::Reference, kMsInheritStencil)}, scale, 1, 0,
                              tox2::AaMode::Msaa);
        CHECK(reference.ok && reference.width == kAaSize * scale);
        if (!reference.ok) return false;
        words.covered = pixelArgb(reference.bgra, reference.width, 10 * scale, 8 * scale);
        words.uncovered = pixelArgb(reference.bgra, reference.width, 0, 0);
        CHECK((words.covered & 0xFF) == kMsInheritStencil && (words.uncovered & 0xFF) == 0 &&
              words.covered != words.uncovered);
        return true;
    };

    for (const std::uint32_t scale : {1u, 2u}) {
        Words words;
        if (!referenceWords(scale, words)) continue;
        const auto expectedWord = [&](std::uint32_t x, std::uint32_t y) {
            return msaaTexelCovered(x, y, scale) ? words.covered : words.uncovered;
        };

        // Inherit: the left half of the layout (texels X < 32 s) lands in the plain 32x32 surfaces.
        {
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaInheritCapture(MsaaInherit::PlainSquare)}, scale, 1,
                                                    0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0, covered = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    covered += msaaTexelCovered(x, y, scale);
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expectedWord(x, y)) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "inherited multisampled depth at scale %u: %u texels differ\n", scale, mismatches);
            CHECK(mismatches == 0 && covered > 0);
            CHECK(frame.stats.surface_inherits >= 2 && frame.stats.layout_copies >= 2);
            CHECK(frame.stats.draws_skipped == 0 && frame.notes.empty());
        }
        {
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaInheritCapture(MsaaInherit::PlainSquareColor)}, scale,
                                                    1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t expected = msaaTexelCovered(x, y, scale) ? kRed : kBlack;
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "inherited multisampled colour at scale %u: %u texels differ\n", scale,
                             mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.surface_inherits >= 2 && frame.stats.layout_copies >= 2);
            CHECK(frame.stats.draws_skipped == 0 && frame.notes.empty());
        }

        // A single-sample pass over the same memory as a plain 2W x H surface finds the sidecar newer than the
        // layout image and copies the depth first: the whole layout is read back.
        {
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaInheritCapture(MsaaInherit::PlainWide)}, scale, 1, 0,
                                                    tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expectedWord(x, y)) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled depth under a single-sample pass at scale %u: %u texels differ\n",
                             scale, mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.surface_inherits == 0 && frame.stats.layout_copies >= 2);
            CHECK(frame.stats.draws_skipped == 0 && frame.notes.empty());
        }

        // A draw that neither tests, writes nor uses stencil leaves the depth binding alone: no 32x32 depth target is
        // made, so the 2-sample target is neither synced nor copied, and a later draw that tests the depth finds it as
        // it was.
        {
            const ScaledFrame control = renderAtScale(ctx, {makeMsaaInheritCapture(MsaaInherit::PlainWide)}, scale, 1,
                                                      0, tox2::AaMode::Msaa);
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaUnusedDepthCapture(true)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(control.ok && frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!control.ok || !frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expectedWord(x, y)) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "multisampled depth after an unused-depth draw at scale %u: %u texels differ\n",
                             scale, mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.surface_inherits == control.stats.surface_inherits);
            CHECK(frame.stats.layout_copies == control.stats.layout_copies);
            CHECK(frame.stats.draws_skipped == 0 && frame.notes.empty());
        }
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaUnusedDepthCapture(false)}, scale, 1, 0, tox2::AaMode::Msaa);
            CHECK(frame.ok && frame.width == kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const bool inside = (x + 0.5f) / static_cast<float>(scale) >= kMsRectLeft &&
                                        (x + 0.5f) / static_cast<float>(scale) < kMsRectRight &&
                                        (y + 0.5f) / static_cast<float>(scale) >= kMsRectTop &&
                                        (y + 0.5f) / static_cast<float>(scale) < kMsRectBottom;
                    const std::uint32_t argb = pixelArgb(frame.bgra, frame.width, x, y);
                    // The plain surface is cleared by nothing, so only the rectangle is drawn; texels outside it
                    // are the new target's initial contents.
                    if (inside && argb != kRed) mismatches++;
                }
            }
            CHECK(mismatches == 0);
            CHECK(frame.stats.surface_inherits == 0 && frame.stats.layout_copies == 0);
            CHECK(frame.stats.draws_skipped == 0 && frame.notes.empty());
        }
    }

    // Rescale: the scale changes between the frames, and binding the targets again copies the sidecars into the
    // layout images before the blit.
    Words words;
    const bool depth_blit = depthRescaleBlittable(ctx);
    if (!depth_blit)
        std::printf("render tests: multisampled depth rescale skipped: the depth format cannot be blitted\n");
    if (depth_blit && referenceWords(1, words)) {
        const auto runTwoFrames = [&](MsaaInherit second, std::vector<std::uint8_t>* image, std::uint32_t& width,
                                      std::uint32_t& height, std::uint64_t& copies,
                                      std::vector<std::pair<std::string, std::uint64_t>>& notes) {
            tox2::RsxRenderer renderer(ctx, readbackAllOptions(1, tox2::AaMode::Msaa));
            CHECK(renderer.valid());
            std::vector<tox2::RsxCommand> first =
                    replaySynthetic(makeMsaaDepthCapture(MsaaDepthRead::Texels, kMsInheritStencil));
            std::vector<tox2::RsxCommand> rest = replaySynthetic(makeMsaaInheritCapture(second));
            tox2::rsxUnimplementedReset();
            renderer.execute(first);
            const std::uint64_t before = renderer.stats().layout_copies;
            CHECK(renderer.setResolutionScale(2));
            tox2::rsxUnimplementedReset();
            renderer.execute(rest);
            CHECK(renderer.valid());
            copies = renderer.stats().layout_copies - before;
            notes = tox2::rsxUnimplementedSnapshot();
            if (image != nullptr) CHECK(renderer.readDisplay(*image, width, height));
        };

        std::uint32_t width = 0, height = 0;
        std::uint64_t copies = 0;
        std::vector<std::pair<std::string, std::uint64_t>> notes;
        // Bound again as the AA pair by a draw that is not a full clear: both sidecars are synced for the blit, and the
        // new sidecars are behind the rescaled layout images.
        runTwoFrames(MsaaInherit::RescaleAa, nullptr, width, height, copies, notes);
        CHECK(copies >= 2);
        CHECK(!notes.empty());
        for (const auto& [name, count] : notes) {
            if (name != "state anti-aliased pass over layout data")
                std::fprintf(stderr, "unexpected note %s\n", name.c_str());
            CHECK(name == "state anti-aliased pass over layout data");
        }

        // Bound as a plain 64x32 pair: the rescaled depth holds each scale-1 word twice across and down.
        std::vector<std::uint8_t> bgra;
        runTwoFrames(MsaaInherit::RescaleWide, &bgra, width, height, copies, notes);
        CHECK(copies >= 2 && notes.empty());
        CHECK(width == 2 * kAaSize * 2 && height == kAaSize * 2);
        if (width == 2 * kAaSize * 2 && height == kAaSize * 2) {
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < height; y++) {
                for (std::uint32_t x = 0; x < width; x++) {
                    const std::uint32_t expected = msaaTexelCovered(x / 2, y / 2, 1) ? words.covered : words.uncovered;
                    if (pixelArgb(bgra, width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0) std::fprintf(stderr, "rescaled multisampled depth: %u texels differ\n", mismatches);
            CHECK(mismatches == 0);
        }
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// The stencil of a 2-sample depth target reaches its layout image bit for bit. Inherit uses 0xA5; 0x5A (bits 1, 3, 4
// and 6) and 0xFF complete the coverage, so a layout pass that loses or invents any of the eight bits is caught.
void testRendererMsaaLayoutStencilBits(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    if (!ctx.twoSampleSupportGap().empty()) {
        std::printf("multisampled layout stencil: skipped (%s)\n", ctx.twoSampleSupportGap().c_str());
        return;
    }
    if (ctx.deviceProperties().limits.standardSampleLocations == VK_FALSE) {
        std::printf("multisampled layout stencil: skipped (no standard sample locations)\n");
        return;
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    constexpr std::uint32_t kScale = 2;

    for (const std::uint32_t stencil : {0xA5u, 0x5Au, 0xFFu, 0x01u, 0x80u}) {
        // The words of a plain surface with the same rectangle, depth and stencil.
        const ScaledFrame reference = renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::Reference, stencil)},
                                                    kScale, 1, 0, tox2::AaMode::Msaa);
        CHECK(reference.ok && reference.width == kAaSize * kScale);
        if (!reference.ok) continue;
        const std::uint32_t covered_word = pixelArgb(reference.bgra, reference.width, 10 * kScale, 8 * kScale);
        const std::uint32_t uncovered_word = pixelArgb(reference.bgra, reference.width, 0, 0);
        CHECK((covered_word & 0xFF) == stencil && (uncovered_word & 0xFF) == 0);

        // A single-sample pass over the same memory as a plain 2W x H surface reads the layout copy of the depth.
        const ScaledFrame frame = renderAtScale(ctx, {makeMsaaInheritCapture(MsaaInherit::PlainWide, stencil)}, kScale,
                                                1, 0, tox2::AaMode::Msaa);
        CHECK(frame.ok && frame.width == 2 * kAaSize * kScale && frame.height == kAaSize * kScale);
        if (!frame.ok) continue;
        std::uint32_t mismatches = 0, covered = 0;
        for (std::uint32_t y = 0; y < frame.height; y++) {
            for (std::uint32_t x = 0; x < frame.width; x++) {
                const bool is_covered = msaaTexelCovered(x, y, kScale);
                covered += is_covered;
                if (pixelArgb(frame.bgra, frame.width, x, y) != (is_covered ? covered_word : uncovered_word)) {
                    mismatches++;
                }
            }
        }
        if (mismatches != 0) std::fprintf(stderr, "layout stencil 0x%02X: %u texels differ\n", stencil, mismatches);
        CHECK(mismatches == 0 && covered > 0);
        CHECK(frame.stats.layout_copies >= 2 && frame.stats.draws_skipped == 0 && frame.notes.empty());
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// ---- The 1-sample scene image of Dlaa mode ----

// Whether pixel (x, y) of the rectangle's surface is red when it is drawn with one sample at the pixel centre at
// resolution scale `scale`. No rectangle edge passes through a pixel centre at scales 1 and 2.
bool dlaaPixelCovered(std::uint32_t x, std::uint32_t y, std::uint32_t scale) {
    const float guest_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale);
    const float guest_y = (static_cast<float>(y) + 0.5f) / static_cast<float>(scale);
    return guest_x >= kMsRectLeft && guest_x < kMsRectRight && guest_y >= kMsRectTop && guest_y < kMsRectBottom;
}

// In Dlaa mode a mode-3 surface is drawn one sample per pixel at the clip size and reaches its memory layout by a
// copy that gives both texels of a pixel the same value; the depth read as A8R8G8B8 does the same, and switching the
// mode at run time renders each frame like a fresh renderer in that mode.
void testRendererDlaaSceneImage(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    {
        tox2::RsxRenderer probe(ctx, readbackAllOptions(1, tox2::AaMode::Dlaa));
        if (!probe.dlaaAvailable()) {
            std::printf("DLAA scene image: skipped (%s)\n", probe.dlaaUnavailableReason().c_str());
            tox2::rsxUnimplementedReset();
            return;
        }
    }
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    constexpr std::uint32_t kRed = 0xFFFF0000, kBlack = 0xFF000000;

    for (const std::uint32_t scale : {1u, 2u}) {
        // The surface read texel by texel: both texels of a pixel are red iff the pixel centre is in the rectangle.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Texels)}, scale, 1, 0, tox2::AaMode::Dlaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0, red = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t expected = dlaaPixelCovered(x / 2, y, scale) ? kRed : kBlack;
                    red += expected == kRed;
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "DLAA scene image at scale %u: %u texels differ\n", scale, mismatches);
            CHECK(mismatches == 0 && red > 0);
            CHECK(frame.stats.layout_copies >= 1 && frame.stats.draws_skipped == 0);
            CHECK(frame.notes.empty());
        }

        // The game's resolve (a LINEAR read of the layout) averages two equal texels: no partial pixels remain.
        {
            const ScaledFrame frame =
                    renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Linear)}, scale, 1, 0, tox2::AaMode::Dlaa);
            CHECK(frame.ok && frame.width == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t partial = 0, red = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t argb = pixelArgb(frame.bgra, frame.width, x, y);
                    const std::uint32_t value = (argb >> 16) & 0xFF;
                    partial += value > 0 && value < 255;
                    red += argb == kRed;
                }
            }
            CHECK(partial == 0 && red > 0);
            CHECK(frame.notes.empty());
        }

        // The depth read as A8R8G8B8 matches a plain surface's words, texel by texel, and so does a pass into the
        // colour that samples it (the game's edge effect), now that both texels of a pixel are the same sample.
        const ScaledFrame reference =
                renderAtScale(ctx, {makeMsaaDepthCapture(MsaaDepthRead::Reference)}, scale, 1, 0, tox2::AaMode::Dlaa);
        CHECK(reference.ok && reference.width == kAaSize * scale);
        if (!reference.ok) continue;
        const std::uint32_t covered_word = pixelArgb(reference.bgra, reference.width, 10 * scale, 8 * scale);
        const std::uint32_t uncovered_word = pixelArgb(reference.bgra, reference.width, 0, 0);
        CHECK(covered_word != uncovered_word);
        for (const MsaaDepthRead read : {MsaaDepthRead::Texels, MsaaDepthRead::EdgeEffect}) {
            const ScaledFrame frame = renderAtScale(ctx, {makeMsaaDepthCapture(read)}, scale, 1, 0, tox2::AaMode::Dlaa);
            CHECK(frame.ok && frame.width == 2 * kAaSize * scale && frame.height == kAaSize * scale);
            if (!frame.ok) continue;
            std::uint32_t mismatches = 0;
            for (std::uint32_t y = 0; y < frame.height; y++) {
                for (std::uint32_t x = 0; x < frame.width; x++) {
                    const std::uint32_t expected = dlaaPixelCovered(x / 2, y, scale) ? covered_word : uncovered_word;
                    if (pixelArgb(frame.bgra, frame.width, x, y) != expected) mismatches++;
                }
            }
            if (mismatches != 0)
                std::fprintf(stderr, "DLAA depth read %d at scale %u: %u texels differ\n", static_cast<int>(read),
                             scale, mismatches);
            CHECK(mismatches == 0);
            CHECK(frame.stats.depth_conversions >= 1 && frame.stats.draws_skipped == 0);
            CHECK(frame.notes.empty());
        }

        // Msaa -> Dlaa -> Msaa on one renderer: each frame equals a fresh renderer's in that mode. The full clear at
        // the start of the scene makes the older representation irrelevant.
        {
            const auto fresh = [&](tox2::AaMode mode) {
                RawDisplay display;
                const ScaledFrame frame =
                        renderAtScale(ctx, {makeMsaaLayoutCapture(MsaaRead::Linear)}, scale, 1, 0, mode);
                CHECK(frame.ok && frame.notes.empty());
                display.bgra = frame.bgra;
                display.width = frame.width;
                display.height = frame.height;
                return display;
            };
            const RawDisplay msaa_reference = fresh(tox2::AaMode::Msaa);
            const RawDisplay dlaa_reference = fresh(tox2::AaMode::Dlaa);
            CHECK(!sameDisplay(msaa_reference, dlaa_reference));

            const std::vector<tox2::RsxCommand> drawn = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::Linear));
            tox2::rsxUnimplementedReset();
            tox2::RsxRenderer renderer(ctx, readbackAllOptions(scale, tox2::AaMode::Msaa));
            CHECK(renderer.valid() && renderer.aaMode() == tox2::AaMode::Msaa);
            CHECK(sameDisplay(runFrame(renderer, drawn), msaa_reference));
            CHECK(renderer.setAaMode(tox2::AaMode::Dlaa) && renderer.aaMode() == tox2::AaMode::Dlaa);
            CHECK(sameDisplay(runFrame(renderer, drawn), dlaa_reference));
            CHECK(renderer.setAaMode(tox2::AaMode::Msaa) && renderer.aaMode() == tox2::AaMode::Msaa);
            CHECK(sameDisplay(runFrame(renderer, drawn), msaa_reference));
            CHECK(renderer.stats().layout_copies >= 3 && renderer.stats().draws_skipped == 0);
            CHECK(tox2::rsxUnimplementedSnapshot().empty());
        }
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// The game's resolve read of the scene runs DLAA once per scene write: the first evaluation starts a history, the
// second (the next frame) continues it, nothing fails, and the picture keeps the scene's colours. The history is
// reset by a frame without an evaluation and by a switch away from Dlaa and back.
void testRendererDlaaEvaluate(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    {
        tox2::RsxRenderer probe(ctx, readbackAllOptions(1, tox2::AaMode::Dlaa));
        if (!probe.dlaaAvailable()) {
            std::printf("DLAA evaluate: skipped (%s)\n", probe.dlaaUnavailableReason().c_str());
            tox2::rsxUnimplementedReset();
            return;
        }
    }
    const std::uint32_t errors_before = ctx.validationErrorCount();
    const std::vector<tox2::RsxCommand> drawn = replaySynthetic(makeMsaaLayoutCapture(MsaaRead::Linear));
    const std::vector<tox2::RsxCommand> cleared = replaySynthetic(makeClearCapture(0xFF102030));
    for (const std::uint32_t scale : {1u, 2u}) {
        tox2::rsxUnimplementedReset();
        tox2::RenderOptions options = readbackAllOptions(scale, tox2::AaMode::Dlaa);
        options.dlaa_evaluate = true;
        // The scene has no perspective draw, so the camera rules would reset every frame; this test is about the
        // frame-to-frame rules.
        options.dlaa_debug.camera = tox2::DlaaCamera::Off;
        tox2::RsxRenderer renderer(ctx, options);
        CHECK(renderer.valid() && renderer.aaMode() == tox2::AaMode::Dlaa);
        RawDisplay display = runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 1 && renderer.stats().dlaa_resets == 1);
        display = runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 2 && renderer.stats().dlaa_resets == 1);
        CHECK(renderer.stats().dlaa_failures == 0 && renderer.stats().draws_skipped == 0);
        CHECK(tox2::rsxUnimplementedSnapshot().empty());
        // The history only continues from the immediately preceding frame: a frame that evaluates nothing (a plain
        // clear) in between starts a new one ...
        runFrame(renderer, cleared);
        runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 3 && renderer.stats().dlaa_resets == 2);
        runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 4 && renderer.stats().dlaa_resets == 2);
        // ... and so does a switch away from Dlaa and back, even between consecutive frames.
        CHECK(renderer.setAaMode(tox2::AaMode::Msaa) && renderer.setAaMode(tox2::AaMode::Dlaa));
        runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 5 && renderer.stats().dlaa_resets == 3);
        runFrame(renderer, drawn);
        CHECK(renderer.stats().dlaa_evaluations == 6 && renderer.stats().dlaa_resets == 3);
        CHECK(renderer.stats().dlaa_failures == 0 && renderer.stats().draws_skipped == 0);
        CHECK(display.width == kAaSize * scale && display.height == kAaSize * scale);
        if (display.width != kAaSize * scale || display.height != kAaSize * scale) continue;
        // Well inside the red rectangle and well outside it: DLAA may soften edges but not change flat areas much.
        const std::uint32_t inside = pixelArgb(display.bgra, display.width, 10 * scale, 8 * scale);
        const std::uint32_t outside = pixelArgb(display.bgra, display.width, scale, scale);
        CHECK(((inside >> 16) & 0xFF) > 192 && ((inside >> 8) & 0xFF) < 64 && (inside & 0xFF) < 64);
        CHECK(((outside >> 16) & 0xFF) < 64 && ((outside >> 8) & 0xFF) < 64 && (outside & 0xFF) < 64);
        CHECK((inside >> 24) == 0xFF && (outside >> 24) == 0xFF);
    }
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// A frame generation target on a renderer without a presenter is reported unsupported ("no window"), warned about once
// and changes nothing in the picture; the diagnostics default to the measured settings.
void testRendererFrameGenOptions(tox2::VkContext& ctx) {
    using namespace tox2::rsx_synthetic;
    tox2::rsxUnimplementedReset();
    const std::uint32_t errors_before = ctx.validationErrorCount();
    const tox2::RenderOptions defaults;
    CHECK(defaults.frame_gen == tox2::FrameGenTarget::Off);
    CHECK(defaults.fg_debug.present_mode == VK_PRESENT_MODE_MAILBOX_KHR && defaults.fg_debug.reflex_low_latency);
    CHECK(!defaults.fg_debug.only_generated && defaults.fg_debug.cycle_seconds == 0);

    const std::vector<tox2::RsxCommand> drawn = replaySynthetic(makeQuadCapture());
    RawDisplay off_display;
    {
        tox2::RsxRenderer off(ctx, readbackAllOptions());
        CHECK(off.valid() && off.frameGenTarget() == tox2::FrameGenTarget::Off);
        CHECK(!off.frameGenSupported() && off.frameGenUnsupportedReason() == "no window");
        off_display = runFrame(off, drawn);
    }

    std::vector<std::string> warnings;
    tox2::setLogSink([&warnings](std::string_view line) {
        if (line.find("frame generation") != std::string_view::npos) warnings.emplace_back(line);
    });
    {
        tox2::RenderOptions options = readbackAllOptions();
        options.frame_gen = tox2::FrameGenTarget::Fps120;
        tox2::RsxRenderer renderer(ctx, options);
        CHECK(renderer.valid());
        CHECK(!renderer.frameGenSupported() && renderer.frameGenUnsupportedReason() == "no window");
        CHECK(renderer.frameGenTarget() == tox2::FrameGenTarget::Fps120);
        const RawDisplay first = runFrame(renderer, drawn);
        const RawDisplay second = runFrame(renderer, drawn);
        CHECK(sameDisplay(first, off_display) && sameDisplay(second, off_display));
        // A target other than off cannot be set without support; off always can.
        CHECK(!renderer.setFrameGenTarget(tox2::FrameGenTarget::Fps60));
        CHECK(renderer.frameGenTarget() == tox2::FrameGenTarget::Fps120);
        CHECK(renderer.setFrameGenTarget(tox2::FrameGenTarget::Off));
        CHECK(renderer.frameGenTarget() == tox2::FrameGenTarget::Off);
    }
    tox2::setLogSink({});
    for (const std::string& line : warnings) std::printf("frame generation options: log: %s\n", line.c_str());
    CHECK(warnings.size() == 1);
    CHECK(!warnings.empty() &&
          warnings[0].find("frame generation unavailable (no window); presenting without it") != std::string::npos);
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

// ---- Point size ----

// One red point at the centre of the 64x64 surface (a pixel corner), SET_POINT_SIZE `size`.
std::vector<std::uint8_t> makePointCapture(float size) {
    using namespace tox2::rsx_synthetic;
    SceneBuilder scene;
    scene.setSurface();
    scene.clear(0xFF000000);
    setQuadRenderState(scene);
    scene.setFloatAttrib(0, 3, kPositionsOffset);
    scene.setColorAttrib(3, kColorsOffset);
    scene.setPrograms(quadPrograms());
    for (std::uint32_t c = 0; c < 3; c++) scene.memory().putFloat(1, kPositionsOffset + c * 4, 0.0f);
    scene.memory().putWord(1, kColorsOffset, 0xFFFF0000);
    scene.write(tox2::kMethodNv4097SetPointSize, std::bit_cast<std::uint32_t>(size));
    scene.write(tox2::kMethodNv4097SetBeginEnd, static_cast<std::uint32_t>(tox2::RsxPrimitive::Points));
    scene.write(tox2::kMethodNv4097DrawArrays, 0);
    scene.write(tox2::kMethodNv4097SetBeginEnd, 0);
    return scene.finish();
}

// A point of size 4 covers the 4x4 block around the surface centre at scale 1, where gl_PointSize used to be clamped
// to one pixel, and the same block after the box downsample at scales 2 and 3 (the size follows the scale).
void testRendererPointSize(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    if (!ctx.hasLargePoints()) {
        std::printf("skipping the point size test: the device has no large points\n");
        const ScaledFrame frame = renderAtScale(ctx, {makePointCapture(4.0f)}, 2);
        CHECK(frame.ok && unimplementedCount("state point size unavailable") >= 1);
        tox2::rsxUnimplementedReset();
        return;
    }

    for (const std::uint32_t scale : {1u, 2u, 3u}) {
        const ScaledFrame frame = renderAtScale(ctx, {makePointCapture(4.0f)}, scale);
        CHECK(frame.ok && frame.width == 64 && frame.height == 64);
        if (!frame.ok) continue;
        std::uint32_t red = 0;
        bool block = true;
        for (std::uint32_t y = 0; y < frame.height; y++) {
            for (std::uint32_t x = 0; x < frame.width; x++) {
                const std::uint32_t argb = pixelArgb(frame.bgra, frame.width, x, y);
                const bool inside = x >= 30 && x < 34 && y >= 30 && y < 34;
                if (argb == 0xFFFF0000) red++;
                block = block && argb == (inside ? 0xFFFF0000u : kBlack);
            }
        }
        if (!block) std::fprintf(stderr, "point of size 4 at scale %u: %u red pixels\n", scale, red);
        CHECK(block && red == 16);
        CHECK(frame.stats.draws == 1 && frame.stats.draws_skipped == 0 && frame.notes.empty());
    }

    // Size 1 stays one guest pixel; an unset (zero) size is treated as 1 as before.
    for (const float size : {1.0f, 0.0f}) {
        const ScaledFrame frame = renderAtScale(ctx, {makePointCapture(size)}, 2);
        CHECK(frame.ok && frame.stats.draws == 1);
        if (!frame.ok) continue;
        std::uint32_t lit = 0;
        for (std::uint32_t y = 0; y < frame.height; y++) {
            for (std::uint32_t x = 0; x < frame.width; x++) {
                if (pixelArgb(frame.bgra, frame.width, x, y) != kBlack) lit++;
            }
        }
        // A guest pixel wide point centred on a pixel corner lights the 2x2 image pixels around it at scale 2, one
        // in each of the four guest pixels around the corner.
        CHECK(lit == 4);
    }
    tox2::rsxUnimplementedReset();
}

// The sampler cache keys on the forced level: a mip-reading linear sampler differs between level 1 and 16, a
// point-sampled or single-level one does not.
void testForcedAnisotropySampler(tox2::VkContext& ctx) {
    tox2::TextureCache cache(ctx);
    CHECK(cache.valid());

    tox2::RsxTextureDesc linear;
    linear.format = 0x85;  // A8R8G8B8
    linear.width = linear.height = 4;
    linear.mips = 3;
    linear.filter = (6u << 16) | (2u << 24);  // LINEAR_LINEAR, LINEAR
    linear.control0 = (2u * 256u) << 7;       // max LOD 2
    tox2::RsxTextureDesc nearest = linear;
    nearest.filter = (3u << 16) | (1u << 24);  // NEAREST_LINEAR, NEAREST
    tox2::RsxTextureDesc single = linear;
    single.mips = 1;

    const VkSampler linear_off = cache.sampler(linear);
    const VkSampler nearest_off = cache.sampler(nearest);
    const VkSampler single_off = cache.sampler(single);
    CHECK(linear_off != VK_NULL_HANDLE && nearest_off != VK_NULL_HANDLE && single_off != VK_NULL_HANDLE);

    cache.setAnisotropy(16);
    const VkSampler linear_16 = cache.sampler(linear);
    CHECK(linear_16 != VK_NULL_HANDLE);
    if (ctx.hasSamplerAnisotropy() && ctx.maxSamplerAnisotropy() >= 2.0f) {
        CHECK(linear_16 != linear_off);
    } else {
        std::printf("skipping the anisotropic sampler comparison: the device has no sampler anisotropy\n");
        CHECK(linear_16 == linear_off);
    }
    CHECK(cache.sampler(nearest) == nearest_off);
    CHECK(cache.sampler(single) == single_off);
    // The same level again hits the cache; the earlier sampler is still there for frames in flight.
    CHECK(cache.sampler(linear) == linear_16);

    cache.setAnisotropy(1);
    CHECK(cache.sampler(linear) == linear_off);
    CHECK(ctx.validationErrorCount() == 0);
}

// The level is validated, clamped to the device and changeable at run time; a mip-reading draw with forced
// anisotropy renders without validation errors.
void testRendererAnisotropy(tox2::VkContext& ctx) {
    tox2::rsxUnimplementedReset();
    const std::uint64_t errors_before = ctx.validationErrorCount();
    const auto options = [](std::uint32_t level) {
        tox2::RenderOptions render_options = readbackAllOptions();
        render_options.anisotropy = level;
        return render_options;
    };
    const std::uint32_t supported =
            ctx.maxSamplerAnisotropy() >= 2.0f
                    ? std::min<std::uint32_t>(16, static_cast<std::uint32_t>(ctx.maxSamplerAnisotropy()))
                    : 1;

    {
        tox2::RsxRenderer renderer(ctx, options(1));
        CHECK(renderer.valid() && renderer.anisotropy() == 1);
        CHECK(renderer.maxAnisotropy() == ctx.maxSamplerAnisotropy());
    }
    {
        // Not a level: the renderer is invalid.
        tox2::RsxRenderer renderer(ctx, options(3));
        CHECK(!renderer.valid());
    }
    {
        tox2::RsxRenderer renderer(ctx, options(16));
        CHECK(renderer.valid() && renderer.anisotropy() == supported);

        TextureOptions chain;
        chain.scene = TextureScene::Chain;
        chain.min_filter = 6;  // LINEAR_LINEAR: reads the mip chain
        std::vector<tox2::RsxCommand> commands = replaySynthetic(makeTexturedCapture(chain));
        tox2::rsxUnimplementedReset();
        renderer.execute(commands);
        CHECK(renderer.valid() && renderer.stats().draws_skipped == 0);
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        CHECK(renderer.readDisplay(bgra, width, height) && width == 64 && height == 64);

        // Run-time changes: an unknown level changes nothing, a level is clamped to what the device offers.
        CHECK(!renderer.setAnisotropy(0) && renderer.anisotropy() == supported);
        CHECK(!renderer.setAnisotropy(5) && renderer.anisotropy() == supported);
        CHECK(renderer.setAnisotropy(1) && renderer.anisotropy() == 1);
        CHECK(renderer.setAnisotropy(2) && renderer.anisotropy() == std::min<std::uint32_t>(2, supported));
        CHECK(renderer.setAnisotropy(16) && renderer.anisotropy() == supported);
        renderer.execute(commands);
        CHECK(renderer.valid() && renderer.stats().draws_skipped == 0);
        CHECK(renderer.readDisplay(bgra, width, height));
    }
    CHECK(tox2::rsxUnimplementedSnapshot().empty());
    CHECK(ctx.validationErrorCount() == errors_before);
    tox2::rsxUnimplementedReset();
}

bool sameRect(const tox2::PixelRect& rect, std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

// The device choice is a pure function of the device list and the driver preference.
void testChooseVkDevice() {
    using tox2::VkDeviceCandidate;
    using tox2::VkDriverChoice;
    using tox2::VkDriverKind;
    const VkDeviceCandidate kosmic{"Apple M4 Pro", "KosmicKrisp", false, VkDriverKind::KosmicKrisp};
    const VkDeviceCandidate molten{"Apple M4 Pro", "MoltenVK", false, VkDriverKind::MoltenVk};
    const VkDeviceCandidate other{"Software", "llvmpipe", false, VkDriverKind::Other};
    const VkDeviceCandidate discrete_other{"Discrete", "NVIDIA", true, VkDriverKind::Other};
    const VkDeviceCandidate discrete_kosmic{"Discrete", "KosmicKrisp", true, VkDriverKind::KosmicKrisp};

    const auto pick = [](std::vector<VkDeviceCandidate> devices, VkDriverChoice choice) {
        const auto index = tox2::chooseVkDevice(devices, choice);
        return index ? static_cast<int>(*index) : -1;
    };
    // Auto takes MoltenVK over KosmicKrisp in either listing order, and both over other drivers.
    CHECK(pick({kosmic, molten}, VkDriverChoice::Auto) == 1);
    CHECK(pick({molten, kosmic}, VkDriverChoice::Auto) == 0);
    CHECK(pick({other, kosmic, molten}, VkDriverChoice::Auto) == 2);
    CHECK(pick({other, kosmic}, VkDriverChoice::Auto) == 0);
    // Without either, and among equals, the first device wins.
    CHECK(pick({other, other}, VkDriverChoice::Auto) == 0);
    CHECK(pick({molten, molten}, VkDriverChoice::Auto) == 0);
    // A discrete GPU beats the type-independent driver preference.
    CHECK(pick({molten, discrete_other}, VkDriverChoice::Auto) == 1);
    CHECK(pick({molten, discrete_kosmic}, VkDriverChoice::Auto) == 1);

    // A named driver makes only its devices eligible.
    CHECK(pick({kosmic, molten}, VkDriverChoice::KosmicKrisp) == 0);
    CHECK(pick({molten, kosmic}, VkDriverChoice::KosmicKrisp) == 1);
    CHECK(pick({kosmic, molten}, VkDriverChoice::MoltenVk) == 1);
    CHECK(pick({discrete_other, kosmic}, VkDriverChoice::KosmicKrisp) == 1);
    CHECK(pick({kosmic, discrete_kosmic}, VkDriverChoice::KosmicKrisp) == 1);

    const std::vector<VkDeviceCandidate> only_kosmic = {kosmic};
    const auto missing = tox2::chooseVkDevice(only_kosmic, VkDriverChoice::MoltenVk);
    CHECK(!missing && missing.error() == "no Vulkan device with driver MoltenVK (found: Apple M4 Pro (KosmicKrisp))");
    const std::vector<VkDeviceCandidate> mixed = {other, molten};
    const auto missing_kosmic = tox2::chooseVkDevice(mixed, VkDriverChoice::KosmicKrisp);
    CHECK(!missing_kosmic && missing_kosmic.error() ==
                                     "no Vulkan device with driver KosmicKrisp (found: Software (llvmpipe), Apple M4 "
                                     "Pro (MoltenVK))");
    CHECK(!tox2::chooseVkDevice({}, VkDriverChoice::Auto));

    for (const VkDriverChoice choice : {VkDriverChoice::Auto, VkDriverChoice::MoltenVk, VkDriverChoice::KosmicKrisp}) {
        const auto parsed = tox2::parseVkDriverChoice(tox2::vkDriverChoiceName(choice));
        CHECK(parsed && *parsed == choice);
    }
    CHECK(!tox2::parseVkDriverChoice("MoltenVK") && !tox2::parseVkDriverChoice("") &&
          !tox2::parseVkDriverChoice("mvk"));
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

void testChoosePresentMode() {
    constexpr VkPresentModeKHR kFifo = VK_PRESENT_MODE_FIFO_KHR;
    constexpr VkPresentModeKHR kMailbox = VK_PRESENT_MODE_MAILBOX_KHR;
    constexpr VkPresentModeKHR kImmediate = VK_PRESENT_MODE_IMMEDIATE_KHR;
    const std::array<VkPresentModeKHR, 3> all = {kFifo, kMailbox, kImmediate};
    const std::array<VkPresentModeKHR, 2> fifo_mailbox = {kFifo, kMailbox};
    const std::array<VkPresentModeKHR, 2> fifo_immediate = {kFifo, kImmediate};
    const std::array<VkPresentModeKHR, 1> fifo_only = {kFifo};

    std::vector<std::string> lines;
    tox2::setLogSink([&lines](std::string_view line) { lines.emplace_back(line); });
    const auto choose = [&lines](VkPresentModeKHR requested, std::span<const VkPresentModeKHR> offered) {
        lines.clear();
        return tox2::choosePresentMode(requested, offered);
    };
    const auto isWarn = [](const std::string& line) { return line.find("warn: ") != std::string::npos; };

    // FIFO is the off path and the diagnostic request: no line, no fallback.
    CHECK(choose(kFifo, all) == kFifo && lines.empty());
    CHECK(choose(kFifo, fifo_only) == kFifo && lines.empty());
    CHECK(choose(kMailbox, all) == kMailbox && lines.empty());
    CHECK(choose(kImmediate, all) == kImmediate && lines.empty());

    // The chain is requested -> MAILBOX -> IMMEDIATE; the info line names the requested and the chosen mode.
    CHECK(choose(kImmediate, fifo_mailbox) == kMailbox);
    CHECK(lines.size() == 1 && !isWarn(lines[0]) && lines[0].find("immediate") != std::string::npos &&
          lines[0].find("mailbox") != std::string::npos);
    CHECK(choose(kMailbox, fifo_immediate) == kImmediate);
    CHECK(lines.size() == 1 && !isWarn(lines[0]) && lines[0].find("mailbox") != std::string::npos &&
          lines[0].find("immediate") != std::string::npos);

    // Nothing without vsync: FIFO with one warning per call.
    CHECK(choose(kMailbox, fifo_only) == kFifo);
    CHECK(lines.size() == 1 && isWarn(lines[0]) &&
          lines[0].find("no present mode without vsync; using fifo (DLSS-G requires vsync off)") != std::string::npos);
    CHECK(choose(kImmediate, fifo_only) == kFifo && lines.size() == 1 && isWarn(lines[0]));
    CHECK(choose(kFifo, fifo_only) == kFifo && lines.empty());
    tox2::setLogSink({});

    CHECK(tox2::WindowStatus{}.refresh_hz == 0);
}

#if defined(_WIN32)
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
#endif

// The presenter's overlay-facing types; no window is opened.
void testPresenterTypes() {
    const tox2::MessageRoute none;
    CHECK(!none.handled && none.result == 0 && !none.withhold_key);

    tox2::PresenterOptions options;
    CHECK(!options.on_message && !options.on_fullscreen_changed);
    CHECK(options.window_size == tox2::WindowSizePreset::Size720p && !options.fullscreen);
#if defined(_WIN32)
    int calls = 0;
    options.on_message = [&calls](const tox2::WindowMessage& message) {
        calls++;
        CHECK(message.hwnd == nullptr && message.message == 0x100 && message.wparam == 0x70 && message.lparam == -1);
        return tox2::MessageRoute{.withhold_key = true};
    };
    const tox2::MessageRoute route = options.on_message(tox2::WindowMessage{nullptr, 0x100, 0x70, -1});
    CHECK(calls == 1 && route.withhold_key && !route.handled && route.result == 0);

    const tox2::WindowMessage blank;
    CHECK(blank.hwnd == nullptr && blank.message == 0 && blank.wparam == 0 && blank.lparam == 0);
#else
    int calls = 0;
    options.on_message = [&calls](const tox2::WindowMessage& message) {
        calls++;
        CHECK(message.view == nullptr && message.event == nullptr && message.kind == tox2::WindowEventKind::Key);
        CHECK(message.key && message.key->scan_code == 0x3B && message.key->down);
        return tox2::MessageRoute{.withhold_key = true};
    };
    tox2::WindowMessage key_message;
    key_message.kind = tox2::WindowEventKind::Key;
    key_message.key = tox2::KeyMessage{0x3B, true, false};
    const tox2::MessageRoute route = options.on_message(key_message);
    CHECK(calls == 1 && route.withhold_key && !route.handled && route.result == 0);

    const tox2::WindowMessage blank;
    CHECK(blank.view == nullptr && blank.event == nullptr && blank.kind == tox2::WindowEventKind::Other && !blank.key);
#endif

    const tox2::Presenter::Target empty;
    CHECK(empty.view == VK_NULL_HANDLE && empty.format == VK_FORMAT_UNDEFINED);
    tox2::Presenter::Target target;
    target.view = reinterpret_cast<VkImageView>(static_cast<std::uintptr_t>(0x10));
    target.format = VK_FORMAT_B8G8R8A8_UNORM;
    CHECK(target.view != VK_NULL_HANDLE && target.format == VK_FORMAT_B8G8R8A8_UNORM);
}

// Streamline is optional: the status line says why it is off, and when it is on the hooks must be installed.
void testStreamlineContext(tox2::VkContext& ctx) {
    std::printf("streamline: %s\n", ctx.streamlineStatus().c_str());
#if TOX2_HAS_STREAMLINE
    if (ctx.streamlineStatus().find("DLSS supported") == std::string::npos) return;
    CHECK(ctx.streamline() != nullptr);
    const PFN_vkVoidFunction loader_surface = vkGetInstanceProcAddr(ctx.instance(), "vkCreateWin32SurfaceKHR");
    const PFN_vkVoidFunction surface = ctx.instanceProc("vkCreateWin32SurfaceKHR");
    CHECK(surface != nullptr && surface != loader_surface);
    // Streamline's proxy replaces the volk global; the loader still returns the driver's function.
    const PFN_vkVoidFunction loader_present = vkGetDeviceProcAddr(ctx.device(), "vkQueuePresentKHR");
    CHECK(reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR) != loader_present);
#endif
}

// The shared context loads DLSS only, so every frame generation call must fail naming the missing plugins or do
// nothing, while the frame identity still works. A build without the SDK cannot create a Streamline object at all.
void testStreamlineFrameGenApi(tox2::VkContext& ctx) {
#if TOX2_HAS_STREAMLINE
    tox2::Streamline* streamline = ctx.streamline();
    if (streamline == nullptr) {
        std::printf("streamline frame generation api: skipped: %s\n", ctx.streamlineStatus().c_str());
        return;
    }
    const auto notLoaded = [](const std::string& text) { return text.find("not loaded") != std::string::npos; };

    std::string reason;
    tox2::StreamlineRequirements requirements;
    CHECK(!streamline->requirements(tox2::StreamlineFeature::FrameGeneration, requirements, reason) &&
          notLoaded(reason));
    reason.clear();
    CHECK(!streamline->featureSupported(tox2::StreamlineFeature::FrameGeneration, ctx.physicalDevice(), reason) &&
          notLoaded(reason));
    reason.clear();
    CHECK(!streamline->setFrameGenerationLoaded(true, reason) && notLoaded(reason));
    CHECK(!streamline->frameGenerationLoaded());

    streamline->beginFrame(7);
    CHECK(streamline->frameIndex() == 7);
    CHECK(streamline->frameHandle() != nullptr);

    // Without the plugins these return without calling into Streamline.
    for (const tox2::LatencyMarker marker :
         {tox2::LatencyMarker::SimulationStart, tox2::LatencyMarker::SimulationEnd,
          tox2::LatencyMarker::RenderSubmitStart, tox2::LatencyMarker::RenderSubmitEnd,
          tox2::LatencyMarker::PresentStart, tox2::LatencyMarker::PresentEnd}) {
        streamline->latencyMarker(marker);
    }
    streamline->reflexSleep(streamline->frameHandle());
    streamline->freeFrameGen();
    CHECK(streamline->frameIndex() == 7);

    std::string error;
    CHECK(!streamline->setReflex({}, error) && notLoaded(error));
    error.clear();
    CHECK(!streamline->setFrameGenOptions({}, error) && notLoaded(error));
    error.clear();
    tox2::FrameGenState state;
    CHECK(!streamline->frameGenState(state, nullptr, error) && notLoaded(error));
    error.clear();
    CHECK(!streamline->tagFrameGen(VK_NULL_HANDLE, {}, error) && notLoaded(error));
    error.clear();
    CHECK(!streamline->tagBackbufferRect(VK_NULL_HANDLE, 0, 0, 16, 9, error) && notLoaded(error));
    CHECK(streamline->frameGenApiErrors() == 0);
#else
    (void)ctx;
    std::string reason;
    CHECK(tox2::Streamline::load({.frame_generation = true}, reason) == nullptr);
    CHECK(reason == "built without the Streamline SDK");
#endif
}

struct SlTestImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    tox2::SlImage sl;
};

bool createSlTestImage(tox2::VkContext& ctx, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                       std::uint32_t width, std::uint32_t height, SlTestImage& out) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Streamline is told the device memory, so the image owns a whole allocation.
    alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateImage(ctx.allocator(), &image_info, &alloc_info, &out.image, &out.allocation, &allocated) !=
        VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(ctx.device(), &view_info, nullptr, &out.view) != VK_SUCCESS) return false;
    out.sl.image = out.image;
    out.sl.memory = allocated.deviceMemory;
    out.sl.view = out.view;
    out.sl.width = width;
    out.sl.height = height;
    out.sl.format = format;
    out.sl.usage = usage;
    return true;
}

void destroySlTestImage(tox2::VkContext& ctx, SlTestImage& image) {
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device(), image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) vmaDestroyImage(ctx.allocator(), image.image, image.allocation);
    image = {};
}

// Moves the whole image between layouts with full-pipeline barriers; the test is about Streamline, not barrier cost.
void slTestBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkAccessFlags2 src_access, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void slTestMemoryBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

// Two DLAA evaluations through the frame API on synthetic images: a hard black/white diagonal edge must come out
// anti-aliased with the same mean brightness, with no validation error and no Streamline warning.
void testStreamlineEvaluate(tox2::VkContext& ctx) {
    tox2::Streamline* streamline = ctx.streamline();
    if (streamline == nullptr) {
        std::printf("streamline evaluate: skipped: %s\n", ctx.streamlineStatus().c_str());
        return;
    }
    constexpr std::uint32_t kWidth = 640;
    constexpr std::uint32_t kHeight = 360;
    constexpr VkDeviceSize kPixels = static_cast<VkDeviceSize>(kWidth) * kHeight;
    VkDevice device = ctx.device();
    const VkFormat depth_format = ctx.depthStencilFormat();

    // The formats and usages a DLAA pass needs; a device without them cannot run DLAA at all.
    struct FormatNeed {
        VkFormat format;
        VkFormatFeatureFlags features;
    };
    const FormatNeed needs[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
            {depth_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
            {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
            {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                               VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
    };
    for (const FormatNeed& need : needs) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), need.format, &properties);
        if ((properties.optimalTilingFeatures & need.features) != need.features) {
            std::printf("streamline evaluate: skipped: format %d lacks features\n", static_cast<int>(need.format));
            return;
        }
    }

    const std::uint32_t errors_before = ctx.validationErrorCount();
    std::vector<std::string> sl_lines;
    tox2::setLogSink([&sl_lines](std::string_view line) {
        if (line.find("tox2: streamline:") != std::string_view::npos) sl_lines.emplace_back(line);
    });

    constexpr VkImageUsageFlags kInputUsage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    SlTestImage color, depth, motion, output;
    bool ok = createSlTestImage(ctx, VK_FORMAT_B8G8R8A8_UNORM, kInputUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight, color) &&
              createSlTestImage(ctx, depth_format, kInputUsage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, kWidth, kHeight, depth) &&
              createSlTestImage(ctx, VK_FORMAT_R16G16_SFLOAT, kInputUsage | VK_IMAGE_USAGE_STORAGE_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight, motion) &&
              createSlTestImage(ctx, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight, output);
    CHECK(ok);

    // One host-visible buffer holds the input pixels first and the read-back output second.
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = kPixels * 4 * 2;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo buffer_alloc_info{};
    buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_alloc = nullptr;
    VmaAllocationInfo mapped{};
    ok = ok && vmaCreateBuffer(ctx.allocator(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc, &mapped) ==
                       VK_SUCCESS;
    CHECK(ok && mapped.pMappedData != nullptr);

    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::uint32_t viewport = 0;
    bool viewport_made = false;
    if (ok && mapped.pMappedData != nullptr) {
        auto* bytes = static_cast<std::uint8_t*>(mapped.pMappedData);
        // B8G8R8A8: black above the diagonal from the top left to the bottom right, white below it.
        double input_sum = 0.0;
        for (std::uint32_t y = 0; y < kHeight; y++) {
            for (std::uint32_t x = 0; x < kWidth; x++) {
                const std::uint8_t value =
                        static_cast<std::uint64_t>(x) * kHeight >= static_cast<std::uint64_t>(y) * kWidth ? 255 : 0;
                std::uint8_t* pixel = bytes + (static_cast<std::size_t>(y) * kWidth + x) * 4;
                pixel[0] = pixel[1] = pixel[2] = value;
                pixel[3] = 255;
                input_sum += value;
            }
        }
        const double input_mean = input_sum / static_cast<double>(kPixels);
        vmaFlushAllocation(ctx.allocator(), buffer_alloc, 0, VK_WHOLE_SIZE);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
        CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        CHECK(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

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

        constexpr VkImageAspectFlags kColorAspect = VK_IMAGE_ASPECT_COLOR_BIT;
        // Colour: upload from the host buffer.
        slTestBarrier(cmd, color.image, kColorAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy upload{};
        upload.imageSubresource = {kColorAspect, 0, 0, 1};
        upload.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyBufferToImage(cmd, buffer, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
        slTestBarrier(cmd, color.image, kColorAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
        // Depth: a constant 0.5.
        const VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        slTestBarrier(cmd, depth.image, depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue depth_clear = {0.5f, 0};
        const VkImageSubresourceRange depth_range = {depth_aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth_clear, 1,
                                    &depth_range);
        slTestBarrier(cmd, depth.image, depth_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
        // Motion vectors: zero.
        slTestBarrier(cmd, motion.image, kColorAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange color_range = {kColorAspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &color_range);
        slTestBarrier(cmd, motion.image, kColorAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT);
        // Output: written by Streamline as a storage image.
        slTestBarrier(cmd, output.image, kColorAspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        viewport = streamline->newViewport();
        viewport_made = true;
        // Viewport 0 is reserved for frame generation.
        CHECK(viewport == 1);
        tox2::DlaaFrame frame;
        frame.viewport = viewport;
        frame.frame_index = 7;
        frame.color_in = color.sl;
        frame.color_in.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame.depth = depth.sl;
        frame.depth.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame.motion = motion.sl;
        frame.motion.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame.color_out = output.sl;
        frame.color_out.layout = VK_IMAGE_LAYOUT_GENERAL;
        frame.mvec_scale[0] = 1.0f / kWidth;
        frame.mvec_scale[1] = 1.0f / kHeight;
        frame.alpha = true;
        frame.mvec_invalid_value = 0.0f;
        bool evaluated = true;
        const void* first_handle = nullptr;
        for (std::uint64_t i = 0; i < 3; i++) {
            // Evaluations 0 and 1 are the same frame (index 7) with identical constants, which Streamline requires
            // within a frame; evaluation 2 is the next frame and uses the history evaluation 0 started.
            frame.frame_index = i < 2 ? 7 : 8;
            frame.reset = i < 2;
            const std::array<float, 2> jitter = tox2::dlaaJitterPixels(i < 2 ? 0 : 1);
            frame.jitter_px[0] = jitter[0];
            frame.jitter_px[1] = jitter[1];
            std::string error;
            if (!streamline->evaluateDlaa(cmd, frame, error)) {
                std::printf("streamline evaluate: evaluation %llu failed: %s\n", static_cast<unsigned long long>(i),
                            error.c_str());
                evaluated = false;
                break;
            }
            // Streamline leaves no bound state behind that the test relies on; make its writes visible to the
            // next evaluation and to the read-back.
            slTestMemoryBarrier(cmd);
            if (i == 0) first_handle = streamline->frameHandle();
            if (i == 1) {
                // The frame's second evaluation reuses its token.
                CHECK(streamline->frameIndex() == 7);
                CHECK(first_handle != nullptr && streamline->frameHandle() == first_handle);
            }
        }
        CHECK(evaluated);
        CHECK(streamline->frameIndex() == 8);

        slTestBarrier(cmd, output.image, kColorAspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy readback{};
        readback.bufferOffset = kPixels * 4;
        readback.imageSubresource = {kColorAspect, 0, 0, 1};
        readback.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &readback);
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
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30'000'000'000ull) == VK_SUCCESS);

        if (evaluated) {
            vmaInvalidateAllocation(ctx.allocator(), buffer_alloc, 0, VK_WHOLE_SIZE);
            const std::uint8_t* out = bytes + kPixels * 4;
            double sum[3] = {0.0, 0.0, 0.0};
            std::uint64_t nonzero = 0;
            std::uint64_t edge_pixels = 0;
            for (VkDeviceSize i = 0; i < kPixels; i++) {
                const std::uint8_t* pixel = out + i * 4;
                for (int c = 0; c < 3; c++) sum[c] += pixel[c];
                if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 0) nonzero++;
                if (pixel[0] > 0 && pixel[0] < 255) edge_pixels++;
            }
            const double mean_r = sum[0] / static_cast<double>(kPixels);
            const double mean_g = sum[1] / static_cast<double>(kPixels);
            const double mean_b = sum[2] / static_cast<double>(kPixels);
            std::printf(
                    "streamline evaluate: input mean %.2f, output mean %.2f/%.2f/%.2f, %llu non-black, %llu partial\n",
                    input_mean, mean_r, mean_g, mean_b, static_cast<unsigned long long>(nonzero),
                    static_cast<unsigned long long>(edge_pixels));
            CHECK(nonzero > 0);
            CHECK(std::abs(mean_r - input_mean) <= 8.0);
            CHECK(std::abs(mean_g - input_mean) <= 8.0);
            CHECK(std::abs(mean_b - input_mean) <= 8.0);
            CHECK(edge_pixels >= 16);
        }
    }

    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx.allocator(), buffer, buffer_alloc);
    vkDeviceWaitIdle(device);
    if (viewport_made) streamline->freeViewport(viewport);
    destroySlTestImage(ctx, output);
    destroySlTestImage(ctx, motion);
    destroySlTestImage(ctx, depth);
    destroySlTestImage(ctx, color);

    tox2::setLogSink({});
    for (const std::string& line : sl_lines) std::printf("streamline evaluate: log: %s\n", line.c_str());
    CHECK(sl_lines.empty());
    CHECK(ctx.validationErrorCount() == errors_before);
}

// One-time-submit command buffer on the graphics queue; returns once it has completed.
template <typename Record>
bool fgTestSubmit(tox2::VkContext& ctx, Record&& record) {
    VkDevice device = ctx.device();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    bool ok = vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS &&
              vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS;
    if (ok) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.commandPool = pool;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandBufferCount = 1;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok = vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS &&
             vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS;
        if (ok) {
            record(cmd);
            VkCommandBufferSubmitInfo cmd_submit{};
            cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmd_submit.commandBuffer = cmd;
            VkSubmitInfo2 submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &cmd_submit;
            ok = vkEndCommandBuffer(cmd) == VK_SUCCESS &&
                 vkQueueSubmit2(ctx.graphicsQueue(), 1, &submit, fence) == VK_SUCCESS &&
                 vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
        }
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
    return ok;
}

struct FgHostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
};

bool createFgHostBuffer(tox2::VkContext& ctx, VkDeviceSize size, FgHostBuffer& out) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx.allocator(), &buffer_info, &alloc_info, &out.buffer, &out.allocation, &info) !=
        VK_SUCCESS) {
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    out.mapped = info.pMappedData;
    return out.mapped != nullptr;
}

void destroyFgHostBuffer(tox2::VkContext& ctx, FgHostBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx.allocator(), buffer.buffer, buffer.allocation);
    buffer = {};
}

// Makes the device's writes to the buffer visible to the host after the submit completed.
void fgHostBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

float halfToFloat(std::uint16_t half) {
    const float sign = (half >> 15) != 0 ? -1.0f : 1.0f;
    const int exponent = (half >> 10) & 0x1F;
    const int mantissa = half & 0x3FF;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 31) return sign * std::numeric_limits<float>::infinity();
    return sign * std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
}

// Row-vector world-to-clip matrix of a pinhole camera: translate to the eye, yaw about y, then a projection with
// depth 0..1 and clip w = view depth.
tox2::Mat4 fgTestWorldToClip(float eye_x, float eye_y, float eye_z, float yaw_degrees) {
    constexpr float kNear = 1.0f, kFar = 100.0f, kSx = 0.5625f, kSy = 1.0f;
    const float yaw = yaw_degrees * 3.14159265f / 180.0f;
    const float c = std::cos(yaw), s = std::sin(yaw);
    const tox2::Mat4 translate = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eye_x, -eye_y, -eye_z, 1};
    const tox2::Mat4 rotate = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
    const tox2::Mat4 project = {
            kSx, 0, 0, 0, 0, kSy, 0, 0, 0, 0, kFar / (kFar - kNear), 1, 0, 0, -kNear * kFar / (kFar - kNear), 0};
    return tox2::mat4Multiply(tox2::mat4Multiply(translate, rotate), project);
}

// The motion vector pass on a tilted depth plane: the read-back vectors match the formula evaluated on the CPU, the
// identity matrix gives zeros and a flip sign negates only that component.
void testFgMotionVectors(tox2::VkContext& ctx) {
    if (!ctx.hasD24S8()) {
        std::printf("fg motion vectors: skipped: no D24S8\n");
        return;
    }
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 36;
    constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    // depth = 0.3 + 0.004 x + 0.003 y, as the 24-bit values the image stores.
    std::vector<std::uint32_t> depth_words(kPixels);
    std::vector<float> depth_values(kPixels);
    for (std::uint32_t y = 0; y < kHeight; y++) {
        for (std::uint32_t x = 0; x < kWidth; x++) {
            const double depth = 0.3 + 0.004 * x + 0.003 * y;
            const auto word = static_cast<std::uint32_t>(std::lround(depth * 16777215.0));
            depth_words[y * kWidth + x] = word;
            depth_values[y * kWidth + x] = static_cast<float>(word) / 16777215.0f;
        }
    }

    SlTestImage depth;
    FgHostBuffer upload, readback;
    bool ok = createSlTestImage(ctx, ctx.depthStencilFormat(),
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, kWidth, kHeight, depth) &&
              createFgHostBuffer(ctx, kPixels * 4, upload) && createFgHostBuffer(ctx, kPixels * 4, readback);
    CHECK(ok);
    auto pass = std::make_unique<tox2::FgMotionPass>(ctx);
    std::shared_ptr<void> retired;
    bool recreated = false;
    std::string error;
    ok = ok && pass->prepare(kWidth, kHeight, retired, recreated, error);
    CHECK(ok && recreated && retired == nullptr);
    if (!ok) {
        std::printf("fg motion vectors: %s\n", error.c_str());
    } else {
        std::memcpy(upload.mapped, depth_words.data(), kPixels * 4);
        vmaFlushAllocation(ctx.allocator(), upload.allocation, 0, VK_WHOLE_SIZE);
        // D24_UNORM_S8_UINT buffer texels hold the depth in their low 24 bits.
        const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            slTestBarrier(cmd, depth.image, both, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            copy.imageExtent = {kWidth, kHeight, 1};
            vkCmdCopyBufferToImage(cmd, upload.buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            slTestBarrier(cmd, depth.image, both, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_ACCESS_2_SHADER_READ_BIT);
        }));

        // The current camera moved right and up a little and turned 2 degrees; the previous one is at the origin.
        const tox2::Mat4 current = fgTestWorldToClip(0.4f, 0.15f, 0.0f, 2.0f);
        const tox2::Mat4 previous = fgTestWorldToClip(0.0f, 0.0f, 0.0f, 0.0f);
        tox2::Mat4 current_inverse{};
        CHECK(tox2::mat4Inverse(current, current_inverse));
        const tox2::Mat4 clip_to_prev_clip = tox2::mat4Multiply(current_inverse, previous);

        // Runs the pass and returns the vectors as floats (x, y per pixel).
        const auto run = [&](const tox2::Mat4& matrix, float flip_x, float flip_y) {
            std::vector<float> vectors(kPixels * 2, 1000.0f);
            tox2::FgMotionInputs inputs;
            inputs.depth = depth.view;
            inputs.clip_to_prev_clip = matrix;
            inputs.flip[0] = flip_x;
            inputs.flip[1] = flip_y;
            std::string run_error;
            const bool done = fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
                CHECK(pass->recordMotion(cmd, inputs, run_error));
                const tox2::SlImage motion = pass->motionImage();
                slTestBarrier(cmd, motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {kWidth, kHeight, 1};
                vkCmdCopyImageToBuffer(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                       &copy);
                fgHostBarrier(cmd);
            });
            CHECK(done);
            if (!run_error.empty()) std::printf("fg motion vectors: %s\n", run_error.c_str());
            if (done) {
                vmaInvalidateAllocation(ctx.allocator(), readback.allocation, 0, VK_WHOLE_SIZE);
                const auto* halves = static_cast<const std::uint16_t*>(readback.mapped);
                for (std::size_t i = 0; i < kPixels * 2; i++) vectors[i] = halfToFloat(halves[i]);
            }
            return vectors;
        };

        // The formula of the pass: previous minus current position in pixels, y down.
        const auto expected = [&](std::uint32_t x, std::uint32_t y, float& mv_x, float& mv_y) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const std::array<float, 4> clip = {px / kWidth * 2.0f - 1.0f, py / kHeight * 2.0f - 1.0f,
                                               depth_values[y * kWidth + x], 1.0f};
            const std::array<float, 4> q = tox2::mat4Transform(clip, clip_to_prev_clip);
            CHECK(q[3] > 0.0f);
            mv_x = (q[0] / q[3] + 1.0f) * 0.5f * kWidth - px;
            mv_y = (q[1] / q[3] + 1.0f) * 0.5f * kHeight - py;
        };

        const std::vector<float> moved = run(clip_to_prev_clip, 1.0f, 1.0f);
        const std::vector<float> flipped = run(clip_to_prev_clip, -1.0f, 1.0f);
        const std::vector<float> still = run(tox2::mat4Identity(), 1.0f, 1.0f);

        float largest = 0.0f, worst = 0.0f, worst_flip = 0.0f;
        for (std::uint32_t i = 0; i < 20; i++) {
            const std::uint32_t x = (i * 7 + 3) % kWidth;
            const std::uint32_t y = (i * 5 + 2) % kHeight;
            float mv_x = 0.0f, mv_y = 0.0f;
            expected(x, y, mv_x, mv_y);
            const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 2;
            worst = std::max({worst, std::abs(moved[at] - mv_x), std::abs(moved[at + 1] - mv_y)});
            worst_flip = std::max(
                    {worst_flip, std::abs(flipped[at] + moved[at]), std::abs(flipped[at + 1] - moved[at + 1])});
            largest = std::max({largest, std::abs(mv_x), std::abs(mv_y)});
        }
        std::printf("fg motion vectors: largest %.2f px, worst error %.4f px\n", largest, worst);
        // A transposed matrix or a wrong sign would give other vectors, so the plane must really move.
        CHECK(largest > 1.0f);
        CHECK(worst < 0.01f);
        CHECK(worst_flip < 1e-6f);

        float still_largest = 0.0f;
        for (const float value : still) still_largest = std::max(still_largest, std::abs(value));
        CHECK(still_largest < 0.01f);
    }

    pass.reset();
    vkDeviceWaitIdle(ctx.device());
    destroyFgHostBuffer(ctx, readback);
    destroyFgHostBuffer(ctx, upload);
    destroySlTestImage(ctx, depth);
    CHECK(ctx.validationErrorCount() == errors_before);
}

// The warp check on a gradient shifted by (+3, -2) pixels: motion (-3, +2) compensates it exactly, so only the
// pixels whose compensated position is inside the image are counted and compare equal.
void testFgWarpCheck(tox2::VkContext& ctx) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 36;
    constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    // value(x, y) = 3x + y in every colour channel; the current image is the previous one moved right by 3 and up by
    // 2, and 0 where nothing moved in.
    const auto gradient = [](std::uint32_t x, std::uint32_t y) { return static_cast<std::uint8_t>(3 * x + y); };
    std::vector<std::uint8_t> previous_bytes(kPixels * 4), current_bytes(kPixels * 4);
    for (std::uint32_t y = 0; y < kHeight; y++) {
        for (std::uint32_t x = 0; x < kWidth; x++) {
            const bool from_inside = x >= 3 && y + 2 < kHeight;
            const std::uint8_t previous_value = gradient(x, y);
            const std::uint8_t current_value = from_inside ? gradient(x - 3, y + 2) : 0;
            std::uint8_t* previous_pixel = &previous_bytes[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            std::uint8_t* current_pixel = &current_bytes[(static_cast<std::size_t>(y) * kWidth + x) * 4];
            previous_pixel[0] = previous_pixel[1] = previous_pixel[2] = previous_value;
            current_pixel[0] = current_pixel[1] = current_pixel[2] = current_value;
            previous_pixel[3] = current_pixel[3] = 255;
        }
    }

    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    SlTestImage previous, current, motion;
    FgHostBuffer upload;
    bool ok = createSlTestImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kUsage, VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight,
                                previous) &&
              createSlTestImage(ctx, VK_FORMAT_R8G8B8A8_UNORM, kUsage, VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight,
                                current) &&
              createSlTestImage(ctx, VK_FORMAT_R16G16_SFLOAT, kUsage, VK_IMAGE_ASPECT_COLOR_BIT, kWidth, kHeight,
                                motion) &&
              createFgHostBuffer(ctx, kPixels * 4 * 2, upload);
    CHECK(ok);
    auto pass = std::make_unique<tox2::FgMotionPass>(ctx);
    std::shared_ptr<void> retired;
    bool recreated = false;
    std::string error;
    ok = ok && pass->prepare(kWidth, kHeight, retired, recreated, error);
    CHECK(ok);
    if (!ok) {
        std::printf("fg warp check: %s\n", error.c_str());
    } else {
        auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
        std::memcpy(bytes, previous_bytes.data(), kPixels * 4);
        std::memcpy(bytes + kPixels * 4, current_bytes.data(), kPixels * 4);
        vmaFlushAllocation(ctx.allocator(), upload.allocation, 0, VK_WHOLE_SIZE);

        constexpr VkImageAspectFlags kColor = VK_IMAGE_ASPECT_COLOR_BIT;
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            const SlTestImage* images[2] = {&previous, &current};
            for (std::size_t i = 0; i < 2; i++) {
                slTestBarrier(cmd, images[i]->image, kColor, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy copy{};
                copy.bufferOffset = i * kPixels * 4;
                copy.imageSubresource = {kColor, 0, 0, 1};
                copy.imageExtent = {kWidth, kHeight, 1};
                vkCmdCopyBufferToImage(cmd, upload.buffer, images[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &copy);
                slTestBarrier(cmd, images[i]->image, kColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_ACCESS_2_SHADER_READ_BIT);
            }
            // Previous minus current position of the moved content: (-3, +2) everywhere.
            slTestBarrier(cmd, motion.image, kColor, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue vector{};
            vector.float32[0] = -3.0f;
            vector.float32[1] = 2.0f;
            const VkImageSubresourceRange range = {kColor, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &vector, 1, &range);
            slTestBarrier(cmd, motion.image, kColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_ACCESS_2_SHADER_READ_BIT);
        }));

        // The first call only seeds the previous colour; the second compares against it.
        std::string check_error;
        tox2::FgWarpStats stats;
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            CHECK(pass->recordWarpCheck(cmd, previous.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion.view,
                                        check_error));
        }));
        CHECK(!pass->collectWarpStats(stats));
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            CHECK(pass->recordWarpCheck(cmd, current.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion.view,
                                        check_error));
        }));
        if (!check_error.empty()) std::printf("fg warp check: %s\n", check_error.c_str());
        CHECK(pass->collectWarpStats(stats));
        std::printf("fg warp check: warp mean %.3f, no-motion mean %.3f, %u pixels\n", stats.warp_mean,
                    stats.no_motion_mean, stats.pixels);
        CHECK(stats.pixels == (kWidth - 3) * (kHeight - 2));
        CHECK(stats.warp_mean < 0.5);
        CHECK(stats.no_motion_mean > 5.0);
        CHECK(std::abs(stats.no_motion_mean - 7.0) < 0.05);
        CHECK(!pass->collectWarpStats(stats));
    }

    pass.reset();
    vkDeviceWaitIdle(ctx.device());
    destroyFgHostBuffer(ctx, upload);
    destroySlTestImage(ctx, motion);
    destroySlTestImage(ctx, current);
    destroySlTestImage(ctx, previous);
    CHECK(ctx.validationErrorCount() == errors_before);
}

// The object image: cleared once per frame to (0, kObjectMotionNone) by the first beginObjectWrites, kept by a second
// call in the same frame (a rectangle written in between survives) and cleared again by the next frame.
void testFgMotionObjectImage(tox2::VkContext& ctx) {
    constexpr std::uint32_t kWidth = 64, kHeight = 36;
    constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;
    constexpr VkImageAspectFlags kColor = VK_IMAGE_ASPECT_COLOR_BIT;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    FgHostBuffer readback;
    CHECK(createFgHostBuffer(ctx, kPixels * 8, readback));
    auto pass = std::make_unique<tox2::FgMotionPass>(ctx);
    std::shared_ptr<void> retired;
    bool recreated = false;
    std::string error;
    const bool ok = readback.mapped != nullptr && pass->prepare(kWidth, kHeight, retired, recreated, error);
    CHECK(ok);
    if (!ok) {
        std::printf("fg motion object image: %s\n", error.c_str());
    } else {
        CHECK(pass->objectImage() != VK_NULL_HANDLE && pass->objectView() != VK_NULL_HANDLE);
        constexpr std::uint32_t kRectX = 8, kRectY = 4, kRectW = 16, kRectH = 8;

        // Copies the object image to the host buffer and puts it back in the layout the pass leaves it in.
        const auto readBack = [&](VkCommandBuffer cmd) {
            slTestBarrier(cmd, pass->objectImage(), kColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {kColor, 0, 0, 1};
            copy.imageExtent = {kWidth, kHeight, 1};
            vkCmdCopyImageToBuffer(cmd, pass->objectImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                   &copy);
            fgHostBarrier(cmd);
            slTestBarrier(cmd, pass->objectImage(), kColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        };
        // Counts the pixels equal to `value` inside and outside the rectangle.
        const auto countPixels = [&](std::uint32_t x_value, std::uint32_t y_value, std::size_t& inside,
                                     std::size_t& outside) {
            vmaInvalidateAllocation(ctx.allocator(), readback.allocation, 0, VK_WHOLE_SIZE);
            const auto* words = static_cast<const std::uint32_t*>(readback.mapped);
            inside = outside = 0;
            for (std::uint32_t y = 0; y < kHeight; y++) {
                for (std::uint32_t x = 0; x < kWidth; x++) {
                    const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 2;
                    if (words[at] != x_value || words[at + 1] != y_value) continue;
                    const bool in_rect = x >= kRectX && x < kRectX + kRectW && y >= kRectY && y < kRectY + kRectH;
                    (in_rect ? inside : outside)++;
                }
            }
        };

        std::size_t inside = 0, outside = 0;
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            pass->beginObjectWrites(cmd, 1);
            readBack(cmd);
        }));
        countPixels(0, tox2::kObjectMotionNone, inside, outside);
        CHECK(inside + outside == kPixels);

        // A rectangle written inside a rendering instance on the view, then the same frame's second call.
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            VkRenderingAttachmentInfo attachment{};
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            attachment.imageView = pass->objectView();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {kWidth, kHeight}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            vkCmdBeginRendering(cmd, &rendering);
            VkClearAttachment clear{};
            clear.aspectMask = kColor;
            clear.colorAttachment = 0;
            clear.clearValue.color.uint32[0] = 1;
            clear.clearValue.color.uint32[1] = 2;
            const VkClearRect rect = {{{kRectX, kRectY}, {kRectW, kRectH}}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
            vkCmdEndRendering(cmd);
            pass->beginObjectWrites(cmd, 1);
            readBack(cmd);
        }));
        countPixels(1, 2, inside, outside);
        std::printf("fg motion object image: same frame keeps %zu rectangle pixels\n", inside);
        CHECK(inside == static_cast<std::size_t>(kRectW) * kRectH && outside == 0);

        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            pass->beginObjectWrites(cmd, 2);
            readBack(cmd);
        }));
        countPixels(0, tox2::kObjectMotionNone, inside, outside);
        CHECK(inside + outside == kPixels);
    }

    pass.reset();
    vkDeviceWaitIdle(ctx.device());
    destroyFgHostBuffer(ctx, readback);
    CHECK(ctx.validationErrorCount() == errors_before);
}

// The combine pass over a constant depth: pixels whose object image entry is a vector and the depth the draw wrote get
// that vector, a depth off by two 24-bit steps or no entry gets the camera vector (zero for the identity matrix).
void testFgMotionObjectCombine(tox2::VkContext& ctx) {
    if (!ctx.hasD24S8()) {
        std::printf("fg motion object combine: skipped: no D24S8\n");
        return;
    }
    constexpr std::uint32_t kWidth = 64, kHeight = 36;
    constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;
    constexpr VkImageAspectFlags kColor = VK_IMAGE_ASPECT_COLOR_BIT;
    const std::uint32_t errors_before = ctx.validationErrorCount();

    constexpr std::uint32_t kDepthWord = 8388608;  // depth 0.5 as the 24-bit value the image stores
    const float depth_value = static_cast<float>(kDepthWord) / 16777215.0f;
    const auto depthBits = [](std::uint32_t word) {
        return std::bit_cast<std::uint32_t>(static_cast<float>(word) / 16777215.0f);
    };
    // packHalf2x16(3, -2): x in the low half. 3.0 is 0x4200 and -2.0 is 0xC000 as half floats.
    constexpr std::uint32_t kPackedVector = 0x4200u | (0xC000u << 16);
    struct Rect {
        std::uint32_t x, y, w, h, vector, depth;
    };
    // A valid entry, and an entry whose depth is off by two steps (a later draw changed the pixel).
    const Rect rects[2] = {{4, 4, 16, 8, kPackedVector, depthBits(kDepthWord)},
                           {30, 4, 16, 8, kPackedVector, depthBits(kDepthWord + 2)}};
    const auto inRect = [&](const Rect& rect, std::uint32_t x, std::uint32_t y) {
        return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
    };

    SlTestImage depth;
    FgHostBuffer upload, readback;
    bool ok = createSlTestImage(ctx, ctx.depthStencilFormat(),
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, kWidth, kHeight, depth) &&
              createFgHostBuffer(ctx, kPixels * 4, upload) && createFgHostBuffer(ctx, kPixels * 4, readback);
    CHECK(ok);
    auto pass = std::make_unique<tox2::FgMotionPass>(ctx);
    std::shared_ptr<void> retired;
    bool recreated = false;
    std::string error;
    ok = ok && pass->prepare(kWidth, kHeight, retired, recreated, error);
    CHECK(ok);
    if (!ok) {
        std::printf("fg motion object combine: %s\n", error.c_str());
    } else {
        auto* words = static_cast<std::uint32_t*>(upload.mapped);
        for (std::size_t i = 0; i < kPixels; i++) words[i] = kDepthWord;
        vmaFlushAllocation(ctx.allocator(), upload.allocation, 0, VK_WHOLE_SIZE);
        const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            slTestBarrier(cmd, depth.image, both, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            copy.imageExtent = {kWidth, kHeight, 1};
            vkCmdCopyBufferToImage(cmd, upload.buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            slTestBarrier(cmd, depth.image, both, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_ACCESS_2_SHADER_READ_BIT);
        }));

        // The object image of frame 1: the two rectangles on a cleared (no entry) image, as a scene pass leaves it.
        CHECK(!pass->hasObjectWrites(1));
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            pass->beginObjectWrites(cmd, 1);
            VkRenderingAttachmentInfo attachment{};
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            attachment.imageView = pass->objectView();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {kWidth, kHeight}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            vkCmdBeginRendering(cmd, &rendering);
            for (const Rect& rect : rects) {
                VkClearAttachment clear{};
                clear.aspectMask = kColor;
                clear.colorAttachment = 0;
                clear.clearValue.color.uint32[0] = rect.vector;
                clear.clearValue.color.uint32[1] = rect.depth;
                const VkClearRect area = {
                        {{static_cast<std::int32_t>(rect.x), static_cast<std::int32_t>(rect.y)}, {rect.w, rect.h}},
                        0,
                        1};
                vkCmdClearAttachments(cmd, 1, &clear, 1, &area);
            }
            vkCmdEndRendering(cmd);
        }));
        CHECK(pass->hasObjectWrites(1) && !pass->hasObjectWrites(2));

        // Runs the combine pass and returns the final vectors (x, y per pixel).
        const auto run = [&](const tox2::Mat4& matrix) {
            std::vector<float> vectors(kPixels * 2, 1000.0f);
            tox2::FgMotionInputs inputs;
            inputs.depth = depth.view;
            inputs.clip_to_prev_clip = matrix;
            inputs.object = true;
            std::string run_error;
            const bool done = fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
                CHECK(pass->recordMotion(cmd, inputs, run_error));
                const tox2::SlImage motion = pass->motionImage();
                slTestBarrier(cmd, motion.image, kColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {kColor, 0, 0, 1};
                copy.imageExtent = {kWidth, kHeight, 1};
                vkCmdCopyImageToBuffer(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                       &copy);
                fgHostBarrier(cmd);
            });
            CHECK(done);
            if (!run_error.empty()) std::printf("fg motion object combine: %s\n", run_error.c_str());
            if (done) {
                vmaInvalidateAllocation(ctx.allocator(), readback.allocation, 0, VK_WHOLE_SIZE);
                const auto* halves = static_cast<const std::uint16_t*>(readback.mapped);
                for (std::size_t i = 0; i < kPixels * 2; i++) vectors[i] = halfToFloat(halves[i]);
            }
            return vectors;
        };

        // The identity matrix has no camera motion: the object vector inside the valid rectangle, zero elsewhere.
        const std::vector<float> still = run(tox2::mat4Identity());
        CHECK(pass->objectLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        std::size_t object_pixels = 0, stale_pixels = 0, wrong = 0;
        for (std::uint32_t y = 0; y < kHeight; y++) {
            for (std::uint32_t x = 0; x < kWidth; x++) {
                const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 2;
                const bool valid = inRect(rects[0], x, y);
                const float want_x = valid ? 3.0f : 0.0f, want_y = valid ? -2.0f : 0.0f;
                if (std::abs(still[at] - want_x) > 0.01f || std::abs(still[at + 1] - want_y) > 0.01f) wrong++;
                object_pixels += valid ? 1 : 0;
                stale_pixels += inRect(rects[1], x, y) ? 1 : 0;
            }
        }
        std::printf("fg motion object combine: %zu object pixels, %zu stale pixels, %zu wrong\n", object_pixels,
                    stale_pixels, wrong);
        CHECK(object_pixels == 16 * 8 && stale_pixels == 16 * 8);
        CHECK(wrong == 0);

        // A moving camera: outside the valid rectangle the vector is the camera's, on the depth plane of the test.
        const tox2::Mat4 current = fgTestWorldToClip(0.4f, 0.15f, 0.0f, 2.0f);
        const tox2::Mat4 previous = fgTestWorldToClip(0.0f, 0.0f, 0.0f, 0.0f);
        tox2::Mat4 current_inverse{};
        CHECK(tox2::mat4Inverse(current, current_inverse));
        const tox2::Mat4 clip_to_prev_clip = tox2::mat4Multiply(current_inverse, previous);
        const std::vector<float> moved = run(clip_to_prev_clip);
        float worst = 0.0f, largest = 0.0f;
        for (std::uint32_t i = 0; i < 40; i++) {
            const std::uint32_t x = (i * 7 + 3) % kWidth;
            const std::uint32_t y = (i * 5 + 2) % kHeight;
            const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 2;
            if (inRect(rects[0], x, y)) {
                worst = std::max({worst, std::abs(moved[at] - 3.0f), std::abs(moved[at + 1] + 2.0f)});
                continue;
            }
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const std::array<float, 4> clip = {px / kWidth * 2.0f - 1.0f, py / kHeight * 2.0f - 1.0f, depth_value,
                                               1.0f};
            const std::array<float, 4> q = tox2::mat4Transform(clip, clip_to_prev_clip);
            CHECK(q[3] > 0.0f);
            const float mv_x = (q[0] / q[3] + 1.0f) * 0.5f * kWidth - px;
            const float mv_y = (q[1] / q[3] + 1.0f) * 0.5f * kHeight - py;
            worst = std::max({worst, std::abs(moved[at] - mv_x), std::abs(moved[at + 1] - mv_y)});
            largest = std::max({largest, std::abs(mv_x), std::abs(mv_y)});
        }
        std::printf("fg motion object combine: camera largest %.2f px, worst error %.4f px\n", largest, worst);
        CHECK(largest > 1.0f);
        CHECK(worst < 0.01f);
    }

    pass.reset();
    vkDeviceWaitIdle(ctx.device());
    destroyFgHostBuffer(ctx, readback);
    destroyFgHostBuffer(ctx, upload);
    destroySlTestImage(ctx, depth);
    CHECK(ctx.validationErrorCount() == errors_before);
}

// The history buffer: frame 1's vertex arrays and constant blocks, recorded from an upload-like buffer, come back byte
// for byte from the ranges frame 2's matches return; draws sharing a constants range share its copy; a frame that does
// not follow on, or a scene of another size, matches nothing; a larger frame replaces the buffer of its parity.
void testObjectMotionHistory(tox2::VkContext& ctx) {
    using tox2::ObjectMotionHistory;
    constexpr std::uint32_t kWidth = 64, kHeight = 36;
    constexpr VkDeviceSize kConstantBytes = tox2::kRsxVpConstantCount * 16;
    constexpr VkDeviceSize kBigBytes = 1536 * 1024;
    constexpr VkDeviceSize kUploadBytes = 256 * 1024;
    const std::uint32_t errors_before = ctx.validationErrorCount();
    const VkDeviceSize uniform_alignment = ctx.deviceProperties().limits.minUniformBufferOffsetAlignment;

    FgHostBuffer upload, big, readback;
    CHECK(createFgHostBuffer(ctx, kUploadBytes, upload));
    CHECK(createFgHostBuffer(ctx, 2 * kBigBytes, big));
    CHECK(createFgHostBuffer(ctx, 2 * kBigBytes, readback));
    if (upload.mapped == nullptr || big.mapped == nullptr || readback.mapped == nullptr) {
        destroyFgHostBuffer(ctx, upload);
        destroyFgHostBuffer(ctx, big);
        destroyFgHostBuffer(ctx, readback);
        return;
    }

    // Two constant blocks and the vertex arrays of three candidates, 256-aligned like the renderer's uploads. All but
    // the last array lie close enough to be copied as one region; that one is far away and gets a region of its own.
    constexpr VkDeviceSize kConstantsA = 0, kConstantsB = 8192;
    const ObjectMotionHistory::Range arrays[3][2] = {
            {{upload.buffer, 16384, 48}, {upload.buffer, 16384 + 256, 64}},
            {{upload.buffer, 17408, 96}, {}},
            {{upload.buffer, 18432, 32}, {upload.buffer, 18432 + 128 * 1024, 20}},
    };
    const std::uint32_t slots[3][2] = {{0, 2}, {1, 0}, {0, 5}};
    const auto pattern = [](std::uint32_t seed, VkDeviceSize index) {
        return static_cast<std::uint8_t>(seed + index * 7 + (index >> 8));
    };
    {
        auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
        std::memset(bytes, 0xEE, kUploadBytes);
        for (VkDeviceSize i = 0; i < kConstantBytes; i++) {
            bytes[kConstantsA + i] = pattern(1, i);
            bytes[kConstantsB + i] = pattern(2, i);
        }
        for (std::uint32_t c = 0; c < 3; c++) {
            for (const ObjectMotionHistory::Range& range : arrays[c]) {
                for (VkDeviceSize i = 0; i < range.size; i++) bytes[range.offset + i] = pattern(10 + c, i);
            }
        }
        auto* big_bytes = static_cast<std::uint8_t*>(big.mapped);
        for (VkDeviceSize i = 0; i < kBigBytes; i++) big_bytes[i] = pattern(99, i);
        vmaFlushAllocation(ctx.allocator(), upload.allocation, 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(ctx.allocator(), big.allocation, 0, VK_WHOLE_SIZE);
    }

    const tox2::MotionDrawSignature signatures[3] = {{101, 1}, {102, 2}, {103, 3}};
    const auto candidateUniforms = [](std::uint32_t c) {
        tox2::VpDrawUniforms draw{};
        draw.clip_width = static_cast<float>(c + 1);
        draw.branch_bits = 0x100 + c;
        return draw;
    };
    const ObjectMotionHistory::Range constants_a = {upload.buffer, kConstantsA, kConstantBytes};
    const ObjectMotionHistory::Range constants_b = {upload.buffer, kConstantsB, kConstantBytes};
    const ObjectMotionHistory::Range constants_of[3] = {constants_a, constants_a, constants_b};
    const auto uploadsOf = [&](std::uint32_t c) {
        std::array<ObjectMotionHistory::Range, tox2::kPipelineVertexSlots> uploads{};
        for (std::uint32_t i = 0; i < 2; i++) {
            if (arrays[c][i].size != 0) uploads[slots[c][i]] = arrays[c][i];
        }
        return uploads;
    };

    ObjectMotionHistory history(ctx);
    std::shared_ptr<void> retired;
    std::string error;
    // One frame of the three candidates: the matches (copied out) and the number of them.
    const auto runFrame = [&](std::uint64_t frame, std::uint32_t width) {
        history.beginFrame(frame, width, kHeight, tox2::MotionMatchRule::AddressThenOrdered);
        std::vector<ObjectMotionHistory::Previous> matches;
        std::uint32_t matched = 0;
        for (std::uint32_t c = 0; c < 3; c++) {
            const ObjectMotionHistory::Previous* previous = history.match(signatures[c]);
            matched += previous != nullptr;
            matches.push_back(previous != nullptr ? *previous : ObjectMotionHistory::Previous{});
            history.record(signatures[c], constants_of[c], candidateUniforms(c), uploadsOf(c));
        }
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) { CHECK(history.recordCopy(cmd, retired, error)); }));
        return std::pair{matched, matches};
    };
    const auto read = [&](const ObjectMotionHistory::Range& range, std::vector<std::uint8_t>& out) {
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) {
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
            const VkBufferCopy region = {range.offset, 0, range.size};
            vkCmdCopyBuffer(cmd, range.buffer, readback.buffer, 1, &region);
            fgHostBarrier(cmd);
        }));
        vmaInvalidateAllocation(ctx.allocator(), readback.allocation, 0, VK_WHOLE_SIZE);
        const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
        out.assign(bytes, bytes + range.size);
    };
    const auto sourceBytes = [&](const ObjectMotionHistory::Range& range) {
        const auto* bytes = static_cast<const std::uint8_t*>(range.buffer == big.buffer ? big.mapped : upload.mapped);
        return std::vector<std::uint8_t>(bytes + range.offset, bytes + range.offset + range.size);
    };

    // Frame 1 has nothing to match; the set the first two candidates share is copied once.
    {
        const auto [matched, matches] = runFrame(1, kWidth);
        CHECK(matched == 0);
        CHECK(retired == nullptr);
        const ObjectMotionHistory::Stats stats = history.stats();
        CHECK(stats.candidates == 3 && stats.matched == 0);
        CHECK(stats.history_bytes == 48 + 64 + 96 + 32 + 20 + 2 * kConstantBytes);
        CHECK(stats.regions == 2);
    }

    // Frame 2 matches all three, in the history buffer.
    std::vector<ObjectMotionHistory::Previous> frame1;
    {
        auto [matched, matches] = runFrame(2, kWidth);
        CHECK(matched == 3);
        CHECK(retired == nullptr);
        CHECK(history.stats().candidates == 3 && history.stats().matched == 3);
        frame1 = std::move(matches);
    }
    if (frame1.size() == 3 && frame1[0].constants.buffer != VK_NULL_HANDLE) {
        CHECK(frame1[0].constants.buffer == frame1[1].constants.buffer);
        CHECK(frame1[0].constants.offset == frame1[1].constants.offset);
        CHECK(frame1[2].constants.offset != frame1[0].constants.offset);
        for (const ObjectMotionHistory::Previous& previous : frame1) {
            CHECK(previous.constants.size == kConstantBytes);
            CHECK(previous.constants.offset % uniform_alignment == 0);
            CHECK(previous.constants.buffer != upload.buffer);
        }
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t c = 0; c < 3; c++) {
            const ObjectMotionHistory::Previous& previous = frame1[c];
            CHECK(previous.draw.branch_bits == 0x100 + c && previous.draw.clip_width == static_cast<float>(c + 1));
            read(previous.constants, bytes);
            CHECK(bytes == sourceBytes(constants_of[c]));
            std::uint32_t used = 0;
            for (std::uint32_t slot = 0; slot < tox2::kPipelineVertexSlots; slot++) {
                const ObjectMotionHistory::Range& range = previous.slots[slot];
                if (range.size == 0) continue;
                used++;
                CHECK(range.buffer == previous.constants.buffer && range.offset % 16 == 0);
                const ObjectMotionHistory::Range& expected = arrays[c][slots[c][0] == slot ? 0 : 1];
                CHECK(range.size == expected.size);
                read(range, bytes);
                CHECK(bytes == sourceBytes(expected));
            }
            CHECK(used == (arrays[c][1].size != 0 ? 2u : 1u));
        }
    } else {
        CHECK(false);
    }

    // Frame 4 does not follow frame 2; frame 5 follows frame 4; frame 6 changes the scene size.
    CHECK(runFrame(4, kWidth).first == 0);
    CHECK(runFrame(5, kWidth).first == 3);
    CHECK(runFrame(6, kWidth + 1).first == 0);
    CHECK(retired == nullptr);

    // Frame 7 shares frame 5's buffer and is far larger: the buffer is replaced and the old one handed back.
    {
        history.beginFrame(7, kWidth + 1, kHeight, tox2::MotionMatchRule::AddressThenOrdered);
        CHECK(history.match(signatures[0]) != nullptr);
        std::array<ObjectMotionHistory::Range, tox2::kPipelineVertexSlots> uploads{};
        uploads[0] = {big.buffer, 0, kBigBytes};
        history.record(signatures[0], constants_a, candidateUniforms(0), uploads);
        CHECK(fgTestSubmit(ctx, [&](VkCommandBuffer cmd) { CHECK(history.recordCopy(cmd, retired, error)); }));
        CHECK(retired != nullptr);
        CHECK(history.stats().history_bytes == kBigBytes + kConstantBytes);
        CHECK(history.stats().regions == 2);
        retired.reset();

        history.beginFrame(8, kWidth + 1, kHeight, tox2::MotionMatchRule::AddressThenOrdered);
        const ObjectMotionHistory::Previous* previous = history.match(signatures[0]);
        CHECK(previous != nullptr);
        if (previous != nullptr) {
            std::vector<std::uint8_t> bytes;
            read(previous->slots[0], bytes);
            CHECK(bytes == sourceBytes(uploads[0]));
            read(previous->constants, bytes);
            CHECK(bytes == sourceBytes(constants_a));
        }
        // Frame 8 recorded nothing and copied nothing, so frame 9 finds nothing to match.
        history.beginFrame(9, kWidth + 1, kHeight, tox2::MotionMatchRule::AddressThenOrdered);
        CHECK(history.match(signatures[0]) == nullptr);
        history.clear();
    }

    vkDeviceWaitIdle(ctx.device());
    destroyFgHostBuffer(ctx, upload);
    destroyFgHostBuffer(ctx, big);
    destroyFgHostBuffer(ctx, readback);
    CHECK(error.empty());
    CHECK(ctx.validationErrorCount() == errors_before);
}

}  // namespace

int main() {
    const bool layer = tox2::vkValidationLayerAvailable();
    std::printf("validation layer: %s\n", layer ? "installed" : "not installed");
    testChooseVkDevice();
    testLetterbox();
    testChoosePresentMode();
#if defined(_WIN32)
    testKeyMessages();
#endif
    testPresenterTypes();
    testShaderCompiler();
    testTextureMapping();
    testFrameDump();
    testPartialStencilNote();
    testStencilDecode();
    testUserClipDecode();
    testAntiAliasDecode();
    testScaleRect();
    testZetaWriteRule();
    testVertexConvert();
    testVertexConvertMatchesReference();
    testConvertIndicesChecked();
    testAaModeNames();
    testDlaaJitter();
    testDlaaPerspective();
    testDlaaMatrices();
    testDlaaCameraCut();
    testDlaaCameraTracker();
    testWindowSizePresets();
    testPlaceInWorkArea();
    testAnisotropyNames();
    testForcedAnisotropy();
    testLayoutSource();
    testIdlePresentDue();

    tox2::VkContextOptions options;
    options.validation = true;
    options.streamline = true;
    std::string error;
    std::unique_ptr<tox2::VkContext> ctx = tox2::createVkContext(options, error);
    if (!ctx) {
        std::printf("vulkan: %s\n", error.c_str());
        std::printf("render tests: skipped: no Vulkan device\n");
        return g_failed != 0 ? 1 : 0;
    }

    const std::uint32_t api = ctx->deviceProperties().apiVersion;
    std::printf(
            "device: %s (Vulkan %u.%u.%u), driver %s %s, triangle fans %s, D24S8 %s, clip distance %s, large points %s, "
            "validation %s\n",
            ctx->deviceName().data(), VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
            std::string(ctx->driverName()).c_str(), std::string(ctx->driverInfo()).c_str(),
            ctx->triangleFans() ? "yes" : "no", ctx->hasD24S8() ? "supported" : "unsupported (D32S8 fallback)",
            ctx->hasClipDistance() ? "supported" : "unsupported", ctx->hasLargePoints() ? "supported" : "unsupported",
            ctx->validationEnabled() ? "active" : "inactive");
    CHECK(layer == ctx->validationEnabled());

    testStreamlineContext(*ctx);
    testStreamlineEvaluate(*ctx);
    testStreamlineFrameGenApi(*ctx);
    testClearAndReadback(*ctx);
    testTwoSampleSupport(*ctx);
    testRendererAaMode(*ctx);
    testLayoutPassPipelines(*ctx);
    testObjectMotionPipeline(*ctx);
    testRendererClear(*ctx);
    testRendererReadbackGate(*ctx);
    testRendererRequestedDump(*ctx);
    testRendererRequestedDumpSharesDumpDir(*ctx);
    testPresentOverlayNeedsPresenter(*ctx);
    testPresentIdleNeedsPresenter(*ctx);
    testRendererScissorAndMask(*ctx);
    testRendererQuad(*ctx);
    testRendererAlphaTest(*ctx);
    testIndexOutsideRange(*ctx);
    testRendererBranchBits(*ctx);
    testVertexConstantUploadReuse(*ctx);
    testRendererTextures(*ctx);
    testReadBarrierSkipped(*ctx);
    testDrawPassMerging(*ctx);
    testClearPassMerging(*ctx);
    testRendererVolumeTextures(*ctx);
    testRendererFloatSurface(*ctx);
    testRendererDepthTexture(*ctx);
    testImagePool(*ctx);
    testRendererImageCreationCounters(*ctx);
    testRendererDepthArgbPool(*ctx);
    testRendererStencil(*ctx);
    testRendererUserClip(*ctx);
    testRendererConditionCode(*ctx);
    testRendererSurfaceResize(*ctx);
    testRendererScaledPlain(*ctx);
    testRendererScaledTargetsAsTextures(*ctx);
    testRendererScaleChange(*ctx);
    testRendererAntiAliasedSurface(*ctx);
    testRendererMsaaLayout(*ctx);
    testDrawStateShadow(*ctx);
    testRendererAaModeSwitch(*ctx);
    testRendererMsaaDepthAsArgb(*ctx);
    testRendererMsaaInherit(*ctx);
    testRendererMsaaLayoutStencilBits(*ctx);
    testRendererDlaaSceneImage(*ctx);
    testRendererDlaaEvaluate(*ctx);
    testRendererFrameGenOptions(*ctx);
    testRendererPointSize(*ctx);
    testForcedAnisotropySampler(*ctx);
    testRendererAnisotropy(*ctx);
    testFgMotionVectors(*ctx);
    testFgWarpCheck(*ctx);
    testFgMotionObjectImage(*ctx);
    testFgMotionObjectCombine(*ctx);
    testObjectMotionHistory(*ctx);
    CHECK(ctx->validationErrorCount() == 0);

    ctx.reset();
    std::printf("render tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

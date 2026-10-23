// Opens a real window and presents a few frames to it. The Cocoa window belongs to the process main thread,
// so this runs in main() and not in render_test. Exit code 77 means "skipped" (no window server session).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "render/vk_context.h"
#include "render/vk_window.h"

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

constexpr int kSkipped = 77;  // ctest SKIP_RETURN_CODE
constexpr int kFrames = 3;

// Records a layout transition of the acquired image to PRESENT_SRC and submits it between the acquire and
// render semaphores. The window shows whatever the image holds; this test checks the path, not the pixels.
bool presentFrame(tox2::VkContext& ctx, tox2::Presenter& presenter, VkCommandPool pool, VkCommandBuffer cmd,
                  std::size_t slot) {
    tox2::Presenter::Target target;
    if (!presenter.acquire(slot, target)) return false;

    vkResetCommandPool(ctx.device(), pool, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) return false;

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;

    VkCommandBufferSubmitInfo cmd_submit{};
    cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_submit.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = target.acquire_semaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = target.render_semaphore;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_submit;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    if (vkQueueSubmit2(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;

    presenter.present(target);
    // The command pool is reset at the next frame, so the queue must be idle first.
    return vkQueueWaitIdle(ctx.graphicsQueue()) == VK_SUCCESS;
}

}  // namespace

int main() {
    tox2::VkContextOptions options;
    options.validation = true;
    options.window = true;
    std::string error;
    std::unique_ptr<tox2::VkContext> ctx = tox2::createVkContext(options, error);
    if (!ctx) {
        std::fprintf(stderr, "vulkan: %s\n", error.c_str());
        std::printf("render window tests: 0 passed, 1 failed\n");
        return 1;
    }

    tox2::PresenterOptions presenter_options;
    presenter_options.window_size = tox2::WindowSizePreset::Size720p;
    std::unique_ptr<tox2::Presenter> presenter = tox2::Presenter::create(*ctx, presenter_options, error);
    if (!presenter) {
        // Without a window server session (ssh, a launchd daemon) no window can be shown at all.
        if (error.find("no GUI session") != std::string::npos) {
            std::printf("render window tests: skipped: %s\n", error.c_str());
            return kSkipped;
        }
        std::fprintf(stderr, "window: %s\n", error.c_str());
        std::printf("render window tests: 0 passed, 1 failed\n");
        return 1;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx->graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(ctx->device(), &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (pool != VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(ctx->device(), &alloc, &cmd) == VK_SUCCESS);
    }

    if (cmd != VK_NULL_HANDLE) {
        for (int frame = 0; frame < kFrames; frame++) {
            CHECK(presentFrame(*ctx, *presenter, pool, cmd, static_cast<std::size_t>(frame) % presenter->frameSlots()));
            tox2::pumpHostEvents();
        }
    }
    CHECK(presenter->nativeWindowHandle() != nullptr);
    const tox2::WindowStatus status = presenter->windowStatus();
    CHECK(status.preset == tox2::WindowSizePreset::Size720p && !status.fullscreen);
    CHECK(status.client.width > 0 && status.client.height > 0);

    vkDeviceWaitIdle(ctx->device());
    if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(ctx->device(), pool, nullptr);
    presenter.reset();
    CHECK(ctx->validationErrorCount() == 0);
    ctx.reset();

    std::printf("render window tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

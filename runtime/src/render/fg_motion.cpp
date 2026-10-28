#include "render/fg_motion.h"

#include <array>
#include <iterator>
#include <utility>
#include <vector>

#include "render/fp_translator.h"
#include "render/shader_compiler.h"
#include "render/vk_pipelines.h"

namespace tox2 {
namespace {

constexpr std::uint64_t kVertexKey = 0x46474D4F5456ull;
constexpr std::uint64_t kMotionFragmentKey = 0x46474D4F5446ull;
constexpr std::uint64_t kWarpFragmentKey = 0x46474D575246ull;

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kWarpFormat = VK_FORMAT_R32G32_SFLOAT;
constexpr VkFormat kColorCopyFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Fragment push constants. The layout holds the larger block; the warp pass pushes only its 12 bytes.
struct MotionConstants {
    Mat4 clip_to_prev_clip;
    float size[2];
    float flip[2];
};
static_assert(sizeof(MotionConstants) == 80);

struct WarpConstants {
    float size[2];
    std::int32_t has_previous;
};
static_assert(sizeof(WarpConstants) == 12);

// One triangle covering the target, the same as the clear pass of the draw pipelines.
constexpr const char* kVertexGlsl = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Mat4 is row-major and acts on row vectors (dlaa_math.h). The push constant arrives in GLSL's column-major order, so
// GLSL column j holds row j of the Mat4 and `m * c` is the row-vector product `c * Mat4`.
constexpr const char* kMotionFragmentGlsl = R"(#version 450
layout(push_constant) uniform Motion { mat4 clip_to_prev_clip; vec2 size; vec2 flip; } pc;
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(location = 0) out vec2 out_mv;
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec2 p = gl_FragCoord.xy;
  vec2 ndc = p / pc.size * 2.0 - 1.0;
  float depth = texelFetch(depth_tex, ip, 0).r;
  vec4 q = pc.clip_to_prev_clip * vec4(ndc, depth, 1.0);
  vec2 mv = vec2(0.0);
  if (q.w > 0.0) mv = (q.xy / q.w + 1.0) * 0.5 * pc.size - p;
  out_mv = mv * pc.flip;
}
)";

// Writes the two differences, or (-1, -1) where there is nothing to compare: no previous colour, or p + mv outside the
// image. The current colour is also copied out, to be the previous colour of the next call (a second target, since the
// previous colour is sampled in this pass).
constexpr const char* kWarpFragmentGlsl = R"(#version 450
layout(push_constant) uniform Warp { vec2 size; int has_previous; } pc;
layout(set = 0, binding = 0) uniform sampler2D cur_tex;
layout(set = 0, binding = 1) uniform sampler2D prev_tex;
layout(set = 0, binding = 2) uniform sampler2D motion_tex;
layout(location = 0) out vec2 out_diff;
layout(location = 1) out vec4 out_copy;
float meanOf(vec3 v) { return (v.x + v.y + v.z) * (1.0 / 3.0); }
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 cur = texelFetch(cur_tex, ip, 0);
  out_copy = cur;
  out_diff = vec2(-1.0);
  if (pc.has_previous == 0) return;
  vec2 q = gl_FragCoord.xy + texelFetch(motion_tex, ip, 0).rg;
  if (q.x < 0.0 || q.y < 0.0 || q.x >= pc.size.x || q.y >= pc.size.y) return;
  vec3 warped = textureLod(prev_tex, q / pc.size, 0.0).rgb;
  vec3 still = texelFetch(prev_tex, ip, 0).rgb;
  out_diff = vec2(meanOf(abs(cur.rgb - warped)), meanOf(abs(cur.rgb - still))) * 255.0;
}
)";

struct Image {
    VkContext* context = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;

    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image() { reset(); }

    // Also cleans up a half-built image, so a failed createImage() can be retried.
    void reset() {
        if (context == nullptr) return;
        if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
        if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        allocation = nullptr;
        memory = VK_NULL_HANDLE;
    }
};

// `dedicated` gives the image its own allocation, which is needed when the device memory is handed to Streamline.
bool createImage(VkContext& context, VkFormat format, VkImageUsageFlags usage, std::uint32_t width,
                 std::uint32_t height, bool dedicated, Image& out, std::string& error) {
    out.context = &context;
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
    if (dedicated) alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo allocated{};
    if (vmaCreateImage(context.allocator(), &image_info, &alloc_info, &out.image, &out.allocation, &allocated) !=
        VK_SUCCESS) {
        out.image = VK_NULL_HANDLE;
        error = "vmaCreateImage failed for a motion vector image";
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(context.device(), &view_info, nullptr, &out.view) != VK_SUCCESS) {
        out.view = VK_NULL_HANDLE;
        error = "vkCreateImageView failed for a motion vector image";
        return false;
    }
    out.memory = allocated.deviceMemory;
    out.width = width;
    out.height = height;
    out.format = format;
    out.usage = usage;
    return true;
}

// The contents of the image are never needed across a barrier here (every pass overwrites all of it, or the image is
// seeded first), so `from` is UNDEFINED unless a layout is being kept.
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

VkShaderModule createModule(VkDevice device, const std::vector<std::uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

VkSampler createSampler(VkDevice device, VkFilter filter) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sampler;
}

VkDescriptorImageInfo imageInfo(VkSampler sampler, VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = layout;
    return info;
}

VkWriteDescriptorSet sampledWrite(std::uint32_t binding, const VkDescriptorImageInfo& info) {
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    return write;
}

void setFullViewport(VkCommandBuffer cmd, std::uint32_t width, std::uint32_t height) {
    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkRenderingAttachmentInfo overwriteAttachment(VkImageView view) {
    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    return attachment;
}

}  // namespace

struct FgMotionPass::Resources {
    VkContext* context = nullptr;
    std::uint32_t width = 0, height = 0;
    Image motion;
    // The scene pass writes object motion here (kObjectMotionFormat); `camera` is the camera-only vectors the combine
    // pass will keep beside the final ones.
    Image object, camera;
    VkImageLayout object_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool object_cleared = false;
    std::uint64_t object_frame = 0;
    // The warp check, created on first use. `previous[read]` holds the last call's colour once `has_history`.
    std::array<Image, 2> previous;
    std::size_t read = 0;
    bool has_history = false;
    Image warp;
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_allocation = nullptr;
    const float* mapped = nullptr;
    bool warp_ready = false;
    bool stats_pending = false;

    Resources() = default;
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    ~Resources() {
        if (readback != VK_NULL_HANDLE) vmaDestroyBuffer(context->allocator(), readback, readback_allocation);
    }
};

FgMotionPass::FgMotionPass(VkContext& context) : context_(context) {}

FgMotionPass::~FgMotionPass() {
    resources_.reset();
    VkDevice device = context_.device();
    if (motion_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, motion_pipeline_, nullptr);
    if (warp_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, warp_pipeline_, nullptr);
    if (vertex_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertex_, nullptr);
    if (motion_fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, motion_fragment_, nullptr);
    if (warp_fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, warp_fragment_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    if (nearest_ != VK_NULL_HANDLE) vkDestroySampler(device, nearest_, nullptr);
    if (linear_ != VK_NULL_HANDLE) vkDestroySampler(device, linear_, nullptr);
}

bool FgMotionPass::createObjects(std::string& error) {
    // Each object is created only when missing, so a retry after a partial failure keeps what exists.
    if (nearest_ != VK_NULL_HANDLE && linear_ != VK_NULL_HANDLE && set_layout_ != VK_NULL_HANDLE &&
        pipeline_layout_ != VK_NULL_HANDLE && vertex_ != VK_NULL_HANDLE) {
        return true;
    }
    VkDevice device = context_.device();
    if (nearest_ == VK_NULL_HANDLE) nearest_ = createSampler(device, VK_FILTER_NEAREST);
    if (linear_ == VK_NULL_HANDLE) linear_ = createSampler(device, VK_FILTER_LINEAR);
    if (nearest_ == VK_NULL_HANDLE || linear_ == VK_NULL_HANDLE) {
        error = "vkCreateSampler failed";
        return false;
    }

    if (set_layout_ == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (std::uint32_t i = 0; i < 3; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        set_info.bindingCount = 3;
        set_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
            set_layout_ = VK_NULL_HANDLE;
            error = "vkCreateDescriptorSetLayout failed";
            return false;
        }
    }
    if (pipeline_layout_ == VK_NULL_HANDLE) {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size = sizeof(MotionConstants);
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            pipeline_layout_ = VK_NULL_HANDLE;
            error = "vkCreatePipelineLayout failed";
            return false;
        }
    }

    if (vertex_ == VK_NULL_HANDLE) {
        auto vertex = compileGlsl(ShaderStage::Vertex, kVertexGlsl, kVertexKey);
        if (!vertex) {
            error = "the motion vector vertex shader did not compile: " + vertex.error();
            return false;
        }
        vertex_ = createModule(device, *vertex);
        if (vertex_ == VK_NULL_HANDLE) {
            error = "vkCreateShaderModule failed";
            return false;
        }
    }
    return true;
}

bool FgMotionPass::createPipeline(bool warp, std::string& error) {
    VkPipeline& pipeline = warp ? warp_pipeline_ : motion_pipeline_;
    if (pipeline != VK_NULL_HANDLE) return true;
    if (!createObjects(error)) return false;

    VkShaderModule& fragment = warp ? warp_fragment_ : motion_fragment_;
    if (fragment == VK_NULL_HANDLE) {
        auto compiled = compileGlsl(ShaderStage::Fragment, warp ? kWarpFragmentGlsl : kMotionFragmentGlsl,
                                    warp ? kWarpFragmentKey : kMotionFragmentKey);
        if (!compiled) {
            error = "a motion vector shader did not compile: " + compiled.error();
            return false;
        }
        fragment = createModule(context_.device(), *compiled);
        if (fragment == VK_NULL_HANDLE) {
            error = "vkCreateShaderModule failed";
            return false;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState attachments[2]{};
    attachments[0].colorWriteMask = attachments[1].colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = warp ? 2 : 1;
    blend.pAttachments = attachments;
    constexpr VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kDynamic));
    dynamic.pDynamicStates = kDynamic;
    const VkFormat formats[2] = {warp ? kWarpFormat : kMotionFormat, kColorCopyFormat};
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = warp ? 2 : 1;
    rendering.pColorAttachmentFormats = formats;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        pipeline = VK_NULL_HANDLE;
        error = "vkCreateGraphicsPipelines failed for a motion vector pass";
        return false;
    }
    return true;
}

bool FgMotionPass::prepare(std::uint32_t width, std::uint32_t height, std::shared_ptr<void>& retired, bool& recreated,
                           std::string& error) {
    recreated = false;
    if (resources_ && resources_->width == width && resources_->height == height) return true;
    if (width == 0 || height == 0) {
        error = "the scene image has no size";
        return false;
    }
    auto created = std::make_shared<Resources>();
    created->context = &context_;
    created->width = width;
    created->height = height;
    constexpr VkImageUsageFlags kMotionUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // Streamline is told the device memory of this image, so it owns a whole allocation.
    if (!createImage(context_, kMotionFormat, kMotionUsage, width, height, true, created->motion, error)) return false;
    constexpr VkImageUsageFlags kObjectUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!createImage(context_, kObjectMotionFormat, kObjectUsage, width, height, false, created->object, error) ||
        !createImage(context_, kMotionFormat, kMotionUsage, width, height, false, created->camera, error)) {
        return false;
    }
    retired = std::move(resources_);
    resources_ = std::move(created);
    recreated = true;
    return true;
}

SlImage FgMotionPass::motionImage() const {
    SlImage out;
    if (!resources_) return out;
    const Image& motion = resources_->motion;
    out.image = motion.image;
    out.memory = motion.memory;
    out.view = motion.view;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.width = motion.width;
    out.height = motion.height;
    out.format = motion.format;
    out.usage = motion.usage;
    return out;
}

VkImageView FgMotionPass::motionView() const { return resources_ ? resources_->motion.view : VK_NULL_HANDLE; }

VkImage FgMotionPass::objectImage() const { return resources_ ? resources_->object.image : VK_NULL_HANDLE; }

VkImageView FgMotionPass::objectView() const { return resources_ ? resources_->object.view : VK_NULL_HANDLE; }

VkImageLayout FgMotionPass::objectLayout() const {
    return resources_ ? resources_->object_layout : VK_IMAGE_LAYOUT_UNDEFINED;
}

void FgMotionPass::beginObjectWrites(VkCommandBuffer cmd, std::uint64_t frame) {
    Resources& resources = *resources_;
    const VkImage image = resources.object.image;
    if (!resources.object_cleared || resources.object_frame != frame) {
        // Earlier readers and writers of the previous frame finish first; their contents are dropped.
        imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue none{};
        none.uint32[0] = 0;
        none.uint32[1] = kObjectMotionNone;
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &none, 1, &range);
        imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        resources.object_cleared = true;
        resources.object_frame = frame;
    } else {
        // The contents are kept: only the layout (if another pass moved it) and write-after-write ordering.
        imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     resources.object_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    resources.object_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

bool FgMotionPass::recordMotion(VkCommandBuffer cmd, const FgMotionInputs& inputs, std::string& error) {
    if (!resources_) {
        error = "the motion vector pass has no images";
        return false;
    }
    if (!createPipeline(false, error)) return false;
    Resources& resources = *resources_;

    // The image is overwritten whole, so the old contents are dropped. The source stage waits for earlier readers
    // (Streamline's copy, the warp check); nothing is written to it before this pass.
    imageBarrier(cmd, resources.motion.image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment = overwriteAttachment(resources.motion.view);
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {resources.width, resources.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion_pipeline_);
    setFullViewport(cmd, resources.width, resources.height);

    const VkDescriptorImageInfo depth = imageInfo(nearest_, inputs.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkWriteDescriptorSet write = sampledWrite(0, depth);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &write);
    MotionConstants constants{};
    constants.clip_to_prev_clip = inputs.clip_to_prev_clip;
    constants.size[0] = static_cast<float>(resources.width);
    constants.size[1] = static_cast<float>(resources.height);
    constants.flip[0] = inputs.flip[0];
    constants.flip[1] = inputs.flip[1];
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // Streamline copies the image when it is tagged, and the warp check samples it.
    imageBarrier(cmd, resources.motion.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool FgMotionPass::createWarpResources(std::string& error) {
    Resources& resources = *resources_;
    if (resources.warp_ready) return true;
    constexpr VkImageUsageFlags kCopyUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr VkImageUsageFlags kWarpUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // A failure drops every image made so far, so a retry starts clean instead of overwriting live handles.
    auto dropImages = [&resources] {
        resources.previous[0].reset();
        resources.previous[1].reset();
        resources.warp.reset();
    };
    if (!createImage(context_, kColorCopyFormat, kCopyUsage, resources.width, resources.height, false,
                     resources.previous[0], error) ||
        !createImage(context_, kColorCopyFormat, kCopyUsage, resources.width, resources.height, false,
                     resources.previous[1], error) ||
        !createImage(context_, kWarpFormat, kWarpUsage, resources.width, resources.height, false, resources.warp,
                     error)) {
        dropImages();
        return false;
    }
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = static_cast<VkDeviceSize>(resources.width) * resources.height * 2 * sizeof(float);
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &resources.readback,
                        &resources.readback_allocation, &mapped) != VK_SUCCESS ||
        mapped.pMappedData == nullptr) {
        if (resources.readback != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context_.allocator(), resources.readback, resources.readback_allocation);
        }
        resources.readback = VK_NULL_HANDLE;
        resources.readback_allocation = nullptr;
        dropImages();
        error = "vmaCreateBuffer failed for the warp check read-back";
        return false;
    }
    resources.mapped = static_cast<const float*>(mapped.pMappedData);
    resources.warp_ready = true;
    return true;
}

bool FgMotionPass::recordWarpCheck(VkCommandBuffer cmd, VkImageView color, VkImageLayout color_layout,
                                   VkImageView motion, std::string& error) {
    if (!resources_) {
        error = "the motion vector pass has no images";
        return false;
    }
    // The read-back buffer is written by every check, so an uncollected result would be overwritten.
    if (resources_->stats_pending) {
        error = "the previous warp check has not been collected";
        return false;
    }
    if (!createPipeline(true, error) || !createWarpResources(error)) return false;
    Resources& resources = *resources_;
    Image& source = resources.previous[resources.read];
    Image& target = resources.previous[1 - resources.read];

    // The source is kept in SHADER_READ_ONLY_OPTIMAL between calls; the first call has nothing in it, so it only
    // needs a layout to be bound with.
    if (!resources.has_history) {
        imageBarrier(cmd, source.image, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    // The target and the result are overwritten whole. The source stages wait for the last reader of each: the pass
    // two calls ago, and the copy to the host buffer.
    imageBarrier(cmd, target.image, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    imageBarrier(cmd, resources.warp.image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachments[2] = {overwriteAttachment(resources.warp.view),
                                                overwriteAttachment(target.view)};
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {resources.width, resources.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 2;
    rendering.pColorAttachments = attachments;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, warp_pipeline_);
    setFullViewport(cmd, resources.width, resources.height);

    const VkDescriptorImageInfo infos[3] = {
            imageInfo(nearest_, color, color_layout),
            imageInfo(linear_, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            imageInfo(nearest_, motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    const VkWriteDescriptorSet writes[3] = {sampledWrite(0, infos[0]), sampledWrite(1, infos[1]),
                                            sampledWrite(2, infos[2])};
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 3, writes);
    WarpConstants constants{};
    constants.size[0] = static_cast<float>(resources.width);
    constants.size[1] = static_cast<float>(resources.height);
    constants.has_previous = resources.has_history ? 1 : 0;
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    imageBarrier(cmd, target.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    imageBarrier(cmd, resources.warp.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {resources.width, resources.height, 1};
    vkCmdCopyImageToBuffer(cmd, resources.warp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resources.readback, 1,
                           &region);
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

    resources.stats_pending = resources.has_history;
    resources.has_history = true;
    resources.read = 1 - resources.read;
    return true;
}

bool FgMotionPass::collectWarpStats(FgWarpStats& out) {
    if (!resources_ || !resources_->stats_pending) return false;
    Resources& resources = *resources_;
    resources.stats_pending = false;
    // The buffer may be host-cached without being coherent, so the device's write must be made visible first.
    vmaInvalidateAllocation(context_.allocator(), resources.readback_allocation, 0, VK_WHOLE_SIZE);
    out = {};
    const std::size_t count = static_cast<std::size_t>(resources.width) * resources.height;
    // The shader marks pixels without a comparison with a negative first value.
    for (std::size_t i = 0; i < count; i++) {
        const float warp = resources.mapped[i * 2];
        if (warp < 0.0f) continue;
        out.warp_mean += warp;
        out.no_motion_mean += resources.mapped[i * 2 + 1];
        out.pixels++;
    }
    if (out.pixels > 0) {
        out.warp_mean /= out.pixels;
        out.no_motion_mean /= out.pixels;
    }
    return true;
}

}  // namespace tox2

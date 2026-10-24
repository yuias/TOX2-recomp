#pragma once

// Shader modules and graphics pipelines of the RSX draw path: translated vertex and fragment programs
// compiled once per program, pipelines cached by everything that is baked into them. Cull mode, front
// face, depth test, depth write, depth compare, primitive topology (within a topology class), primitive
// restart, viewport, scissor, blend constants and the whole stencil state (test enable, ops, compare and write
// masks, reference, both faces) are dynamic state (Vulkan 1.3 core).

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "render/vk_context.h"
#include "rsx/rsx_frame.h"

namespace tox2 {

inline constexpr std::uint32_t kPipelineVertexSlots = 16;

// Translated and compiled programs of a draw. Valid until the DrawPipelines object is destroyed.
struct DrawShaders {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    std::uint32_t inputs_used = 0;    // bit n: the vertex program reads attribute slot n
    std::uint32_t texture_units = 0;  // bit n: the fragment program samples unit n
    // kFpTextureType* of each sampled unit, 2 bits per unit (the fragment program declares those samplers);
    // 0 for a unit the program does not sample.
    std::uint32_t texture_types = 0;
};

// Sampler types the draw's textures call for, 2 bits per unit (fpTextureType). A unit without a texture is
// 2D, like one whose texture the renderer cannot sample.
std::uint32_t drawTextureTypes(const RsxDraw& draw);

struct PipelineKey {
    std::uint64_t vp_hash = 0, fp_hash = 0;
    // The fragment GLSL depends on both words, so they are part of the program identity.
    std::uint32_t fp_control = 0, texcoord_control = 0;
    // Sampler types of the sampled units (DrawShaders::texture_types): the module declares them.
    std::uint32_t fp_texture_types = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;                               // undefined: no depth/stencil attachment
    VkPrimitiveTopology topology_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // point, line or triangle list
    // Per vertex slot: format of the bound data (undefined: slot unused) and its binding stride in bytes
    // (0: one element shared by every vertex).
    std::array<VkFormat, kPipelineVertexSlots> vertex_formats{};
    std::array<std::uint8_t, kPipelineVertexSlots> vertex_strides{};
    bool blend = false;
    VkBlendFactor src_color = VK_BLEND_FACTOR_ZERO, dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ZERO, dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD, alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags color_mask = 0xF;
    // rasterizationSamples; every attachment of the pass has this sample count.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    auto operator<=>(const PipelineKey&) const = default;
};

// Push constants of the passes that copy between a 2-sample image and the memory layout (render_scale.h
// layoutSource). std430 block of the fragment stage.
struct LayoutPassConstants {
    std::int32_t column_sample[2];  // kLayoutColumnSample
    std::int32_t stencil_bit;       // layoutDepthPipeline: -1 = depth pass, 0..7 = stencil bit pass
    std::int32_t unused;
};
static_assert(sizeof(LayoutPassConstants) == 16);

struct PipelineCacheStats {
    std::uint64_t loaded_bytes = 0;  // driver cache data accepted at start-up (0: none or rejected)
    std::uint64_t created = 0;       // pipelines created in this run
    std::uint64_t create_us = 0;     // time spent in vkCreateGraphicsPipelines
};

class DrawPipelines {
public:
    // `cache_file` holds the driver's pipeline cache data across runs; empty keeps it in memory only. Data written
    // by another device or driver is ignored.
    explicit DrawPipelines(VkContext& context, std::filesystem::path cache_file = {});
    ~DrawPipelines();
    DrawPipelines(const DrawPipelines&) = delete;
    DrawPipelines& operator=(const DrawPipelines&) = delete;

    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // One push-descriptor set: bindings 0..2 uniform buffers, 3..18 combined image samplers.
    VkPipelineLayout drawLayout() const { return draw_layout_; }

    // Translates and compiles the draw's programs on first use. False when a translator or the compiler
    // rejects one of them; the reason is counted with rsxNoteUnimplemented.
    bool shaders(const RsxDraw& draw, DrawShaders& out);

    // Null on a Vulkan failure (error() is set).
    VkPipeline pipeline(const PipelineKey& key, const DrawShaders& shaders);

    // Full-target triangle writing a push-constant colour (VkPipelineLayout of clearLayout()) with the
    // given colour write mask, no blending, no depth. `samples` is the sample count of the colour attachment.
    VkPipelineLayout clearLayout() const { return clear_layout_; }
    VkPipeline maskedClearPipeline(VkFormat color_format, VkColorComponentFlags mask,
                                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

    // Full-target passes over a pair of images, all drawn with one push-descriptor set (binding 0 the colour or
    // depth aspect, binding 1 the stencil aspect; sampler2D / usampler2D, or sampler2DMS / usampler2DMS for a
    // 2-sample source, always read with texelFetch) and LayoutPassConstants as fragment push constants.
    VkPipelineLayout layoutPassLayout() const { return layout_pass_layout_; }

    // Writes a Z24S8 surface as the A8R8G8B8 texels the RSX would read from the same memory, into a B8G8R8A8 target.
    // `multisampled` reads a 2-sample source and writes the memory layout (render_scale.h layoutSource); the
    // single-sample variant ignores the push constants.
    VkPipeline depthConvertPipeline(bool multisampled);

    // Copy a 2-sample image into a single-sample image in the memory layout (render_scale.h layoutSource).
    // The colour pass writes B8G8R8A8_UNORM or R32_SFLOAT texels. The depth pass has no colour attachment; it writes
    // gl_FragDepth, and for a format with stencil also the stencil: with the stencil export extension the same shader
    // writes it through gl_FragStencilRefARB, else the stencil bits are rebuilt one bit per draw (see
    // LayoutPassConstants).
    VkPipeline layoutColorPipeline(VkFormat color_format);
    VkPipeline layoutDepthPipeline(VkFormat depth_format);

    // Writes the pipeline cache data to the cache file (atomically, through a temporary file). Any thread: the
    // stop path calls it because the process ends without running destructors. False when nothing could be
    // written.
    bool savePipelineCache();
    // Any thread.
    PipelineCacheStats pipelineCacheStats() const;

private:
    struct Program;
    using FragmentKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>;

    const Program* vertexProgram(const RsxDraw& draw);
    const Program* fragmentProgram(const RsxDraw& draw, std::uint32_t texture_types);
    bool createClearShaders();
    enum class LayoutShader : std::uint32_t {
        DepthConvert,
        DepthConvertMs,
        LayoutColor,
        LayoutDepth,
        LayoutDepthStencil,
        LayoutDepthStencilExport
    };
    bool createLayoutPassObjects();
    VkShaderModule layoutShader(LayoutShader which);
    VkPipeline createFullTargetPipeline(VkShaderModule fragment, VkFormat color_format, VkFormat depth_format);
    void fail(std::string message);
    void loadPipelineCache();
    VkResult createPipeline(const VkGraphicsPipelineCreateInfo& info, VkPipeline* pipeline);

    VkContext& context_;
    std::string error_;
    std::filesystem::path cache_file_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::uint64_t cache_loaded_bytes_ = 0;
    std::atomic<std::uint64_t> pipelines_created_{0}, create_us_{0};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout draw_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout clear_layout_ = VK_NULL_HANDLE;
    VkShaderModule clear_vertex_ = VK_NULL_HANDLE;
    VkShaderModule clear_fragment_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_pass_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_pass_layout_ = VK_NULL_HANDLE;
    std::array<VkShaderModule, 6> layout_shaders_{};       // LayoutShader, created on first use
    std::array<VkPipeline, 2> depth_convert_pipelines_{};  // single-sample, multisampled

    std::map<std::uint64_t, std::unique_ptr<Program>> vertex_programs_;
    std::map<FragmentKey, std::unique_ptr<Program>> fragment_programs_;
    std::map<PipelineKey, VkPipeline> pipelines_;
    std::map<std::tuple<VkFormat, VkColorComponentFlags, VkSampleCountFlagBits>, VkPipeline> clear_pipelines_;
    std::map<VkFormat, VkPipeline> layout_color_pipelines_;
    std::map<VkFormat, VkPipeline> layout_depth_pipelines_;
};

}  // namespace tox2

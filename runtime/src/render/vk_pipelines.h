#pragma once

// Shader modules and graphics pipelines of the RSX draw path: translated vertex and fragment programs
// compiled once per program, pipelines cached by everything that is baked into them. Cull mode, front
// face, depth test, depth write, depth compare, primitive topology (within a topology class), primitive
// restart, viewport, scissor and blend constants are dynamic state (Vulkan 1.3 core).

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "render/vk_context.h"
#include "rsx/rsx_frame.h"

namespace dal {

inline constexpr std::uint32_t kPipelineVertexSlots = 16;

// Translated and compiled programs of a draw. Valid until the DrawPipelines object is destroyed.
struct DrawShaders {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    std::uint32_t inputs_used = 0;    // bit n: the vertex program reads attribute slot n
    std::uint32_t texture_units = 0;  // bit n: the fragment program samples unit n
};

struct PipelineKey {
    std::uint64_t vp_hash = 0, fp_hash = 0;
    // The fragment GLSL depends on both words, so they are part of the program identity.
    std::uint32_t fp_control = 0, texcoord_control = 0;
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

    auto operator<=>(const PipelineKey&) const = default;
};

class DrawPipelines {
public:
    explicit DrawPipelines(VkContext& context);
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
    // given colour write mask, no blending, no depth.
    VkPipelineLayout clearLayout() const { return clear_layout_; }
    VkPipeline maskedClearPipeline(VkFormat color_format, VkColorComponentFlags mask);

private:
    struct Program;
    using FragmentKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>;

    const Program* vertexProgram(const RsxDraw& draw);
    const Program* fragmentProgram(const RsxDraw& draw);
    bool createClearShaders();
    void fail(std::string message);

    VkContext& context_;
    std::string error_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout draw_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout clear_layout_ = VK_NULL_HANDLE;
    VkShaderModule clear_vertex_ = VK_NULL_HANDLE;
    VkShaderModule clear_fragment_ = VK_NULL_HANDLE;

    std::map<std::uint64_t, std::unique_ptr<Program>> vertex_programs_;
    std::map<FragmentKey, std::unique_ptr<Program>> fragment_programs_;
    std::map<PipelineKey, VkPipeline> pipelines_;
    std::map<std::pair<VkFormat, VkColorComponentFlags>, VkPipeline> clear_pipelines_;
};

}  // namespace dal

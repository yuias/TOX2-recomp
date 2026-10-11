#include "render/vk_pipelines.h"

#include <cstdio>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "render/fp_translator.h"
#include "render/shader_compiler.h"
#include "render/vp_translator.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

// Cache keys of the built-in clear shaders; the GLSL hash in the file name keeps them distinct from the
// translated programs.
constexpr std::uint64_t kClearVertexKey = 0x434C45415256ull;
constexpr std::uint64_t kClearFragmentKey = 0x434C45415246ull;
constexpr std::uint64_t kDepthConvertFragmentKey = 0x44455054434Full;

constexpr const char* kClearVertexGlsl = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kClearFragmentGlsl = R"(#version 450
layout(push_constant) uniform Clear { vec4 color; } clear;
layout(location = 0) out vec4 out_color;
void main() { out_color = clear.color; }
)";

// RSX reads a Z24S8 word as A8R8G8B8 texel bytes: A = depth bits 23..16, R = 15..8, G = 7..0, B = stencil
// (the depth sits in the top 24 bits of the big-endian word). This is the layout RPCS3 rebuilds in
// `RSXFragmentTextureDepthConversion.glsl` (`decode_depth24`, `convert_z24x8_to_rgba8`), with the rounding of
// `f32_to_unorm24`. The target is B8G8R8A8 and its view is the identity, so the output vector is the (R, G, B, A)
// of the texel.
constexpr const char* kDepthConvertFragmentGlsl = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform usampler2D stencil_tex;
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint z = uint(floor(texelFetch(depth_tex, p, 0).r * 16777215.0 + 0.5));
  uint s = texelFetch(stencil_tex, p, 0).r;
  out_color = vec4(float((z >> 8) & 0xFFu), float(z & 0xFFu), float(s), float(z >> 16)) / 255.0;
}
)";

std::uint64_t mixKey(std::uint64_t hash, std::uint32_t a, std::uint32_t b) {
    std::uint64_t key = hash;
    key ^= (static_cast<std::uint64_t>(a) + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    key ^= (static_cast<std::uint64_t>(b) + 0x7F4A7C159E3779B9ull) * 0x94D049BB133111EBull;
    return key;
}

// Translator messages are "<stage> opcode <name>" for opcodes and free text for everything else.
void noteTranslatorMessage(const std::string& message) {
    struct Prefix {
        std::string_view text;
        std::string_view category;
    };
    static constexpr Prefix kPrefixes[] = {
            {"vp scalar opcode ", "vp sca opcode"}, {"vp opcode ", "vp opcode"}, {"fp opcode ", "fp opcode"}};
    for (const Prefix& prefix : kPrefixes) {
        if (message.starts_with(prefix.text)) {
            rsxNoteUnimplemented(prefix.category, std::string_view(message).substr(prefix.text.size()));
            return;
        }
    }
    rsxNoteUnimplemented("state", message);
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

}  // namespace

struct DrawPipelines::Program {
    bool ok = false;
    VkShaderModule module = VK_NULL_HANDLE;
    std::uint32_t inputs_used = 0;
    std::uint32_t texture_units = 0;
    std::vector<std::string> notes;  // counted again by every draw that uses a rejected program
};

DrawPipelines::DrawPipelines(VkContext& context) : context_(context) {
    const VkDevice device = context_.device();

    VkDescriptorSetLayoutBinding bindings[3 + kFpTextureUnitCount]{};
    for (std::uint32_t i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (std::uint32_t i = 0; i < kFpTextureUnitCount; i++) {
        VkDescriptorSetLayoutBinding& b = bindings[3 + i];
        b.binding = kFpTextureBindingBase + i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set_info.bindingCount = static_cast<std::uint32_t>(std::size(bindings));
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
        set_layout_ = VK_NULL_HANDLE;
        fail("vkCreateDescriptorSetLayout failed");
        return;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &draw_layout_) != VK_SUCCESS) {
        draw_layout_ = VK_NULL_HANDLE;
        fail("vkCreatePipelineLayout failed");
        return;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = 16;
    layout_info.setLayoutCount = 0;
    layout_info.pSetLayouts = nullptr;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &clear_layout_) != VK_SUCCESS) {
        clear_layout_ = VK_NULL_HANDLE;
        fail("vkCreatePipelineLayout failed");
    }
}

DrawPipelines::~DrawPipelines() {
    const VkDevice device = context_.device();
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    for (auto& [key, pipeline] : clear_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    if (depth_convert_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device, depth_convert_pipeline_, nullptr);
    for (auto& [key, program] : vertex_programs_) {
        if (program->module != VK_NULL_HANDLE) vkDestroyShaderModule(device, program->module, nullptr);
    }
    for (auto& [key, program] : fragment_programs_) {
        if (program->module != VK_NULL_HANDLE) vkDestroyShaderModule(device, program->module, nullptr);
    }
    if (depth_convert_fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, depth_convert_fragment_, nullptr);
    if (depth_convert_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, depth_convert_layout_, nullptr);
    if (depth_convert_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, depth_convert_set_layout_, nullptr);
    }
    if (clear_vertex_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, clear_vertex_, nullptr);
    if (clear_fragment_ != VK_NULL_HANDLE) vkDestroyShaderModule(device, clear_fragment_, nullptr);
    if (clear_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, clear_layout_, nullptr);
    if (draw_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, draw_layout_, nullptr);
    if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
}

void DrawPipelines::fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
}

const DrawPipelines::Program* DrawPipelines::vertexProgram(const RsxDraw& draw) {
    auto it = vertex_programs_.find(draw.vp_hash);
    if (it == vertex_programs_.end()) {
        auto program = std::make_unique<Program>();
        if (draw.vp_ucode.empty()) {
            program->notes.push_back("draw without vertex program");
        } else {
            GlslResult glsl = translateVertexProgram(draw.vp_ucode, draw.vp_input_mask);
            program->notes = std::move(glsl.unimplemented);
            if (glsl.ok) {
                auto spirv = compileGlsl(ShaderStage::Vertex, glsl.glsl, draw.vp_hash);
                if (spirv) {
                    program->module = createModule(context_.device(), *spirv);
                    program->ok = program->module != VK_NULL_HANDLE;
                    if (!program->ok) fail("vkCreateShaderModule failed");
                    program->inputs_used = glsl.inputs_used;
                } else {
                    std::fprintf(stderr, "tox2: render: vertex program %016llx did not compile: %s\n",
                                 static_cast<unsigned long long>(draw.vp_hash), spirv.error().c_str());
                    program->notes.push_back("vertex program compile error");
                }
            }
        }
        it = vertex_programs_.emplace(draw.vp_hash, std::move(program)).first;
    }
    return it->second.get();
}

std::uint32_t drawTextureTypes(const RsxDraw& draw) {
    std::uint32_t types = 0;
    for (const RsxTextureDesc& texture : draw.textures) {
        if (texture.unit >= kFpTextureUnitCount) continue;
        const RsxTextureKind kind = rsxTextureKind(texture);
        const std::uint32_t type = kind == RsxTextureKind::Tex3D  ? kFpTextureType3D
                                   : kind == RsxTextureKind::Cube ? kFpTextureTypeCube
                                                                  : kFpTextureType2D;
        types |= type << (texture.unit * 2);
    }
    return types;
}

const DrawPipelines::Program* DrawPipelines::fragmentProgram(const RsxDraw& draw, std::uint32_t texture_types) {
    const FragmentKey key{draw.fp_hash, draw.fp_control, draw.texcoord_control, texture_types};
    auto it = fragment_programs_.find(key);
    if (it == fragment_programs_.end()) {
        auto program = std::make_unique<Program>();
        if (draw.fp_ucode.empty()) {
            program->notes.push_back("draw without fragment program");
        } else {
            GlslResult glsl =
                    translateFragmentProgram(draw.fp_ucode, draw.fp_control, draw.texcoord_control, texture_types);
            program->notes = std::move(glsl.unimplemented);
            if (glsl.ok) {
                auto spirv = compileGlsl(
                        ShaderStage::Fragment, glsl.glsl,
                        mixKey(mixKey(draw.fp_hash, draw.fp_control, draw.texcoord_control), texture_types, 0));
                if (spirv) {
                    program->module = createModule(context_.device(), *spirv);
                    program->ok = program->module != VK_NULL_HANDLE;
                    if (!program->ok) fail("vkCreateShaderModule failed");
                    program->texture_units = glsl.texture_units;
                } else {
                    std::fprintf(stderr, "tox2: render: fragment program %016llx did not compile: %s\n",
                                 static_cast<unsigned long long>(draw.fp_hash), spirv.error().c_str());
                    program->notes.push_back("fragment program compile error");
                }
            }
        }
        it = fragment_programs_.emplace(key, std::move(program)).first;
    }
    return it->second.get();
}

bool DrawPipelines::shaders(const RsxDraw& draw, DrawShaders& out) {
    const Program* vertex = vertexProgram(draw);
    // The program text depends on the types of the units it samples only. The sampled units are known after
    // the first translation, so a draw with a 3D or cube texture on some unit first gets the all-2D program,
    // then the variant restricted to the types of its sampled units (the same program when none is 3D or cube).
    std::uint32_t texture_types = drawTextureTypes(draw);
    const Program* fragment = fragmentProgram(draw, 0);
    if (texture_types != 0 && fragment->ok) {
        std::uint32_t sampled = 0;
        for (std::uint32_t unit = 0; unit < kFpTextureUnitCount; unit++) {
            if ((fragment->texture_units & (1u << unit)) != 0) sampled |= 3u << (unit * 2);
        }
        texture_types &= sampled;
        if (texture_types != 0) fragment = fragmentProgram(draw, texture_types);
    } else {
        texture_types = 0;
    }
    if (!valid()) return false;
    if (!vertex->ok || !fragment->ok) {
        for (const std::string& note : vertex->notes) noteTranslatorMessage(note);
        for (const std::string& note : fragment->notes) noteTranslatorMessage(note);
        return false;
    }
    out.vertex = vertex->module;
    out.fragment = fragment->module;
    out.inputs_used = vertex->inputs_used;
    out.texture_units = fragment->texture_units;
    out.texture_types = texture_types;
    return true;
}

VkPipeline DrawPipelines::pipeline(const PipelineKey& key, const DrawShaders& shaders) {
    const auto cached = pipelines_.find(key);
    if (cached != pipelines_.end()) return cached->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shaders.vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shaders.fragment;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[kPipelineVertexSlots]{};
    VkVertexInputAttributeDescription attributes[kPipelineVertexSlots]{};
    std::uint32_t input_count = 0;
    for (std::uint32_t slot = 0; slot < kPipelineVertexSlots; slot++) {
        if (key.vertex_formats[slot] == VK_FORMAT_UNDEFINED) continue;
        bindings[input_count] = {slot, key.vertex_strides[slot], VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[input_count] = {slot, slot, key.vertex_formats[slot], 0};
        input_count++;
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = input_count;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = input_count;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = key.topology_class;

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
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = key.blend ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = key.src_color;
    attachment.dstColorBlendFactor = key.dst_color;
    attachment.colorBlendOp = key.color_op;
    attachment.srcAlphaBlendFactor = key.src_alpha;
    attachment.dstAlphaBlendFactor = key.dst_alpha;
    attachment.alphaBlendOp = key.alpha_op;
    attachment.colorWriteMask = key.color_mask;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    constexpr VkDynamicState kDynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_CULL_MODE,          VK_DYNAMIC_STATE_FRONT_FACE,
            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kDynamic));
    dynamic.pDynamicStates = kDynamic;

    const bool has_stencil =
            key.depth_format == VK_FORMAT_D24_UNORM_S8_UINT || key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &key.color_format;
    rendering.depthAttachmentFormat = key.depth_format;
    rendering.stencilAttachmentFormat = has_stencil ? key.depth_format : VK_FORMAT_UNDEFINED;

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
    info.layout = draw_layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    pipelines_.emplace(key, pipeline);
    return pipeline;
}

bool DrawPipelines::createClearShaders() {
    if (clear_vertex_ != VK_NULL_HANDLE) return true;
    auto vertex = compileGlsl(ShaderStage::Vertex, kClearVertexGlsl, kClearVertexKey);
    auto fragment = compileGlsl(ShaderStage::Fragment, kClearFragmentGlsl, kClearFragmentKey);
    if (!vertex || !fragment) {
        fail("the clear shaders did not compile: " + (!vertex ? vertex.error() : fragment.error()));
        return false;
    }
    clear_vertex_ = createModule(context_.device(), *vertex);
    clear_fragment_ = createModule(context_.device(), *fragment);
    if (clear_vertex_ == VK_NULL_HANDLE || clear_fragment_ == VK_NULL_HANDLE) {
        fail("vkCreateShaderModule failed");
        return false;
    }
    return true;
}

VkPipeline DrawPipelines::maskedClearPipeline(VkFormat color_format, VkColorComponentFlags mask) {
    const std::pair<VkFormat, VkColorComponentFlags> key{color_format, mask};
    const auto cached = clear_pipelines_.find(key);
    if (cached != clear_pipelines_.end()) return cached->second;
    if (!createClearShaders()) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = clear_vertex_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = clear_fragment_;
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
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = mask;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    constexpr VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kDynamic));
    dynamic.pDynamicStates = kDynamic;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

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
    info.layout = clear_layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    clear_pipelines_.emplace(key, pipeline);
    return pipeline;
}

bool DrawPipelines::createDepthConvertObjects() {
    if (depth_convert_layout_ != VK_NULL_HANDLE) return true;
    if (!createClearShaders()) return false;  // the full-target vertex shader is shared
    auto fragment = compileGlsl(ShaderStage::Fragment, kDepthConvertFragmentGlsl, kDepthConvertFragmentKey);
    if (!fragment) {
        fail("the depth conversion shader did not compile: " + fragment.error());
        return false;
    }
    depth_convert_fragment_ = createModule(context_.device(), *fragment);
    if (depth_convert_fragment_ == VK_NULL_HANDLE) {
        fail("vkCreateShaderModule failed");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(context_.device(), &set_info, nullptr, &depth_convert_set_layout_) != VK_SUCCESS) {
        depth_convert_set_layout_ = VK_NULL_HANDLE;
        fail("vkCreateDescriptorSetLayout failed");
        return false;
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &depth_convert_set_layout_;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr, &depth_convert_layout_) != VK_SUCCESS) {
        depth_convert_layout_ = VK_NULL_HANDLE;
        fail("vkCreatePipelineLayout failed");
        return false;
    }
    return true;
}

VkPipeline DrawPipelines::depthConvertPipeline() {
    if (depth_convert_pipeline_ != VK_NULL_HANDLE) return depth_convert_pipeline_;
    if (!createDepthConvertObjects()) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = clear_vertex_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = depth_convert_fragment_;
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
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    constexpr VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kDynamic));
    dynamic.pDynamicStates = kDynamic;
    constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kColorFormat;

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
    info.layout = depth_convert_layout_;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &depth_convert_pipeline_) !=
        VK_SUCCESS) {
        depth_convert_pipeline_ = VK_NULL_HANDLE;
        fail("vkCreateGraphicsPipelines failed");
    }
    return depth_convert_pipeline_;
}

}  // namespace tox2

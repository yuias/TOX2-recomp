#include "render/vk_pipelines.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <string>
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
constexpr std::uint64_t kDepthConvertMsFragmentKey = 0x4445505443534Dull;
constexpr std::uint64_t kLayoutColorFragmentKey = 0x4C41594F434Full;
constexpr std::uint64_t kLayoutDepthFragmentKey = 0x4C4159444550ull;
constexpr std::uint64_t kLayoutDepthStencilFragmentKey = 0x4C4159445354ull;
constexpr std::uint64_t kLayoutDepthStencilExportFragmentKey = 0x4C4159445345ull;

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
// `f32_to_unorm24`. The clamp keeps depth 1.0 at 0xFFFFFF: 1.0 * 16777215.0 + 0.5 rounds up to 2^24 in float32.
// The target is B8G8R8A8 and its view is the identity, so the output vector is the (R, G, B, A) of the texel.
constexpr const char* kDepthConvertFragmentGlsl = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform usampler2D stencil_tex;
layout(location = 0) out vec4 out_color;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint z = min(uint(floor(texelFetch(depth_tex, p, 0).r * 16777215.0 + 0.5)), 0xFFFFFFu);
  uint s = texelFetch(stencil_tex, p, 0).r;
  out_color = vec4(float((z >> 8) & 0xFFu), float(z & 0xFFu), float(s), float(z >> 16)) / 255.0;
}
)";

// The copy passes from a 2-sample image into the memory layout share this prologue: fragment (x, y) of the layout
// image reads pixel (x >> 1, y), sample column_sample[x & 1] (render_scale.h layoutSource). No resolution scale
// enters: the 2-sample image is half as wide as the layout image at every scale.
constexpr const char* kLayoutPassVersion = "#version 450\n";
constexpr const char* kLayoutPassPrologue =
        R"(layout(push_constant) uniform Layout { int column_sample[2]; int stencil_bit; int unused; } pc;
)";

constexpr const char* kLayoutPassSource = R"(  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 src = ivec2(p.x >> 1, p.y);
  int sample_index = pc.column_sample[p.x & 1];
)";

// Same byte packing as kDepthConvertFragmentGlsl, from a 2-sample source.
constexpr const char* kDepthConvertMsBody = R"(layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
layout(set = 0, binding = 1) uniform usampler2DMS stencil_tex;
layout(location = 0) out vec4 out_color;
void main() {
)";
constexpr const char* kDepthConvertMsEnd =
        R"(  uint z = min(uint(floor(texelFetch(depth_tex, src, sample_index).r * 16777215.0 + 0.5)), 0xFFFFFFu);
  uint s = texelFetch(stencil_tex, src, sample_index).r;
  out_color = vec4(float((z >> 8) & 0xFFu), float(z & 0xFFu), float(s), float(z >> 16)) / 255.0;
}
)";

constexpr const char* kLayoutColorBody = R"(layout(set = 0, binding = 0) uniform sampler2DMS color_tex;
layout(location = 0) out vec4 out_color;
void main() {
)";
constexpr const char* kLayoutColorEnd = R"(  out_color = texelFetch(color_tex, src, sample_index);
}
)";

constexpr const char* kLayoutDepthBody = R"(layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
void main() {
)";
constexpr const char* kLayoutDepthEnd = R"(  gl_FragDepth = texelFetch(depth_tex, src, sample_index).r;
}
)";

// A shader cannot write the stencil value without an extension, so the stencil is rebuilt one bit per draw: with
// the stencil test set to REPLACE and the write mask of bit b, only the fragments whose sampled stencil has bit b
// set (the others are discarded) set that bit.
constexpr const char* kLayoutDepthStencilBody = R"(layout(set = 0, binding = 0) uniform sampler2DMS depth_tex;
layout(set = 0, binding = 1) uniform usampler2DMS stencil_tex;
void main() {
)";
constexpr const char* kLayoutDepthStencilEnd = R"(  gl_FragDepth = texelFetch(depth_tex, src, sample_index).r;
  if (pc.stencil_bit >= 0 && ((texelFetch(stencil_tex, src, sample_index).r >> uint(pc.stencil_bit)) & 1u) == 0u) {
    discard;
  }
}
)";

// `extensions` is placed right after the version line: #extension must precede every other token.
// Writes depth and stencil together where the device exports the stencil reference from the fragment shader. The
// exported value replaces the pipeline's stencil reference, so a REPLACE pass op stores it.
constexpr const char* kLayoutDepthStencilExportExtensions = "#extension GL_ARB_shader_stencil_export : require\n";
constexpr const char* kLayoutDepthStencilExportEnd = R"(  gl_FragDepth = texelFetch(depth_tex, src, sample_index).r;
  gl_FragStencilRefARB = int(texelFetch(stencil_tex, src, sample_index).r);
}
)";

std::string layoutPassGlsl(const char* body, const char* end, const char* extensions = "") {
    return std::string(kLayoutPassVersion) + extensions + kLayoutPassPrologue + body + kLayoutPassSource + end;
}

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

DrawPipelines::DrawPipelines(VkContext& context, std::filesystem::path cache_file)
    : context_(context), cache_file_(std::move(cache_file)) {
    const VkDevice device = context_.device();
    loadPipelineCache();

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
    // The vertex jitter offset (VpPush in the vertex program translator).
    VkPushConstantRange jitter_push{};
    jitter_push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    jitter_push.size = 2 * sizeof(float);
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &jitter_push;
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
    if (cache_ != VK_NULL_HANDLE) vkDestroyPipelineCache(device, cache_, nullptr);
    for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    for (auto& [key, pipeline] : clear_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    for (auto& [key, pipeline] : layout_color_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    for (auto& [key, pipeline] : layout_depth_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
    for (const VkPipeline pipeline : depth_convert_pipelines_) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
    }
    for (auto& [key, program] : vertex_programs_) {
        if (program->module != VK_NULL_HANDLE) vkDestroyShaderModule(device, program->module, nullptr);
    }
    for (auto& [key, program] : fragment_programs_) {
        if (program->module != VK_NULL_HANDLE) vkDestroyShaderModule(device, program->module, nullptr);
    }
    for (const VkShaderModule module : layout_shaders_) {
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr);
    }
    if (layout_pass_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout_pass_layout_, nullptr);
    if (layout_pass_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, layout_pass_set_layout_, nullptr);
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

void DrawPipelines::loadPipelineCache() {
    std::vector<char> data;
    if (!cache_file_.empty()) {
        std::ifstream file(cache_file_, std::ios::binary);
        if (file) data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    // Drivers are required to reject foreign data themselves, but some have crashed on it, so the header
    // (VkPipelineCacheHeaderVersionOne) is checked against this device first.
    const VkPhysicalDeviceProperties& props = context_.deviceProperties();
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() >= sizeof header) std::memcpy(&header, data.data(), sizeof header);
    const bool usable = data.size() >= sizeof header && header.headerSize >= sizeof header &&
                        header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                        header.vendorID == props.vendorID && header.deviceID == props.deviceID &&
                        std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (usable) {
        info.initialDataSize = data.size();
        info.pInitialData = data.data();
    }
    if (vkCreatePipelineCache(context_.device(), &info, nullptr, &cache_) != VK_SUCCESS) {
        // Not fatal: pipelines are then created without a cache, as before.
        cache_ = VK_NULL_HANDLE;
        return;
    }
    cache_loaded_bytes_ = info.initialDataSize;
}

bool DrawPipelines::savePipelineCache() {
    if (cache_ == VK_NULL_HANDLE || cache_file_.empty()) return false;
    std::size_t size = 0;
    if (vkGetPipelineCacheData(context_.device(), cache_, &size, nullptr) != VK_SUCCESS || size == 0) return false;
    std::vector<char> data(size);
    // VK_INCOMPLETE is possible when pipelines were added in between; the shorter data is still valid.
    const VkResult result = vkGetPipelineCacheData(context_.device(), cache_, &size, data.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return false;
    data.resize(size);
    std::error_code ec;
    std::filesystem::create_directories(cache_file_.parent_path(), ec);
    std::filesystem::path temp = cache_file_;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, cache_file_, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

PipelineCacheStats DrawPipelines::pipelineCacheStats() const {
    return {cache_loaded_bytes_, pipelines_created_.load(std::memory_order_relaxed),
            create_us_.load(std::memory_order_relaxed)};
}

VkResult DrawPipelines::createPipeline(const VkGraphicsPipelineCreateInfo& info, VkPipeline* pipeline) {
    const auto start = std::chrono::steady_clock::now();
    const VkResult result = vkCreateGraphicsPipelines(context_.device(), cache_, 1, &info, nullptr, pipeline);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    create_us_.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(),
                         std::memory_order_relaxed);
    pipelines_created_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

const DrawPipelines::Program* DrawPipelines::vertexProgram(const RsxDraw& draw) {
    auto it = vertex_programs_.find(draw.vp_hash);
    if (it == vertex_programs_.end()) {
        auto program = std::make_unique<Program>();
        if (draw.vp_ucode.empty()) {
            program->notes.push_back("draw without vertex program");
        } else {
            GlslResult glsl = translateVertexProgram(draw.vp_ucode, draw.vp_input_mask,
                                                     VpTranslateOptions{context_.hasClipDistance()});
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
    multisample.rasterizationSamples = key.samples;

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

    constexpr VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                           VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_CULL_MODE,
                                           VK_DYNAMIC_STATE_FRONT_FACE,
                                           VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                                           VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                           VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                           VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                           VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                           VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                                           VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                                           VK_DYNAMIC_STATE_STENCIL_OP,
                                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                           VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                           VK_DYNAMIC_STATE_STENCIL_REFERENCE};
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
    if (createPipeline(info, &pipeline) != VK_SUCCESS) {
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

VkPipeline DrawPipelines::maskedClearPipeline(VkFormat color_format, VkColorComponentFlags mask,
                                              VkSampleCountFlagBits samples) {
    const std::tuple<VkFormat, VkColorComponentFlags, VkSampleCountFlagBits> key{color_format, mask, samples};
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
    multisample.rasterizationSamples = samples;
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
    if (createPipeline(info, &pipeline) != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    clear_pipelines_.emplace(key, pipeline);
    return pipeline;
}

bool DrawPipelines::createLayoutPassObjects() {
    if (layout_pass_layout_ != VK_NULL_HANDLE) return true;
    if (!createClearShaders()) return false;  // the full-target vertex shader is shared

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
    if (vkCreateDescriptorSetLayout(context_.device(), &set_info, nullptr, &layout_pass_set_layout_) != VK_SUCCESS) {
        layout_pass_set_layout_ = VK_NULL_HANDLE;
        fail("vkCreateDescriptorSetLayout failed");
        return false;
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(LayoutPassConstants);
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &layout_pass_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(context_.device(), &layout_info, nullptr, &layout_pass_layout_) != VK_SUCCESS) {
        layout_pass_layout_ = VK_NULL_HANDLE;
        fail("vkCreatePipelineLayout failed");
        return false;
    }
    return true;
}

VkShaderModule DrawPipelines::layoutShader(LayoutShader which) {
    VkShaderModule& module = layout_shaders_[static_cast<std::size_t>(which)];
    if (module != VK_NULL_HANDLE) return module;

    std::string glsl;
    std::uint64_t key = 0;
    switch (which) {
        case LayoutShader::DepthConvert:
            glsl = kDepthConvertFragmentGlsl;
            key = kDepthConvertFragmentKey;
            break;
        case LayoutShader::DepthConvertMs:
            glsl = layoutPassGlsl(kDepthConvertMsBody, kDepthConvertMsEnd);
            key = kDepthConvertMsFragmentKey;
            break;
        case LayoutShader::LayoutColor:
            glsl = layoutPassGlsl(kLayoutColorBody, kLayoutColorEnd);
            key = kLayoutColorFragmentKey;
            break;
        case LayoutShader::LayoutDepth:
            glsl = layoutPassGlsl(kLayoutDepthBody, kLayoutDepthEnd);
            key = kLayoutDepthFragmentKey;
            break;
        case LayoutShader::LayoutDepthStencil:
            glsl = layoutPassGlsl(kLayoutDepthStencilBody, kLayoutDepthStencilEnd);
            key = kLayoutDepthStencilFragmentKey;
            break;
        case LayoutShader::LayoutDepthStencilExport:
            glsl = layoutPassGlsl(kLayoutDepthStencilBody, kLayoutDepthStencilExportEnd,
                                  kLayoutDepthStencilExportExtensions);
            key = kLayoutDepthStencilExportFragmentKey;
            break;
    }
    auto fragment = compileGlsl(ShaderStage::Fragment, glsl, key);
    if (!fragment) {
        fail("a layout pass shader did not compile: " + fragment.error());
        return VK_NULL_HANDLE;
    }
    module = createModule(context_.device(), *fragment);
    if (module == VK_NULL_HANDLE) fail("vkCreateShaderModule failed");
    return module;
}

// A full-target triangle (the clear vertex shader) with the given fragment shader and no blending. An undefined
// colour format means no colour attachment; a depth format makes the depth and stencil state dynamic, so one
// pipeline serves the depth pass and every stencil bit pass.
VkPipeline DrawPipelines::createFullTargetPipeline(VkShaderModule fragment, VkFormat color_format,
                                                   VkFormat depth_format) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = clear_vertex_;
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
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    const bool has_color = color_format != VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = has_color ? 1 : 0;
    blend.pAttachments = has_color ? &attachment : nullptr;

    constexpr VkDynamicState kColorDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    constexpr VkDynamicState kDepthDynamic[] = {
            VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,   VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_OP,         VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    if (depth_format != VK_FORMAT_UNDEFINED) {
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kDepthDynamic));
        dynamic.pDynamicStates = kDepthDynamic;
    } else {
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(std::size(kColorDynamic));
        dynamic.pDynamicStates = kColorDynamic;
    }

    const bool has_stencil =
            depth_format == VK_FORMAT_D24_UNORM_S8_UINT || depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = has_color ? 1 : 0;
    rendering.pColorAttachmentFormats = has_color ? &color_format : nullptr;
    rendering.depthAttachmentFormat = depth_format;
    rendering.stencilAttachmentFormat = has_stencil ? depth_format : VK_FORMAT_UNDEFINED;

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
    info.layout = layout_pass_layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (createPipeline(info, &pipeline) != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

VkPipeline DrawPipelines::depthConvertPipeline(bool multisampled) {
    VkPipeline& pipeline = depth_convert_pipelines_[multisampled ? 1 : 0];
    if (pipeline != VK_NULL_HANDLE) return pipeline;
    if (!createLayoutPassObjects()) return VK_NULL_HANDLE;
    const VkShaderModule fragment =
            layoutShader(multisampled ? LayoutShader::DepthConvertMs : LayoutShader::DepthConvert);
    if (fragment == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    pipeline = createFullTargetPipeline(fragment, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED);
    return pipeline;
}

VkPipeline DrawPipelines::layoutColorPipeline(VkFormat color_format) {
    const auto cached = layout_color_pipelines_.find(color_format);
    if (cached != layout_color_pipelines_.end()) return cached->second;
    if (!createLayoutPassObjects()) return VK_NULL_HANDLE;
    const VkShaderModule fragment = layoutShader(LayoutShader::LayoutColor);
    if (fragment == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    const VkPipeline pipeline = createFullTargetPipeline(fragment, color_format, VK_FORMAT_UNDEFINED);
    if (pipeline != VK_NULL_HANDLE) layout_color_pipelines_.emplace(color_format, pipeline);
    return pipeline;
}

VkPipeline DrawPipelines::layoutDepthPipeline(VkFormat depth_format) {
    const auto cached = layout_depth_pipelines_.find(depth_format);
    if (cached != layout_depth_pipelines_.end()) return cached->second;
    if (!createLayoutPassObjects()) return VK_NULL_HANDLE;
    const bool has_stencil =
            depth_format == VK_FORMAT_D24_UNORM_S8_UINT || depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    LayoutShader which = LayoutShader::LayoutDepth;
    if (has_stencil) {
        which = context_.hasStencilExport() ? LayoutShader::LayoutDepthStencilExport : LayoutShader::LayoutDepthStencil;
    }
    const VkShaderModule fragment = layoutShader(which);
    if (fragment == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    const VkPipeline pipeline = createFullTargetPipeline(fragment, VK_FORMAT_UNDEFINED, depth_format);
    if (pipeline != VK_NULL_HANDLE) layout_depth_pipelines_.emplace(depth_format, pipeline);
    return pipeline;
}

}  // namespace tox2

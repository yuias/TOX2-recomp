#include "render/vp_translator.h"

#include <algorithm>
#include <format>

#include "rsx/rsx_registers.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kNoTemp = 63;
constexpr std::uint32_t kNoOutput = 31;
constexpr std::uint32_t kOutputSlotCount = 16;
constexpr std::uint32_t kCondAlways = 7;

// Bits of the output mask register for the slots the translator exports (bit = column of the
// register; slots without a bit, like position, are always exported).
constexpr std::uint32_t kMaskFrontDiffuse = 1u << 0;
constexpr std::uint32_t kMaskFrontSpecular = 1u << 1;
constexpr std::uint32_t kMaskBackDiffuse = 1u << 2;
constexpr std::uint32_t kMaskBackSpecular = 1u << 3;
constexpr std::uint32_t kMaskFog = 1u << 4;
constexpr std::uint32_t kMaskPointSize = 1u << 5;
constexpr std::uint32_t kMaskTex8 = 1u << 12;
constexpr unsigned kMaskTex0Bit = 14;

std::uint32_t bits(std::uint32_t value, unsigned lo, unsigned count) { return (value >> lo) & ((1u << count) - 1u); }

const char* const kVecNames[0x1a] = {"NOP", "MOV", "MUL", "ADD", "MAD", "DP3",   "DPH",   "DP4", "DST",
                                     "MIN", "MAX", "SLT", "SGE", "ARL", "FRC",   "FLR",   "SEQ", "SFL",
                                     "SGT", "SLE", "SNE", "STR", "SSG", nullptr, nullptr, "TXL"};
const char* const kScaNames[0x15] = {"NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT", "BRA", "BRI", "CAL",
                                     "CLI", "RET", "LG2", "EX2", "SIN", "COS", "BRB", "CLB", "PSH", "POP"};

std::string opcodeText(const char* const* names, std::size_t count, std::uint32_t op) {
    if (op < count && names[op] != nullptr) return names[op];
    return std::format("0x{:02x}", op);
}

struct Instruction {
    std::uint32_t w[4];

    std::uint32_t vecOpcode() const { return bits(w[1], 22, 5); }
    std::uint32_t scaOpcode() const { return bits(w[1], 27, 5); }
    std::uint32_t source(int index) const {
        switch (index) {
            case 0: return (bits(w[1], 0, 8) << 9) | bits(w[2], 23, 9);
            case 1: return bits(w[2], 6, 17);
            default: return (bits(w[2], 0, 6) << 11) | bits(w[3], 21, 11);
        }
    }
};

// A GLSL expression. `compound` marks a binary expression that needs parentheses before a swizzle.
struct Expr {
    std::string text;
    bool compound = false;
};

std::string maskLanes(std::uint32_t mask_x, std::uint32_t mask_y, std::uint32_t mask_z, std::uint32_t mask_w) {
    std::string lanes;
    if (mask_x) lanes += 'x';
    if (mask_y) lanes += 'y';
    if (mask_z) lanes += 'z';
    if (mask_w) lanes += 'w';
    return lanes;
}

class Translator {
public:
    GlslResult run(std::span<const std::uint32_t> ucode);

private:
    void note(std::string message);
    void translateInstruction(const Instruction& in);
    std::string operand(const Instruction& in, int index);
    bool vectorExpr(const Instruction& in, std::uint32_t op, Expr& out);
    bool scalarExpr(const Instruction& in, std::uint32_t op, Expr& out);
    void emitWrite(const std::vector<std::string>& destinations, const Expr& value, const std::string& lanes);
    std::string exported(std::uint32_t slot, std::uint32_t mask, const char* member = "") const;
    std::string assemble() const;

    std::vector<std::string> unimplemented_;
    std::string body_;
    std::uint64_t temps_used_ = 0;
    std::uint32_t inputs_used_ = 0;
    std::uint32_t slots_written_ = 0;
    bool uses_lit_ = false;
};

void Translator::note(std::string message) {
    if (std::find(unimplemented_.begin(), unimplemented_.end(), message) == unimplemented_.end()) {
        unimplemented_.push_back(std::move(message));
    }
}

std::string Translator::operand(const Instruction& in, int index) {
    const std::uint32_t raw = in.source(index);
    std::string text;
    switch (bits(raw, 0, 2)) {
        case 1: {
            const std::uint32_t temp = bits(raw, 2, 6);
            temps_used_ |= 1ull << temp;
            text = std::format("r_{}", temp);
            break;
        }
        case 2: {
            const std::uint32_t input = bits(in.w[1], 8, 4);
            inputs_used_ |= 1u << input;
            text = std::format("attr_{}", input);
            break;
        }
        case 3: {
            const std::uint32_t constant = bits(in.w[1], 12, 10);
            if (bits(in.w[3], 1, 1) != 0) note("vp relative constant addressing");
            if (constant >= kRsxVpConstantCount) note(std::format("vp constant index {}", constant));
            text = std::format("vp_consts.c[{}]", constant);
            break;
        }
        default:
            note("vp source type 0");
            text = "vec4(0.0)";
            break;
    }

    // The x lane pair is the high pair of the source field.
    const std::uint32_t lanes[4] = {bits(raw, 14, 2), bits(raw, 12, 2), bits(raw, 10, 2), bits(raw, 8, 2)};
    if (lanes[0] != 0 || lanes[1] != 1 || lanes[2] != 2 || lanes[3] != 3) {
        text += '.';
        for (const std::uint32_t lane : lanes) text += "xyzw"[lane];
    }
    if (bits(in.w[0], 21 + static_cast<unsigned>(index), 1) != 0) text = "abs(" + text + ")";
    if (bits(raw, 16, 1) != 0) text = "-" + text;
    return text;
}

bool Translator::vectorExpr(const Instruction& in, std::uint32_t op, Expr& out) {
    // Only operands the opcode reads are decoded, so an unused field cannot raise a false report.
    switch (op) {
        case 0x01:  // MOV
            out = {operand(in, 0), false};
            return true;
        case 0x02:  // MUL
            out = {operand(in, 0) + " * " + operand(in, 1), true};
            return true;
        case 0x03:  // ADD uses src0 and src2; src1 is ignored
            out = {operand(in, 0) + " + " + operand(in, 2), true};
            return true;
        case 0x04:  // MAD
            out = {"fma(" + operand(in, 0) + ", " + operand(in, 1) + ", " + operand(in, 2) + ")", false};
            return true;
        case 0x05: {  // DP3
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(dot(" + a + ".xyz, " + b + ".xyz))", false};
            return true;
        }
        case 0x06: {  // DPH
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(dot(vec4(" + a + ".xyz, 1.0), " + b + "))", false};
            return true;
        }
        case 0x07: {  // DP4
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(dot(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x08: {  // DST
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)", false};
            return true;
        }
        case 0x09: {  // MIN
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"min(" + a + ", " + b + ")", false};
            return true;
        }
        case 0x0a: {  // MAX
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"max(" + a + ", " + b + ")", false};
            return true;
        }
        case 0x0b: {  // SLT
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(lessThan(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x0c: {  // SGE
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(greaterThanEqual(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x0e: {  // FRC
            const std::string a = operand(in, 0);
            out = {a + " - floor(" + a + ")", true};
            return true;
        }
        case 0x0f:  // FLR
            out = {"floor(" + operand(in, 0) + ")", false};
            return true;
        case 0x10: {  // SEQ
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(equal(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x11:  // SFL
            out = {"vec4(0.0)", false};
            return true;
        case 0x12: {  // SGT
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(greaterThan(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x13: {  // SLE
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(lessThanEqual(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x14: {  // SNE
            const std::string a = operand(in, 0), b = operand(in, 1);
            out = {"vec4(notEqual(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x15:  // STR
            out = {"vec4(1.0)", false};
            return true;
        case 0x16:  // SSG
            out = {"sign(" + operand(in, 0) + ")", false};
            return true;
        case 0x0d:  // ARL writes the address register
            note("vp relative addressing (ARL)");
            return false;
        default: note("vp opcode " + opcodeText(kVecNames, std::size(kVecNames), op)); return false;
    }
}

bool Translator::scalarExpr(const Instruction& in, std::uint32_t op, Expr& out) {
    switch (op) {
        case 0x01:  // MOV
            out = {operand(in, 2), false};
            return true;
        case 0x02:  // RCP
            out = {"vec4(1.0 / " + operand(in, 2) + ".x)", false};
            return true;
        case 0x03:  // RCC
            out = {"vec4(clamp(1.0 / " + operand(in, 2) + ".x, 5.42101e-20, 1.884467e19))", false};
            return true;
        case 0x04:  // RSQ
            out = {"vec4(1.0 / sqrt(max(" + operand(in, 2) + ".x, 1e-10)))", false};
            return true;
        case 0x05:  // EXP: natural, per component, no clamp
            out = {"exp(" + operand(in, 2) + ")", false};
            return true;
        case 0x06:  // LOG: natural, per component, no clamp
            out = {"log(" + operand(in, 2) + ")", false};
            return true;
        case 0x07:  // LIT
            uses_lit_ = true;
            out = {"lit_op(" + operand(in, 2) + ")", false};
            return true;
        case 0x0d:  // LG2
            out = {"vec4(log2(max(" + operand(in, 2) + ".x, 1e-10)))", false};
            return true;
        case 0x0e:  // EX2
            out = {"vec4(exp2(" + operand(in, 2) + ".x))", false};
            return true;
        case 0x0f:  // SIN
            out = {"vec4(sin(" + operand(in, 2) + ".x))", false};
            return true;
        case 0x10:  // COS
            out = {"vec4(cos(" + operand(in, 2) + ".x))", false};
            return true;
        default:
            // Every branch, call and stack opcode.
            note("vp scalar opcode " + opcodeText(kScaNames, std::size(kScaNames), op));
            return false;
    }
}

void Translator::emitWrite(const std::vector<std::string>& destinations, const Expr& value, const std::string& lanes) {
    const bool full = lanes == "xyzw";
    if (destinations.size() == 1) {
        std::string rhs = value.compound ? "(" + value.text + ")" : value.text;
        if (full) {
            body_ += std::format("  {} = {};\n", destinations[0], value.text);
        } else {
            body_ += std::format("  {}.{} = {}.{};\n", destinations[0], lanes, rhs, lanes);
        }
        return;
    }
    // One value for several destinations: evaluate once.
    body_ += std::format("  {{\n    vec4 res = {};\n", value.text);
    for (const std::string& dest : destinations) {
        if (full) {
            body_ += std::format("    {} = res;\n", dest);
        } else {
            body_ += std::format("    {}.{} = res.{};\n", dest, lanes, lanes);
        }
    }
    body_ += "  }\n";
}

void Translator::translateInstruction(const Instruction& in) {
    const std::uint32_t w0 = in.w[0], w3 = in.w[3];
    const std::uint32_t vec_op = in.vecOpcode(), sca_op = in.scaOpcode();

    // Condition 0 skips the instruction whatever the enable bit says. Other conditions need the
    // condition registers; an enable bit that is clear or condition "always" means unconditional.
    if (bits(w0, 10, 3) == 0) return;
    if (bits(w0, 13, 1) != 0 && bits(w0, 10, 3) != kCondAlways) {
        note("vp condition other than always");
        return;
    }
    if (bits(w0, 14, 1) != 0 || bits(w0, 29, 1) != 0) {
        note("vp condition code update");
        return;
    }

    const std::uint32_t vec_temp = bits(w0, 15, 6);
    const std::uint32_t sca_temp = bits(w3, 7, 6);
    const std::uint32_t slot = bits(w3, 2, 5);
    const bool vector_to_output = bits(w0, 30, 1) != 0;
    const bool has_slot = slot != kNoOutput;
    if (has_slot && slot >= kOutputSlotCount) {
        note(std::format("vp output slot {}", slot));
        return;
    }
    const bool saturate = bits(w0, 26, 1) != 0;

    struct Part {
        std::uint32_t op;
        std::uint32_t temp;
        bool to_output;
        std::string lanes;
    };
    const Part parts[2] = {
            {vec_op, vec_temp, has_slot && vector_to_output,
             maskLanes(bits(w3, 16, 1), bits(w3, 15, 1), bits(w3, 14, 1), bits(w3, 13, 1))},
            {sca_op, sca_temp, has_slot && !vector_to_output,
             maskLanes(bits(w3, 20, 1), bits(w3, 19, 1), bits(w3, 18, 1), bits(w3, 17, 1))},
    };

    // The vector op runs first; a scalar op that reads a temp the vector op wrote sees the new value.
    for (int stage = 0; stage < 2; stage++) {
        const Part& part = parts[stage];
        if (part.op == 0) continue;

        Expr value;
        if (!(stage == 0 ? vectorExpr(in, part.op, value) : scalarExpr(in, part.op, value))) continue;
        if (saturate) value = {"clamp(" + value.text + ", 0.0, 1.0)", false};

        std::vector<std::string> destinations;
        if (part.temp != kNoTemp) {
            temps_used_ |= 1ull << part.temp;
            destinations.push_back(std::format("r_{}", part.temp));
        }
        if (part.to_output) destinations.push_back(std::format("out_{}", slot));
        if (destinations.empty()) {
            // Hardware would write a condition register instead.
            note("vp condition code write");
            continue;
        }
        if (part.lanes.empty()) continue;
        if (part.to_output) slots_written_ |= 1u << slot;
        emitWrite(destinations, value, part.lanes);
    }
}

// GLSL for an exported slot: the value when the draw's output mask enables it and the program
// wrote the slot, otherwise the (0, 0, 0, 1) default.
std::string Translator::exported(std::uint32_t slot, std::uint32_t mask, const char* member) const {
    if ((slots_written_ & (1u << slot)) == 0) return "vec4(0.0, 0.0, 0.0, 1.0)";
    std::string value = std::format("out_{}", slot);
    if (member[0] != '\0') value = std::format("vec4({}.{})", value, member);
    return std::format("(draw.output_mask & {}u) != 0u ? {} : vec4(0.0, 0.0, 0.0, 1.0)", mask, value);
}

std::string Translator::assemble() const {
    std::string glsl =
            "#version 450\n"
            "\n"
            "layout(std140, set = 0, binding = 0) uniform VpConstants {\n"
            "  vec4 c[" +
            std::to_string(kRsxVpConstantCount) +
            "];\n"
            "} vp_consts;\n"
            "\n"
            "layout(std140, set = 0, binding = 2) uniform VpDraw {\n"
            "  vec4 viewport_scale;\n"
            "  vec4 viewport_offset;\n"
            "  vec4 clip_params;  // clip width, clip height, depth clip min, depth clip max\n"
            "  uint output_mask;\n"
            "  float point_size;\n"
            "  uint reserved0;\n"
            "  uint reserved1;\n"
            "} draw;\n"
            "\n";

    for (std::uint32_t input = 0; input < 16; input++) {
        if ((inputs_used_ & (1u << input)) != 0) {
            glsl += std::format("layout(location = {}) in vec4 attr_{};\n", input, input);
        }
    }
    glsl += "\n";
    for (std::uint32_t i = 0; i < kVaryingTexCoordCount; i++) {
        glsl += std::format("layout(location = {}) out vec4 var_tex{};\n", i, i);
    }
    glsl += std::format(
            "layout(location = {}) out vec4 var_front_diffuse;\n"
            "layout(location = {}) out vec4 var_back_diffuse;\n"
            "layout(location = {}) out vec4 var_front_specular;\n"
            "layout(location = {}) out vec4 var_back_specular;\n"
            "layout(location = {}) out vec4 var_fog;\n",
            kVaryingFrontDiffuse, kVaryingBackDiffuse, kVaryingFrontSpecular, kVaryingBackSpecular, kVaryingFog);
    glsl += "\ninvariant gl_Position;\n\n";

    if (uses_lit_) {
        glsl += "vec4 lit_op(vec4 v) {\n"
                "  vec4 t = vec4(max(v.x, 0.0), max(v.y, 1e-10), v.z, v.w);\n"
                "  return vec4(1.0, t.x, t.x > 0.0 ? exp2(t.w * log2(t.y)) : 0.0, 1.0);\n"
                "}\n\n";
    }

    glsl += "void main() {\n";
    for (std::uint32_t temp = 0; temp < 64; temp++) {
        if ((temps_used_ & (1ull << temp)) != 0) glsl += std::format("  vec4 r_{} = vec4(0.0);\n", temp);
    }
    for (std::uint32_t slot = 0; slot < kOutputSlotCount; slot++) {
        if ((slots_written_ & (1u << slot)) != 0) {
            glsl += std::format("  vec4 out_{} = vec4(0.0, 0.0, 0.0, 1.0);\n", slot);
        }
    }
    glsl += body_;

    // Epilogue. Outputs the program did not write, or the output mask disables, are (0, 0, 0, 1).
    const std::string_view kDefault = "vec4(0.0, 0.0, 0.0, 1.0)";
    glsl += "\n";
    for (std::uint32_t i = 0; i < kVaryingTexCoordCount; i++) {
        std::string value(kDefault);
        if (i < 8) {
            value = exported(7 + i, 1u << (kMaskTex0Bit + i));
        } else if (i == 8) {
            value = exported(15, kMaskTex8);
        }
        glsl += std::format("  var_tex{} = {};\n", i, value);
    }
    glsl += std::format("  var_front_diffuse = {};\n", exported(1, kMaskFrontDiffuse | kMaskBackDiffuse));
    glsl += std::format("  var_front_specular = {};\n", exported(2, kMaskFrontSpecular | kMaskBackSpecular));
    glsl += std::format("  var_back_diffuse = {};\n", exported(3, kMaskFrontDiffuse | kMaskBackDiffuse));
    glsl += std::format("  var_back_specular = {};\n", exported(4, kMaskFrontSpecular | kMaskBackSpecular));
    glsl += std::format("  var_fog = {};\n", exported(5, kMaskFog, "x"));
    glsl += std::format(
            "  gl_PointSize = {};\n",
            (slots_written_ & (1u << 6)) != 0
                    ? std::format("(draw.output_mask & {}u) != 0u ? out_6.x : draw.point_size", kMaskPointSize)
                    : std::string("draw.point_size"));

    // Viewport transform into clip space normalised to the surface clip size, without a y flip,
    // then the depth range remap onto a 24-bit grid (float: the device is not required to have
    // shaderFloat64).
    glsl += std::string(
                    "\n"
                    "  vec4 pos = ") +
            ((slots_written_ & 1u) != 0 ? "out_0" : std::string(kDefault)) +
            ";\n"
            "  vec2 half_clip = 0.5 * draw.clip_params.xy;\n"
            "  pos = vec4(\n"
            "      pos.x * (draw.viewport_scale.x / half_clip.x) +\n"
            "          pos.w * ((draw.viewport_offset.x - half_clip.x) / half_clip.x),\n"
            "      pos.y * (draw.viewport_scale.y / half_clip.y) +\n"
            "          pos.w * ((draw.viewport_offset.y - half_clip.y) / half_clip.y),\n"
            "      pos.z * draw.viewport_scale.z + pos.w * draw.viewport_offset.z,\n"
            "      pos.w);\n"
            // The identity range (the common case) keeps z exact instead of going through the grid.
            "  bool identity_depth = draw.clip_params.z == 0.0 && draw.clip_params.w == 1.0;\n"
            "  if (pos.w != 0.0 && !identity_depth) {\n"
            "    float depth_near = min(draw.clip_params.z, draw.clip_params.w);\n"
            "    float depth_far = max(draw.clip_params.z, draw.clip_params.w);\n"
            "    float range = depth_far - depth_near;\n"
            "    float d = 0.0;\n"
            "    if (range > 1e-6) {\n"
            "      d = (pos.z - depth_near * pos.w) / (range * pos.w);\n"
            "      float nearest = floor(d + 0.5);\n"
            "      if (abs(d - nearest) < 1.0 / (range * 16777215.0)) d = nearest;\n"
            "    }\n"
            "    pos.z = d * pos.w;\n"
            "  }\n"
            "  gl_Position = pos;\n"
            "}\n";
    return glsl;
}

GlslResult Translator::run(std::span<const std::uint32_t> ucode) {
    GlslResult result;
    if (ucode.size() < 4 || ucode.size() % 4 != 0) {
        result.unimplemented.push_back("vp malformed ucode size");
        return result;
    }
    for (std::size_t i = 0; i + 4 <= ucode.size(); i += 4) {
        const Instruction in = {{ucode[i], ucode[i + 1], ucode[i + 2], ucode[i + 3]}};
        translateInstruction(in);
        if ((in.w[3] & 1u) != 0) break;
    }

    // Without a position write the whole program is disabled.
    if ((slots_written_ & 1u) == 0) {
        body_.clear();
        slots_written_ = 0;
        temps_used_ = 0;
        inputs_used_ = 0;
        uses_lit_ = false;
    }

    result.unimplemented = std::move(unimplemented_);
    if (result.unimplemented.empty()) {
        result.glsl = assemble();
        result.inputs_used = inputs_used_;
        result.ok = true;
    }
    return result;
}

}  // namespace

GlslResult translateVertexProgram(std::span<const std::uint32_t> ucode, std::uint32_t) {
    return Translator().run(ucode);
}

}  // namespace tox2

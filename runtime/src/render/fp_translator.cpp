#include "render/fp_translator.h"

#include <algorithm>
#include <format>

namespace tox2 {
namespace {

// Execute mask bits (src0 bits 18-20): the instruction runs in lanes whose condition register
// value compares as less, equal or greater than zero.
constexpr std::uint32_t kCondLess = 1;
constexpr std::uint32_t kCondEqual = 2;
constexpr std::uint32_t kCondGreater = 4;
constexpr std::uint32_t kCondNever = 0;
constexpr std::uint32_t kCondAlways = kCondLess | kCondEqual | kCondGreater;

constexpr std::uint32_t kOpKil = 0x12;
constexpr std::uint32_t kOpFenceTexture = 0x3d;  // FENCT
constexpr std::uint32_t kOpFenceBranch = 0x3e;   // FENCB

constexpr std::uint32_t kSourceTemp = 0;
constexpr std::uint32_t kSourceInput = 1;
constexpr std::uint32_t kSourceConstant = 2;

// Input attribute register numbers (W0 bits 13-16).
constexpr std::uint32_t kInputWpos = 0;
constexpr std::uint32_t kInputCol0 = 1;
constexpr std::uint32_t kInputCol1 = 2;
constexpr std::uint32_t kInputFog = 3;
constexpr std::uint32_t kInputTex0 = 4;
constexpr std::uint32_t kInputTex9 = 13;
constexpr std::uint32_t kInputSsa = 14;

// Source and destination precision modifier values.
constexpr std::uint32_t kPrecisionHalf = 1;
constexpr std::uint32_t kPrecisionFixed12 = 2;
constexpr std::uint32_t kPrecisionFixed9 = 3;
constexpr std::uint32_t kPrecisionSaturate = 4;

std::uint32_t bits(std::uint32_t value, unsigned lo, unsigned count) { return (value >> lo) & ((1u << count) - 1u); }

struct Source {
    std::uint32_t type, index, half, swizzle, negate, absolute, precision;
};

// One instruction with the halfword swap already undone.
struct Instruction {
    std::uint32_t w[4];

    std::uint32_t opcode() const { return bits(w[0], 24, 6) | (bits(w[2], 31, 1) << 6); }
    bool hasConstant() const {
        for (int i = 1; i < 4; i++) {
            if (bits(w[i], 0, 2) == kSourceConstant) return true;
        }
        return false;
    }
    Source source(int index) const {
        const std::uint32_t word = w[1 + index];
        return {bits(word, 0, 2), bits(word, 2, 6), bits(word, 8, 1), bits(word, 9, 8), bits(word, 17, 1),
                // Source 0 keeps its abs bit at the top of the word, after the condition fields.
                index == 0 ? bits(word, 29, 1) : bits(word, 18, 1),
                bits(w[2], 19 + 3 * static_cast<unsigned>(index), 3)};
    }
};

// A GLSL expression. `compound` marks a binary expression that needs parentheses before a swizzle.
struct Expr {
    std::string text;
    bool compound = false;
};

class Translator {
public:
    GlslResult run(std::span<const std::uint32_t> ucode, std::uint32_t control, std::uint32_t texcoord_control,
                   std::uint32_t texture_types, bool object_motion);

private:
    void note(std::string message);
    void translateInstruction(const Instruction& in, int constant_slot);
    std::string operand(const Instruction& in, int index, int constant_slot);
    std::string inputName(std::uint32_t input);
    std::string clampText(const std::string& text, const char* lo, const char* hi);
    std::string halfText(const std::string& text);
    bool computeValue(const Instruction& in, std::uint32_t op, int constant_slot, Expr& out);
    std::string predicate(const Instruction& in, std::uint32_t exec_mask);
    std::string destination(const Instruction& in);
    std::string assemble() const;

    std::vector<std::string> unimplemented_;
    std::string body_;
    std::uint64_t temps_used_[2] = {};  // [0] = R, [1] = H
    std::uint32_t texcoord_control_ = 0;
    std::uint32_t texture_types_ = 0;  // kFpTextureType* per unit, 2 bits each
    std::uint32_t control_ = 0;
    bool object_motion_ = false;
    std::uint32_t tex_inputs_used_ = 0;  // bit n = TEXn read
    std::uint32_t texture_units_ = 0;
    std::uint32_t constant_count_ = 0;   // inline constant blocks seen, referenced or not
    std::uint32_t constant_extent_ = 0;  // fc[] entries the emitted code reads
    std::uint32_t cc_used_ = 0;          // bit n = condition register ccN is read or written
    bool col0_used_ = false;
    bool col1_used_ = false;
    bool uses_clamp_ = false;
    bool uses_half_ = false;
    bool uses_divsq_ = false;
};

void Translator::note(std::string message) {
    if (std::find(unimplemented_.begin(), unimplemented_.end(), message) == unimplemented_.end()) {
        unimplemented_.push_back(std::move(message));
    }
}

std::string Translator::clampText(const std::string& text, const char* lo, const char* hi) {
    uses_clamp_ = true;
    return std::format("fp_clamp({}, {}, {})", text, lo, hi);
}

std::string Translator::halfText(const std::string& text) {
    uses_half_ = true;
    return "fp_half(" + text + ")";
}

// Name of the GLSL variable holding an input register's value.
std::string Translator::inputName(std::uint32_t input) {
    if (input == kInputCol0) {
        col0_used_ = true;
        return "in_col0";
    }
    if (input == kInputCol1) {
        col1_used_ = true;
        return "in_col1";
    }
    if (input >= kInputTex0 && input <= kInputTex9) {
        const std::uint32_t n = input - kInputTex0;
        tex_inputs_used_ |= 1u << n;
        return (texcoord_control_ >> n & 1u) != 0 ? std::format("in_tex{}", n) : std::format("var_tex{}", n);
    }
    switch (input) {
        case kInputWpos: note("fp input WPOS"); break;
        case kInputFog: note("fp input FOGC"); break;
        case kInputSsa: note("fp input SSA"); break;
        default: note(std::format("fp input {}", input)); break;
    }
    return "vec4(0.0)";
}

std::string Translator::operand(const Instruction& in, int index, int constant_slot) {
    const Source src = in.source(index);
    std::string text;
    switch (src.type) {
        case kSourceTemp: {
            temps_used_[src.half] |= 1ull << src.index;
            text = std::format("{}_{}", src.half != 0 ? 'h' : 'r', src.index);
            break;
        }
        case kSourceInput:
            text = inputName(bits(in.w[0], 13, 4));
            // W3 bit 30 applies to the whole instruction: a dynamically indexed input needs the address
            // register whichever source reads it.
            if (bits(in.w[3], 30, 1) != 0) note("fp indexed input");
            break;
        case kSourceConstant:
            constant_extent_ = std::max(constant_extent_, static_cast<std::uint32_t>(constant_slot) + 1);
            text = std::format("fc[{}]", constant_slot);
            break;
        default:
            note("fp source type 3");
            text = "vec4(0.0)";
            break;
    }

    // The x lane pair is the low pair of the swizzle field.
    const std::uint32_t lanes[4] = {bits(src.swizzle, 0, 2), bits(src.swizzle, 2, 2), bits(src.swizzle, 4, 2),
                                    bits(src.swizzle, 6, 2)};
    if (lanes[0] != 0 || lanes[1] != 1 || lanes[2] != 2 || lanes[3] != 3) {
        text += '.';
        for (const std::uint32_t lane : lanes) text += "xyzw"[lane];
    }

    // Modifier order: swizzle, [saturate clamp], abs, [other precision], negate.
    if (src.precision == kPrecisionSaturate) text = clampText(text, "0.0", "1.0");
    if (src.absolute != 0) text = "abs(" + text + ")";
    if (src.precision == kPrecisionHalf) text = halfText(text);
    if (src.precision == kPrecisionFixed12) text = clampText(text, "-2.0", "2.0");
    if (src.precision == kPrecisionFixed9) text = clampText(text, "-1.0", "1.0");
    if (src.negate != 0) text = "-" + text;
    return text;
}

bool Translator::computeValue(const Instruction& in, std::uint32_t op, int constant_slot, Expr& out) {
    // Only the operands an opcode reads are decoded, so an unused field cannot raise a false report.
    switch (op) {
        case 0x01:  // MOV
            out = {operand(in, 0, constant_slot), false};
            return true;
        case 0x02:  // MUL
            out = {operand(in, 0, constant_slot) + " * " + operand(in, 1, constant_slot), true};
            return true;
        case 0x03:  // ADD
            out = {operand(in, 0, constant_slot) + " + " + operand(in, 1, constant_slot), true};
            return true;
        case 0x04:  // MAD
            out = {"fma(" + operand(in, 0, constant_slot) + ", " + operand(in, 1, constant_slot) + ", " +
                           operand(in, 2, constant_slot) + ")",
                   false};
            return true;
        case 0x05: {  // DP3
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {"vec4(dot(" + a + ".xyz, " + b + ".xyz))", false};
            return true;
        }
        case 0x06: {  // DP4
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {"vec4(dot(" + a + ", " + b + "))", false};
            return true;
        }
        case 0x08: {  // MIN
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {"min(" + a + ", " + b + ")", false};
            return true;
        }
        case 0x09: {  // MAX
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {"max(" + a + ", " + b + ")", false};
            return true;
        }
        case 0x0a:    // SLT
        case 0x0b:    // SGE
        case 0x0c:    // SLE
        case 0x0d:    // SGT
        case 0x0e:    // SNE
        case 0x0f: {  // SEQ
            static constexpr const char* kCompare[] = {"lessThan",    "greaterThanEqual", "lessThanEqual",
                                                       "greaterThan", "notEqual",         "equal"};
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {std::format("vec4({}({}, {}))", kCompare[op - 0x0a], a, b), false};
            return true;
        }
        case 0x10:  // FRC
            out = {"fract(" + operand(in, 0, constant_slot) + ")", false};
            return true;
        case 0x11:  // FLR
            out = {"floor(" + operand(in, 0, constant_slot) + ")", false};
            return true;
        case 0x1a:  // RCP
            out = {"vec4(1.0 / " + operand(in, 0, constant_slot) + ".x)", false};
            return true;
        case 0x1c:  // EX2
            out = {"vec4(exp2(" + operand(in, 0, constant_slot) + ".x))", false};
            return true;
        case 0x1d:  // LG2
            out = {"vec4(log2(" + operand(in, 0, constant_slot) + ".x))", false};
            return true;
        case 0x3a: {  // DIV: vector numerator, scalar divisor from src1.x
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            out = {a + " / " + b + ".x", true};
            return true;
        }
        case 0x3b: {  // DIVSQ: vector numerator, scalar divisor from src1.x
            const std::string a = operand(in, 0, constant_slot), b = operand(in, 1, constant_slot);
            uses_divsq_ = true;
            out = {"fp_divsq(" + a + ", " + b + ".x)", false};
            return true;
        }
        case 0x17:    // TEX
        case 0x18: {  // TXP
            const std::uint32_t unit = bits(in.w[0], 17, 4);
            texture_units_ |= 1u << unit;
            const std::string coord = operand(in, 0, constant_slot);
            // The sampler type follows the texture bound to the unit. A 2D lookup reads two coordinates, a 3D
            // or cube lookup three; TXP divides them by w first. The unnormalized-coordinate scale applies to
            // 2D and 3D lookups only (a cube direction has no unnormalized form).
            const std::uint32_t type = fpTextureType(texture_types_, unit);
            const char* lanes = type == kFpTextureType2D ? "xy" : "xyz";
            const std::string scale = std::format("draw.texture_scale[{}].{}", unit, lanes);
            std::string uv = op == 0x17 ? std::format("{}.{}", coord, lanes)
                                        : std::format("({}.{} / {}.w)", coord, lanes, coord);
            if (type != kFpTextureTypeCube) uv += " * " + scale;
            // The expand (bx2) bit cannot be verified against the reference output.
            if (bits(in.w[0], 21, 1) != 0) {
                note("fp texture expand");
                return false;
            }
            out = {std::format("texture(tex{}, {})", unit, uv), false};
            return true;
        }
        default: note(std::format("fp opcode 0x{:02x}", op)); return false;
    }
}

std::string Translator::destination(const Instruction& in) {
    const std::uint32_t index = bits(in.w[0], 1, 6);
    const std::uint32_t half = bits(in.w[0], 7, 1);
    temps_used_[half] |= 1ull << index;
    return std::format("{}_{}", half != 0 ? 'h' : 'r', index);
}

void Translator::translateInstruction(const Instruction& in, int constant_slot) {
    const std::uint32_t op = in.opcode();
    // The fences order texture and branch work on the hardware and write nothing.
    if (op == 0x00 || op == kOpFenceTexture || op == kOpFenceBranch) return;

    // No condition bit set disables the instruction; all set is unconditional. A partial condition
    // gates each written lane on the condition register.
    const std::uint32_t cond = bits(in.w[1], 18, 3);
    if (cond == kCondNever) return;
    const std::string pred = predicate(in, cond);

    if (op == kOpKil) {
        // KIL has no destination; any lane passing the condition discards the fragment.
        body_ += pred.empty() ? "  discard;\n" : std::format("  if (any({})) discard;\n", pred);
        return;
    }

    Expr value;
    if (!computeValue(in, op, constant_slot, value)) return;
    const bool no_dest = bits(in.w[0], 30, 1) != 0;
    const bool set_cond = bits(in.w[0], 8, 1) != 0;
    if (no_dest && !set_cond) return;  // nothing observable

    // Result scale, destination precision, saturate, then the masked write.
    std::string text = value.compound ? "(" + value.text + ")" : value.text;
    switch (bits(in.w[2], 28, 3)) {
        case 0: text = value.text; break;
        case 1: text += " * 2.0"; break;
        case 2: text += " * 4.0"; break;
        case 3: text += " * 8.0"; break;
        case 5: text += " * 0.5"; break;
        case 6: text += " * 0.25"; break;
        case 7: text += " * 0.125"; break;
        default: note("fp result scale 4"); return;
    }
    // The description says some precision cases are skipped for MIN/MAX (and other ops) without
    // saying which; the modifier is applied to every opcode here.
    switch (bits(in.w[0], 22, 2)) {
        case kPrecisionHalf: text = halfText(text); break;
        case kPrecisionFixed12: text = clampText(text, "-2.0", "2.0"); break;
        case kPrecisionFixed9: text = clampText(text, "-1.0", "1.0"); break;
        default: break;
    }
    if (bits(in.w[0], 31, 1) != 0) text = clampText(text, "0.0", "1.0");

    std::string lanes;
    for (unsigned i = 0; i < 4; i++) {
        if (bits(in.w[0], 9 + i, 1) != 0) lanes += "xyzw"[i];
    }
    if (lanes.empty()) return;

    // The condition register takes the value after scale, precision and saturate, like the
    // destination does. With no destination only the register is written.
    const std::string cc_name = std::format("cc{}", bits(in.w[1], 30, 1));
    if (set_cond) cc_used_ |= 1u << bits(in.w[1], 30, 1);
    const std::string suffix = lanes == "xyzw" ? "" : "." + lanes;
    // A swizzle applies to a whole expression, so the masked form parenthesises it.
    const std::string rhs = suffix.empty() ? text : "(" + text + ")" + suffix;
    const auto assign = [&](const std::string& target) {
        if (pred.empty()) {
            body_ += std::format("  {}{} = {};\n", target, suffix, rhs);
        } else {
            body_ += std::format("  {0}{1} = mix({0}{1}, {2}, {3}{1});\n", target, suffix, rhs, pred);
        }
    };
    if (no_dest) {
        assign(cc_name);
        return;
    }
    const std::string dest = destination(in);
    assign(dest);
    if (set_cond) body_ += std::format("  {}{} = {}{};\n", cc_name, suffix, dest, suffix);
}

// A bvec4 expression that is true in the lanes where the instruction runs, or empty when it always
// runs. Lane i of the result compares condition register lane swizzle[i] against zero.
std::string Translator::predicate(const Instruction& in, std::uint32_t exec_mask) {
    if (exec_mask == kCondAlways) return {};
    const std::uint32_t reg = bits(in.w[1], 31, 1);
    cc_used_ |= 1u << reg;
    std::string cc = std::format("cc{}", reg);
    const std::uint32_t swizzle = bits(in.w[1], 21, 8);
    if (swizzle != 0xe4) {
        cc += '.';
        for (unsigned i = 0; i < 4; i++) cc += "xyzw"[bits(swizzle, 2 * i, 2)];
    }
    const char* compare = "";
    switch (exec_mask) {
        case kCondGreater | kCondEqual: compare = "greaterThanEqual"; break;
        case kCondLess | kCondEqual: compare = "lessThanEqual"; break;
        case kCondGreater | kCondLess: compare = "notEqual"; break;
        case kCondGreater: compare = "greaterThan"; break;
        case kCondLess: compare = "lessThan"; break;
        default: compare = "equal"; break;  // kCondEqual
    }
    return std::format("{}({}, vec4(0.0))", compare, cc);
}

std::string Translator::assemble() const {
    std::string glsl = "#version 450\n\n";
    if (constant_extent_ != 0) {
        glsl += std::format("layout(std140, set = 0, binding = {}) uniform FpConstants {{\n  vec4 fc[{}];\n}};\n\n",
                            kFpConstantsBinding, constant_extent_);
    }
    glsl += std::format("layout(constant_id = {}) const bool kAlphaTest = true;\n\n", kFpAlphaTestConstantId);
    // The first 64 bytes belong to the vertex stage.
    glsl += std::format(
            "layout(std140, set = 0, binding = {}) uniform FpDraw {{\n"
            "  vec4 vertex_part[{}];\n"
            "  uint alpha_func;\n"
            "  float alpha_ref;\n"
            "  uint flags;\n"
            "  uint reserved;\n"
            "  vec4 texture_scale[{}];\n"
            "}} draw;\n\n",
            kFpDrawBinding, kFpDrawBlockOffset / 16, kFpTextureUnitCount);

    for (std::uint32_t unit = 0; unit < kFpTextureUnitCount; unit++) {
        if ((texture_units_ & (1u << unit)) != 0) {
            const std::uint32_t type = fpTextureType(texture_types_, unit);
            glsl += std::format("layout(set = 0, binding = {}) uniform {} tex{};\n", kFpTextureBindingBase + unit,
                                type == kFpTextureType3D     ? "sampler3D"
                                : type == kFpTextureTypeCube ? "samplerCube"
                                                             : "sampler2D",
                                unit);
        }
    }
    for (std::uint32_t n = 0; n < kVaryingTexCoordCount; n++) {
        if ((tex_inputs_used_ & (1u << n)) != 0) {
            glsl += std::format("layout(location = {}) in vec4 var_tex{};\n", n, n);
        }
    }
    if (col0_used_) {
        glsl += std::format(
                "layout(location = {}) in vec4 var_front_diffuse;\n"
                "layout(location = {}) in vec4 var_back_diffuse;\n",
                kVaryingFrontDiffuse, kVaryingBackDiffuse);
    }
    if (col1_used_) {
        glsl += std::format(
                "layout(location = {}) in vec4 var_front_specular;\n"
                "layout(location = {}) in vec4 var_back_specular;\n",
                kVaryingFrontSpecular, kVaryingBackSpecular);
    }
    if (object_motion_) {
        glsl += std::format(
                "layout(location = {}) in vec4 var_motion_cur;\n"
                "layout(location = {}) in vec4 var_motion_prev;\n"
                "layout(location = {}) flat in uint var_motion_valid;\n",
                kVaryingMotionCur, kVaryingMotionPrev, kVaryingMotionValid);
    }
    glsl += "layout(location = 0) out vec4 out_color0;\n";
    if (object_motion_) glsl += "layout(location = 1) out uvec2 out_object_motion;\n";
    glsl += "\n";

    if (uses_clamp_) {
        glsl += "// NaN becomes 0, like the hardware clamps.\n"
                "vec4 fp_clamp(vec4 v, float lo, float hi) {\n"
                "  return mix(clamp(v, lo, hi), vec4(0.0), isnan(v));\n"
                "}\n\n";
    }
    if (uses_divsq_) {
        // A zero numerator stays zero even for a zero divisor, and the divisor's sign is ignored.
        glsl += "vec4 fp_divsq(vec4 a, float b) {\n"
                "  return mix(a, a / sqrt(abs(b)), greaterThan(abs(a), vec4(0.0)));\n"
                "}\n\n";
    }
    if (uses_half_) {
        glsl += "vec4 fp_half(vec4 v) {\n"
                "  return vec4(unpackHalf2x16(packHalf2x16(v.xy)), unpackHalf2x16(packHalf2x16(v.zw)));\n"
                "}\n\n";
    }

    const bool color_is_full = (control_ & kFpControl32BitExport) != 0;
    const bool depth_export = (control_ & kFpControlDepthExport) != 0;
    std::uint64_t r_used = temps_used_[0], h_used = temps_used_[1];
    // The export registers exist even when the program never writes them.
    (color_is_full ? r_used : h_used) |= 1ull;
    if (depth_export) r_used |= 1ull << 1;

    glsl += "void main() {\n";
    for (std::uint32_t i = 0; i < 64; i++) {
        if ((r_used & (1ull << i)) != 0) glsl += std::format("  vec4 r_{} = vec4(0.0);\n", i);
    }
    for (std::uint32_t i = 0; i < 64; i++) {
        if ((h_used & (1ull << i)) != 0) glsl += std::format("  vec4 h_{} = vec4(0.0);\n", i);
    }
    for (std::uint32_t i = 0; i < 2; i++) {
        if ((cc_used_ & (1u << i)) != 0) glsl += std::format("  vec4 cc{} = vec4(0.0);\n", i);
    }
    // Colour reads are clamped to [0, 1]; two-sided lighting picks the back colour on back faces.
    const std::string back_face = std::format("(draw.flags & {}u) != 0u && !gl_FrontFacing", kFpFlagTwoSided);
    if (col0_used_) {
        glsl += std::format("  vec4 in_col0 = clamp({} ? var_back_diffuse : var_front_diffuse, 0.0, 1.0);\n",
                            back_face);
    }
    if (col1_used_) {
        glsl += std::format("  vec4 in_col1 = clamp({} ? var_back_specular : var_front_specular, 0.0, 1.0);\n",
                            back_face);
    }
    for (std::uint32_t n = 0; n < kVaryingTexCoordCount; n++) {
        if ((tex_inputs_used_ & texcoord_control_ & (1u << n)) != 0) {
            glsl += std::format("  vec4 in_tex{} = vec4(var_tex{}.xy, 0.0, gl_FragCoord.w);\n", n, n);
        }
    }
    glsl += body_;

    const char* color = color_is_full ? "r_0" : "h_0";
    // 8-bit render targets quantise the colour before the alpha test. The test sits under a specialization
    // constant so a pipeline with the test off has no discard from it (see kFpAlphaTestConstantId).
    glsl += std::format(
            "\n"
            "  if ((draw.flags & {1}u) != 0u) {0} = floor(max({0}, 0.0) * 255.0 + 0.5) / 255.0;\n"
            "  if (kAlphaTest) {{\n"
            "    bool alpha_passes = true;\n"
            "    switch (draw.alpha_func) {{\n"
            "      case 0u: alpha_passes = false; break;\n"
            "      case 1u: alpha_passes = {0}.a < draw.alpha_ref; break;\n"
            "      case 2u: alpha_passes = {0}.a == draw.alpha_ref; break;\n"
            "      case 3u: alpha_passes = {0}.a <= draw.alpha_ref; break;\n"
            "      case 4u: alpha_passes = {0}.a > draw.alpha_ref; break;\n"
            "      case 5u: alpha_passes = {0}.a != draw.alpha_ref; break;\n"
            "      case 6u: alpha_passes = {0}.a >= draw.alpha_ref; break;\n"
            "      default: break;\n"
            "    }}\n"
            "    if (!alpha_passes) discard;\n"
            "  }}\n"
            "  out_color0 = {0};\n",
            color, kFpFlagRound8Bit);
    if (depth_export) glsl += "  gl_FragDepth = fp_clamp(r_1, 0.0, 1.0).z;\n";
    if (object_motion_) {
        // After the discards, so a discarded fragment writes neither output. The depth is stored with the vector so
        // the combine pass can tell whether a later draw replaced this fragment.
        glsl += std::format(
                "\n"
                "  float motion_depth = {};\n"
                "  bool has_motion = var_motion_valid != 0u && var_motion_cur.w > 0.0 && var_motion_prev.w > 0.0;\n"
                "  vec2 mv = has_motion ? var_motion_prev.xy / var_motion_prev.w - var_motion_cur.xy / "
                "var_motion_cur.w : vec2(0.0);\n"
                "  out_object_motion = uvec2(packHalf2x16(mv), has_motion ? floatBitsToUint(motion_depth) : {}u);\n",
                depth_export ? "gl_FragDepth" : "gl_FragCoord.z", kObjectMotionNone);
    }
    glsl += "}\n";
    return glsl;
}

GlslResult Translator::run(std::span<const std::uint32_t> ucode, std::uint32_t control, std::uint32_t texcoord_control,
                           std::uint32_t texture_types, bool object_motion) {
    GlslResult result;
    control_ = control;
    object_motion_ = object_motion;
    texcoord_control_ = texcoord_control;
    texture_types_ = texture_types;
    if (ucode.size() < 4 || ucode.size() % 4 != 0) {
        result.unimplemented.push_back("fp malformed ucode size");
        return result;
    }
    if ((control & kFpControlDepthExport) != 0) uses_clamp_ = true;

    for (std::size_t i = 0; i + 4 <= ucode.size();) {
        Instruction in;
        for (std::size_t w = 0; w < 4; w++) in.w[w] = fpFixupWord(ucode[i + w]);
        // An instruction with a constant operand is followed by its 4-word constant block.
        const bool has_constant = in.hasConstant();
        if (has_constant && i + 8 > ucode.size()) {
            note("fp truncated constant block");
            break;
        }
        const int constant_slot = has_constant ? static_cast<int>(constant_count_++) : -1;
        translateInstruction(in, constant_slot);
        if ((in.w[0] & 1u) != 0) break;
        i += has_constant ? 8 : 4;
    }

    result.unimplemented = std::move(unimplemented_);
    if (result.unimplemented.empty()) {
        result.glsl = assemble();
        result.texture_units = texture_units_;
        result.ok = true;
    }
    return result;
}

}  // namespace

GlslResult translateFragmentProgram(std::span<const std::uint32_t> ucode, std::uint32_t control,
                                    std::uint32_t texcoord_control, std::uint32_t texture_types, bool object_motion) {
    return Translator().run(ucode, control, texcoord_control, texture_types, object_motion);
}

}  // namespace tox2

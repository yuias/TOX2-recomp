#include "render/vp_translator.h"

#include <algorithm>
#include <format>

#include "rsx/rsx_census.h"
#include "rsx/rsx_registers.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kNoTemp = 63;
constexpr std::uint32_t kNoOutput = 31;
constexpr std::uint32_t kOutputSlotCount = 16;
constexpr std::uint32_t kCondAlways = 7;
constexpr std::uint32_t kCondNever = 0;
// The hardware call stack is four deep; callees are inlined at their call sites, so the cap also
// bounds the size of the generated code.
constexpr int kMaxCallDepth = 8;

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

// Flow-control opcodes the translator emits code for. BRA is left out: its target comes from the
// address register, which the translator does not model.
bool isHandledFlowOp(std::uint32_t op) {
    return op == kRsxVpScaBri || op == kRsxVpScaCal || op == kRsxVpScaCli || op == kRsxVpScaRet || op == kRsxVpScaBrb ||
           op == kRsxVpScaClb;
}

std::string opcodeText(const char* const* names, std::size_t count, std::uint32_t op) {
    if (op < count && names[op] != nullptr) return names[op];
    return std::format("0x{:02x}", op);
}

struct Instruction {
    std::uint32_t w[4];

    std::uint32_t vecOpcode() const { return bits(w[1], 22, 5); }
    std::uint32_t scaOpcode() const { return bits(w[1], 27, 5); }
    std::uint32_t condition() const { return bits(w[0], 10, 3); }
    // Condition register component read for lane x, y, z, w (fields run w to x from bit 2 upward).
    std::string conditionSwizzle() const {
        std::string lanes;
        for (const unsigned lo : {8u, 6u, 4u, 2u}) lanes += "xyzw"[bits(w[0], lo, 2)];
        return lanes;
    }
    std::uint32_t conditionRegister() const { return bits(w[0], 25, 1); }
    // Bit 13 turns the condition into a per-lane write select; BRI, CAL, CLI and RET ignore it.
    bool conditionTest() const { return bits(w[0], 13, 1) != 0; }
    // The condition register is a fallback destination of one part (RPCS3 VertexProgramDecompiler
    // SetDST and GetDST): a part with a temp, or one that owns the output register, writes those and never
    // the condition register. Without either it writes the condition register when the instruction has no
    // output and an update bit (14 or 29) is set, or when the output belongs to the other part.
    bool updatesCondition(bool scalar_part) const {
        if ((scalar_part ? bits(w[3], 7, 6) : bits(w[0], 15, 6)) != kNoTemp) return false;
        if (bits(w[3], 2, 5) == kNoOutput) return bits(w[0], 14, 1) != 0 || bits(w[0], 29, 1) != 0;
        return (bits(w[0], 30, 1) != 0) == scalar_part;
    }
    bool end() const { return (w[3] & 1u) != 0; }
    std::uint32_t target() const { return rsxVpBranchTarget(w); }
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
    GlslResult run(std::span<const std::uint32_t> ucode, const VpTranslateOptions& options);

private:
    enum class Condition { Always, Never, Unsupported };

    void note(std::string message);
    void translateData(const Instruction& in);
    Instruction instructionAt(std::uint32_t pc) const;
    Condition branchCondition(const Instruction& in);
    std::string booleanCondition(const Instruction& in) const;
    void emitStraightLine();
    void emitControlFlow();
    void emitCall(const Instruction& in, int depth);
    void emitJump(const Instruction& in);
    std::uint32_t blockLevel(std::uint32_t target) const;
    std::string operand(const Instruction& in, int index);
    bool vectorExpr(const Instruction& in, std::uint32_t op, Expr& out);
    bool scalarExpr(const Instruction& in, std::uint32_t op, Expr& out);
    std::string conditionCompare(const Instruction& in);
    void emitWrite(const std::vector<std::string>& destinations, const Expr& value, const std::string& lanes,
                   const std::string& select);
    std::string exported(std::uint32_t slot, std::uint32_t mask, const char* member = "") const;
    std::string assemble() const;
    bool clipPlanesEmitted() const { return clip_distances_ && (slots_written_ & ((1u << 5) | (1u << 6))) != 0; }

    std::vector<std::string> unimplemented_;
    std::span<const std::uint32_t> ucode_;
    RsxVpFlow flow_;
    std::vector<std::uint32_t> labels_;  // sorted branch targets above 0; each starts a jump-position block
    std::string body_;
    std::uint64_t temps_used_ = 0;
    std::uint32_t inputs_used_ = 0;
    std::uint32_t slots_written_ = 0;
    std::uint32_t cc_used_ = 0;  // bit n: cc_n is read or written
    bool uses_lit_ = false;
    bool clip_distances_ = false;
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
            // BRA (target from the address register) and the stack opcodes.
            note("vp scalar opcode " + opcodeText(kScaNames, std::size(kScaNames), op));
            return false;
    }
}

// Compare of the swizzled condition register with zero, one boolean per lane. Lane l is the compare of
// component swizzle[l], which is what a destination lane l tests.
std::string Translator::conditionCompare(const Instruction& in) {
    static constexpr const char* kCompare[8] = {nullptr,    "lessThan",         "equal", "lessThanEqual", "greaterThan",
                                                "notEqual", "greaterThanEqual", nullptr};
    const std::uint32_t reg = in.conditionRegister();
    cc_used_ |= 1u << reg;
    std::string swizzle = in.conditionSwizzle();
    swizzle = swizzle == "xyzw" ? "" : "." + swizzle;
    return std::format("{}(cc_{}{}, vec4(0.0))", kCompare[in.condition()], reg, swizzle);
}

// `select` is empty for an unconditional write; otherwise it is a bvec4 expression and each written lane
// keeps its old value where the lane is false.
void Translator::emitWrite(const std::vector<std::string>& destinations, const Expr& value, const std::string& lanes,
                           const std::string& select) {
    const bool full = lanes == "xyzw";
    const std::string rhs = value.compound ? "(" + value.text + ")" : value.text;
    const auto assign = [&](const std::string& dest, const std::string& source, const std::string& source_text) {
        const std::string target = full ? dest : dest + "." + lanes;
        const std::string lane_source = full ? source_text : source + "." + lanes;
        if (select.empty()) return std::format("{} = {};\n", target, lane_source);
        return std::format("{0} = mix({0}, {1}, {2}{3});\n", target, lane_source, select,
                           full ? std::string() : "." + lanes);
    };
    if (destinations.size() == 1) {
        body_ += "  " + assign(destinations[0], rhs, value.text);
        return;
    }
    // One value for several destinations: evaluate once.
    body_ += std::format("  {{\n    vec4 res = {};\n", value.text);
    for (const std::string& dest : destinations) body_ += "    " + assign(dest, "res", "res");
    body_ += "  }\n";
}

// The vector and scalar data operations of one instruction; flow control is emitted by the callers.
void Translator::translateData(const Instruction& in) {
    const std::uint32_t w0 = in.w[0], w3 = in.w[3];
    const std::uint32_t vec_op = in.vecOpcode(), sca_op = in.scaOpcode();

    // Condition 0 skips the whole instruction (condition register update included) whatever the test bit says.
    if (in.condition() == kCondNever) return;
    const bool conditional = in.conditionTest() && in.condition() != kCondAlways;

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

    // The vector op runs first; the scalar op sees the temps and the condition register it wrote.
    for (int stage = 0; stage < 2; stage++) {
        const Part& part = parts[stage];
        if (part.op == 0) continue;
        // The target and branch index overlap the source 2 fields, and there is nothing to write.
        if (stage == 1 && isHandledFlowOp(part.op)) continue;

        // The operands are decoded first so that an opcode the translator lacks is reported even when the
        // part writes nothing; the registers they touched are forgotten again in that case.
        const std::uint64_t saved_temps = temps_used_;
        const std::uint32_t saved_inputs = inputs_used_, saved_cc = cc_used_;
        const bool saved_lit = uses_lit_;
        const std::size_t saved_notes = unimplemented_.size();
        Expr value;
        if (!(stage == 0 ? vectorExpr(in, part.op, value) : scalarExpr(in, part.op, value))) continue;
        if (saturate) value = {"clamp(" + value.text + ", 0.0, 1.0)", false};

        std::vector<std::string> destinations;
        if (part.temp != kNoTemp) {
            temps_used_ |= 1ull << part.temp;
            destinations.push_back(std::format("r_{}", part.temp));
        }
        if (part.to_output) destinations.push_back(std::format("out_{}", slot));
        if (destinations.empty() && in.updatesCondition(stage == 1)) {
            const std::uint32_t reg = in.conditionRegister();
            cc_used_ |= 1u << reg;
            destinations.push_back(std::format("cc_{}", reg));
        }
        if (destinations.empty() || part.lanes.empty()) {
            // Nothing is written: the part leaves no trace in the program.
            temps_used_ = saved_temps;
            inputs_used_ = saved_inputs;
            cc_used_ = saved_cc;
            uses_lit_ = saved_lit;
            unimplemented_.resize(saved_notes);
            continue;
        }
        if (part.to_output) slots_written_ |= 1u << slot;
        emitWrite(destinations, value, part.lanes, conditional ? conditionCompare(in) : std::string());
    }
}

Instruction Translator::instructionAt(std::uint32_t pc) const {
    return {{ucode_[pc * 4], ucode_[pc * 4 + 1], ucode_[pc * 4 + 2], ucode_[pc * 4 + 3]}};
}

// The condition of BRI, CAL, CLI and RET. Anything but "always" and "never" tests a condition
// register, which the branches do not model yet.
Translator::Condition Translator::branchCondition(const Instruction& in) {
    switch (in.condition()) {
        case kCondAlways: return Condition::Always;
        case kCondNever: return Condition::Never;
        default: note("vp branch condition other than always"); return Condition::Unsupported;
    }
}

// BRB and CLB test bit `index` of the branch bits register and ignore the condition field.
std::string Translator::booleanCondition(const Instruction& in) const {
    const bool taken_when_set = bits(in.w[3], 28, 1) != 0;
    return std::format("(draw.branch_bits & (1u << {})) {} 0u", bits(in.w[3], 23, 5), taken_when_set ? "!=" : "==");
}

// Jump-position level of the block that starts at `target`; level 0 is the block at the program start.
std::uint32_t Translator::blockLevel(std::uint32_t target) const {
    if (target == 0) return 0;
    return 1 + static_cast<std::uint32_t>(std::lower_bound(labels_.begin(), labels_.end(), target) - labels_.begin());
}

void Translator::emitJump(const Instruction& in) {
    std::string condition;
    if (in.scaOpcode() == kRsxVpScaBrb) {
        condition = booleanCondition(in);
    } else if (branchCondition(in) != Condition::Always) {
        return;
    }
    const std::uint32_t level = blockLevel(in.target());
    if (condition.empty()) {
        body_ += std::format("  jump_position = {};\n  continue;\n", level);
    } else {
        body_ += std::format("  if ({}) {{\n    jump_position = {};\n    continue;\n  }}\n", condition, level);
    }
}

// Callees are inlined at the call site, so a subroutine needs no call stack. Branches inside a
// subroutine would need their own jump-position blocks and are reported instead.
void Translator::emitCall(const Instruction& call, int depth) {
    std::string condition;
    if (call.scaOpcode() == kRsxVpScaClb) {
        condition = booleanCondition(call);
    } else if (branchCondition(call) != Condition::Always) {
        return;
    }
    if (depth >= kMaxCallDepth) {
        note("vp call nesting too deep");
        return;
    }
    body_ += condition.empty() ? "  {\n" : std::format("  if ({}) {{\n", condition);
    for (std::uint32_t pc = call.target(); pc < ucode_.size() / 4; pc++) {
        if (flow_.reached[pc] == 0) {
            note("vp call target not reached");
            break;
        }
        const Instruction in = instructionAt(pc);
        translateData(in);
        bool returned = false;
        switch (in.scaOpcode()) {
            case kRsxVpScaBri:
            case kRsxVpScaBrb: note("vp branch inside a subroutine"); break;
            case kRsxVpScaCal:
            case kRsxVpScaCli:
            case kRsxVpScaClb: emitCall(in, depth + 1); break;
            case kRsxVpScaRet: returned = branchCondition(in) != Condition::Never; break;
            default: break;
        }
        if (returned) break;
        if (in.end()) {
            // The end bit ends the whole program, also inside a subroutine.
            body_ += "  break;\n";
            break;
        }
    }
    body_ += "  }\n";
}

void Translator::emitStraightLine() {
    for (std::uint32_t pc = 0; pc < ucode_.size() / 4; pc++) {
        if (flow_.reached[pc] == 0) continue;
        const Instruction in = instructionAt(pc);
        translateData(in);
        if (in.end()) break;
    }
}

// Branches select among the blocks of one `while (true)` loop: block k runs when jump_position <= k,
// so leaving block k enters block k + 1, and a jump stores the level of its target block and restarts
// the loop. This is the structure RPCS3's VertexProgramDecompiler uses, and it also expresses backward
// branches. The end bit leaves the loop, not main(), so the epilogue still runs.
void Translator::emitControlFlow() {
    const std::uint32_t count = static_cast<std::uint32_t>(ucode_.size() / 4);
    for (std::uint32_t pc = 0; pc < count; pc++) {
        if (flow_.reached[pc] == 0) continue;
        const Instruction in = instructionAt(pc);
        const std::uint32_t op = in.scaOpcode();
        const bool jumps = op == kRsxVpScaBrb || (op == kRsxVpScaBri && in.condition() != kCondNever);
        if (jumps && in.target() > 0 && in.target() < count) labels_.push_back(in.target());
    }
    std::sort(labels_.begin(), labels_.end());
    labels_.erase(std::unique(labels_.begin(), labels_.end()), labels_.end());

    body_ += "  int jump_position = 0;\n  while (true) {\n  if (jump_position <= 0) {\n";
    // After an end bit, a return or an unconditional jump, only a label can start reachable code.
    bool dead = false;
    for (std::uint32_t pc = 0; pc < count; pc++) {
        if (flow_.reached[pc] == 0) continue;
        if (pc > 0 && std::binary_search(labels_.begin(), labels_.end(), pc)) {
            body_ += std::format("  }}\n  if (jump_position <= {}) {{\n", blockLevel(pc));
            dead = false;
        }
        if (dead) continue;

        const Instruction in = instructionAt(pc);
        translateData(in);
        switch (in.scaOpcode()) {
            case kRsxVpScaBrb: emitJump(in); break;
            case kRsxVpScaBri:
                emitJump(in);
                dead = in.condition() == kCondAlways;
                break;
            case kRsxVpScaCal:
            case kRsxVpScaCli:
            case kRsxVpScaClb: emitCall(in, 0); break;
            case kRsxVpScaRet:
                // A return outside a subroutine ends the program.
                if (branchCondition(in) == Condition::Always) {
                    body_ += "  break;\n";
                    dead = true;
                }
                break;
            default: break;
        }
        if (in.end()) {
            body_ += "  break;\n";
            dead = true;
        }
    }
    body_ += "  }\n  break;\n  }\n";
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
            "  uint branch_bits;\n"
            "  uint user_clip;\n"
            "} draw;\n"
            "\n"
            "layout(push_constant) uniform VpPush {\n"
            "  vec2 jitter_ndc;  // sub-pixel offset of a DLAA scene pass in clip space, 0 for every other draw\n"
            "} vp_push;\n"
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
    glsl += "\ninvariant gl_Position;\n";
    // Without a written slot 5 or 6 no plane has a source, so the array is not declared at all.
    if (clipPlanesEmitted()) glsl += "out float gl_ClipDistance[6];\n";
    glsl += "\n";

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
    // Both condition registers start at zero when a program begins.
    for (std::uint32_t reg = 0; reg < 2; reg++) {
        if ((cc_used_ & (1u << reg)) != 0) glsl += std::format("  vec4 cc_{} = vec4(0.0);\n", reg);
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
    // viewport_scale.w carries the renderer's image pixels per clip pixel (the raw RSX lane is unused), so a point
    // keeps its size relative to the picture at any resolution scale.
    glsl += std::format(
            "  gl_PointSize = {} * draw.viewport_scale.w;\n",
            (slots_written_ & (1u << 6)) != 0
                    ? std::format("((draw.output_mask & {}u) != 0u ? out_6.x : draw.point_size)", kMaskPointSize)
                    : std::string("draw.point_size"));

    if (clipPlanesEmitted()) {
        // Planes 0..2 read o[5].y/.z/.w and planes 3..5 read o[6].y/.z/.w. The slot registers start at
        // (0, 0, 0, 1), so a lane the program did not write reads 0, or 1 for .w.
        static constexpr char kLanes[3] = {'y', 'z', 'w'};
        glsl += "\n";
        for (std::uint32_t plane = 0; plane < 6; plane++) {
            const std::uint32_t slot = plane < 3 ? 5 : 6;
            if ((slots_written_ & (1u << slot)) == 0) {
                glsl += std::format("  gl_ClipDistance[{}] = 0.5;\n", plane);
                continue;
            }
            const std::string value = std::format("out_{}.{}", slot, kLanes[plane % 3]);
            glsl += std::format(
                    "  uint clip_op_{0} = (draw.user_clip >> {1}u) & 15u;\n"
                    "  gl_ClipDistance[{0}] = ((clip_op_{0} == 1u || clip_op_{0} == 2u) && "
                    "(draw.output_mask & {2}u) != 0u) ? (clip_op_{0} == 2u ? {3} : -{3}) : 0.5;\n",
                    plane, 4 * plane, 1u << (6 + plane), value);
        }
    }

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
            // Scaled by w so the shift is a constant offset in screen space after the perspective divide.
            "  pos.xy += vp_push.jitter_ndc * pos.w;\n"
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

GlslResult Translator::run(std::span<const std::uint32_t> ucode, const VpTranslateOptions& options) {
    GlslResult result;
    clip_distances_ = options.clip_distances;
    if (ucode.size() < 4 || ucode.size() % 4 != 0) {
        result.unimplemented.push_back("vp malformed ucode size");
        return result;
    }
    ucode_ = ucode;
    flow_ = analyzeVertexProgramFlow(ucode.data(), static_cast<std::uint32_t>(ucode.size() / 4), 0);
    if (flow_.bad_target) note("vp branch target out of range");
    if (flow_.has_flow_control) {
        emitControlFlow();
    } else {
        emitStraightLine();
    }

    // Without a position write the whole program is disabled.
    if ((slots_written_ & 1u) == 0) {
        body_.clear();
        slots_written_ = 0;
        temps_used_ = 0;
        inputs_used_ = 0;
        cc_used_ = 0;
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

GlslResult translateVertexProgram(std::span<const std::uint32_t> ucode, std::uint32_t,
                                  const VpTranslateOptions& options) {
    return Translator().run(ucode, options);
}

}  // namespace tox2

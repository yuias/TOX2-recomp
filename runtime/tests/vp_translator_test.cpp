// Self-checking tests for the vertex program translator. No Vulkan device is needed: the shaders
// are compiled to SPIR-V only.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "render/shader_compiler.h"
#include "render/vp_translator.h"

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

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool hasReport(const tox2::GlslResult& result, const std::string& part) {
    return std::any_of(result.unimplemented.begin(), result.unimplemented.end(),
                       [&](const std::string& entry) { return contains(entry, part); });
}

// ---- Minimal vertex program assembler (one vector and one scalar op per instruction) ----

struct Src {
    std::uint32_t type = 1;  // 1 temp, 2 input, 3 constant, 0 invalid
    std::uint32_t temp = 0;
    const char* swizzle = "xyzw";
    bool negate = false;

    std::uint32_t encode() const {
        std::uint32_t lane[4];
        for (int i = 0; i < 4; i++) {
            lane[i] = static_cast<std::uint32_t>(std::string("xyzw").find(swizzle[i]));
        }
        return type | (temp << 2) | (lane[3] << 8) | (lane[2] << 10) | (lane[1] << 12) | (lane[0] << 14) |
               (negate ? 1u << 16 : 0u);
    }
};

Src temp(std::uint32_t index, const char* swizzle = "xyzw") { return {1, index, swizzle, false}; }
Src input(const char* swizzle = "xyzw") { return {2, 0, swizzle, false}; }
Src constant(const char* swizzle = "xyzw") { return {3, 0, swizzle, false}; }

struct Asm {
    std::uint32_t vec_op = 0, sca_op = 0;
    std::uint32_t vec_temp = 63, sca_temp = 63;
    std::uint32_t slot = 31;
    bool vec_to_output = true;
    std::uint32_t vec_mask = 0xf, sca_mask = 0;  // bit 0 = x ... bit 3 = w
    std::uint32_t input_index = 0, constant_index = 0;
    Src src0 = {2, 0, "xyzw", false}, src1 = {3, 0, "xyzw", false}, src2 = {3, 0, "xyzw", false};
    std::uint32_t cond = 7;
    bool cond_enable = false, saturate = false, cc_update = false, relative_const = false, end = false;
    std::uint32_t abs_bits = 0;
    // Flow-control fields (BRI, CAL, CLI, RET, BRB, CLB); they overlap the source 2 fields.
    std::uint32_t branch_target = 0, branch_bit = 0;
    bool branch_taken_when_set = false;

    std::array<std::uint32_t, 4> words() const {
        const std::uint32_t s0 = src0.encode(), s1 = src1.encode(), s2 = src2.encode();
        std::uint32_t w0 = (3u << 2) | (2u << 4) | (1u << 6) | (cond << 10) | (vec_temp << 15) | (abs_bits << 21) |
                           (vec_to_output ? 1u << 30 : 0u);
        if (cond_enable) w0 |= 1u << 13;
        if (cc_update) w0 |= 1u << 14;
        if (saturate) w0 |= 1u << 26;
        const std::uint32_t w1 =
                (s0 >> 9) | (input_index << 8) | (constant_index << 12) | (vec_op << 22) | (sca_op << 27);
        std::uint32_t w2 = (s2 >> 11) | (s1 << 6) | ((s0 & 0x1ff) << 23);
        std::uint32_t w3 =
                (end ? 1u : 0u) | (relative_const ? 2u : 0u) | (slot << 2) | (sca_temp << 7) | ((s2 & 0x7ff) << 21);
        // The write mask bits run w, z, y, x from low to high.
        const auto spread = [](std::uint32_t mask, unsigned base) {
            return (((mask >> 3) & 1u) << base) | (((mask >> 2) & 1u) << (base + 1)) |
                   (((mask >> 1) & 1u) << (base + 2)) | ((mask & 1u) << (base + 3));
        };
        w3 |= spread(vec_mask, 13) | spread(sca_mask, 17);
        if (sca_op == 0x09 || sca_op == 0x0a || sca_op == 0x0b || sca_op == 0x0c || sca_op == 0x11 || sca_op == 0x12) {
            w0 |= ((branch_target >> 9) & 1u) << 23;
            w2 = (w2 & ~0x3Fu) | ((branch_target >> 3) & 0x3Fu);
            w3 = (w3 & 0x007FFFFFu) | (branch_bit << 23) | (branch_taken_when_set ? 1u << 28 : 0u) |
                 ((branch_target & 7u) << 29);
        }
        return {w0, w1, w2, w3};
    }
};

std::vector<std::uint32_t> program(const std::vector<Asm>& instructions) {
    std::vector<std::uint32_t> words;
    for (const Asm& in : instructions) {
        const auto w = in.words();
        words.insert(words.end(), w.begin(), w.end());
    }
    return words;
}

tox2::GlslResult translate(const std::vector<Asm>& instructions) {
    const std::vector<std::uint32_t> words = program(instructions);
    return tox2::translateVertexProgram(words, 0);
}

// ---- Dumps ----

struct Dump {
    std::string name;
    std::vector<std::uint32_t> words;
};

std::vector<Dump> loadDumps() {
    std::vector<Dump> dumps;
#ifdef TOX2_RSX_DUMP_DIR
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(TOX2_RSX_DUMP_DIR, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("vp_") || entry.path().extension() != ".bin") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() % 16 != 0) continue;
        Dump dump{name, {}};
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            dump.words.push_back((static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i])) << 24) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 1])) << 16) |
                                 (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 2])) << 8) |
                                 static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[i + 3])));
        }
        dumps.push_back(std::move(dump));
    }
#endif
    std::sort(dumps.begin(), dumps.end(), [](const Dump& a, const Dump& b) { return a.name < b.name; });
    return dumps;
}

void printHistogram(const std::vector<Dump>& dumps) {
    std::array<unsigned, 32> vec{}, sca{};
    for (const Dump& dump : dumps) {
        for (std::size_t i = 0; i + 4 <= dump.words.size(); i += 4) {
            vec[(dump.words[i + 1] >> 22) & 0x1f]++;
            sca[(dump.words[i + 1] >> 27) & 0x1f]++;
        }
    }
    std::printf("vp opcode histogram over %zu dumps:\n", dumps.size());
    for (int op = 0; op < 32; op++) {
        if (vec[static_cast<std::size_t>(op)] != 0) {
            std::printf("  vector 0x%02x: %u\n", op, vec[static_cast<std::size_t>(op)]);
        }
    }
    for (int op = 0; op < 32; op++) {
        if (sca[static_cast<std::size_t>(op)] != 0) {
            std::printf("  scalar 0x%02x: %u\n", op, sca[static_cast<std::size_t>(op)]);
        }
    }
}

// Decoded body of each dumped program (all scalar ops are NOP).
const char* const kFrontColor = "  out_1 = attr_3;\n";
const char* const kRow1 = "  r_0 = attr_0.yyyy * vp_consts.c[1];\n";
const char* const kTexCoord = "  out_7.xy = attr_8.xyxx.xy;\n";
const char* const kRows023 =
        "  r_0 = fma(attr_0.xxxx, vp_consts.c[0], r_0);\n"
        "  r_0 = fma(attr_0.zzzz, vp_consts.c[2], r_0);\n"
        "  out_0 = fma(attr_0.wwww, vp_consts.c[3], r_0);\n";

std::string expectedBody(const std::string& name) {
    if (name.starts_with("vp_30e29a531ca34b4e")) {
        return std::string(kFrontColor) + kRow1 + kTexCoord + kRows023;
    }
    if (name.starts_with("vp_6433cfb568fd74d1")) return std::string(kFrontColor) + kRow1 + kRows023;
    if (name.starts_with("vp_a00fab88abddf079")) return std::string(kRow1) + kTexCoord + kRows023;
    return {};
}

// Attributes read by each dumped program: v[0], v[3] (front colour) and v[8] (TEX0) as applicable.
std::uint32_t expectedInputs(const std::string& name) {
    if (name.starts_with("vp_30e29a531ca34b4e")) return 0x109;
    if (name.starts_with("vp_6433cfb568fd74d1")) return 0x9;  // no texture coordinate
    if (name.starts_with("vp_a00fab88abddf079")) return 0x101;
    return 0;
}

// Prints the reports of the failed translations, most frequent first.
void printTally(const std::map<std::string, int>& tally) {
    std::vector<std::pair<std::string, int>> sorted(tally.begin(), tally.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [text, count] : sorted) std::printf("  %6d  %s\n", count, text.c_str());
}

// The dumps hold every program the game ran, so most have no expected body. Those are translated and
// must compile when the translation succeeds; a translation the translator reports is tallied, not
// failed, because the census of unimplemented features is the translator's own output.
void testDumps() {
    const std::vector<Dump> dumps = loadDumps();
    if (dumps.empty()) {
        std::printf("vp translator: no dumps in generated/rsx, dump tests skipped\n");
        return;
    }
    printHistogram(dumps);

    bool printed = false;
    int translated = 0;
    int reported = 0;
    std::map<std::string, int> tally;
    for (const Dump& dump : dumps) {
        const tox2::GlslResult result = tox2::translateVertexProgram(dump.words, 0);
        const std::string body = expectedBody(dump.name);
        if (!body.empty()) {
            CHECK(result.ok);
            CHECK(result.unimplemented.empty());
            CHECK(contains(result.glsl, body));
            CHECK(result.inputs_used == expectedInputs(dump.name));
        }
        if (!result.ok) {
            reported++;
            for (const std::string& entry : result.unimplemented) tally[entry]++;
            continue;
        }
        translated++;

        auto spirv = tox2::compileGlsl(tox2::ShaderStage::Vertex, result.glsl, 0xd0d0);
        CHECK(spirv.has_value());
        if (!spirv) {
            std::fprintf(stderr, "%s: %s\n", dump.name.c_str(), spirv.error().c_str());
            std::fprintf(stderr, "%s\n", result.glsl.c_str());
        }
        if (!printed && dump.name.starts_with("vp_30e29a531ca34b4e")) {
            std::printf("translated %s:\n%s\n", dump.name.c_str(), result.glsl.c_str());
            printed = true;
        }
    }
    std::printf("vp translator: %zu dumps, %d translated, %d reported\n", dumps.size(), translated, reported);
    printTally(tally);
}

// ---- Hand-assembled programs ----

void testAssembler() {
    // The assembler reproduces a decoded instruction word for word: MAD o[0], v[0].wwww, c[3], R0.
    Asm mad;
    mad.vec_op = 4;
    mad.slot = 0;
    mad.src0 = input("wwww");
    mad.constant_index = 3;
    mad.src1 = constant();
    mad.src2 = temp(0);
    mad.end = true;
    const auto w = mad.words();
    CHECK(w[0] == 0x401f9c6c && w[1] == 0x0100307f && w[2] == 0x8106c0c3 && w[3] == 0x6021ff81);
}

void testMov() {
    Asm mov;
    mov.vec_op = 1;
    mov.slot = 0;
    mov.end = true;
    const tox2::GlslResult result = translate({mov});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  out_0 = attr_0;\n"));
    CHECK(result.inputs_used == 1u);
    auto spirv = tox2::compileGlsl(tox2::ShaderStage::Vertex, result.glsl, 0xd0d1);
    CHECK(spirv.has_value());
    if (!spirv) std::fprintf(stderr, "%s\n", spirv.error().c_str());
}

void testOperations() {
    // ADD reads src0 and src2.
    Asm add;
    add.vec_op = 3;
    add.vec_temp = 1;
    add.src0 = input("xyzw");
    add.src1 = constant();
    add.src2 = temp(2, "wzyx");
    add.src2.negate = true;
    add.abs_bits = 4;
    Asm out;
    out.vec_op = 1;
    out.slot = 0;
    out.src0 = temp(1);
    out.end = true;
    tox2::GlslResult result = translate({add, out});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_1 = attr_0 + -abs(r_2.wzyx);\n"));

    // Masked scalar write routed to the output slot, saturate, dual destinations.
    Asm sca;
    sca.sca_op = 2;  // RCP
    sca.vec_to_output = false;
    sca.slot = 3;
    sca.sca_mask = 0x3;
    sca.src2 = temp(0, "yyyy");
    sca.saturate = true;
    Asm pos;
    pos.vec_op = 5;  // DP3 into temp and the position
    pos.vec_temp = 0;
    pos.slot = 0;
    pos.src0 = input();
    pos.src1 = constant("zzzw");
    pos.end = true;
    result = translate({sca, pos});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  out_3.xy = clamp(vec4(1.0 / r_0.yyyy.x), 0.0, 1.0).xy;\n"));
    CHECK(contains(result.glsl, "    vec4 res = vec4(dot(attr_0.xyz, vp_consts.c[0].zzzw.xyz));\n"));
    auto spirv = tox2::compileGlsl(tox2::ShaderStage::Vertex, result.glsl, 0xd0d2);
    CHECK(spirv.has_value());
    if (!spirv) std::fprintf(stderr, "%s\n%s\n", spirv.error().c_str(), result.glsl.c_str());
}

void testAllOutputsCompile() {
    // Every vector and scalar op the translator implements, writing every exported slot.
    std::vector<Asm> instructions;
    const std::uint32_t slots[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (const std::uint32_t slot : slots) {
        Asm in;
        in.vec_op = 1;
        in.slot = slot;
        instructions.push_back(in);
    }
    const std::uint32_t vec_ops[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    for (const std::uint32_t op : vec_ops) {
        Asm in;
        in.vec_op = op;
        in.vec_temp = 0;
        in.src0 = temp(0);
        in.src2 = temp(1);
        instructions.push_back(in);
    }
    const std::uint32_t sca_ops[] = {1, 2, 3, 4, 5, 6, 7, 13, 14, 15, 16};
    for (const std::uint32_t op : sca_ops) {
        Asm in;
        in.sca_op = op;
        in.sca_temp = 1;
        in.sca_mask = 0xf;
        in.vec_mask = 0;
        in.src2 = input("xyzw");
        instructions.push_back(in);
    }
    instructions.back().end = true;
    const tox2::GlslResult result = translate(instructions);
    CHECK(result.ok);
    if (!result.ok) {
        for (const std::string& entry : result.unimplemented) std::fprintf(stderr, "%s\n", entry.c_str());
        return;
    }
    CHECK(contains(result.glsl, "(draw.output_mask & 16384u) != 0u ? out_7 : vec4(0.0, 0.0, 0.0, 1.0)"));
    CHECK(contains(result.glsl, "var_fog = (draw.output_mask & 16u) != 0u ? vec4(out_5.x) :"));
    // Front and back colours are exported when either of their mask bits is set.
    CHECK(contains(result.glsl, "var_back_diffuse = (draw.output_mask & 5u) != 0u ? out_3 :"));
    CHECK(contains(result.glsl, "var_front_specular = (draw.output_mask & 10u) != 0u ? out_2 :"));
    auto spirv = tox2::compileGlsl(tox2::ShaderStage::Vertex, result.glsl, 0xd0d3);
    CHECK(spirv.has_value());
    if (!spirv) std::fprintf(stderr, "%s\n%s\n", spirv.error().c_str(), result.glsl.c_str());
}

void testUnwrittenOutputsAreDefault() {
    Asm pos;
    pos.vec_op = 1;
    pos.slot = 0;
    pos.end = true;
    const tox2::GlslResult result = translate({pos});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  var_front_diffuse = vec4(0.0, 0.0, 0.0, 1.0);\n"));
    CHECK(contains(result.glsl, "  var_tex0 = vec4(0.0, 0.0, 0.0, 1.0);\n"));
    CHECK(contains(result.glsl, "  gl_PointSize = draw.point_size;\n"));

    // No position write: the body is disabled and the position is (0, 0, 0, 1).
    Asm color;
    color.vec_op = 1;
    color.slot = 1;
    color.end = true;
    const tox2::GlslResult disabled = translate({color});
    CHECK(disabled.ok);
    CHECK(contains(disabled.glsl, "  var_front_diffuse = vec4(0.0, 0.0, 0.0, 1.0);\n"));
    CHECK(contains(disabled.glsl, "vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);"));
    CHECK(disabled.inputs_used == 0u);
}

void testUnimplementedReports() {
    Asm unknown;
    unknown.vec_op = 0x1f;
    unknown.slot = 0;
    unknown.end = true;
    tox2::GlslResult result = translate({unknown});
    CHECK(!result.ok && result.glsl.empty());
    CHECK(hasReport(result, "vp opcode 0x1f"));

    Asm arl;
    arl.vec_op = 0x0d;
    arl.end = true;
    result = translate({arl});
    CHECK(!result.ok && hasReport(result, "ARL"));

    Asm relative;
    relative.vec_op = 2;
    relative.slot = 0;
    relative.relative_const = true;
    relative.end = true;
    result = translate({relative});
    CHECK(!result.ok && hasReport(result, "relative"));

    Asm conditional;
    conditional.vec_op = 1;
    conditional.slot = 0;
    conditional.cond_enable = true;
    conditional.cond = 2;
    conditional.end = true;
    result = translate({conditional});
    CHECK(!result.ok && hasReport(result, "condition"));

    Asm branch;
    branch.sca_op = 0x08;  // BRA
    branch.vec_op = 1;
    branch.slot = 0;
    branch.end = true;
    result = translate({branch});
    CHECK(!result.ok && hasReport(result, "BRA"));

    Asm invalid_source;
    invalid_source.vec_op = 1;
    invalid_source.slot = 0;
    invalid_source.src0.type = 0;
    invalid_source.end = true;
    result = translate({invalid_source});
    CHECK(!result.ok && hasReport(result, "source type 0"));

    // An unused source field of type 0 is harmless.
    Asm unused_invalid;
    unused_invalid.vec_op = 1;
    unused_invalid.slot = 0;
    unused_invalid.src1.type = 0;
    unused_invalid.src2.type = 0;
    unused_invalid.end = true;
    result = translate({unused_invalid});
    CHECK(result.ok);

    // Writing nothing but a condition code.
    Asm cc_only;
    cc_only.vec_op = 1;
    cc_only.vec_mask = 0xf;
    cc_only.end = true;
    result = translate({cc_only});
    CHECK(!result.ok && hasReport(result, "condition code"));

    // Condition 0 skips the instruction, even with the enable bit clear.
    Asm never;
    never.vec_op = 1;
    never.slot = 0;
    never.cond = 0;
    never.end = true;
    result = translate({never});
    CHECK(result.ok && !contains(result.glsl, "out_0 = attr_0"));

    result = tox2::translateVertexProgram({}, 0);
    CHECK(!result.ok);
}

void testScalarDualDestination() {
    // A scalar op routed to the output slot also writes its own temp, with the same masked value.
    Asm scalar_both;
    scalar_both.sca_op = 1;
    scalar_both.vec_to_output = false;
    scalar_both.slot = 0;
    scalar_both.sca_temp = 2;
    scalar_both.vec_mask = 0;
    scalar_both.sca_mask = 0x5;  // x and z
    scalar_both.src2 = input();
    scalar_both.end = true;
    const tox2::GlslResult result = translate({scalar_both});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "r_2.xz = res.xz;"));
    CHECK(contains(result.glsl, "out_0.xz = res.xz;"));
}

// ---- Flow control ----

std::size_t position(const std::string& text, const std::string& part) { return text.find(part); }

Asm movTo(std::uint32_t slot, bool end = false) {
    Asm in;
    in.vec_op = 1;
    in.slot = slot;
    in.end = end;
    return in;
}

Asm flowOp(std::uint32_t sca_op, std::uint32_t target, std::uint32_t bit = 0, bool taken_when_set = false) {
    Asm in;
    in.sca_op = sca_op;
    in.branch_target = target;
    in.branch_bit = bit;
    in.branch_taken_when_set = taken_when_set;
    in.vec_mask = 0;
    return in;
}

void checkCompiles(const tox2::GlslResult& result, std::uint64_t key) {
    auto spirv = tox2::compileGlsl(tox2::ShaderStage::Vertex, result.glsl, key);
    CHECK(spirv.has_value());
    if (!spirv) std::fprintf(stderr, "%s\n%s\n", spirv.error().c_str(), result.glsl.c_str());
}

void printReports(const tox2::GlslResult& result) {
    for (const std::string& entry : result.unimplemented) std::fprintf(stderr, "unimplemented: %s\n", entry.c_str());
}

void testBranchOnBooleanConstant() {
    // o[0] = v[0]; BRB b5 (taken when clear) over the o[1] write; o[2] write; o[3] write and end.
    const tox2::GlslResult clear_taken =
            translate({movTo(0), flowOp(0x11, 3, 5, false), movTo(1), movTo(2), movTo(3, true)});
    CHECK(clear_taken.ok);
    if (!clear_taken.ok) printReports(clear_taken);
    CHECK(contains(clear_taken.glsl, "if ((draw.branch_bits & (1u << 5)) == 0u) {\n    jump_position = 1;\n"));
    // The skipped write sits in block 0 and the target is the first instruction of block 1.
    const std::size_t skipped = position(clear_taken.glsl, "out_1 = attr_0;");
    const std::size_t block1 = position(clear_taken.glsl, "if (jump_position <= 1) {");
    const std::size_t target = position(clear_taken.glsl, "out_3 = attr_0;");
    CHECK(skipped != std::string::npos && block1 != std::string::npos && target != std::string::npos);
    CHECK(skipped < block1 && block1 < target);
    checkCompiles(clear_taken, 0xd0e0);

    // The sense bit selects the polarity; the index selects the bit.
    const tox2::GlslResult set_taken = translate({movTo(0), flowOp(0x11, 2, 31, true), movTo(1), movTo(2, true)});
    CHECK(set_taken.ok);
    CHECK(contains(set_taken.glsl, "if ((draw.branch_bits & (1u << 31)) != 0u) {"));
    CHECK(contains(set_taken.glsl, "uint branch_bits;"));
    checkCompiles(set_taken, 0xd0e1);

    // The vector part of the branch instruction runs before the branch.
    Asm dual = flowOp(0x11, 2, 0, false);
    dual.vec_op = 1;
    dual.vec_mask = 0xf;
    dual.slot = 1;
    const tox2::GlslResult with_vector = translate({movTo(0), dual, movTo(2, true)});
    CHECK(with_vector.ok);
    CHECK(position(with_vector.glsl, "out_1 = attr_0;") < position(with_vector.glsl, "(draw.branch_bits"));

    // A program without flow control keeps the straight-line body.
    const tox2::GlslResult plain = translate({movTo(0, true)});
    CHECK(plain.ok && !contains(plain.glsl, "jump_position") && !contains(plain.glsl, "branch_bits &"));
}

void testProgramPastTheFirstEnd() {
    // The first end bit is skipped by the branch; the instructions after it are reached through the label.
    const tox2::GlslResult result =
            translate({movTo(0), flowOp(0x11, 3, 1, true), movTo(1, true), movTo(2), movTo(3, true)});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "out_2 = attr_0;") && contains(result.glsl, "out_3 = attr_0;"));
    // The end bit leaves the loop, not main(), so the epilogue still runs.
    CHECK(contains(result.glsl, "  break;\n  }\n  if (jump_position <= 1) {"));
    CHECK(!contains(result.glsl, "return;"));
    checkCompiles(result, 0xd0e2);

    // Instructions no path reaches are not translated, whatever they hold.
    Asm garbage;
    garbage.vec_op = 0x1f;
    const tox2::GlslResult unreached = translate({movTo(0, true), garbage});
    CHECK(unreached.ok);
}

void testBackwardBranch() {
    // 0: o[0] = v[0]   1: o[1] = v[0]   2: BRB b2 back to 1   3: end. The target is above 0, so it gets a block.
    const tox2::GlslResult loop = translate({movTo(0), movTo(1), flowOp(0x11, 1, 2, true), movTo(3, true)});
    CHECK(loop.ok);
    CHECK(contains(loop.glsl, "if ((draw.branch_bits & (1u << 2)) != 0u) {\n    jump_position = 1;\n    continue;\n"));
    CHECK(position(loop.glsl, "if (jump_position <= 1) {") < position(loop.glsl, "out_1 = attr_0;"));
    checkCompiles(loop, 0xd0e3);

    // A jump to the first instruction restarts block 0.
    const tox2::GlslResult restart = translate({movTo(0), flowOp(0x11, 0, 7, true), movTo(3, true)});
    CHECK(restart.ok);
    CHECK(contains(restart.glsl, "jump_position = 0;\n    continue;"));
    checkCompiles(restart, 0xd0e4);

    // BRI with condition "always" is an unconditional jump; what follows it is dead until the next label.
    const tox2::GlslResult unconditional =
            translate({movTo(0), flowOp(0x09, 3), movTo(1), movTo(2, true), movTo(3, true)});
    CHECK(unconditional.ok);
    if (!unconditional.ok) printReports(unconditional);
    CHECK(contains(unconditional.glsl, "  jump_position = 1;\n  continue;\n"));
    CHECK(!contains(unconditional.glsl, "out_1 = attr_0;"));  // skipped by the jump
    CHECK(contains(unconditional.glsl, "out_2 = attr_0;"));   // the jump target
    CHECK(!contains(unconditional.glsl, "out_3 = attr_0;"));  // past the end bit, reached by nothing
    checkCompiles(unconditional, 0xd0e5);
}

void testSubroutines() {
    // 0: o[0] = v[0]   1: CAL 4   2: o[1] = v[0]   3: end   4: o[2] = v[0]   5: RET
    const tox2::GlslResult call =
            translate({movTo(0), flowOp(0x0a, 4), movTo(1), movTo(3, true), movTo(2), flowOp(0x0c, 0)});
    CHECK(call.ok);
    if (!call.ok) printReports(call);
    // The callee is inlined at the call, once, and the code after the call still runs.
    CHECK(contains(call.glsl, "  {\n  out_2 = attr_0;\n  }\n"));
    CHECK(position(call.glsl, "out_2 = attr_0;") < position(call.glsl, "out_1 = attr_0;"));
    CHECK(call.glsl.find("out_2 = attr_0;") == call.glsl.rfind("out_2 = attr_0;"));
    checkCompiles(call, 0xd0e6);

    // CLB calls when its bit is set (or clear, by the sense bit).
    const tox2::GlslResult conditional_call =
            translate({movTo(0), flowOp(0x12, 3, 6, true), movTo(3, true), movTo(2), flowOp(0x0c, 0)});
    CHECK(conditional_call.ok);
    if (!conditional_call.ok) printReports(conditional_call);
    CHECK(contains(conditional_call.glsl, "if ((draw.branch_bits & (1u << 6)) != 0u) {\n  out_2 = attr_0;"));
    checkCompiles(conditional_call, 0xd0e7);

    // A call inside a call: 1: CAL 4, 4: o[2] write and CAL 7, 7: o[1] write, RET, RET.
    const tox2::GlslResult nested = translate({movTo(0), flowOp(0x0a, 4), movTo(3, true), movTo(3), movTo(2),
                                               flowOp(0x0a, 7), flowOp(0x0c, 0), movTo(1), flowOp(0x0c, 0)});
    CHECK(nested.ok);
    if (!nested.ok) printReports(nested);
    CHECK(position(nested.glsl, "out_2 = attr_0;") < position(nested.glsl, "out_1 = attr_0;"));
    checkCompiles(nested, 0xd0e8);
}

void testFlowControlReports() {
    // A branch inside a subroutine would need its own blocks.
    tox2::GlslResult result =
            translate({movTo(0), flowOp(0x0a, 3), movTo(3, true), flowOp(0x11, 5, 0, true), movTo(1), flowOp(0x0c, 0)});
    CHECK(!result.ok && hasReport(result, "branch inside a subroutine"));

    // A condition-code branch needs the condition registers.
    Asm conditional = flowOp(0x09, 2);
    conditional.cond = 2;
    result = translate({movTo(0), conditional, movTo(3, true)});
    CHECK(!result.ok && hasReport(result, "condition other than always"));

    // A target outside the program.
    result = translate({movTo(0), flowOp(0x11, 40, 0, true), movTo(3, true)});
    CHECK(!result.ok && hasReport(result, "branch target out of range"));

    // BRA stays unimplemented, whatever surrounds it.
    Asm bra = movTo(0, true);
    bra.sca_op = 0x08;
    result = translate({bra});
    CHECK(!result.ok && hasReport(result, "BRA"));

    // A call that calls itself is bounded by the call depth.
    result = translate({movTo(0), flowOp(0x0a, 3), movTo(3, true), flowOp(0x0a, 3)});
    CHECK(!result.ok && hasReport(result, "call nesting"));
}

void testLocations() {
    CHECK(tox2::vpOutputSlotLocation(0) == -1);
    CHECK(tox2::vpOutputSlotLocation(1) == 10);
    CHECK(tox2::vpOutputSlotLocation(2) == 12);
    CHECK(tox2::vpOutputSlotLocation(3) == 11);
    CHECK(tox2::vpOutputSlotLocation(4) == 13);
    CHECK(tox2::vpOutputSlotLocation(5) == 14);
    CHECK(tox2::vpOutputSlotLocation(6) == -1);
    CHECK(tox2::vpOutputSlotLocation(7) == 0);
    CHECK(tox2::vpOutputSlotLocation(14) == 7);
    CHECK(tox2::vpOutputSlotLocation(15) == 8);
    CHECK(tox2::vpOutputSlotLocation(16) == -1);
}

// ---- User clip planes ----

tox2::GlslResult translateClip(const std::vector<Asm>& instructions, bool clip_distances) {
    const std::vector<std::uint32_t> words = program(instructions);
    return tox2::translateVertexProgram(words, 0, tox2::VpTranslateOptions{clip_distances});
}

void testUserClipPlanes() {
    std::vector<Asm> both = {movTo(0), movTo(5), movTo(6, true)};

    // Without the option the program has no clip distance and the block still names the control word.
    tox2::GlslResult result = translateClip(both, false);
    CHECK(result.ok);
    CHECK(!contains(result.glsl, "gl_ClipDistance"));
    CHECK(contains(result.glsl, "  uint user_clip;\n"));
    checkCompiles(result, 0xd0e0);

    // Both clip slots written: six planes from o[5].y/.z/.w and o[6].y/.z/.w.
    result = translateClip(both, true);
    CHECK(result.ok);
    CHECK(contains(result.glsl, "out float gl_ClipDistance[6];\n"));
    const char* const sources[6] = {"out_5.y", "out_5.z", "out_5.w", "out_6.y", "out_6.z", "out_6.w"};
    for (unsigned plane = 0; plane < 6; plane++) {
        const std::string n = std::to_string(plane);
        CHECK(contains(result.glsl,
                       "uint clip_op_" + n + " = (draw.user_clip >> " + std::to_string(4 * plane) + "u) & 15u;\n"));
        CHECK(contains(result.glsl, "(draw.output_mask & " + std::to_string(64u << plane) + "u) != 0u"));
        CHECK(contains(result.glsl,
                       "(clip_op_" + n + " == 2u ? " + sources[plane] + " : -" + sources[plane] + ") : 0.5;\n"));
    }
    checkCompiles(result, 0xd0e1);

    // Neither slot written: nothing to clip against, so no declaration and no write.
    result = translateClip({movTo(0, true)}, true);
    CHECK(result.ok);
    CHECK(!contains(result.glsl, "gl_ClipDistance"));
    checkCompiles(result, 0xd0e2);

    // Only slot 5: planes 3..5 have no source and stay at the constant 0.5.
    result = translateClip({movTo(0), movTo(5, true)}, true);
    CHECK(result.ok);
    CHECK(contains(result.glsl, "out float gl_ClipDistance[6];\n"));
    CHECK(contains(result.glsl, "? out_5.y : -out_5.y)"));
    CHECK(contains(result.glsl, "? out_5.w : -out_5.w)"));
    CHECK(!contains(result.glsl, "out_6"));
    for (const char* const plane : {"3", "4", "5"}) {
        CHECK(contains(result.glsl, std::string("  gl_ClipDistance[") + plane + "] = 0.5;\n"));
        CHECK(!contains(result.glsl, std::string("clip_op_") + plane));
    }
    CHECK(!contains(result.glsl, "  gl_ClipDistance[0] = 0.5;\n"));
    checkCompiles(result, 0xd0e3);

    // Only slot 6: planes 0..2 are the constant.
    result = translateClip({movTo(0), movTo(6, true)}, true);
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  gl_ClipDistance[0] = 0.5;\n"));
    CHECK(contains(result.glsl, "  gl_ClipDistance[2] = 0.5;\n"));
    CHECK(contains(result.glsl, "? out_6.z : -out_6.z)"));
    CHECK(!contains(result.glsl, "out_5"));
    checkCompiles(result, 0xd0e4);

    // A program without a position write is disabled whole, clip slots included.
    result = translateClip({movTo(5, true)}, true);
    CHECK(result.ok);
    CHECK(!contains(result.glsl, "gl_ClipDistance"));
}

}  // namespace

int main() {
    testAssembler();
    testMov();
    testOperations();
    testAllOutputsCompile();
    testUnwrittenOutputsAreDefault();
    testUnimplementedReports();
    testScalarDualDestination();
    testBranchOnBooleanConstant();
    testProgramPastTheFirstEnd();
    testBackwardBranch();
    testSubroutines();
    testFlowControlReports();
    testLocations();
    testUserClipPlanes();
    testDumps();
    std::printf("vp translator tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

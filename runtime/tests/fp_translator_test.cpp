// Self-checking tests for the fragment program translator. No Vulkan device is needed: the shaders
// are compiled to SPIR-V only.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "render/fp_translator.h"
#include "render/shader_compiler.h"

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

// ---- Minimal fragment program assembler (words after the halfword fixup) ----

struct Src {
    std::uint32_t type = 0;  // 0 temp, 1 input, 2 constant, 3 invalid
    std::uint32_t index = 0;
    bool half = false;
    const char* swizzle = "xyzw";
    bool negate = false, absolute = false;
    std::uint32_t precision = 0;

    std::uint32_t swizzleBits() const {
        std::uint32_t bits = 0;
        for (std::uint32_t i = 0; i < 4; i++) {
            bits |= static_cast<std::uint32_t>(std::string("xyzw").find(swizzle[i])) << (2 * i);
        }
        return bits;
    }
    std::uint32_t common() const {
        return type | (index << 2) | (half ? 1u << 8 : 0u) | (swizzleBits() << 9) | (negate ? 1u << 17 : 0u);
    }
};

Src temp(std::uint32_t index, bool half = false, const char* swizzle = "xyzw") {
    return {0, index, half, swizzle, false, false, 0};
}
Src inputSrc(const char* swizzle = "xyzw") { return {1, 0, false, swizzle, false, false, 0}; }
Src constant(const char* swizzle = "xyzw") { return {2, 0, false, swizzle, false, false, 0}; }

struct Asm {
    std::uint32_t op = 1;
    std::uint32_t dest = 0;
    bool dest_half = false;
    std::uint32_t mask = 0xf;  // bit 0 = x
    std::uint32_t input = 1;   // input register read by input-type sources
    std::uint32_t unit = 0;
    std::uint32_t dest_precision = 0, scale = 0, cond = 7;
    const char* cond_swizzle = "xyzw";
    std::uint32_t cc_read = 0, cc_write = 0;  // condition register indices (src0 bits 31 and 30)
    bool end = false, saturate = false, cc_update = false, no_dest = false, bx2 = false, indexed = false;
    Src src0 = inputSrc(), src1 = temp(0), src2 = temp(0);

    bool hasConstant() const { return src0.type == 2 || src1.type == 2 || src2.type == 2; }

    std::uint32_t condSwizzleBits() const {
        std::uint32_t bits = 0;
        for (std::uint32_t i = 0; i < 4; i++) {
            bits |= static_cast<std::uint32_t>(std::string("xyzw").find(cond_swizzle[i])) << (2 * i);
        }
        return bits;
    }

    // Words as the hardware reads them; stored() swaps the halves like RsxDraw::fp_ucode.
    std::array<std::uint32_t, 4> words() const {
        const std::uint32_t w0 = (end ? 1u : 0u) | (dest << 1) | (dest_half ? 1u << 7 : 0u) |
                                 (cc_update ? 1u << 8 : 0u) | (mask << 9) | (input << 13) | (unit << 17) |
                                 (bx2 ? 1u << 21 : 0u) | (dest_precision << 22) | ((op & 0x3f) << 24) |
                                 (no_dest ? 1u << 30 : 0u) | (saturate ? 1u << 31 : 0u);
        const std::uint32_t w1 = src0.common() | (cond << 18) | (condSwizzleBits() << 21) |
                                 (src0.absolute ? 1u << 29 : 0u) | (cc_write << 30) | (cc_read << 31);
        const std::uint32_t w2 = src1.common() | (src1.absolute ? 1u << 18 : 0u) | (src0.precision << 19) |
                                 (src1.precision << 22) | (src2.precision << 25) | (scale << 28) | ((op >> 6) << 31);
        const std::uint32_t w3 = src2.common() | (src2.absolute ? 1u << 18 : 0u) | (indexed ? 1u << 30 : 0u);
        return {w0, w1, w2, w3};
    }
};

std::vector<std::uint32_t> program(const std::vector<Asm>& instructions) {
    std::vector<std::uint32_t> words;
    for (const Asm& in : instructions) {
        for (const std::uint32_t w : in.words()) words.push_back(tox2::fpFixupWord(w));
        if (in.hasConstant()) words.insert(words.end(), 4, 0);  // the constants live elsewhere
    }
    return words;
}

tox2::GlslResult translate(const std::vector<Asm>& instructions, std::uint32_t control = 0x40,
                           std::uint32_t texcoord_control = 0) {
    const std::vector<std::uint32_t> words = program(instructions);
    return tox2::translateFragmentProgram(words, control, texcoord_control);
}

bool compiles(const tox2::GlslResult& result, std::uint64_t key) {
    auto spirv = tox2::compileGlsl(tox2::ShaderStage::Fragment, result.glsl, key);
    if (!spirv) std::fprintf(stderr, "%s\n%s\n", spirv.error().c_str(), result.glsl.c_str());
    return spirv.has_value();
}

// ---- Dumps ----

struct Dump {
    std::string name;
    std::vector<std::uint32_t> words;  // as stored, like RsxDraw::fp_ucode
};

std::vector<Dump> loadDumps() {
    std::vector<Dump> dumps;
#ifdef TOX2_RSX_DUMP_DIR
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(TOX2_RSX_DUMP_DIR, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("fp_") || entry.path().extension() != ".bin") continue;
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
    std::map<std::uint32_t, unsigned> histogram;
    for (const Dump& dump : dumps) {
        for (std::size_t i = 0; i + 4 <= dump.words.size(); i += 4) {
            const std::uint32_t w0 = tox2::fpFixupWord(dump.words[i]), w2 = tox2::fpFixupWord(dump.words[i + 2]);
            histogram[((w0 >> 24) & 0x3f) | ((w2 >> 31) << 6)]++;
        }
    }
    std::printf("fp opcode histogram over %zu dumps:\n", dumps.size());
    for (const auto& [op, count] : histogram) std::printf("  0x%02x: %u\n", op, count);
}

// Control register of the captures behind each dump: 0x40 (fp32 export) except for the H0 program.
std::uint32_t controlFor(const std::string& name) { return name.starts_with("fp_64c5f189a1dfd290") ? 0u : 0x40u; }

// TEXCOORD_CTRL0 of the captures: bit 0 set (2D coordinate) for the programs that sample TEX0.
std::uint32_t texcoordControlFor(const std::string& name) { return name.starts_with("fp_beb1f05618778cfe") ? 0u : 1u; }

// Expected body of each dumped program: fp_64c5 is TEX H0, TEX0; fp_beb1 is MOV R0, COL0; fp_fa92 is
// TEX H0, TEX0 followed by MUL R0, H0, COL0.
std::string expectedBody(const std::string& name) {
    const std::string tex =
            "  vec4 in_tex0 = vec4(var_tex0.xy, 0.0, gl_FragCoord.w);\n"
            "  h_0 = texture(tex0, in_tex0.xy * draw.texture_scale[0].xy);\n";
    const std::string end = "\n  if ((draw.flags & 2u)";
    if (name.starts_with("fp_64c5f189a1dfd290")) return tex + end;
    if (name.starts_with("fp_beb1f05618778cfe")) return "  r_0 = in_col0;\n" + end;
    if (name.starts_with("fp_fa922174d3478656")) return tex + "  r_0 = h_0 * in_col0;\n" + end;
    return {};
}

std::string expectedExport(const std::string& name) {
    return name.starts_with("fp_64c5f189a1dfd290") ? "  out_color0 = h_0;\n" : "  out_color0 = r_0;\n";
}

std::uint32_t expectedUnits(const std::string& name) { return name.starts_with("fp_beb1f05618778cfe") ? 0u : 1u; }

// The dialogue text program (356 slots, 218 instructions, 138 inline constants) from the dump
// directory; it is game-derived, so it is not tracked and the test is skipped without it. It
// exercises every comparison, FRC, FLR, both forms of condition register update and per-lane
// conditional execution. testDumps leaves it out because it has no expected body text.
void testTextProgram() {
    const std::vector<Dump> dumps = loadDumps();
    const auto found = std::find_if(dumps.begin(), dumps.end(),
                                    [](const Dump& dump) { return dump.name.starts_with("fp_c5a1c19123aa52bb"); });
    if (found == dumps.end()) {
        std::printf("fp translator: text program skipped: fp_c5a1c19123aa52bb.bin not present in generated/rsx\n");
        return;
    }
    CHECK(found->words.size() == 1424);

    const tox2::GlslResult result = tox2::translateFragmentProgram(found->words, 0x40, 1);
    for (const std::string& entry : result.unimplemented) {
        std::fprintf(stderr, "text program: unimplemented: %s\n", entry.c_str());
    }
    CHECK(result.ok);
    CHECK(result.unimplemented.empty());
    CHECK(result.texture_units == 1u);
    CHECK(contains(result.glsl, "vec4 fc[138];"));  // 138 * 16 bytes, far below the uniform block limit
    CHECK(contains(result.glsl, "vec4 cc0 = vec4(0.0);") && !contains(result.glsl, "cc1"));
    CHECK(contains(result.glsl, "mix(r_0, "));
    CHECK(contains(result.glsl, "notEqual(cc0.xxxx, vec4(0.0))"));
    CHECK(contains(result.glsl, "lessThan(cc0.yyyy, vec4(0.0))"));
    CHECK(contains(result.glsl, "fract(") && contains(result.glsl, "floor("));

    const auto begin = std::chrono::steady_clock::now();
    const bool ok = compiles(result, 0xf400);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin);
    CHECK(ok);
    std::printf("text program: %zu bytes of GLSL, shaderc %.1f ms (cache hits excluded)\n", result.glsl.size(),
                elapsed.count());
}

void testDumps() {
    const std::vector<Dump> dumps = loadDumps();
    if (dumps.empty()) {
        std::printf("fp translator: no dumps in generated/rsx, dump tests skipped\n");
        return;
    }
    printHistogram(dumps);

    bool printed = false;
    int seen = 0;
    for (const Dump& dump : dumps) {
        if (dump.name.starts_with("fp_c5a1c19123aa52bb")) continue;  // covered by testTextProgram
        const tox2::GlslResult result =
                tox2::translateFragmentProgram(dump.words, controlFor(dump.name), texcoordControlFor(dump.name));
        CHECK(result.ok);
        CHECK(result.unimplemented.empty());
        for (const std::string& entry : result.unimplemented) {
            std::fprintf(stderr, "%s: unimplemented: %s\n", dump.name.c_str(), entry.c_str());
        }
        const std::string body = expectedBody(dump.name);
        CHECK(!body.empty());  // an unknown dump must not skip the comparison silently
        if (!body.empty()) {
            CHECK(contains(result.glsl, body));
            CHECK(contains(result.glsl, expectedExport(dump.name)));
            CHECK(result.texture_units == expectedUnits(dump.name));
            seen++;
        }
        CHECK(compiles(result, 0xf0f0));
        if (!printed && dump.name.starts_with("fp_fa922174d3478656")) {
            std::printf("translated %s:\n%s\n", dump.name.c_str(), result.glsl.c_str());
            printed = true;
        }
    }
#ifdef TOX2_RSX_DUMP_DIR
    CHECK(seen == 3);  // all three known programs must be present once the directory has dumps
#endif
}

// ---- Hand-assembled programs ----

void testAssembler() {
    // The assembler reproduces decoded instructions word for word (src2 is unused there and carries
    // junk in the dumps, so only the first three words are compared).
    Asm tex;
    tex.op = 0x17;
    tex.dest_half = true;
    tex.input = 4;
    tex.end = true;
    auto w = tex.words();
    CHECK(w[0] == 0x17009e81 && w[1] == 0x1c9dc801 && w[2] == 0x0001c800);

    Asm mul;
    mul.op = 2;
    mul.src0 = temp(0, true);
    mul.src1 = inputSrc();
    mul.end = true;
    w = mul.words();
    CHECK(w[0] == 0x02003e01 && w[1] == 0x1c9dc900 && w[2] == 0x0001c801);
}

void testTextureOnly() {
    Asm tex;
    tex.op = 0x17;
    tex.dest_half = true;
    tex.input = 4;  // TEX0
    tex.end = true;
    const tox2::GlslResult result = translate({tex}, 0);
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  h_0 = texture(tex0, var_tex0.xy * draw.texture_scale[0].xy);\n"));
    CHECK(contains(result.glsl, "layout(location = 0) in vec4 var_tex0;"));
    CHECK(contains(result.glsl, "layout(set = 0, binding = 3) uniform sampler2D tex0;"));
    CHECK(contains(result.glsl, "  out_color0 = h_0;\n"));
    CHECK(result.texture_units == 1u);
    CHECK(compiles(result, 0xf0f1));

    // Projective sample from unit 5 with TEX3.
    Asm txp;
    txp.op = 0x18;
    txp.unit = 5;
    txp.input = 7;
    txp.src0 = inputSrc("xyzw");
    txp.end = true;
    const tox2::GlslResult projective = translate({txp});
    CHECK(projective.ok);
    CHECK(contains(projective.glsl, "texture(tex5, (var_tex3.xy / var_tex3.w) * draw.texture_scale[5].xy)"));
    CHECK(projective.texture_units == 1u << 5);
    CHECK(compiles(projective, 0xf0f2));

    // The texture expand bit is not implemented.
    txp.bx2 = true;
    const tox2::GlslResult expand = translate({txp});
    CHECK(!expand.ok && hasReport(expand, "texture expand"));
}

// The sampler type of each unit comes from the texture bound to it: sampler3D and samplerCube read three
// coordinates, TXP divides them by w, and a cube lookup is not scaled for unnormalized coordinates.
void testTextureTypes() {
    const auto types = [](std::uint32_t unit, std::uint32_t type) { return type << (unit * 2); };
    const auto build = [&](const std::vector<Asm>& instructions, std::uint32_t texture_types) {
        return tox2::translateFragmentProgram(program(instructions), 0x40, 0, texture_types);
    };

    Asm tex;
    tex.op = 0x17;
    tex.input = 4;  // TEX0
    tex.end = true;
    const tox2::GlslResult volume = build({tex}, types(0, tox2::kFpTextureType3D));
    CHECK(volume.ok);
    CHECK(contains(volume.glsl, "layout(set = 0, binding = 3) uniform sampler3D tex0;"));
    CHECK(contains(volume.glsl, "texture(tex0, var_tex0.xyz * draw.texture_scale[0].xyz)"));
    CHECK(volume.texture_units == 1u && compiles(volume, 0xf0f3));

    Asm txp;
    txp.op = 0x18;
    txp.unit = 5;
    txp.input = 7;  // TEX3
    txp.src0 = inputSrc("xyzw");
    txp.end = true;
    const tox2::GlslResult projective = build({txp}, types(5, tox2::kFpTextureType3D));
    CHECK(projective.ok && contains(projective.glsl, "uniform sampler3D tex5;"));
    CHECK(contains(projective.glsl, "texture(tex5, (var_tex3.xyz / var_tex3.w) * draw.texture_scale[5].xyz)"));
    CHECK(compiles(projective, 0xf0f4));

    const tox2::GlslResult cube = build({txp}, types(5, tox2::kFpTextureTypeCube));
    CHECK(cube.ok && contains(cube.glsl, "layout(set = 0, binding = 8) uniform samplerCube tex5;"));
    CHECK(contains(cube.glsl, "texture(tex5, (var_tex3.xyz / var_tex3.w))") &&
          !contains(cube.glsl, "texture_scale[5]"));
    CHECK(compiles(cube, 0xf0f5));
    tex.unit = 2;
    CHECK(contains(build({tex}, types(2, tox2::kFpTextureTypeCube)).glsl, "texture(tex2, var_tex0.xyz)"));

    // The types are per unit, and a unit the program does not sample does not change its text.
    Asm first = tex, second = tex, third = tex;
    first.unit = 0;
    first.dest = 0;
    first.end = false;
    second.unit = 1;
    second.dest = 1;
    second.end = false;
    third.unit = 2;
    third.dest = 2;
    const std::uint32_t mixed = types(0, tox2::kFpTextureType3D) | types(1, tox2::kFpTextureTypeCube);
    const tox2::GlslResult three = build({first, second, third}, mixed);
    CHECK(three.ok && contains(three.glsl, "uniform sampler3D tex0;") &&
          contains(three.glsl, "uniform samplerCube tex1;"));
    CHECK(contains(three.glsl, "uniform sampler2D tex2;") && three.texture_units == 7u && compiles(three, 0xf0f6));
    CHECK(build({tex}, types(7, tox2::kFpTextureType3D)).glsl == build({tex}, 0).glsl);
    CHECK(build({tex}, 0).glsl == translate({tex}).glsl);
}

void testConstants() {
    // Each instruction with a constant operand takes the next fc slot; all its constant sources share it.
    Asm first;
    first.op = 2;
    first.src0 = constant("wzyx");
    first.src1 = constant();
    Asm second;
    second.op = 3;
    second.dest = 1;
    second.src0 = temp(0);
    second.src1 = constant("xxxx");
    Asm out;
    out.op = 1;
    out.src0 = temp(1);
    out.end = true;
    const tox2::GlslResult result = translate({first, second, out});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "vec4 fc[2];"));
    CHECK(contains(result.glsl, "  r_0 = fc[0].wzyx * fc[0];\n"));
    CHECK(contains(result.glsl, "  r_1 = r_0 + fc[1].xxxx;\n"));
    CHECK(compiles(result, 0xf0f3));
}

void testOperations() {
    std::vector<Asm> instructions;
    const std::uint32_t ops[] = {1, 2, 3, 4, 5, 6, 8, 9};
    for (const std::uint32_t op : ops) {
        Asm in;
        in.op = op;
        in.dest = 2;
        in.src0 = inputSrc();
        in.src1 = temp(1, true);
        in.src2 = temp(2);
        instructions.push_back(in);
    }
    Asm tex;
    tex.op = 0x17;
    tex.dest = 3;
    tex.input = 4;
    instructions.push_back(tex);
    instructions.back().end = true;
    const tox2::GlslResult result = translate(instructions);
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_2 = in_col0 + h_1;\n"));
    CHECK(contains(result.glsl, "  r_2 = fma(in_col0, h_1, r_2);\n"));
    CHECK(contains(result.glsl, "  r_2 = vec4(dot(in_col0.xyz, h_1.xyz));\n"));
    CHECK(contains(result.glsl, "  r_2 = vec4(dot(in_col0, h_1));\n"));
    CHECK(contains(result.glsl, "  r_2 = min(in_col0, h_1);\n"));
    CHECK(contains(result.glsl, "  r_2 = max(in_col0, h_1);\n"));
    CHECK(compiles(result, 0xf0f4));
}

void testCompareAndRounding() {
    const struct {
        std::uint32_t op;
        const char* expected;
    } cases[] = {
            {0x0a, "  r_2 = vec4(lessThan(in_col0, h_1));\n"},
            {0x0b, "  r_2 = vec4(greaterThanEqual(in_col0, h_1));\n"},
            {0x0c, "  r_2 = vec4(lessThanEqual(in_col0, h_1));\n"},
            {0x0d, "  r_2 = vec4(greaterThan(in_col0, h_1));\n"},
            {0x0e, "  r_2 = vec4(notEqual(in_col0, h_1));\n"},
            {0x0f, "  r_2 = vec4(equal(in_col0, h_1));\n"},
            {0x10, "  r_2 = fract(in_col0);\n"},
            {0x11, "  r_2 = floor(in_col0);\n"},
    };
    std::uint32_t key = 0xf100;
    for (const auto& test : cases) {
        Asm in;
        in.op = test.op;
        in.dest = 2;
        in.src0 = inputSrc();
        in.src1 = temp(1, true);
        in.end = true;
        const tox2::GlslResult result = translate({in});
        CHECK(result.ok);
        CHECK(contains(result.glsl, test.expected));
        CHECK(compiles(result, key++));
    }

    // The result scale and masked write apply to the 1.0 / 0.0 answer like any other value.
    Asm masked;
    masked.op = 0x0b;
    masked.mask = 0x2;
    masked.scale = 1;
    masked.src0 = inputSrc("yyyy");
    masked.src1 = temp(1);
    masked.end = true;
    const tox2::GlslResult result = translate({masked});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_0.y = (vec4(greaterThanEqual(in_col0.yyyy, r_1)) * 2.0).y;\n"));
}

void testConditionalExecution() {
    // Execute mask (lt = 1, eq = 2, gr = 4) to the per-lane predicate against zero.
    const struct {
        std::uint32_t mask;
        const char* compare;
    } table[] = {
            {6, "greaterThanEqual"}, {3, "lessThanEqual"}, {5, "notEqual"},
            {4, "greaterThan"},      {1, "lessThan"},      {2, "equal"},
    };
    std::uint32_t key = 0xf200;
    for (const auto& entry : table) {
        Asm mov;
        mov.op = 1;
        mov.cond = entry.mask;
        mov.cond_swizzle = "yyyy";
        mov.cc_read = 1;
        mov.end = true;
        const tox2::GlslResult result = translate({mov});
        CHECK(result.ok);
        CHECK(contains(result.glsl,
                       std::string("  r_0 = mix(r_0, in_col0, ") + entry.compare + "(cc1.yyyy, vec4(0.0)));\n"));
        CHECK(contains(result.glsl, "  vec4 cc1 = vec4(0.0);\n"));
        CHECK(!contains(result.glsl, "cc0"));
        CHECK(compiles(result, key++));
    }

    // Swizzled lanes map to the written lanes; a partial write mask selects per written lane.
    Asm lanes;
    lanes.op = 2;
    lanes.mask = 0x5;  // x and z
    lanes.cond = 1;
    lanes.cond_swizzle = "wzyx";
    lanes.end = true;
    tox2::GlslResult result = translate({lanes});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_0.xz = mix(r_0.xz, (in_col0 * r_0).xz, lessThan(cc0.wzyx, vec4(0.0)).xz);\n"));
    CHECK(contains(result.glsl, "  vec4 cc0 = vec4(0.0);\n") && !contains(result.glsl, "cc1"));
    CHECK(compiles(result, key++));

    // All three bits is unconditional and does not need the register; none disables the instruction.
    Asm always;
    always.cond = 7;
    always.cc_read = 1;
    always.cond_swizzle = "zzzz";
    always.end = true;
    result = translate({always});
    CHECK(result.ok && contains(result.glsl, "  r_0 = in_col0;\n") && !contains(result.glsl, "cc1"));
    Asm never;
    never.cond = 0;
    never.end = true;
    result = translate({never});
    CHECK(result.ok && !contains(result.glsl, "  r_0 = ") && !contains(result.glsl, "mix("));
}

void testSetCondition() {
    std::uint32_t key = 0xf300;

    // No destination: the masked value goes to the register selected by src0 bit 30 and no temporary
    // is touched. Scale, precision and saturate apply before the write.
    Asm no_dest;
    no_dest.op = 2;
    no_dest.cc_update = true;
    no_dest.no_dest = true;
    no_dest.dest = 63;
    no_dest.mask = 0x3;
    no_dest.cc_write = 1;
    no_dest.dest_precision = 2;
    no_dest.src0 = inputSrc();
    no_dest.src1 = temp(0);
    Asm use;
    use.op = 1;
    use.cond = 1;
    use.cc_read = 1;
    use.cond_swizzle = "xxxx";
    use.end = true;
    tox2::GlslResult result = translate({no_dest, use});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  cc1.xy = (fp_clamp(in_col0 * r_0, -2.0, 2.0)).xy;\n"));
    CHECK(contains(result.glsl, "  r_0 = mix(r_0, in_col0, lessThan(cc1.xxxx, vec4(0.0)));\n"));
    CHECK(!contains(result.glsl, "r_63") && !contains(result.glsl, "cc0"));
    CHECK(compiles(result, key++));

    // With a destination the register copies the masked destination value after the assignment.
    Asm with_dest;
    with_dest.op = 1;
    with_dest.dest = 2;
    with_dest.mask = 0x9;  // x and w
    with_dest.cc_update = true;
    with_dest.end = true;
    result = translate({with_dest});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_2.xw = (in_col0).xw;\n  cc0.xw = r_2.xw;\n"));
    CHECK(compiles(result, key++));

    // A conditional update with a full mask: the condition gates the register write too.
    Asm conditional;
    conditional.op = 1;
    conditional.cc_update = true;
    conditional.no_dest = true;
    conditional.cond = 4;
    conditional.end = true;
    result = translate({conditional});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  cc0 = mix(cc0, in_col0, greaterThan(cc0, vec4(0.0)));\n"));
    CHECK(compiles(result, key++));

    // Without the update bit a no-destination instruction produces nothing.
    Asm nothing;
    nothing.no_dest = true;
    nothing.end = true;
    result = translate({nothing});
    CHECK(result.ok && !contains(result.glsl, "cc0") && !contains(result.glsl, "  r_0 = in_col0"));

    // The first update must not leak into the declaration of the other register.
    Asm second;
    second.op = 1;
    second.cc_update = true;
    second.no_dest = true;
    second.cc_write = 1;
    second.end = true;
    result = translate({second});
    CHECK(result.ok && contains(result.glsl, "  vec4 cc1 = vec4(0.0);\n") && !contains(result.glsl, "vec4 cc0"));
}

void testModifiers() {
    // Masked write with result scale, destination precision and saturate.
    Asm mul;
    mul.op = 2;
    mul.mask = 0x7;
    mul.scale = 2;  // x4
    mul.dest_precision = 3;
    mul.saturate = true;
    mul.src0 = inputSrc();
    mul.src0.negate = true;
    mul.src0.absolute = true;
    mul.src0.precision = 4;
    mul.src1 = temp(1, false, "yyyy");
    mul.src1.precision = 2;
    mul.end = true;
    const tox2::GlslResult result = translate({mul});
    CHECK(result.ok);
    CHECK(contains(result.glsl,
                   "  r_0.xyz = (fp_clamp(fp_clamp((-abs(fp_clamp(in_col0, 0.0, 1.0)) * "
                   "fp_clamp(r_1.yyyy, -2.0, 2.0)) * 4.0, -1.0, 1.0), 0.0, 1.0)).xyz;\n"));
    CHECK(compiles(result, 0xf0f5));

    // Half precision on a source and as the destination modifier, and the /2 scale.
    Asm half;
    half.op = 1;
    half.scale = 5;
    half.dest_precision = 1;
    half.src0 = inputSrc();
    half.src0.precision = 1;
    half.end = true;
    const tox2::GlslResult half_result = translate({half});
    CHECK(half_result.ok);
    CHECK(contains(half_result.glsl, "  r_0 = fp_half(fp_half(in_col0) * 0.5);\n"));
    CHECK(compiles(half_result, 0xf0f6));
}

void testInputsAndEpilogue() {
    // COL1 and a 2D texture coordinate; fp16 export (control 0) with the alpha test in the epilogue.
    Asm add;
    add.op = 3;
    add.input = 2;  // COL1
    add.src0 = inputSrc();
    add.src1 = temp(0);
    Asm tex_coord;
    tex_coord.op = 1;
    tex_coord.dest_half = true;
    tex_coord.input = 5;  // TEX1
    tex_coord.src0 = inputSrc();
    tex_coord.end = true;
    const tox2::GlslResult result = translate({add, tex_coord}, 0, 1u << 1);
    CHECK(result.ok);
    CHECK(contains(result.glsl,
                   "vec4 in_col1 = clamp((draw.flags & 1u) != 0u && !gl_FrontFacing ? "
                   "var_back_specular : var_front_specular, 0.0, 1.0);"));
    CHECK(contains(result.glsl, "vec4 in_tex1 = vec4(var_tex1.xy, 0.0, gl_FragCoord.w);"));
    CHECK(contains(result.glsl, "  h_0 = in_tex1;\n"));
    CHECK(contains(result.glsl, "case 4u: alpha_passes = h_0.a > draw.alpha_ref; break;"));
    CHECK(contains(result.glsl, "  out_color0 = h_0;\n"));
    // The 8-bit rounding flag applies before the alpha compare.
    CHECK(contains(result.glsl,
                   "  if ((draw.flags & 2u) != 0u) h_0 = floor(max(h_0, 0.0) * 255.0 + 0.5) / 255.0;\n"
                   "  bool alpha_passes = true;\n"));
    CHECK(compiles(result, 0xf0f7));

    // An empty program still exports the (zero) output register under either control value.
    Asm nop;
    nop.op = 0;
    nop.end = true;
    const tox2::GlslResult full = translate({nop}, 0x40);
    CHECK(full.ok && contains(full.glsl, "vec4 r_0 = vec4(0.0);") && contains(full.glsl, "out_color0 = r_0;"));
    CHECK(compiles(full, 0xf0f8));
    const tox2::GlslResult halves = translate({nop}, 0);
    CHECK(halves.ok && contains(halves.glsl, "vec4 h_0 = vec4(0.0);") && contains(halves.glsl, "out_color0 = h_0;"));

    // Depth export reads R1.z.
    const tox2::GlslResult depth = translate({nop}, 0x40 | 0x2);
    CHECK(depth.ok && contains(depth.glsl, "gl_FragDepth = fp_clamp(r_1, 0.0, 1.0).z;"));
    CHECK(compiles(depth, 0xf0f9));
}

void testUnimplementedReports() {
    Asm unknown;
    unknown.op = 0x3f;
    unknown.end = true;
    tox2::GlslResult result = translate({unknown});
    CHECK(!result.ok && result.glsl.empty());
    CHECK(hasReport(result, "fp opcode 0x3f"));

    Asm flow;
    flow.op = 0x40;  // bit 6 comes from W2
    flow.end = true;
    result = translate({flow});
    CHECK(!result.ok && hasReport(result, "fp opcode 0x40"));

    // An unsupported opcode is still reported when the instruction is conditional or updates a
    // condition register.
    Asm conditional_unknown;
    conditional_unknown.op = 0x1b;
    conditional_unknown.cond = 2;
    conditional_unknown.cc_update = true;
    conditional_unknown.end = true;
    result = translate({conditional_unknown});
    CHECK(!result.ok && hasReport(result, "fp opcode 0x1b"));

    Asm wpos;
    wpos.input = 0;
    wpos.end = true;
    result = translate({wpos});
    CHECK(!result.ok && hasReport(result, "WPOS"));

    Asm fog;
    fog.input = 3;
    fog.end = true;
    result = translate({fog});
    CHECK(!result.ok && hasReport(result, "FOGC"));

    // W3 bit 30 is instruction-wide: an input on src0 plus the index bit is reported.
    Asm indexed;
    indexed.src0 = inputSrc();
    indexed.indexed = true;
    indexed.end = true;
    result = translate({indexed});
    CHECK(!result.ok && hasReport(result, "indexed input"));

    Asm bad_scale;
    bad_scale.scale = 4;
    bad_scale.end = true;
    result = translate({bad_scale});
    CHECK(!result.ok && hasReport(result, "scale"));

    Asm invalid_source;
    invalid_source.src0.type = 3;
    invalid_source.end = true;
    result = translate({invalid_source});
    CHECK(!result.ok && hasReport(result, "source type 3"));

    // A source field the opcode does not read is not decoded (MOV reads src0 only).
    Asm unused_invalid;
    unused_invalid.src1.type = 3;
    unused_invalid.src2.type = 3;
    unused_invalid.end = true;
    CHECK(translate({unused_invalid}).ok);

    // No condition bit set disables the instruction; a NOP is skipped, constant block included.
    Asm never;
    never.op = 0x3f;
    never.cond = 0;
    Asm nop;
    nop.op = 0;
    nop.src0 = constant();
    nop.end = true;
    result = translate({never, nop});
    CHECK(result.ok && !contains(result.glsl, "fc["));

    result = tox2::translateFragmentProgram({}, 0x40, 0);
    CHECK(!result.ok);
}

void testScalarAndDivide() {
    // RCP, EX2 and LG2 read the x lane of the swizzled source and replicate the result.
    const struct {
        std::uint32_t op;
        const char* expected;
    } scalar[] = {
            {0x1a, "  r_2 = vec4(1.0 / in_col0.yyyy.x);\n"},
            {0x1c, "  r_2 = vec4(exp2(in_col0.yyyy.x));\n"},
            {0x1d, "  r_2 = vec4(log2(in_col0.yyyy.x));\n"},
    };
    std::uint32_t key = 0xf600;
    for (const auto& test : scalar) {
        Asm in;
        in.op = test.op;
        in.dest = 2;
        in.src0 = inputSrc("yyyy");
        in.end = true;
        const tox2::GlslResult result = translate({in});
        CHECK(result.ok);
        CHECK(contains(result.glsl, test.expected));
        CHECK(compiles(result, key++));
    }

    // DIV and DIVSQ divide the whole src0 vector by src1.x; modifiers stay inside the scalar operand.
    Asm div;
    div.op = 0x3a;
    div.dest = 2;
    div.src0 = inputSrc();
    div.src1 = temp(1, true, "zyxw");
    div.src1.negate = true;
    div.end = true;
    tox2::GlslResult result = translate({div});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  r_2 = in_col0 / -h_1.zyxw.x;\n"));
    CHECK(compiles(result, key++));

    // The scale applies to the whole quotient.
    div.scale = 1;
    div.src1.negate = false;
    result = translate({div});
    CHECK(result.ok && contains(result.glsl, "  r_2 = (in_col0 / h_1.zyxw.x) * 2.0;\n"));
    CHECK(compiles(result, key++));

    Asm divsq;
    divsq.op = 0x3b;
    divsq.dest = 2;
    divsq.mask = 0x7;
    divsq.src0 = inputSrc();
    divsq.src1 = temp(1, true);
    divsq.end = true;
    result = translate({divsq});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "vec4 fp_divsq(vec4 a, float b) {"));
    CHECK(contains(result.glsl, "  r_2.xyz = (fp_divsq(in_col0, h_1.x)).xyz;\n"));
    CHECK(compiles(result, key++));

    // The helper is only emitted for programs that use it.
    CHECK(!contains(translate({div}).glsl, "fp_divsq"));
}

void testKillAndFences() {
    std::uint32_t key = 0xf700;

    // An unconditional KIL discards; it writes no register.
    Asm kil;
    kil.op = 0x12;
    kil.end = true;
    tox2::GlslResult result = translate({kil});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  discard;\n\n  if ((draw.flags & 2u)"));
    CHECK(!contains(result.glsl, "  r_0 = ") && !contains(result.glsl, "mix("));
    CHECK(compiles(result, key++));

    // A conditional KIL discards when any selected lane passes; the condition register is declared.
    Asm cond_kil;
    cond_kil.op = 0x12;
    cond_kil.cond = 1;  // less than zero
    cond_kil.cond_swizzle = "wzyx";
    cond_kil.cc_read = 1;
    Asm mov;
    mov.op = 1;
    mov.end = true;
    result = translate({cond_kil, mov});
    CHECK(result.ok);
    CHECK(contains(result.glsl, "  if (any(lessThan(cc1.wzyx, vec4(0.0)))) discard;\n  r_0 = in_col0;\n"));
    CHECK(contains(result.glsl, "  vec4 cc1 = vec4(0.0);\n") && !contains(result.glsl, "cc0"));
    CHECK(compiles(result, key++));

    // No condition bit set disables KIL.
    Asm never_kil = cond_kil;
    never_kil.cond = 0;
    result = translate({never_kil, mov});
    CHECK(result.ok && !contains(result.glsl, "discard;\n  r_0") && !contains(result.glsl, "if (any("));

    // FENCT and FENCB schedule work and emit nothing, however their condition or update bits are set.
    for (const std::uint32_t op : {0x3du, 0x3eu}) {
        Asm fence;
        fence.op = op;
        fence.cond = 2;
        fence.cc_update = true;
        Asm out;
        out.op = 1;
        out.end = true;
        result = translate({fence, out});
        CHECK(result.ok && result.unimplemented.empty());
        CHECK(contains(result.glsl, "  r_0 = in_col0;\n"));
        CHECK(!contains(result.glsl, "cc0") && !contains(result.glsl, "mix("));
        CHECK(compiles(result, key++));
    }
}

void testLayout() {
    CHECK(tox2::kFpDrawBlockOffset == 64);
    CHECK(tox2::fpFixupWord(0x9e811700u) == 0x17009e81u);
    const tox2::GlslResult result = translate({});
    CHECK(!result.ok);

    Asm nop;
    nop.op = 0;
    nop.end = true;
    const tox2::GlslResult ok = translate({nop});
    CHECK(contains(ok.glsl, "layout(std140, set = 0, binding = 2) uniform FpDraw {\n  vec4 vertex_part[4];\n"));
}

}  // namespace

int main() {
    testAssembler();
    testTextureOnly();
    testTextureTypes();
    testConstants();
    testOperations();
    testCompareAndRounding();
    testConditionalExecution();
    testSetCondition();
    testModifiers();
    testInputsAndEpilogue();
    testUnimplementedReports();
    testScalarAndDivide();
    testKillAndFences();
    testLayout();
    testTextProgram();
    testDumps();
    std::printf("fp translator tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

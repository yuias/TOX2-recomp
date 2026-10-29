// Self-checking tests for the object motion candidates, signatures and matcher. No Vulkan device is needed.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "render/object_motion.h"
#include "rsx/rsx_registers.h"

namespace {

using tox2::MotionDrawMatcher;
using tox2::MotionDrawSignature;
using tox2::MotionMatchRule;
using tox2::RsxDraw;

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

std::shared_ptr<const std::vector<std::uint32_t>> constants(bool perspective) {
    auto words = std::make_shared<std::vector<std::uint32_t>>(tox2::kRsxVpConstantCount * 4, 0u);
    // c0..c3 are an identity matrix; a perspective projection has a non-zero w column (floats 3, 7, 11).
    for (std::size_t i = 0; i < 4; ++i) (*words)[i * 4 + i] = std::bit_cast<std::uint32_t>(1.0f);
    if (perspective) (*words)[11] = std::bit_cast<std::uint32_t>(1.0f);
    return words;
}

RsxDraw makeDraw() {
    RsxDraw draw;
    draw.surfaces.color.samples_x = 2;
    draw.surfaces.color.samples_y = 2;
    draw.surfaces.depth_format = tox2::RsxDepthFormat::Z24S8;
    draw.raster.depth_test = true;
    draw.raster.depth_write = true;
    draw.vp_constants = constants(true);
    draw.vp_hash = 0x1111;
    draw.fp_hash = 0x2222;
    draw.primitive = tox2::RsxPrimitive::Triangles;
    draw.indexed = true;
    draw.index_type = 1;
    draw.first = 10;
    draw.count = 300;
    draw.min_index = 40;
    draw.max_index = 139;
    for (std::uint8_t slot : {0, 1, 3}) {
        tox2::RsxVertexAttrib attrib;
        attrib.slot = slot;
        attrib.type = 2;
        attrib.size = 3;
        attrib.stride = 12;
        attrib.location = 1;
        attrib.offset = 0x1000u + slot * 0x400u;
        attrib.has_data = true;
        draw.attribs.push_back(attrib);
    }
    tox2::RsxTextureDesc texture;
    texture.unit = 0;
    texture.location = 1;
    texture.offset = 0x8000;
    texture.format = 0x85;
    draw.textures.push_back(texture);
    draw.vertex_data.assign(100 * 12 * 3, 0xAB);
    return draw;
}

void testCandidate() {
    RsxDraw draw = makeDraw();
    CHECK(tox2::objectMotionCandidate(draw));

    RsxDraw ortho = draw;
    ortho.vp_constants = constants(false);
    CHECK(!tox2::objectMotionCandidate(ortho));

    RsxDraw no_constants = draw;
    no_constants.vp_constants = nullptr;
    CHECK(!tox2::objectMotionCandidate(no_constants));

    RsxDraw one_sample = draw;
    one_sample.surfaces.color.samples_x = 1;
    one_sample.surfaces.color.samples_y = 1;
    CHECK(!tox2::objectMotionCandidate(one_sample));

    RsxDraw two_by_one = draw;
    two_by_one.surfaces.color.samples_y = 1;
    CHECK(tox2::objectMotionCandidate(two_by_one));

    RsxDraw no_depth_write = draw;
    no_depth_write.raster.depth_write = false;
    CHECK(!tox2::objectMotionCandidate(no_depth_write));

    RsxDraw no_depth_test = draw;
    no_depth_test.raster.depth_test = false;
    CHECK(!tox2::objectMotionCandidate(no_depth_test));
}

void testSignature() {
    const RsxDraw base = makeDraw();
    const MotionDrawSignature sig = tox2::motionDrawSignature(base);
    CHECK(sig == tox2::motionDrawSignature(base));

    // Only the vertex bytes and the constants differ: the same mesh in another pose.
    RsxDraw content = base;
    content.vertex_data.assign(content.vertex_data.size(), 0x11);
    content.vp_constants = constants(true);
    (*std::const_pointer_cast<std::vector<std::uint32_t>>(content.vp_constants))[40] = 123;
    content.vp_constants_id = 99;
    CHECK(tox2::motionDrawSignature(content) == sig);

    RsxDraw stride = base;
    stride.attribs[1].stride = 16;
    CHECK(tox2::motionDrawSignature(stride).layout != sig.layout);

    RsxDraw count = base;
    count.count = 303;
    CHECK(tox2::motionDrawSignature(count).layout != sig.layout);

    RsxDraw vertex_count = base;
    vertex_count.max_index += 1;
    CHECK(tox2::motionDrawSignature(vertex_count).layout != sig.layout);

    RsxDraw texture = base;
    texture.textures[0].offset += 0x100;
    CHECK(tox2::motionDrawSignature(texture).layout != sig.layout);

    RsxDraw vp = base;
    vp.vp_hash += 1;
    CHECK(tox2::motionDrawSignature(vp).layout != sig.layout);

    RsxDraw no_data = base;
    no_data.attribs[2].has_data = false;
    CHECK(tox2::motionDrawSignature(no_data).layout != sig.layout);

    // The layout does not depend on where the data sits.
    RsxDraw moved = base;
    moved.attribs[0].offset += 0x10000;
    CHECK(tox2::motionDrawSignature(moved).layout == sig.layout);
    CHECK(tox2::motionDrawSignature(moved).address != sig.address);

    RsxDraw other_location = base;
    other_location.attribs[2].location = 0;
    CHECK(tox2::motionDrawSignature(other_location).layout == sig.layout);
    CHECK(tox2::motionDrawSignature(other_location).address != sig.address);

    // The same arrays at a shifted first/min_index with the same vertex count.
    RsxDraw shifted = base;
    shifted.first += 50;
    shifted.min_index += 50;
    shifted.max_index += 50;
    CHECK(tox2::motionDrawSignature(shifted).layout == sig.layout);
    CHECK(tox2::motionDrawSignature(shifted).address != sig.address);

    RsxDraw shifted_first = base;
    shifted_first.first += 1;
    CHECK(tox2::motionDrawSignature(shifted_first).layout == sig.layout);
    CHECK(tox2::motionDrawSignature(shifted_first).address != sig.address);

    // The attribute list order does not matter, only the slots.
    RsxDraw reversed = base;
    std::vector<tox2::RsxVertexAttrib> attribs(base.attribs.rbegin(), base.attribs.rend());
    reversed.attribs = attribs;
    CHECK(tox2::motionDrawSignature(reversed) == sig);
}

std::vector<std::optional<std::uint32_t>> matchAll(std::span<const MotionDrawSignature> previous,
                                                   std::span<const MotionDrawSignature> current, MotionMatchRule rule) {
    MotionDrawMatcher matcher;
    matcher.beginFrame(previous, rule);
    std::vector<std::optional<std::uint32_t>> out;
    for (const MotionDrawSignature& sig : current) out.push_back(matcher.match(sig));
    return out;
}

using Matches = std::vector<std::optional<std::uint32_t>>;
constexpr MotionMatchRule kRules[] = {MotionMatchRule::Ordered, MotionMatchRule::AddressThenOrdered,
                                      MotionMatchRule::AddressOnly};

void testMatcher() {
    // Layouts 1 (three repeats) and 2, with distinct addresses.
    const std::vector<MotionDrawSignature> frame = {{1, 10}, {2, 20}, {1, 11}, {1, 12}, {2, 21}};
    for (const MotionMatchRule rule : kRules) {
        const Matches same = matchAll(frame, frame, rule);
        CHECK((same == Matches{0, 1, 2, 3, 4}));
    }

    // A draw inserted at the front shifts the ordered match and leaves the address match alone.
    std::vector<MotionDrawSignature> inserted = frame;
    inserted.insert(inserted.begin(), MotionDrawSignature{1, 99});
    // The inserted draw takes the first unused previous draw of its layout, which moves every later one of that layout.
    const Matches shifted_by_insert = {0, 2, 1, 3, std::nullopt, 4};
    CHECK((matchAll(frame, inserted, MotionMatchRule::Ordered) == shifted_by_insert));
    CHECK((matchAll(frame, inserted, MotionMatchRule::AddressThenOrdered) == shifted_by_insert));
    CHECK((matchAll(frame, inserted, MotionMatchRule::AddressOnly) == Matches{std::nullopt, 0, 1, 2, 3, 4}));

    // A mesh drawn into alternating buffers: the address changes every frame.
    const std::vector<MotionDrawSignature> prev_buffers = {{5, 100}, {5, 200}};
    const std::vector<MotionDrawSignature> next_buffers = {{5, 300}, {5, 400}};
    CHECK((matchAll(prev_buffers, next_buffers, MotionMatchRule::Ordered) == Matches{0, 1}));
    CHECK((matchAll(prev_buffers, next_buffers, MotionMatchRule::AddressThenOrdered) == Matches{0, 1}));
    CHECK((matchAll(prev_buffers, next_buffers, MotionMatchRule::AddressOnly) == Matches{std::nullopt, std::nullopt}));

    // Two buffers that swap roles: the ordered rule pairs by order, the address rules by address.
    const std::vector<MotionDrawSignature> swapped = {{5, 200}, {5, 100}};
    CHECK((matchAll(prev_buffers, swapped, MotionMatchRule::Ordered) == Matches{0, 1}));
    CHECK((matchAll(prev_buffers, swapped, MotionMatchRule::AddressThenOrdered) == Matches{1, 0}));
    CHECK((matchAll(prev_buffers, swapped, MotionMatchRule::AddressOnly) == Matches{1, 0}));

    // A single mesh whose buffer alternates.
    const std::vector<MotionDrawSignature> one_prev = {{7, 1}};
    const std::vector<MotionDrawSignature> one_next = {{7, 2}};
    CHECK((matchAll(one_prev, one_next, MotionMatchRule::Ordered) == Matches{0}));
    CHECK((matchAll(one_prev, one_next, MotionMatchRule::AddressThenOrdered) == Matches{0}));
    CHECK((matchAll(one_prev, one_next, MotionMatchRule::AddressOnly) == Matches{std::nullopt}));

    // Another layout never matches, and more current draws than previous ones leave the extra ones unmatched.
    const std::vector<MotionDrawSignature> other_layout = {{9, 10}};
    const std::vector<MotionDrawSignature> twice = {{1, 10}, {1, 10}, {1, 10}};
    const std::vector<MotionDrawSignature> once = {{1, 10}};
    for (const MotionMatchRule rule : kRules) {
        CHECK((matchAll(frame, other_layout, rule) == Matches{std::nullopt}));
        CHECK((matchAll(once, twice, rule) == Matches{0, std::nullopt, std::nullopt}));
    }

    // A previous index is never returned twice, whatever the input.
    const std::vector<MotionDrawSignature> repeated = {{3, 1}, {3, 1}, {3, 2}, {3, 2}, {3, 3}, {4, 1}, {3, 1}};
    const std::vector<MotionDrawSignature> repeated_next = {{3, 2}, {3, 2}, {3, 2}, {3, 1}, {3, 1},
                                                            {3, 1}, {4, 1}, {3, 9}, {3, 9}, {3, 9}};
    for (const MotionMatchRule rule : kRules) {
        const Matches result = matchAll(repeated, repeated_next, rule);
        std::set<std::uint32_t> seen;
        bool unique = true;
        for (const auto& m : result) {
            if (m && !seen.insert(*m).second) unique = false;
        }
        CHECK(unique);
    }

    // An empty previous list matches nothing, and so does the state before the first beginFrame.
    for (const MotionDrawSignature& sig : frame) {
        MotionDrawMatcher fresh;
        CHECK(!fresh.match(sig));
    }
    for (const MotionMatchRule rule : kRules) {
        CHECK((matchAll(std::span<const MotionDrawSignature>{}, frame, rule) == Matches(frame.size(), std::nullopt)));
    }

    // beginFrame forgets the previous frame's matches.
    MotionDrawMatcher matcher;
    matcher.beginFrame(frame, MotionMatchRule::Ordered);
    CHECK(matcher.match(frame[0]) == 0u);
    matcher.beginFrame(frame, MotionMatchRule::Ordered);
    CHECK(matcher.match(frame[0]) == 0u);
    matcher.beginFrame(std::span<const MotionDrawSignature>{}, MotionMatchRule::Ordered);
    CHECK(!matcher.match(frame[0]));
}

}  // namespace

int main() {
    testCandidate();
    testSignature();
    testMatcher();
    std::printf("object motion tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}

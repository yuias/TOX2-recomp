// Tests for SCE metadata and package decryption on synthetic containers.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"
#include "sce.h"
#include "sce_builder.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

using test::BuiltSce;
using test::buildSce;
using test::buildScePackage;
using test::fakeKeyVault;
using test::SceBuildOptions;
using test::SceBuildSection;
using test::scePackageOptions;

Bytes patternBytes(std::size_t size, std::uint32_t seed) {
    Bytes bytes(size);
    std::uint32_t state = seed;
    for (std::uint8_t& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

void putBe64At(Bytes& file, std::size_t offset, std::uint64_t value) {
    for (int i = 0; i < 8; i++) {
        file[offset + i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
}

}  // namespace

TOX2_TEST(scePackageRoundTrip) {
    const KeyVault keys = fakeKeyVault({});
    const Bytes info0 = patternBytes(0x40, 1);
    const Bytes info1 = patternBytes(0x40, 2);
    // Larger than one CTR batch and not a multiple of 16.
    const Bytes payload = patternBytes(70001 * 3, 3);

    for (const bool debug_flag : {false, true}) {
        SceBuildOptions options = scePackageOptions(keys, info0, info1, payload);
        options.flags = debug_flag ? 0x8000 : 0;
        const BuiltSce built = buildSce(options);

        std::vector<Bytes> outputs;
        std::string error;
        CHECK(decryptScePackage(built.file, keys, outputs, error));
        CHECK(outputs.size() == 3);
        if (outputs.size() == 3) {
            CHECK(outputs[0] == info0);
            CHECK(outputs[1] == info1);
            CHECK(outputs[2] == payload);
        }
    }

    // Header and metadata fields, with a non-zero metadata offset.
    SceBuildOptions options = scePackageOptions(keys, info0, info1, payload);
    options.flags = 0x001C;
    options.prefix = patternBytes(0x40, 4);
    const BuiltSce built = buildSce(options);
    SceHeader header{};
    std::string error;
    CHECK(readSceHeader(built.file, header, error));
    CHECK(header.version == 2);
    CHECK(header.flags == 0x001C);
    CHECK(header.type == 3);
    CHECK(header.metadata_offset == 0x40);
    CHECK(header.header_length == built.data_offsets[0]);

    SceMetadata metadata;
    CHECK(readSceMetadata(built.file, header, keys.scepkg_erk, keys.scepkg_riv, metadata, error));
    CHECK(metadata.sections.size() == 3);
    CHECK(metadata.key_slots == built.key_slots);
    if (metadata.sections.size() == 3) {
        CHECK(metadata.sections[0].type == 1);
        CHECK(metadata.sections[0].encrypted == 1);
        CHECK(metadata.sections[2].type == 3);
        CHECK(metadata.sections[2].encrypted == 3);
        CHECK(metadata.sections[2].key_index == 18);
        CHECK(metadata.sections[2].iv_index == 19);
        CHECK(metadata.sections[2].compressed == 2);
        CHECK(metadata.sections[2].offset == built.data_offsets[2]);
        CHECK(metadata.sections[2].size == built.data_sizes[2]);
    }
}

TOX2_TEST(scePackageWrongKey) {
    const KeyVault keys = fakeKeyVault({});
    const BuiltSce built = buildScePackage(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), patternBytes(300, 3));

    KeyVault wrong = keys;
    wrong.scepkg_erk[5] ^= 0x01;
    std::vector<Bytes> outputs;
    std::string error;
    CHECK(!decryptScePackage(built.file, wrong, outputs, error));
    CHECK(error.find("wrong key") != std::string::npos);

    wrong = keys;
    wrong.scepkg_riv[0] ^= 0x80;
    // A wrong IV only garbles the first CBC block (the metadata key), so the padding check does not
    // see it; the garbled region is then refused by the bounds checks instead.
    CHECK(!decryptScePackage(built.file, wrong, outputs, error));
    CHECK(!error.empty());

    // The right keys still work afterwards.
    CHECK(decryptScePackage(built.file, keys, outputs, error));
}

TOX2_TEST(scePackageBadIndex) {
    const KeyVault keys = fakeKeyVault({});
    const Bytes plain = patternBytes(0x100, 5);
    SceBuildOptions options = scePackageOptions(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), plain);
    options.sections.pop_back();

    // Key slot out of range, IV slot out of range, both in range (for contrast); none compressed.
    SceBuildSection bad_key;
    bad_key.type = 3;
    bad_key.plain = plain;
    bad_key.key_index = 20;
    bad_key.iv_index = 1;
    SceBuildSection bad_iv = bad_key;
    bad_iv.key_index = 0;
    bad_iv.iv_index = 0xFFFFFFF0;
    SceBuildSection good = bad_key;
    good.key_index = 3;
    good.iv_index = 4;
    options.sections.push_back(bad_key);
    options.sections.push_back(bad_iv);
    options.sections.push_back(good);

    const BuiltSce built = buildSce(options);
    std::vector<Bytes> outputs;
    std::string error;
    CHECK(decryptScePackage(built.file, keys, outputs, error));
    CHECK(outputs.size() == 5);
    if (outputs.size() == 5) {
        CHECK(outputs[2] == Bytes(plain.size(), 0));
        CHECK(outputs[3] == Bytes(plain.size(), 0));
        CHECK(outputs[4] == plain);
    }
}

TOX2_TEST(scePackageBadZlib) {
    const KeyVault keys = fakeKeyVault({});
    const BuiltSce built = buildScePackage(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), patternBytes(2000, 3));
    std::vector<Bytes> outputs;
    std::string error;
    CHECK(decryptScePackage(built.file, keys, outputs, error));

    // CTR is a stream cipher, so flipping a stored byte flips the same byte of the compressed stream.
    Bytes bad_header = built.file;
    bad_header[built.data_offsets[2]] ^= 0xFF;
    CHECK(!decryptScePackage(bad_header, keys, outputs, error));
    CHECK(error.find("zlib") != std::string::npos);
    CHECK(error.find("section 2") != std::string::npos);

    // A valid stream that stops before its end is refused too (RPCS3 would drop the section).
    SceBuildOptions options =
            scePackageOptions(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), patternBytes(2000, 3));
    options.sections[2].cut_compressed = 8;
    CHECK(!decryptScePackage(buildSce(options).file, keys, outputs, error));
    CHECK(error.find("zlib") != std::string::npos);
}

TOX2_TEST(sceTruncated) {
    const KeyVault keys = fakeKeyVault({});
    const BuiltSce built = buildScePackage(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), patternBytes(3000, 3));
    std::vector<Bytes> outputs;
    std::string error;
    SceHeader header{};

    // File shorter than the header, and a bad magic.
    CHECK(!readSceHeader(std::span<const std::uint8_t>(built.file).first(0x1F), header, error));
    CHECK(!error.empty());
    Bytes bad_magic = built.file;
    bad_magic[1] = 'X';
    CHECK(!readSceHeader(bad_magic, header, error));
    CHECK(!decryptScePackage(bad_magic, keys, outputs, error));

    // Header length past the end of the file, and too small to hold the metadata.
    Bytes long_header = built.file;
    putBe64At(long_header, 0x10, built.file.size() + 1);
    CHECK(!decryptScePackage(long_header, keys, outputs, error));
    Bytes short_header = built.file;
    putBe64At(short_header, 0x10, 0x40);
    CHECK(!decryptScePackage(short_header, keys, outputs, error));

    // Section data past the end of the file.
    Bytes cut = built.file;
    cut.resize(cut.size() - 1);
    CHECK(!decryptScePackage(cut, keys, outputs, error));
    CHECK(error.find("outside the file") != std::string::npos);

    // Declared counts that do not fit in the metadata region.
    SceBuildOptions options =
            scePackageOptions(keys, patternBytes(0x40, 1), patternBytes(0x40, 2), patternBytes(100, 3));
    options.declared_section_count = 0x10000;
    CHECK(!decryptScePackage(buildSce(options).file, keys, outputs, error));
    options.declared_section_count = test::kNoIndex;
    options.declared_key_count = 0xFFFFFFFF - 1;
    CHECK(!decryptScePackage(buildSce(options).file, keys, outputs, error));
    options.declared_key_count = 0xFFFFFFF0;
    options.declared_section_count = 0xFFFFFFF0;
    CHECK(!decryptScePackage(buildSce(options).file, keys, outputs, error));

    // Every proper prefix of the file is refused without reading out of bounds.
    bool all_refused = true;
    for (std::size_t length = 0; length < built.file.size(); length++) {
        all_refused = all_refused &&
                      !decryptScePackage(std::span<const std::uint8_t>(built.file).first(length), keys, outputs, error);
    }
    CHECK(all_refused);
    CHECK(decryptScePackage(built.file, keys, outputs, error));
}

}  // namespace tox2::decrypt

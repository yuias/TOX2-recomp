// Tests for SELF to ELF reassembly on synthetic SELFs.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"
#include "sce_builder.h"
#include "self.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

using test::BuiltSce;
using test::buildSelf;
using test::fakeKeyVault;
using test::fakeSelfKey;
using test::SceBuildSection;
using test::SelfBuildOptions;
using test::SelfBuildProgramHeader;

constexpr std::uint16_t kRevision = 0x001C;

Bytes patternBytes(std::size_t size, std::uint32_t seed) {
    Bytes bytes(size);
    std::uint32_t state = seed;
    for (std::uint8_t& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

// Compresses well, so the zlib path sees a stream that is shorter than its input.
Bytes runsBytes(std::size_t size) {
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; i++) {
        bytes[i] = static_cast<std::uint8_t>(i / 7);
    }
    return bytes;
}

void copyInto(Bytes& target, std::size_t offset, const Bytes& data) {
    std::copy(data.begin(), data.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
}

Bytes slice(const Bytes& data, std::size_t offset, std::size_t size) {
    return Bytes(data.begin() + static_cast<std::ptrdiff_t>(offset),
                 data.begin() + static_cast<std::ptrdiff_t>(offset + size));
}

SceBuildSection segment(std::uint32_t program_index, Bytes plain, std::uint32_t key_index, bool compress) {
    SceBuildSection section;
    section.type = 2;
    section.program_index = program_index;
    section.plain = std::move(plain);
    section.compress = compress;
    section.key_index = key_index;
    section.iv_index = key_index + 1;
    return section;
}

SelfBuildProgramHeader loadHeader(std::uint64_t offset, std::uint64_t filesz) {
    SelfBuildProgramHeader phdr;
    phdr.offset = offset;
    phdr.filesz = filesz;
    phdr.memsz = filesz;
    return phdr;
}

// A SELF with the shape of libsre: three zlib segments with gaps between them and no section headers.
struct LibsreLike {
    SelfBuildOptions options;
    std::array<Bytes, 3> plain;
};

LibsreLike libsreLike(const SelfKey& key, std::size_t first_size) {
    LibsreLike result;
    result.plain = {runsBytes(first_size), patternBytes(0x400, 12), patternBytes(0x210, 13)};
    const std::uint64_t second_offset = 0x100 + first_size + 0x100;
    const std::uint64_t third_offset = second_offset + 0x400 + 0x100;

    SelfBuildOptions& options = result.options;
    options.sce.flags = kRevision;
    options.sce.erk = key.erk;
    options.sce.riv = key.riv;
    options.sce.key_count = 24;
    options.phdrs = {loadHeader(0x100, first_size), loadHeader(second_offset, 0x400), loadHeader(third_offset, 0x210)};
    options.phdrs[2].type = 0x700000A4;
    options.sce.sections = {segment(0, result.plain[0], 6, true), segment(1, result.plain[1], 14, true),
                            segment(2, result.plain[2], 22, true)};
    return result;
}

}  // namespace

TOX2_TEST(selfLikeEboot) {
    // The matching entry is not the first APP entry, and an ISO entry with the same revision is ignored.
    const SelfKey decoy = fakeSelfKey(KeyType::App, 0, 0, 0x001B, 1);
    const SelfKey iso = fakeSelfKey(KeyType::Iso, 0, ~std::uint64_t{0}, kRevision, 2);
    const SelfKey key = fakeSelfKey(KeyType::App, 0, 0, kRevision, 3);
    const KeyVault keys = fakeKeyVault({decoy, iso, key});

    // Segment 0 starts at file offset 0 and covers the first 0x30 bytes of the ELF header, as in the
    // game's EBOOT; the rest of the header and all program headers stay visible.
    const Bytes seg0 = patternBytes(0x30, 21);
    const Bytes seg1 = patternBytes(0x180, 22);
    const Bytes type3 = patternBytes(0x20, 23);
    const Bytes shdrs = patternBytes(4 * 0x40, 24);
    constexpr std::uint64_t kShoff = 0x1000;

    SelfBuildOptions options;
    options.sce.flags = kRevision;
    options.sce.erk = key.erk;
    options.sce.riv = key.riv;
    options.sce.key_count = 40;
    options.e_shoff = kShoff;
    options.shdrs = shdrs;
    options.phdrs = {loadHeader(0, seg0.size()), loadHeader(0x400, seg1.size()), loadHeader(0x9000, 0),
                     loadHeader(0x9000, 0), loadHeader(0x500, 4)};
    SceBuildSection plain_type3;
    plain_type3.type = 3;
    plain_type3.plain = type3;
    plain_type3.encrypt = false;
    options.sce.sections = {segment(0, seg0, 6, false), segment(1, seg1, 14, false), segment(2, {}, 22, false),
                            segment(3, {}, 30, false), plain_type3};
    const BuiltSce built = buildSelf(options);

    // Expected output: headers, then segment bytes over them, then the section headers.
    Bytes expected(kShoff + shdrs.size(), 0);
    copyInto(expected, 0, slice(built.file, 0x90, 0x40));
    copyInto(expected, 0x40, slice(built.file, 0xD0, 5 * 0x38));
    copyInto(expected, 0, seg0);
    copyInto(expected, 0x400, seg1);
    copyInto(expected, kShoff, shdrs);

    Bytes elf;
    SelfInfo info;
    std::string error;
    CHECK(decryptSelf(built.file, keys, elf, info, error));
    CHECK(elf == expected);
    CHECK(elf.size() == 0x1100);
    CHECK(info.key_revision == kRevision);
    CHECK(info.program_type == 4);
    CHECK(info.program_version == 0x0004009300000000);
    CHECK(info.section_count == 6);
    if (elf.size() >= 0x40) {
        // e_phnum, e_shnum and e_shoff from the header copy survive; the first 0x30 bytes are segment 0.
        CHECK(elf[0x39] == 5);
        CHECK(elf[0x3D] == 4);
        CHECK(slice(elf, 0, 0x30) == seg0);
    }

    // The zero-size segments at 0x9000 must not have extended the output; an existing output is replaced.
    Bytes again = {1, 2, 3};
    CHECK(decryptSelf(built.file, keys, again, info, error));
    CHECK(again == expected);
}

TOX2_TEST(selfLikeLibsre) {
    const SelfKey key = fakeSelfKey(KeyType::App, 0, 0, kRevision, 3);
    const KeyVault keys = fakeKeyVault({key});
    const std::size_t first_size = 0x3000;
    LibsreLike libsre = libsreLike(key, first_size);
    const BuiltSce built = buildSelf(libsre.options);

    const std::size_t second_offset = 0x100 + first_size + 0x100;
    const std::size_t third_offset = second_offset + 0x400 + 0x100;
    Bytes expected(third_offset + 0x210, 0);
    copyInto(expected, 0, slice(built.file, 0x90, 0x40));
    copyInto(expected, 0x40, slice(built.file, 0xD0, 3 * 0x38));
    copyInto(expected, 0x100, libsre.plain[0]);
    copyInto(expected, second_offset, libsre.plain[1]);
    copyInto(expected, third_offset, libsre.plain[2]);

    Bytes elf;
    SelfInfo info;
    std::string error;
    CHECK(decryptSelf(built.file, keys, elf, info, error));
    CHECK(elf == expected);
    CHECK(elf.size() == third_offset + 0x210);
    if (elf.size() == expected.size()) {
        // The gaps are zero: after the program headers, and between the segments.
        const auto all_zero = [&](std::size_t from, std::size_t to) {
            return std::all_of(elf.begin() + static_cast<std::ptrdiff_t>(from),
                               elf.begin() + static_cast<std::ptrdiff_t>(to), [](std::uint8_t b) { return b == 0; });
        };
        CHECK(all_zero(0x40 + 3 * 0x38, 0x100));
        CHECK(all_zero(0x100 + first_size, second_offset));
        CHECK(all_zero(second_offset + 0x400, third_offset));
    }
    CHECK(info.section_count == 3);
}

TOX2_TEST(selfRefusals) {
    const SelfKey key = fakeSelfKey(KeyType::App, 0, 0, kRevision, 3);
    const KeyVault keys = fakeKeyVault({key});
    const LibsreLike valid = libsreLike(key, 0x300);

    Bytes elf;
    SelfInfo info;
    std::string error;
    CHECK(decryptSelf(buildSelf(valid.options).file, keys, elf, info, error));

    // Builds the SELF after `change`, runs it and checks the failure and that the message mentions `text`.
    const auto refuses = [&](const char* text, auto change) {
        SelfBuildOptions options = valid.options;
        change(options);
        Bytes out = {7};
        std::string message;
        const bool ok = decryptSelf(buildSelf(options).file, keys, out, info, message);
        const bool found = message.find(text) != std::string::npos;
        if (ok || !found) {
            std::fprintf(stderr, "selfRefusals: expected \"%s\", got %s \"%s\"\n", text, ok ? "success" : "error",
                         message.c_str());
        }
        CHECK(!ok);
        CHECK(found);
    };

    refuses("debug", [](SelfBuildOptions& o) { o.sce.flags = 0x8000; });
    refuses("debug", [](SelfBuildOptions& o) { o.sce.flags = 0xC000; });
    refuses("ELF32", [](SelfBuildOptions& o) { o.elf_class = 1; });
    refuses("NPDRM", [](SelfBuildOptions& o) { o.supplemental = {1, 2, 3}; });
    refuses("NPDRM", [](SelfBuildOptions& o) { o.supplemental = {3}; });
    refuses("no key for program type 4 revision 29", [](SelfBuildOptions& o) { o.sce.flags = kRevision + 1; });
    refuses("no key for program type 5", [](SelfBuildOptions& o) { o.program_type = 5; });
    refuses("unsupported section layout", [](SelfBuildOptions& o) { o.sce.sections[1].encrypt = false; });
    refuses("unsupported section layout", [](SelfBuildOptions& o) { o.sce.sections[2].key_index = 24; });
    refuses("unsupported section layout", [](SelfBuildOptions& o) {
        SceBuildSection first;
        first.type = 1;
        first.plain = patternBytes(0x40, 30);
        first.key_index = 2;
        first.iv_index = 3;
        o.sce.sections.insert(o.sce.sections.begin(), first);
    });
    refuses("p_filesz", [](SelfBuildOptions& o) { o.phdrs[1].filesz += 1; });
    refuses("zlib", [](SelfBuildOptions& o) { o.phdrs[1].filesz -= 1; });
    refuses("program index", [](SelfBuildOptions& o) { o.sce.sections[0].program_index = 3; });
    refuses("e_shoff", [](SelfBuildOptions& o) { o.shdrs = patternBytes(0x40, 31); });

    // An encrypted type-1 section after the segments is harmless.
    {
        SelfBuildOptions options = valid.options;
        SceBuildSection later;
        later.type = 1;
        later.plain = patternBytes(0x40, 30);
        options.sce.sections.push_back(later);
        CHECK(decryptSelf(buildSelf(options).file, keys, elf, info, error));
    }

    // Truncated files fail without reading out of bounds: every proper prefix is refused.
    const BuiltSce built = buildSelf(valid.options);
    bool all_refused = true;
    for (std::size_t length = 0; length < built.file.size(); length++) {
        all_refused = all_refused &&
                      !decryptSelf(std::span<const std::uint8_t>(built.file).first(length), keys, elf, info, error);
    }
    CHECK(all_refused);
    CHECK(decryptSelf(built.file, keys, elf, info, error));

    // Header offsets that point past the end of the file.
    Bytes bad_offsets = built.file;
    bad_offsets[0x20 + 0x08 + 7] = 0xFF;  // app info offset
    bad_offsets[0x20 + 0x08 + 0] = 0x7F;
    CHECK(!decryptSelf(bad_offsets, keys, elf, info, error));
    CHECK(error.find("past the end") != std::string::npos);
}

TOX2_TEST(selfWrongKey) {
    const SelfKey key = fakeSelfKey(KeyType::App, 0, 0, kRevision, 3);
    const SelfKey other = fakeSelfKey(KeyType::App, 0, 0, kRevision, 4);
    LibsreLike libsre = libsreLike(key, 0x300);
    // The file is built with one key; the vault holds a different erk under the same revision.
    libsre.options.sce.erk = other.erk;
    libsre.options.sce.riv = other.riv;
    const BuiltSce built = buildSelf(libsre.options);

    Bytes elf;
    SelfInfo info;
    std::string error;
    CHECK(!decryptSelf(built.file, fakeKeyVault({key}), elf, info, error));
    CHECK(error.find("wrong key") != std::string::npos);
    CHECK(decryptSelf(built.file, fakeKeyVault({other}), elf, info, error));
}

}  // namespace tox2::decrypt

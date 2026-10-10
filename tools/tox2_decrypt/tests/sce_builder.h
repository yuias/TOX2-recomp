#pragma once

// Builds synthetic SCE containers by running the decryption procedure backwards with the library's own
// primitives. Every key and payload is made up.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <zlib.h>

#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"

namespace tox2::decrypt::test {

constexpr std::uint32_t kNoIndex = 0xFFFFFFFF;

struct SceBuildSection {
    std::uint32_t type = 2;
    std::uint32_t program_index = 0;
    Bytes plain;
    bool compress = false;           // zlib (RFC 1950) before encryption
    std::size_t cut_compressed = 0;  // bytes removed from the end of the compressed stream
    bool encrypt = true;             // AES-128-CTR with key_slots[key_index] and IV key_slots[iv_index]
    std::uint32_t key_index = 0;
    std::uint32_t iv_index = 1;
};

struct SceBuildOptions {
    std::uint16_t flags = 0;  // key revision; 0x8000 stores the metadata info unencrypted
    std::uint16_t type = 3;
    Bytes prefix;  // bytes between the 0x20-byte header and the metadata info; sets the metadata offset
    Key256 erk{};
    Block16 riv{};
    std::vector<SceBuildSection> sections;
    std::uint32_t key_count = 20;
    std::uint32_t optional_size = 0x30;
    // Values written into the metadata header instead of the real counts, to test bounds checks.
    std::uint32_t declared_section_count = kNoIndex;
    std::uint32_t declared_key_count = kNoIndex;
};

struct BuiltSce {
    Bytes file;
    std::vector<std::uint64_t> data_offsets;  // per section, in the file
    std::vector<std::uint64_t> data_sizes;    // stored sizes
    std::vector<Block16> key_slots;
};

namespace detail {

inline void putBe16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

inline void putBe32(Bytes& out, std::uint32_t value) {
    putBe16(out, static_cast<std::uint16_t>(value >> 16));
    putBe16(out, static_cast<std::uint16_t>(value));
}

inline void putBe64(Bytes& out, std::uint64_t value) {
    putBe32(out, static_cast<std::uint32_t>(value >> 32));
    putBe32(out, static_cast<std::uint32_t>(value));
}

inline Bytes zlibCompress(const Bytes& data) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    Bytes out(size);
    compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), Z_BEST_COMPRESSION);
    out.resize(size);
    return out;
}

}  // namespace detail

inline BuiltSce buildSce(const SceBuildOptions& options) {
    BuiltSce built;
    for (std::uint32_t i = 0; i < options.key_count; i++) {
        built.key_slots.push_back(fakeBytes<Block16>(5000 + i));
    }

    // Stored section bytes. A section whose slot index is out of range cannot be encrypted; its
    // plain bytes are stored and the reader must not use them.
    std::vector<Bytes> stored;
    for (const SceBuildSection& section : options.sections) {
        Bytes data = section.compress ? detail::zlibCompress(section.plain) : section.plain;
        data.resize(data.size() - std::min(data.size(), section.cut_compressed));
        if (section.encrypt && section.key_index < options.key_count && section.iv_index < options.key_count) {
            aes128Ctr(built.key_slots[section.key_index], built.key_slots[section.iv_index], data, data.data());
        }
        stored.push_back(std::move(data));
    }

    const std::uint32_t section_count = static_cast<std::uint32_t>(options.sections.size());
    const std::size_t region_size = 0x20 + section_count * 0x30u + options.key_count * 16u + options.optional_size;
    const std::uint64_t header_length = 0x20 + options.prefix.size() + 0x40 + region_size;

    // Data offsets are known now; build the metadata region.
    Bytes region;
    detail::putBe64(region, 0x500);
    detail::putBe32(region, 1);
    detail::putBe32(region,
                    options.declared_section_count != kNoIndex ? options.declared_section_count : section_count);
    detail::putBe32(region, options.declared_key_count != kNoIndex ? options.declared_key_count : options.key_count);
    detail::putBe32(region, options.optional_size);
    detail::putBe32(region, 0);
    detail::putBe32(region, 0);
    std::uint64_t data_offset = header_length;
    std::uint64_t data_total = 0;
    for (std::size_t i = 0; i < options.sections.size(); i++) {
        const SceBuildSection& section = options.sections[i];
        built.data_offsets.push_back(data_offset);
        built.data_sizes.push_back(stored[i].size());
        detail::putBe64(region, data_offset);
        detail::putBe64(region, stored[i].size());
        detail::putBe32(region, section.type);
        detail::putBe32(region, section.program_index);
        detail::putBe32(region, 2);  // hashed
        detail::putBe32(region, 0);  // SHA-1 slot
        detail::putBe32(region, section.encrypt ? 3 : 1);
        detail::putBe32(region, section.encrypt ? section.key_index : kNoIndex);
        detail::putBe32(region, section.encrypt ? section.iv_index : kNoIndex);
        detail::putBe32(region, section.compress ? 2 : 1);
        data_offset += stored[i].size();
        data_total += stored[i].size();
    }
    for (const Block16& slot : built.key_slots) {
        region.insert(region.end(), slot.begin(), slot.end());
    }
    region.resize(region.size() + options.optional_size, 0);

    const Block16 meta_key = fakeBytes<Block16>(6001);
    const Block16 meta_iv = fakeBytes<Block16>(6002);
    Bytes info(0x40, 0);
    std::memcpy(info.data(), meta_key.data(), 16);
    std::memcpy(info.data() + 0x20, meta_iv.data(), 16);
    if ((options.flags & 0x8000) == 0) {
        aes256CbcEncrypt(options.erk, options.riv, info, info.data());
    }
    aes128Ctr(meta_key, meta_iv, region, region.data());

    Bytes& file = built.file;
    file = {'S', 'C', 'E', 0};
    detail::putBe32(file, 2);
    detail::putBe16(file, options.flags);
    detail::putBe16(file, options.type);
    detail::putBe32(file, static_cast<std::uint32_t>(options.prefix.size()));
    detail::putBe64(file, header_length);
    detail::putBe64(file, data_total);
    file.insert(file.end(), options.prefix.begin(), options.prefix.end());
    file.insert(file.end(), info.begin(), info.end());
    file.insert(file.end(), region.begin(), region.end());
    for (const Bytes& data : stored) {
        file.insert(file.end(), data.begin(), data.end());
    }
    return built;
}

// The layout of a firmware dev_flash_* package: two plain 0x40-byte info sections, then the inner TAR
// as an encrypted zlib section (key slot 18, IV slot 19 of 20).
inline SceBuildOptions scePackageOptions(const KeyVault& keys, const Bytes& info0, const Bytes& info1,
                                         const Bytes& payload) {
    SceBuildOptions options;
    options.erk = keys.scepkg_erk;
    options.riv = keys.scepkg_riv;
    SceBuildSection first;
    first.type = 1;
    first.plain = info0;
    first.encrypt = false;
    SceBuildSection second;
    second.type = 2;
    second.plain = info1;
    second.encrypt = false;
    SceBuildSection tar;
    tar.type = 3;
    tar.plain = payload;
    tar.compress = true;
    tar.key_index = 18;
    tar.iv_index = 19;
    options.sections = {first, second, tar};
    return options;
}

inline BuiltSce buildScePackage(const KeyVault& keys, const Bytes& info0, const Bytes& info1, const Bytes& payload) {
    return buildSce(scePackageOptions(keys, info0, info1, payload));
}

struct SelfBuildProgramHeader {
    std::uint32_t type = 1;
    std::uint32_t flags = 5;
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t paddr = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
    std::uint64_t align = 0x10000;
};

struct SelfBuildOptions {
    // flags (key revision), erk/riv and the sections; `prefix` and `type` are set by buildSelf.
    SceBuildOptions sce;
    std::uint32_t program_type = 4;
    std::uint64_t program_version = 0x0004009300000000;
    std::uint8_t elf_class = 2;
    std::uint64_t e_shoff = 0;
    Bytes shdrs;  // plain section header table copy; stored as a trailing plain type-1 section
    std::vector<SelfBuildProgramHeader> phdrs;
    std::vector<std::uint32_t> supplemental = {1, 2};  // types; sizes 0x30, 0x40 and 0x90 for type 3
};

// A SELF: SCE header, extended header, app info, ELF header and program header copies, segment extended
// headers, version header and supplemental headers, then the encrypted metadata and the sections. The
// section list in `options.sce` is used as given; the section header copy, if any, is appended.
inline BuiltSce buildSelf(const SelfBuildOptions& options) {
    const std::uint32_t phnum = static_cast<std::uint32_t>(options.phdrs.size());
    const std::uint32_t shnum = static_cast<std::uint32_t>(options.shdrs.size() / 0x40);

    Bytes supplemental;
    for (std::size_t i = 0; i < options.supplemental.size(); i++) {
        const std::uint32_t type = options.supplemental[i];
        const std::uint32_t size = type == 1 ? 0x30 : (type == 2 ? 0x40 : 0x90);
        detail::putBe32(supplemental, type);
        detail::putBe32(supplemental, size);
        detail::putBe64(supplemental, i + 1 < options.supplemental.size() ? 1 : 0);
        supplemental.resize(supplemental.size() + size - 16, static_cast<std::uint8_t>(0xA0 + type));
    }

    // Offsets are absolute; the extended header starts at 0x20.
    const std::uint64_t app_info_offset = 0x70;
    const std::uint64_t elf_header_offset = app_info_offset + 0x20;
    const std::uint64_t phdr_offset = elf_header_offset + 0x40;
    const std::uint64_t segment_info_offset = phdr_offset + phnum * 0x38u;
    const std::uint64_t version_offset = segment_info_offset + phnum * 0x20u;
    const std::uint64_t supplemental_offset = version_offset + 0x10;

    Bytes prefix;
    detail::putBe64(prefix, 3);
    detail::putBe64(prefix, app_info_offset);
    detail::putBe64(prefix, elf_header_offset);
    detail::putBe64(prefix, phdr_offset);
    detail::putBe64(prefix, 0);  // section header copy offset, patched below
    detail::putBe64(prefix, segment_info_offset);
    detail::putBe64(prefix, version_offset);
    detail::putBe64(prefix, supplemental_offset);
    detail::putBe64(prefix, supplemental.size());
    detail::putBe64(prefix, 0);

    detail::putBe64(prefix, 0x1010000001000003);  // authority id
    detail::putBe32(prefix, 0x01000002);          // vendor id
    detail::putBe32(prefix, options.program_type);
    detail::putBe64(prefix, options.program_version);
    detail::putBe64(prefix, 0);

    prefix.insert(prefix.end(), {0x7F, 'E', 'L', 'F', options.elf_class, 2, 1, 0x66});
    prefix.resize(prefix.size() + 8, 0);
    detail::putBe16(prefix, 2);     // e_type
    detail::putBe16(prefix, 0x15);  // e_machine: PowerPC64
    detail::putBe32(prefix, 1);
    detail::putBe64(prefix, 0);  // e_entry
    detail::putBe64(prefix, phnum != 0 ? 0x40 : 0);
    detail::putBe64(prefix, options.e_shoff);
    detail::putBe32(prefix, 0);
    detail::putBe16(prefix, 0x40);
    detail::putBe16(prefix, 0x38);
    detail::putBe16(prefix, static_cast<std::uint16_t>(phnum));
    detail::putBe16(prefix, 0x40);
    detail::putBe16(prefix, static_cast<std::uint16_t>(shnum));
    detail::putBe16(prefix, 0);

    for (const SelfBuildProgramHeader& phdr : options.phdrs) {
        detail::putBe32(prefix, phdr.type);
        detail::putBe32(prefix, phdr.flags);
        detail::putBe64(prefix, phdr.offset);
        detail::putBe64(prefix, phdr.vaddr);
        detail::putBe64(prefix, phdr.paddr);
        detail::putBe64(prefix, phdr.filesz);
        detail::putBe64(prefix, phdr.memsz);
        detail::putBe64(prefix, phdr.align);
    }
    // Segment extended headers and the version header are never read; zeros stand in for them.
    prefix.resize(prefix.size() + phnum * 0x20u + 0x10, 0);
    prefix.insert(prefix.end(), supplemental.begin(), supplemental.end());

    SceBuildOptions sce = options.sce;
    sce.type = 1;
    sce.prefix = std::move(prefix);
    if (!options.shdrs.empty()) {
        SceBuildSection shdr_section;
        shdr_section.type = 1;
        shdr_section.plain = options.shdrs;
        shdr_section.encrypt = false;
        sce.sections.push_back(shdr_section);
    }
    BuiltSce built = buildSce(sce);
    if (!options.shdrs.empty()) {
        // The section header copy lives in section data, whose offset is only known after the build.
        Bytes offset;
        detail::putBe64(offset, built.data_offsets.back());
        std::copy(offset.begin(), offset.end(), built.file.begin() + 0x20 + 0x20);
    }
    return built;
}

}  // namespace tox2::decrypt::test

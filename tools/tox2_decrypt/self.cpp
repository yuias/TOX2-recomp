#include "self.h"

#include <cstring>

#include "sce.h"

namespace tox2::decrypt {
namespace {

constexpr std::size_t kSceHeaderSize = 0x20;
constexpr std::size_t kExtendedHeaderSize = 0x50;
constexpr std::size_t kAppInfoSize = 0x20;
constexpr std::size_t kElfHeaderSize = 0x40;
constexpr std::size_t kProgramHeaderSize = 0x38;
constexpr std::size_t kSectionHeaderSize = 0x40;
constexpr std::size_t kSupplementalMinSize = 8;
constexpr std::uint32_t kSupplementalNpdrm = 3;
constexpr std::uint32_t kSectionProgramSegment = 2;
constexpr std::uint32_t kEncryptedAesCtr = 3;
constexpr std::uint32_t kCompressedZlib = 2;
constexpr std::uint8_t kElfClass32 = 1;
// A broken header must not make the output grow without bound; the largest real input is far below this.
constexpr std::uint64_t kMaxElfSize = 0x40000000;

bool fits(std::span<const std::uint8_t> file, std::uint64_t offset, std::uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
}

// A write past the current end zero-fills the gap; an empty write changes nothing, not even the length.
bool writeAt(Bytes& elf, std::uint64_t offset, const std::uint8_t* data, std::size_t size, std::string& error) {
    if (size == 0) {
        return true;
    }
    if (offset > kMaxElfSize || size > kMaxElfSize - offset) {
        error = "SELF: output would be larger than 1 GiB";
        return false;
    }
    const std::size_t end = static_cast<std::size_t>(offset) + size;
    if (end > elf.size()) {
        elf.resize(end, 0);
    }
    std::memcpy(elf.data() + offset, data, size);
    return true;
}

}  // namespace

bool decryptSelf(std::span<const std::uint8_t> file, const KeyVault& keys, Bytes& elf, SelfInfo& info,
                 std::string& error) {
    info = SelfInfo{};
    SceHeader header;
    if (!readSceHeader(file, header, error)) {
        return false;
    }
    info.key_revision = header.flags;
    if (header.flags == 0x8000 || header.flags == 0xC000) {
        error = "debug SELF not supported";
        return false;
    }

    // Extended header, app info and the plaintext ELF header copy.
    if (!fits(file, kSceHeaderSize, kExtendedHeaderSize)) {
        error = "SELF: extended header is past the end of the file";
        return false;
    }
    const std::uint8_t* ext = file.data() + kSceHeaderSize;
    const std::uint64_t app_info_offset = readBe64(ext + 0x08);
    const std::uint64_t elf_header_offset = readBe64(ext + 0x10);
    const std::uint64_t phdr_offset = readBe64(ext + 0x18);
    const std::uint64_t shdr_offset = readBe64(ext + 0x20);
    const std::uint64_t supplemental_offset = readBe64(ext + 0x38);
    const std::uint64_t supplemental_size = readBe64(ext + 0x40);

    if (!fits(file, app_info_offset, kAppInfoSize)) {
        error = "SELF: program identification header is past the end of the file";
        return false;
    }
    info.program_type = readBe32(file.data() + app_info_offset + 0x0C);
    info.program_version = readBe64(file.data() + app_info_offset + 0x10);

    if (!fits(file, elf_header_offset, kElfHeaderSize)) {
        error = "SELF: ELF header copy is past the end of the file";
        return false;
    }
    const std::uint8_t* elf_header = file.data() + elf_header_offset;
    if (elf_header[4] == kElfClass32) {
        error = "ELF32 SELF not supported";
        return false;
    }
    const std::uint64_t e_phoff = readBe64(elf_header + 0x20);
    const std::uint64_t e_shoff = readBe64(elf_header + 0x28);
    const std::uint32_t e_phnum = readBe16(elf_header + 0x38);
    const std::uint32_t e_shnum = readBe16(elf_header + 0x3C);

    // The copy is used instead of e_phoff, but RPCS3 treats a zero offset with entries as a broken file.
    if (e_phoff == 0 && e_phnum != 0) {
        error = "SELF: ELF header has program headers but e_phoff is 0";
        return false;
    }
    const std::uint64_t phdr_bytes = static_cast<std::uint64_t>(e_phnum) * kProgramHeaderSize;
    if (!fits(file, phdr_offset, phdr_bytes)) {
        error = "SELF: program header copy is past the end of the file";
        return false;
    }
    const std::uint8_t* phdrs = file.data() + phdr_offset;

    // Supplemental headers: only the NPDRM type changes decryption, and that layer is not implemented.
    if (supplemental_size != 0) {
        if (!fits(file, supplemental_offset, supplemental_size)) {
            error = "SELF: supplemental headers are past the end of the file";
            return false;
        }
        const std::uint8_t* supplemental = file.data() + supplemental_offset;
        for (std::uint64_t pos = 0; pos < supplemental_size;) {
            if (supplemental_size - pos < kSupplementalMinSize) {
                error = "SELF: supplemental header is truncated";
                return false;
            }
            const std::uint32_t type = readBe32(supplemental + pos);
            const std::uint32_t size = readBe32(supplemental + pos + 4);
            if (type == kSupplementalNpdrm) {
                error = "NPDRM SELF not supported";
                return false;
            }
            if (size < kSupplementalMinSize || size > supplemental_size - pos) {
                error = "SELF: supplemental header has a bad size";
                return false;
            }
            pos += size;
        }
    }

    const std::uint64_t shdr_bytes = static_cast<std::uint64_t>(e_shnum) * kSectionHeaderSize;
    const bool has_shdrs = shdr_offset != 0 && e_shnum != 0;
    if (has_shdrs && !fits(file, shdr_offset, shdr_bytes)) {
        error = "SELF: section header copy is past the end of the file";
        return false;
    }

    const SelfKey* key = findSelfKey(keys, info.program_type, header.flags, info.program_version);
    if (key == nullptr) {
        error = "no key for program type " + std::to_string(info.program_type) + " revision " +
                std::to_string(header.flags);
        return false;
    }

    SceMetadata metadata;
    if (!readSceMetadata(file, header, key->erk, key->riv, metadata, error)) {
        return false;
    }
    info.section_count = static_cast<std::uint32_t>(metadata.sections.size());

    // RPCS3 decrypts only the encrypted sections into one buffer but walks it with a cursor that moves
    // for every program segment. The two agree only if every segment is encrypted and no other encrypted
    // section comes first; other layouts are refused instead of reproducing the wrong output.
    const std::size_t slot_count = metadata.key_slots.size();
    bool other_encrypted_seen = false;
    for (std::size_t i = 0; i < metadata.sections.size(); i++) {
        const SceSection& section = metadata.sections[i];
        const std::string prefix = "SELF section " + std::to_string(i) + ": ";
        if (section.type != kSectionProgramSegment) {
            other_encrypted_seen = other_encrypted_seen || section.encrypted == kEncryptedAesCtr;
            continue;
        }
        if (section.encrypted != kEncryptedAesCtr || section.key_index >= slot_count ||
            section.iv_index >= slot_count) {
            error = prefix + "unsupported section layout: program segment is not encrypted with valid key slots";
            return false;
        }
        if (other_encrypted_seen) {
            error = prefix + "unsupported section layout: an encrypted section of another type comes first";
            return false;
        }
        if (section.program_index >= e_phnum) {
            error = prefix + "program index " + std::to_string(section.program_index) + " is not below e_phnum " +
                    std::to_string(e_phnum);
            return false;
        }
        if (!fits(file, section.offset, section.size)) {
            error = prefix + "data is outside the file";
            return false;
        }
    }

    // Order matters: segment 0 of an EBOOT starts at file offset 0 and overwrites the header copies.
    Bytes out;
    if (!writeAt(out, 0, elf_header, kElfHeaderSize, error) ||
        !writeAt(out, kElfHeaderSize, phdrs, static_cast<std::size_t>(phdr_bytes), error)) {
        return false;
    }
    for (std::size_t i = 0; i < metadata.sections.size(); i++) {
        const SceSection& section = metadata.sections[i];
        if (section.type != kSectionProgramSegment) {
            continue;
        }
        const std::string prefix = "SELF section " + std::to_string(i) + ": ";
        const std::uint8_t* phdr = phdrs + static_cast<std::size_t>(section.program_index) * kProgramHeaderSize;
        const std::uint64_t p_offset = readBe64(phdr + 0x08);
        const std::uint64_t p_filesz = readBe64(phdr + 0x20);

        const std::size_t size = static_cast<std::size_t>(section.size);
        Bytes data(size);
        if (!aes128Ctr(metadata.key_slots[section.key_index], metadata.key_slots[section.iv_index],
                       file.subspan(static_cast<std::size_t>(section.offset), size), data.data())) {
            error = prefix + "AES-128-CTR decryption failed";
            return false;
        }
        if (section.compressed == kCompressedZlib) {
            if (p_filesz > kMaxElfSize) {
                error = prefix + "p_filesz is larger than 1 GiB";
                return false;
            }
            Bytes inflated;
            std::string zlib_error;
            if (!zlibInflate(data, static_cast<std::size_t>(p_filesz), inflated, zlib_error)) {
                error = prefix + zlib_error;
                return false;
            }
            if (inflated.size() != p_filesz) {
                error = prefix + "inflated to " + std::to_string(inflated.size()) + " bytes, p_filesz is " +
                        std::to_string(p_filesz);
                return false;
            }
            data = std::move(inflated);
        }
        if (!writeAt(out, p_offset, data.data(), data.size(), error)) {
            return false;
        }
    }

    if (has_shdrs) {
        if (e_shoff == 0) {
            error = "SELF: section header copy is present but e_shoff is 0";
            return false;
        }
        if (!writeAt(out, e_shoff, file.data() + shdr_offset, static_cast<std::size_t>(shdr_bytes), error)) {
            return false;
        }
    }

    elf = std::move(out);
    return true;
}

}  // namespace tox2::decrypt

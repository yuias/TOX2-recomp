#include "sce.h"

#include <cstring>
#include <limits>

namespace tox2::decrypt {
namespace {

constexpr std::size_t kSceHeaderSize = 0x20;
constexpr std::size_t kMetadataInfoSize = 0x40;
constexpr std::size_t kMetadataHeaderSize = 0x20;
constexpr std::size_t kSectionHeaderSize = 0x30;
constexpr std::uint16_t kDebugFlag = 0x8000;
constexpr std::uint32_t kEncryptedAesCtr = 3;
constexpr std::uint32_t kCompressedZlib = 2;

}  // namespace

bool readSceHeader(std::span<const std::uint8_t> file, SceHeader& header, std::string& error) {
    if (file.size() < kSceHeaderSize) {
        error = "SCE header: file is shorter than 0x20 bytes";
        return false;
    }
    if (std::memcmp(file.data(), "SCE\0", 4) != 0) {
        error = "SCE header: bad magic";
        return false;
    }
    header.version = readBe32(file.data() + 4);
    header.flags = readBe16(file.data() + 8);
    header.type = readBe16(file.data() + 0xA);
    header.metadata_offset = readBe32(file.data() + 0xC);
    header.header_length = readBe64(file.data() + 0x10);
    header.data_length = readBe64(file.data() + 0x18);
    return true;
}

bool readSceMetadata(std::span<const std::uint8_t> file, const SceHeader& header, const Key256& erk, const Block16& riv,
                     SceMetadata& metadata, std::string& error) {
    // The info block is at metadata_offset + 0x20; the encrypted region follows it and ends at the
    // header length. The region must at least hold the 0x20-byte metadata header.
    const std::uint64_t info_offset = static_cast<std::uint64_t>(header.metadata_offset) + kSceHeaderSize;
    const std::uint64_t region_offset = info_offset + kMetadataInfoSize;
    if (header.header_length < region_offset + kMetadataHeaderSize) {
        error = "SCE metadata: header length is too small for the metadata";
        return false;
    }
    if (header.header_length > file.size()) {
        error = "SCE metadata: header length is past the end of the file";
        return false;
    }

    std::uint8_t info[kMetadataInfoSize];
    if ((header.flags & kDebugFlag) != 0) {
        std::memcpy(info, file.data() + info_offset, kMetadataInfoSize);
    } else if (!aes256CbcDecrypt(erk, riv, file.subspan(info_offset, kMetadataInfoSize), info)) {
        error = "SCE metadata: AES-256-CBC decryption failed";
        return false;
    }
    // The padding bytes are the only key check there is.
    if (info[0x10] != 0 || info[0x30] != 0) {
        error = "wrong key: metadata info does not decrypt";
        return false;
    }

    Block16 region_key;
    Block16 region_iv;
    std::memcpy(region_key.data(), info, 16);
    std::memcpy(region_iv.data(), info + 0x20, 16);

    const std::size_t region_size = static_cast<std::size_t>(header.header_length - region_offset);
    Bytes region(region_size);
    if (!aes128Ctr(region_key, region_iv, file.subspan(region_offset, region_size), region.data())) {
        error = "SCE metadata: AES-128-CTR decryption failed";
        return false;
    }

    const std::uint32_t section_count = readBe32(region.data() + 0xC);
    const std::uint32_t key_count = readBe32(region.data() + 0x10);
    const std::uint64_t slots_offset =
            kMetadataHeaderSize + static_cast<std::uint64_t>(section_count) * kSectionHeaderSize;
    const std::uint64_t needed = slots_offset + static_cast<std::uint64_t>(key_count) * 16;
    if (needed > region_size) {
        error = "SCE metadata: " + std::to_string(section_count) + " sections and " + std::to_string(key_count) +
                " key slots do not fit in the metadata region";
        return false;
    }

    SceMetadata parsed;
    parsed.sections.reserve(section_count);
    for (std::uint32_t i = 0; i < section_count; i++) {
        const std::uint8_t* p = region.data() + kMetadataHeaderSize + static_cast<std::size_t>(i) * kSectionHeaderSize;
        SceSection section;
        section.offset = readBe64(p);
        section.size = readBe64(p + 8);
        section.type = readBe32(p + 0x10);
        section.program_index = readBe32(p + 0x14);
        section.encrypted = readBe32(p + 0x20);
        section.key_index = readBe32(p + 0x24);
        section.iv_index = readBe32(p + 0x28);
        section.compressed = readBe32(p + 0x2C);
        parsed.sections.push_back(section);
    }
    parsed.key_slots.resize(key_count);
    if (key_count != 0) {
        std::memcpy(parsed.key_slots.data(), region.data() + slots_offset, static_cast<std::size_t>(key_count) * 16);
    }
    metadata = std::move(parsed);
    return true;
}

bool decryptScePackage(std::span<const std::uint8_t> file, const KeyVault& keys, std::vector<Bytes>& outputs,
                       std::string& error) {
    SceHeader header;
    SceMetadata metadata;
    if (!readSceHeader(file, header, error) ||
        !readSceMetadata(file, header, keys.scepkg_erk, keys.scepkg_riv, metadata, error)) {
        return false;
    }

    std::vector<Bytes> result;
    result.reserve(metadata.sections.size());
    for (std::size_t i = 0; i < metadata.sections.size(); i++) {
        const SceSection& section = metadata.sections[i];
        const std::string prefix = "SCE package section " + std::to_string(i) + ": ";
        if (section.offset > file.size() || section.size > file.size() - section.offset) {
            error = prefix + "data is outside the file";
            return false;
        }
        const std::size_t size = static_cast<std::size_t>(section.size);
        const auto stored = file.subspan(static_cast<std::size_t>(section.offset), size);

        Bytes data(size);
        if (section.encrypted != kEncryptedAesCtr) {
            std::memcpy(data.data(), stored.data(), size);
        } else if (section.key_index < metadata.key_slots.size() && section.iv_index < metadata.key_slots.size()) {
            if (!aes128Ctr(metadata.key_slots[section.key_index], metadata.key_slots[section.iv_index], stored,
                           data.data())) {
                error = prefix + "AES-128-CTR decryption failed";
                return false;
            }
        }  // else: out-of-range slots leave the section as zeros, as RPCS3 does.

        if (section.compressed == kCompressedZlib) {
            Bytes inflated;
            std::string zlib_error;
            if (!zlibInflate(data, std::numeric_limits<std::size_t>::max(), inflated, zlib_error)) {
                error = prefix + zlib_error;
                return false;
            }
            data = std::move(inflated);
        }
        result.push_back(std::move(data));
    }
    outputs = std::move(result);
    return true;
}

}  // namespace tox2::decrypt

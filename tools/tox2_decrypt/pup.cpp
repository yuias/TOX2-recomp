#include "pup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "sce.h"

namespace tox2::decrypt {
namespace {

constexpr std::size_t kPupHeaderSize = 0x30;
constexpr std::size_t kPupTableEntrySize = 0x20;
constexpr std::uint64_t kFirmwareEntryId = 0x100;
constexpr std::uint64_t kOuterTarEntryId = 0x300;
constexpr std::size_t kTarBlockSize = 512;
constexpr std::size_t kTarNameSize = 100;
constexpr std::size_t kTarPrefixOffset = 0x159;
constexpr std::size_t kTarPrefixSize = 155;
constexpr std::size_t kTarSizeOffset = 0x7C;
constexpr std::size_t kTarSizeFieldSize = 12;
constexpr std::size_t kTarTypeOffset = 0x9C;
constexpr std::size_t kTarMagicOffset = 0x101;
// The third output of a package holds the inner TAR.
constexpr std::size_t kPackageTarOutput = 2;

std::string hexId(std::uint64_t id) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(id));
    return text;
}

// Up to the first NUL, or the whole field.
std::string fieldString(const std::uint8_t* field, std::size_t size) {
    const void* nul = std::memchr(field, 0, size);
    const std::size_t length =
            nul != nullptr ? static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - field) : size;
    return std::string(reinterpret_cast<const char*>(field), length);
}

bool isRegularFile(char type_flag) { return type_flag == '0' || type_flag == '\0'; }

}  // namespace

bool readPupEntry(std::span<const std::uint8_t> pup, const KeyVault& keys, std::uint64_t id,
                  std::span<const std::uint8_t>& data, std::string& error) {
    if (pup.size() < kPupHeaderSize) {
        error = "PUP: file is shorter than the 0x30-byte header";
        return false;
    }
    if (std::memcmp(pup.data(), "SCEUF\0\0\0", 8) != 0) {
        error = "PUP: bad magic";
        return false;
    }
    const std::uint64_t count = readBe64(pup.data() + 0x18);
    const std::uint64_t header_length = readBe64(pup.data() + 0x20);
    const std::uint64_t data_length = readBe64(pup.data() + 0x28);
    if (header_length > pup.size() || data_length > pup.size() - header_length) {
        error = "PUP: file is smaller than its header says";
        return false;
    }
    // The file table and the hash table have one entry per file each and follow the header.
    if (count == 0) {
        error = "PUP: no files";
        return false;
    }
    if (count > (pup.size() - kPupHeaderSize) / (2 * kPupTableEntrySize)) {
        error = "PUP: file and hash tables do not fit in the file";
        return false;
    }

    const std::uint8_t* files = pup.data() + kPupHeaderSize;
    const std::uint8_t* hashes = files + count * kPupTableEntrySize;
    std::uint64_t found = count;
    for (std::uint64_t i = 0; i < count; i++) {
        const std::uint8_t* entry = files + i * kPupTableEntrySize;
        const std::uint64_t offset = readBe64(entry + 8);
        const std::uint64_t length = readBe64(entry + 0x10);
        if (offset > pup.size() || length > pup.size() - offset) {
            error = "PUP: entry " + hexId(readBe64(entry)) + " is outside the file";
            return false;
        }
        if (found == count && readBe64(entry) == id) {
            found = i;
        }
    }
    if (found == count) {
        error = "PUP: no entry " + hexId(id);
        return false;
    }

    const std::uint8_t* entry = files + found * kPupTableEntrySize;
    const auto content = pup.subspan(static_cast<std::size_t>(readBe64(entry + 8)),
                                     static_cast<std::size_t>(readBe64(entry + 0x10)));
    Sha1Digest digest;
    if (!hmacSha1(keys.pup_key, content, digest)) {
        error = "PUP: HMAC-SHA1 failed";
        return false;
    }
    if (std::memcmp(digest.data(), hashes + found * kPupTableEntrySize + 8, digest.size()) != 0) {
        error = "PUP: entry " + hexId(id) + " does not match its hash (damaged download, or wrong key)";
        return false;
    }
    data = content;
    return true;
}

bool readPupFirmware(std::span<const std::uint8_t> pup, const KeyVault& keys, std::string& version,
                     std::string& error) {
    std::span<const std::uint8_t> data;
    if (!readPupEntry(pup, keys, kFirmwareEntryId, data, error)) {
        return false;
    }
    const auto newline = std::find(data.begin(), data.end(), static_cast<std::uint8_t>('\n'));
    std::string text(data.begin(), newline);
    if (!text.empty() && text.back() == '\r') {
        text.pop_back();
    }
    if (text.empty()) {
        error = "PUP: entry 0x100 holds no version text";
        return false;
    }
    version = std::move(text);
    return true;
}

std::vector<TarEntry> scanTar(std::span<const std::uint8_t> archive) {
    // Keyed by path: a later duplicate replaces an earlier one and the result comes out sorted.
    std::map<std::string, TarEntry> entries;
    std::uint64_t offset = 0;
    while (offset < archive.size()) {
        if (archive.size() - offset < kTarBlockSize) {
            break;
        }
        const std::uint8_t* block = archive.data() + offset;
        offset += kTarBlockSize;
        if (std::memcmp(block + kTarMagicOffset, "ustar", 5) != 0) {
            continue;
        }

        // Octal digits, then a NUL or a space inside the 12-byte field.
        const std::uint8_t* size_field = block + kTarSizeOffset;
        std::uint64_t size = 0;
        std::size_t digits = 0;
        while (digits < kTarSizeFieldSize && size_field[digits] >= '0' && size_field[digits] <= '7') {
            size = size * 8 + (size_field[digits] - '0');
            digits++;
        }
        if (digits == 0 || digits == kTarSizeFieldSize || (size_field[digits] != 0 && size_field[digits] != ' ')) {
            continue;
        }

        const std::string name = fieldString(block, kTarNameSize);
        const std::string prefix = fieldString(block + kTarPrefixOffset, kTarPrefixSize);
        if ((name.empty() && prefix.empty()) || size > archive.size() - offset) {
            continue;
        }

        TarEntry entry;
        entry.path = prefix + name;
        entry.type_flag = static_cast<char>(block[kTarTypeOffset]);
        entry.offset = offset;
        entry.size = size;
        entries[entry.path] = entry;
        // Entry data is padded to whole blocks.
        offset += (size + kTarBlockSize - 1) / kTarBlockSize * kTarBlockSize;
    }

    std::vector<TarEntry> result;
    result.reserve(entries.size());
    for (auto& [path, entry] : entries) {
        result.push_back(std::move(entry));
    }
    return result;
}

bool extractFromPup(std::span<const std::uint8_t> pup, const KeyVault& keys, std::string_view path, PupExtract& result,
                    std::string& error) {
    std::span<const std::uint8_t> outer;
    if (!readPupEntry(pup, keys, kOuterTarEntryId, outer, error)) {
        return false;
    }
    // "dev_flash3_" packages do not contain this substring and are not searched.
    for (const TarEntry& package : scanTar(outer)) {
        if (!isRegularFile(package.type_flag) || package.path.find("dev_flash_") == std::string::npos) {
            continue;
        }
        const std::string prefix = "package " + package.path + ": ";
        std::vector<Bytes> outputs;
        std::string package_error;
        if (!decryptScePackage(
                    outer.subspan(static_cast<std::size_t>(package.offset), static_cast<std::size_t>(package.size)),
                    keys, outputs, package_error)) {
            error = prefix + package_error;
            return false;
        }
        if (outputs.size() <= kPackageTarOutput) {
            error = prefix + "has " + std::to_string(outputs.size()) + " sections, expected at least 3";
            return false;
        }
        const Bytes& archive = outputs[kPackageTarOutput];
        for (const TarEntry& entry : scanTar(archive)) {
            if (isRegularFile(entry.type_flag) && entry.path == path) {
                result.package = package.path;
                result.data.assign(archive.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                                   archive.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.size));
                return true;
            }
        }
    }
    error = "PUP: " + std::string(path) + " not found in any dev_flash_ package";
    return false;
}

}  // namespace tox2::decrypt

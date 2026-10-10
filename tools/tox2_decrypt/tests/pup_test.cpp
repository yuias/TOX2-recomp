// Tests for PUP entries, the TAR scan and the dev_flash package search on synthetic inputs.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"
#include "pup.h"
#include "pup_builder.h"
#include "sce_builder.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

using test::buildPup;
using test::buildScePackage;
using test::buildTar;
using test::fakeKeyVault;
using test::PupBuildEntry;
using test::TarBuildEntry;

Bytes patternBytes(std::size_t size, std::uint32_t seed) {
    Bytes bytes(size);
    std::uint32_t state = seed;
    for (std::uint8_t& byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

Bytes textBytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

void putBe64At(Bytes& file, std::size_t offset, std::uint64_t value) {
    for (int i = 0; i < 8; i++) {
        file[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    }
}

TarBuildEntry tarFile(const std::string& name, Bytes data) {
    TarBuildEntry entry;
    entry.name = name;
    entry.data = std::move(data);
    return entry;
}

TarBuildEntry tarDirectory(const std::string& name) {
    TarBuildEntry entry;
    entry.name = name;
    entry.type_flag = '5';
    return entry;
}

const TarEntry* findPath(const std::vector<TarEntry>& entries, const std::string& path) {
    for (const TarEntry& entry : entries) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

Bytes entryData(const Bytes& archive, const TarEntry& entry) {
    return Bytes(archive.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                 archive.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.size));
}

constexpr std::size_t kFileTableOffset = 0x30;

}  // namespace

TOX2_TEST(tarScan) {
    const Bytes libsre = patternBytes(1000, 1);  // not a multiple of 512

    TarBuildEntry with_prefix = tarFile("libsre.sprx", libsre);
    with_prefix.prefix = "dev_flash/sys/external/";

    TarBuildEntry zero_block;
    zero_block.raw = Bytes(512, 0);
    // Random bytes that are not a header: the magic area is overwritten so that it cannot match by chance.
    TarBuildEntry garbage;
    garbage.raw = patternBytes(512, 2);
    std::fill(garbage.raw.begin() + 0x101, garbage.raw.begin() + 0x107, 'X');

    TarBuildEntry space = tarFile("space.txt", textBytes("abcde"));
    space.size_terminator = ' ';
    TarBuildEntry bad_digits = tarFile("bad_size.txt", patternBytes(10, 3));
    bad_digits.size_field = std::string("zzzzzzzzzzz\0", 12);
    TarBuildEntry no_terminator = tarFile("twelve.txt", {});
    no_terminator.size_field = "777777777777";
    TarBuildEntry no_digits = tarFile("empty_size.txt", {});
    no_digits.size_field = std::string("\0\0\0\0\0\0\0\0\0\0\0\0", 12);
    TarBuildEntry nameless = tarFile("", textBytes("xyz"));
    TarBuildEntry nul_type = tarFile("nul_type.txt", textBytes("data"));
    nul_type.type_flag = '\0';
    TarBuildEntry plain_magic = tarFile("no_space_magic.txt", textBytes("m"));
    plain_magic.magic = std::string(
            "ustar\0"
            "00",
            8);
    TarBuildEntry not_ustar = tarFile("not_ustar.txt", textBytes("n"));
    not_ustar.magic = "posix ";
    TarBuildEntry too_long = tarFile("too_long.bin", patternBytes(100, 4));
    too_long.size_field = std::string("00000010000\0", 12);  // 4096 bytes, but fewer follow

    const Bytes archive =
            buildTar({with_prefix, zero_block, garbage, space, bad_digits, no_terminator, no_digits, nameless,
                      tarDirectory("dir/"), tarFile("dup.txt", textBytes("first")),
                      tarFile("dup.txt", textBytes("second!")), nul_type, plain_magic, not_ustar, too_long});
    const std::vector<TarEntry> entries = scanTar(archive);

    std::vector<std::string> paths;
    for (const TarEntry& entry : entries) {
        paths.push_back(entry.path);
    }
    const std::vector<std::string> expected_paths = {
            "dev_flash/sys/external/libsre.sprx", "dir/", "dup.txt", "no_space_magic.txt", "nul_type.txt", "space.txt"};
    CHECK(paths == expected_paths);

    // Prefix and name are joined without a separator; the data starts after the 512-byte header.
    const TarEntry* found = findPath(entries, "dev_flash/sys/external/libsre.sprx");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->offset == 512);
        CHECK(found->size == 1000);
        CHECK(found->type_flag == '0');
        CHECK(entryData(archive, *found) == libsre);
    }
    // The next entry sits after the data rounded up to 1024, the zero block and the garbage block.
    found = findPath(entries, "space.txt");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->offset == 512 + 1024 + 512 + 512 + 512);
        CHECK(entryData(archive, *found) == textBytes("abcde"));
    }
    found = findPath(entries, "dir/");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->type_flag == '5');
        CHECK(found->size == 0);
    }
    found = findPath(entries, "dup.txt");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(entryData(archive, *found) == textBytes("second!"));
    }
    found = findPath(entries, "nul_type.txt");
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->type_flag == '\0');
    }

    CHECK(scanTar({}).empty());
    CHECK(scanTar(std::span<const std::uint8_t>(archive).first(300)).empty());
}

TOX2_TEST(pupEntryHmac) {
    const KeyVault keys = fakeKeyVault({});
    const Bytes version = textBytes("4.93\n");
    const Bytes middle = patternBytes(100, 1);
    const Bytes last = patternBytes(5000, 2);
    const Bytes pup = buildPup(keys, {{0x100, version}, {0x200, middle}, {0x300, last}});

    std::span<const std::uint8_t> data;
    std::string error;
    CHECK(readPupEntry(pup, keys, 0x300, data, error));
    CHECK(Bytes(data.begin(), data.end()) == last);
    CHECK(readPupEntry(pup, keys, 0x200, data, error));
    CHECK(Bytes(data.begin(), data.end()) == middle);
    CHECK(!readPupEntry(pup, keys, 0x999, data, error));
    CHECK(error.find("0x999") != std::string::npos);

    // One flipped data byte: only the damaged entry is refused.
    Bytes flipped = pup;
    flipped[flipped.size() - 1] ^= 0x01;
    CHECK(!readPupEntry(flipped, keys, 0x300, data, error));
    CHECK(error.find("hash") != std::string::npos);
    CHECK(readPupEntry(flipped, keys, 0x200, data, error));

    KeyVault other_key = keys;
    other_key.pup_key[3] ^= 0x10;
    CHECK(!readPupEntry(pup, other_key, 0x300, data, error));

    // An entry that leaves the file, even one that is not asked for.
    Bytes outside = pup;
    putBe64At(outside, kFileTableOffset + 0x20 + 0x10, 0x10000000);  // length of entry 1
    CHECK(!readPupEntry(outside, keys, 0x300, data, error));
    CHECK(error.find("outside") != std::string::npos);
    outside = pup;
    putBe64At(outside, kFileTableOffset + 2 * 0x20 + 0x08, pup.size() + 1);  // offset of entry 2
    CHECK(!readPupEntry(outside, keys, 0x100, data, error));

    Bytes bad_magic = pup;
    bad_magic[2] = 'X';
    CHECK(!readPupEntry(bad_magic, keys, 0x300, data, error));
    CHECK(error.find("magic") != std::string::npos);

    // Shorter than the header, and shorter than the header length plus the data length.
    CHECK(!readPupEntry(std::span<const std::uint8_t>(pup).first(0x2F), keys, 0x300, data, error));
    CHECK(!readPupEntry(std::span<const std::uint8_t>(pup).first(pup.size() - 1), keys, 0x300, data, error));
    Bytes long_header = pup;
    putBe64At(long_header, 0x20, pup.size() + 1);
    CHECK(!readPupEntry(long_header, keys, 0x300, data, error));

    // No files, and a file count whose tables cannot fit.
    Bytes no_files = pup;
    putBe64At(no_files, 0x18, 0);
    CHECK(!readPupEntry(no_files, keys, 0x300, data, error));
    Bytes many_files = pup;
    putBe64At(many_files, 0x18, 0x0FFFFFFFFFFFFFFF);
    CHECK(!readPupEntry(many_files, keys, 0x300, data, error));

    // Every proper prefix is refused.
    bool all_refused = true;
    for (std::size_t length = 0; length < pup.size(); length++) {
        all_refused = all_refused &&
                      !readPupEntry(std::span<const std::uint8_t>(pup).first(length), keys, 0x300, data, error);
    }
    CHECK(all_refused);

    // The first of two entries with the same id is used.
    const Bytes twice = buildPup(keys, {{0x300, textBytes("first")}, {0x300, textBytes("second")}});
    CHECK(readPupEntry(twice, keys, 0x300, data, error));
    CHECK(Bytes(data.begin(), data.end()) == textBytes("first"));
}

TOX2_TEST(pupFirmware) {
    const KeyVault keys = fakeKeyVault({});
    std::string version;
    std::string error;
    for (const char* text : {"4.93\n", "4.93\r\n", "4.93", "4.93\nsecond line\n", "4.93\r\nsecond\r\n"}) {
        version.clear();
        CHECK(readPupFirmware(buildPup(keys, {{0x100, textBytes(text)}, {0x300, patternBytes(10, 1)}}), keys, version,
                              error));
        CHECK(version == "4.93");
    }

    // Only one trailing CR is removed, and only at the line end.
    CHECK(readPupFirmware(buildPup(keys, {{0x100, textBytes("4.9\r3\r\n")}}), keys, version, error));
    CHECK(version == "4.9\r3");

    CHECK(!readPupFirmware(buildPup(keys, {{0x300, patternBytes(10, 1)}}), keys, version, error));
    CHECK(!readPupFirmware(buildPup(keys, {{0x100, textBytes("\n")}}), keys, version, error));
}

TOX2_TEST(pupExtract) {
    const KeyVault keys = fakeKeyVault({});
    const Bytes info0 = patternBytes(0x40, 1);
    const Bytes info1 = patternBytes(0x40, 2);
    const std::string wanted = "dev_flash/sys/external/libsre.sprx";
    const Bytes wanted_data = patternBytes(777, 10);

    // 000: look-alikes of the wanted path (a directory, a longer name). 001: the file. 002: another copy
    // that must not win. dev_flash3_023: not searched.
    const Bytes tar0 =
            buildTar({tarDirectory("dev_flash/"), tarDirectory(wanted), tarFile(wanted + ".bak", patternBytes(50, 11)),
                      tarFile("dev_flash/etc/other.bin", patternBytes(60, 12))});
    const Bytes tar1 = buildTar({tarDirectory("dev_flash/"), tarFile(wanted, wanted_data)});
    const Bytes tar2 = buildTar({tarFile(wanted, patternBytes(777, 13))});
    const Bytes tar3 =
            buildTar({tarFile("dev_flash/only3.sprx", patternBytes(70, 14)), tarFile(wanted, patternBytes(5, 15))});
    const std::string name0 = "dev_flash_000.tar.aa.2026_01_08_191610";
    const std::string name1 = "dev_flash_001.tar.aa.2026_01_08_191610";
    const std::string name3 = "dev_flash3_023.tar.aa.2026_01_08_191651";

    const auto package = [&](const Bytes& tar) { return buildScePackage(keys, info0, info1, tar).file; };
    TarBuildEntry named2 = tarFile("dev_flash_002.tar.aa.2026_01_08_191610", package(tar2));
    named2.prefix = "pkgs/";  // the substring may appear anywhere in the path
    // Out of name order on purpose: the search order is the sorted order, not the archive order.
    const Bytes outer = buildTar({named2, tarFile(name1, package(tar1)), tarFile("RL_FOR_X.img", patternBytes(900, 16)),
                                  tarFile(name3, package(tar3)), tarFile(name0, package(tar0))});
    const Bytes pup = buildPup(keys, {{0x100, textBytes("4.93\n")}, {0x200, patternBytes(64, 17)}, {0x300, outer}});

    PupExtract result;
    std::string error;
    CHECK(extractFromPup(pup, keys, wanted, result, error));
    CHECK(result.package == name1);
    CHECK(result.data == wanted_data);

    // A path that exists only in the dev_flash3_ package, and one that exists nowhere.
    CHECK(!extractFromPup(pup, keys, "dev_flash/only3.sprx", result, error));
    CHECK(error.find("dev_flash/only3.sprx") != std::string::npos);
    CHECK(!extractFromPup(pup, keys, "dev_flash/nothing.sprx", result, error));
    CHECK(error.find("dev_flash/nothing.sprx") != std::string::npos);

    // A path that only names a directory.
    CHECK(!extractFromPup(pup, keys, "dev_flash/", result, error));

    // A package that does not decrypt is an error that names it; the search does not skip it.
    Bytes broken = package(tar0);
    broken[1] = 'X';
    const Bytes outer_broken = buildTar({tarFile(name0, broken), tarFile(name1, package(tar1))});
    const Bytes pup_broken = buildPup(keys, {{0x300, outer_broken}});
    CHECK(!extractFromPup(pup_broken, keys, wanted, result, error));
    CHECK(error.find(name0) != std::string::npos);

    // A package with fewer than three sections.
    test::SceBuildOptions options = test::scePackageOptions(keys, info0, info1, tar1);
    options.sections.pop_back();
    const Bytes outer_short = buildTar({tarFile(name0, test::buildSce(options).file)});
    CHECK(!extractFromPup(buildPup(keys, {{0x300, outer_short}}), keys, wanted, result, error));
    CHECK(error.find("sections") != std::string::npos);

    // The wrong package key.
    KeyVault wrong = keys;
    wrong.scepkg_erk[0] ^= 0x01;
    CHECK(!extractFromPup(pup, wrong, wanted, result, error));
    CHECK(error.find("wrong key") != std::string::npos);

    // A damaged outer TAR entry is caught by the HMAC before any package is read.
    Bytes damaged = pup;
    damaged[damaged.size() - 5] ^= 0x01;
    CHECK(!extractFromPup(damaged, keys, wanted, result, error));
    CHECK(error.find("hash") != std::string::npos);

    // A PUP without entry 0x300.
    CHECK(!extractFromPup(buildPup(keys, {{0x100, textBytes("4.93\n")}}), keys, wanted, result, error));
}

}  // namespace tox2::decrypt

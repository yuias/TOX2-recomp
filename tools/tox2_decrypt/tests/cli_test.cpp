// Tests for the command line on synthetic key files, a SELF and a PUP in a temporary directory.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "crypto.h"
#include "key_vault_text.h"
#include "keys.h"
#include "pup_builder.h"
#include "sce_builder.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

namespace fs = std::filesystem;

using test::BuiltSce;
using test::buildPup;
using test::buildScePackage;
using test::buildSelf;
using test::buildTar;
using test::fakeKeyVault;
using test::fakeKeyVaultHeader;
using test::fakeKeyVaultSource;
using test::fakeSelfKey;
using test::FakeHeaderOptions;
using test::SceBuildSection;
using test::SelfBuildOptions;
using test::TarBuildEntry;

constexpr std::uint16_t kRevision = 0x001C;
constexpr const char* kLibsrePath = "dev_flash/sys/external/libsre.sprx";
constexpr std::size_t kSegmentOffset = 0x100;
constexpr std::size_t kSegmentSize = 0x300;

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

void writeFile(const fs::path& path, const Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void writeText(const fs::path& path, const std::string& text) { writeFile(path, textBytes(text)); }

Bytes readBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string sha256Hex(const Bytes& data) {
    Sha256Digest digest;
    sha256(data, digest);
    return toHex(digest);
}

struct Result {
    int code = 0;
    std::string out;
    std::string err;
};

Result run(const std::vector<std::string>& args) {
    const std::vector<std::string_view> views(args.begin(), args.end());
    Result result;
    result.code = runCli(views, result.out, result.err);
    return result;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string flipLastDigit(std::string hex) {
    hex.back() = hex.back() == '0' ? '1' : '0';
    return hex;
}

// A fresh directory with the fake key files, a SELF (the "EBOOT.BIN") and a PUP whose libsre package
// holds the same SELF. Removed on destruction.
struct Fixture {
    fs::path dir;
    KeyVault vault;
    Bytes self_file;
    Bytes expected_elf;
    Bytes pup_file;

    Fixture() {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path() /
              ("tox2_decrypt_cli_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "_" + std::to_string(counter++));
        fs::remove_all(dir);
        fs::create_directories(dir);

        const SelfKey app = fakeSelfKey(KeyType::App, 0, 0, kRevision, 3);
        vault = fakeKeyVault({fakeSelfKey(KeyType::Lv0, 0, 0, 0, 1), fakeSelfKey(KeyType::Lv2, 1, 5, 0, 2),
                              fakeSelfKey(KeyType::Lv2, 6, 9, 0, 4), fakeSelfKey(KeyType::App, 0, 0, 0x1B, 5), app,
                              fakeSelfKey(KeyType::App, 0, 0, 0x1D, 6), fakeSelfKey(KeyType::Npdrm, 0, 0, 1, 7)});
        writeKeys(dir / "keys", vault);

        // One plain segment at 0x100, so the expected ELF is easy to write down independently.
        const Bytes segment = patternBytes(kSegmentSize, 31);
        SelfBuildOptions options;
        options.sce.flags = kRevision;
        options.sce.erk = app.erk;
        options.sce.riv = app.riv;
        test::SelfBuildProgramHeader phdr;
        phdr.offset = kSegmentOffset;
        phdr.filesz = kSegmentSize;
        phdr.memsz = kSegmentSize;
        options.phdrs = {phdr};
        SceBuildSection section;
        section.type = 2;
        section.plain = segment;
        section.key_index = 6;
        section.iv_index = 7;
        options.sce.sections = {section};
        const BuiltSce built = buildSelf(options);
        self_file = built.file;

        expected_elf.assign(kSegmentOffset + kSegmentSize, 0);
        std::copy_n(built.file.begin() + 0x90, 0x40, expected_elf.begin());
        std::copy_n(built.file.begin() + 0xD0, 0x38, expected_elf.begin() + 0x40);
        std::copy(segment.begin(), segment.end(), expected_elf.begin() + kSegmentOffset);
        writeFile(dir / "EBOOT.BIN", self_file);

        const Bytes inner = buildTar({tarEntry("dev_flash/", {}, '5'), tarEntry(kLibsrePath, self_file, '0')});
        const Bytes package = buildScePackage(vault, patternBytes(0x40, 1), patternBytes(0x40, 2), inner).file;
        const Bytes outer = buildTar({tarEntry("dev_flash_000.tar.aa.2026_01_08_191610", package, '0')});
        pup_file = buildPup(vault, {{0x100, textBytes("4.93\n")}, {0x300, outer}});
        writeFile(dir / "PS3UPDAT.PUP", pup_file);
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    static TarBuildEntry tarEntry(const std::string& name, Bytes data, char type_flag) {
        TarBuildEntry entry;
        entry.name = name;
        entry.data = std::move(data);
        entry.type_flag = type_flag;
        return entry;
    }

    static void writeKeys(const fs::path& keys_dir, const KeyVault& keys, const FakeHeaderOptions& header = {}) {
        writeText(keys_dir / "key_vault.h", fakeKeyVaultHeader(keys, header));
        writeText(keys_dir / "key_vault.cpp", fakeKeyVaultSource(keys.self_keys));
    }

    std::string path(const char* name) const { return (dir / name).string(); }

    std::vector<std::string> selfArgs(const std::string& out_name) const {
        return {"self",
                "--keys",
                path("keys"),
                "--in",
                path("EBOOT.BIN"),
                "--out",
                path(out_name.c_str()),
                "--expect-size",
                std::to_string(expected_elf.size()),
                "--expect-sha256",
                sha256Hex(expected_elf)};
    }

    std::vector<std::string> pupArgs(const std::string& out_name, const std::string& firmware = "4.93") const {
        return {"pup",
                "--keys",
                path("keys"),
                "--in",
                path("PS3UPDAT.PUP"),
                "--path",
                kLibsrePath,
                "--firmware",
                firmware,
                "--out",
                path(out_name.c_str()),
                "--expect-size",
                std::to_string(expected_elf.size()),
                "--expect-sha256",
                sha256Hex(expected_elf)};
    }
};

// Replaces the value that follows `option`.
void setOption(std::vector<std::string>& args, const std::string& option, const std::string& value) {
    const auto found = std::find(args.begin(), args.end(), option);
    if (found != args.end() && found + 1 != args.end()) {
        *(found + 1) = value;
    }
}

bool startsWithError(const std::string& err) { return err.rfind("error: ", 0) == 0; }

}  // namespace

TOX2_TEST(cliSelfOk) {
    const Fixture fixture;
    std::vector<std::string> args = fixture.selfArgs("outdir/EBOOT.elf");
    args.insert(args.end(), {"--expect-input-sha256", sha256Hex(fixture.self_file)});

    const Result result = run(args);
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(result.out == "EBOOT.elf: " + std::to_string(fixture.expected_elf.size()) + " bytes, sha256 " +
                                sha256Hex(fixture.expected_elf) + ": matches the pin\n");
    CHECK(readBytes(fixture.dir / "outdir" / "EBOOT.elf") == fixture.expected_elf);
    CHECK(!fs::exists(fixture.dir / "outdir" / "EBOOT.elf.part"));

    // A pre-existing output is replaced, and an upper-case pin is accepted.
    writeText(fixture.dir / "outdir" / "EBOOT.elf", "stale");
    std::string upper = sha256Hex(fixture.expected_elf);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return static_cast<char>(std::toupper(c)); });
    setOption(args, "--expect-sha256", upper);
    CHECK(run(args).code == 0);
    CHECK(readBytes(fixture.dir / "outdir" / "EBOOT.elf") == fixture.expected_elf);
}

TOX2_TEST(cliSelfMismatch) {
    const Fixture fixture;
    std::vector<std::string> args = fixture.selfArgs("EBOOT.elf");
    setOption(args, "--expect-sha256", flipLastDigit(sha256Hex(fixture.expected_elf)));

    Result result = run(args);
    CHECK(result.code == 1);
    CHECK(startsWithError(result.err));
    CHECK(contains(result.err, "EBOOT.elf: " + std::to_string(fixture.expected_elf.size()) + " bytes, sha256 " +
                                       sha256Hex(fixture.expected_elf) + "; expected "));
    CHECK(result.out.empty());
    CHECK(!fs::exists(fixture.dir / "EBOOT.elf"));
    CHECK(!fs::exists(fixture.dir / "EBOOT.elf.part"));

    // An existing output stays as it was.
    writeText(fixture.dir / "EBOOT.elf", "previous");
    result = run(args);
    CHECK(result.code == 1);
    CHECK(readBytes(fixture.dir / "EBOOT.elf") == textBytes("previous"));
    CHECK(!fs::exists(fixture.dir / "EBOOT.elf.part"));

    // The size alone is enough to refuse.
    args = fixture.selfArgs("EBOOT.elf");
    setOption(args, "--expect-size", std::to_string(fixture.expected_elf.size() + 1));
    CHECK(run(args).code == 1);
    CHECK(readBytes(fixture.dir / "EBOOT.elf") == textBytes("previous"));
}

TOX2_TEST(cliSelfInputPin) {
    const Fixture fixture;
    std::vector<std::string> args = fixture.selfArgs("EBOOT.elf");
    const std::string wrong = flipLastDigit(sha256Hex(fixture.self_file));
    args.insert(args.end(), {"--expect-input-sha256", wrong});

    const Result result = run(args);
    CHECK(result.code == 1);
    CHECK(startsWithError(result.err));
    CHECK(contains(result.err, "is not the pinned input " + wrong));
    CHECK(!fs::exists(fixture.dir / "EBOOT.elf"));

    // A missing input file is a failure, not a usage error.
    args = fixture.selfArgs("EBOOT.elf");
    setOption(args, "--in", fixture.path("absent.bin"));
    CHECK(run(args).code == 1);
}

TOX2_TEST(cliPup) {
    const Fixture fixture;
    const Result result = run(fixture.pupArgs("libsre.prx"));
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, fixture.path("PS3UPDAT.PUP") + ": firmware 4.93, " + kLibsrePath +
                                       " in dev_flash_000.tar.aa.2026_01_08_191610\n"));
    CHECK(contains(result.out, "libsre.prx: " + std::to_string(fixture.expected_elf.size()) + " bytes, sha256 " +
                                       sha256Hex(fixture.expected_elf) + ": matches the pin\n"));
    CHECK(readBytes(fixture.dir / "libsre.prx") == fixture.expected_elf);

    // A different firmware fails before any extraction, so nothing about a package is printed.
    const Result wrong = run(fixture.pupArgs("other.prx", "4.90"));
    CHECK(wrong.code == 1);
    CHECK(startsWithError(wrong.err));
    CHECK(contains(wrong.err, "firmware 4.93, expected 4.90"));
    CHECK(wrong.out.empty());
    CHECK(!fs::exists(fixture.dir / "other.prx"));

    // A path that is not in the PUP names the path.
    std::vector<std::string> args = fixture.pupArgs("missing.prx");
    setOption(args, "--path", "dev_flash/sys/external/none.sprx");
    const Result missing = run(args);
    CHECK(missing.code == 1);
    CHECK(contains(missing.err, "dev_flash/sys/external/none.sprx"));
}

TOX2_TEST(cliKeys) {
    const Fixture fixture;
    const Result result = run({"keys", "--keys", fixture.path("keys")});
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(result.out ==
          "key_vault: PUP_KEY, SCEPKG_ERK, SCEPKG_RIV found; self keys: LV0 1, LV1 0, LV2 2, APP 3, "
          "ISO 0, LDR 0, UNK7 0, NPDRM 1\n");

    const Result missing = run({"keys", "--keys", fixture.path("no_such_dir")});
    CHECK(missing.code == 1);
    CHECK(startsWithError(missing.err));
}

TOX2_TEST(cliUsage) {
    const Fixture fixture;
    const std::vector<std::string> valid = fixture.selfArgs("EBOOT.elf");

    CHECK(run({}).code == 2);
    CHECK(contains(run({}).err, "usage:"));
    CHECK(run({"bogus"}).code == 2);

    std::vector<std::string> args = valid;
    args.insert(args.end(), {"--bogus", "1"});
    CHECK(run(args).code == 2);

    // Required options, in turn.
    for (const char* option : {"--keys", "--in", "--out", "--expect-size", "--expect-sha256"}) {
        args = valid;
        const auto found = std::find(args.begin(), args.end(), option);
        args.erase(found, found + 2);
        const Result result = run(args);
        CHECK(result.code == 2);
        CHECK(contains(result.err, std::string("missing option ") + option));
    }

    args = valid;
    setOption(args, "--expect-sha256", "xyz");
    CHECK(run(args).code == 2);
    args = valid;
    setOption(args, "--expect-sha256", std::string(63, 'a'));
    CHECK(run(args).code == 2);
    args = valid;
    setOption(args, "--expect-sha256", std::string(63, 'a') + "g");
    CHECK(run(args).code == 2);
    args = valid;
    setOption(args, "--expect-size", "12x");
    CHECK(run(args).code == 2);
    args = valid;
    setOption(args, "--expect-size", "-1");
    CHECK(run(args).code == 2);
    args = valid;
    args.insert(args.end(), {"--expect-input-sha256", "not hex"});
    CHECK(run(args).code == 2);

    // An option without its value, and a repeated option.
    args = valid;
    args.pop_back();
    CHECK(run(args).code == 2);
    args = valid;
    args.insert(args.end(), {"--keys", fixture.path("keys")});
    CHECK(run(args).code == 2);

    CHECK(run({"keys"}).code == 2);
    CHECK(run({"pup", "--keys", fixture.path("keys")}).code == 2);

    // None of the usage errors wrote an output.
    CHECK(!fs::exists(fixture.dir / "EBOOT.elf"));
}

TOX2_TEST(cliNoKeyBytes) {
    const Fixture fixture;

    // A second key directory whose APP key differs (wrong key for the SELF) and one that does not parse.
    KeyVault other = fixture.vault;
    for (SelfKey& key : other.self_keys) {
        key.erk[0] ^= 0x5A;
        key.riv[0] ^= 0xA5;
    }
    Fixture::writeKeys(fixture.dir / "keys_other", other);
    FakeHeaderOptions no_riv;
    no_riv.omit_riv = true;
    Fixture::writeKeys(fixture.dir / "keys_broken", fixture.vault, no_riv);

    std::vector<Result> results;
    results.push_back(run(fixture.selfArgs("a.elf")));
    results.push_back(run(fixture.pupArgs("a.prx")));
    results.push_back(run({"keys", "--keys", fixture.path("keys")}));
    std::vector<std::string> args = fixture.selfArgs("b.elf");
    setOption(args, "--expect-sha256", flipLastDigit(sha256Hex(fixture.expected_elf)));
    results.push_back(run(args));
    for (const char* dir_name : {"keys_other", "keys_broken"}) {
        args = fixture.selfArgs("c.elf");
        setOption(args, "--keys", fixture.path(dir_name));
        results.push_back(run(args));
        args = fixture.pupArgs("c.prx");
        setOption(args, "--keys", fixture.path(dir_name));
        results.push_back(run(args));
        results.push_back(run({"keys", "--keys", fixture.path(dir_name)}));
    }
    CHECK(results[0].code == 0);
    CHECK(results[1].code == 0);
    CHECK(results[4].code == 1);
    CHECK(results[5].code == 1);
    CHECK(results[6].code == 0);
    CHECK(results[7].code == 1);
    CHECK(results[9].code == 1);

    std::vector<std::string> secrets;
    const auto add = [&](std::span<const std::uint8_t> bytes) {
        secrets.push_back(toHex(bytes));
        secrets.push_back(toHex(bytes.first(8)));
    };
    for (const KeyVault* vault : std::vector<const KeyVault*>{&fixture.vault, &other}) {
        add(vault->pup_key);
        add(vault->scepkg_erk);
        add(vault->scepkg_riv);
        for (const SelfKey& key : vault->self_keys) {
            add(key.erk);
            add(key.riv);
        }
    }
    bool clean = true;
    for (const Result& result : results) {
        for (std::string secret : secrets) {
            clean = clean && !contains(result.out, secret) && !contains(result.err, secret);
            std::transform(secret.begin(), secret.end(), secret.begin(),
                           [](char c) { return static_cast<char>(std::toupper(c)); });
            clean = clean && !contains(result.out, secret) && !contains(result.err, secret);
        }
    }
    CHECK(clean);
}

}  // namespace tox2::decrypt

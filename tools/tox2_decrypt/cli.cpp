#include "cli.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "crypto.h"
#include "keys.h"
#include "pup.h"
#include "self.h"

namespace tox2::decrypt {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
        "usage:\n"
        "  tox2_decrypt self --keys DIR --in EBOOT.BIN --out EBOOT.elf --expect-size N --expect-sha256 HEX\n"
        "                    [--expect-input-sha256 HEX]\n"
        "  tox2_decrypt pup  --keys DIR --in PS3UPDAT.PUP --path PATH --firmware VERSION --out libsre.prx\n"
        "                    --expect-size N --expect-sha256 HEX\n"
        "  tox2_decrypt keys --keys DIR\n";

struct OptionSpec {
    std::string_view name;
    bool required;
};

using OptionMap = std::map<std::string, std::string, std::less<>>;

int fail(std::string& err, std::string_view message) {
    err += "error: ";
    err += message;
    err += '\n';
    return 1;
}

int usageError(std::string& err, std::string_view message) {
    fail(err, message);
    err += kUsage;
    return 2;
}

// Options are `--name value` pairs. Anything else, a repeated option or a missing required one is a usage
// error; the message names the option, never a value.
bool parseOptions(std::span<const std::string_view> args, std::span<const OptionSpec> specs, OptionMap& options,
                  std::string& error) {
    for (std::size_t i = 0; i < args.size(); i += 2) {
        const std::string_view name = args[i];
        const OptionSpec* spec = nullptr;
        for (const OptionSpec& candidate : specs) {
            if (name.size() > 2 && name.substr(0, 2) == "--" && name.substr(2) == candidate.name) {
                spec = &candidate;
            }
        }
        if (spec == nullptr) {
            error = "unknown option " + std::string(name);
            return false;
        }
        if (i + 1 >= args.size()) {
            error = "option " + std::string(name) + " needs a value";
            return false;
        }
        if (!options.emplace(std::string(spec->name), std::string(args[i + 1])).second) {
            error = "option " + std::string(name) + " given twice";
            return false;
        }
    }
    for (const OptionSpec& spec : specs) {
        if (spec.required && !options.contains(std::string(spec.name))) {
            error = "missing option --" + std::string(spec.name);
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> parseSize(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// 64 hex digits of either case; returns the lowercase form.
std::optional<std::string> parseSha256(std::string_view text) {
    if (text.size() != 64) {
        return std::nullopt;
    }
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return std::nullopt;
        }
    }
    return lower;
}

bool readFile(const fs::path& path, Bytes& data, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open file";
        return false;
    }
    const std::streamsize size = file.tellg();
    data.assign(static_cast<std::size_t>(size), 0);
    file.seekg(0);
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        error = "cannot read file";
        return false;
    }
    return true;
}

// The output appears under its final name only after the pin check, and never half written.
bool writeFileReplacing(const fs::path& path, const Bytes& data, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create directory " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    fs::path part = path;
    part += ".part";
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file) {
            fs::remove(part, ec);
            error = "cannot write " + part.string();
            return false;
        }
    }
    fs::rename(part, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        fs::remove(part, ec);
        error = "cannot rename " + part.string() + " to " + path.string() + ": " + reason;
        return false;
    }
    return true;
}

struct Pin {
    std::uint64_t size;
    std::string sha256;
};

// Compares the result with its pin and, only on a match, writes it.
int finishOutput(const OptionMap& options, const Pin& pin, const Bytes& data, std::string& out, std::string& err) {
    const fs::path out_path(options.at("out"));
    const std::string name = out_path.filename().string();
    Sha256Digest digest;
    if (!sha256(data, digest)) {
        return fail(err, name + ": SHA-256 failed");
    }
    const std::string actual = toHex(digest);
    if (data.size() != pin.size || actual != pin.sha256) {
        return fail(err, name + ": " + std::to_string(data.size()) + " bytes, sha256 " + actual + "; expected " +
                                 std::to_string(pin.size) + " bytes, sha256 " + pin.sha256);
    }
    std::string error;
    if (!writeFileReplacing(out_path, data, error)) {
        return fail(err, name + ": " + error);
    }
    out += name + ": " + std::to_string(data.size()) + " bytes, sha256 " + actual + ": matches the pin\n";
    return 0;
}

// Parses the options shared by `self` and `pup` into a Pin; returns false after writing a usage error.
bool parsePin(const OptionMap& options, Pin& pin, std::string& err, int& code) {
    const std::optional<std::uint64_t> size = parseSize(options.at("expect-size"));
    if (!size) {
        code = usageError(err, "--expect-size is not a decimal number");
        return false;
    }
    const std::optional<std::string> hash = parseSha256(options.at("expect-sha256"));
    if (!hash) {
        code = usageError(err, "--expect-sha256 is not 64 hex digits");
        return false;
    }
    pin = {*size, *hash};
    return true;
}

bool loadKeys(const OptionMap& options, KeyVault& keys, std::string& err) {
    std::string error;
    if (!loadKeyVault(options.at("keys"), keys, error)) {
        fail(err, options.at("keys") + ": " + error);
        return false;
    }
    return true;
}

int runSelf(std::span<const std::string_view> args, std::string& out, std::string& err) {
    static constexpr OptionSpec kSpecs[] = {{"keys", true},          {"in", true},
                                            {"out", true},           {"expect-size", true},
                                            {"expect-sha256", true}, {"expect-input-sha256", false}};
    OptionMap options;
    std::string error;
    if (!parseOptions(args, kSpecs, options, error)) {
        return usageError(err, error);
    }
    Pin pin;
    int code = 0;
    if (!parsePin(options, pin, err, code)) {
        return code;
    }
    std::optional<std::string> input_hash;
    if (options.contains("expect-input-sha256")) {
        input_hash = parseSha256(options.at("expect-input-sha256"));
        if (!input_hash) {
            return usageError(err, "--expect-input-sha256 is not 64 hex digits");
        }
    }

    const std::string& in_name = options.at("in");
    Bytes file;
    if (!readFile(in_name, file, error)) {
        return fail(err, in_name + ": " + error);
    }
    if (input_hash) {
        Sha256Digest digest;
        if (!sha256(file, digest)) {
            return fail(err, in_name + ": SHA-256 failed");
        }
        if (toHex(digest) != *input_hash) {
            return fail(err, in_name + ": sha256 " + toHex(digest) + " is not the pinned input " + *input_hash);
        }
    }

    KeyVault keys;
    if (!loadKeys(options, keys, err)) {
        return 1;
    }
    Bytes elf;
    SelfInfo info;
    if (!decryptSelf(file, keys, elf, info, error)) {
        return fail(err, in_name + ": " + error);
    }
    return finishOutput(options, pin, elf, out, err);
}

int runPup(std::span<const std::string_view> args, std::string& out, std::string& err) {
    static constexpr OptionSpec kSpecs[] = {{"keys", true},         {"in", true},  {"path", true},
                                            {"firmware", true},     {"out", true}, {"expect-size", true},
                                            {"expect-sha256", true}};
    OptionMap options;
    std::string error;
    if (!parseOptions(args, kSpecs, options, error)) {
        return usageError(err, error);
    }
    Pin pin;
    int code = 0;
    if (!parsePin(options, pin, err, code)) {
        return code;
    }

    const std::string& in_name = options.at("in");
    const std::string& path = options.at("path");
    KeyVault keys;
    if (!loadKeys(options, keys, err)) {
        return 1;
    }
    Bytes pup;
    if (!readFile(in_name, pup, error)) {
        return fail(err, in_name + ": " + error);
    }

    std::string version;
    if (!readPupFirmware(pup, keys, version, error)) {
        return fail(err, in_name + ": " + error);
    }
    if (version != options.at("firmware")) {
        return fail(err, in_name + ": firmware " + version + ", expected " + options.at("firmware"));
    }
    PupExtract extract;
    if (!extractFromPup(pup, keys, path, extract, error)) {
        return fail(err, in_name + ": " + error);
    }
    out += in_name + ": firmware " + version + ", " + path + " in " + extract.package + "\n";

    Bytes elf;
    SelfInfo info;
    if (!decryptSelf(extract.data, keys, elf, info, error)) {
        return fail(err, path + " in " + extract.package + ": " + error);
    }
    return finishOutput(options, pin, elf, out, err);
}

int runKeys(std::span<const std::string_view> args, std::string& out, std::string& err) {
    static constexpr OptionSpec kSpecs[] = {{"keys", true}};
    OptionMap options;
    std::string error;
    if (!parseOptions(args, kSpecs, options, error)) {
        return usageError(err, error);
    }
    KeyVault keys;
    if (!loadKeys(options, keys, err)) {
        return 1;
    }
    struct TypeName {
        KeyType type;
        const char* name;
    };
    static constexpr TypeName kTypes[] = {{KeyType::Lv0, "LV0"},   {KeyType::Lv1, "LV1"},    {KeyType::Lv2, "LV2"},
                                          {KeyType::App, "APP"},   {KeyType::Iso, "ISO"},    {KeyType::Ldr, "LDR"},
                                          {KeyType::Unk7, "UNK7"}, {KeyType::Npdrm, "NPDRM"}};
    out += "key_vault: PUP_KEY, SCEPKG_ERK, SCEPKG_RIV found; self keys:";
    bool first = true;
    for (const TypeName& entry : kTypes) {
        std::size_t count = 0;
        for (const SelfKey& key : keys.self_keys) {
            count += key.type == entry.type ? 1 : 0;
        }
        out += first ? " " : ", ";
        out += std::string(entry.name) + " " + std::to_string(count);
        first = false;
    }
    out += '\n';
    return 0;
}

}  // namespace

int runCli(std::span<const std::string_view> args, std::string& out, std::string& err) {
    if (args.empty()) {
        err += kUsage;
        return 2;
    }
    const std::string_view command = args[0];
    const auto rest = args.subspan(1);
    if (command == "self") {
        return runSelf(rest, out, err);
    }
    if (command == "pup") {
        return runPup(rest, out, err);
    }
    if (command == "keys") {
        return runKeys(rest, out, err);
    }
    return usageError(err, "unknown command " + std::string(command));
}

}  // namespace tox2::decrypt

// dal_sdat: decrypts the .sdat files of a game directory into a host cache directory that
// `dal_game --sdata-cache` serves to the guest. The AES keys are read from a local RPCS3 source
// checkout at run time and are never stored or printed.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sdat.h"

namespace {

namespace stdfs = std::filesystem;

void printUsage() {
    std::fputs(
            "usage: dal_sdat --keys PATH --game-dir DIR --out DIR\n"
            "  PATH  key_vault.h (or key_vault.cpp, or the directory holding them) of an RPCS3 checkout\n"
            "  DIR   game directory searched recursively for *.sdat\n"
            "  --out DIR receives each file at its path relative to the game directory\n",
            stderr);
}

bool hasSdatExtension(const stdfs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".sdat";
}

}  // namespace

int main(int argc, char** argv) {
    std::string keys_path;
    std::string game_dir;
    std::string out_dir;
    for (int i = 1; i < argc; i++) {
        const std::string_view arg = argv[i];
        std::string* target = arg == "--keys"       ? &keys_path
                              : arg == "--game-dir" ? &game_dir
                              : arg == "--out"      ? &out_dir
                                                    : nullptr;
        if (target == nullptr || i + 1 >= argc) {
            printUsage();
            return 2;
        }
        *target = argv[++i];
    }
    if (keys_path.empty() || game_dir.empty() || out_dir.empty()) {
        printUsage();
        return 2;
    }

    dal::sdat::Keys keys;
    std::string error;
    if (!dal::sdat::loadKeys(keys_path, keys, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::error_code ec;
    const stdfs::path root = stdfs::absolute(game_dir, ec);
    std::vector<stdfs::path> inputs;
    for (stdfs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && hasSdatExtension(it->path())) {
            inputs.push_back(it->path());
        }
    }
    if (ec) {
        std::fprintf(stderr, "error: cannot scan %s: %s\n", game_dir.c_str(), ec.message().c_str());
        return 1;
    }
    std::sort(inputs.begin(), inputs.end());

    int failures = 0;
    for (const stdfs::path& input : inputs) {
        const stdfs::path relative = stdfs::relative(input, root, ec);
        std::ifstream in(input, std::ios::binary);
        const dal::sdat::Bytes image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dal::sdat::Bytes plain;
        dal::sdat::SdatInfo info;
        if (!dal::sdat::decryptSdat(image, keys, plain, info, error)) {
            std::fprintf(stderr, "error: %s: %s\n", relative.string().c_str(), error.c_str());
            failures++;
            continue;
        }
        const stdfs::path target = stdfs::path(out_dir) / relative;
        stdfs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
        if (!out) {
            std::fprintf(stderr, "error: cannot write %s\n", target.string().c_str());
            failures++;
            continue;
        }
        std::printf("%s: version %d flags %#010x block %u blocks %u (compressed %u) -> %llu bytes\n",
                    relative.generic_string().c_str(), info.version, info.flags, info.block_size, info.blocks,
                    info.compressed_blocks, static_cast<unsigned long long>(plain.size()));
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "warning: no .sdat files under %s\n", game_dir.c_str());
    }
    return failures == 0 ? 0 : 1;
}

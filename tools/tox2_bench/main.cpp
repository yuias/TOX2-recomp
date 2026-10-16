// tox2_bench: fixed-input benchmarks in thread cycles for code that the game spends CPU in. Every case runs a
// fixed workload several times, checks its output outside the measured span and prints one summary line that the
// A/B comparison script parses:
//
//   bench <case>: N iterations (W warm-up), median C cycles (M ms), min C, max C; check ok
//
// A case may print diagnostic lines before it; they start with "bench <case>:" and do not contain "median".
//
// Usage: tox2_bench --list
//        tox2_bench <case> [case options] [--iterations N] [--warmup N]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/thread_cpu.h"
#include "spu_lzma_fixture.h"

namespace {

struct Options {
    std::uint32_t iterations = 0;
    std::uint32_t warmup = 0;
    std::map<std::string, std::string, std::less<>> values;  // case options by name, without the leading dashes

    std::string_view value(std::string_view name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::string_view() : std::string_view(it->second);
    }
};

struct Case {
    const char* name;
    const char* summary;
    std::uint32_t default_iterations;
    std::uint32_t default_warmup;
    std::vector<std::string_view> options;  // case options that take a value, without the leading dashes
    int (*run)(const Case& self, const Options& options);
};

// Runs `iteration` for the warm-up and the measured iterations. It stores the cycles of its measured span and returns
// false when the check of its output fails. Prints the summary line; the exit status is 0 only when every check held.
int runMeasured(const Case& self, const Options& options, const std::function<bool(std::uint64_t* cycles)>& iteration) {
    std::vector<std::uint64_t> samples;
    for (std::uint32_t i = 0; i < options.warmup + options.iterations; i++) {
        std::uint64_t cycles = 0;
        if (!iteration(&cycles)) {
            // The iteration number counts the warm-up iterations too.
            std::printf("bench %s: check FAILED iteration %u\n", self.name, i);
            return 1;
        }
        if (i >= options.warmup) samples.push_back(cycles);
    }
    const tox2::CycleStats stats = tox2::summarizeCycles(samples);
    std::printf("bench %s: %u iterations (%u warm-up), median %llu cycles (%.1f ms), min %llu, max %llu; check ok\n",
                self.name, options.iterations, options.warmup, static_cast<unsigned long long>(stats.median),
                static_cast<double>(stats.median) / tox2::hostCyclesPerMs(), static_cast<unsigned long long>(stats.min),
                static_cast<unsigned long long>(stats.max));
    return 0;
}

// The recompiled inflate of the game's LZMA task: the cycles of the runSpu calls of one inflate of the whole item,
// checked against the item's known SHA-256.
int runSpuLzma(const Case& self, const Options& options) {
    const std::string game_dir(options.value("game-dir"));
    const std::string elf(options.value("elf"));
    if (game_dir.empty() || elf.empty()) {
        std::fputs("error: spu-lzma needs --game-dir DIR and --elf EBOOT.elf\n", stderr);
        return 2;
    }
    namespace fixture = tox2::lzma_fixture;
    fixture::Fixture lzma;
    if (!lzma.init(game_dir, elf)) return 1;
    fixture::Inflate run;
    return runMeasured(self, options, [&](std::uint64_t* cycles) {
        if (!lzma.inflate(&run)) return false;
        *cycles = run.cycles;
        return run.output.size() == lzma.itemUsize() &&
               fixture::sha256Hex(run.output.data(), run.output.size()) == fixture::kExpectedSha256;
    });
}

const std::vector<Case>& cases() {
    static const std::vector<Case> kCases = {
            {"spu-lzma",
             "recompiled LZMA inflate of a TLZC item (--game-dir DIR --elf EBOOT.elf)",
             50,
             3,
             {"game-dir", "elf"},
             runSpuLzma},
    };
    return kCases;
}

void printUsage(std::FILE* out) {
    std::fputs(
            "usage: tox2_bench --list\n"
            "       tox2_bench <case> [case options] [--iterations N] [--warmup N]\n",
            out);
}

// Parses a whole number in [min, max]; false (with a message) otherwise.
bool parseCount(std::string_view option, std::string_view text, std::uint32_t min, std::uint32_t max,
                std::uint32_t* out) {
    const std::string copy(text);
    char* end = nullptr;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
    if (copy.empty() || *end != '\0' || value < min || value > max) {
        std::fprintf(stderr, "error: %.*s takes a whole number from %u to %u\n", static_cast<int>(option.size()),
                     option.data(), min, max);
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(stderr);
        return 2;
    }
    const std::string_view first = argv[1];
    if (first == "--list") {
        for (const Case& c : cases()) std::printf("%s  %s\n", c.name, c.summary);
        return 0;
    }
    if (first == "--help" || first == "-h") {
        printUsage(stdout);
        return 0;
    }
    const Case* selected = nullptr;
    for (const Case& c : cases()) {
        if (first == c.name) selected = &c;
    }
    if (selected == nullptr) {
        std::fprintf(stderr, "error: unknown case %s (see --list)\n", argv[1]);
        return 2;
    }

    Options options;
    options.iterations = selected->default_iterations;
    options.warmup = selected->default_warmup;
    for (int i = 2; i < argc; i++) {
        const std::string_view arg = argv[i];
        if (!arg.starts_with("--") || i + 1 >= argc) {
            std::fprintf(stderr, "error: option %s is unknown or has no value\n", argv[i]);
            return 2;
        }
        const std::string_view name = arg.substr(2);
        const std::string_view value = argv[++i];
        if (name == "iterations") {
            if (!parseCount(arg, value, 1, 100000, &options.iterations)) return 2;
        } else if (name == "warmup") {
            if (!parseCount(arg, value, 0, 100000, &options.warmup)) return 2;
        } else if (std::find(selected->options.begin(), selected->options.end(), name) != selected->options.end()) {
            options.values[std::string(name)] = std::string(value);
        } else {
            std::fprintf(stderr, "error: case %s has no option %s\n", selected->name, argv[i - 1]);
            return 2;
        }
    }
    return selected->run(*selected, options);
}

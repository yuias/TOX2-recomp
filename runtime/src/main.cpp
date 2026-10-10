#include <cstdio>
#include <cstring>

#include "tox2/compiler.h"

#ifndef TOX2_VERSION
#define TOX2_VERSION "0.0.0"
#endif

namespace {

// Debug builds do not optimize, so only a real tail call keeps this within the stack.
TOX2_NOINLINE int tailCount(int remaining, int acc) {
    if (remaining == 0) {
        return acc;
    }
    TOX2_MUSTTAIL return tailCount(remaining - 1, acc + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: tox2_runtime [--version | --selftest-musttail]");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("tox2_runtime " TOX2_VERSION);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--selftest-musttail") == 0) {
        constexpr int kDepth = 10'000'000;
        if (tailCount(kDepth, 0) != kDepth) {
            std::fputs("musttail failed\n", stderr);
            return 1;
        }
        std::puts("musttail ok");
        return 0;
    }
    std::fprintf(stderr, "tox2_runtime: unknown argument '%s'\n", argv[1]);
    return 2;
}

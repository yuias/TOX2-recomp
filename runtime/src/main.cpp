#include <cstdio>
#include <cstring>

#include "dal/compiler.h"

#ifndef DAL_VERSION
#define DAL_VERSION "0.0.0"
#endif

namespace {

// Debug builds do not optimize, so only a real tail call keeps this within the stack.
DAL_NOINLINE int tailCount(int remaining, int acc) {
    if (remaining == 0) {
        return acc;
    }
    DAL_MUSTTAIL return tailCount(remaining - 1, acc + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: dal_runtime [--version | --selftest-musttail]");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("dal_runtime " DAL_VERSION);
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
    std::fprintf(stderr, "dal_runtime: unknown argument '%s'\n", argv[1]);
    return 2;
}

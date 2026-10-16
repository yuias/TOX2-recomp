// Runs the recompiled edgeLzmaInflateRawData of the game's LZMA SPURS task (image eboot_edgelzma) on real
// data: a TLZC item of TLFILE.TLDAT is inflated chunk by chunk the way the task's main loop does it, and the
// concatenated output is compared with the SHA-256 of the item as inflated by a reference decoder. The time
// spent in the recompiled code is printed as a throughput figure; it is not checked. The fixture is shared with
// tox2_bench, which measures the same inflate in thread cycles.
//
// Usage: tox2_spu_lzma_inflate_test <game-dir> <decrypted EBOOT.elf>

#include <chrono>
#include <cstdio>
#include <string>

#include "spu_lzma_fixture.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <game-dir> <EBOOT.elf>\n", argv[0]);
        return 2;
    }
    namespace fixture = tox2::lzma_fixture;
    fixture::Fixture lzma;
    if (!lzma.init(argv[1], argv[2])) return 1;
    fixture::Inflate run;
    if (!lzma.inflate(&run)) return 1;

    const std::string sha256 = fixture::sha256Hex(run.output.data(), run.output.size());
    std::printf("spu lzma inflate: %#zx bytes sha256 %s\n", run.output.size(), sha256.c_str());
    const double spu_us = std::chrono::duration<double, std::micro>(run.time).count();
    std::printf("spu lzma inflate: %.0f us in the recompiled code, %.1f MB/s\n", spu_us, run.output.size() / spu_us);
    if (run.output.size() != lzma.itemUsize() || sha256 != fixture::kExpectedSha256) {
        std::fprintf(stderr, "expected %#x bytes sha256 %s\n", lzma.itemUsize(),
                     std::string(fixture::kExpectedSha256).c_str());
        return 1;
    }
    return 0;
}

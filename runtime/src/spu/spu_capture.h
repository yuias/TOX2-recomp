#pragma once

// Capture of one function span of generated SPU code: the local store and registers at a dispatch to an entry S, and
// again at the first later dispatch to an entry E, so a benchmark can replay the span alone and compare its result.
//
// A call is a direct tail call in the stack-free lowering and a return is a dispatch, so the runtime only sees
// the computed transfers: the span of a call runs from the dispatch to S (before the call site) to the dispatch to E
// (the return address). installSpuCapture builds a copy of the image table whose S and E entries run a trampoline
// first; no SPU runtime path is edited, and with no selector the generated table is used unchanged.
//
// The file format is host little-endian, one file per span:
//
//   offset  size     field
//   0       8        magic "TOX2SPUC"
//   8       4        version
//   12      4        header size (160)
//   16      32       image name, NUL-padded
//   48      64       image sha256 (Tox2SpuImageInfo::sha256, hex)
//   112     4 x 4    start S, end E, thread id, tag run count
//   128     8 x 2    span cycles (thread cycles from S to E, diagnostic), milliseconds since installSpuCapture
//   144     4 x 4    dispatches in the span (E included), word 0 of r1 at S, at E, flags (0)
//   160     2080     pre state: gpr[128] (Tox2Vr in memory layout), fpscr (16), srr0 (4), int_enabled (4), pad to 16
//           262144   pre LS
//           2080     post state: gpr[128], fpscr (16), jump_target (4), pad to 16
//           262144   post LS
//           48 x n   tag runs: image name (32, NUL-padded), segment u32, delta i32, ls lo u32, ls hi u32
//
// A tag run says that LS [lo, hi) holds the code segment `segment` of the named image, `delta` bytes away from the
// segment's link address; any registered image can appear (the policy module and kernel code sit in the LS too).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tox2/spu_context.h"

namespace tox2 {

inline constexpr std::uint32_t kSpuCaptureVersion = 1;
inline constexpr std::uint32_t kSpuCaptureHeaderSize = 160;
inline constexpr std::uint32_t kSpuCaptureNameSize = 32;
inline constexpr std::uint32_t kSpuCaptureHashSize = 64;
inline constexpr std::uint32_t kSpuCaptureStateSize = 2080;
inline constexpr std::uint32_t kSpuCaptureTagRunSize = 48;
// Trampolines available to one installation; a slot serves one entry however many selectors use it.
inline constexpr std::uint32_t kSpuCaptureMaxSlots = 16;

struct SpuCaptureTagRun {
    std::string image;
    std::uint32_t segment = 0;
    std::int32_t delta = 0;
    std::uint32_t lo = 0;  // LS range, multiples of the 16-byte tag unit
    std::uint32_t hi = 0;
};

struct SpuCapture {
    std::string image;
    std::string sha256;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t thread_id = 0;
    std::uint64_t span_cycles = 0;
    std::uint64_t ms_since_install = 0;  // from the installSpuCapture call to the end of the span
    std::uint32_t dispatches = 0;
    std::uint32_t sp_start = 0;
    std::uint32_t sp_end = 0;
    std::uint32_t flags = 0;

    // State when the dispatch to `start` is about to run.
    std::array<Tox2Vr, 128> pre_gpr{};
    Tox2Vr pre_fpscr{};
    std::uint32_t pre_srr0 = 0;
    std::uint32_t pre_int_enabled = 0;
    std::vector<std::uint8_t> pre_ls;  // kTox2SpuLsSize bytes

    // State when the dispatch to `end` is about to run (jump_target is the dispatch target as the code set it).
    std::array<Tox2Vr, 128> post_gpr{};
    Tox2Vr post_fpscr{};
    std::uint32_t post_jump_target = 0;
    std::vector<std::uint8_t> post_ls;

    std::vector<SpuCaptureTagRun> tag_runs;
};

// Total size of a file with `tag_runs` tag runs.
std::uint64_t spuCaptureFileSize(std::uint32_t tag_runs);

// Bytes of the file. False with `error` set when a field cannot be stored (LS size, name or hash too long).
bool serializeSpuCapture(const SpuCapture& capture, std::vector<std::uint8_t>& out, std::string& error);
// Checks magic, version, header size and the exact file size; nullopt with `error` set otherwise.
std::optional<SpuCapture> parseSpuCapture(std::span<const std::uint8_t> bytes, std::string& error);

// Checks that the capture belongs to this table: an image of that name with that sha256, and every tag run names an
// image of the table with a code segment of that number and a range inside the LS.
bool validateSpuCapture(const SpuCapture& capture, const Tox2SpuImageInfo* images, std::uint32_t count,
                        std::string& error);

bool writeSpuCapture(const std::filesystem::path& path, const SpuCapture& capture, std::string& error);
// Reads, parses and validates against the given table (the one registered for the replay).
std::optional<SpuCapture> readSpuCapture(const std::filesystem::path& path, const Tox2SpuImageInfo* images,
                                         std::uint32_t count, std::string& error);

// One span to capture: the image, the entries a span may start at, the entry that ends it, and how many dispatches
// (the one to the end entry included) it may contain.
struct SpuCaptureSelector {
    std::string image;
    std::vector<std::uint32_t> starts;
    std::uint32_t end = 0;
    std::uint32_t max_dispatches = 0;  // 0 = no limit
};

// Parses "IMAGE:S[+S...]:E[:dispatches=N]". S and E are hex with an optional 0x prefix, N is decimal. On failure
// returns nullopt and sets `error`.
std::optional<SpuCaptureSelector> parseSpuCaptureSelector(std::string_view text, std::string& error);

struct SpuCaptureSettings {
    std::string dir;           // created when missing
    double after_seconds = 0;  // spans start to count this long after installSpuCapture
    std::uint32_t count = 4;   // files per selector
    std::uint32_t every = 1;   // write every K-th accepted span
};

// Returns the table to register in place of `images`: a copy in which the selected entries run a trampoline first.
// With no selector it returns `images` itself. nullptr with `error` set for an unknown image, an S or E that is not
// an entry of it, E listed among the starts, an unusable setting or more than kSpuCaptureMaxSlots distinct entries.
//
// One installation is live at a time: a later call replaces the selectors and slots of an earlier one (its table
// must not be registered any more). The result and the memory behind it stay valid for the rest of the process.
// Spans are tracked per SPU host thread; a span is accepted when no channel was read, no MFC command ran, no `stop`
// was counted between S and E and it holds no more dispatches than the selector allows. A logged line
// "tox2: spu capture: wrote ..." follows each file.
const Tox2SpuImageInfo* installSpuCapture(const Tox2SpuImageInfo* images, std::uint32_t count,
                                          std::span<const SpuCaptureSelector> selectors,
                                          const SpuCaptureSettings& settings, std::string& error);

}  // namespace tox2

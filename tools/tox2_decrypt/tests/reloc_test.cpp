// Tests for PRX relocation on a synthetic in-memory PRX, and for the `reloc` command.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "crypto.h"
#include "prx_reloc.h"
#include "test_util.h"

namespace tox2::decrypt {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kBase = 0x011D0000;
constexpr std::uint16_t kPrxType = 0xFFA4;
constexpr std::uint32_t kSceRela = 0x700000A4;

// File layout: ELF header, up to four program headers, segment 0 data, segment 1 data, the records.
constexpr std::size_t kPhdrOffset = 0x40;
constexpr std::size_t kPhdrSize = 0x38;
constexpr std::size_t kSeg0Offset = 0x200;
constexpr std::size_t kSeg0Size = 0x100;
constexpr std::size_t kSeg1Offset = 0x300;
constexpr std::size_t kSeg1Size = 0x80;
constexpr std::size_t kRelaOffset = 0x380;

void put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    put16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
    put16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

void put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    put32(bytes, offset, static_cast<std::uint32_t>(value >> 32));
    put32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

struct Record {
    std::uint32_t type = 1;
    std::uint32_t off_seg = 0;
    std::uint32_t sym_seg = 0;
    std::uint64_t r_offset = 0;
    std::int64_t addend = 0;
    // Overrides the pre-linked value the builder writes at the site.
    std::optional<std::uint32_t> site = std::nullopt;
};

// Segment 0 is linked at 0 and segment 1 at `seg1_vaddr`; segment 0's memory size reaches past
// `seg1_vaddr`, as in the real module. Header index 2 (and 3) is the SCE_PPURELA header.
struct SyntheticPrx {
    std::uint16_t e_type = kPrxType;
    std::uint32_t rela_headers = 1;
    std::uint64_t seg1_vaddr = 0x100;
    std::vector<Record> records;

    Bytes build() const {
        const std::size_t phnum = 2 + rela_headers;
        Bytes file(kRelaOffset + records.size() * 24, 0);
        for (std::size_t i = 0; i < file.size(); i++) {
            file[i] = static_cast<std::uint8_t>(0xC0 + (i * 7) % 29);
        }
        file[0] = 0x7F;
        file[1] = 'E';
        file[2] = 'L';
        file[3] = 'F';
        file[4] = 2;
        file[5] = 2;
        file[6] = 1;
        put16(file, 0x10, e_type);
        put16(file, 0x12, 21);
        put64(file, 0x20, kPhdrOffset);
        put16(file, 0x34, 0x40);
        put16(file, 0x36, kPhdrSize);
        put16(file, 0x38, static_cast<std::uint16_t>(phnum));

        const auto header = [&](std::size_t index, std::uint32_t type, std::uint64_t offset, std::uint64_t vaddr,
                                std::uint64_t filesz, std::uint64_t memsz) {
            const std::size_t at = kPhdrOffset + index * kPhdrSize;
            put32(file, at, type);
            put32(file, at + 4, 5);
            put64(file, at + 0x08, offset);
            put64(file, at + 0x10, vaddr);
            // Module info position of the real file; the tool must leave it alone.
            put64(file, at + 0x18, index == 0 ? kSeg0Offset + 0x20 : 0);
            put64(file, at + 0x20, filesz);
            put64(file, at + 0x28, memsz);
            put64(file, at + 0x30, 0x10);
        };
        header(0, 1, kSeg0Offset, 0, kSeg0Size, 0x180);
        header(1, 1, kSeg1Offset, seg1_vaddr, kSeg1Size, 0xC0);
        for (std::size_t i = 0; i < rela_headers; i++) {
            header(2 + i, kSceRela, kRelaOffset, 0, records.size() * 24, 0);
        }

        const std::uint64_t vaddrs[2] = {0, seg1_vaddr};
        for (std::size_t i = 0; i < records.size(); i++) {
            const Record& record = records[i];
            put64(file, kRelaOffset + i * 24, record.r_offset);
            put64(file, kRelaOffset + i * 24 + 8,
                  (static_cast<std::uint64_t>(record.sym_seg) << 40) |
                          (static_cast<std::uint64_t>(record.off_seg) << 32) | record.type);
            put64(file, kRelaOffset + i * 24 + 16, static_cast<std::uint64_t>(record.addend));

            const std::size_t size = record.type == 1 ? 4 : 2;
            const bool known = record.type == 1 || (record.type >= 4 && record.type <= 6);
            if (!known || record.off_seg > 1 || record.sym_seg > 1) {
                continue;
            }
            const std::size_t seg_offset = record.off_seg == 0 ? kSeg0Offset : kSeg1Offset;
            const std::size_t seg_size = record.off_seg == 0 ? kSeg0Size : kSeg1Size;
            if (record.r_offset > seg_size || size > seg_size - record.r_offset) {
                continue;
            }
            const std::uint32_t s = static_cast<std::uint32_t>(vaddrs[record.sym_seg] + record.addend);
            std::uint32_t value = s;
            if (record.type == 4) {
                value = s & 0xFFFF;
            } else if (record.type == 5) {
                value = s >> 16;
            } else if (record.type == 6) {
                value = ((s + 0x8000) >> 16) & 0xFFFF;
            }
            if (record.site) {
                value = *record.site;
            }
            if (size == 4) {
                put32(file, seg_offset + record.r_offset, value);
            } else {
                put16(file, seg_offset + record.r_offset, static_cast<std::uint16_t>(value));
            }
        }
        return file;
    }
};

// One record of every type; the three 16-bit records target 0x18000 so that HA differs from HI.
std::vector<Record> goodRecords() {
    return {
            {.type = 1, .off_seg = 0, .sym_seg = 1, .r_offset = 0x20, .addend = 0x10},
            {.type = 1, .off_seg = 1, .sym_seg = 0, .r_offset = 0x00, .addend = 0x1234},
            {.type = 4, .off_seg = 0, .sym_seg = 0, .r_offset = 0x40, .addend = 0x18000},
            {.type = 5, .off_seg = 0, .sym_seg = 0, .r_offset = 0x44, .addend = 0x18000},
            {.type = 6, .off_seg = 1, .sym_seg = 0, .r_offset = 0x10, .addend = 0x18000},
    };
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Runs relocatePrx and reports whether it failed with a message containing `part`; `out` stays empty then.
bool refused(const Bytes& prx, std::uint32_t base, const std::string& part) {
    Bytes out = {1, 2, 3};
    RelocCounts counts;
    std::uint32_t image_end = 7;
    std::string error;
    const bool ok = relocatePrx(prx, base, out, counts, image_end, error);
    if (ok || !out.empty() || !contains(error, part)) {
        std::fprintf(stderr, "refused(): ok=%d out=%zu error=\"%s\" wanted \"%s\"\n", ok ? 1 : 0, out.size(),
                     error.c_str(), part.c_str());
        return false;
    }
    return true;
}

SyntheticPrx withRecords(std::vector<Record> records) {
    SyntheticPrx prx;
    prx.records = std::move(records);
    return prx;
}

std::string sha256Hex(const Bytes& data) {
    Sha256Digest digest;
    sha256(data, digest);
    return toHex(digest);
}

struct TempDir {
    fs::path dir;

    TempDir() {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path() / ("tox2_decrypt_reloc_test_" +
                                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                           "_" + std::to_string(counter++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string path(const char* name) const { return (dir / name).string(); }
};

void writeFile(const std::string& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

Bytes readBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

void setOption(std::vector<std::string>& args, const std::string& option, const std::string& value) {
    for (std::size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == option) {
            args[i + 1] = value;
        }
    }
}

}  // namespace

TOX2_TEST(relocPrxSynthetic) {
    const Bytes input = withRecords(goodRecords()).build();
    const Bytes original = input;

    Bytes out;
    RelocCounts counts;
    std::uint32_t image_end = 0;
    std::string error;
    CHECK(relocatePrx(input, kBase, out, counts, image_end, error));
    CHECK(error.empty());
    CHECK(input == original);
    CHECK(out.size() == input.size());
    CHECK(counts.addr32 == 2);
    CHECK(counts.addr16_lo == 1);
    CHECK(counts.addr16_hi == 1);
    CHECK(counts.addr16_ha == 1);
    CHECK(counts.total() == 5);
    // Segment 1 ends at 0x100 + 0xC0, past segment 0's trimmed end of 0x100.
    CHECK(image_end == kBase + 0x1C0);

    // The expected file is written down from the table in the spec, independently of the tool.
    Bytes expected = input;
    put64(expected, kPhdrOffset + 0x10, kBase);
    put64(expected, kPhdrOffset + 0x28, 0x100);
    put64(expected, kPhdrOffset + kPhdrSize + 0x10, kBase + 0x100);
    put32(expected, kPhdrOffset + 2 * kPhdrSize, 0);
    put32(expected, kSeg0Offset + 0x20, 0x011D0110);
    put32(expected, kSeg1Offset + 0x00, 0x011D1234);
    // Target 0x011E8000: LO unchanged, HI 0x011E, HA rounds up to 0x011F.
    put16(expected, kSeg0Offset + 0x40, 0x8000);
    put16(expected, kSeg0Offset + 0x44, 0x011E);
    put16(expected, kSeg1Offset + 0x10, 0x011F);
    CHECK(out == expected);

    // p_paddr (the module info position), file offsets and the record bytes stay.
    CHECK(Bytes(out.begin() + kPhdrOffset + 0x18, out.begin() + kPhdrOffset + 0x20) ==
          Bytes(input.begin() + kPhdrOffset + 0x18, input.begin() + kPhdrOffset + 0x20));
    CHECK(Bytes(out.begin() + kRelaOffset, out.end()) == Bytes(input.begin() + kRelaOffset, input.end()));

    // A pure function: the same input and base give the same bytes.
    Bytes again;
    RelocCounts again_counts;
    std::uint32_t again_end = 0;
    CHECK(relocatePrx(input, kBase, again, again_counts, again_end, error));
    CHECK(again == out);
    CHECK(again_end == image_end);

    // Another base moves HI and HA by exactly base >> 16 and leaves LO.
    Bytes other;
    CHECK(relocatePrx(input, 0x00340000, other, again_counts, again_end, error));
    CHECK(other.size() == input.size());
    CHECK(Bytes(other.begin() + kSeg0Offset + 0x40, other.begin() + kSeg0Offset + 0x42) ==
          Bytes(out.begin() + kSeg0Offset + 0x40, out.begin() + kSeg0Offset + 0x42));
    CHECK(((other[kSeg0Offset + 0x44] << 8) | other[kSeg0Offset + 0x45]) == 0x0001 + 0x34);
    CHECK(((other[kSeg1Offset + 0x10] << 8) | other[kSeg1Offset + 0x11]) == 0x0002 + 0x34);

    // No records: only the header fields change.
    const Bytes bare = withRecords({}).build();
    CHECK(relocatePrx(bare, kBase, out, counts, image_end, error));
    CHECK(counts.total() == 0);
    CHECK(image_end == kBase + 0x1C0);
}

TOX2_TEST(relocPrxRefusals) {
    const SyntheticPrx good = withRecords(goodRecords());

    // Base.
    CHECK(refused(good.build(), 0, "base"));
    CHECK(refused(good.build(), kBase + 1, "base"));
    CHECK(refused(good.build(), 0x8000, "base"));
    CHECK(refused(good.build(), 0xFFFF0000, "does not fit"));

    // File and header shape.
    SyntheticPrx prx = good;
    prx.e_type = 0xFFA3;
    CHECK(refused(prx.build(), kBase, "e_type"));
    CHECK(refused(Bytes(10, 0), kBase, "not an ELF"));
    Bytes elf32 = good.build();
    elf32[4] = 1;
    CHECK(refused(elf32, kBase, "big-endian ELF64"));
    Bytes truncated = good.build();
    truncated.resize(kPhdrOffset + kPhdrSize);
    CHECK(refused(truncated, kBase, "past the end"));
    Bytes cut_records = good.build();
    cut_records.resize(cut_records.size() - 1);
    CHECK(refused(cut_records, kBase, "SCE_PPURELA data"));

    // SCE_PPURELA headers.
    prx = good;
    prx.rela_headers = 0;
    CHECK(refused(prx.build(), kBase, "no SCE_PPURELA"));
    prx.rela_headers = 2;
    CHECK(refused(prx.build(), kBase, "2 SCE_PPURELA"));

    // Segments.
    prx = good;
    prx.seg1_vaddr = 0xF0;
    prx.records.clear();
    CHECK(refused(prx.build(), kBase, "inside this segment's file data"));

    // Records; the message names the record and, for a value mismatch, the site and both values.
    CHECK(refused(withRecords({{.type = 2}}).build(), kBase, "record 0: unsupported relocation type 2"));
    CHECK(refused(withRecords({{.type = 1, .off_seg = 0, .sym_seg = 0, .r_offset = 0x20}, {.type = 7}}).build(), kBase,
                  "record 1: unsupported relocation type 7"));
    CHECK(refused(withRecords({{.type = 1, .r_offset = 0xFE}}).build(), kBase, "outside the file data"));
    CHECK(refused(withRecords({{.type = 4, .r_offset = 0xFF}}).build(), kBase, "outside the file data"));
    CHECK(refused(withRecords({{.type = 1, .r_offset = 0xFFFFFFFFFFFFFFFCull}}).build(), kBase,
                  "outside the file data"));
    CHECK(refused(withRecords({{.type = 1, .off_seg = 2}}).build(), kBase, "not a PT_LOAD"));
    CHECK(refused(withRecords({{.type = 1, .sym_seg = 2}}).build(), kBase, "not a PT_LOAD"));
    CHECK(refused(withRecords({{.type = 1, .off_seg = 9}}).build(), kBase, "program header index 9"));
    CHECK(refused(withRecords({{.type = 1, .sym_seg = 200}}).build(), kBase, "program header index 200"));
    CHECK(refused(withRecords({{.type = 1, .addend = -1}}).build(), kBase, "does not fit"));
    CHECK(refused(withRecords({{.type = 1, .addend = 0xFFFF0000}}).build(), kBase, "does not fit"));

    // The file must be pre-linked: a site that does not hold the link-time value is an error.
    CHECK(refused(withRecords({{.type = 1, .r_offset = 0x20, .addend = 0x110, .site = 0xDEADBEEF}}).build(), kBase,
                  "record 0: ADDR32 site 0x00000020 holds 0xdeadbeef, the pre-linked value is 0x00000110"));
    CHECK(refused(withRecords({{.type = 6, .off_seg = 1, .r_offset = 0x10, .addend = 0x18000, .site = 0x0001}}).build(),
                  kBase, "ADDR16_HA site 0x00000110 holds 0x0001, the pre-linked value is 0x0002"));

    // Two records for one site: the second finds the relocated value.
    CHECK(refused(withRecords({{.type = 1, .r_offset = 0x20, .addend = 0x110},
                               {.type = 1, .r_offset = 0x20, .addend = 0x110}})
                          .build(),
                  kBase, "record 1: ADDR32 site"));
}

TOX2_TEST(cliReloc) {
    const TempDir temp;
    const Bytes input = withRecords(goodRecords()).build();
    writeFile(temp.path("libsre.prx"), input);

    Bytes expected;
    RelocCounts counts;
    std::uint32_t image_end = 0;
    std::string error;
    CHECK(relocatePrx(input, kBase, expected, counts, image_end, error));

    const std::vector<std::string> valid = {"reloc",
                                            "--in",
                                            temp.path("libsre.prx"),
                                            "--base",
                                            "0x011d0000",
                                            "--out",
                                            temp.path("outdir/libsre.reloc.elf"),
                                            "--expect-size",
                                            std::to_string(expected.size()),
                                            "--expect-sha256",
                                            sha256Hex(expected)};

    // The matching pin, with and without the input pin.
    std::vector<std::string> args = valid;
    args.insert(args.end(), {"--expect-input-sha256", sha256Hex(input)});
    Result result = run(args);
    CHECK(result.code == 0);
    CHECK(result.err.empty());
    CHECK(result.out ==
          "relocated: 5 relocations (ADDR32 2, ADDR16_LO 1, ADDR16_HI 1, ADDR16_HA 1), image "
          "0x011d0000..0x011d01c0\n"
          "libsre.reloc.elf: " +
                  std::to_string(expected.size()) + " bytes, sha256 " + sha256Hex(expected) + ": matches the pin\n");
    CHECK(readBytes(temp.path("outdir/libsre.reloc.elf")) == expected);
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf.part"));

    fs::remove(temp.dir / "outdir" / "libsre.reloc.elf");
    result = run(valid);
    CHECK(result.code == 0);
    CHECK(contains(result.out, "sha256 " + sha256Hex(expected) + ": matches the pin"));

    // A wrong output pin: exit 1, the real size and hash are in the message, nothing is written.
    fs::remove(temp.dir / "outdir" / "libsre.reloc.elf");
    args = valid;
    std::string wrong = sha256Hex(expected);
    wrong.back() = wrong.back() == '0' ? '1' : '0';
    setOption(args, "--expect-sha256", wrong);
    result = run(args);
    CHECK(result.code == 1);
    CHECK(result.err.rfind("error: ", 0) == 0);
    CHECK(contains(result.err, "libsre.reloc.elf: " + std::to_string(expected.size()) + " bytes, sha256 " +
                                       sha256Hex(expected) + "; expected "));
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf"));
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf.part"));
    args = valid;
    setOption(args, "--expect-size", std::to_string(expected.size() + 1));
    CHECK(run(args).code == 1);
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf"));

    // A wrong input pin.
    args = valid;
    wrong = sha256Hex(input);
    wrong.back() = wrong.back() == '0' ? '1' : '0';
    args.insert(args.end(), {"--expect-input-sha256", wrong});
    result = run(args);
    CHECK(result.code == 1);
    CHECK(contains(result.err, "is not the pinned input " + wrong));
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf"));

    // Relocation failures are exit 1 as well: an unaligned base, a missing input.
    args = valid;
    setOption(args, "--base", "0x011d0001");
    result = run(args);
    CHECK(result.code == 1);
    CHECK(contains(result.err, "base"));
    args = valid;
    setOption(args, "--in", temp.path("absent.prx"));
    CHECK(run(args).code == 1);

    // A bad --base or a missing option is a usage error.
    for (const char* base : {"011d0000", "0x", "0xzz", "0x1ffffffff", "0x-1", "", "0x 1"}) {
        args = valid;
        setOption(args, "--base", base);
        result = run(args);
        CHECK(result.code == 2);
        CHECK(contains(result.err, "--base"));
    }
    args = valid;
    args.erase(args.begin() + 3, args.begin() + 5);
    result = run(args);
    CHECK(result.code == 2);
    CHECK(contains(result.err, "missing option --base"));
    CHECK(!fs::exists(temp.dir / "outdir" / "libsre.reloc.elf"));
}

}  // namespace tox2::decrypt

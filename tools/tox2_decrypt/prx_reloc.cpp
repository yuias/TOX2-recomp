#include "prx_reloc.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace tox2::decrypt {
namespace {

constexpr std::size_t kElfHeaderSize = 0x40;
constexpr std::size_t kProgramHeaderSize = 0x38;
constexpr std::size_t kRelaSize = 24;
constexpr std::uint16_t kPrxType = 0xFFA4;
constexpr std::uint16_t kMachinePpc64 = 21;
constexpr std::uint32_t kPtNull = 0;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtSceRela = 0x700000A4;
constexpr std::uint32_t kBaseAlignment = 0x10000;
constexpr std::uint64_t kAddressLimit = 0x100000000ull;

enum RelocType : std::uint32_t { kAddr32 = 1, kAddr16Lo = 4, kAddr16Hi = 5, kAddr16Ha = 6 };

struct Segment {
    std::uint32_t type = 0;
    std::uint64_t offset = 0, vaddr = 0, filesz = 0, memsz = 0;
};

void writeBe16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void writeBe32(std::uint8_t* p, std::uint32_t v) {
    writeBe16(p, static_cast<std::uint16_t>(v >> 16));
    writeBe16(p + 2, static_cast<std::uint16_t>(v));
}

void writeBe64(std::uint8_t* p, std::uint64_t v) {
    writeBe32(p, static_cast<std::uint32_t>(v >> 32));
    writeBe32(p + 4, static_cast<std::uint32_t>(v));
}

std::string hex(std::uint64_t value, int digits = 8) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%0*llx", digits, static_cast<unsigned long long>(value));
    return text;
}

const char* typeName(std::uint32_t type) {
    switch (type) {
        case kAddr32: return "ADDR32";
        case kAddr16Lo: return "ADDR16_LO";
        case kAddr16Hi: return "ADDR16_HI";
        case kAddr16Ha: return "ADDR16_HA";
        default: return "unknown";
    }
}

std::size_t siteSize(std::uint32_t type) { return type == kAddr32 ? 4 : 2; }

// The bytes a record leaves at its site for the 32-bit address `s`.
std::uint32_t siteValue(std::uint32_t type, std::uint32_t s) {
    switch (type) {
        case kAddr16Lo: return s & 0xFFFF;
        case kAddr16Hi: return (s >> 16) & 0xFFFF;
        case kAddr16Ha: return ((s + 0x8000) >> 16) & 0xFFFF;
        default: return s;
    }
}

bool fits(std::size_t file_size, std::uint64_t offset, std::uint64_t size) {
    return offset <= file_size && size <= file_size - offset;
}

}  // namespace

bool relocatePrx(std::span<const std::uint8_t> prx, std::uint32_t base, Bytes& out, RelocCounts& counts,
                 std::uint32_t& image_end, std::string& error) {
    out.clear();
    counts = RelocCounts{};
    image_end = 0;

    if (base == 0 || base % kBaseAlignment != 0) {
        error = "base " + hex(base) + " is not a non-zero multiple of 0x10000";
        return false;
    }
    if (prx.size() < kElfHeaderSize || prx[0] != 0x7F || prx[1] != 'E' || prx[2] != 'L' || prx[3] != 'F') {
        error = "PRX: not an ELF file";
        return false;
    }
    if (prx[4] != 2 || prx[5] != 2) {
        error = "PRX: not a big-endian ELF64 file";
        return false;
    }
    const std::uint16_t e_type = readBe16(prx.data() + 0x10);
    if (e_type != kPrxType) {
        error = "PRX: e_type " + hex(e_type, 4) + " is not a PRX (0xffa4)";
        return false;
    }
    if (readBe16(prx.data() + 0x12) != kMachinePpc64) {
        error = "PRX: e_machine is not PowerPC64";
        return false;
    }
    const std::uint64_t e_phoff = readBe64(prx.data() + 0x20);
    if (readBe16(prx.data() + 0x36) != kProgramHeaderSize) {
        error = "PRX: e_phentsize is not 0x38";
        return false;
    }
    const std::uint32_t phnum = readBe16(prx.data() + 0x38);
    if (!fits(prx.size(), e_phoff, static_cast<std::uint64_t>(phnum) * kProgramHeaderSize)) {
        error = "PRX: program headers are past the end of the file";
        return false;
    }

    std::vector<Segment> segments(phnum);
    std::vector<std::size_t> loads;
    std::vector<std::size_t> relas;
    for (std::size_t i = 0; i < phnum; i++) {
        const std::uint8_t* phdr = prx.data() + e_phoff + i * kProgramHeaderSize;
        Segment& segment = segments[i];
        segment.type = readBe32(phdr);
        segment.offset = readBe64(phdr + 0x08);
        segment.vaddr = readBe64(phdr + 0x10);
        segment.filesz = readBe64(phdr + 0x20);
        segment.memsz = readBe64(phdr + 0x28);
        if (segment.type == kPtLoad) {
            loads.push_back(i);
        } else if (segment.type == kPtSceRela) {
            relas.push_back(i);
        }
    }
    if (relas.size() != 1) {
        error = relas.empty() ? "PRX: no SCE_PPURELA program header (0x700000a4)"
                              : "PRX: " + std::to_string(relas.size()) + " SCE_PPURELA program headers, expected one";
        return false;
    }
    if (loads.empty()) {
        error = "PRX: no PT_LOAD program header";
        return false;
    }
    const std::size_t rela_index = relas[0];
    const Segment& rela = segments[rela_index];
    if (!fits(prx.size(), rela.offset, rela.filesz) || rela.filesz % kRelaSize != 0) {
        error = "PRX: SCE_PPURELA data is outside the file or not a whole number of 24-byte records";
        return false;
    }

    for (const std::size_t i : loads) {
        const Segment& segment = segments[i];
        if (!fits(prx.size(), segment.offset, segment.filesz)) {
            error = "PRX: program header " + std::to_string(i) + ": file data is past the end of the file";
            return false;
        }
        if (segment.vaddr >= kAddressLimit || segment.memsz >= kAddressLimit ||
            segment.vaddr + segment.memsz >= kAddressLimit - base) {
            error = "PRX: program header " + std::to_string(i) + ": segment does not fit below 4 GiB at base " +
                    hex(base);
            return false;
        }
    }

    // The unrelocated image lets one segment's memory size reach into the next one; the loaded image is
    // contiguous, so the overlap is cut off instead of loaded twice.
    std::vector<std::size_t> order = loads;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return segments[a].vaddr < segments[b].vaddr; });
    for (std::size_t k = 0; k + 1 < order.size(); k++) {
        Segment& segment = segments[order[k]];
        const std::uint64_t next_vaddr = segments[order[k + 1]].vaddr;
        if (segment.vaddr + segment.memsz > next_vaddr) {
            const std::uint64_t trimmed = next_vaddr - segment.vaddr;
            if (trimmed < segment.filesz) {
                error = "PRX: program header " + std::to_string(order[k]) + ": the next segment starts at " +
                        hex(next_vaddr) + ", inside this segment's file data";
                return false;
            }
            segment.memsz = trimmed;
        }
    }
    std::uint64_t end = 0;
    for (const std::size_t i : loads) {
        end = std::max(end, segments[i].vaddr + segments[i].memsz);
    }

    Bytes result(prx.begin(), prx.end());
    RelocCounts applied;
    const std::uint64_t record_count = rela.filesz / kRelaSize;
    for (std::uint64_t i = 0; i < record_count; i++) {
        const std::uint8_t* record = prx.data() + rela.offset + i * kRelaSize;
        const std::uint64_t r_offset = readBe64(record);
        const std::uint64_t r_info = readBe64(record + 8);
        const std::int64_t r_addend = static_cast<std::int64_t>(readBe64(record + 16));
        const std::string prefix = "relocation record " + std::to_string(i) + ": ";

        const std::uint32_t type = static_cast<std::uint32_t>(r_info);
        if (type != kAddr32 && type != kAddr16Lo && type != kAddr16Hi && type != kAddr16Ha) {
            error = prefix + "unsupported relocation type " + std::to_string(type);
            return false;
        }
        const std::uint32_t off_seg = static_cast<std::uint32_t>((r_info >> 32) & 0xFF);
        const std::uint32_t sym_seg = static_cast<std::uint32_t>((r_info >> 40) & 0xFF);
        for (const std::uint32_t index : {off_seg, sym_seg}) {
            if (index >= phnum || segments[index].type != kPtLoad) {
                error = prefix + "program header index " + std::to_string(index) + " is not a PT_LOAD header";
                return false;
            }
        }
        const Segment& site_segment = segments[off_seg];
        const std::size_t size = siteSize(type);
        if (r_offset > site_segment.filesz || size > site_segment.filesz - r_offset) {
            error = prefix + std::string(typeName(type)) + " site " + hex(site_segment.vaddr + r_offset) +
                    " is outside the file data of program header " + std::to_string(off_seg);
            return false;
        }

        // Addresses are 32-bit; the range check also keeps the sum below from overflowing.
        const std::int64_t sym_base = static_cast<std::int64_t>(segments[sym_seg].vaddr);
        if (r_addend < -sym_base || r_addend > static_cast<std::int64_t>(kAddressLimit - 1) - sym_base ||
            static_cast<std::uint64_t>(sym_base + r_addend) > kAddressLimit - 1 - base) {
            error = prefix + "target address does not fit below 4 GiB at base " + hex(base);
            return false;
        }
        const std::uint32_t s_link = static_cast<std::uint32_t>(sym_base + r_addend);
        const std::uint32_t s = s_link + base;

        std::uint8_t* site = result.data() + site_segment.offset + r_offset;
        const std::uint32_t found = size == 4 ? readBe32(site) : readBe16(site);
        const std::uint32_t expected = siteValue(type, s_link);
        if (found != expected) {
            error = prefix + std::string(typeName(type)) + " site " + hex(site_segment.vaddr + r_offset) + " holds " +
                    hex(found, static_cast<int>(size * 2)) + ", the pre-linked value is " +
                    hex(expected, static_cast<int>(size * 2));
            return false;
        }
        const std::uint32_t value = siteValue(type, s);
        if (size == 4) {
            writeBe32(site, value);
        } else {
            writeBe16(site, static_cast<std::uint16_t>(value));
        }
        switch (type) {
            case kAddr32: applied.addr32++; break;
            case kAddr16Lo: applied.addr16_lo++; break;
            case kAddr16Hi: applied.addr16_hi++; break;
            default: applied.addr16_ha++; break;
        }
    }

    for (const std::size_t i : loads) {
        std::uint8_t* phdr = result.data() + e_phoff + i * kProgramHeaderSize;
        writeBe64(phdr + 0x10, base + segments[i].vaddr);
        writeBe64(phdr + 0x28, segments[i].memsz);
    }
    writeBe32(result.data() + e_phoff + rela_index * kProgramHeaderSize, kPtNull);

    out = std::move(result);
    counts = applied;
    image_end = static_cast<std::uint32_t>(base + end);
    return true;
}

}  // namespace tox2::decrypt

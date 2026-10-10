#include "rsx/rsx_capture.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include "rsx/fifo_parser.h"
#include "rsx/rsx_unimplemented.h"

namespace tox2 {
namespace {

constexpr std::uint32_t fourcc(const char (&tag)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(tag[3])) << 24);
}

constexpr std::uint32_t kTagRegs = fourcc("REGS");
constexpr std::uint32_t kTagDisp = fourcc("DISP");
constexpr std::uint32_t kTagSurf = fourcc("SURF");
constexpr std::uint32_t kTagWrit = fourcc("WRIT");
constexpr std::uint32_t kTagMemr = fourcc("MEMR");
constexpr std::uint32_t kTagFlip = fourcc("FLIP");
constexpr std::uint32_t kTagEnd = fourcc("END ");

constexpr char kMagic[8] = {'T', 'X', '2', 'R', 'S', 'X', 'C', '1'};
constexpr std::size_t kHeaderSize = 8 + 4 + 8;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::size_t kSurfaceRecordSize = 6 * 4;
constexpr std::size_t kDisplayRecordSize = 4 * 4;
constexpr std::size_t kMemoryHeaderSize = 3 * 4;

// Version 2 appends the vertex default written mask after the load pointers. A version 1 chunk has no
// mask; the reader infers it from the default attribute registers.
constexpr std::size_t kRegsWordsBase = 0x4000 + kRsxVpInstructionCount * 4 + kRsxVpConstantCount * 4 + 3;
constexpr std::size_t kRegsBytesBase = kRegsWordsBase * 4;
constexpr std::size_t kRegsBytes = kRegsBytesBase + 4;
constexpr std::size_t kDispBytes = 4 + kRsxDisplayBufferCount * kDisplayRecordSize;

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    put32(out, static_cast<std::uint32_t>(value));
    put32(out, static_cast<std::uint32_t>(value >> 32));
}

void beginChunk(std::vector<std::uint8_t>& out, std::uint32_t tag, std::size_t size) {
    put32(out, tag);
    put32(out, static_cast<std::uint32_t>(size));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t get64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(get32(p)) | (static_cast<std::uint64_t>(get32(p + 4)) << 32);
}

}  // namespace

RsxCaptureWriter::RsxCaptureWriter(std::uint64_t flip_index, const RsxRegisters& regs,
                                   std::span<const RsxSurface> surfaces) {
    head_.insert(head_.end(), std::begin(kMagic), std::end(kMagic));
    put32(head_, kRsxCaptureVersion);
    put64(head_, flip_index);

    head_.reserve(head_.size() + kChunkHeaderSize + kRegsBytes);
    beginChunk(head_, kTagRegs, kRegsBytes);
    for (const std::uint32_t word : regs.regs) put32(head_, word);
    for (const std::uint32_t word : regs.vp_ucode) put32(head_, word);
    for (const std::uint32_t word : regs.vp_constants) put32(head_, word);
    put32(head_, regs.vp_load);
    put32(head_, regs.vp_start);
    put32(head_, regs.constant_load);
    put32(head_, regs.vertex_default_written);

    beginChunk(surfaces_, kTagSurf, 4 + surfaces.size() * kSurfaceRecordSize);
    put32(surfaces_, static_cast<std::uint32_t>(surfaces.size()));
    for (const RsxSurface& surface : surfaces) {
        put32(surfaces_, surface.location);
        put32(surfaces_, surface.offset);
        put32(surfaces_, surface.pitch);
        put32(surfaces_, surface.width);
        put32(surfaces_, surface.height);
        put32(surfaces_, static_cast<std::uint32_t>(surface.format));
    }
}

void RsxCaptureWriter::onWrite(std::uint32_t method, std::uint32_t value) {
    put32(pending_, method);
    put32(pending_, value);
    writes_++;
}

void RsxCaptureWriter::flushWrites() {
    if (pending_.empty()) return;
    beginChunk(stream_, kTagWrit, pending_.size());
    stream_.insert(stream_.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void RsxCaptureWriter::onMemory(std::uint32_t location, std::uint32_t offset, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    flushWrites();
    beginChunk(stream_, kTagMemr, kMemoryHeaderSize + bytes.size());
    put32(stream_, location);
    put32(stream_, offset);
    put32(stream_, static_cast<std::uint32_t>(bytes.size()));
    stream_.insert(stream_.end(), bytes.begin(), bytes.end());
    ranges_++;
}

void RsxCaptureWriter::onFlip(std::uint32_t buffer) {
    flushWrites();
    beginChunk(stream_, kTagFlip, 4);
    put32(stream_, buffer);
}

std::vector<std::uint8_t> RsxCaptureFile::flatten() const {
    std::vector<std::uint8_t> all;
    all.reserve(size());
    all.insert(all.end(), head.begin(), head.end());
    all.insert(all.end(), stream.begin(), stream.end());
    all.insert(all.end(), tail.begin(), tail.end());
    return all;
}

RsxCaptureFile RsxCaptureWriter::finish(const std::array<RsxDisplayBuffer, kRsxDisplayBufferCount>& display,
                                        std::uint32_t display_count) {
    flushWrites();
    RsxCaptureFile file;
    file.head = std::move(head_);
    beginChunk(file.head, kTagDisp, kDispBytes);
    put32(file.head, display_count);
    for (const RsxDisplayBuffer& buffer : display) {
        put32(file.head, buffer.offset);
        put32(file.head, buffer.pitch);
        put32(file.head, buffer.width);
        put32(file.head, buffer.height);
    }
    file.head.insert(file.head.end(), surfaces_.begin(), surfaces_.end());
    file.stream = std::move(stream_);
    beginChunk(file.tail, kTagEnd, 0);
    head_.clear();
    surfaces_.clear();
    stream_.clear();
    return file;
}

bool RsxRecordingMemoryView::read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) {
    if (!inner_.read(location, offset, out)) return false;
    if (writer_ != nullptr) writer_->onMemory(location, offset, out);
    return true;
}

bool RsxCaptureReader::parse(std::vector<std::uint8_t> bytes, std::string& error) {
    data_ = std::move(bytes);
    events_.clear();
    surfaces_.clear();
    regs_.reset();
    writes_ = ranges_ = 0;
    const std::uint8_t* base = data_.data();
    const std::size_t size = data_.size();

    if (size < kHeaderSize || std::memcmp(base, kMagic, sizeof(kMagic)) != 0) {
        error = "not a capture file (bad magic)";
        return false;
    }
    const std::uint32_t version = get32(base + 8);
    if (version != 1 && version != kRsxCaptureVersion) {
        error = std::format("unsupported capture version {}", version);
        return false;
    }
    flip_index_ = get64(base + 12);

    bool have_regs = false, have_disp = false, have_surf = false, have_end = false;
    std::size_t pos = kHeaderSize;
    while (!have_end) {
        if (size - pos < kChunkHeaderSize) {
            error = "truncated capture (chunk header)";
            return false;
        }
        const std::uint32_t tag = get32(base + pos);
        const std::size_t chunk_size = get32(base + pos + 4);
        pos += kChunkHeaderSize;
        if (chunk_size > size - pos) {
            error = std::format("truncated capture (chunk at {:#x} wants {} bytes)", pos - kChunkHeaderSize,
                                chunk_size);
            return false;
        }
        const std::uint8_t* chunk = base + pos;

        if (tag == kTagRegs) {
            // Version 1 has no written mask; version 2 must have it.
            if (have_regs || chunk_size != (version == 1 ? kRegsBytesBase : kRegsBytes)) {
                error = "bad REGS chunk";
                return false;
            }
            regs_ = std::make_unique<RsxRegisters>();
            const std::uint8_t* p = chunk;
            const auto take = [&p]() {
                const std::uint32_t word = get32(p);
                p += 4;
                return word;
            };
            for (std::uint32_t& word : regs_->regs) word = take();
            for (std::uint32_t& word : regs_->vp_ucode) word = take();
            for (std::uint32_t& word : regs_->vp_constants) word = take();
            regs_->vp_load = take();
            regs_->vp_start = take();
            regs_->constant_load = take();
            if (version != 1) {
                regs_->vertex_default_written = take();
            } else {
                for (std::uint32_t slot = 0; slot < 16; slot++) {
                    for (std::uint32_t c = 0; c < 4; c++) {
                        if (regs_->regs[(kMethodNv4097SetVertexData4fM >> 2) + slot * 4 + c] != 0) {
                            regs_->vertex_default_written |= 1u << slot;
                        }
                    }
                }
            }
            have_regs = true;
        } else if (tag == kTagDisp) {
            if (have_disp || chunk_size != kDispBytes) {
                error = "bad DISP chunk";
                return false;
            }
            display_count_ = get32(chunk);
            for (std::uint32_t i = 0; i < kRsxDisplayBufferCount; i++) {
                const std::uint8_t* p = chunk + 4 + i * kDisplayRecordSize;
                display_[i] = {get32(p), get32(p + 4), get32(p + 8), get32(p + 12)};
            }
            have_disp = true;
        } else if (tag == kTagSurf) {
            if (have_surf || chunk_size < 4) {
                error = "bad SURF chunk";
                return false;
            }
            const std::uint32_t count = get32(chunk);
            if (count > kRsxMaxKnownSurfaces ||
                chunk_size != 4 + static_cast<std::size_t>(count) * kSurfaceRecordSize) {
                error = "bad SURF chunk";
                return false;
            }
            for (std::uint32_t i = 0; i < count; i++) {
                const std::uint8_t* p = chunk + 4 + i * kSurfaceRecordSize;
                const std::uint32_t format = get32(p + 20);
                if (format > static_cast<std::uint32_t>(RsxColorFormat::Unsupported)) {
                    error = "bad surface format in SURF chunk";
                    return false;
                }
                surfaces_.push_back({get32(p), get32(p + 4), get32(p + 8), get32(p + 12), get32(p + 16),
                                     static_cast<RsxColorFormat>(format)});
            }
            have_surf = true;
        } else if (tag == kTagWrit) {
            if (!have_regs || chunk_size % 8 != 0) {
                error = "bad WRIT chunk";
                return false;
            }
            for (std::size_t i = 0; i < chunk_size; i += 8) {
                events_.push_back({EventKind::Write, get32(chunk + i), get32(chunk + i + 4), 0, 0});
            }
            writes_ += chunk_size / 8;
        } else if (tag == kTagMemr) {
            if (!have_regs || chunk_size < kMemoryHeaderSize) {
                error = "bad MEMR chunk";
                return false;
            }
            const std::uint32_t location = get32(chunk);
            const std::uint32_t offset = get32(chunk + 4);
            const std::uint32_t length = get32(chunk + 8);
            if (location > 1 || chunk_size != kMemoryHeaderSize + static_cast<std::size_t>(length)) {
                error = "bad MEMR chunk";
                return false;
            }
            events_.push_back({EventKind::Memory, location, offset, length, pos + kMemoryHeaderSize});
            ranges_++;
        } else if (tag == kTagFlip) {
            if (!have_regs || chunk_size != 4) {
                error = "bad FLIP chunk";
                return false;
            }
            events_.push_back({EventKind::Flip, get32(chunk), 0, 0, 0});
        } else if (tag == kTagEnd) {
            if (chunk_size != 0) {
                error = "bad END chunk";
                return false;
            }
            have_end = true;
        } else {
            error = std::format("unknown chunk tag {:#010x}", tag);
            return false;
        }
        pos += chunk_size;
    }
    if (pos != size) {
        error = "trailing bytes after END";
        return false;
    }
    if (!have_regs || !have_disp || !have_surf) {
        error = "capture lacks REGS, DISP or SURF";
        return false;
    }
    return true;
}

bool RsxCaptureReader::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open " + path.generic_string();
        return false;
    }
    const std::streamsize length = file.tellg();
    if (length < 0) {
        error = "cannot size " + path.generic_string();
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        error = "cannot read " + path.generic_string();
        return false;
    }
    return parse(std::move(bytes), error);
}

void ReplayRsxMemoryView::add(std::uint32_t location, std::uint32_t offset, std::span<const std::uint8_t> bytes) {
    if (location > 1) return;
    auto& pages = pages_[location];
    std::uint64_t pos = offset;
    std::size_t done = 0;
    while (done < bytes.size() && pos <= 0xFFFFFFFFull) {
        const std::uint32_t index = static_cast<std::uint32_t>(pos / kPageSize);
        const std::uint32_t in_page = static_cast<std::uint32_t>(pos % kPageSize);
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - done, kPageSize - in_page);
        std::unique_ptr<Page>& page = pages[index];
        if (!page) page = std::make_unique<Page>();
        std::memcpy(page->data.data() + in_page, bytes.data() + done, chunk);
        if (page->valid_count != kPageSize) {
            for (std::size_t i = 0; i < chunk; i++) {
                if (!page->valid[in_page + i]) {
                    page->valid[in_page + i] = true;
                    page->valid_count++;
                }
            }
        }
        done += chunk;
        pos += chunk;
    }
}

bool ReplayRsxMemoryView::read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) {
    if (out.empty()) return true;
    const auto missing = [&]() {
        rsxNoteUnimplemented("capture", "missing range");
        return false;
    };
    if (location > 1 || static_cast<std::uint64_t>(offset) + out.size() > 0x100000000ull) return missing();
    const auto& pages = pages_[location];
    std::uint64_t pos = offset;
    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint32_t index = static_cast<std::uint32_t>(pos / kPageSize);
        const std::uint32_t in_page = static_cast<std::uint32_t>(pos % kPageSize);
        const std::size_t chunk = std::min<std::size_t>(out.size() - done, kPageSize - in_page);
        const auto it = pages.find(index);
        if (it == pages.end()) return missing();
        const Page& page = *it->second;
        if (page.valid_count != kPageSize) {
            for (std::size_t i = 0; i < chunk; i++) {
                if (!page.valid[in_page + i]) return missing();
            }
        }
        std::memcpy(out.data() + done, page.data.data() + in_page, chunk);
        done += chunk;
        pos += chunk;
    }
    return true;
}

void replayCapture(const RsxCaptureReader& reader, ReplayRsxMemoryView& memory, RsxFrameBuilder& builder) {
    auto regs = std::make_unique<RsxRegisters>(reader.startRegisters());
    builder.setKnownSurfaces(reader.surfaces());
    builder.setFlipIndex(reader.flipIndex());
    for (const RsxCaptureReader::Event& event : reader.events()) {
        switch (event.kind) {
            case RsxCaptureReader::EventKind::Write:
                regs->apply(event.a, event.b);
                builder.onWrite(*regs, event.a, event.b);
                break;
            case RsxCaptureReader::EventKind::Memory: memory.add(event.a, event.b, reader.memoryBytes(event)); break;
            case RsxCaptureReader::EventKind::Flip: {
                static constexpr RsxDisplayBuffer kNoBuffer{};
                builder.onFlip(*regs, event.a,
                               event.a < kRsxDisplayBufferCount ? reader.display()[event.a] : kNoBuffer);
                regs->contextReset();
                break;
            }
        }
    }
}

bool writeCaptureFile(const std::filesystem::path& path, const RsxCaptureFile& capture, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "cannot create " + path.parent_path().generic_string() + ": " + ec.message();
            return false;
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open " + path.generic_string() + " for writing";
        return false;
    }
    for (const std::vector<std::uint8_t>* part : {&capture.head, &capture.stream, &capture.tail}) {
        file.write(reinterpret_cast<const char*>(part->data()), static_cast<std::streamsize>(part->size()));
    }
    file.close();
    if (!file) {
        error = "cannot write " + path.generic_string();
        return false;
    }
    return true;
}

}  // namespace tox2

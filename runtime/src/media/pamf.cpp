#include "media/pamf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <utility>

namespace tox2 {

namespace {

constexpr std::size_t kHeaderUnit = 2048;  // header and data sizes are counted in these
constexpr std::size_t kPackSize = 2048;
constexpr std::size_t kFixedHeaderSize = 0x88;  // up to and including the stream count
constexpr std::size_t kStreamEntrySize = 48;
constexpr std::size_t kCodecInfoSize = 32;

constexpr std::uint8_t kPrivateStream1 = 0xBD;
constexpr std::uint8_t kProgramEnd = 0xB9;
constexpr std::uint8_t kPackStart = 0xBA;
constexpr std::uint8_t kFirstStreamId = 0xBB;  // system header; every later id carries a 16-bit length

// ATRAC3plus frame header: sync word, then sample rate / channel configuration / size code.
constexpr std::size_t kAtracHeaderSize = 8;
constexpr std::size_t kAtracMaxSizeCode = 0x1FF;
constexpr std::int64_t kAtracFrameTicks48k = 3840;
constexpr std::int64_t kAtracFrameTicks44k = 4180;
constexpr std::size_t kPrivateHeaderSize = 4;

std::uint32_t be16(const std::uint8_t* p) { return (std::uint32_t{p[0]} << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }
std::uint64_t be48(const std::uint8_t* p) { return (std::uint64_t{be16(p)} << 32) | be32(p + 2); }

bool isVideoId(std::uint8_t id) { return (id & 0xF0) == 0xE0; }

// Maps a coding type byte to the codec; false for any value the format does not define.
bool codecFromCodingType(std::uint8_t coding_type, PamfCodec& codec) {
    switch (coding_type) {
        case 0x1B: codec = PamfCodec::Avc; return true;
        case 0x02: codec = PamfCodec::M2v; return true;
        case 0xDC: codec = PamfCodec::Atrac3plus; return true;
        case 0x80: codec = PamfCodec::Lpcm; return true;
        case 0x81: codec = PamfCodec::Ac3; return true;
        case 0xDD: codec = PamfCodec::UserData; return true;
        default: return false;
    }
}

// High nibble of the private-stream-1 sub-stream id per audio/user-data codec.
std::uint8_t subStreamBase(PamfCodec codec) {
    switch (codec) {
        case PamfCodec::Atrac3plus: return 0x00;
        case PamfCodec::UserData: return 0x20;
        case PamfCodec::Ac3: return 0x30;
        default: return 0x40;  // Lpcm
    }
}

bool isVideoCodec(PamfCodec codec) { return codec == PamfCodec::Avc || codec == PamfCodec::M2v; }

// 33-bit PES timestamp: 4-bit prefix, then 3 + 15 + 15 bits, each group followed by a marker bit.
bool parseTimestamp(const std::uint8_t* p, std::uint8_t prefix, std::int64_t& out) {
    if ((p[0] & 0xF1) != ((prefix << 4) | 1) || (p[2] & 1) == 0 || (p[4] & 1) == 0) return false;
    out = (std::int64_t{(p[0] >> 1) & 7} << 30) | (std::int64_t{p[1]} << 22) | (std::int64_t{p[2] >> 1} << 15) |
          (std::int64_t{p[3]} << 7) | (p[4] >> 1);
    return true;
}

}  // namespace

std::expected<PamfInfo, std::string> parsePamfHeader(std::span<const std::uint8_t> header) {
    const std::uint8_t* h = header.data();
    if (header.size() < kFixedHeaderSize) return std::unexpected("header too short");
    if (std::memcmp(h, "PAMF", 4) != 0) return std::unexpected("not a PAMF file");
    if (std::memcmp(h + 4, "0040", 4) != 0 && std::memcmp(h + 4, "0041", 4) != 0)
        return std::unexpected("unsupported PAMF version");

    const std::uint32_t header_units = be32(h + 0x08);
    const std::uint32_t data_units = be32(h + 0x0C);
    if (header_units == 0) return std::unexpected("zero header size");
    const std::uint64_t header_bytes = std::uint64_t{header_units} * kHeaderUnit;
    const std::uint64_t data_bytes = std::uint64_t{data_units} * kHeaderUnit;
    if (header_bytes > UINT32_MAX || data_bytes > UINT32_MAX) return std::unexpected("sizes do not fit in 32 bits");

    const std::uint32_t seq_size = be32(h + 0x50);
    if (std::uint64_t{0x54} + seq_size > header.size() || seq_size < kFixedHeaderSize - 0x54)
        return std::unexpected("invalid sequence info size");

    const std::uint64_t start_pts = be48(h + 0x56);
    const std::uint64_t end_pts = be48(h + 0x5C);
    if (end_pts > UINT32_MAX || start_pts >= end_pts) return std::unexpected("invalid presentation interval");

    const std::uint32_t stream_count = h[0x87];
    if (be32(h + 0x6A) != stream_count) return std::unexpected("stream count mismatch");
    if (h[0x6F] != 1 || h[0x81] != 1) return std::unexpected("unsupported grouping period or group count");
    if (stream_count == 0) return std::unexpected("no streams");
    if (std::uint64_t{kFixedHeaderSize} + std::uint64_t{kStreamEntrySize} * stream_count >
        0x54 + std::uint64_t{seq_size})
        return std::unexpected("stream table exceeds sequence info");
    const std::uint32_t period_size = be32(h + 0x70);
    const std::uint32_t group_size = be32(h + 0x82);
    if (std::uint64_t{0x74} + period_size > header.size() || std::uint64_t{0x86} + group_size > header.size())
        return std::unexpected("invalid grouping period or group size");
    // Each block must fit in its parent: the period after the sequence info's reserved fields, the
    // group after the period's own fields.
    if (std::uint64_t{period_size} + 0x20 > seq_size || std::uint64_t{group_size} + 0x12 > period_size)
        return std::unexpected("grouping period or group does not fit in its parent");
    if (be48(h + 0x74) != start_pts) return std::unexpected("grouping period start differs from the start PTS");

    PamfInfo info;
    info.header_size = static_cast<std::uint32_t>(header_bytes);
    info.data_offset = static_cast<std::uint32_t>(header_bytes);
    info.data_size = static_cast<std::uint32_t>(data_bytes);
    info.start_pts = start_pts;
    info.end_pts = end_pts;
    info.mux_rate_bound = be32(h + 0x62) & 0x3FFFFF;

    std::array<bool, 6 * 16> seen{};       // (codec, channel) pairs may appear only once
    std::array<bool, 16> video_id_used{};  // AVC and MPEG-2 video share the 0xE0-0xEF PES stream ids
    for (std::uint32_t i = 0; i < stream_count; i++) {
        const std::uint8_t* e = h + kFixedHeaderSize + kStreamEntrySize * i;
        PamfStream s;
        if (!codecFromCodingType(e[0], s.codec)) return std::unexpected("unknown coding type");
        s.stream_id = e[4];
        s.private_stream_id = e[5];
        if (isVideoCodec(s.codec)) {
            if (!isVideoId(s.stream_id)) return std::unexpected("video stream with a non-video stream id");
            s.channel = s.stream_id & 0x0F;
            if (video_id_used[s.channel]) return std::unexpected("two video streams share a PES stream id");
            video_id_used[s.channel] = true;
        } else {
            if (s.stream_id != kPrivateStream1 || (s.private_stream_id & 0xF0) != subStreamBase(s.codec))
                return std::unexpected("audio or user-data stream id does not match its coding type");
            s.channel = s.private_stream_id & 0x0F;
        }
        if (seen[static_cast<std::size_t>(s.codec) * 16 + s.channel])
            return std::unexpected("duplicate stream channel");
        seen[static_cast<std::size_t>(s.codec) * 16 + s.channel] = true;

        const std::uint8_t* c = e + 0x10;
        s.codec_info.assign(c, c + kCodecInfoSize);
        if (isVideoCodec(s.codec)) {
            s.width = std::uint32_t{c[0x09]} * 16;
            s.height = std::uint32_t{c[0x0B]} * 16;
        } else {
            s.channels = c[0x02] & 0x0F;
            s.sample_rate = (c[0x03] & 0x0F) == 1 ? 48000 : 0;
        }
        info.streams.push_back(std::move(s));
    }
    return info;
}

struct PamfDemuxer::Impl {
    // ATRAC3plus stream reassembled across PES packets. `buf` holds unconsumed bytes starting at
    // absolute offset `base`; a frame can start in one packet and end in a later one.
    struct AudioState {
        std::vector<std::uint8_t> buf;
        std::size_t head = 0;
        std::uint64_t base = 0;
        std::deque<std::pair<std::uint64_t, std::int64_t>> stamps;  // payload start offset, PTS
        std::int64_t last_pts = -1;
        bool started = false;    // a frame has been seen; bytes skipped after that are damage
        bool resyncing = false;  // damage was counted and no frame has been delivered since
    };

    struct StreamState {
        PamfCodec codec = PamfCodec::UserData;
        std::deque<PamfAccessUnit> out;
        AudioState audio;
    };

    explicit Impl(const PamfInfo& info) {
        by_video_channel.fill(-1);
        by_sub_stream.fill(-1);
        streams.resize(info.streams.size());
        for (std::size_t i = 0; i < info.streams.size(); i++) {
            const PamfStream& s = info.streams[i];
            streams[i].codec = s.codec;
            if (isVideoCodec(s.codec)) {
                by_video_channel[s.stream_id & 0x0F] = static_cast<int>(i);
            } else {
                by_sub_stream[s.private_stream_id] = static_cast<int>(i);
            }
        }
    }

    void feed(std::span<const std::uint8_t> bytes) {
        if (ended) return;
        // Whole sections are processed straight from the caller's buffer; only a split section is copied.
        if (!pending.empty()) {
            const std::size_t take = std::min(kPackSize - pending.size(), bytes.size());
            pending.insert(pending.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take));
            bytes = bytes.subspan(take);
            if (pending.size() < kPackSize) return;
            processSection(pending);
            pending.clear();
        }
        while (bytes.size() >= kPackSize) {
            processSection(bytes.first(kPackSize));
            bytes = bytes.subspan(kPackSize);
        }
        pending.assign(bytes.begin(), bytes.end());
    }

    void endOfStream() {
        if (ended) return;
        // A short last section is parsed as far as it goes and counts as one error, whatever it was cut off in.
        if (!pending.empty()) {
            const std::uint64_t before = errors;
            processSection(pending);
            errors = before + 1;
            pending.clear();
        }
        ended = true;
        flushAudio();
    }

    void processSection(std::span<const std::uint8_t> s) {
        if (s.size() >= 4 && s[0] == 0 && s[1] == 0 && s[2] == 1) {
            if (s[3] == kProgramEnd) return flushAudio();
            if (s[3] == kPackStart) return processPack(s);
        }
        errors++;
    }

    void processPack(std::span<const std::uint8_t> s) {
        // Only the MPEG-2 pack header (marker '01') exists in PAMF streams.
        if (s.size() < 14 || (s[4] & 0xC0) != 0x40) {
            errors++;
            return;
        }
        std::size_t pos = 14 + (s[13] & 7);
        // Walk every packet by its length; system header, padding and private stream 2 are skipped this way.
        while (pos < s.size()) {
            const std::size_t rest = s.size() - pos;
            if (rest < 4 || s[pos] != 0 || s[pos + 1] != 0 || s[pos + 2] != 1) {
                errors++;
                return;
            }
            const std::uint8_t id = s[pos + 3];
            if (id == kProgramEnd) return flushAudio();
            if (id < kFirstStreamId || rest < 6) {  // includes a second pack start code
                errors++;
                return;
            }
            std::size_t total = 6 + std::size_t{be16(&s[pos + 4])};
            // A zero length is only meaningful for video, where it means "up to the end of the pack".
            if (total == 6 && isVideoId(id)) total = rest;
            if (total > rest) {
                errors++;
                return;
            }
            if (isVideoId(id) || id == kPrivateStream1) handlePes(s.subspan(pos, total));
            pos += total;
        }
    }

    void handlePes(std::span<const std::uint8_t> pkt) {
        const std::uint8_t id = pkt[3];
        int video_index = -1;
        if (isVideoId(id)) {
            video_index = by_video_channel[id & 0x0F];
            if (video_index < 0) return;  // not a stream of this file: skipped without parsing
        }

        if (pkt.size() < 9 || (pkt[6] & 0xC0) != 0x80) {
            errors++;
            return;
        }
        const unsigned pts_dts_flags = pkt[7] >> 6;
        const std::size_t header_len = pkt[8];
        const std::size_t payload_off = 9 + header_len;
        const std::size_t needed = pts_dts_flags == 2 ? 5 : pts_dts_flags == 3 ? 10 : 0;
        if (pts_dts_flags == 1 || header_len < needed || payload_off > pkt.size()) {
            errors++;
            return;
        }
        std::int64_t pts = -1, dts = -1;
        if (needed != 0) {
            const bool ok = parseTimestamp(&pkt[9], pts_dts_flags == 3 ? 3 : 2, pts) &&
                            (pts_dts_flags != 3 || parseTimestamp(&pkt[14], 1, dts));
            if (!ok) {
                errors++;  // the payload is still usable without a label
                pts = dts = -1;
            }
        }
        const std::span<const std::uint8_t> payload = pkt.subspan(payload_off);

        if (video_index >= 0) {
            if (payload.empty()) return;
            PamfAccessUnit au;
            au.stream_index = static_cast<std::uint32_t>(video_index);
            au.pts = pts;
            au.dts = dts;
            au.bytes.assign(payload.begin(), payload.end());
            streams[static_cast<std::size_t>(video_index)].out.push_back(std::move(au));
            return;
        }

        if (payload.size() < kPrivateHeaderSize) {
            errors++;
            return;
        }
        const std::uint8_t sub_id = payload[0];
        const std::uint8_t kind = sub_id & 0xF0;
        if (kind != 0x00 && kind != 0x20 && kind != 0x30 && kind != 0x40) {
            errors++;
            return;
        }
        const int index = by_sub_stream[sub_id];
        // LPCM, AC-3 and user data are not used by any known PAMF file; their packets are dropped.
        if (index < 0 || streams[static_cast<std::size_t>(index)].codec != PamfCodec::Atrac3plus) return;
        appendAudio(static_cast<std::uint32_t>(index), payload.subspan(kPrivateHeaderSize), pts);
    }

    void appendAudio(std::uint32_t index, std::span<const std::uint8_t> data, std::int64_t pts) {
        AudioState& a = streams[index].audio;
        if (pts >= 0) a.stamps.emplace_back(a.base + a.buf.size(), pts);
        a.buf.insert(a.buf.end(), data.begin(), data.end());
        drainAudio(index, false);
    }

    void flushAudio() {
        for (std::uint32_t i = 0; i < streams.size(); i++) {
            if (streams[i].codec != PamfCodec::Atrac3plus) continue;
            drainAudio(i, true);
            streams[i].audio = AudioState{};  // a following program starts its own timeline
        }
    }

    // One error per resynchronisation episode, i.e. until the next frame is delivered; the search for
    // the next sync word would otherwise count the same damage repeatedly.
    void countAudioDamage(AudioState& a) {
        if (!a.resyncing) errors++;
        a.resyncing = true;
        a.started = true;
    }

    // Cuts complete frames out of the buffer. A frame is complete when the next sync word follows at
    // its computed end, or, with `final`, when its last byte is also the last byte of the stream.
    void drainAudio(std::uint32_t index, bool final) {
        AudioState& a = streams[index].audio;
        for (;;) {
            const std::size_t avail = a.buf.size() - a.head;
            const std::uint8_t* p = a.buf.data() + a.head;
            if (avail < 2) break;
            if (p[0] != 0x0F || p[1] != 0xD0) {
                // Data before the first sync word is discarded without complaint; later it means damage.
                const std::uint8_t* end = a.buf.data() + a.buf.size();
                const std::uint8_t* s = p + 1;
                while (s + 1 < end && !(s[0] == 0x0F && s[1] == 0xD0)) s++;
                if (a.started) countAudioDamage(a);
                if (s + 1 >= end) {
                    // Keep a trailing 0x0F: its sync partner may arrive with the next packet.
                    a.head = a.buf.size() - (end[-1] == 0x0F ? 1 : 0);
                    break;
                }
                a.head += static_cast<std::size_t>(s - p);
                continue;
            }
            if (avail < 4) break;

            const std::uint32_t params = be16(p + 2);
            const std::uint32_t rate_index = params >> 13;
            const std::uint32_t channel_config = (params >> 10) & 7;
            const std::size_t size_code = params & 0x3FF;
            if ((rate_index != 1 && rate_index != 2) || channel_config == 0 || size_code > kAtracMaxSizeCode) {
                countAudioDamage(a);
                a.head++;
                continue;
            }
            // Distance to the next sync word: payload of (size code + 1) * 8 bytes plus the 8-byte header.
            const std::size_t total = (size_code + 1) * 8 + kAtracHeaderSize;

            bool complete = false;
            if (avail >= total + 2) {
                if (p[total] == 0x0F && p[total + 1] == 0xD0) {
                    complete = true;
                } else {
                    // Size mismatch: the candidate is dropped and the search restarts one byte after its start.
                    // The description restarts one byte after the start of the current PES payload instead;
                    // that is the same on valid input (no mismatch occurs) and would here need PES boundaries
                    // in the reassembled buffer and could re-deliver bytes of frames already emitted.
                    countAudioDamage(a);
                    a.head++;
                    continue;
                }
            } else if (final && avail >= total) {
                complete = true;  // the stream ends at, or just after, the last frame
            }
            if (!complete) break;

            emitFrame(index, total, rate_index);
            a.started = true;
            a.resyncing = false;
        }

        if (final) {
            if (a.head < a.buf.size() && a.started) countAudioDamage(a);  // cut-off frame at the end of the stream
            a.head = a.buf.size();
        }
        if (a.head == a.buf.size() || a.head >= (1u << 16)) {
            a.base += a.head;
            a.buf.erase(a.buf.begin(), a.buf.begin() + static_cast<std::ptrdiff_t>(a.head));
            a.head = 0;
        }
    }

    void emitFrame(std::uint32_t index, std::size_t total, std::uint32_t rate_index) {
        AudioState& a = streams[index].audio;
        const std::uint64_t start = a.base + a.head;
        // The PTS of the first frame starting in a packet (or the next one after it) labels that frame.
        std::int64_t pts = -1;
        while (!a.stamps.empty() && a.stamps.front().first <= start) {
            pts = a.stamps.front().second;
            a.stamps.pop_front();
        }
        if (pts < 0 && a.last_pts >= 0)
            pts = a.last_pts + (rate_index == 1 ? kAtracFrameTicks44k : kAtracFrameTicks48k);
        a.last_pts = pts;

        PamfAccessUnit au;
        au.stream_index = index;
        au.pts = pts;
        au.bytes.assign(a.buf.begin() + static_cast<std::ptrdiff_t>(a.head),
                        a.buf.begin() + static_cast<std::ptrdiff_t>(a.head + total));
        streams[index].out.push_back(std::move(au));
        a.head += total;
    }

    std::array<int, 16> by_video_channel;
    std::array<int, 256> by_sub_stream;
    std::vector<StreamState> streams;
    std::vector<std::uint8_t> pending;  // start of a section that has not been fully fed yet
    std::uint64_t errors = 0;
    bool ended = false;
};

PamfDemuxer::PamfDemuxer(const PamfInfo& info) : impl_(std::make_unique<Impl>(info)) {}
PamfDemuxer::~PamfDemuxer() = default;

void PamfDemuxer::feed(std::span<const std::uint8_t> bytes) { impl_->feed(bytes); }
void PamfDemuxer::endOfStream() { impl_->endOfStream(); }

std::optional<PamfAccessUnit> PamfDemuxer::next(std::uint32_t stream_index) {
    if (stream_index >= impl_->streams.size()) return std::nullopt;
    auto& out = impl_->streams[stream_index].out;
    if (out.empty()) return std::nullopt;
    PamfAccessUnit au = std::move(out.front());
    out.pop_front();
    return au;
}

std::uint64_t PamfDemuxer::errors() const { return impl_->errors; }

}  // namespace tox2

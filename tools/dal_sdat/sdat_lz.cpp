#include "sdat_lz.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace dal::sdat {

namespace {

constexpr std::size_t kHeaderSize = 5;
constexpr std::size_t kTableSize = 0xCA8;
constexpr std::uint8_t kStoredThreshold = 0x80;
constexpr std::uint8_t kProbInit = 0x80;
constexpr std::uint32_t kNormalizeBelow = 1u << 24;

constexpr std::size_t kMatchFlagBase = 0xB68;
constexpr std::size_t kLengthBase = 0xBA8;
constexpr std::size_t kLongDistTreeBase = 0x7F1;
constexpr std::size_t kShortDistTreeBase = 0x8E9;
constexpr std::size_t kDistExtraBase = 0x928;
constexpr std::uint64_t kLongTreeLimit = 22;
constexpr std::uint64_t kShortTreeLimit = 4;
constexpr std::uint64_t kEndMarker = 255;

// The probability table is one flat array on purpose: several regions overlap and share state.
class RangeDecoder {
public:
    RangeDecoder(std::span<const std::uint8_t> in, std::uint32_t code) : in_(in), pos_(kHeaderSize), code_(code) {
        probs_.fill(kProbInit);
    }

    // The probability tracks a 1 bit (the inverse of the usual convention).
    unsigned adaptiveBit(std::size_t addr) {
        normalize();
        std::uint32_t p = probs_[addr];
        const std::uint32_t bound = (range_ >> 8) * p;
        p -= p >> 3;
        unsigned bit;
        if (code_ < bound) {
            bit = 1;
            range_ = bound;
            p += 31;
        } else {
            bit = 0;
            code_ -= bound;
            range_ -= bound;
        }
        probs_[addr] = static_cast<std::uint8_t>(p);
        return bit;
    }

    unsigned directBit() {
        range_ >>= 1;
        if (code_ < range_) return 1;
        code_ -= range_;
        return 0;
    }

    // A single step, not a loop: direct bits can leave `range` below the threshold afterwards.
    void normalize() {
        if (range_ < kNormalizeBelow) {
            range_ <<= 8;
            code_ = (code_ << 8) | nextByte();
        }
    }

    // Value coding shared by lengths and distances: `cls + 1` bits behind a leading 1. The leading bits
    // use the context at base + high_offset, the last up-to-three bits sit at base + i * stride.
    std::uint64_t classBits(int cls, std::size_t base, std::size_t stride, std::size_t high_offset) {
        std::uint64_t acc = 1;
        if (cls >= 3) acc = acc * 2 + adaptiveBit(base + high_offset);
        if (cls >= 4) acc = acc * 2 + adaptiveBit(base + high_offset);
        if (cls >= 5) {
            normalize();
            for (int i = 0; i < cls - 4; i++) acc = acc * 2 + directBit();
        }
        acc = acc * 2 + adaptiveBit(base);
        if (cls >= 1) acc = acc * 2 + adaptiveBit(base + stride);
        if (cls >= 2) acc = acc * 2 + adaptiveBit(base + 2 * stride);
        return acc;
    }

private:
    std::uint8_t nextByte() { return pos_ < in_.size() ? in_[pos_++] : (pos_++, std::uint8_t{0}); }

    std::span<const std::uint8_t> in_;
    std::size_t pos_;
    std::uint32_t range_ = 0xFFFFFFFFu;
    std::uint32_t code_;
    std::array<std::uint8_t, kTableSize> probs_;
};

}  // namespace

std::int64_t lzDecompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    if (in.size() < kHeaderSize) return -1;
    const std::uint8_t mode = in[0];
    const std::uint32_t value = (std::uint32_t{in[1]} << 24) | (std::uint32_t{in[2]} << 16) |
                                (std::uint32_t{in[3]} << 8) | std::uint32_t{in[4]};
    const std::uint64_t capacity = out.size();

    if (mode > kStoredThreshold) {
        if (value > capacity || kHeaderSize + std::uint64_t{value} > in.size()) return -1;
        if (value != 0) std::memcpy(out.data(), in.data() + kHeaderSize, value);
        return value;
    }

    RangeDecoder rc(in, value);
    std::uint64_t pos = 0;
    std::uint8_t last = 0;
    unsigned state = 0;

    for (;;) {
        if (rc.adaptiveBit(kMatchFlagBase + state) == 0) {
            if (state > 0) state--;
            if (pos == capacity) return static_cast<std::int64_t>(pos);
            // Mode bytes of 11 and above shift every bit out, which leaves a constant context.
            const unsigned window = static_cast<unsigned>((pos % 8) * 256 + last);
            const unsigned ctx = mode >= 11 ? 0u : (window >> mode) & 7u;
            std::size_t node = 1;
            while (node < 256) node = node * 2 + rc.adaptiveBit(255 * ctx + node - 1);
            last = static_cast<std::uint8_t>(node);
            out[pos++] = last;
            continue;
        }

        // Length class: unary code over the flags at m = 1..7.
        int len_class = -1;
        for (std::size_t m = 1; m <= 7; m++) {
            if (rc.adaptiveBit(kMatchFlagBase + state + 8 * m) == 0) break;
            len_class++;
        }

        std::uint64_t len_value = 1;
        if (len_class >= 0) {
            const std::size_t q = static_cast<std::size_t>((pos << len_class) & 3);
            const std::size_t base = kLengthBase + 32 * static_cast<std::size_t>(len_class) + 8 * q + state;
            len_value = rc.classBits(len_class, base, 8, 0x18);
        }
        if (len_value == kEndMarker) return static_cast<std::int64_t>(pos);
        const std::uint64_t length = len_value + 1;

        // Distance class: bounded bit tree, the short one for match lengths 2 and 3.
        const bool short_tree = len_value <= 2;
        const std::uint64_t limit = short_tree ? kShortTreeLimit : kLongTreeLimit;
        const std::size_t tree_base = short_tree ? kShortDistTreeBase + static_cast<std::size_t>(len_class)
                                                 : kLongDistTreeBase + static_cast<std::size_t>(len_class);
        std::uint64_t t = 1;
        std::uint64_t u;
        do {
            u = t;
            t = u * 2 + rc.adaptiveBit(tree_base + 8 * u);
        } while (u < limit);
        const std::int64_t dist_class = static_cast<std::int64_t>(t) - static_cast<std::int64_t>(2 * limit) - 1;

        std::uint64_t distance = 1;
        if (dist_class >= 0) {
            distance = rc.classBits(static_cast<int>(dist_class),
                                    kDistExtraBase + 8 * static_cast<std::size_t>(dist_class), 1, 4);
        }

        if (distance > pos || pos + length > capacity) return -1;
        // Byte-wise on purpose: with distance < length the copy reads bytes it has just written.
        for (std::uint64_t i = 0; i < length; i++) out[pos + i] = out[pos - distance + i];
        pos += length;
        last = out[pos - 1];
        state = (pos & 1) ? 6 : 7;
    }
}

}  // namespace dal::sdat

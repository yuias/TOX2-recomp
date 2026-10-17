#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tox2 {

// Streaming form of rsxHashBytes: the digest after any sequence of update() calls equals rsxHashBytes of the
// concatenated bytes, however they are split. It lets a memory view hash a range that lies in several pieces
// without gathering it first. The state lives inline so that no allocation is needed per range.
class RsxHashStream {
public:
    RsxHashStream();
    RsxHashStream(const RsxHashStream&) = delete;
    RsxHashStream& operator=(const RsxHashStream&) = delete;
    void update(std::span<const std::uint8_t> bytes);
    std::uint64_t digest() const;

private:
    // Sized for XXH3_state_t (checked in rsx_hash_stream.cpp), which keeps xxhash.h out of this header.
    static constexpr std::size_t kStateBytes = 576;
    alignas(64) unsigned char state_[kStateBytes];
};

}  // namespace tox2

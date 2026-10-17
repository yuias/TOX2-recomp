#include "rsx/rsx_hash_stream.h"

#include <new>

#include <xxhash.h>

namespace tox2 {

static_assert(sizeof(XXH3_state_t) <= sizeof(RsxHashStream), "RsxHashStream::kStateBytes is too small");
static_assert(alignof(XXH3_state_t) <= alignof(RsxHashStream), "RsxHashStream is under-aligned for XXH3_state_t");

RsxHashStream::RsxHashStream() {
    auto* state = new (state_) XXH3_state_t;
    XXH3_64bits_reset(state);
}

void RsxHashStream::update(std::span<const std::uint8_t> bytes) {
    XXH3_64bits_update(std::launder(reinterpret_cast<XXH3_state_t*>(state_)), bytes.data(), bytes.size());
}

std::uint64_t RsxHashStream::digest() const {
    return XXH3_64bits_digest(std::launder(reinterpret_cast<const XXH3_state_t*>(state_)));
}

}  // namespace tox2

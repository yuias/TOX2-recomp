#include "rsx/fifo_parser.h"

#include <format>
#include <utility>

namespace dal {

void FifoParser::reset() { *this = FifoParser{}; }

FifoStatus FifoParser::fail(std::string message) {
    fault_ = std::move(message);
    faulted_ = true;
    return FifoStatus::Fault;
}

FifoStatus FifoParser::run(std::uint32_t& get, std::uint32_t put, std::uint32_t max_words, RsxSink& sink) {
    if (faulted_) return FifoStatus::Fault;
    put &= ~3u;
    std::uint32_t words = 0;

    for (;;) {
        if (pending_) {
            if (!sink.write(pending_method_, pending_value_)) return FifoStatus::Waiting;
            pending_ = false;
            if (sink.stopAfterWrite()) return FifoStatus::Budget;
        }
        if (get == put) return FifoStatus::Idle;
        if (words >= max_words) return FifoStatus::Budget;
        if ((get & 3) != 0) return fail(std::format("unaligned get 0x{:x}", get));

        std::uint32_t word = 0;
        if (!sink.readIo(get, word)) return fail(std::format("unmapped IO offset 0x{:x}", get));
        ++words;

        if (remaining_ != 0) {
            pending_ = true;
            pending_method_ = method_;
            pending_value_ = word;
            // Register offsets are 16 bits wide; an incrementing method wraps instead of leaving them.
            if (increment_) method_ = (method_ + 4) & 0xFFFF;
            --remaining_;
            get += 4;
            continue;
        }

        if ((word & kFifoNonMethodMask) != 0) {
            // Same order as RPCS3 `thread::run_FIFO`; the call test only looks at the low bits.
            if ((word & kFifoOldJumpMask) == kFifoOldJump || (word & kFifoNewJumpMask) == kFifoNewJump) {
                const std::uint32_t mask =
                        (word & kFifoOldJumpMask) == kFifoOldJump ? kFifoOldJumpOffsetMask : kFifoNewJumpOffsetMask;
                const std::uint32_t target = word & mask;
                // A jump to itself is how the driver parks the puller; report it as idle and look again
                // on the next run, since the guest may rewrite the word.
                if (target == get) return FifoStatus::Idle;
                get = target;
            } else if ((word & kFifoCallMask) == kFifoCall) {
                if (in_call_)
                    return fail(std::format("call at 0x{:x} inside a subroutine (header 0x{:08x})", get, word));
                in_call_ = true;
                return_get_ = get + 4;
                get = word & kFifoCallOffsetMask;
            } else if ((word & kFifoReturnMask) == kFifoReturn) {
                if (!in_call_) return fail(std::format("return at 0x{:x} without a call (header 0x{:08x})", get, word));
                in_call_ = false;
                get = return_get_;
            } else {
                return fail(std::format("invalid header 0x{:08x} at 0x{:x}", word, get));
            }
            continue;
        }

        const std::uint32_t header_get = get;
        get += 4;
        const std::uint32_t count = (word >> kFifoCountShift) & kFifoCountMask;
        if (count == 0) continue;  // NOP
        ++headers_;
        command_get_ = header_get;
        remaining_ = count;
        method_ = word & kFifoMethodMask;
        increment_ = (word & kFifoNonIncrementMask) != kFifoNonIncrement;
    }
}

}  // namespace dal

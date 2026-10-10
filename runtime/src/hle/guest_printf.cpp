// Guest printf. Integer, character and floating conversions are handed to the host snprintf with a
// format rebuilt from the parsed fields, so flag and rounding behaviour is the C library's. Strings and
// pointers are formatted here because they read guest memory (or print 0x + the 32-bit address).

#include "hle/guest_printf.h"

#include <bit>
#include <cstdio>
#include <string>

#include "core/guest_memory.h"
#include "hle/hle.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kMaxFormatBytes = 0x10000;
constexpr std::uint32_t kMaxStringBytes = 0x100000;
// A corrupted format would otherwise ask the host for gigabytes of padding.
constexpr int kMaxFieldSize = 0x10000;
constexpr std::uint32_t kParameterSaveArea = 0x30;
constexpr std::uint32_t kRegisterSlots = 8;

enum class Length { Default, Char, Short, Long, LongDouble };

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = -1;      // -1: none
    int precision = -1;  // -1: none
    Length length = Length::Default;
    char conv = 0;
};

// snprintf into a string without a fixed buffer; an embedded NUL (from %c) is kept.
template <class... Args>
void appendHost(std::string& out, const char* format, Args... args) {
    const int size = std::snprintf(nullptr, 0, format, args...);
    if (size <= 0) return;
    std::string text(static_cast<std::size_t>(size) + 1, '\0');
    std::snprintf(text.data(), text.size(), format, args...);
    text.resize(static_cast<std::size_t>(size));
    out += text;
}

void appendPadded(std::string& out, const std::string& text, const Spec& spec) {
    const std::size_t width = spec.width > 0 ? static_cast<std::size_t>(spec.width) : 0;
    const std::size_t pad = width > text.size() ? width - text.size() : 0;
    if (!spec.left) out.append(pad, ' ');
    out += text;
    if (spec.left) out.append(pad, ' ');
}

// "%" + flags + width + precision, for the host snprintf.
std::string hostPrefix(const Spec& spec) {
    std::string format = "%";
    if (spec.left) format += '-';
    if (spec.plus) format += '+';
    if (spec.space) format += ' ';
    if (spec.alt) format += '#';
    if (spec.zero) format += '0';
    if (spec.width >= 0) format += std::to_string(spec.width);
    if (spec.precision >= 0) format += '.' + std::to_string(spec.precision);
    return format;
}

std::int64_t signedArgument(std::uint64_t slot, Length length) {
    switch (length) {
        case Length::Char: return static_cast<std::int8_t>(slot);
        case Length::Short: return static_cast<std::int16_t>(slot);
        case Length::Default: return static_cast<std::int32_t>(slot);
        default: return static_cast<std::int64_t>(slot);
    }
}

std::uint64_t unsignedArgument(std::uint64_t slot, Length length) {
    switch (length) {
        case Length::Char: return static_cast<std::uint8_t>(slot);
        case Length::Short: return static_cast<std::uint16_t>(slot);
        case Length::Default: return static_cast<std::uint32_t>(slot);
        default: return slot;
    }
}

}  // namespace

GuestVarArgs GuestVarArgs::fromRegisters(const Tox2PpuContext* ctx, std::uint32_t first_slot) {
    GuestVarArgs args;
    args.ctx = ctx;
    args.slot = first_slot;
    return args;
}

GuestVarArgs GuestVarArgs::fromVaList(std::uint32_t va) {
    GuestVarArgs args;
    args.address = va;
    return args;
}

std::uint64_t GuestVarArgs::next(const std::uint8_t* base) {
    if (ctx == nullptr) {
        const std::uint64_t value = tox2Read64(base, address);
        address += 8;
        return value;
    }
    const std::uint32_t k = slot++;
    if (k < kRegisterSlots) return ctx->gpr[3 + k];
    return tox2Read64(base, static_cast<std::uint32_t>(ctx->gpr[1]) + kParameterSaveArea + 8 * k);
}

std::string formatGuestPrintf(const std::uint8_t* base, std::uint32_t fmt, GuestVarArgs& args, std::string& error) {
    error.clear();
    const std::string format = readGuestString(base, fmt, kMaxFormatBytes);
    std::string out;
    std::size_t i = 0;
    while (i < format.size()) {
        if (format[i] != '%') {
            out += format[i++];
            continue;
        }
        const std::size_t start = i++;
        Spec spec;
        auto fail = [&](const std::string& message) {
            error = message;
            return out;
        };

        for (; i < format.size(); i++) {
            const char c = format[i];
            if (c == '-') {
                spec.left = true;
            } else if (c == '+') {
                spec.plus = true;
            } else if (c == ' ') {
                spec.space = true;
            } else if (c == '#') {
                spec.alt = true;
            } else if (c == '0') {
                spec.zero = true;
            } else {
                break;
            }
        }

        // The number of digits is bounded below by the size check, so this cannot overflow.
        auto number = [&](int& value) {
            value = 0;
            while (i < format.size() && format[i] >= '0' && format[i] <= '9' && value <= kMaxFieldSize) {
                value = value * 10 + (format[i++] - '0');
            }
        };
        if (i < format.size() && format[i] == '*') {
            i++;
            const std::int64_t width = static_cast<std::int32_t>(args.next(base));
            spec.left = spec.left || width < 0;
            spec.width = static_cast<int>(width < 0 ? -width : width);
        } else if (i < format.size() && format[i] >= '0' && format[i] <= '9') {
            number(spec.width);
        }
        if (i < format.size() && format[i] == '.') {
            i++;
            if (i < format.size() && format[i] == '*') {
                i++;
                const std::int64_t precision = static_cast<std::int32_t>(args.next(base));
                spec.precision = precision < 0 ? -1 : static_cast<int>(precision);
            } else {
                number(spec.precision);
            }
        }
        if (spec.width > kMaxFieldSize || spec.precision > kMaxFieldSize) {
            return fail("width or precision is too large");
        }

        if (i < format.size()) {
            switch (format[i]) {
                case 'h':
                    spec.length = Length::Short;
                    i++;
                    if (i < format.size() && format[i] == 'h') {
                        spec.length = Length::Char;
                        i++;
                    }
                    break;
                case 'l':
                    spec.length = Length::Long;
                    i++;
                    if (i < format.size() && format[i] == 'l') i++;
                    break;
                case 'z':
                case 'j':
                case 't':
                case 'q':
                    spec.length = Length::Long;
                    i++;
                    break;
                case 'L':
                    spec.length = Length::LongDouble;
                    i++;
                    break;
                default: break;
            }
        }
        if (i >= format.size()) return fail("incomplete conversion '" + format.substr(start) + "'");

        spec.conv = format[i++];
        switch (spec.conv) {
            case '%': out += '%'; break;
            case 'd':
            case 'i': {
                const std::string host = hostPrefix(spec) + "lld";
                appendHost(out, host.c_str(), static_cast<long long>(signedArgument(args.next(base), spec.length)));
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                const std::string host = hostPrefix(spec) + "ll" + spec.conv;
                appendHost(out, host.c_str(),
                           static_cast<unsigned long long>(unsignedArgument(args.next(base), spec.length)));
                break;
            }
            case 'c': {
                const std::string host = hostPrefix(spec) + 'c';
                appendHost(out, host.c_str(), static_cast<int>(args.next(base) & 0xff));
                break;
            }
            case 's': {
                const auto address = static_cast<std::uint32_t>(args.next(base));
                std::string text;
                if (address == 0) {
                    text = "(null)";
                } else {
                    text = readGuestString(
                            base, address,
                            spec.precision >= 0 ? static_cast<std::uint32_t>(spec.precision) : kMaxStringBytes);
                }
                appendPadded(out, text, spec);
                break;
            }
            case 'p': {
                char digits[16];
                std::snprintf(digits, sizeof digits, "%x", static_cast<std::uint32_t>(args.next(base)));
                appendPadded(out, std::string("0x") + digits, spec);
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                const std::string host = hostPrefix(spec) + spec.conv;
                appendHost(out, host.c_str(), std::bit_cast<double>(args.next(base)));
                break;
            }
            case 'n': return fail("%n is not supported");
            default: return fail("unsupported conversion '" + format.substr(start, i - start) + "'");
        }
    }
    return out;
}

}  // namespace tox2

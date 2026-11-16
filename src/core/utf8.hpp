// src/core/utf8.hpp (internal to navigator_core)
//
// Purpose: strict UTF-8 decoding with "surrogateescape" for invalid bytes, shared by
//          fold_name(), compare_names() and to_display_name() (navigator/fs.hpp).
// Thread-safety: pure functions, no state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace navigator::core {

// Invalid byte b decodes to kEscapeBase + b (U+DC80..U+DCFF; bytes < 0x80 are always valid).
inline constexpr char32_t kEscapeBase = 0xDC00;

struct Decoded {
    char32_t cp = 0;
    std::uint8_t length = 1; // bytes consumed (1..4)
    bool escaped = false;    // cp == kEscapeBase + the single consumed byte
};

namespace detail {
[[nodiscard]] constexpr bool in_range(unsigned char b, unsigned char lo,
                                      unsigned char hi) noexcept {
    return b >= lo && b <= hi;
}
} // namespace detail

// Decodes the code point at s[i] (requires i < s.size()). Accepts only shortest-form sequences
// of scalar values (no surrogates, nothing above U+10FFFF). On any failure exactly one byte is
// consumed and escaped, so every invalid byte maps to its own escape.
[[nodiscard]] constexpr Decoded decode_utf8(std::string_view s, std::size_t i) noexcept {
    const auto at = [&](std::size_t k) noexcept { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = at(i);
    if (b0 < 0x80) {
        return {b0, 1, false};
    }
    const Decoded bad{kEscapeBase + b0, 1, true};
    const std::size_t left = s.size() - i;
    if (detail::in_range(b0, 0xC2, 0xDF)) {
        if (left < 2 || !detail::in_range(at(i + 1), 0x80, 0xBF)) {
            return bad;
        }
        return {(char32_t{b0 & 0x1FU} << 6U) | (at(i + 1) & 0x3FU), 2, false};
    }
    if (detail::in_range(b0, 0xE0, 0xEF)) {
        if (left < 3) {
            return bad;
        }
        const unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;
        const unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;
        if (!detail::in_range(at(i + 1), lo, hi) || !detail::in_range(at(i + 2), 0x80, 0xBF)) {
            return bad;
        }
        return {(char32_t{b0 & 0x0FU} << 12U) | (char32_t{at(i + 1) & 0x3FU} << 6U) |
                    (at(i + 2) & 0x3FU),
                3, false};
    }
    if (detail::in_range(b0, 0xF0, 0xF4)) {
        if (left < 4) {
            return bad;
        }
        const unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;
        const unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;
        if (!detail::in_range(at(i + 1), lo, hi) || !detail::in_range(at(i + 2), 0x80, 0xBF) ||
            !detail::in_range(at(i + 3), 0x80, 0xBF)) {
            return bad;
        }
        return {(char32_t{b0 & 0x07U} << 18U) | (char32_t{at(i + 1) & 0x3FU} << 12U) |
                    (char32_t{at(i + 2) & 0x3FU} << 6U) | (at(i + 3) & 0x3FU),
                4, false};
    }
    return bad;
}

// Appends the UTF-8 encoding of a Unicode scalar value (not a surrogate, <= U+10FFFF).
inline void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

} // namespace navigator::core

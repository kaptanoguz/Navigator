// src/ui/search/highlight_map.cpp — see highlight_map.hpp.
#include "search/highlight_map.hpp"

#include <algorithm>

namespace navigator::ui {
namespace {

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

// Length of a valid UTF-8 sequence at `s[i]`, or 0 when the byte starts no valid sequence.
std::size_t valid_seq(std::string_view s, std::size_t i) noexcept {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (b < 0x80U) {
        return 1;
    }
    if ((b & 0xE0U) == 0xC0U) {
        n = 2;
        cp = b & 0x1FU;
    } else if ((b & 0xF0U) == 0xE0U) {
        n = 3;
        cp = b & 0x0FU;
    } else if ((b & 0xF8U) == 0xF0U) {
        n = 4;
        cp = b & 0x07U;
    } else {
        return 0;
    }
    if (i + n > s.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < n; ++k) {
        const auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0U) != 0x80U) {
            return 0;
        }
        cp = (cp << 6U) | (c & 0x3FU);
    }
    const bool overlong =
        (n == 2 && cp < 0x80U) || (n == 3 && cp < 0x800U) || (n == 4 && cp < 0x10000U);
    if (overlong || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return 0;
    }
    return n;
}

void push_merged(ByteRanges& out, std::uint32_t b, std::uint32_t e) {
    if (b >= e) {
        return;
    }
    if (!out.empty() && b <= out.back().second) {
        out.back().second = std::max(out.back().second, e);
        return;
    }
    out.emplace_back(b, e);
}

} // namespace

ByteRanges to_display_ranges(std::string_view raw, std::string_view display,
                             const std::vector<search::HighlightRange>& ranges) {
    ByteRanges out;
    if (ranges.empty()) {
        return out;
    }
    if (raw == display) {
        for (const search::HighlightRange& r : ranges) {
            push_merged(out,
                        std::min<std::uint32_t>(r.begin, static_cast<std::uint32_t>(raw.size())),
                        std::min<std::uint32_t>(r.end, static_cast<std::uint32_t>(raw.size())));
        }
        return out;
    }
    // offsets[i]: display byte where raw byte i lands (raw.size() + 1 entries).
    std::vector<std::uint32_t> offsets(raw.size() + 1, 0);
    std::size_t d = 0;
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::size_t n = valid_seq(raw, i);
        if (n == 0) {
            offsets[i] = static_cast<std::uint32_t>(d);
            d += 3; // U+FFFD
            ++i;
            continue;
        }
        for (std::size_t k = 0; k < n; ++k) {
            offsets[i + k] = static_cast<std::uint32_t>(d + k);
        }
        d += n;
        i += n;
    }
    offsets[raw.size()] = static_cast<std::uint32_t>(d);
    if (d != display.size()) {
        return {}; // not the mapping fs::to_display_name made
    }
    for (const search::HighlightRange& r : ranges) {
        const std::size_t b = std::min<std::size_t>(r.begin, raw.size());
        const std::size_t e = std::min<std::size_t>(r.end, raw.size());
        push_merged(out, offsets[b], offsets[e]);
    }
    return out;
}

ByteRanges to_shown_ranges(std::string_view display, std::string_view shown,
                           const ByteRanges& ranges) {
    if (shown.empty() || shown == display) {
        return ranges;
    }
    const std::size_t dot = shown.find(kEllipsis);
    if (dot == std::string_view::npos) {
        return {};
    }
    const std::size_t head = dot; // shown[0, head) == display head
    const std::size_t tail =
        shown.size() - dot - kEllipsis.size(); // shown suffix == display suffix
    // TextFitter cuts at cluster boundaries and trims spaces at the cut: the head is still a
    // prefix of the display text and the tail a suffix.
    if (head + tail > display.size() || display.substr(0, head) != shown.substr(0, head) ||
        display.substr(display.size() - tail) != shown.substr(dot + kEllipsis.size())) {
        return {};
    }
    const std::size_t tail_start = display.size() - tail; // in display
    const auto ell_b = static_cast<std::uint32_t>(dot);
    const auto ell_e = static_cast<std::uint32_t>(dot + kEllipsis.size());
    const auto shift = static_cast<std::int64_t>(ell_e) - static_cast<std::int64_t>(tail_start);
    ByteRanges out;
    for (const auto& [b, e] : ranges) {
        if (b < head) {
            push_merged(out, b, static_cast<std::uint32_t>(std::min<std::size_t>(e, head)));
        }
        if (e > head && b < tail_start) {
            push_merged(out, ell_b, ell_e); // part of the match is in the elided middle
        }
        if (e > tail_start) {
            const std::size_t tb = std::max<std::size_t>(b, tail_start);
            push_merged(out, static_cast<std::uint32_t>(static_cast<std::int64_t>(tb) + shift),
                        static_cast<std::uint32_t>(static_cast<std::int64_t>(e) + shift));
        }
    }
    return out;
}

} // namespace navigator::ui

// src/preview/text.cpp: looks_binary() and decode_text(): BOM detection, UTF-16 conversion,
// UTF-8 repair (U+FFFD per maximal invalid subpart) and splitting lines into bounded rows.
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <navigator/preview.hpp>

#include "internal.hpp"

namespace navigator::preview {

namespace detail {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
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

} // namespace detail

namespace {

constexpr std::uint32_t kReplacement = 0xFFFD;

// Length of the valid UTF-8 sequence at p (1..4), 0 when invalid. `max_sub` receives the length
// of the maximal subpart (>= 1) for an invalid sequence; `incomplete` is set when the bytes are a
// valid prefix that ran out of input.
std::size_t utf8_seq(const std::uint8_t* p, std::size_t avail, std::size_t& max_sub,
                     bool& incomplete) noexcept {
    incomplete = false;
    max_sub = 1;
    const std::uint8_t b0 = p[0];
    if (b0 < 0x80U) {
        return 1;
    }
    std::size_t len = 0;
    std::uint8_t lo = 0x80;
    std::uint8_t hi = 0xBF;
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        len = 2;
    } else if (b0 == 0xE0U) {
        len = 3;
        lo = 0xA0;
    } else if ((b0 >= 0xE1U && b0 <= 0xECU) || b0 == 0xEEU || b0 == 0xEFU) {
        len = 3;
    } else if (b0 == 0xEDU) {
        len = 3;
        hi = 0x9F;
    } else if (b0 == 0xF0U) {
        len = 4;
        lo = 0x90;
    } else if (b0 >= 0xF1U && b0 <= 0xF3U) {
        len = 4;
    } else if (b0 == 0xF4U) {
        len = 4;
        hi = 0x8F;
    } else {
        return 0;
    }
    for (std::size_t i = 1; i < len; ++i) {
        if (i >= avail) {
            incomplete = true;
            max_sub = i;
            return 0;
        }
        const std::uint8_t b = p[i];
        const std::uint8_t l = i == 1 ? lo : std::uint8_t{0x80};
        const std::uint8_t h = i == 1 ? hi : std::uint8_t{0xBF};
        if (b < l || b > h) {
            max_sub = i;
            return 0;
        }
    }
    return len;
}

void decode_utf8(const std::uint8_t* p, std::size_t n, bool cut, TextPreview& out) {
    std::size_t i = 0;
    out.text.reserve(n);
    while (i < n) {
        // Fast ASCII run.
        std::size_t j = i;
        while (j < n && p[j] < 0x80U) {
            ++j;
        }
        if (j > i) {
            out.text.append(reinterpret_cast<const char*>(p + i), j - i);
            i = j;
            continue;
        }
        std::size_t sub = 1;
        bool incomplete = false;
        const std::size_t len = utf8_seq(p + i, n - i, sub, incomplete);
        if (len > 0) {
            out.text.append(reinterpret_cast<const char*>(p + i), len);
            i += len;
            continue;
        }
        if (incomplete && cut) { // cut by the size cap: drop
            break;
        }
        detail::append_utf8(out.text, kReplacement);
        ++out.replaced_sequences;
        i += sub;
    }
    out.bytes_read = i;
}

void decode_utf16(const std::uint8_t* p, std::size_t n, bool big_endian, bool cut,
                  TextPreview& out) {
    out.text.reserve(n);
    std::size_t i = 0;
    const auto unit = [&](std::size_t at) -> std::uint32_t {
        return big_endian ? (std::uint32_t{p[at]} << 8U) | p[at + 1]
                          : (std::uint32_t{p[at + 1]} << 8U) | p[at];
    };
    while (i + 1 < n) {
        const std::uint32_t u = unit(i);
        if (u >= 0xD800U && u <= 0xDBFFU) {
            if (i + 3 < n) {
                const std::uint32_t u2 = unit(i + 2);
                if (u2 >= 0xDC00U && u2 <= 0xDFFFU) {
                    detail::append_utf8(out.text,
                                        0x10000U + ((u - 0xD800U) << 10U) + (u2 - 0xDC00U));
                    i += 4;
                    continue;
                }
            } else if (cut) {
                break; // high surrogate cut by the cap
            }
            detail::append_utf8(out.text, kReplacement);
            ++out.replaced_sequences;
            i += 2;
            continue;
        }
        if (u >= 0xDC00U && u <= 0xDFFFU) {
            detail::append_utf8(out.text, kReplacement);
            ++out.replaced_sequences;
            i += 2;
            continue;
        }
        detail::append_utf8(out.text, u);
        i += 2;
    }
    if (i < n && !cut && i + 1 == n) { // odd final byte at the real end of the file
        detail::append_utf8(out.text, kReplacement);
        ++out.replaced_sequences;
        i = n;
    }
    out.bytes_read = i;
}

// Splits text into rows; returns false when max_rows was reached (text truncated after the
// last row).
bool build_rows(TextPreview& out, const TextOptions& options) {
    const std::string& t = out.text;
    const std::size_t n = t.size();
    std::size_t pos = 0;
    std::uint32_t line = 0;
    while (pos < n) {
        std::size_t eol = pos;
        while (eol < n && t[eol] != '\n' && t[eol] != '\r') {
            ++eol;
        }
        std::size_t next = eol;
        if (eol < n) {
            next = (t[eol] == '\r' && eol + 1 < n && t[eol + 1] == '\n') ? eol + 2 : eol + 1;
        }
        const std::size_t line_len = eol - pos;
        out.longest_line_bytes = std::max<std::uint64_t>(out.longest_line_bytes, line_len);
        std::size_t start = pos;
        bool first = true;
        while (first || start < eol) {
            first = false;
            std::size_t len = std::min<std::size_t>(eol - start, options.max_row_bytes);
            if (start + len < eol) {
                // Never split inside a UTF-8 sequence: back off continuation bytes.
                std::size_t k = len;
                while (k > 0 && (static_cast<std::uint8_t>(t[start + k]) & 0xC0U) == 0x80U) {
                    --k;
                }
                if (k > 0) {
                    len = k;
                }
            }
            if (out.rows.size() >= options.max_rows) {
                out.text.resize(start);
                out.line_count = out.rows.empty() ? 0 : out.rows.back().line + 1;
                return false;
            }
            out.rows.push_back(
                TextRow{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(len), line});
            start += len;
        }
        ++line;
        pos = next;
    }
    out.line_count = line;
    return true;
}

} // namespace

bool looks_binary(Span<const std::uint8_t> head) noexcept {
    if (head.size() >= 2 &&
        ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF))) {
        return false;
    }
    const std::size_t n = std::min(head.size(), kBinarySniffBytes);
    return std::memchr(head.data(), 0, n) != nullptr;
}

Result<TextPreview> decode_text(Span<const std::uint8_t> bytes, std::uint64_t file_size,
                                const TextOptions& options) {
    if (options.max_row_bytes < 64 || options.max_rows == 0 || options.max_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "decode_text: invalid options");
    }
    if (looks_binary(bytes)) {
        return fail(ErrorCode::Unsupported, "decode_text: binary content");
    }
    const std::size_t n =
        static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), options.max_bytes));
    if (n > std::numeric_limits<std::uint32_t>::max() / 2U) {
        return fail(ErrorCode::InvalidArgument, "decode_text: more than 2 GiB of text");
    }
    const bool cut = std::uint64_t{n} < file_size;
    TextPreview out;
    out.file_size = file_size;
    try {
        const std::uint8_t* p = bytes.data();
        std::size_t skip = 0;
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            out.encoding = TextEncoding::Utf8Bom;
            skip = 3;
        } else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
            out.encoding = TextEncoding::Utf16Le;
            skip = 2;
        } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
            out.encoding = TextEncoding::Utf16Be;
            skip = 2;
        }
        if (out.encoding == TextEncoding::Utf16Le || out.encoding == TextEncoding::Utf16Be) {
            decode_utf16(p + skip, n - skip, out.encoding == TextEncoding::Utf16Be, cut, out);
        } else {
            decode_utf8(p + skip, n - skip, cut, out);
        }
        out.bytes_read += skip;
        if (out.text.size() > std::numeric_limits<std::uint32_t>::max()) {
            return fail(ErrorCode::InvalidArgument, "decode_text: more than 4 GiB of text");
        }
        const bool all_rows = build_rows(out, options);
        out.truncated = cut || out.bytes_read < file_size || !all_rows;
        out.text.shrink_to_fit();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "decode_text: allocation failed");
    }
    return out;
}

} // namespace navigator::preview

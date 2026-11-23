// fold_name(), fold_name_into(), compare_names(), to_display_name() (navigator/fs.hpp, "Names").
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>

#include "case_fold.hpp"
#include "utf8.hpp"

namespace navigator::fs {
namespace {

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// End of the maximal ASCII digit run starting at `i`.
[[nodiscard]] std::size_t digit_run_end(std::string_view s, std::size_t i) noexcept {
    while (i < s.size() && is_ascii_digit(s[i])) {
        ++i;
    }
    return i;
}

// Compares the digit runs lhs[li, le) and rhs[ri, re) as unbounded non-negative integers.
[[nodiscard]] std::weak_ordering compare_digit_runs(std::string_view lhs, std::size_t li,
                                                    std::size_t le, std::string_view rhs,
                                                    std::size_t ri, std::size_t re) noexcept {
    while (li < le && lhs[li] == '0') {
        ++li;
    }
    while (ri < re && rhs[ri] == '0') {
        ++ri;
    }
    if (le - li != re - ri) {
        return le - li < re - ri ? std::weak_ordering::less : std::weak_ordering::greater;
    }
    for (; li < le; ++li, ++ri) {
        if (lhs[li] != rhs[ri]) {
            return lhs[li] < rhs[ri] ? std::weak_ordering::less : std::weak_ordering::greater;
        }
    }
    return std::weak_ordering::equivalent;
}

// Code point at s[i] (ASCII fast path), and its byte length.
[[nodiscard]] core::Decoded next_code_point(std::string_view s, std::size_t i) noexcept {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        return {b, 1, false};
    }
    return core::decode_utf8(s, i);
}

} // namespace

void fold_name_into(std::string_view name, std::string& out,
                    std::vector<std::uint32_t>* source_offsets) {
    out.clear();
    if (source_offsets != nullptr) {
        source_offsets->clear();
    }
    std::size_t i = 0;
    while (i < name.size()) {
        const core::Decoded d = next_code_point(name, i);
        if (d.escaped) {
            out.push_back(name[i]); // invalid byte: emitted unchanged
        } else {
            core::append_utf8(out, core::fold_code_point(d.cp));
        }
        if (source_offsets != nullptr) {
            // One entry per result byte: every byte of this folded code point came from the
            // input code point starting at i.
            source_offsets->resize(out.size(), static_cast<std::uint32_t>(i));
        }
        i += d.length;
    }
    if (source_offsets != nullptr) {
        source_offsets->push_back(static_cast<std::uint32_t>(name.size()));
    }
}

std::string fold_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    fold_name_into(name, out);
    return out;
}

std::weak_ordering compare_names(std::string_view lhs, std::string_view rhs,
                                 const SortOptions& options) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        if (options.natural && is_ascii_digit(lhs[i]) && is_ascii_digit(rhs[j])) {
            const std::size_t ie = digit_run_end(lhs, i);
            const std::size_t je = digit_run_end(rhs, j);
            const std::weak_ordering r = compare_digit_runs(lhs, i, ie, rhs, j, je);
            if (r != 0) {
                return r;
            }
            i = ie;
            j = je;
            continue;
        }
        const core::Decoded a = next_code_point(lhs, i);
        const core::Decoded b = next_code_point(rhs, j);
        char32_t ca = a.cp;
        char32_t cb = b.cp;
        if (options.case_insensitive) {
            ca = core::fold_code_point(ca);
            cb = core::fold_code_point(cb);
        }
        if (ca != cb) {
            return ca < cb ? std::weak_ordering::less : std::weak_ordering::greater;
        }
        i += a.length;
        j += b.length;
    }
    if (i < lhs.size()) {
        return std::weak_ordering::greater;
    }
    if (j < rhs.size()) {
        return std::weak_ordering::less;
    }
    return std::weak_ordering::equivalent;
}

std::string to_display_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    std::size_t i = 0;
    while (i < name.size()) {
        const core::Decoded d = next_code_point(name, i);
        if (d.escaped) {
            out.append("\xEF\xBF\xBD"); // U+FFFD
        } else {
            out.append(name.substr(i, d.length));
        }
        i += d.length;
    }
    return out;
}

} // namespace navigator::fs

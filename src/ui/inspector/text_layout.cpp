// src/ui/inspector/text_layout.cpp — Inspector text wrapping and virtualization. See
// text_layout.hpp.
#include "inspector/text_layout.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui::inspector {

namespace {

// Length of the UTF-8 sequence starting with `b` (1 for continuation / invalid bytes; the
// preview already repaired the text, so this only guards against misuse).
std::uint32_t seq_len(unsigned char b) noexcept {
    if (b < 0x80U) {
        return 1;
    }
    if ((b & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((b & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((b & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

std::uint32_t advance_cells(unsigned char b, std::uint32_t cell) noexcept {
    if (b == '\t') {
        return kTabCells - (cell % kTabCells);
    }
    if (b == '\n' || b == '\r') {
        return 0;
    }
    return 1;
}

// Walks `s` and calls emit(byte_begin, byte_end, cell_begin) for every visual line. Lines break
// after the last blank that fits (word wrap); a word longer than the line breaks at the column.
template <typename Emit> void wrap_row(std::string_view s, std::uint32_t columns, Emit&& emit) {
    std::uint32_t begin = 0;
    std::uint32_t line_cells = 0;
    std::uint32_t row_cell = 0;
    std::uint32_t begin_cell = 0;
    std::uint32_t brk = 0;      // byte after the last blank of the current line (0: none)
    std::uint32_t brk_cell = 0; // row cell at brk
    std::uint32_t i = 0;
    const auto n = static_cast<std::uint32_t>(s.size());
    while (i < n) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::uint32_t len = std::min(seq_len(b), n - i);
        const std::uint32_t adv = advance_cells(b, row_cell);
        const bool blank = b == ' ' || b == '\t';
        // Blanks may hang past the edge (invisible); the next word then moves down.
        if (line_cells + adv > columns && line_cells > 0 && !blank) {
            if (brk > begin) {
                emit(begin, brk, begin_cell);
                begin = brk;
                begin_cell = brk_cell;
            } else {
                emit(begin, i, begin_cell);
                begin = i;
                begin_cell = row_cell;
            }
            line_cells = row_cell - begin_cell;
            brk = 0;
        }
        line_cells += adv;
        row_cell += adv;
        i += len;
        if (blank) {
            brk = i;
            brk_cell = row_cell;
        }
    }
    emit(begin, n, begin_cell); // an empty row is one (empty) visual line
}

} // namespace

std::uint32_t count_cells(std::string_view utf8, std::uint32_t start_cell) noexcept {
    std::uint32_t cell = start_cell;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b = static_cast<unsigned char>(utf8[i]);
        cell += advance_cells(b, cell);
        i += seq_len(b);
    }
    return cell - start_cell;
}

std::string display_text(std::string_view segment, std::uint32_t start_cell) {
    std::string out;
    out.reserve(segment.size() + 8);
    std::uint32_t cell = start_cell;
    std::size_t i = 0;
    while (i < segment.size()) {
        const auto b = static_cast<unsigned char>(segment[i]);
        const std::size_t len = std::min<std::size_t>(seq_len(b), segment.size() - i);
        if (b == '\t') {
            const std::uint32_t adv = advance_cells(b, cell);
            out.append(adv, ' ');
            cell += adv;
        } else if (b == '\n' || b == '\r') {
            // dropped
        } else if (b < 0x20U || b == 0x7FU) {
            out.append("\xC2\xB7"); // U+00B7
            ++cell;
        } else {
            out.append(segment.substr(i, len));
            ++cell;
        }
        i += len;
    }
    return out;
}

void TextWrap::rebuild(const preview::TextPreview* text, std::uint32_t columns) {
    text_ = text;
    columns_ = std::max<std::uint32_t>(columns, 8U);
    prefix_.clear();
    if (text == nullptr) {
        return;
    }
    prefix_.reserve(text->rows.size() + 1);
    std::uint32_t total = 0;
    prefix_.push_back(0);
    for (std::size_t r = 0; r < text->rows.size(); ++r) {
        const std::string_view s = text->row_text(r);
        std::uint32_t lines = 0;
        // Fast path: ASCII without blanks, tabs or terminators wraps at fixed widths.
        bool plain = true;
        for (const char c : s) {
            const auto b = static_cast<unsigned char>(c);
            if (b >= 0x80U || b == '\t' || b == '\n' || b == '\r' || b == ' ') {
                plain = false;
                break;
            }
        }
        if (plain) {
            lines =
                s.empty() ? 1U : static_cast<std::uint32_t>((s.size() + columns_ - 1) / columns_);
        } else {
            wrap_row(s, columns_,
                     [&lines](std::uint32_t, std::uint32_t, std::uint32_t) { ++lines; });
        }
        total += lines;
        prefix_.push_back(total);
    }
}

VisualLine TextWrap::line(std::size_t k) const {
    VisualLine out;
    if (text_ == nullptr || k >= visual_lines()) {
        return out;
    }
    const auto it = std::upper_bound(prefix_.begin(), prefix_.end(), static_cast<std::uint32_t>(k));
    const auto row = static_cast<std::size_t>(std::distance(prefix_.begin(), it) - 1);
    const std::size_t j = k - prefix_[row];
    const std::string_view s = text_->row_text(row);
    std::size_t seen = 0;
    bool done = false;
    wrap_row(s, columns_, [&](std::uint32_t b, std::uint32_t e, std::uint32_t c) {
        if (!done && seen == j) {
            out.byte_begin = b;
            out.byte_end = e;
            out.cell_begin = c;
            done = true;
        }
        ++seen;
    });
    out.row = row;
    out.line = text_->rows[row].line;
    out.first_of_line = j == 0 && (row == 0 || text_->rows[row - 1].line != out.line);
    return out;
}

LineRange visible_lines(double scroll_px, double view_h, double line_h,
                        std::size_t count) noexcept {
    if (line_h <= 0.0 || count == 0 || view_h <= 0.0) {
        return {};
    }
    const double s = std::max(0.0, scroll_px);
    const auto first = std::min(count, static_cast<std::size_t>(std::floor(s / line_h)));
    const auto last = std::min(count, static_cast<std::size_t>(std::ceil((s + view_h) / line_h)));
    return LineRange{first, std::max(first, last)};
}

double max_scroll(double view_h, double line_h, std::size_t count) noexcept {
    return std::max(0.0, (static_cast<double>(count) * line_h) - view_h);
}

} // namespace navigator::ui::inspector

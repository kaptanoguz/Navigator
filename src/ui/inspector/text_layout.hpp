// src/ui/inspector/text_layout.hpp
//
// Phase 6 Inspector text view virtualization (preview.hpp "Text": long lines never cost frame
// time). preview::decode_text splits every logical line into display ROWS of at most 1 KiB; this
// model soft-wraps each row at a column count (monospace cells; after the last blank that fits,
// else at the column) into visual lines and answers
// "which row segment is visual line k" in O(log rows), so a frame shapes only the visible lines
// (at most `columns` cells each) however long a line is: a 4 MiB single-line file is 4096 rows,
// wrapped into about 4096 * ceil(1024 / columns) visual lines.
//
// Cells: one per code point; a tab advances to the next multiple of kTabCells (expanded to
// spaces for drawing); control characters other than tab show as one cell (U+FFFD-free: the
// shaper draws them as boxes). Wide (East Asian) glyphs are counted as one cell and may overflow
// the right edge, where the view clips them.
// Thread-safety: plain value; UI thread. rebuild() is O(text bytes) and runs only when the
// preview or the column count changes (bounded by TextOptions::max_bytes, 4 MiB by default).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/preview.hpp>

namespace navigator::ui::inspector {

inline constexpr std::uint32_t kTabCells = 4;

struct VisualLine {
    std::size_t row = 0;          // index into TextPreview::rows
    std::uint32_t byte_begin = 0; // offset within the row's text
    std::uint32_t byte_end = 0;   // exclusive
    std::uint32_t cell_begin = 0; // cell column of byte_begin within the row
    bool first_of_line = false;   // starts a logical line (gets a line number)
    std::uint32_t line = 0;       // logical line (0-based)
};

class TextWrap {
public:
    // Wraps `text` at `columns` cells (>= 8). Keeps a pointer to `text`: rebuild when it changes.
    void rebuild(const preview::TextPreview* text, std::uint32_t columns);
    [[nodiscard]] bool matches(const preview::TextPreview* text,
                               std::uint32_t columns) const noexcept {
        return text == text_ && columns == columns_;
    }

    [[nodiscard]] std::size_t visual_lines() const noexcept {
        return prefix_.empty() ? 0 : prefix_.back();
    }
    [[nodiscard]] std::uint32_t columns() const noexcept { return columns_; }
    // Visual line `k` (< visual_lines()).
    [[nodiscard]] VisualLine line(std::size_t k) const;
    // First visual line of display row `row`.
    [[nodiscard]] std::size_t first_visual_of_row(std::size_t row) const noexcept {
        return row < prefix_.size() ? prefix_[row] : visual_lines();
    }

private:
    const preview::TextPreview* text_ = nullptr;
    std::uint32_t columns_ = 0;
    std::vector<std::uint32_t> prefix_; // prefix_[r] = visual lines before row r; size rows+1
};

// Cells of `utf8` (tabs expanded from column `start_cell`).
[[nodiscard]] std::uint32_t count_cells(std::string_view utf8,
                                        std::uint32_t start_cell = 0) noexcept;
// Drawable text of a visual line: tabs expanded to spaces (stops relative to `start_cell`), line
// terminators dropped, other C0 controls and DEL shown as U+00B7 (middle dot).
[[nodiscard]] std::string display_text(std::string_view segment, std::uint32_t start_cell);

// Visible range of a virtualized list: lines [first, last) for a scroll offset (px) in a view of
// `view_h` px with `line_h` px lines (pure helper shared with the folder view).
struct LineRange {
    std::size_t first = 0;
    std::size_t last = 0; // exclusive
    friend constexpr bool operator==(const LineRange&, const LineRange&) = default;
};
[[nodiscard]] LineRange visible_lines(double scroll_px, double view_h, double line_h,
                                      std::size_t count) noexcept;
// Largest scroll offset (px) for `count` lines.
[[nodiscard]] double max_scroll(double view_h, double line_h, std::size_t count) noexcept;

} // namespace navigator::ui::inspector

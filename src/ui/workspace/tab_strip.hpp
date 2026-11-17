// src/ui/workspace/tab_strip.hpp
//
// Width math of a pane's tab strip (headless, unit-tested):
//   1. every tab wants its natural width (icon + label + close slot), clamped to
//      [min_width, max_width];
//   2. when they do not fit, the widest tabs shrink first, all to one common cap ("water
//      filling"), never below min_width, so short names keep their full label as long as
//      possible;
//   3. when even min_width tabs do not fit, the strip overflows: tabs keep min_width (the
//      active tab keeps up to 55% of the strip so its name stays readable) and the strip
//      scrolls horizontally (the view shows a chevron menu with every tab and fades the
//      clipped edges). `scroll` is clamped to [0, content - available].
// Positions are relative to the strip's left edge, before scrolling.
// Thread-safety: pure functions on values.
#pragma once

#include <cstddef>
#include <vector>

namespace navigator::ui {

struct TabStripInput {
    std::vector<float> natural; // px, one per tab
    float available = 0.0F;     // px for the tabs (the "+" button and menu are outside)
    float min_width = 0.0F;
    float max_width = 0.0F;
    float gap = 0.0F;    // px between tabs
    float scroll = 0.0F; // requested scroll offset (px)
    // Overflow only: this tab (the active one) keeps up to 55% of `available` so its label stays
    // readable in a narrow pane. Out of range: none.
    std::size_t active = static_cast<std::size_t>(-1);
};

struct TabStripLayout {
    std::vector<float> x; // left edge of each tab, strip coordinates (unscrolled)
    std::vector<float> w;
    float content = 0.0F; // total width of the tabs including gaps
    bool overflow = false;
    float scroll = 0.0F; // clamped
    [[nodiscard]] float max_scroll(float available) const noexcept;
};

[[nodiscard]] TabStripLayout layout_tab_strip(const TabStripInput& in);

// Scroll offset that brings tab `index` fully into view with the least movement.
[[nodiscard]] float scroll_to_show(const TabStripLayout& l, std::size_t index, float available,
                                   float current) noexcept;

// Insertion index for a dragged tab released at strip x `x` (unscrolled strip coordinates):
// the number of tabs whose centre lies left of x. Range [0, tab count].
[[nodiscard]] std::size_t insertion_index(const TabStripLayout& l, float x) noexcept;

} // namespace navigator::ui

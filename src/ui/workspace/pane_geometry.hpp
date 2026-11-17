// src/ui/workspace/pane_geometry.hpp
//
// Pixel rectangles of the split-pane tree (docs/architecture.md, "Phase 3"): every frame the ui
// turns the fractional sizes of layout::Layout into pane rectangles and splitter hit areas for
// the current window size. Pure math, headless (unit-tested in tests/ui).
//
//   - A split of extent E with n children gives each child sizes[i] * (E - (n - 1) * gap); the
//     gap between two children is the splitter's visible slot. Boundaries are rounded to whole
//     pixels so neighbouring panes never overlap or leave a seam.
//   - Splitter k of a split sits between children k and k + 1. Its hit area is `hit` px wide
//     (at least the gap), centred on the slot, and spans the split's cross extent.
//   - splitter_position() converts a pointer coordinate into the `position` argument of
//     Layout::move_splitter(): the boundary between child k and k + 1 as a fraction of the
//     split's extent measured from its start. min_fraction() turns a minimum pane size in px
//     into the matching `min_fraction`.
// Thread-safety: pure functions on values.
#pragma once

#include <cstddef>
#include <vector>

#include <navigator/layout.hpp>

#include "widgets/rect.hpp"

namespace navigator::ui {

struct PaneRect {
    layout::PaneId pane;
    RectF rect;
};

struct SplitterRect {
    layout::SplitId split;
    std::size_t index = 0; // boundary between children index and index + 1
    layout::SplitAxis axis = layout::SplitAxis::Horizontal;
    RectF slot;          // the gap between the two children (may be 0 px thick)
    RectF hit;           // pointer hit area
    float start = 0.0F;  // split rectangle start along the axis (px)
    float extent = 0.0F; // split rectangle extent along the axis (px)
    std::size_t children = 2;
    float gap = 0.0F;
};

struct PaneGeometry {
    std::vector<PaneRect> panes; // in Layout::pane_order()
    std::vector<SplitterRect> splitters;

    [[nodiscard]] const PaneRect* find(layout::PaneId pane) const noexcept;
    // Pane under (x, y), or nullptr.
    [[nodiscard]] const PaneRect* pane_at(float x, float y) const noexcept;
    // Splitter whose hit area contains (x, y), or nullptr. Deeper splitters win over the ones of
    // their ancestors where hit areas overlap.
    [[nodiscard]] const SplitterRect* splitter_at(float x, float y) const noexcept;
};

[[nodiscard]] PaneGeometry compute_pane_geometry(const layout::Layout& layout, RectF bounds,
                                                 float gap, float hit);

// Layout::move_splitter() position for the pointer coordinate `pointer` along the splitter's
// axis (x for Horizontal splits, y for Vertical). Not clamped (move_splitter clamps).
[[nodiscard]] double splitter_position(const SplitterRect& s, float pointer) noexcept;

// min_fraction for move_splitter() that keeps each child >= min_px, clamped to [0, 0.49].
[[nodiscard]] double min_fraction(const SplitterRect& s, float min_px) noexcept;

} // namespace navigator::ui

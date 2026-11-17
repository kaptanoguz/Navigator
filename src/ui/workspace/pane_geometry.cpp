// src/ui/workspace/pane_geometry.cpp — see pane_geometry.hpp.
#include "workspace/pane_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace navigator::ui {
namespace {

struct Builder {
    PaneGeometry& out;
    float gap;
    float hit;

    void visit(const layout::LayoutNode& node, RectF r) {
        if (const auto* pane = std::get_if<layout::PaneNode>(&node.node)) {
            out.panes.push_back(PaneRect{pane->id, r});
            return;
        }
        const auto& split = std::get<layout::SplitNode>(node.node);
        const std::size_t n = split.children.size();
        if (n == 0) {
            return;
        }
        const bool horizontal = split.axis == layout::SplitAxis::Horizontal;
        const float start = horizontal ? r.x : r.y;
        const float extent = horizontal ? r.w : r.h;
        const float g = n > 1 ? std::min(gap, extent / static_cast<float>(2 * (n - 1))) : 0.0F;
        const float available = std::max(extent - (g * static_cast<float>(n - 1)), 0.0F);
        double acc = 0.0;
        float prev_end = start;
        // Splitters are appended after the children so deeper ones come first in the list.
        std::vector<SplitterRect> mine;
        for (std::size_t i = 0; i < n; ++i) {
            const double share = i < split.sizes.size() ? split.sizes[i] : 1.0 / double(n);
            acc += share;
            const float child_start = i == 0 ? start : prev_end + g;
            float child_end = i + 1 == n
                                  ? start + extent
                                  : std::round(start + (static_cast<float>(acc) * available) +
                                               (g * static_cast<float>(i)));
            child_end = std::clamp(child_end, child_start, start + extent);
            const RectF child = horizontal ? RectF{child_start, r.y, child_end - child_start, r.h}
                                           : RectF{r.x, child_start, r.w, child_end - child_start};
            visit(split.children[i], child);
            if (i + 1 < n) {
                SplitterRect s;
                s.split = split.id;
                s.index = i;
                s.axis = split.axis;
                s.start = start;
                s.extent = extent;
                s.children = n;
                s.gap = g;
                const float h = std::max(hit, g);
                const float mid = child_end + (0.5F * g);
                if (horizontal) {
                    s.slot = RectF{child_end, r.y, g, r.h};
                    s.hit = RectF{mid - (0.5F * h), r.y, h, r.h};
                } else {
                    s.slot = RectF{r.x, child_end, r.w, g};
                    s.hit = RectF{r.x, mid - (0.5F * h), r.w, h};
                }
                mine.push_back(s);
            }
            prev_end = child_end;
        }
        out.splitters.insert(out.splitters.end(), mine.begin(), mine.end());
    }
};

} // namespace

const PaneRect* PaneGeometry::find(layout::PaneId pane) const noexcept {
    for (const PaneRect& p : panes) {
        if (p.pane == pane) {
            return &p;
        }
    }
    return nullptr;
}

const PaneRect* PaneGeometry::pane_at(float x, float y) const noexcept {
    for (const PaneRect& p : panes) {
        if (contains(p.rect, x, y)) {
            return &p;
        }
    }
    return nullptr;
}

const SplitterRect* PaneGeometry::splitter_at(float x, float y) const noexcept {
    for (const SplitterRect& s : splitters) {
        if (contains(s.hit, x, y)) {
            return &s;
        }
    }
    return nullptr;
}

PaneGeometry compute_pane_geometry(const layout::Layout& layout, RectF bounds, float gap,
                                   float hit) {
    PaneGeometry g;
    Builder b{g, std::max(gap, 0.0F), std::max(hit, 0.0F)};
    b.visit(layout.root(), bounds);
    return g;
}

double splitter_position(const SplitterRect& s, float pointer) noexcept {
    const auto n = static_cast<float>(s.children);
    const float available = s.extent - (s.gap * (n - 1.0F));
    if (available <= 0.0F) {
        return 0.5;
    }
    // Boundary k (fraction f of `available`) sits at start + f * available + k * gap; the
    // pointer grabs the middle of the slot after it.
    const auto k = static_cast<float>(s.index);
    const double f = (pointer - s.start - (k * s.gap) - (0.5F * s.gap)) / available;
    return std::isfinite(f) ? f : 0.5;
}

double min_fraction(const SplitterRect& s, float min_px) noexcept {
    const auto n = static_cast<float>(s.children);
    const float available = s.extent - (s.gap * (n - 1.0F));
    if (available <= 0.0F || min_px <= 0.0F) {
        return 0.0;
    }
    return std::clamp(static_cast<double>(min_px / available), 0.0, 0.49);
}

} // namespace navigator::ui

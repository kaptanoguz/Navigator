// src/ui/workspace/drag_drop.cpp — see drag_drop.hpp.
#include "workspace/drag_drop.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

namespace navigator::ui {

DropTarget hit_test_drop(const std::vector<PaneDropArea>& panes, float x, float y,
                         const DropZoneMetrics& metrics) {
    for (const PaneDropArea& p : panes) {
        if (contains(p.strip, x, y)) {
            DropTarget t;
            t.kind = DropKind::TabBar;
            t.pane = p.pane;
            t.index = static_cast<std::size_t>(std::count_if(
                p.tab_center.begin(), p.tab_center.end(), [x](float c) { return c < x; }));
            return t;
        }
    }
    for (const PaneDropArea& p : panes) {
        if (!contains(p.rect, x, y)) {
            continue;
        }
        // Content area below the strip.
        const float top = std::max(p.rect.y, p.strip.bottom());
        const RectF body{p.rect.x, top, p.rect.w, std::max(p.rect.bottom() - top, 0.0F)};
        DropTarget t;
        t.pane = p.pane;
        t.kind = DropKind::Centre;
        const float band_x = std::min(body.w * metrics.band_fraction, metrics.band_max);
        const float band_y = std::min(body.h * metrics.band_fraction, metrics.band_max);
        struct Candidate {
            layout::SplitEdge edge;
            float distance;
            float band;
        };
        const std::array candidates{
            Candidate{layout::SplitEdge::Left, x - body.x, band_x},
            Candidate{layout::SplitEdge::Right, body.right() - x, band_x},
            Candidate{layout::SplitEdge::Top, y - body.y, band_y},
            Candidate{layout::SplitEdge::Bottom, body.bottom() - y, band_y},
        };
        float best = std::numeric_limits<float>::max();
        for (const Candidate& c : candidates) {
            if (c.band <= 0.0F || c.distance < 0.0F || c.distance >= c.band) {
                continue;
            }
            const float relative = c.distance / c.band;
            if (relative < best) {
                best = relative;
                t.kind = DropKind::Edge;
                t.edge = c.edge;
            }
        }
        return t;
    }
    return DropTarget{};
}

RectF drop_preview(const PaneDropArea& area, const DropTarget& target) {
    const float top = std::max(area.rect.y, area.strip.bottom());
    const RectF body{area.rect.x, top, area.rect.w, std::max(area.rect.bottom() - top, 0.0F)};
    if (target.kind == DropKind::Centre) {
        return body;
    }
    if (target.kind != DropKind::Edge) {
        return RectF{};
    }
    const float hw = 0.5F * body.w;
    const float hh = 0.5F * body.h;
    switch (target.edge) {
    case layout::SplitEdge::Left: return RectF{body.x, body.y, hw, body.h};
    case layout::SplitEdge::Right: return RectF{body.x + hw, body.y, body.w - hw, body.h};
    case layout::SplitEdge::Top: return RectF{body.x, body.y, body.w, hh};
    case layout::SplitEdge::Bottom: return RectF{body.x, body.y + hh, body.w, body.h - hh};
    }
    return RectF{};
}

bool apply_drop(layout::Layout& layout, layout::TabId tab, const DropTarget& target) {
    const std::optional<layout::TabLocation> from = layout.find_tab(tab);
    if (!from) {
        return false;
    }
    switch (target.kind) {
    case DropKind::None: return false;
    case DropKind::TabBar: {
        std::size_t index = target.index;
        if (target.pane == from->pane) {
            if (index > from->index) {
                --index; // counted with the tab itself still in place
            }
        }
        return layout.move_tab(tab, target.pane, index).has_value();
    }
    case DropKind::Edge: return layout.split_pane(target.pane, target.edge, tab).has_value();
    case DropKind::Centre:
        if (target.pane == from->pane) {
            return false;
        }
        return layout.move_tab(tab, target.pane, std::numeric_limits<std::size_t>::max())
            .has_value();
    }
    return false;
}

void TabDrag::press(layout::TabId tab, layout::PaneId from, float x, float y) noexcept {
    state_ = State::Pressed;
    tab_ = tab;
    from_ = from;
    x0_ = x;
    y0_ = y;
    target_ = DropTarget{};
}

bool TabDrag::move(float x, float y, float threshold_px) noexcept {
    if (state_ != State::Pressed) {
        return false;
    }
    const float dx = x - x0_;
    const float dy = y - y0_;
    if ((dx * dx) + (dy * dy) > threshold_px * threshold_px) {
        state_ = State::Dragging;
        return true;
    }
    return false;
}

TabDrag::Outcome TabDrag::release() noexcept {
    Outcome o;
    o.tab = tab_;
    if (state_ == State::Pressed) {
        o.kind = Outcome::Kind::Click;
    } else if (state_ == State::Dragging) {
        o.kind = Outcome::Kind::Drop;
        o.target = target_;
    }
    cancel();
    return o;
}

void TabDrag::cancel() noexcept {
    state_ = State::Idle;
    tab_ = layout::TabId{};
    from_ = layout::PaneId{};
    target_ = DropTarget{};
}

} // namespace navigator::ui

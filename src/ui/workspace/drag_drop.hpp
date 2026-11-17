// src/ui/workspace/drag_drop.hpp
//
// Tab drag and drop as pure logic (docs/architecture.md, "Frame-end order and drag and drop"):
//
//   Idle --press on tab--> Pressed(tab, origin)
//   Pressed --move > threshold--> Dragging(tab)      Pressed --release--> Idle (+ Click)
//   Dragging: hit_test_drop() every frame -> DropTarget (TabBar / Edge / Centre / None)
//   Dragging --cancel (Escape, focus lost)--> Idle (no change)
//   Dragging --release--> Drop(tab, target) -> Idle
//
// The view feeds the machine and draws the feedback (insertion marker, split preview); the
// resulting Drop is applied at frame end with apply_drop(), one all-or-nothing Layout call.
// Thread-safety: plain values and pure functions (UI thread in practice).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <navigator/layout.hpp>
#include <navigator/types.hpp>

#include "widgets/rect.hpp"

namespace navigator::ui {

enum class DropKind : std::uint8_t {
    None,   // outside every pane: no change
    TabBar, // over a tab strip: move/reorder to `index`
    Edge,   // in a pane's outer band: split at `edge`
    Centre, // elsewhere in a pane: append to that pane
};

struct DropTarget {
    DropKind kind = DropKind::None;
    layout::PaneId pane;
    std::size_t index = 0; // TabBar: insertion index counted with the dragged tab still present
    layout::SplitEdge edge = layout::SplitEdge::Right;

    friend bool operator==(const DropTarget&, const DropTarget&) = default;
};

// What the hit test needs to know about one pane.
struct PaneDropArea {
    layout::PaneId pane;
    RectF rect;                    // the whole pane (tab strip + content)
    RectF strip;                   // its tab strip
    std::vector<float> tab_center; // window x of each tab's centre, in tab order
};

struct DropZoneMetrics {
    float band_fraction = 0.25F; // edge band thickness as a share of the pane's width/height
    float band_max = 160.0F;     // px cap of the band
};

// Drop target under (x, y). Strips win over pane bodies; within a body the nearest edge band
// (distance relative to that band's thickness) wins, otherwise Centre.
[[nodiscard]] DropTarget hit_test_drop(const std::vector<PaneDropArea>& panes, float x, float y,
                                       const DropZoneMetrics& metrics);

// Preview rectangle for an Edge or Centre target over `area` (content below the strip): the
// half the new pane would take, or the whole content area for Centre. Empty otherwise.
[[nodiscard]] RectF drop_preview(const PaneDropArea& area, const DropTarget& target);

// The Layout operation of a drop (architecture.md): TabBar -> move_tab (same pane: index
// adjusted for the tab's own removal), Edge -> split_pane, Centre -> move_tab to the end (same
// pane: no-op), None -> no-op. Returns true when the layout changed. A refused edit
// (InvalidArgument: a pane's only tab dropped on its own edge; NotFound: stale ids) is a no-op.
bool apply_drop(layout::Layout& layout, layout::TabId tab, const DropTarget& target);

class TabDrag {
public:
    enum class State : std::uint8_t { Idle, Pressed, Dragging };
    struct Outcome {
        enum class Kind : std::uint8_t { None, Click, Drop };
        Kind kind = Kind::None;
        layout::TabId tab;
        DropTarget target; // Drop only
    };

    void press(layout::TabId tab, layout::PaneId from, float x, float y) noexcept;
    // Pointer moved while the button is held. Returns true when the drag just started.
    bool move(float x, float y, float threshold_px) noexcept;
    // Remembers the current target (call every frame while Dragging).
    void hover(const DropTarget& target) noexcept { target_ = target; }
    // Button released: Click when it never became a drag, Drop with the last target otherwise.
    Outcome release() noexcept;
    void cancel() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool dragging() const noexcept { return state_ == State::Dragging; }
    [[nodiscard]] layout::TabId tab() const noexcept { return tab_; }
    [[nodiscard]] layout::PaneId origin_pane() const noexcept { return from_; }
    [[nodiscard]] const DropTarget& target() const noexcept { return target_; }
    [[nodiscard]] float press_x() const noexcept { return x0_; }
    [[nodiscard]] float press_y() const noexcept { return y0_; }

private:
    State state_ = State::Idle;
    layout::TabId tab_;
    layout::PaneId from_;
    float x0_ = 0.0F;
    float y0_ = 0.0F;
    DropTarget target_;
};

} // namespace navigator::ui

// src/ui/ops/ops_view.hpp
//
// Drawing of everything file operations show (Phase 7, docs/architecture.md "Phase 7",
// docs/review/phase-6.md "Notes for Phase 7"), immediate mode:
//   - the progress chip in the active pane's status bar (a thin bar and the operation's title),
//     expanding into the operations panel (per operation: icon, title, bar, items / bytes, rate
//     and ETA when the engine reports them, the current item, pause / resume, cancel; finished
//     operations collapse to one line with Undo and "Show in folder" and the failing items);
//   - the notice ("Moved 3 items to the trash.  Undo") bottom centre;
//   - the conflict sheet (source and destination side by side with thumbnails, Replace / Skip /
//     Keep both / Merge / Cancel, "Apply to all", the typed copy name), the permanent-delete,
//     untrashable, empty-trash and quit-while-running sheets (focus starts on the safe button);
//   - the drag badge and the drop target highlight (internal and incoming drags).
// Nothing here mutates state: buttons enqueue Commands or record Intents that the App applies at
// frame end. The progress chip, panel and notice never take keyboard focus; sheets are driven by
// the keys the App routes to FileOps.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/types.hpp>

#include "dnd/file_drag.hpp"
#include "ops/file_ops.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/rect.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

// What the drag badge shows (an internal drag or an incoming drop).
struct DragVisual {
    bool active = false;
    bool external = false; // from another program (incoming drop)
    DropEval eval;
    std::size_t count = 0;
    fs::DirEntry first; // glyph and name of the first item
    float x = 0.0F;     // pointer, px
    float y = 0.0F;
    bool pointer_inside = true;
    // Outgoing: the pointer left the window; what the badge says then.
    std::string outgoing_note;
};

using SheetThumbFn =
    std::function<bool(const Path& path, const fs::DirEntry& entry, const RectF& box)>;

struct OpsFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const FileOps& ops;
    const PointerInput& pointer;
    CommandQueue& queue;
    std::vector<Intent>& intents;
    float width = 0.0F; // px, window
    float height = 0.0F;
    RectF content; // the panes' area
    Density density = Density::Compact;
    double now_ms = 0.0;
    double dt_ms = 16.0;
    bool animations = true;
    Timestamp now{};
    float notice_lift_px = 0.0F; // a toast sits at the default position: stack above it
    SheetThumbFn thumb;          // conflict sheet previews (empty: glyphs only)
    const DragVisual* drag = nullptr;
};

class OpsView {
public:
    // Width (px) the chip needs in the status bar; 0: nothing to show.
    [[nodiscard]] float chip_width(const OpsFrame& f);
    // The chip inside `slot` (px; right-aligned, vertically centred in the status bar).
    void draw_chip(const OpsFrame& f, const RectF& slot);
    // Panel, notice, sheets, drag visuals: after the panes and the toasts.
    void draw_overlays(const OpsFrame& f);

    // Something animates (progress bar, spinner): keep frames coming.
    [[nodiscard]] bool animating() const noexcept { return animating_; }
    // The pointer is over an element of this view (the panes below must not react).
    [[nodiscard]] bool hit(float x, float y) const;
    // A modal sheet (scrim) is open: the panes get no pointer at all.
    [[nodiscard]] bool modal() const noexcept { return modal_; }
    void clear_caches() noexcept { fitter_.clear(); }

private:
    [[nodiscard]] bool button(const OpsFrame& f, const RectF& r, std::size_t id,
                              bool focused_ring = false);
    void draw_panel(const OpsFrame& f);
    void draw_notice(const OpsFrame& f);
    void draw_conflict(const OpsFrame& f);
    void draw_confirm(const OpsFrame& f);
    void draw_quit(const OpsFrame& f);
    void draw_drag(const OpsFrame& f);
    RectF card(const OpsFrame& f, const RectF& r, float radius_dp, bool scrim);
    void progress_bar(const OpsFrame& f, const RectF& r, std::optional<double> fraction,
                      bool paused);
    void op_glyph(const OpsFrame& f, ops::OpKind kind, float cx, float cy, float size,
                  const Color& color);
    // A sheet button: returns true when clicked.
    bool sheet_button(const OpsFrame& f, const RectF& r, std::string_view label, bool primary,
                      bool danger, bool focused, std::size_t id);
    float keycap_row(const OpsFrame& f, const RectF& area,
                     const std::vector<std::pair<std::string, std::string>>& hints);

    std::vector<RectF> hits_;
    std::vector<RectF> next_hits_;
    bool animating_ = false;
    bool modal_ = false;
    std::optional<std::size_t> pressed_;
    TextFitter fitter_;
    float conflict_name_scroll_ = 0.0F;
    float panel_scroll_ = 0.0F;
};

} // namespace navigator::ui

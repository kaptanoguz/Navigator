// src/ui/browser/browser_view.hpp
//
// One tab's browser inside a pane, immediate mode: navigation bar (Back / Forward / Up,
// breadcrumb path), notice banner, the virtualized Details list (sticky column header, rows,
// rubber band, overlay scrollbar, empty states) and the pane's status line, all inside
// BrowserFrame::bounds. Phase 3: one BrowserView per tab (scroll and pointer state survive tab
// switches); the workspace view draws the tab strip, splitters and window-level overlays.
//
// Contract with the App (frame model in docs/architecture.md):
//   update(frame)  before drawing: consumes the wheel, follows the cursor, animates scrolling.
//                  Touches only view state (scroll, pressed widget).
//   draw(frame)    reads the model, draws through the Painter, and records pointer gestures as
//                  Intents or enqueues commands. Never mutates the model.
// Virtualization: only rows that intersect the list viewport are measured, shaped and drawn,
// so the per-frame cost is independent of the entry count.
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

#include "browser/browser_model.hpp"
#include "render/renderer.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct BrowserFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const BrowserModel& model;
    const Keymap& keymap;
    const PointerInput& pointer;
    CommandQueue& queue;
    std::vector<Intent>& intents;
    float width = 0.0F; // px, window
    float height = 0.0F;
    RectF bounds;          // px, the area this view draws in (below the pane's tab strip)
    std::uint32_t tab = 0; // layout::TabId value: every recorded Intent carries it
    bool focused = true;   // window has keyboard focus AND this is the active pane
    // Phase 3: false in an inactive pane (quieter breadcrumb). marked: draw the active-pane
    // accent frame around the list (only with more than one pane).
    bool pane_active = true;
    bool marked = false;
    bool animations = true;
    Density density = Density::Compact;
    double dt_ms = 16.0;        // since the previous frame
    Timestamp now{};            // for "Today"/"Yesterday"
    const Path* home = nullptr; // breadcrumb "Home" root (may be null)
};

// Performance overlay (F12) at the bottom-right of the window.
void draw_fps_overlay(const Painter& p, const Palette& pal, float width, float height,
                      Density density, double fps, double frame_ms, std::string_view driver);

class BrowserView {
public:
    BrowserView() = default;

    void update(const BrowserFrame& frame);
    void draw(const BrowserFrame& frame);

    // Smooth scroll or a pointer drag is in progress: the App keeps rendering continuously.
    [[nodiscard]] bool animating() const noexcept;
    // Rows that fit in the list viewport (for CursorPageUp/Down) and the scroll offset (dp).
    [[nodiscard]] std::size_t page_rows() const noexcept { return page_rows_; }
    [[nodiscard]] float scroll() const noexcept { return scroll_; }
    [[nodiscard]] float max_scroll() const noexcept { return max_scroll_; }

    // Development: scroll by `dp` immediately (benchmark), force a hovered row (screenshots).
    void scroll_by(float dp) noexcept;
    void set_hover_override(std::size_t row) noexcept { hover_override_ = row; }

    void clear_caches() noexcept { fitter_.clear(); }

private:
    struct Layout {
        RectF top;
        RectF notice;
        RectF panel;
        RectF header;
        RectF body;
        RectF status;
        float pad = 0.0F;   // px above the first / below the last row
        float row_h = 0.0F; // px
        // Column x extents (px).
        float name_x = 0.0F; // glyph left
        float text_x = 0.0F; // name text left
        float name_r = 0.0F;
        float size_x = 0.0F;
        float size_r = 0.0F;
        float type_x = 0.0F;
        float type_r = 0.0F;
        float date_x = 0.0F;
        float date_r = 0.0F;
        bool show_size = true;
        bool show_type = true;
        bool show_date = true;
        bool short_date = false;
    };
    struct Crumb {
        std::string label;
        Path path;
        bool ellipsis = false;
    };

    [[nodiscard]] Layout compute_layout(const BrowserFrame& f) const;
    [[nodiscard]] float scroll_px(const BrowserFrame& f) const;
    void clamp_scroll() noexcept;
    void ensure_visible(std::size_t row, const Layout& l, const BrowserFrame& f);
    [[nodiscard]] std::size_t row_at(float y, const Layout& l, const BrowserFrame& f) const;

    void draw_top_bar(const BrowserFrame& f, const Layout& l);
    bool tool_button(const BrowserFrame& f, const RectF& r, WidgetId id, bool enabled, int glyph);
    void draw_breadcrumbs(const BrowserFrame& f, const RectF& area);
    void draw_notice(const BrowserFrame& f, const Layout& l);
    void draw_header(const BrowserFrame& f, const Layout& l);
    void draw_rows(const BrowserFrame& f, const Layout& l);
    void handle_list_pointer(const BrowserFrame& f, const Layout& l);
    void draw_band(const BrowserFrame& f, const Layout& l);
    void draw_scrollbar(const BrowserFrame& f, const Layout& l);
    void draw_empty_state(const BrowserFrame& f, const Layout& l);
    void draw_status(const BrowserFrame& f, const Layout& l);
    float draw_key_hint(const BrowserFrame& f, std::string_view command, std::string_view label,
                        float center_x, float center_y, bool measure_only);

    // Scroll state (dp).
    float scroll_ = 0.0F;
    float target_ = 0.0F;
    float max_scroll_ = 0.0F;
    std::size_t page_rows_ = 1;
    std::uint64_t seen_location_ = 0;
    std::uint64_t seen_cursor_ = 0;
    std::uint64_t seen_content_ = 0;
    bool primed_ = false;

    // Pointer capture.
    WidgetId active_ = WidgetId::None;
    std::size_t active_index_ = 0;
    float press_x_ = 0.0F;
    float press_y_ = 0.0F; // content y (px, includes scroll) of a list press
    std::size_t press_row_ = kNoRow;
    Modifiers press_mods_ = Modifiers::None;
    bool band_started_ = false;
    float band_cx_ = 0.0F; // current band corner (px window x, content y)
    float band_cy_ = 0.0F;
    float drag_offset_ = 0.0F; // scrollbar thumb grab offset (px)
    std::size_t last_band_first_ = 1;
    std::size_t last_band_last_ = 0;
    std::size_t hover_override_ = kNoRow;

    TextFitter fitter_;
};

} // namespace navigator::ui

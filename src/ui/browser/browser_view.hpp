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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/index.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

#include "browser/browser_model.hpp"
#include "dnd/file_drag.hpp"
#include "ops/file_ops.hpp"
#include "render/renderer.hpp"
#include "shell/painter.hpp"
#include "theme/theme.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

// Phase 6 list thumbnails (implemented by inspector/list_thumbnails.hpp): the view asks for the
// rows it draws and a few just off screen; a ready thumbnail replaces the type glyph.
class RowThumbnailSource {
public:
    RowThumbnailSource() = default;
    virtual ~RowThumbnailSource() = default;
    RowThumbnailSource(const RowThumbnailSource&) = delete;
    RowThumbnailSource& operator=(const RowThumbnailSource&) = delete;
    RowThumbnailSource(RowThumbnailSource&&) = delete;
    RowThumbnailSource& operator=(RowThumbnailSource&&) = delete;

    // Draws the thumbnail of `row` into the glyph box `box` (px) and returns true, or returns
    // false (not an image/video, not ready yet: the glyph is drawn). Requests it when missing.
    virtual bool draw_row(Renderer& r, const Palette& pal, const BrowserModel& model,
                          std::size_t row, const RectF& box, float scale, float dim) = 0;
    // A row just off screen: requested with Prefetch priority, nothing drawn.
    virtual void prefetch_row(const BrowserModel& model, std::size_t row, float box_px) = 0;
};

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
    // Phase 4: the address bar of this tab is in edit mode (nav.focus_address_bar) and shows
    // this field instead of the breadcrumbs; error: the field's frame turns red.
    const TextEdit* address = nullptr;
    bool address_error = false;
    bool bookmarked = false; // the star button is filled
    // Phase 5: this tab's filter field has the keyboard (accent ring, caret, list cursor drawn
    // inactive), and the index of the volume a whole-volume search runs on (status bar).
    bool filter_focused = false;
    std::optional<search::IndexStatus> index_status;
    // Phase 6: list thumbnails (null: type glyphs only).
    RowThumbnailSource* thumbnails = nullptr;
    // Phase 7: cut rows are dimmed (null: none), the inline rename editor of this tab (null:
    // none), where folder rows / breadcrumbs / the pane register their drop slots, whether a
    // file drag is in progress (edge auto-scroll) and whether a press on a row may start one.
    const FileOps* file_ops = nullptr;
    const InlineEdit* inline_edit = nullptr;
    std::vector<DropSlot>* slots = nullptr;
    bool drag_active = false;
    bool drag_enabled = true;
    // Phase 7: px kept free at the right end of the status bar (the progress chip).
    float status_reserve = 0.0F;
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

    // Phase 4: the address field's rectangle (px) from the last draw in edit mode, or the
    // breadcrumb area otherwise (the dropdown hangs below it).
    [[nodiscard]] const RectF& address_rect() const noexcept { return address_rect_; }
    // Phase 5: the types button (the types popup hangs below it) and the filter field.
    [[nodiscard]] const RectF& types_rect() const noexcept { return types_rect_; }
    [[nodiscard]] const RectF& filter_rect() const noexcept { return filter_rect_; }
    // A spinner or progress bar is animating (keep frames coming).
    [[nodiscard]] bool busy_animating() const noexcept { return spinning_; }
    // Phase 7: the inline editor's rectangle from the last draw (empty: none).
    [[nodiscard]] const RectF& inline_rect() const noexcept { return inline_rect_; }

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
        // Phase 5: the flattened view's Folder column (or the parent path after the name in
        // narrow panes).
        bool show_folder = false;
        bool inline_parent = false;
        float folder_x = 0.0F;
        float folder_r = 0.0F;
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
    void draw_address_field(const BrowserFrame& f, const RectF& area);
    void draw_notice(const BrowserFrame& f, const Layout& l);
    void draw_header(const BrowserFrame& f, const Layout& l);
    void draw_rows(const BrowserFrame& f, const Layout& l);
    void handle_list_pointer(const BrowserFrame& f, const Layout& l);
    void draw_band(const BrowserFrame& f, const Layout& l);
    void draw_scrollbar(const BrowserFrame& f, const Layout& l);
    void draw_empty_state(const BrowserFrame& f, const Layout& l);
    void draw_status(const BrowserFrame& f, const Layout& l);
    // Phase 5 (browser_search_view.cpp).
    void draw_filter_field(const BrowserFrame& f, const RectF& area, bool collapsed);
    bool toggle_button(const BrowserFrame& f, const RectF& r, WidgetId id, int icon, bool on,
                       std::string_view badge);
    void draw_progress(const BrowserFrame& f, const Layout& l) const;
    void draw_search_empty(const BrowserFrame& f, const Layout& l);
    float draw_search_status(const BrowserFrame& f, const Layout& l, float x, float limit);
    void draw_spinner(const BrowserFrame& f, float cx, float cy, float radius) const;
    [[nodiscard]] float crumbs_width(const BrowserFrame& f) const;
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
    name_highlights(const BrowserFrame& f, const std::string& raw, const std::string& display,
                    const std::string& shown);
    [[nodiscard]] std::string folder_text(const BrowserFrame& f, std::size_t row) const;
    void draw_tooltip(const BrowserFrame& f);
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
    RectF address_rect_;
    std::size_t middle_crumb_ = static_cast<std::size_t>(-1);
    float address_scroll_ = 0.0F;

    // Phase 7: press on a row that may become a drag, and the inline editor.
    std::size_t drag_row_ = kNoRow;
    bool deferred_click_ = false; // the click of a press on a selected row waits for the release
    bool drag_started_ = false;
    bool inline_active_ = false;
    float inline_scroll_ = 0.0F;
    RectF inline_rect_;
    std::string inline_message_;
    bool inline_message_error_ = false;
    RectF inline_message_anchor_;

    // Phase 5.
    RectF types_rect_;
    RectF filter_rect_;
    float filter_scroll_ = 0.0F;
    std::string tooltip_text_; // set while drawing a hovered control; drawn last, then cleared
    RectF tooltip_anchor_;
    double spin_ms_ = 0.0;
    bool spinning_ = false;
    std::uint64_t hl_gen_ = ~std::uint64_t{0};
    search::Matcher hl_matcher_;
    std::unordered_map<std::string, std::vector<search::HighlightRange>> hl_cache_;
};

} // namespace navigator::ui

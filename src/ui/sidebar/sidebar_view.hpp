// src/ui/sidebar/sidebar_view.hpp
//
// The window's sidebar (Phase 4), immediate mode: a top bar (hide button, theme switch), then
// the sections built by build_sidebar_rows() -- collapsible headers, bookmark / recent / place
// rows, storage rows with a usage bar and percent, offline / missing badges, empty-state hints
// with key caps -- a resize edge (160-480 dp, double-click: 220 dp), the bookmark rename field,
// bookmark drag reordering and the drop marker for a tab dragged onto Bookmarks.
// Icon-only mode (narrow windows, derived by the App each frame, never persisted): 52 dp, one
// icon per row, hairlines between sections, a clock button for Recents (opens GoTo), tooltips.
//
// Contract (frame model): draw() reads the rows and records Intents / enqueues commands; it
// never mutates program state. The App applies the intents at frame end. Rows outside the
// viewport are not drawn (the list is short, but the rule stands).
// Thread-safety: UI thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/layout.hpp>
#include <navigator/places.hpp>
#include <navigator/ui.hpp>

#include "dnd/file_drag.hpp"
#include "shell/painter.hpp"
#include "sidebar/sidebar_model.hpp"
#include "theme/theme.hpp"
#include "widgets/text_edit.hpp"
#include "widgets/text_fit.hpp"
#include "widgets/ui_context.hpp"

namespace navigator::ui {

struct SidebarFrame {
    const Painter& painter;
    const Palette& palette;
    Theme theme;
    const std::vector<SidebarRow>& rows;
    const Keymap& keymap;
    const PointerInput& pointer; // masked by the App while a popup owns the pointer
    CommandQueue& queue;
    std::vector<Intent>& intents;
    RectF rect;             // px, the sidebar's area
    bool icon_only = false; // narrow window
    bool focused = false;   // keyboard focus is in the sidebar
    std::size_t focus_row = static_cast<std::size_t>(-1);
    bool window_focused = true;
    Density density = Density::Compact;
    layout::TabId dragged_tab; // a tab drag is in progress (drop onto Bookmarks)
    bool popup_open = false;
    // Phase 7: rows register themselves as drop targets (null: none).
    std::vector<DropSlot>* slots = nullptr;
};

class SidebarView {
public:
    static constexpr float kIconOnlyWidth = 52.0F; // dp
    static constexpr float kResizeHit = 6.0F;      // dp, half on each side of the edge

    void draw(const SidebarFrame& f);
    // Tooltips (icon-only mode) and drag feedback, drawn after the panes.
    void draw_overlay(const SidebarFrame& f);

    [[nodiscard]] bool animating() const noexcept;
    // The pointer is over the resize edge or dragging it.
    [[nodiscard]] bool resize_cursor() const noexcept { return resize_hot_ || resizing_; }
    // A press of this frame landed in the sidebar (the workspace ignores it).
    [[nodiscard]] bool owns_pointer() const noexcept { return owns_pointer_; }
    [[nodiscard]] bool dragging() const noexcept { return drag_row_.has_value() && drag_moved_; }

    // Inline bookmark rename.
    void begin_rename(places::BookmarkId id, std::string_view current);
    void end_rename() noexcept { rename_.reset(); }
    [[nodiscard]] bool renaming() const noexcept { return rename_.has_value(); }
    [[nodiscard]] places::BookmarkId rename_target() const noexcept {
        return rename_ ? rename_->id : places::BookmarkId{};
    }
    [[nodiscard]] TextEdit* rename_edit() noexcept { return rename_ ? &rename_->edit : nullptr; }

    // Keeps a keyboard-focused row in view.
    void reveal(std::size_t row) noexcept { reveal_ = row; }
    // Development: force the hovered row (screenshots).
    void set_hover_override(std::size_t row) noexcept { hover_override_ = row; }
    void clear_caches() noexcept { fitter_.clear(); }
    // Physical rectangle of a row from the last draw (scripts, tests); nullopt when hidden.
    [[nodiscard]] std::optional<RectF> row_rect(std::size_t row) const;

private:
    struct Rename {
        Rename() : edit(256) {}
        places::BookmarkId id;
        TextEdit edit;
        float scroll = 0.0F;
    };
    struct Placed {
        std::size_t row;
        RectF rect;
    };
    [[nodiscard]] float row_height(const SidebarFrame& f, const SidebarRow& row) const;
    void draw_top(const SidebarFrame& f, const RectF& top);
    void draw_header(const SidebarFrame& f, const SidebarRow& row, std::size_t index,
                     const RectF& r, bool hover);
    void draw_item(const SidebarFrame& f, const SidebarRow& row, std::size_t index, const RectF& r,
                   bool hover);
    void draw_empty(const SidebarFrame& f, const SidebarRow& row, const RectF& r);
    void draw_icon_row(const SidebarFrame& f, const SidebarRow& row, std::size_t index,
                       const RectF& r, bool hover);
    void handle_resize(const SidebarFrame& f);
    bool button(const SidebarFrame& f, const RectF& r, std::size_t index);

    float scroll_ = 0.0F; // px
    float content_h_ = 0.0F;
    std::vector<Placed> placed_;
    std::optional<std::size_t> pressed_row_;
    WidgetId active_ = WidgetId::None;
    std::size_t active_index_ = 0;
    std::optional<std::size_t> middle_row_;
    std::optional<std::size_t> drag_row_; // bookmark row being dragged
    bool drag_moved_ = false;
    float press_x_ = 0.0F;
    float press_y_ = 0.0F;
    std::optional<std::size_t> drop_index_; // insertion index among bookmarks (drag feedback)
    float drop_y_ = 0.0F;
    bool resizing_ = false;
    bool resize_hot_ = false;
    bool owns_pointer_ = false;
    std::optional<std::size_t> tooltip_row_;
    RectF tooltip_anchor_;
    std::optional<Rename> rename_;
    std::size_t reveal_ = static_cast<std::size_t>(-1);
    std::size_t hover_override_ = static_cast<std::size_t>(-1);
    TextFitter fitter_;
};

} // namespace navigator::ui

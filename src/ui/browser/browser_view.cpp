// src/ui/browser/browser_view.cpp — layout, scrolling and the virtualized Details list.
// Chrome (top bar, notice, status bar, empty states, overlay) is in browser_chrome.cpp.
#include "browser/browser_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

// sRGB "over" of a translucent overlay on an opaque base (what the GPU blend does).
Color over(const Color& base, const Color& overlay) {
    const float a = overlay.a;
    return Color{(base.r * (1.0F - a)) + (overlay.r * a), (base.g * (1.0F - a)) + (overlay.g * a),
                 (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
}

constexpr float kWheelRows = 3.0F;         // rows per wheel notch
constexpr float kDragThreshold = 4.0F;     // dp before a press becomes a band
constexpr float kColumnGap = kColumnGapDp; // dp (format.hpp, shared with plan_columns)
constexpr float kSizeWidth = kSizeWidthDp;
constexpr float kTypeWidth = kTypeWidthDp;
constexpr float kDateWidth = kDateWidthDp;
constexpr float kTextPad = 10.0F;          // row highlight edge -> content
constexpr float kGlyphGap = 10.0F;         // glyph -> name text
constexpr float kListPad = 4.0F;           // above the first row
constexpr float kFolderMinRoomDp = 260.0F; // name room needed before Folder gets a column

} // namespace

// ---------------------------------------------------------------------------------------------
// Layout and scrolling
// ---------------------------------------------------------------------------------------------

BrowserView::Layout BrowserView::compute_layout(const BrowserFrame& f) const {
    const Painter& p = f.painter;
    const DensityMetrics dm = density_metrics(f.density, f.spacing_scale_percent);
    Layout l;
    const RectF b = f.bounds;
    const float w = b.w;
    const float margin = std::round(p.px(dm.panel_margin));
    l.top = RectF{b.x, b.y, w, std::round(p.px(dm.top_bar_height))};
    float y = l.top.bottom();
    if (f.model.notice_shown()) {
        l.notice = RectF{b.x + margin, y, w - (2.0F * margin), std::round(p.px(dm.notice_height))};
        y = l.notice.bottom() + margin;
    }
    const float status_h = std::round(p.px(dm.status_bar_height));
    l.status = RectF{b.x, b.bottom() - status_h, w, status_h};
    l.panel =
        RectF{b.x + margin, y, std::max(w - (2.0F * margin), 0.0F), std::max(l.status.y - y, 0.0F)};
    const bool has_header = f.model.view_mode() == view::ViewMode::Details ||
                            f.model.view_mode() == view::ViewMode::Tree;
    l.header = RectF{l.panel.x, l.panel.y, l.panel.w,
                     has_header ? std::round(p.px(dm.header_height)) : 0.0F};
    l.body = RectF{l.panel.x, l.header.bottom(), l.panel.w,
                   std::max(l.panel.bottom() - l.header.bottom(), 0.0F)};
    l.pad = std::round(p.px(kListPad));
    l.row_h = std::round(p.px(dm.row_height));

    const float inset = p.px(metrics::row_inset);
    const float left = std::round(l.panel.x + inset + p.px(kTextPad));
    const float right = std::round(l.panel.right() - inset - p.px(kTextPad));
    const float gap = std::round(p.px(kColumnGap));
    const float icon_gap_px = std::round(p.px(metrics::icon_size + kGlyphGap));

    // Columns degrade gracefully in narrow panes (plan_columns): Type goes first, then
    // Modified shortens, then Size goes; Modified is the last to go. Name always stays.
    const ColumnPlan plan = plan_columns((right - (left + icon_gap_px)) / p.scale());
    l.show_size = plan.size;
    l.show_type = plan.type;
    l.show_date = plan.date;
    l.short_date = plan.short_date;

    const float size_w = l.show_size ? std::round(p.px(kSizeWidth)) : 0.0F;
    const float type_w = l.show_type ? std::round(p.px(kTypeWidth)) : 0.0F;
    const float date_w =
        l.show_date ? std::round(p.px(l.short_date ? kShortDateWidthDp : kDateWidth)) : 0.0F;
    const int vis_meta = (l.show_size ? 1 : 0) + (l.show_type ? 1 : 0) + (l.show_date ? 1 : 0);
    const float fixed_w = size_w + type_w + date_w + (static_cast<float>(vis_meta) * gap);
    const float name_w = std::max((right - left) - fixed_w, icon_gap_px + p.px(40.0F));

    // Lay out visible columns left-to-right in the user's configured column order (Phase 9A).
    float cx = left;
    const view::ColumnOrder& order = f.model.column_order();
    for (view::ColumnId col : order) {
        switch (col) {
        case view::ColumnId::Name:
            l.name_x = cx;
            l.text_x = std::round(cx + icon_gap_px);
            l.name_r = cx + name_w;
            cx = l.name_r + gap;
            break;
        case view::ColumnId::Size:
            if (l.show_size) {
                l.size_x = cx;
                l.size_r = cx + size_w;
                cx = l.size_r + gap;
            }
            break;
        case view::ColumnId::Type:
            if (l.show_type) {
                l.type_x = cx;
                l.type_r = cx + type_w;
                cx = l.type_r + gap;
            }
            break;
        case view::ColumnId::Modified:
            if (l.show_date) {
                l.date_x = cx;
                l.date_r = cx + date_w;
                cx = l.date_r + gap;
            }
            break;
        }
    }

    // Phase 5: a flattened view adds the Folder column (relative parent path) next to Name so
    // rows with the same name can be told apart; narrow panes show it after the name instead.
    if (f.model.flattened()) {
        const float room = l.name_r - l.text_x;
        if (room >= p.px(kFolderMinRoomDp)) {
            const float fw = std::round(std::clamp(room * 0.42F, p.px(110.0F), p.px(320.0F)));
            l.show_folder = true;
            l.folder_r = l.name_r;
            l.folder_x = l.folder_r - fw;
            l.name_r = l.folder_x - gap;
        } else {
            l.inline_parent = true;
        }
    }
    return l;
}

float BrowserView::scroll_px(const BrowserFrame& f) const {
    return std::round(f.painter.px(scroll_));
}

void BrowserView::clamp_scroll() noexcept {
    target_ = std::clamp(target_, 0.0F, max_scroll_);
    scroll_ = std::clamp(scroll_, 0.0F, max_scroll_);
}

void BrowserView::scroll_by(float dp) noexcept {
    target_ += dp;
    scroll_ = target_;
    clamp_scroll();
    target_ = scroll_;
}

bool BrowserView::animating() const noexcept {
    return std::abs(target_ - scroll_) > 0.01F || active_ == WidgetId::Band ||
           active_ == WidgetId::Scrollbar;
}

std::size_t BrowserView::row_at(float y, const Layout& l, const BrowserFrame& f) const {
    const float content_y = y - l.body.y + scroll_px(f) - l.pad;
    if (content_y < 0.0F || l.row_h <= 0.0F) {
        return kNoRow;
    }
    const auto row = static_cast<std::size_t>(content_y / l.row_h);
    return row < f.model.row_count() ? row : kNoRow;
}

void BrowserView::ensure_visible(std::size_t row, const Layout& l, const BrowserFrame& f) {
    if (row == kNoRow || l.row_h <= 0.0F) {
        return;
    }
    const float scale = f.painter.scale();
    const float row_dp = l.row_h / scale;
    const float top = (l.pad / scale) + (static_cast<float>(row) * row_dp);
    const float view = l.body.h / scale;
    if (top < target_) {
        target_ = top - (l.pad / scale);
    } else if (top + row_dp > target_ + view) {
        target_ = top + row_dp + (l.pad / scale) - view;
    }
    target_ = std::clamp(target_, 0.0F, max_scroll_);
}

void BrowserView::update(const BrowserFrame& f) {
    const Layout l = compute_layout(f);
    const BrowserModel& m = f.model;
    const float scale = f.painter.scale();
    if (m.group_by() != view::GroupBy::None && !m.flattened() &&
        (m.view_mode() == view::ViewMode::Details || m.view_mode() == view::ViewMode::Icons)) {
        ensure_grouped_cache(f);
    }
    const float content_dp = compute_mode_content_dp(f, l);
    max_scroll_ = std::max(0.0F, content_dp - (l.body.h / scale));
    page_rows_ =
        l.row_h > 0.0F ? std::max<std::size_t>(1, static_cast<std::size_t>(l.body.h / l.row_h)) : 1;

    // Horizontal scroll extent for List/Columns mode.
    if (m.view_mode() == view::ViewMode::List && l.row_h > 0.0F) {
        const float pad_px = std::round(f.painter.px(6.0F));
        const std::size_t rows_per_col = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::max(l.body.h - (2.0F * pad_px), 0.0F) / l.row_h));
        const std::size_t total_cols = (m.row_count() + rows_per_col - 1) / rows_per_col;
        const float total_w_dp = 12.0F + (static_cast<float>(total_cols) * 228.0F);
        hmax_scroll_ = std::max(0.0F, total_w_dp - (l.body.w / scale));
    } else {
        hmax_scroll_ = 0.0F;
        hscroll_ = htarget_ = 0.0F;
    }

    if (!primed_ || m.location_serial() != seen_location_) {
        primed_ = true;
        seen_location_ = m.location_serial();
        scroll_ = target_ = m.restored_scroll();
        clamp_scroll();
        seen_cursor_ = m.cursor_serial();
        ensure_visible(m.cursor(), l, f);
        scroll_ = target_; // a new folder appears in place, no scroll animation
        seen_content_ = m.content_serial();
    }
    if (m.cursor_serial() != seen_cursor_) {
        seen_cursor_ = m.cursor_serial();
        if (active_ != WidgetId::Band) {
            ensure_visible(m.cursor(), l, f);
        }
    }
    if (m.content_serial() != seen_content_) {
        seen_content_ = m.content_serial();
        clamp_scroll();
    }

    const PointerInput& ptr = f.pointer;
    if ((ptr.wheel_y != 0.0F || ptr.wheel_x != 0.0F) && !has(ptr.mods, Modifiers::Ctrl) &&
        contains(l.panel, ptr.x, ptr.y)) {
        if (m.view_mode() == view::ViewMode::List) {
            const float delta = ptr.wheel_x != 0.0F ? ptr.wheel_x : -ptr.wheel_y;
            htarget_ = std::clamp(htarget_ + (delta * 114.0F), 0.0F, hmax_scroll_);
            if (!f.animations) {
                hscroll_ = htarget_;
            }
        } else if (ptr.wheel_y != 0.0F) {
            target_ -= ptr.wheel_y * kWheelRows *
                       density_metrics(f.density, f.spacing_scale_percent).row_height;
            target_ = std::clamp(target_, 0.0F, max_scroll_);
            if (!f.animations) {
                scroll_ = target_;
            }
        }
    }

    // Rubber band near the edges auto-scrolls.
    if (active_ == WidgetId::Band && band_started_ && ptr.down) {
        const float edge = f.painter.px(24.0F);
        float speed = 0.0F;
        if (ptr.y < l.body.y + edge) {
            speed = -(l.body.y + edge - ptr.y);
        } else if (ptr.y > l.body.bottom() - edge) {
            speed = ptr.y - (l.body.bottom() - edge);
        }
        if (speed != 0.0F) {
            scroll_by(std::clamp(speed / scale, -60.0F, 60.0F) * 0.35F);
        }
    }
    // Phase 7: a file drag near the top or bottom edge of this list scrolls it (faster closer to
    // the edge); the wheel works as usual because `ptr` keeps its wheel.
    if (f.drag_active && ptr.inside) {
        const float edge = f.painter.px(24.0F);
        const RectF zone{l.panel.x, l.body.y - edge, l.panel.w, l.body.h + (2.0F * edge)};
        if (contains(zone, ptr.x, ptr.y)) {
            float speed = 0.0F;
            if (ptr.y < l.body.y + edge) {
                speed = -(l.body.y + edge - ptr.y);
            } else if (ptr.y > l.body.bottom() - edge) {
                speed = ptr.y - (l.body.bottom() - edge);
            }
            if (speed != 0.0F) {
                scroll_by(std::clamp(speed / scale, -60.0F, 60.0F) * 0.35F);
            }
        }
    }
    // Phase 7: the inline editor scrolls its row into view when it opens.
    if (f.inline_edit != nullptr && !inline_active_) {
        const std::size_t row = m.find_row(f.inline_edit->original);
        if (row != kNoRow) {
            ensure_visible(row, l, f);
            scroll_ = target_;
        }
    }
    inline_active_ = f.inline_edit != nullptr;
    // Scrollbar drag.
    if (active_ == WidgetId::Scrollbar && ptr.down && max_scroll_ > 0.0F) {
        const float content_px = content_dp * scale;
        const float thumb_h = std::max(f.painter.px(32.0F), l.body.h * (l.body.h / content_px));
        const float travel = std::max(l.body.h - thumb_h, 1.0F);
        const float t = std::clamp((ptr.y - drag_offset_ - l.body.y) / travel, 0.0F, 1.0F);
        target_ = scroll_ = t * max_scroll_;
    }

    // Phase 5: the spinner and the indeterminate progress bar run while a search does.
    spinning_ = m.searching();
    if (spinning_) {
        spin_ms_ += f.animations ? f.dt_ms : 0.0;
    }

    if (f.animations) {
        const double k = 1.0 - std::exp(-f.dt_ms / motion::scroll_settle_ms);
        scroll_ += (target_ - scroll_) * static_cast<float>(k);
        if (std::abs(target_ - scroll_) < 0.3F) {
            scroll_ = target_;
        }
        hscroll_ += (htarget_ - hscroll_) * static_cast<float>(k);
        if (std::abs(htarget_ - hscroll_) < 0.3F) {
            hscroll_ = htarget_;
        }
    } else {
        scroll_ = target_;
        hscroll_ = htarget_;
    }
    htarget_ = std::clamp(htarget_, 0.0F, hmax_scroll_);
    hscroll_ = std::clamp(hscroll_, 0.0F, hmax_scroll_);
    clamp_scroll();
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void BrowserView::draw(const BrowserFrame& f) {
    const Layout l = compute_layout(f);
    Renderer& r = f.painter.renderer();
    const std::size_t first_intent = f.intents.size();

    draw_top_bar(f, l);
    if (f.model.notice_shown()) {
        draw_notice(f, l);
    }

    // List panel: a quiet card on the chrome background.
    const float rad = std::round(f.painter.px(radius::lg - 2.0F));
    const float hair = std::max(1.0F, std::round(f.painter.px(1.0F)));
    r.fill_rounded_rect(l.panel, rad, f.palette.surface);
    if (l.header.h > 0.0F) {
        draw_header(f, l);
    }
    // Phase 9A: Ctrl+wheel zooms along the Details <-> Icons axis.
    if (f.pointer.wheel_y != 0.0F && has(f.pointer.mods, Modifiers::Ctrl) &&
        contains(l.panel, f.pointer.x, f.pointer.y)) {
        Intent zi;
        zi.kind = Intent::Kind::StepZoom;
        zi.position = f.pointer.wheel_y > 0.0F ? 1.0 : -1.0;
        f.intents.push_back(std::move(zi));
    }
    if (f.slots != nullptr && f.model.loaded() && !f.model.current_dir().empty() &&
        !f.model.flattened()) {
        f.slots->push_back(DropSlot{l.panel, f.model.current_dir(), SlotKind::Pane, f.tab,
                                    display_name(f.model.current_dir().filename().string())});
    }

    const view::ViewMode mode = f.model.view_mode();
    const bool grouped_details = mode == view::ViewMode::Details &&
                                 f.model.group_by() != view::GroupBy::None && !f.model.flattened();
    if (mode == view::ViewMode::Details && !grouped_details) {
        handle_list_pointer(f, l);
    }
    // Rows scroll under the header and stop one list pad above the card's bottom edge.
    r.set_clip(RectF{l.body.x + hair, l.body.y, l.body.w - (2.0F * hair),
                     std::max(l.body.h - l.pad, 0.0F)});
    if (f.model.row_count() == 0) {
        // An unreadable or truly empty folder says so, whatever filters are set.
        const BrowserModel& m = f.model;
        const bool plain = !m.loaded() || m.listing_error().has_value() || m.entries().empty();
        if (m.flattened() || (!plain && (m.filter_active() || m.type_filter().active()))) {
            draw_search_empty(f, l);
        } else {
            draw_empty_state(f, l);
        }
    } else if (mode == view::ViewMode::Icons) {
        draw_icons_view(f, l);
    } else if (mode == view::ViewMode::List) {
        draw_list_view(f, l);
    } else if (mode == view::ViewMode::Tree) {
        draw_tree_view(f, l);
    } else if (grouped_details) {
        draw_grouped_details(f, l);
    } else {
        draw_rows(f, l);
        draw_band(f, l);
    }
    draw_scrollbar(f, l);
    r.set_clip(std::nullopt);
    draw_progress(f, l);
    if (f.marked) {
        // Active pane in a split layout: an accent frame, not just a tint (design review B1).
        r.stroke_rounded_rect(l.panel, rad, hair, f.palette.accent.with_alpha(0.62F));
    } else {
        r.stroke_rounded_rect(l.panel, rad, hair, f.palette.border);
    }

    draw_status(f, l);
    draw_tooltip(f);
    if (f.pointer.released) {
        active_ = WidgetId::None;
    }
    if (f.pointer.middle_released) {
        middle_crumb_ = static_cast<std::size_t>(-1);
    }
    for (std::size_t i = first_intent; i < f.intents.size(); ++i) {
        f.intents[i].tab = f.tab;
    }
}

void BrowserView::draw_header(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const fs::SortOptions& sort = f.model.sort();
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));

    struct Column {
        std::string_view key;
        fs::SortKey sort;
        view::ColumnId col_id;
        float x0;
        float x1;
        bool right;
    };
    const float gap = std::round(p.px(kColumnGap));
    std::vector<Column> cols;
    for (view::ColumnId cid : f.model.column_order()) {
        switch (cid) {
        case view::ColumnId::Name:
            cols.push_back({"column.name", fs::SortKey::Name, cid, l.name_x, l.name_r, false});
            break;
        case view::ColumnId::Size:
            if (l.show_size) {
                cols.push_back({"column.size", fs::SortKey::Size, cid, l.size_x, l.size_r, true});
            }
            break;
        case view::ColumnId::Type:
            if (l.show_type) {
                cols.push_back(
                    {"column.type", fs::SortKey::Extension, cid, l.type_x, l.type_r, false});
            }
            break;
        case view::ColumnId::Modified:
            if (l.show_date) {
                cols.push_back(
                    {"column.modified", fs::SortKey::Modified, cid, l.date_x, l.date_r, false});
            }
            break;
        }
    }
    if (l.show_folder) {
        // Not sortable: drawn as a label only (the flattened order ties on it).
        const ShapedText label = p.shape(type::column_header, tr("column.folder"));
        p.draw(label, l.folder_x,
               std::round(l.header.y + (0.5F * l.header.h) + (0.5F * label.cap_height())),
               pal.text_secondary);
    }
    // A flattened view with a query is in relevance order until a header is clicked: no
    // column shows the sort chevron then.
    const bool relevance =
        f.model.flattened() && !f.model.query().empty() && !f.model.header_sorted();

    const float baseline_mid = l.header.y + (0.5F * l.header.h);
    for (std::size_t i = 0; i < cols.size(); ++i) {
        const Column& c = cols[i];
        // Hit area spans half the gap on each side.
        const RectF cell{c.x0 - (0.5F * gap), l.header.y + p.px(4.0F), (c.x1 - c.x0) + gap,
                         l.header.h - p.px(8.0F)};
        const bool hover = active_ == WidgetId::None && contains(cell, ptr.x, ptr.y);
        if (ptr.pressed && hover) {
            active_ = WidgetId::Header;
            active_index_ = i;
            drag_col_from_ = col_index_of(f, c.col_id);
            drag_col_to_ = drag_col_from_;
            drag_col_start_x_ = ptr.x;
            col_dragging_ = false;
        }
        const bool pressed = active_ == WidgetId::Header && active_index_ == i;
        if (pressed && ptr.down && std::abs(ptr.x - drag_col_start_x_) > p.px(6.0F)) {
            col_dragging_ = true;
        }
        if (col_dragging_ && active_ == WidgetId::Header && ptr.x >= cell.x &&
            ptr.x <= cell.right()) {
            drag_col_to_ = col_index_of(f, c.col_id);
        }
        if (pressed && ptr.released) {
            if (col_dragging_ && drag_col_from_ != drag_col_to_) {
                Intent in;
                in.kind = Intent::Kind::ReorderColumn;
                in.row = drag_col_from_;
                in.row_last = drag_col_to_;
                f.intents.push_back(in);
            } else if (!col_dragging_ && contains(cell, ptr.x, ptr.y)) {
                Intent in;
                in.kind = Intent::Kind::SortBy;
                in.sort_key = c.sort;
                f.intents.push_back(in);
            }
            col_dragging_ = false;
        }
        if (col_dragging_ && active_ == WidgetId::Header &&
            drag_col_to_ == col_index_of(f, c.col_id) && drag_col_from_ != drag_col_to_) {
            r.fill_rect(RectF{cell.x, l.header.y + p.px(4.0F), std::max(2.0F, p.px(2.0F)),
                              l.header.h - p.px(8.0F)},
                        pal.accent);
        }
        if (hover || pressed) {
            r.fill_rounded_rect(snap(cell), p.px(radius::sm),
                                pressed ? pal.control_pressed : pal.control_hover);
        }
        const bool sorted = sort.key == c.sort && !relevance;
        const ShapedText label = p.shape(type::column_header, tr(c.key));
        const float baseline = std::round(baseline_mid + (0.5F * label.cap_height()));
        const Color ink = sorted ? pal.text_primary : pal.text_secondary;
        const float chevron = p.px(7.0F);
        const float chevron_gap = p.px(6.0F);
        float lx = c.right ? c.x1 - label.width() : c.x0;
        if (c.right && sorted) {
            lx -= chevron + chevron_gap;
        }
        p.draw(label, lx, baseline, ink);
        if (sorted) {
            const float cx = c.right ? c.x1 - (0.5F * chevron)
                                     : lx + label.width() + chevron_gap + (0.5F * chevron);
            draw_chevron(r, cx, baseline_mid, chevron,
                         sort.descending ? Direction::Down : Direction::Up,
                         std::max(1.0F, p.px(1.3F)), pal.text_secondary);
        }
    }
    r.fill_rect(
        RectF{l.header.x + hair, l.header.bottom() - hair, l.header.w - (2.0F * hair), hair},
        pal.border);
}

void BrowserView::handle_list_pointer(const BrowserFrame& f, const Layout& l) {
    const PointerInput& ptr = f.pointer;
    const BrowserModel& m = f.model;
    const float sp = scroll_px(f);
    const float hair = std::max(1.0F, std::round(f.painter.px(1.0F)));
    const RectF scroll_hit{l.body.right() - f.painter.px(14.0F), l.body.y,
                           f.painter.px(14.0F) - hair, l.body.h};
    const bool scrollable = max_scroll_ > 0.0F;

    if (ptr.middle_pressed && active_ == WidgetId::None && contains(l.body, ptr.x, ptr.y) &&
        !(scrollable && contains(scroll_hit, ptr.x, ptr.y))) {
        // Middle-click on a folder row: cursor to it (selection unchanged), open in a new tab.
        const std::size_t row = row_at(ptr.y, l, f);
        if (row != kNoRow && fs::is_directory_like(m.row_entry(row))) {
            Intent in;
            in.kind = Intent::Kind::CursorTo;
            in.row = row;
            f.intents.push_back(in);
            f.queue.enqueue(CommandId::OpenInNewTab);
        }
    }
    if (f.inline_edit != nullptr && ptr.pressed && !contains(inline_rect_, ptr.x, ptr.y)) {
        // A press outside the editor commits the shown text (as GNOME Files).
        Intent in;
        in.kind = Intent::Kind::InlineCommit;
        f.intents.push_back(in);
    }
    if (f.inline_edit != nullptr && ptr.pressed && contains(inline_rect_, ptr.x, ptr.y)) {
        return; // a click inside the editor only keeps it
    }
    if (ptr.pressed && active_ == WidgetId::None && contains(l.body, ptr.x, ptr.y)) {
        drag_row_ = kNoRow;
        deferred_click_ = false;
        drag_started_ = false;
        if (scrollable && contains(scroll_hit, ptr.x, ptr.y)) {
            // Thumb grab or a jump on the track.
            const float scale = f.painter.scale();
            const float content_px = (max_scroll_ * scale) + l.body.h;
            const float thumb_h = std::max(f.painter.px(32.0F), l.body.h * (l.body.h / content_px));
            const float travel = std::max(l.body.h - thumb_h, 1.0F);
            const float thumb_y = l.body.y + (travel * (scroll_ / max_scroll_));
            active_ = WidgetId::Scrollbar;
            drag_offset_ =
                (ptr.y >= thumb_y && ptr.y < thumb_y + thumb_h) ? ptr.y - thumb_y : 0.5F * thumb_h;
            return;
        }
        const std::size_t row = row_at(ptr.y, l, f);
        press_x_ = ptr.x;
        press_y_ = ptr.y - l.body.y + sp;
        press_mods_ = ptr.mods;
        press_row_ = row;
        band_started_ = false;
        if (row != kNoRow && ptr.clicks >= 2) {
            // Double-click: the click lands first so Activate sees the row as cursor.
            // Ctrl+double-click is two Ctrl+clicks (the selection ends where it started), then
            // list.open_in_new_tab (docs/keymap.md, "Opening in a new tab").
            const bool ctrl = has(ptr.mods, Modifiers::Ctrl) && !has(ptr.mods, Modifiers::Shift);
            Intent in;
            in.kind = Intent::Kind::ClickRow;
            in.row = row;
            in.mods = ctrl ? Modifiers::Ctrl : Modifiers::None;
            f.intents.push_back(in);
            f.queue.enqueue(ctrl ? CommandId::OpenInNewTab : CommandId::Activate);
            active_ = WidgetId::Rows;
            press_row_ = kNoRow;
            return;
        }
        if (row == kNoRow && ptr.clicks >= 2 && !has(ptr.mods, Modifiers::Ctrl)) {
            f.queue.enqueue(CommandId::NavigateUp); // double-click on empty background
            active_ = WidgetId::Rows;
            press_row_ = kNoRow;
            return;
        }
        bool on_name = false;
        if (row != kNoRow) {
            const ShapedText name = f.painter.shape(type::row, display_name(m.row_entry(row).name));
            const float name_end =
                std::min(l.text_x + name.width(), l.name_r) + f.painter.px(kGlyphGap);
            on_name = ptr.x >= l.name_x - f.painter.px(4.0F) && ptr.x <= name_end;
        }
        if (on_name) {
            const bool plain = !has(ptr.mods, Modifiers::Ctrl) && !has(ptr.mods, Modifiers::Shift);
            if (plain && f.drag_enabled && m.is_selected(row) && m.selected_count() > 1) {
                // A drag takes the whole selection; a plain click selects only this row, on
                // release (nothing changes at the press).
                drag_row_ = row;
                deferred_click_ = true;
            } else {
                Intent in;
                in.kind = Intent::Kind::ClickRow;
                in.row = row;
                in.mods = ptr.mods;
                f.intents.push_back(in);
                drag_row_ = plain && f.drag_enabled ? row : kNoRow;
            }
            active_ = WidgetId::Rows;
            press_row_ = kNoRow;
        } else {
            active_ = WidgetId::Band; // becomes a band once the pointer moves
        }
        if (active_ != WidgetId::Band) {
            return;
        }
    }

    if (active_ == WidgetId::Rows && drag_row_ != kNoRow) {
        const float rdx = ptr.x - press_x_;
        const float rdy = (ptr.y - l.body.y + sp) - press_y_;
        const float thr = f.painter.px(kDragThreshold);
        if (ptr.down && !drag_started_ && ((rdx * rdx) + (rdy * rdy)) >= thr * thr &&
            f.inline_edit == nullptr) {
            drag_started_ = true;
            deferred_click_ = false;
            Intent in;
            in.kind = Intent::Kind::DragBegin;
            in.row = drag_row_;
            f.intents.push_back(in);
        }
        if (ptr.released) {
            if (!drag_started_ && deferred_click_) {
                Intent in;
                in.kind = Intent::Kind::ClickRow;
                in.row = drag_row_;
                in.mods = Modifiers::None;
                f.intents.push_back(in);
            }
            drag_row_ = kNoRow;
            deferred_click_ = false;
            drag_started_ = false;
        }
        return;
    }
    if (active_ != WidgetId::Band) {
        return;
    }
    const float dx = ptr.x - press_x_;
    const float dy = (ptr.y - l.body.y + sp) - press_y_;
    const float threshold = f.painter.px(kDragThreshold);
    if (!band_started_ && ptr.down && ((dx * dx) + (dy * dy)) >= threshold * threshold) {
        band_started_ = true;
        last_band_first_ = 1;
        last_band_last_ = 0;
        Intent in;
        in.kind = Intent::Kind::BandBegin;
        in.additive = has(press_mods_, Modifiers::Ctrl);
        f.intents.push_back(in);
    }
    if (band_started_) {
        band_cx_ = ptr.x;
        band_cy_ = std::clamp(ptr.y, l.body.y, l.body.bottom()) - l.body.y + sp;
        const float y0 = std::min(press_y_, band_cy_) - l.pad;
        const float y1 = std::max(press_y_, band_cy_) - l.pad;
        const std::size_t n = m.row_count();
        std::size_t first = 1;
        std::size_t last = 0;
        if (n > 0 && y1 >= 0.0F && y0 < static_cast<float>(n) * l.row_h) {
            first = static_cast<std::size_t>(std::max(y0, 0.0F) / l.row_h);
            last = std::min(static_cast<std::size_t>(y1 / l.row_h), n - 1);
        }
        if (first != last_band_first_ || last != last_band_last_) {
            last_band_first_ = first;
            last_band_last_ = last;
            Intent in;
            in.kind = Intent::Kind::BandUpdate;
            in.row = first;
            in.row_last = last;
            f.intents.push_back(in);
        }
    }
    if (ptr.released) {
        Intent in;
        if (band_started_) {
            in.kind = Intent::Kind::BandEnd;
        } else if (press_row_ != kNoRow) {
            in.kind = Intent::Kind::ClickRow;
            in.row = press_row_;
            in.mods = press_mods_;
        } else if (!has(press_mods_, Modifiers::Ctrl)) {
            in.kind = Intent::Kind::ClearSelection;
        } else {
            return;
        }
        f.intents.push_back(in);
        band_started_ = false;
    }
}

void BrowserView::draw_rows(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const std::size_t n = m.row_count();
    const float sp = scroll_px(f);
    if (l.row_h <= 0.0F || n == 0) {
        return;
    }

    // Visible window only.
    const float top = sp - l.pad;
    const std::size_t first = top <= 0.0F ? 0 : static_cast<std::size_t>(top / l.row_h);
    const std::size_t last = std::min(n - 1, static_cast<std::size_t>((top + l.body.h) / l.row_h));
    if (first > last || first >= n) {
        return;
    }
    const auto row_y = [&](std::size_t i) {
        return l.body.y + l.pad + (static_cast<float>(i) * l.row_h) - sp;
    };
    const float inset = std::round(p.px(metrics::row_inset));
    const float rx = l.panel.x + inset;
    const float rw = l.panel.w - (2.0F * inset);
    const float rad = std::round(p.px(6.0F));

    // Hovered row (not while dragging a band or the scrollbar).
    std::size_t hover = hover_override_;
    const PointerInput& ptr = f.pointer;
    if (hover == kNoRow && ptr.inside && (active_ == WidgetId::None || active_ == WidgetId::Rows) &&
        contains(l.body, ptr.x, ptr.y) && ptr.x < l.body.right() - p.px(14.0F)) {
        hover = row_at(ptr.y, l, f);
    }

    // 1. Alternating stripes and hover.
    for (std::size_t i = first; i <= last; ++i) {
        const RectF row = snap(RectF{rx, row_y(i), rw, l.row_h});
        if (f.alternating_rows && i % 2 == 1) {
            r.fill_rounded_rect(row, rad, pal.row_alt);
        }
        if (i == hover && !m.is_selected(i)) {
            r.fill_rounded_rect(row, rad, pal.row_hover);
        }
    }

    // 2. Selection: one rounded shape per run of consecutive selected rows, opaque so runs that
    //    continue past the viewport do not double-blend.
    const Color sel = over(pal.surface, f.focused ? pal.selection : pal.selection_inactive);
    const Color marker = f.focused ? pal.selection_marker : pal.text_muted;
    for (std::size_t i = first; i <= last; ++i) {
        if (!m.is_selected(i) || (i > first && m.is_selected(i - 1))) {
            continue;
        }
        std::size_t j = i;
        while (j + 1 < n && m.is_selected(j + 1) && j + 1 <= last + 1) {
            ++j;
        }
        // Extend past the clip when the run continues outside the viewport.
        const float y0 =
            (i == first && i > 0 && m.is_selected(i - 1)) ? row_y(i) - l.row_h : row_y(i);
        const float y1 = row_y(j) + l.row_h;
        const RectF run = snap(RectF{rx, y0, rw, y1 - y0});
        r.fill_rounded_rect(run, rad, sel);
        const float mw = std::max(2.0F, std::round(p.px(3.0F)));
        const float my = p.px(6.0F);
        r.fill_rounded_rect(snap(RectF{rx + p.px(3.0F), run.y + my, mw, run.h - (2.0F * my)}),
                            0.5F * mw, marker);
        i = j;
    }

    // 3. Cursor ring.
    const std::size_t cursor = m.cursor();
    if (cursor != kNoRow && cursor >= first && cursor <= last && f.focused) {
        const RectF row = snap(RectF{rx, row_y(cursor), rw, l.row_h});
        const float t = std::max(1.0F, std::round(p.px(1.0F)));
        const Color ring =
            m.is_selected(cursor) ? pal.focus_ring.with_alpha(0.55F) : pal.focus_ring;
        r.stroke_rounded_rect(row, rad, t, ring);
    }

    // 4. Content: glyph, name, size, type, date.
    const float glyph = std::round(p.px(metrics::icon_size));
    float unit_slot = 0.0F;
    for (const std::string_view key :
         {"size.unit.b", "size.unit.kb", "size.unit.mb", "size.unit.gb", "size.unit.tb"}) {
        unit_slot = std::max(unit_slot, p.shape(type::row_numeric, tr(key)).width());
    }
    unit_slot = std::ceil(unit_slot);
    const float unit_gap = std::round(p.px(3.5F));
    const bool highlight = !m.query().empty();
    const float parent_gap = std::round(p.px(8.0F));
    const bool cut_any = f.file_ops != nullptr && f.file_ops->has_cut();
    bool editor_drawn = false;
    for (std::size_t i = first; i <= last; ++i) {
        const fs::DirEntry& e = m.row_entry(i);
        const float y = row_y(i);
        const float mid = std::round(y + (0.5F * l.row_h));
        const bool dir = fs::is_directory_like(e);
        const bool is_cut = cut_any && f.file_ops->is_cut(m.row_path(i));
        const float dim = (e.hidden ? 0.6F : 1.0F) * (is_cut ? 0.5F : 1.0F);
        if (dir && f.slots != nullptr) {
            const RectF hit = intersect(RectF{rx, y, rw, l.row_h}, l.body);
            if (hit.h > 0.0F) {
                f.slots->push_back(
                    DropSlot{hit, m.row_path(i), SlotKind::Row, f.tab, display_name(e.name)});
            }
        }
        // Phase 6: an image / video thumbnail replaces the glyph once it has landed; it sits in
        // a box 2dp larger than the glyph, centred on it, so the name column does not move.
        const float thumb = std::round(p.px(metrics::icon_size + 2.0F));
        const RectF thumb_box{std::round(l.name_x + (0.5F * glyph) - (0.5F * thumb)),
                              std::round(mid - (0.5F * thumb)), thumb, thumb};
        if (f.thumbnails == nullptr ||
            !f.thumbnails->draw_row(r, pal, m, i, thumb_box, p.scale(), dim)) {
            draw_entry_glyph(r, pal, e, l.name_x, std::round(mid - (0.5F * glyph)), glyph, dim,
                             p.scale());
        }

        const std::string name = display_name(e.name);
        // Narrow flattened panes: the parent path follows the name, muted, when there is room.
        float name_room = l.name_r - l.text_x;
        std::string parent;
        if (l.inline_parent) {
            parent = folder_text(f, i);
            const float natural = p.shape(type::row, name).width();
            name_room = std::min(name_room, std::max(natural, name_room * 0.6F));
        }
        const TextFitter::Fitted fitted =
            fitter_.fit_text(p, type::row, name, name_room, dir ? Elide::End : Elide::Middle);
        const ShapedText& name_text = fitted.shaped;
        const float baseline = std::round(mid + (0.5F * name_text.cap_height()));
        const Color name_ink =
            (e.hidden || is_cut) && !m.is_selected(i) ? pal.text_secondary : pal.text_primary;
        const bool editing =
            f.inline_edit != nullptr && !m.flattened() && e.name == f.inline_edit->original;
        if (editing) {
            // The inline editor replaces the name: a raised field with an accent ring (red when
            // the name is not allowed), the stem selected.
            const float pad_px = std::round(p.px(6.0F));
            const float natural = p.shape(type::row, f.inline_edit->edit.text()).width();
            const float room = l.name_r - l.text_x;
            const float w = std::min(room + pad_px, std::max(natural + p.px(48.0F), p.px(200.0F)));
            const RectF box =
                snap(RectF{l.text_x - pad_px, y + p.px(1.0F), w + pad_px, l.row_h - p.px(2.0F)});
            const float brad = std::round(p.px(5.0F));
            const bool bad = f.inline_edit->message_error;
            r.fill_rounded_rect(box, brad, pal.surface_raised);
            r.stroke_rounded_rect(box, brad, std::max(1.0F, std::round(p.px(1.5F))),
                                  bad ? pal.danger : pal.focus_ring);
            TextFieldStyle st;
            st.text = &type::row;
            st.caret_height_dp = 15.0F;
            (void)draw_text_field(p, pal, f.inline_edit->edit,
                                  RectF{box.x + pad_px, box.y, box.w - (2.0F * pad_px), box.h}, {},
                                  inline_scroll_, st);
            inline_rect_ = box;
            inline_message_ = f.inline_edit->message;
            inline_message_error_ = bad;
            inline_message_anchor_ = box;
            editor_drawn = true;
        } else if (highlight) {
            const std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges =
                name_highlights(f, e.name, name, fitted.shown);
            p.draw_highlighted(name_text, l.text_x, baseline, name_ink, pal.match,
                               pal.match.with_alpha(0.55F), ranges,
                               fitted.shown.empty() ? name.size() : fitted.shown.size());
        } else {
            p.draw(name_text, l.text_x, baseline, name_ink);
        }
        if (l.inline_parent && !parent.empty()) {
            const float px0 = l.text_x + name_text.width() + parent_gap;
            const float room = l.name_r - px0;
            // Never a stub like "R…ch": the whole path, or at least 72dp of it.
            if (room >= std::min(p.px(72.0F), p.shape(type::row, parent).width())) {
                const ShapedText pt = fitter_.fit(p, type::row, parent, room, Elide::Middle);
                p.draw(pt, px0, baseline, pal.text_muted);
            }
        }
        if (l.show_folder) {
            const ShapedText ft = fitter_.fit(p, type::row, folder_text(f, i),
                                              l.folder_r - l.folder_x, Elide::Middle);
            p.draw(ft, l.folder_x, baseline, e.hidden ? pal.text_muted : pal.text_secondary);
        }

        if (l.show_size && !dir && !e.metadata_error) {
            // Numbers right-aligned in one slot, units left-aligned in the next, so "980 B"
            // and "88.1 kB" line up on the digits.
            const SizeParts parts = format_size_parts(e.size);
            const ShapedText number = p.shape(type::row_numeric, parts.number);
            const ShapedText unit = p.shape(type::row_numeric, parts.unit);
            const float unit_x = l.size_r - unit_slot;
            p.draw(number, unit_x - unit_gap - number.width(), baseline, pal.text_secondary);
            p.draw(unit, unit_x, baseline, pal.text_muted);
        }
        if (l.show_type) {
            const ShapedText kind =
                fitter_.fit(p, type::row, type_label(e), l.type_r - l.type_x, Elide::End);
            p.draw(kind, l.type_x, baseline, pal.text_secondary);
        }
        const std::string date = !l.show_date   ? std::string()
                                 : l.short_date ? format_date_short(e.mtime, f.now)
                                                : format_date(e.mtime, f.now);
        if (!date.empty()) {
            const ShapedText when =
                fitter_.fit(p, type::row_numeric, date, l.date_r - l.date_x, Elide::End);
            p.draw(when, l.date_x, baseline, pal.text_secondary);
        }
    }
    if (!editor_drawn) {
        inline_rect_ = RectF{};
        inline_message_.clear();
    } else if (!inline_message_.empty()) {
        // The validation message hangs under the editor.
        const ShapedText msg = p.shape(type::caption, inline_message_);
        const float padx = std::round(p.px(8.0F));
        const float w = std::min(msg.width() + (2.0F * padx), l.panel.w - p.px(16.0F));
        const float h = std::round(p.px(22.0F));
        float by = inline_message_anchor_.bottom() + p.px(3.0F);
        if (by + h > l.body.bottom()) {
            by = inline_message_anchor_.y - h - p.px(3.0F);
        }
        const RectF bubble = snap(RectF{inline_message_anchor_.x, by, w, h});
        const float brad = std::round(p.px(5.0F));
        r.drop_shadow(bubble, brad, p.px(10.0F), p.px(2.0F), pal.shadow);
        r.fill_rounded_rect(bubble, brad,
                            inline_message_error_ ? pal.danger_surface : pal.surface_raised);
        r.stroke_rounded_rect(bubble, brad, std::max(1.0F, std::round(p.px(1.0F))),
                              inline_message_error_ ? pal.danger_border : pal.border);
        const ShapedText fitted =
            fitter_.fit(p, type::caption, inline_message_, w - (2.0F * padx), Elide::End);
        p.draw(fitted, bubble.x + padx,
               std::round(bubble.center_y() + (0.5F * fitted.cap_height())),
               inline_message_error_ ? pal.text_primary : pal.text_secondary);
    }
    // Phase 6: thumbnails of the rows just off screen, so scrolling a little shows them at once.
    if (f.thumbnails != nullptr) {
        const float thumb = std::round(p.px(metrics::icon_size + 2.0F));
        constexpr std::size_t kAhead = 8;
        for (std::size_t i = last + 1; i < n && i <= last + kAhead; ++i) {
            f.thumbnails->prefetch_row(m, i, thumb);
        }
        for (std::size_t k = 1; k <= kAhead && k <= first; ++k) {
            f.thumbnails->prefetch_row(m, first - k, thumb);
        }
    }
}

void BrowserView::draw_band(const BrowserFrame& f, const Layout& l) {
    if (active_ != WidgetId::Band || !band_started_) {
        return;
    }
    const float sp = scroll_px(f);
    const float x0 = std::min(press_x_, band_cx_);
    const float x1 = std::max(press_x_, band_cx_);
    const float y0 = std::min(press_y_, band_cy_) - sp + l.body.y;
    const float y1 = std::max(press_y_, band_cy_) - sp + l.body.y;
    const RectF band = snap(RectF{x0, y0, x1 - x0, y1 - y0});
    Renderer& r = f.painter.renderer();
    const Color accent = f.palette.selection_marker;
    r.fill_rect(band, accent.with_alpha(0.12F));
    r.stroke_rounded_rect(band, 0.0F, std::max(1.0F, std::round(f.painter.px(1.0F))),
                          accent.with_alpha(0.7F));
}

void BrowserView::draw_scrollbar(const BrowserFrame& f, const Layout& l) {
    if (max_scroll_ <= 0.0F) {
        return;
    }
    const Painter& p = f.painter;
    const float scale = p.scale();
    const float content_px = (max_scroll_ * scale) + l.body.h;
    const float margin = p.px(3.0F);
    const float track_h = l.body.h - (2.0F * margin);
    const float thumb_h = std::max(p.px(32.0F), track_h * (l.body.h / content_px));
    const float travel = std::max(track_h - thumb_h, 0.0F);
    const float t = std::clamp(scroll_ / max_scroll_, 0.0F, 1.0F);
    const PointerInput& ptr = f.pointer;
    const RectF hit{l.body.right() - p.px(14.0F), l.body.y, p.px(14.0F), l.body.h};
    const bool hot = active_ == WidgetId::Scrollbar ||
                     (active_ == WidgetId::None && contains(hit, ptr.x, ptr.y));
    const float w = p.px(hot ? metrics::scrollbar_width_hover : metrics::scrollbar_width);
    const RectF thumb =
        snap(RectF{l.body.right() - margin - w, l.body.y + margin + (travel * t), w, thumb_h});
    p.renderer().fill_rounded_rect(thumb, 0.5F * thumb.w,
                                   hot ? f.palette.scrollbar_hover : f.palette.scrollbar);
}

} // namespace navigator::ui

// src/ui/browser/browser_view.cpp — layout, scrolling and the virtualized Details list.
// Chrome (top bar, notice, status bar, empty states, overlay) is in browser_chrome.cpp.
#include "browser/browser_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "widgets/icons.hpp"

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
constexpr float kTextPad = 10.0F;  // row highlight edge -> content
constexpr float kGlyphGap = 10.0F; // glyph -> name text
constexpr float kListPad = 4.0F;   // above the first row

} // namespace

// ---------------------------------------------------------------------------------------------
// Layout and scrolling
// ---------------------------------------------------------------------------------------------

BrowserView::Layout BrowserView::compute_layout(const BrowserFrame& f) const {
    const Painter& p = f.painter;
    const DensityMetrics dm = density_metrics(f.density);
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
    l.header = RectF{l.panel.x, l.panel.y, l.panel.w, std::round(p.px(dm.header_height))};
    l.body = RectF{l.panel.x, l.header.bottom(), l.panel.w,
                   std::max(l.panel.bottom() - l.header.bottom(), 0.0F)};
    l.pad = std::round(p.px(kListPad));
    l.row_h = std::round(p.px(dm.row_height));

    const float inset = p.px(metrics::row_inset);
    const float left = std::round(l.panel.x + inset + p.px(kTextPad));
    const float right = std::round(l.panel.right() - inset - p.px(kTextPad));
    const float gap = std::round(p.px(kColumnGap));
    l.name_x = left;
    l.text_x = std::round(left + p.px(metrics::icon_size + kGlyphGap));

    // Columns degrade gracefully in narrow panes (plan_columns): Type goes first, then
    // Modified shortens, then Size goes; Modified is the last to go. Name always stays.
    const ColumnPlan plan = plan_columns((right - l.text_x) / p.scale());
    l.show_size = plan.size;
    l.show_type = plan.type;
    l.show_date = plan.date;
    l.short_date = plan.short_date;

    float edge = right;
    if (l.show_date) {
        l.date_r = edge;
        l.date_x = std::round(edge - p.px(l.short_date ? kShortDateWidthDp : kDateWidth));
        edge = l.date_x - gap;
    } else {
        l.date_x = l.date_r = edge;
    }
    if (l.show_type) {
        l.type_r = edge;
        l.type_x = std::round(edge - p.px(kTypeWidth));
        edge = l.type_x - gap;
    } else {
        l.type_x = l.type_r = edge;
    }
    if (l.show_size) {
        l.size_r = edge;
        l.size_x = std::round(l.size_r - p.px(kSizeWidth));
        edge = l.size_x - gap;
    } else {
        l.size_x = l.size_r = edge + gap;
    }
    l.name_r = std::max(edge, l.text_x + p.px(40.0F));
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
    return row < f.model.entries().size() ? row : kNoRow;
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
    const auto n = static_cast<float>(m.entries().size());
    const float content_dp = (2.0F * l.pad / scale) + (n * (l.row_h / scale));
    max_scroll_ = std::max(0.0F, content_dp - (l.body.h / scale));
    page_rows_ =
        l.row_h > 0.0F ? std::max<std::size_t>(1, static_cast<std::size_t>(l.body.h / l.row_h)) : 1;

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
    if (ptr.wheel_y != 0.0F && contains(l.panel, ptr.x, ptr.y)) {
        target_ -= ptr.wheel_y * kWheelRows * density_metrics(f.density).row_height;
        target_ = std::clamp(target_, 0.0F, max_scroll_);
        if (!f.animations) {
            scroll_ = target_;
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
    // Scrollbar drag.
    if (active_ == WidgetId::Scrollbar && ptr.down && max_scroll_ > 0.0F) {
        const float content_px = content_dp * scale;
        const float thumb_h = std::max(f.painter.px(32.0F), l.body.h * (l.body.h / content_px));
        const float travel = std::max(l.body.h - thumb_h, 1.0F);
        const float t = std::clamp((ptr.y - drag_offset_ - l.body.y) / travel, 0.0F, 1.0F);
        target_ = scroll_ = t * max_scroll_;
    }

    if (f.animations) {
        const double k = 1.0 - std::exp(-f.dt_ms / motion::scroll_settle_ms);
        scroll_ += (target_ - scroll_) * static_cast<float>(k);
        if (std::abs(target_ - scroll_) < 0.3F) {
            scroll_ = target_;
        }
    } else {
        scroll_ = target_;
    }
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
    draw_header(f, l);

    handle_list_pointer(f, l);
    // Rows scroll under the header and stop one list pad above the card's bottom edge.
    r.set_clip(RectF{l.body.x + hair, l.body.y, l.body.w - (2.0F * hair),
                     std::max(l.body.h - l.pad, 0.0F)});
    if (f.model.entries().empty()) {
        draw_empty_state(f, l);
    } else {
        draw_rows(f, l);
        draw_band(f, l);
    }
    draw_scrollbar(f, l);
    r.set_clip(std::nullopt);
    if (f.marked) {
        // Active pane in a split layout: an accent frame, not just a tint (design review B1).
        r.stroke_rounded_rect(l.panel, rad, hair, f.palette.accent.with_alpha(0.62F));
    } else {
        r.stroke_rounded_rect(l.panel, rad, hair, f.palette.border);
    }

    draw_status(f, l);
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
        float x0;
        float x1;
        bool right;
    };
    const float gap = std::round(p.px(kColumnGap));
    std::vector<Column> cols{
        {"column.name", fs::SortKey::Name, l.name_x, l.name_r, false},
    };
    if (l.show_size) {
        cols.push_back({"column.size", fs::SortKey::Size, l.size_x, l.size_r, true});
    }
    if (l.show_type) {
        cols.push_back({"column.type", fs::SortKey::Extension, l.type_x, l.type_r, false});
    }
    if (l.show_date) {
        cols.push_back({"column.modified", fs::SortKey::Modified, l.date_x, l.date_r, false});
    }

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
        }
        const bool pressed = active_ == WidgetId::Header && active_index_ == i;
        if (pressed && ptr.released && contains(cell, ptr.x, ptr.y)) {
            Intent in;
            in.kind = Intent::Kind::SortBy;
            in.sort_key = c.sort;
            f.intents.push_back(in);
        }
        if (hover || pressed) {
            r.fill_rounded_rect(snap(cell), p.px(radius::sm),
                                pressed ? pal.control_pressed : pal.control_hover);
        }
        const bool sorted = sort.key == c.sort;
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
        if (row != kNoRow && fs::is_directory_like(m.entries()[row])) {
            Intent in;
            in.kind = Intent::Kind::CursorTo;
            in.row = row;
            f.intents.push_back(in);
            f.queue.enqueue(CommandId::OpenInNewTab);
        }
    }
    if (ptr.pressed && active_ == WidgetId::None && contains(l.body, ptr.x, ptr.y)) {
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
            const ShapedText name = f.painter.shape(type::row, display_name(m.entries()[row].name));
            const float name_end =
                std::min(l.text_x + name.width(), l.name_r) + f.painter.px(kGlyphGap);
            on_name = ptr.x >= l.name_x - f.painter.px(4.0F) && ptr.x <= name_end;
        }
        if (on_name) {
            Intent in;
            in.kind = Intent::Kind::ClickRow;
            in.row = row;
            in.mods = ptr.mods;
            f.intents.push_back(in);
            active_ = WidgetId::Rows;
            press_row_ = kNoRow;
        } else {
            active_ = WidgetId::Band; // becomes a band once the pointer moves
        }
        if (active_ != WidgetId::Band) {
            return;
        }
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
        const std::size_t n = m.entries().size();
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
    const auto& entries = m.entries();
    const std::size_t n = entries.size();
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
        if (i % 2 == 1) {
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
    for (std::size_t i = first; i <= last; ++i) {
        const fs::DirEntry& e = entries[i];
        const float y = row_y(i);
        const float mid = std::round(y + (0.5F * l.row_h));
        const bool dir = fs::is_directory_like(e);
        const float dim = e.hidden ? 0.6F : 1.0F;
        draw_entry_glyph(r, pal, e, l.name_x, std::round(mid - (0.5F * glyph)), glyph, dim,
                         p.scale());

        const std::string name = display_name(e.name);
        const ShapedText name_text =
            fitter_.fit(p, type::row, name, l.name_r - l.text_x, dir ? Elide::End : Elide::Middle);
        const float baseline = std::round(mid + (0.5F * name_text.cap_height()));
        const Color name_ink =
            e.hidden && !m.is_selected(i) ? pal.text_secondary : pal.text_primary;
        p.draw(name_text, l.text_x, baseline, name_ink);

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

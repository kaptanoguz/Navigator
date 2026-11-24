// src/ui/browser/browser_modes_view.cpp — Phase 9A view modes (Icon Grid with live zoom,
// List/Columns, virtualized Tree, collapsible Group By), footer zoom slider + mode pills,
// and address-bar View Lock button.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include <navigator/i18n.hpp>

#include "browser/browser_view.hpp"
#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "widgets/icons.hpp"

namespace navigator::ui {
namespace {

constexpr float kGroupHeaderH = 26.0F; // dp
constexpr float kListColW = 220.0F;    // dp
constexpr float kTreeIndent = 18.0F;   // dp per depth level
constexpr float kGridGap = 8.0F;       // dp

void draw_lock_glyph(Renderer& r, float cx, float cy, float size, view::ViewScope scope,
                     const Color& ink, const Color& bg) {
    const float w = std::round(size * 0.68F);
    const float h = std::round(size * 0.48F);
    const float bx = std::round(cx - (0.5F * w));
    const float by = std::round(cy + (0.04F * size));
    const float rad = std::max(1.0F, std::round(size * 0.12F));
    const float stroke = std::max(1.0F, std::round(size * 0.11F));

    // Shackle (open slightly when Global).
    const float sw = std::round(w * 0.62F);
    const float sh = std::round(size * 0.42F);
    const float sx =
        std::round(cx - (0.5F * sw) + (scope == view::ViewScope::Global ? stroke : 0.0F));
    const float sy = std::round(by - sh + (0.14F * size));
    r.stroke_rounded_rect(RectF{sx, sy, sw, sh}, sw * 0.45F, stroke, ink);

    // Body.
    const RectF body{bx, by, w, h};
    if (scope == view::ViewScope::Global) {
        r.fill_rounded_rect(body, rad, bg);
        r.stroke_rounded_rect(body, rad, stroke, ink);
    } else {
        r.fill_rounded_rect(body, rad, ink);
        if (scope == view::ViewScope::Hierarchy) {
            // Small horizontal bar inside the body to signify Hierarchy (folder + subtree).
            const float bw = std::round(w * 0.50F);
            const float bh = std::max(1.5F, std::round(stroke * 0.9F));
            r.fill_rect(RectF{cx - (0.5F * bw), by + (0.5F * (h - bh)), bw, bh}, bg);
        }
    }
}

void draw_mode_glyph(Renderer& r, float cx, float cy, float s, view::ViewMode mode,
                     const Color& ink) {
    const float x0 = std::round(cx - (0.5F * s));
    const float y0 = std::round(cy - (0.5F * s));
    const float bar = std::max(1.5F, std::round(s * 0.16F));
    switch (mode) {
    case view::ViewMode::Details:
        for (int i = 0; i < 3; ++i) {
            const float y = y0 + (static_cast<float>(i) * (s * 0.36F));
            r.fill_rect(RectF{x0, std::round(y), s, bar}, ink);
        }
        break;
    case view::ViewMode::Icons: {
        const float cell = std::round(s * 0.40F);
        const float gap = std::round(s * 0.20F);
        for (int gy = 0; gy < 2; ++gy) {
            for (int gx = 0; gx < 2; ++gx) {
                r.fill_rounded_rect(RectF{x0 + (static_cast<float>(gx) * (cell + gap)),
                                          y0 + (static_cast<float>(gy) * (cell + gap)), cell, cell},
                                    1.0F, ink);
            }
        }
        break;
    }
    case view::ViewMode::List: {
        const float half = std::round(s * 0.42F);
        for (int i = 0; i < 3; ++i) {
            const float y = std::round(y0 + (static_cast<float>(i) * (s * 0.36F)));
            r.fill_rect(RectF{x0, y, half, bar}, ink);
            r.fill_rect(RectF{x0 + s - half, y, half, bar}, ink);
        }
        break;
    }
    case view::ViewMode::Tree: {
        const float indent = std::round(s * 0.28F);
        r.fill_rect(RectF{x0, y0, s, bar}, ink);
        r.fill_rect(RectF{x0 + indent, std::round(y0 + (s * 0.36F)), s - indent, bar}, ink);
        r.fill_rect(RectF{x0 + indent, std::round(y0 + (s * 0.72F)), s - indent, bar}, ink);
        break;
    }
    case view::ViewMode::Columns: {
        const float col_w = std::max(1.5F, std::round(s * 0.25F));
        const float gap = std::round((s - (3.0F * col_w)) * 0.5F);
        for (int c = 0; c < 3; ++c) {
            r.fill_rect(RectF{x0 + (static_cast<float>(c) * (col_w + gap)), y0, col_w, s}, ink);
        }
        break;
    }
    }
}

void draw_grid_tile_metrics(const BrowserFrame& f, float body_w, float& icon_px, float& tile_w,
                            float& tile_h, float& gap_px, std::size_t& cols) {
    const Painter& p = f.painter;
    const float zoom = static_cast<float>(f.model.zoom_percent()) / 100.0F;
    icon_px = std::round(p.px(48.0F * zoom));
    tile_w = std::round(icon_px + p.px(36.0F));
    tile_h = std::round(icon_px + p.px(42.0F));
    gap_px = std::round(p.px(kGridGap));
    const float pad = std::round(p.px(8.0F));
    const float avail = std::max(body_w - (2.0F * pad), tile_w);
    cols = std::max<std::size_t>(1, static_cast<std::size_t>((avail + gap_px) / (tile_w + gap_px)));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Grouped display cache
// ---------------------------------------------------------------------------------------------

void BrowserView::ensure_grouped_cache(const BrowserFrame& f) {
    const BrowserModel& m = f.model;
    const view::GroupBy gb = m.flattened() ? view::GroupBy::None : m.group_by();
    if (grouped_serial_ == m.content_serial() && grouped_by_ == gb &&
        grouped_rows_n_ == m.row_count()) {
        return;
    }
    auto entry_fn = [](const void* ctx, std::size_t row) -> const fs::DirEntry* {
        const auto* model = static_cast<const BrowserModel*>(ctx);
        if (row >= model->row_count()) {
            return nullptr;
        }
        return &model->row_entry(row);
    };
    grouped_cache_ =
        build_grouped_rows(m.row_count(), entry_fn, &m, gb, f.now, m.collapsed_groups());
    grouped_serial_ = m.content_serial();
    grouped_by_ = gb;
    grouped_rows_n_ = m.row_count();
}

std::size_t BrowserView::col_index_of(const BrowserFrame& f, view::ColumnId col) const noexcept {
    const auto& order = f.model.column_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == col) {
            return i;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Content extent per view mode (dp)
// ---------------------------------------------------------------------------------------------

float BrowserView::compute_mode_content_dp(const BrowserFrame& f, const Layout& l) const {
    const BrowserModel& m = f.model;
    const float scale = f.painter.scale();
    const float pad_dp = l.pad / scale;
    const float row_dp = l.row_h / scale;

    switch (m.view_mode()) {
    case view::ViewMode::Details: {
        if (m.group_by() != view::GroupBy::None && !m.flattened() &&
            !grouped_cache_.items.empty()) {
            float total = 2.0F * pad_dp;
            for (const GroupDisplayItem& it : grouped_cache_.items) {
                total += (it.kind == GroupDisplayItem::Kind::Header) ? kGroupHeaderH : row_dp;
            }
            return total;
        }
        return (2.0F * pad_dp) + (static_cast<float>(m.row_count()) * row_dp);
    }
    case view::ViewMode::Icons: {
        float icon_px = 0.0F;
        float tile_w = 0.0F;
        float tile_h = 0.0F;
        float gap_px = 0.0F;
        std::size_t cols = 1;
        draw_grid_tile_metrics(f, l.body.w, icon_px, tile_w, tile_h, gap_px, cols);
        const float tile_h_dp = (tile_h + gap_px) / scale;
        if (m.group_by() != view::GroupBy::None && !m.flattened() &&
            !grouped_cache_.items.empty()) {
            float total = 2.0F * pad_dp;
            std::size_t run = 0;
            for (const GroupDisplayItem& it : grouped_cache_.items) {
                if (it.kind == GroupDisplayItem::Kind::Header) {
                    if (run > 0) {
                        const std::size_t rows = (run + cols - 1) / cols;
                        total += static_cast<float>(rows) * tile_h_dp;
                        run = 0;
                    }
                    total += kGroupHeaderH;
                } else {
                    ++run;
                }
            }
            if (run > 0) {
                const std::size_t rows = (run + cols - 1) / cols;
                total += static_cast<float>(rows) * tile_h_dp;
            }
            return total;
        }
        const std::size_t grid_rows = (m.row_count() + cols - 1) / cols;
        return (2.0F * pad_dp) + (static_cast<float>(grid_rows) * tile_h_dp);
    }
    case view::ViewMode::List:
        return l.body.h / scale; // List scrolls horizontally, never vertically
    case view::ViewMode::Tree:
        return (2.0F * pad_dp) + (static_cast<float>(m.tree_rows().size()) * row_dp);
    case view::ViewMode::Columns:
        return (2.0F * pad_dp) + (static_cast<float>(m.row_count()) * row_dp);
    }
    return 0.0F;
}

// ---------------------------------------------------------------------------------------------
// Address bar View Lock button
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_view_lock_button(const BrowserFrame& f, const RectF& rect) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const view::ViewScope scope = f.model.view_scope();

    const bool inside = contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::ViewLock;
    }
    if (active_ == WidgetId::ViewLock && ptr.released && inside) {
        f.queue.enqueue(CommandId::ToggleViewLock);
    }
    if (inside && (active_ == WidgetId::None || active_ == WidgetId::ViewLock)) {
        r.fill_rounded_rect(rect, p.px(radius::md),
                            active_ == WidgetId::ViewLock ? pal.control_pressed
                                                          : pal.control_hover);
        switch (scope) {
        case view::ViewScope::Global: tooltip_text_ = std::string(tr("view.lock.global")); break;
        case view::ViewScope::Folder: tooltip_text_ = std::string(tr("view.lock.folder")); break;
        case view::ViewScope::Hierarchy:
            tooltip_text_ = std::string(tr("view.lock.hierarchy"));
            break;
        }
        tooltip_anchor_ = rect;
    }
    const Color ink = (scope != view::ViewScope::Global) ? pal.accent
                      : inside                           ? pal.text_primary
                                                         : pal.text_secondary;
    draw_lock_glyph(r, rect.center_x(), rect.center_y(), std::round(p.px(14.0F)), scope, ink,
                    pal.background);
}

// ---------------------------------------------------------------------------------------------
// Footer view-mode switcher and live zoom % slider
// ---------------------------------------------------------------------------------------------

float BrowserView::draw_view_footer_controls(const BrowserFrame& f, const Layout& l,
                                             float right_edge) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const BrowserModel& m = f.model;

    const float btn_sz = std::round(p.px(20.0F));
    const float btn_gap = std::round(p.px(2.0F));
    const float slider_w = std::round(p.px(72.0F));
    const float mid = l.status.y + (0.5F * l.status.h);
    const float btn_y = std::round(mid - (0.5F * btn_sz));

    // Only show if there is reasonable room in the status bar.
    if (l.status.w < p.px(320.0F)) {
        return 0.0F;
    }

    float x = right_edge;

    // 1. Zoom % readout + slider (rightmost).
    char pct_buf[16];
    std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", m.zoom_percent());
    const ShapedText pct_text = p.shape(type::status, pct_buf);
    const float pct_w = std::max(p.shape(type::status, "400%").width(), pct_text.width());
    x -= pct_w;
    p.draw(pct_text, x + pct_w - pct_text.width(), std::round(mid + (0.5F * pct_text.cap_height())),
           pal.text_secondary);

    x -= std::round(p.px(6.0F)) + slider_w;
    const RectF slider_hit{x, l.status.y + p.px(2.0F), slider_w, l.status.h - p.px(4.0F)};
    const bool s_inside = contains(slider_hit, ptr.x, ptr.y);
    if (s_inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::ZoomSlider;
    }
    if (active_ == WidgetId::ZoomSlider && ptr.down) {
        const float t =
            std::clamp((ptr.x - slider_hit.x) / std::max(slider_hit.w, 1.0F), 0.0F, 1.0F);
        const int raw_pct =
            view::kMinZoomPercent +
            static_cast<int>(
                std::lround(t * static_cast<float>(view::kMaxZoomPercent - view::kMinZoomPercent)));
        // Snap to multiples of 5% for clean readout.
        const int snapped = view::clamp_zoom(((raw_pct + 2) / 5) * 5);
        Intent intent;
        intent.kind = Intent::Kind::SetZoomPercent;
        intent.index = static_cast<std::size_t>(snapped);
        intent.additive = (m.view_mode() == view::ViewMode::Details);
        f.intents.push_back(std::move(intent));
    }

    const float track_h = std::max(2.0F, std::round(p.px(3.0F)));
    const RectF track{slider_hit.x, std::round(mid - (0.5F * track_h)), slider_hit.w, track_h};
    r.fill_rounded_rect(track, track_h * 0.5F, pal.border);
    const float frac = static_cast<float>(m.zoom_percent() - view::kMinZoomPercent) /
                       static_cast<float>(view::kMaxZoomPercent - view::kMinZoomPercent);
    const float fill_w = std::round(track.w * std::clamp(frac, 0.0F, 1.0F));
    if (fill_w > 0.0F) {
        r.fill_rounded_rect(RectF{track.x, track.y, fill_w, track_h}, track_h * 0.5F,
                            pal.accent.with_alpha(0.75F));
    }
    const float knob_r = std::round(p.px(4.5F));
    const float kx = track.x + fill_w;
    r.fill_rounded_rect(RectF{kx - knob_r, mid - knob_r, 2.0F * knob_r, 2.0F * knob_r}, knob_r,
                        active_ == WidgetId::ZoomSlider || s_inside ? pal.accent
                                                                    : pal.text_secondary);

    // 2. View mode buttons (Details, Icons, List, Tree, Columns) left of slider.
    x -= std::round(p.px(10.0F));
    constexpr std::array<view::ViewMode, 5> kModes{
        view::ViewMode::Columns,
        view::ViewMode::Tree,
        view::ViewMode::List,
        view::ViewMode::Icons,
        view::ViewMode::Details,
    };
    constexpr std::array<const char*, 5> kKeys{
        "view.mode.columns",
        "view.mode.tree",
        "view.mode.list",
        "view.mode.icons",
        "view.mode.details",
    };
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        x -= btn_sz;
        const RectF br{x, btn_y, btn_sz, btn_sz};
        const bool is_cur = (m.view_mode() == kModes[i]);
        const bool inside = contains(br, ptr.x, ptr.y);
        const auto mode_idx = static_cast<std::size_t>(kModes[i]);
        if (inside && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::ViewModeBtn;
            active_index_ = mode_idx;
        }
        if (active_ == WidgetId::ViewModeBtn && active_index_ == mode_idx && ptr.released &&
            inside) {
            Intent intent;
            intent.kind = Intent::Kind::SetViewMode;
            intent.index = mode_idx;
            f.intents.push_back(std::move(intent));
        }
        if (is_cur) {
            r.fill_rounded_rect(br, p.px(radius::sm), pal.accent.with_alpha(0.18F));
        } else if (inside && active_ == WidgetId::None) {
            r.fill_rounded_rect(br, p.px(radius::sm), pal.control_hover);
        }
        if (inside) {
            tooltip_text_ = std::string(tr(kKeys[i]));
            tooltip_anchor_ = br;
        }
        const Color ink = is_cur ? pal.accent : (inside ? pal.text_primary : pal.text_muted);
        draw_mode_glyph(r, br.center_x(), br.center_y(), std::round(p.px(11.0F)), kModes[i], ink);
        x -= btn_gap;
    }

    return (right_edge - x) + std::round(p.px(10.0F));
}

// ---------------------------------------------------------------------------------------------
// Grouped Details view (virtualized)
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_grouped_details(const BrowserFrame& f, const Layout& l) {
    ensure_grouped_cache(f);
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const PointerInput& ptr = f.pointer;
    const float spx = scroll_px(f);
    const float header_h = std::round(p.px(kGroupHeaderH));
    const float inset = std::round(p.px(metrics::row_inset));
    const float rad = std::round(p.px(radius::sm));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));

    float y = l.body.y + l.pad - spx;
    for (const GroupDisplayItem& it : grouped_cache_.items) {
        const float h = (it.kind == GroupDisplayItem::Kind::Header) ? header_h : l.row_h;
        if (y + h < l.body.y) {
            y += h;
            continue;
        }
        if (y > l.body.bottom()) {
            break;
        }

        if (it.kind == GroupDisplayItem::Kind::Header) {
            const RectF hr{l.body.x + inset, y, std::max(l.body.w - (2.0F * inset), 0.0F), h};
            const bool inside = contains(hr, ptr.x, ptr.y);
            if (inside && ptr.pressed && active_ == WidgetId::None) {
                active_ = WidgetId::GroupHeader;
                active_index_ = it.bucket;
            }
            if (active_ == WidgetId::GroupHeader && active_index_ == it.bucket && ptr.released &&
                inside) {
                Intent intent;
                intent.kind = Intent::Kind::ToggleGroup;
                intent.index = it.bucket;
                f.intents.push_back(std::move(intent));
            }
            if (inside) {
                r.fill_rounded_rect(hr, rad, pal.control_hover.with_alpha(0.5F));
            }
            const float chev_x = hr.x + std::round(p.px(12.0F));
            const float mid = hr.y + (0.5F * hr.h);
            draw_chevron(r, chev_x, mid, p.px(9.0F),
                         it.collapsed ? Direction::Right : Direction::Down,
                         std::max(1.0F, p.px(1.4F)), pal.text_muted);

            const std::string title = std::string(tr(group_bucket_key(m.group_by(), it.bucket))) +
                                      " (" + format_count(it.count) + ")";
            const ShapedText sh = p.shape(type::column_header, title);
            const float tx = hr.x + std::round(p.px(24.0F));
            p.draw(sh, tx, std::round(mid + (0.5F * sh.cap_height())), pal.text_secondary);
            const float line_x = tx + sh.width() + std::round(p.px(10.0F));
            if (line_x + p.px(12.0F) < hr.right()) {
                r.fill_rect(RectF{line_x, std::round(mid), hr.right() - line_x - p.px(6.0F), hair},
                            pal.border.with_alpha(0.6F));
            }
        } else {
            const std::size_t row = it.row;
            const fs::DirEntry& e = m.row_entry(row);
            const RectF rr{l.body.x + inset, y, std::max(l.body.w - (2.0F * inset), 0.0F), h};
            const bool inside = contains(rr, ptr.x, ptr.y);
            const bool sel = m.is_selected(row);
            const bool cur = (row == m.cursor());

            if (inside && ptr.pressed && active_ == WidgetId::None) {
                Intent intent;
                intent.kind = Intent::Kind::ClickRow;
                intent.row = row;
                intent.mods = ptr.mods;
                f.intents.push_back(std::move(intent));
                if (ptr.clicks >= 2) {
                    f.queue.enqueue(CommandId::Activate);
                }
            }

            if (sel) {
                r.fill_rounded_rect(rr, rad,
                                    f.focused ? pal.selection : pal.selection.with_alpha(0.5F));
            } else if (inside) {
                r.fill_rounded_rect(rr, rad, pal.row_hover);
            }
            if (cur && f.focused) {
                r.stroke_rounded_rect(rr, rad, hair, pal.accent.with_alpha(0.75F));
            }

            const float mid = rr.y + (0.5F * rr.h);
            const float icon_sz = std::round(p.px(metrics::icon_size));
            const RectF icon_box{l.name_x, std::round(mid - (0.5F * icon_sz)), icon_sz, icon_sz};
            bool drew_thumb = false;
            if (f.thumbnails != nullptr) {
                drew_thumb = f.thumbnails->draw_row(r, pal, m, row, icon_box, p.scale(), 1.0F);
            }
            if (!drew_thumb) {
                draw_entry_glyph(r, pal, e, icon_box.x, icon_box.y, icon_sz, 1.0F, p.scale());
            }
            const std::string dn = display_name(e.name);
            const ShapedText nt = fitter_.fit(
                p, type::row, dn, std::max(l.name_r - l.text_x, p.px(24.0F)), Elide::End);
            p.draw(nt, l.text_x, std::round(mid + (0.5F * nt.cap_height())), pal.text_primary);

            if (l.show_size && !fs::is_directory_like(e)) {
                const ShapedText st = p.shape(type::numeric, format_size(e.size));
                p.draw(st, l.size_r - st.width(), std::round(mid + (0.5F * st.cap_height())),
                       pal.text_secondary);
            }
            if (l.show_type) {
                const ShapedText tt = fitter_.fit(p, type::row, type_label(e),
                                                  std::max(l.type_r - l.type_x, 0.0F), Elide::End);
                p.draw(tt, l.type_x, std::round(mid + (0.5F * tt.cap_height())), pal.text_muted);
            }
            if (l.show_date && e.mtime != Timestamp{}) {
                const std::string dt_s =
                    l.short_date ? format_date_short(e.mtime, f.now) : format_date(e.mtime, f.now);
                const ShapedText dt = p.shape(type::row, dt_s);
                p.draw(dt, l.date_x, std::round(mid + (0.5F * dt.cap_height())), pal.text_muted);
            }
        }
        y += h;
    }
}

// ---------------------------------------------------------------------------------------------
// Icon Grid view (virtualized, live zoom, supports Group By)
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_icons_view(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const PointerInput& ptr = f.pointer;
    const float spx = scroll_px(f);
    const float pad = std::round(p.px(8.0F));
    const float rad = std::round(p.px(radius::md));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));

    float icon_px = 0.0F;
    float tile_w = 0.0F;
    float tile_h = 0.0F;
    float gap_px = 0.0F;
    std::size_t cols = 1;
    draw_grid_tile_metrics(f, l.body.w, icon_px, tile_w, tile_h, gap_px, cols);

    auto draw_tile = [&](std::size_t row, float tx, float ty) {
        const fs::DirEntry& e = m.row_entry(row);
        const RectF tile{tx, ty, tile_w, tile_h};
        const bool inside = contains(tile, ptr.x, ptr.y);
        const bool sel = m.is_selected(row);
        const bool cur = (row == m.cursor());

        if (inside && ptr.pressed && active_ == WidgetId::None) {
            Intent intent;
            intent.kind = Intent::Kind::ClickRow;
            intent.row = row;
            intent.mods = ptr.mods;
            f.intents.push_back(std::move(intent));
            if (ptr.clicks >= 2) {
                f.queue.enqueue(CommandId::Activate);
            }
        }

        if (sel) {
            r.fill_rounded_rect(tile, rad,
                                f.focused ? pal.selection : pal.selection.with_alpha(0.5F));
        } else if (inside) {
            r.fill_rounded_rect(tile, rad, pal.row_hover);
        }
        if (cur && f.focused) {
            r.stroke_rounded_rect(tile, rad, hair, pal.accent.with_alpha(0.80F));
        }

        const float ix = std::round(tile.x + (0.5F * (tile.w - icon_px)));
        const float iy = std::round(tile.y + p.px(8.0F));
        const RectF icon_box{ix, iy, icon_px, icon_px};
        bool drew_thumb = false;
        if (f.thumbnails != nullptr) {
            drew_thumb = f.thumbnails->draw_row(r, pal, m, row, icon_box, p.scale(), 1.0F);
        }
        if (!drew_thumb) {
            draw_entry_glyph(r, pal, e, ix, iy, icon_px, 1.0F, p.scale());
        }

        const std::string dn = display_name(e.name);
        const ShapedText nt = fitter_.fit(
            p, type::row, dn, std::max(tile.w - p.px(10.0F), p.px(24.0F)), Elide::Middle);
        const float label_x = std::round(tile.x + (0.5F * (tile.w - nt.width())));
        const float label_y = std::round(iy + icon_px + p.px(18.0F));
        p.draw(nt, label_x, label_y, pal.text_primary);
    };

    if (m.group_by() != view::GroupBy::None && !m.flattened()) {
        ensure_grouped_cache(f);
        const float header_h = std::round(p.px(kGroupHeaderH));
        float y = l.body.y + l.pad - spx;
        std::size_t col = 0;
        for (const GroupDisplayItem& it : grouped_cache_.items) {
            if (it.kind == GroupDisplayItem::Kind::Header) {
                if (col > 0) {
                    y += tile_h + gap_px;
                    col = 0;
                }
                if (y + header_h >= l.body.y && y <= l.body.bottom()) {
                    const RectF hr{l.body.x + pad, y, std::max(l.body.w - (2.0F * pad), 0.0F),
                                   header_h};
                    const bool inside = contains(hr, ptr.x, ptr.y);
                    if (inside && ptr.pressed && active_ == WidgetId::None) {
                        active_ = WidgetId::GroupHeader;
                        active_index_ = it.bucket;
                    }
                    if (active_ == WidgetId::GroupHeader && active_index_ == it.bucket &&
                        ptr.released && inside) {
                        Intent intent;
                        intent.kind = Intent::Kind::ToggleGroup;
                        intent.index = it.bucket;
                        f.intents.push_back(std::move(intent));
                    }
                    const float mid = hr.y + (0.5F * hr.h);
                    draw_chevron(r, hr.x + p.px(10.0F), mid, p.px(9.0F),
                                 it.collapsed ? Direction::Right : Direction::Down,
                                 std::max(1.0F, p.px(1.4F)), pal.text_muted);
                    const std::string title =
                        std::string(tr(group_bucket_key(m.group_by(), it.bucket))) + " (" +
                        format_count(it.count) + ")";
                    const ShapedText sh = p.shape(type::column_header, title);
                    p.draw(sh, hr.x + p.px(22.0F), std::round(mid + (0.5F * sh.cap_height())),
                           pal.text_secondary);
                }
                y += header_h;
            } else {
                const float tx = l.body.x + pad + (static_cast<float>(col) * (tile_w + gap_px));
                if (y + tile_h >= l.body.y && y <= l.body.bottom()) {
                    draw_tile(it.row, tx, y);
                }
                ++col;
                if (col >= cols) {
                    col = 0;
                    y += tile_h + gap_px;
                }
            }
        }
        return;
    }

    // Ungrouped Icon Grid: direct O(viewport) index math.
    const std::size_t n = m.row_count();
    const float stride_y = tile_h + gap_px;
    const std::size_t total_grid_rows = (n + cols - 1) / cols;
    const std::size_t first_gr =
        spx > l.pad ? static_cast<std::size_t>((spx - l.pad) / stride_y) : 0;
    const std::size_t vis_gr = static_cast<std::size_t>(l.body.h / stride_y) + 2;
    const std::size_t last_gr = std::min(total_grid_rows, first_gr + vis_gr);

    for (std::size_t gr = first_gr; gr < last_gr; ++gr) {
        const float ty = l.body.y + l.pad + (static_cast<float>(gr) * stride_y) - spx;
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t row = (gr * cols) + c;
            if (row >= n) {
                break;
            }
            const float tx = l.body.x + pad + (static_cast<float>(c) * (tile_w + gap_px));
            draw_tile(row, tx, ty);
        }
    }
    // Prefetch one grid row below viewport.
    if (f.thumbnails != nullptr && last_gr < total_grid_rows) {
        for (std::size_t c = 0; c < cols; ++c) {
            const std::size_t row = (last_gr * cols) + c;
            if (row >= n) {
                break;
            }
            f.thumbnails->prefetch_row(m, row, icon_px);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// List / Columns view (virtualized multi-column flowing list)
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_list_view(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const PointerInput& ptr = f.pointer;
    const float pad = std::round(p.px(6.0F));
    const float col_w = std::round(p.px(kListColW));
    const float col_gap = std::round(p.px(8.0F));
    const float rad = std::round(p.px(radius::sm));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    const float icon_sz = std::round(p.px(metrics::icon_size));

    const std::size_t rows_per_col =
        l.row_h > 0.0F ? std::max<std::size_t>(
                             1, static_cast<std::size_t>((l.body.h - (2.0F * pad)) / l.row_h))
                       : 1;
    const std::size_t n = m.row_count();
    const std::size_t total_cols = (n + rows_per_col - 1) / rows_per_col;
    const float stride_x = col_w + col_gap;
    const float hspx = std::round(p.px(hscroll_));

    const std::size_t first_col =
        hspx > pad ? static_cast<std::size_t>((hspx - pad) / stride_x) : 0;
    const std::size_t vis_cols = static_cast<std::size_t>(l.body.w / stride_x) + 2;
    const std::size_t last_col = std::min(total_cols, first_col + vis_cols);

    for (std::size_t c = first_col; c < last_col; ++c) {
        const float cx = l.body.x + pad + (static_cast<float>(c) * stride_x) - hspx;
        for (std::size_t ri = 0; ri < rows_per_col; ++ri) {
            const std::size_t row = (c * rows_per_col) + ri;
            if (row >= n) {
                break;
            }
            const fs::DirEntry& e = m.row_entry(row);
            const float cy = l.body.y + pad + (static_cast<float>(ri) * l.row_h);
            const RectF rr{cx, cy, col_w, l.row_h};
            const bool inside = contains(rr, ptr.x, ptr.y);
            const bool sel = m.is_selected(row);
            const bool cur = (row == m.cursor());

            if (inside && ptr.pressed && active_ == WidgetId::None) {
                Intent intent;
                intent.kind = Intent::Kind::ClickRow;
                intent.row = row;
                intent.mods = ptr.mods;
                f.intents.push_back(std::move(intent));
                if (ptr.clicks >= 2) {
                    f.queue.enqueue(CommandId::Activate);
                }
            }

            if (sel) {
                r.fill_rounded_rect(rr, rad,
                                    f.focused ? pal.selection : pal.selection.with_alpha(0.5F));
            } else if (inside) {
                r.fill_rounded_rect(rr, rad, pal.row_hover);
            }
            if (cur && f.focused) {
                r.stroke_rounded_rect(rr, rad, hair, pal.accent.with_alpha(0.75F));
            }

            const float mid = rr.y + (0.5F * rr.h);
            const float ix = rr.x + std::round(p.px(6.0F));
            draw_entry_glyph(r, pal, e, ix, std::round(mid - (0.5F * icon_sz)), icon_sz, 1.0F,
                             p.scale());
            const float tx = ix + icon_sz + std::round(p.px(8.0F));
            const ShapedText nt =
                fitter_.fit(p, type::row, display_name(e.name),
                            std::max(rr.right() - tx - p.px(6.0F), p.px(20.0F)), Elide::End);
            p.draw(nt, tx, std::round(mid + (0.5F * nt.cap_height())), pal.text_primary);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Tree view (virtualized expandable directory tree)
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_tree_view(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const PointerInput& ptr = f.pointer;
    const auto& rows = m.tree_rows();
    if (rows.empty() || l.row_h <= 0.0F) {
        return;
    }

    const float spx = scroll_px(f);
    const float inset = std::round(p.px(metrics::row_inset));
    const float rad = std::round(p.px(radius::sm));
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    const float icon_sz = std::round(p.px(metrics::icon_size));
    const float indent_px = std::round(p.px(kTreeIndent));
    const float chev_box = std::round(p.px(18.0F));

    const std::size_t first = spx > l.pad ? static_cast<std::size_t>((spx - l.pad) / l.row_h) : 0;
    const std::size_t vis = static_cast<std::size_t>(l.body.h / l.row_h) + 2;
    const std::size_t last = std::min(rows.size(), first + vis);

    for (std::size_t i = first; i < last; ++i) {
        const TreeRow& tr_row = rows[i];
        const float y = l.body.y + l.pad + (static_cast<float>(i) * l.row_h) - spx;
        const RectF rr{l.body.x + inset, y, std::max(l.body.w - (2.0F * inset), 0.0F), l.row_h};
        const bool inside = contains(rr, ptr.x, ptr.y);
        const bool cur = (i == m.tree_cursor());
        const bool sel = (tr_row.root_row != kTreeNoRootRow && tr_row.root_row < m.row_count() &&
                          m.is_selected(tr_row.root_row));

        const float base_x = l.name_x + (static_cast<float>(tr_row.depth) * indent_px);
        const RectF chev_rect{base_x, rr.y, chev_box, rr.h};
        const bool on_chev = tr_row.expandable && contains(chev_rect, ptr.x, ptr.y);

        if (on_chev && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::TreeChevron;
            active_index_ = i;
        }
        if (active_ == WidgetId::TreeChevron && active_index_ == i && ptr.released && on_chev) {
            Intent intent;
            intent.kind = Intent::Kind::TreeToggle;
            intent.path = tr_row.full_path;
            f.intents.push_back(std::move(intent));
        } else if (inside && !on_chev && ptr.pressed && active_ == WidgetId::None) {
            Intent intent;
            intent.kind = Intent::Kind::TreeClickRow;
            intent.row = i;
            intent.mods = ptr.mods;
            intent.additive = (ptr.clicks >= 2);
            f.intents.push_back(std::move(intent));
        }

        if (sel || cur) {
            r.fill_rounded_rect(rr, rad,
                                f.focused ? pal.selection : pal.selection.with_alpha(0.5F));
        } else if (inside) {
            r.fill_rounded_rect(rr, rad, pal.row_hover);
        }
        if (cur && f.focused) {
            r.stroke_rounded_rect(rr, rad, hair, pal.accent.with_alpha(0.75F));
        }

        const float mid = rr.y + (0.5F * rr.h);
        if (tr_row.expandable) {
            // Animations-off: chevron snaps immediately between Right and Down (no rotation fade).
            draw_chevron(r, chev_rect.center_x(), mid, p.px(9.0F),
                         tr_row.expanded ? Direction::Down : Direction::Right,
                         std::max(1.0F, p.px(1.4F)), on_chev ? pal.text_primary : pal.text_muted);
        }
        const float ix = base_x + chev_box + std::round(p.px(2.0F));
        draw_entry_glyph(r, pal, tr_row.entry, ix, std::round(mid - (0.5F * icon_sz)), icon_sz,
                         1.0F, p.scale());
        const float tx = ix + icon_sz + std::round(p.px(8.0F));
        const std::string dn = display_name(tr_row.entry.name);
        const ShapedText nt =
            fitter_.fit(p, type::row, dn, std::max(l.name_r - tx, p.px(24.0F)), Elide::End);
        p.draw(nt, tx, std::round(mid + (0.5F * nt.cap_height())), pal.text_primary);

        if (l.show_size && !fs::is_directory_like(tr_row.entry)) {
            const ShapedText st = p.shape(type::numeric, format_size(tr_row.entry.size));
            p.draw(st, l.size_r - st.width(), std::round(mid + (0.5F * st.cap_height())),
                   pal.text_secondary);
        }
        if (l.show_date && tr_row.entry.mtime != Timestamp{}) {
            const std::string dt_s = l.short_date ? format_date_short(tr_row.entry.mtime, f.now)
                                                  : format_date(tr_row.entry.mtime, f.now);
            const ShapedText dt = p.shape(type::row, dt_s);
            p.draw(dt, l.date_x, std::round(mid + (0.5F * dt.cap_height())), pal.text_muted);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 10: Miller Columns view (v1)
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_columns_view(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const BrowserModel& m = f.model;

    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    const float rad = std::round(p.px(5.0F));
    const float pad = std::round(p.px(6.0F));
    const float icon_sz = std::round(p.px(metrics::icon_size));

    const float col0_w = std::round(std::clamp(l.body.w * 0.22F, p.px(130.0F), p.px(220.0F)));
    const float col2_w = std::round(std::clamp(l.body.w * 0.30F, p.px(160.0F), p.px(320.0F)));
    const float col1_w = std::max(p.px(160.0F), l.body.w - col0_w - col2_w);

    const RectF col0{l.body.x, l.body.y, col0_w, l.body.h};
    const RectF col1{col0.right(), l.body.y, col1_w, l.body.h};
    const RectF col2{col1.right(), l.body.y, std::max(0.0F, l.body.right() - col1.right()),
                     l.body.h};

    // Vertical column dividers.
    r.fill_rect(RectF{col1.x, l.body.y, hair, l.body.h}, pal.border);
    r.fill_rect(RectF{col2.x, l.body.y, hair, l.body.h}, pal.border);

    // Column 0: ancestor hierarchy chain (clickable to jump up).
    std::vector<Path> chain;
    Path walk = m.current_dir();
    while (!walk.empty()) {
        chain.push_back(walk);
        const Path parent = walk.parent_path();
        if (parent == walk) {
            break;
        }
        walk = parent;
    }
    std::ranges::reverse(chain);
    float ay = col0.y + pad;
    for (std::size_t idx = 0; idx < chain.size(); ++idx) {
        if (ay + l.row_h > col0.bottom()) {
            break;
        }
        const RectF ar = snap(RectF{col0.x + pad, ay, col0.w - (2.0F * pad), l.row_h});
        const bool is_cur = (idx + 1 == chain.size());
        const bool inside = contains(ar, ptr.x, ptr.y);
        if (inside && ptr.pressed && active_ == WidgetId::None && !is_cur) {
            Intent intent;
            intent.kind = Intent::Kind::NavigateTo;
            intent.path = chain[idx];
            f.intents.push_back(std::move(intent));
        }
        if (is_cur) {
            r.fill_rounded_rect(ar, rad, pal.selection.with_alpha(0.6F));
        } else if (inside) {
            r.fill_rounded_rect(ar, rad, pal.row_hover);
        }
        std::string label = chain[idx].filename().native();
        if (label.empty()) {
            label = chain[idx].native();
        }
        const ShapedText t =
            fitter_.fit(p, type::row, display_name(label), std::max(ar.w - p.px(16.0F), p.px(20.0F)),
                        Elide::End);
        p.draw(t, ar.x + p.px(8.0F), std::round(ar.center_y() + (0.5F * t.cap_height())),
               is_cur ? pal.text_primary : pal.text_secondary);
        ay += l.row_h;
    }

    // Column 1: active folder virtualized rows.
    const std::size_t n = m.row_count();
    const float sp = scroll_px(f);
    const float top = sp - l.pad;
    const std::size_t first = top <= 0.0F ? 0 : static_cast<std::size_t>(top / l.row_h);
    const std::size_t last =
        n == 0 ? 0 : std::min(n - 1, static_cast<std::size_t>((top + col1.h) / l.row_h));

    for (std::size_t i = first; i < n && i <= last; ++i) {
        const fs::DirEntry& e = m.row_entry(i);
        const float y = col1.y + l.pad + (static_cast<float>(i) * l.row_h) - sp;
        const RectF rr = snap(RectF{col1.x + pad, y, col1.w - (2.0F * pad) - p.px(10.0F), l.row_h});
        if (rr.bottom() < col1.y || rr.y > col1.bottom()) {
            continue;
        }
        const bool sel = m.is_selected(i);
        const bool cur = (m.cursor() == i);
        const bool inside = contains(rr, ptr.x, ptr.y);
        if (inside && ptr.pressed && active_ == WidgetId::None) {
            Intent in;
            in.kind = Intent::Kind::ClickRow;
            in.row = i;
            in.mods = ptr.mods;
            f.intents.push_back(in);
            if (ptr.clicks >= 2) {
                f.queue.enqueue(CommandId::Activate);
            }
        }
        if (sel) {
            r.fill_rounded_rect(rr, rad, f.focused ? pal.selection : pal.selection_inactive);
        } else if (inside) {
            r.fill_rounded_rect(rr, rad, pal.row_hover);
        }
        if (cur && f.focused) {
            r.stroke_rounded_rect(rr, rad, hair, pal.focus_ring.with_alpha(0.75F));
        }

        const float mid = rr.center_y();
        const float ix = rr.x + p.px(6.0F);
        draw_entry_glyph(r, pal, e, ix, std::round(mid - (0.5F * icon_sz)), icon_sz, 1.0F,
                         p.scale());
        const bool dir = fs::is_directory_like(e);
        const float tx = ix + icon_sz + p.px(8.0F);
        const float right_reserve = dir ? p.px(22.0F) : p.px(10.0F);
        const ShapedText nt = fitter_.fit(
            p, type::row, display_name(e.name),
            std::max(rr.right() - tx - right_reserve, p.px(24.0F)), Elide::End);
        p.draw(nt, tx, std::round(mid + (0.5F * nt.cap_height())), pal.text_primary);
        if (dir) {
            draw_chevron(r, rr.right() - p.px(10.0F), mid, p.px(8.0F), Direction::Right, hair,
                         pal.text_muted);
        }
    }

    // Column 2: preview / metadata card for cursor entry.
    if (col2.w > p.px(40.0F) && m.cursor() != kNoRow && m.cursor() < n) {
        const fs::DirEntry& cur_e = m.row_entry(m.cursor());
        const Path cur_path = m.row_path(m.cursor());
        float py = col2.y + p.px(12.0F);
        const float px_left = col2.x + p.px(12.0F);
        const float avail_w = std::max(col2.w - p.px(24.0F), p.px(24.0F));

        const ShapedText title =
            fitter_.fit(p, type::section, display_name(cur_e.name), avail_w, Elide::Middle);
        p.draw(title, px_left, py + title.cap_height(), pal.text_primary);
        py += title.cap_height() + p.px(14.0F);

        if (!fs::is_directory_like(cur_e)) {
            const ShapedText sz = p.shape(type::caption, format_size(cur_e.size));
            p.draw(sz, px_left, py + sz.cap_height(), pal.text_secondary);
            py += sz.cap_height() + p.px(10.0F);
        }
        if (cur_e.mtime != Timestamp{}) {
            const ShapedText dt = p.shape(type::caption, format_date(cur_e.mtime, f.now));
            p.draw(dt, px_left, py + dt.cap_height(), pal.text_muted);
            py += dt.cap_height() + p.px(12.0F);
        }
        if (f.git_status != nullptr) {
            const git::GitFileState gs = f.git_status->status_for(cur_path);
            const std::string_view badge = git::git_state_badge(gs);
            if (!badge.empty()) {
                const ShapedText gt = p.shape(type::caption, "Git: " + std::string(badge));
                p.draw(gt, px_left, py + gt.cap_height(), pal.accent);
                py += gt.cap_height() + p.px(12.0F);
            }
        }
        if (f.plugins != nullptr) {
            if (const auto prev = f.plugins->query_preview(cur_path); prev && !prev->empty()) {
                const ShapedText pt = fitter_.fit(p, type::caption, *prev, avail_w, Elide::End);
                p.draw(pt, px_left, py + pt.cap_height(), pal.text_secondary);
            }
        }
    }
}

} // namespace navigator::ui

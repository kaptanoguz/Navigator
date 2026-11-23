// src/ui/trash/trash_view.cpp — see trash_view.hpp.
#include "trash/trash_view.hpp"

#include <algorithm>
#include <cmath>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "widgets/icons.hpp"

namespace navigator::ui {
namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

Color over(const Color& base, const Color& overlay) {
    const float a = overlay.a;
    return Color{(base.r * (1.0F - a)) + (overlay.r * a), (base.g * (1.0F - a)) + (overlay.g * a),
                 (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
}

float hair(const Painter& p) { return std::max(1.0F, std::round(p.px(1.0F))); }

} // namespace

void TrashView::draw(const TrashFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const TrashModel& m = f.model;
    const DensityMetrics dm = density_metrics(f.density);
    const RectF b = f.bounds;
    const float margin = std::round(p.px(dm.panel_margin));
    const RectF top{b.x, b.y, b.w, std::round(p.px(dm.top_bar_height))};
    const float status_h = std::round(p.px(dm.status_bar_height));
    const RectF status{b.x, b.bottom() - status_h, b.w, status_h};
    const RectF panel{b.x + margin, top.bottom(), std::max(b.w - (2.0F * margin), 0.0F),
                      std::max(status.y - top.bottom(), 0.0F)};
    const RectF header{panel.x, panel.y, panel.w, std::round(p.px(dm.header_height))};
    const RectF body{panel.x, header.bottom(), panel.w,
                     std::max(panel.bottom() - header.bottom(), 0.0F)};
    const float row_h = std::round(p.px(dm.row_height));
    const float pad = std::round(p.px(4.0F));
    const float inset = std::round(p.px(metrics::row_inset));
    const float rad = std::round(p.px(radius::lg - 2.0F));

    // ---- Top bar ------------------------------------------------------------------------------
    const float btn = std::round(p.px(dm.tool_button));
    const float mid = top.center_y();
    {
        const RectF back = snap(RectF{top.x + margin, mid - (0.5F * btn), btn, btn});
        const bool hv = contains(back, ptr.x, ptr.y);
        if (hv) {
            r.fill_rounded_rect(back, p.px(radius::sm + 1.0F), pal.control_hover);
        }
        draw_chevron(r, back.center_x() - p.px(1.0F), back.center_y(), p.px(5.0F), Direction::Left,
                     std::max(1.5F, p.px(1.6F)), hv ? pal.text_primary : pal.text_secondary);
        if (ptr.pressed && hv && active_ == WidgetId::None) {
            active_ = WidgetId::Back;
        }
        if (active_ == WidgetId::Back && ptr.released && hv) {
            f.queue.enqueue(CommandId::NavigateBack);
        }
        const ShapedText title = p.shape(type::crumb_current, tr("trash.title"));
        const float tx = back.right() + std::round(p.px(8.0F));
        p.draw(title, tx, std::round(mid + (0.5F * title.cap_height())), pal.text_primary);
        if (m.loaded() && m.error() == std::nullopt) {
            const ShapedText n = p.shape(type::status, tr_count("trash.items", m.count()));
            p.draw(n, tx + title.width() + std::round(p.px(12.0F)),
                   std::round(mid + (0.5F * n.cap_height())), pal.text_muted);
        }
    }
    // Right: Restore (primary-ish), Empty Trash (danger text).
    {
        float x = top.right() - margin;
        const auto tool = [&](std::string_view label, bool enabled, bool danger, CommandId id,
                              WidgetId wid, std::size_t idx) {
            const ShapedText t = p.shape(type::button, label);
            const float w = std::round(t.width() + p.px(24.0F));
            const RectF rr =
                snap(RectF{x - w, mid - (0.5F * btn) + p.px(1.0F), w, btn - p.px(2.0F)});
            const bool hv = enabled && contains(rr, ptr.x, ptr.y);
            const float brad = std::round(p.px(radius::md - 1.0F));
            r.stroke_rounded_rect(rr, brad, hair(p), pal.keycap_border);
            if (hv) {
                r.fill_rounded_rect(rr, brad,
                                    active_ == wid && active_index_ == idx && ptr.down
                                        ? pal.control_pressed
                                        : pal.control_hover);
            }
            const Color ink = !enabled ? pal.text_muted : danger ? pal.danger : pal.text_primary;
            p.draw(t, std::round(rr.center_x() - (0.5F * t.width())),
                   std::round(rr.center_y() + (0.5F * t.cap_height())), ink);
            if (enabled && hv && ptr.pressed && active_ == WidgetId::None) {
                active_ = wid;
                active_index_ = idx;
            }
            if (enabled && active_ == wid && active_index_ == idx && ptr.released && hv) {
                f.queue.enqueue(id);
            }
            x = rr.x - std::round(p.px(8.0F));
        };
        tool(tr("trash.empty_button"), m.count() > 0, true, CommandId::EmptyTrash, WidgetId::Types,
             2);
        tool(tr("trash.restore"), m.count() > 0 && (m.selected_count() > 0 || m.cursor() != kNone),
             false, CommandId::RestoreFromTrash, WidgetId::Types, 1);
    }

    // ---- List card ------------------------------------------------------------------------------
    r.fill_rounded_rect(panel, rad, pal.surface);
    // Columns.
    const float gap = std::round(p.px(kColumnGapDp));
    const float left = std::round(panel.x + inset + p.px(10.0F));
    const float right = std::round(panel.right() - inset - p.px(10.0F));
    const float text_x = std::round(left + p.px(metrics::icon_size + 10.0F));
    const float room = (right - text_x) / p.scale();
    const bool show_size = room >= 420.0F;
    const bool show_orig = room >= 560.0F;
    float edge = right;
    const float date_w = std::round(p.px(150.0F));
    const float date_x = std::round(edge - date_w);
    edge = date_x - gap;
    float size_x = edge;
    float size_r = edge;
    if (show_size) {
        size_r = edge;
        size_x = std::round(size_r - p.px(kSizeWidthDp));
        edge = size_x - gap;
    }
    float orig_x = edge;
    float orig_r = edge;
    float name_r = edge;
    if (show_orig) {
        const float ow =
            std::round(std::clamp((edge - text_x) * 0.42F, p.px(140.0F), p.px(340.0F)));
        orig_r = edge;
        orig_x = orig_r - ow;
        name_r = orig_x - gap;
    }
    // Header.
    {
        const float hy = header.y + (0.5F * header.h);
        const auto label = [&](std::string_view key, float x0, float x1, bool align_right) {
            const ShapedText t = p.shape(type::column_header, tr(key));
            const float x = align_right ? x1 - t.width() : x0;
            p.draw(t, x, std::round(hy + (0.5F * t.cap_height())), pal.text_secondary);
        };
        label("column.name", left, name_r, false);
        if (show_orig) {
            label("trash.col.original", orig_x, orig_r, false);
        }
        if (show_size) {
            label("column.size", size_x, size_r, true);
        }
        label("trash.col.deleted", date_x, right, false);
        r.fill_rect(RectF{header.x + hair(p), header.bottom() - hair(p),
                          header.w - (2.0F * hair(p)), hair(p)},
                    pal.border);
    }

    // Scrolling.
    const float content_px = (2.0F * pad) + (static_cast<float>(m.count()) * row_h);
    max_scroll_ = std::max(0.0F, content_px - body.h);
    page_rows_ =
        row_h > 0.0F ? std::max<std::size_t>(1, static_cast<std::size_t>(body.h / row_h)) : 1;
    if (ptr.wheel_y != 0.0F && contains(panel, ptr.x, ptr.y)) {
        scroll_ -= ptr.wheel_y * 3.0F * row_h;
    }
    if (m.serial() != seen_serial_) {
        seen_serial_ = m.serial();
        const std::size_t c = m.cursor();
        if (c != kNone) {
            const float y0 = pad + (static_cast<float>(c) * row_h);
            if (y0 < scroll_) {
                scroll_ = y0 - pad;
            } else if (y0 + row_h > scroll_ + body.h) {
                scroll_ = y0 + row_h + pad - body.h;
            }
        }
    }
    scroll_ = std::clamp(scroll_, 0.0F, max_scroll_);

    // Pointer: rows.
    hover_ = kNone;
    const bool in_body = contains(body, ptr.x, ptr.y) && ptr.x < body.right() - p.px(14.0F);
    std::size_t row_under = kNone;
    if (in_body) {
        const float cy = ptr.y - body.y + scroll_ - pad;
        if (cy >= 0.0F && row_h > 0.0F) {
            const auto idx = static_cast<std::size_t>(cy / row_h);
            if (idx < m.count()) {
                row_under = idx;
            }
        }
        hover_ = row_under;
    }
    if (ptr.pressed && in_body && active_ == WidgetId::None) {
        active_ = WidgetId::TrashRows;
        if (row_under != kNone) {
            Intent in;
            in.kind = Intent::Kind::TrashClickRow;
            in.row = row_under;
            in.mods = ptr.mods;
            f.intents.push_back(in);
            if (ptr.clicks >= 2) {
                f.queue.enqueue(CommandId::RestoreFromTrash);
            }
        } else {
            Intent in;
            in.kind = Intent::Kind::TrashClearSelect;
            f.intents.push_back(in);
        }
    }

    // Rows.
    r.set_clip(
        RectF{body.x + hair(p), body.y, body.w - (2.0F * hair(p)), std::max(body.h - pad, 0.0F)});
    if (!m.available() || m.error()) {
        const bool unavailable =
            !m.available() || (m.error() && m.error()->code == ErrorCode::Unsupported);
        const ShapedText t =
            p.shape(type::empty_title, tr(unavailable ? "trash.unavailable" : "error.io_error"));
        p.draw(t, std::round(body.center_x() - (0.5F * t.width())),
               std::round(body.y + (0.38F * body.h)), pal.text_primary);
        if (unavailable) {
            const ShapedText h = fitter_.fit(p, type::body, tr("trash.unavailable_hint"),
                                             body.w - p.px(40.0F), Elide::End);
            p.draw(h, std::round(body.center_x() - (0.5F * h.width())),
                   std::round(body.y + (0.38F * body.h) + p.px(26.0F)), pal.text_muted);
        }
    } else if (!m.loaded()) {
        const ShapedText t = p.shape(type::empty_title, tr("trash.loading"));
        p.draw(t, std::round(body.center_x() - (0.5F * t.width())),
               std::round(body.y + (0.38F * body.h)), pal.text_secondary);
    } else if (m.count() == 0) {
        const ShapedText t = p.shape(type::empty_title, tr("trash.empty_state"));
        p.draw(t, std::round(body.center_x() - (0.5F * t.width())),
               std::round(body.y + (0.36F * body.h)), pal.text_primary);
        const ShapedText h =
            fitter_.fit(p, type::body, tr("trash.empty_hint"), body.w - p.px(40.0F), Elide::End);
        p.draw(h, std::round(body.center_x() - (0.5F * h.width())),
               std::round(body.y + (0.36F * body.h) + p.px(26.0F)), pal.text_muted);
    } else {
        const float top_px = scroll_ - pad;
        const std::size_t first = top_px <= 0.0F ? 0 : static_cast<std::size_t>(top_px / row_h);
        const std::size_t last =
            std::min(m.count() - 1, static_cast<std::size_t>((top_px + body.h) / row_h));
        const float rx = panel.x + inset;
        const float rw = panel.w - (2.0F * inset);
        const float srad = std::round(p.px(6.0F));
        const auto row_y = [&](std::size_t i) {
            return body.y + pad + (static_cast<float>(i) * row_h) - scroll_;
        };
        const Color sel = over(pal.surface, f.focused ? pal.selection : pal.selection_inactive);
        const float glyph = std::round(p.px(metrics::icon_size));
        for (std::size_t i = first; i <= last && i < m.count(); ++i) {
            const trash::TrashedItem& it = m.items()[i];
            const RectF rr = snap(RectF{rx, row_y(i), rw, row_h});
            if (i % 2 == 1) {
                r.fill_rounded_rect(rr, srad, pal.row_alt);
            }
            const bool selected = m.is_selected(i);
            if (i == hover_ && !selected) {
                r.fill_rounded_rect(rr, srad, pal.row_hover);
            }
            if (selected) {
                r.fill_rounded_rect(rr, srad, sel);
                const float mw = std::max(2.0F, std::round(p.px(3.0F)));
                const float my = p.px(6.0F);
                r.fill_rounded_rect(snap(RectF{rx + p.px(3.0F), rr.y + my, mw, rr.h - (2.0F * my)}),
                                    0.5F * mw, f.focused ? pal.selection_marker : pal.text_muted);
            }
            if (i == m.cursor() && f.focused) {
                r.stroke_rounded_rect(rr, srad, hair(p),
                                      selected ? pal.focus_ring.with_alpha(0.55F) : pal.focus_ring);
            }
            const fs::DirEntry e = TrashModel::entry_of(it);
            const float cy = std::round(rr.y + (0.5F * row_h));
            draw_entry_glyph(r, pal, e, left, std::round(cy - (0.5F * glyph)), glyph, 1.0F,
                             p.scale());
            const std::string name = TrashModel::display_name_of(it);
            const ShapedText nt =
                fitter_.fit(p, type::row, name, name_r - text_x,
                            fs::is_directory_like(e) ? Elide::End : Elide::Middle);
            const float baseline = std::round(cy + (0.5F * nt.cap_height()));
            p.draw(nt, text_x, baseline, pal.text_primary);
            if (show_orig) {
                Path parent = it.original_path.parent_path();
                std::string shown = fs::to_display_name(parent.string());
                if (f.home != nullptr && !f.home->empty() &&
                    shown.rfind(f.home->string(), 0) == 0) {
                    shown = "~" + shown.substr(f.home->string().size());
                }
                const ShapedText ot =
                    fitter_.fit(p, type::row, shown, orig_r - orig_x, Elide::Middle);
                p.draw(ot, orig_x, baseline, pal.text_secondary);
            }
            if (show_size && it.kind != fs::EntryKind::Directory) {
                const SizeParts parts = format_size_parts(it.size);
                const ShapedText num = p.shape(type::row_numeric, parts.number);
                const ShapedText unit = p.shape(type::row_numeric, parts.unit);
                const float unit_slot = std::round(p.px(24.0F));
                p.draw(num, size_r - unit_slot - std::round(p.px(3.5F)) - num.width(), baseline,
                       pal.text_secondary);
                p.draw(unit, size_r - unit_slot, baseline, pal.text_muted);
            }
            const std::string date = format_date(TrashModel::deleted_at(it), f.now);
            if (!date.empty()) {
                p.draw(fitter_.fit(p, type::row_numeric, date, right - date_x, Elide::End), date_x,
                       baseline, pal.text_secondary);
            }
        }
    }
    // Scrollbar.
    if (max_scroll_ > 0.0F) {
        const float margin_s = p.px(3.0F);
        const float track = body.h - (2.0F * margin_s);
        const float thumb_h = std::max(p.px(32.0F), track * (body.h / (max_scroll_ + body.h)));
        const float travel = std::max(track - thumb_h, 0.0F);
        const float t = std::clamp(scroll_ / max_scroll_, 0.0F, 1.0F);
        const float w = p.px(metrics::scrollbar_width);
        r.fill_rounded_rect(
            snap(RectF{body.right() - margin_s - w, body.y + margin_s + (travel * t), w, thumb_h}),
            0.5F * w, pal.scrollbar);
    }
    r.set_clip(std::nullopt);
    if (f.marked) {
        r.stroke_rounded_rect(panel, rad, hair(p), pal.accent.with_alpha(0.62F));
    } else {
        r.stroke_rounded_rect(panel, rad, hair(p), pal.border);
    }

    // Status line.
    {
        const float sm = std::round(p.px(dm.panel_margin + 8.0F));
        const float sy = status.center_y();
        std::string left_text = m.loaded() ? tr_count("trash.items", m.count()) : std::string();
        if (m.orphans() > 0) {
            left_text += "  \xC2\xB7  " + tr_count("trash.orphans", m.orphans());
        }
        const ShapedText lt = p.shape(type::status, left_text);
        p.draw(lt, status.x + sm, std::round(sy + (0.5F * lt.cap_height())), pal.text_secondary);
        if (m.selected_count() > 0) {
            const ShapedText st =
                p.shape(type::status, tr_count("status.selected", m.selected_count()));
            p.draw(st, status.right() - sm - st.width(), std::round(sy + (0.5F * st.cap_height())),
                   pal.text_primary);
        }
    }
    if (ptr.released) {
        active_ = WidgetId::None;
    }
}

} // namespace navigator::ui

// src/ui/sidebar/sidebar_view.cpp — see sidebar_view.hpp.
#include "sidebar/sidebar_view.hpp"

#include <algorithm>
#include <cmath>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// Anatomy (dp).
constexpr float kInsetX = 8.0F;   // row highlight inset from the sidebar edges
constexpr float kRowPadX = 10.0F; // highlight edge -> icon
constexpr float kIcon = 16.0F;
constexpr float kIconGap = 10.0F;    // icon -> label
constexpr float kSectionGap = 10.0F; // space above a header (not the first)
constexpr float kUsageBar = 4.0F;
constexpr float kIconRow = 36.0F; // icon-only mode
constexpr float kDragThreshold = 4.0F;

Color faded(const Color& c, float a) { return c.with_alpha(c.a * a); }

LineIcon icon_of(const SidebarRow& row) {
    switch (row.kind) {
    case SidebarRowKind::Bookmark:
        return row.status == places::LocationStatus::Offline ? LineIcon::Unplugged
                                                             : LineIcon::Folder;
    case SidebarRowKind::Recent:
        return row.status == places::LocationStatus::Offline ? LineIcon::Unplugged
                                                             : LineIcon::Clock;
    case SidebarRowKind::Volume:
        return row.network     ? LineIcon::Network
               : row.removable ? LineIcon::Removable
                               : LineIcon::Drive;
    case SidebarRowKind::Place:
        switch (row.place) {
        case places::PlaceKind::Home: return LineIcon::Home;
        case places::PlaceKind::Desktop: return LineIcon::Desktop;
        case places::PlaceKind::Documents: return LineIcon::Documents;
        case places::PlaceKind::Downloads: return LineIcon::Downloads;
        case places::PlaceKind::Music: return LineIcon::Music;
        case places::PlaceKind::Pictures: return LineIcon::Pictures;
        case places::PlaceKind::Videos: return LineIcon::Videos;
        case places::PlaceKind::Trash: return LineIcon::Folder;
        case places::PlaceKind::FileSystem: return LineIcon::Drive;
        }
        return LineIcon::Folder;
    case SidebarRowKind::Header:
    case SidebarRowKind::Empty: break;
    }
    return LineIcon::Folder;
}

// Usage bar fill: accent, amber from 80 %, danger from 90 % (design review Phase 4).
Color usage_color(const Palette& pal, float frac) {
    return frac >= 0.9F ? pal.danger : frac >= 0.8F ? pal.warning : pal.accent;
}

// First code point of a label (icon-only bookmark monogram).
std::string first_code_point(std::string_view s) {
    std::size_t n = s.empty() ? 0 : 1;
    while (n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0U) == 0x80U) {
        ++n;
    }
    return std::string(s.substr(0, n));
}

constexpr TextStyle kMonogram{9.0F, FontWeight::SemiBold, 0.0F, false};

// Percent text for a usage fraction (df rounding).
std::string percent(double fraction) {
    return std::to_string(static_cast<int>(std::lround(100.0 * fraction))) + "%";
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Rename
// ---------------------------------------------------------------------------------------------

void SidebarView::begin_rename(places::BookmarkId id, std::string_view current) {
    rename_.emplace();
    rename_->id = id;
    rename_->edit.set(current, true);
}

bool SidebarView::animating() const noexcept {
    return resizing_ || (drag_row_.has_value() && drag_moved_);
}

std::optional<RectF> SidebarView::row_rect(std::size_t row) const {
    for (const Placed& p : placed_) {
        if (p.row == row) {
            return p.rect;
        }
    }
    return std::nullopt;
}

float SidebarView::row_height(const SidebarFrame& f, const SidebarRow& row) const {
    const bool comfy = f.density == Density::Comfortable;
    switch (row.kind) {
    case SidebarRowKind::Header: return comfy ? 30.0F : 28.0F;
    case SidebarRowKind::Empty: return 54.0F;
    case SidebarRowKind::Volume:
    case SidebarRowKind::Place:
        if (row.usage.has_value()) {
            return comfy ? 44.0F : 40.0F;
        }
        return comfy ? 30.0F : 28.0F;
    case SidebarRowKind::Bookmark:
    case SidebarRowKind::Recent: return comfy ? 30.0F : 28.0F;
    }
    return 28.0F;
}

// ---------------------------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------------------------

bool SidebarView::button(const SidebarFrame& f, const RectF& rect, std::size_t index) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const PointerInput& ptr = f.pointer;
    const bool inside = !f.popup_open && contains(rect, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::SidebarButton;
        active_index_ = index;
    }
    const bool mine = active_ == WidgetId::SidebarButton && active_index_ == index;
    if (mine && inside && ptr.down) {
        r.fill_rounded_rect(rect, p.px(radius::sm + 1.0F), f.palette.control_pressed);
    } else if (inside && (active_ == WidgetId::None || mine)) {
        r.fill_rounded_rect(rect, p.px(radius::sm + 1.0F), f.palette.control_hover);
    }
    return mine && inside && ptr.released;
}

void SidebarView::draw_top(const SidebarFrame& f, const RectF& top) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const float b = std::round(p.px(26.0F));
    const float cy = top.y + std::round(p.px(5.0F)) + std::round(0.5F * (top.h - p.px(8.0F)));
    const float icon = std::round(p.px(16.0F));
    const float t = std::max(1.0F, p.px(1.35F));
    // Hide the sidebar (view.toggle_sidebar).
    const float left = f.icon_only ? std::round(top.center_x() - (0.5F * b))
                                   : top.x + std::round(p.px(kInsetX + 2.0F));
    const RectF hide = snap(RectF{left, std::round(cy - (0.5F * b)), b, b});
    if (button(f, hide, 0)) {
        f.queue.enqueue(CommandId::ToggleSidebar);
    }
    const bool hot = contains(hide, ptr.x, ptr.y) && !f.popup_open;
    draw_line_icon(r, LineIcon::Sidebar, std::round(hide.center_x() - (0.5F * icon)),
                   std::round(hide.center_y() - (0.5F * icon)), icon, t,
                   hot ? pal.text_primary : pal.text_secondary, pal.background);
    if (hot) {
        tooltip_row_ = kNpos - 1; // the hide button's tooltip
        tooltip_anchor_ = hide;
    }
    // Theme switch (design review O9: a fixed window-level home).
    if (!f.icon_only) {
        const RectF theme = snap(RectF{top.right() - std::round(p.px(kInsetX + 2.0F)) - b,
                                       std::round(cy - (0.5F * b)), b, b});
        if (button(f, theme, 1)) {
            f.queue.enqueue(CommandId::ToggleTheme);
        }
        const bool th = contains(theme, ptr.x, ptr.y) && !f.popup_open;
        Color behind = pal.background;
        if (th) {
            const Color o = ptr.down ? pal.control_pressed : pal.control_hover;
            behind = Color{(behind.r * (1.0F - o.a)) + (o.r * o.a),
                           (behind.g * (1.0F - o.a)) + (o.g * o.a),
                           (behind.b * (1.0F - o.a)) + (o.b * o.a), 1.0F};
        }
        draw_theme_glyph(r, theme.center_x(), theme.center_y(), p.px(7.0F), f.theme,
                         th ? pal.text_primary : pal.text_secondary, behind);
    }
    // Keyboard focus marker: the same accent underline as the active pane (design review B1).
    if (f.focused) {
        const float th = std::max(2.0F, std::round(p.px(2.0F)));
        const float m = std::round(p.px(kInsetX));
        r.fill_rounded_rect(RectF{top.x + m, top.bottom() - th, top.w - (2.0F * m), th}, 0.5F * th,
                            pal.accent);
    }
}

void SidebarView::draw_header(const SidebarFrame& f, const SidebarRow& row, std::size_t index,
                              const RectF& rr, bool hover) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool focus = f.focused && f.focus_row == index;
    const float rad = std::round(p.px(radius::md - 2.0F));
    if (hover) {
        r.fill_rounded_rect(rr, rad, pal.control_hover);
    }
    if (focus) {
        r.stroke_rounded_rect(rr, rad, std::max(1.0F, std::round(p.px(1.5F))), pal.focus_ring);
    }
    const float mid = rr.center_y();
    const float x = rr.x + std::round(p.px(kRowPadX));
    // Chevron at the right: down = expanded, right = collapsed.
    const float chev_x = rr.right() - std::round(p.px(14.0F));
    draw_chevron(r, chev_x, mid, p.px(7.0F), row.collapsed ? Direction::Right : Direction::Down,
                 std::max(1.0F, p.px(1.3F)), hover || focus ? pal.text_secondary : pal.text_muted);
    float right = chev_x - std::round(p.px(12.0F));
    // Recents: "Clear" (recents.clear) on hover / focus.
    if (row.section == layout::SidebarSection::Recents && row.count > 0 && (hover || focus)) {
        const ShapedText ct = p.shape(type::caption, tr("sidebar.clear"));
        const float bw = std::round(ct.width() + p.px(12.0F));
        const float bh = std::round(p.px(20.0F));
        const RectF cb = snap(RectF{right - bw, mid - (0.5F * bh), bw, bh});
        const bool over = contains(cb, ptr.x, ptr.y) && !f.popup_open;
        if (over && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::SidebarButton;
            active_index_ = 100;
        }
        if (over) {
            r.fill_rounded_rect(cb, p.px(radius::sm), pal.control_pressed);
        }
        if (over && ptr.released && active_ == WidgetId::SidebarButton && active_index_ == 100) {
            f.queue.enqueue(CommandId::ClearRecents);
        }
        p.draw(ct, cb.x + std::round(p.px(6.0F)), std::round(mid + (0.5F * ct.cap_height())),
               over ? pal.text_primary : pal.text_secondary);
        right = cb.x - std::round(p.px(6.0F));
    }
    std::string label = row.label;
    const ShapedText t = fitter_.fit(p, type::section, label, right - x, Elide::End);
    p.draw(t, x, std::round(mid + (0.5F * t.cap_height())),
           hover || focus ? pal.text_secondary : pal.text_muted);
    if (row.tracking_off && row.section == layout::SidebarSection::Recents) {
        const ShapedText off = p.shape(type::caption, tr("sidebar.paused"));
        const float ox = x + t.width() + std::round(p.px(8.0F));
        if (ox + off.width() < right) {
            p.draw(off, ox, std::round(mid + (0.5F * off.cap_height())), pal.text_muted);
        }
    }
}

void SidebarView::draw_empty(const SidebarFrame& f, const SidebarRow& row, const RectF& rr) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float x = rr.x + std::round(p.px(kRowPadX));
    const float w = rr.right() - x - std::round(p.px(6.0F));
    const ShapedText t = fitter_.fit(p, type::row, row.label, w, Elide::End);
    const float y1 = rr.y + std::round(p.px(18.0F));
    p.draw(t, x, std::round(y1 + (0.5F * t.cap_height())), pal.text_secondary);
    // Second line: Bookmarks "[Ctrl][D] bookmarks this folder"; Recents a plain hint.
    const float y2 = rr.y + std::round(p.px(39.0F));
    if (row.section == layout::SidebarSection::Bookmarks) {
        const std::string pattern(tr("sidebar.bookmarks_hint"));
        const std::size_t at = pattern.find("{key}");
        const std::vector<KeyChord> chords = f.keymap.chords_for("bookmarks.toggle");
        float cx = x;
        const auto text_run = [&](std::string_view s) {
            if (s.empty() || cx >= x + w) {
                return;
            }
            const ShapedText run = fitter_.fit(p, type::caption, s, x + w - cx, Elide::End);
            p.draw(run, cx, std::round(y2 + (0.5F * run.cap_height())), pal.text_muted);
            cx += run.width();
        };
        text_run(std::string_view(pattern).substr(0, std::min(at, pattern.size())));
        if (at != std::string::npos && !chords.empty()) {
            const float h = std::round(p.px(18.0F));
            const float edge = std::max(1.0F, std::round(p.px(1.0F)));
            cx += std::round(p.px(at > 0 ? 4.0F : 0.0F));
            for (const std::string& legend : chord_labels(chords.front())) {
                const ShapedText k = p.shape(type::keycap, legend);
                const float kw = std::round(std::max(h, k.width() + p.px(10.0F)));
                const RectF cap = snap(RectF{cx, y2 - (0.5F * h), kw, h});
                const float rad = p.px(radius::sm);
                r.fill_rounded_rect(RectF{cap.x, cap.y + edge, cap.w, cap.h}, rad, pal.keycap_edge);
                r.fill_rounded_rect(cap, rad, pal.surface_raised);
                r.stroke_rounded_rect(cap, rad, edge, pal.keycap_border);
                p.draw(k, std::round(cap.center_x() - (0.5F * k.width())),
                       std::round(cap.center_y() + (0.5F * k.cap_height())), pal.keycap_text);
                cx += kw + std::round(p.px(3.0F));
            }
            cx += std::round(p.px(3.0F));
            text_run(std::string_view(pattern).substr(at + 5));
        }
    } else {
        const ShapedText h =
            fitter_.fit(p, type::caption,
                        tr(row.tracking_off ? "sidebar.recents_off_hint" : "sidebar.recents_hint"),
                        w, Elide::End);
        p.draw(h, x, std::round(y2 + (0.5F * h.cap_height())), pal.text_muted);
    }
}

void SidebarView::draw_item(const SidebarFrame& f, const SidebarRow& row, std::size_t index,
                            const RectF& rr, bool hover) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const bool focus = f.focused && f.focus_row == index;
    const bool dim = !row.online();
    const float rad = std::round(p.px(radius::md - 2.0F));
    const bool dragged = drag_row_ == index && drag_moved_;
    if (row.highlighted) {
        r.fill_rounded_rect(rr, rad, f.window_focused ? pal.selection : pal.selection_inactive);
        const float mw = std::max(2.0F, std::round(p.px(3.0F)));
        r.fill_rounded_rect(
            snap(RectF{rr.x + p.px(2.0F), rr.y + p.px(6.0F), mw, rr.h - p.px(12.0F)}), 0.5F * mw,
            pal.selection_marker);
    } else if (hover || dragged) {
        r.fill_rounded_rect(rr, rad, pal.control_hover);
    }
    if (focus) {
        r.stroke_rounded_rect(rr, rad, std::max(1.0F, std::round(p.px(1.5F))), pal.focus_ring);
    }
    const bool usage = row.usage.has_value();
    const float line_mid = usage ? rr.y + std::round(p.px(14.0F)) : rr.center_y();
    const float icon = std::round(p.px(kIcon));
    const float x = rr.x + std::round(p.px(kRowPadX));
    const Color icon_ink = row.highlighted ? pal.accent
                           : dim           ? faded(pal.text_muted, 0.85F)
                                           : pal.text_secondary;
    draw_line_icon(r, icon_of(row), x, std::round(line_mid - (0.5F * icon)), icon,
                   std::max(1.0F, p.px(1.35F)), icon_ink, pal.background);
    const float tx = x + icon + std::round(p.px(kIconGap));
    float right = rr.right() - std::round(p.px(kRowPadX));

    // Right side: badges, or the usage percent (free space on hover).
    if (row.status != places::LocationStatus::Online) {
        const bool missing = row.status == places::LocationStatus::Missing;
        const ShapedText bt =
            p.shape(type::caption, tr(missing ? "sidebar.badge_missing" : "sidebar.badge_offline"));
        const float bh = std::round(p.px(18.0F));
        const float bw = std::round(bt.width() + p.px(12.0F));
        const RectF badge = snap(RectF{right - bw, line_mid - (0.5F * bh), bw, bh});
        r.stroke_rounded_rect(badge, 0.5F * bh, std::max(1.0F, std::round(p.px(1.0F))),
                              missing ? pal.danger_border : pal.keycap_border);
        p.draw(bt, badge.x + std::round(p.px(6.0F)),
               std::round(badge.center_y() + (0.5F * bt.cap_height())),
               missing ? pal.danger : pal.text_secondary);
        right = badge.x - std::round(p.px(8.0F));
    } else if (usage) {
        const std::string caption = percent(*row.usage); // free space: tooltip (O5)
        const ShapedText pt = p.shape(type::status, caption);
        p.draw(pt, std::round(right - pt.width()), std::round(line_mid + (0.5F * pt.cap_height())),
               pal.text_muted);
        right -= pt.width() + std::round(p.px(8.0F));
    }

    if (rename_ && row.kind == SidebarRowKind::Bookmark && rename_->id == row.bookmark) {
        const RectF box =
            snap(RectF{tx - p.px(5.0F), rr.y + p.px(3.0F),
                       rr.right() - p.px(4.0F) - (tx - p.px(5.0F)), rr.h - p.px(6.0F)});
        r.fill_rounded_rect(box, p.px(radius::sm), pal.surface);
        r.stroke_rounded_rect(box, p.px(radius::sm), std::max(1.0F, std::round(p.px(1.5F))),
                              pal.focus_ring);
        TextFieldStyle st;
        st.text = &type::row;
        st.caret_height_dp = 15.0F;
        (void)draw_text_field(p, pal, rename_->edit,
                              RectF{tx, box.y, box.right() - p.px(6.0F) - tx, box.h},
                              tr("sidebar.rename_placeholder"), rename_->scroll, st);
    } else {
        const ShapedText label = fitter_.fit(p, type::row, row.label, right - tx, Elide::Middle);
        const Color ink = dim ? pal.text_muted : pal.text_primary;
        p.draw(label, tx, std::round(line_mid + (0.5F * label.cap_height())), ink);
    }

    if (usage) {
        const float bar_h = std::max(2.0F, std::round(p.px(kUsageBar)));
        const float by = std::round(rr.y + p.px(27.0F));
        const RectF track =
            snap(RectF{tx, by, rr.right() - std::round(p.px(kRowPadX)) - tx, bar_h});
        r.fill_rounded_rect(track, 0.5F * bar_h, pal.control_pressed);
        const auto frac = static_cast<float>(std::clamp(*row.usage, 0.0, 1.0));
        if (frac > 0.0F) {
            const float w = std::max(bar_h, std::round(track.w * frac));
            const Color fill = usage_color(pal, frac);
            r.fill_rounded_rect(RectF{track.x, track.y, w, track.h}, 0.5F * bar_h, fill);
        }
    }
}

void SidebarView::draw_icon_row(const SidebarFrame& f, const SidebarRow& row, std::size_t index,
                                const RectF& rr, bool hover) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const float rad = std::round(p.px(radius::md - 1.0F));
    const bool focus = f.focused && f.focus_row == index;
    if (row.highlighted) {
        r.fill_rounded_rect(rr, rad, f.window_focused ? pal.selection : pal.selection_inactive);
        const float mw = std::max(2.0F, std::round(p.px(3.0F)));
        r.fill_rounded_rect(
            snap(RectF{rr.x - p.px(5.0F), rr.y + p.px(9.0F), mw, rr.h - p.px(18.0F)}), 0.5F * mw,
            pal.selection_marker);
    } else if (hover) {
        r.fill_rounded_rect(rr, rad, pal.control_hover);
    }
    if (focus) {
        r.stroke_rounded_rect(rr, rad, std::max(1.0F, std::round(p.px(1.5F))), pal.focus_ring);
    }
    const float icon = std::round(p.px(18.0F));
    const Color ink = row.highlighted ? pal.accent
                      : !row.online() ? pal.text_muted
                      : hover         ? pal.text_primary
                                      : pal.text_secondary;
    draw_line_icon(r, icon_of(row), std::round(rr.center_x() - (0.5F * icon)),
                   std::round(rr.center_y() - (0.5F * icon) - (row.usage ? p.px(2.0F) : 0.0F)),
                   icon, std::max(1.0F, p.px(1.4F)), ink, pal.background);
    const float ix = std::round(rr.center_x() - (0.5F * icon));
    const float iy = std::round(rr.center_y() - (0.5F * icon));
    if (row.kind == SidebarRowKind::Bookmark && row.status != places::LocationStatus::Offline) {
        // Bookmarks share the folder glyph: its first letter tells them apart (O4).
        const ShapedText mono = p.shape(kMonogram, first_code_point(row.label));
        p.draw(mono, std::round(ix + (0.5F * icon) - (0.5F * mono.width())),
               std::round(iy + (icon * 0.56F) + (0.5F * mono.cap_height())), ink);
    }
    if (row.status != places::LocationStatus::Online) {
        // State dot (O4): danger for missing, muted for offline; tooltips carry the words.
        const float dr = std::round(p.px(3.0F));
        const float dx = ix + icon - p.px(1.0F);
        const float dy = iy + p.px(2.0F);
        r.fill_circle(dx, dy, dr + std::max(1.0F, std::round(p.px(1.5F))), pal.background);
        r.fill_circle(dx, dy, dr,
                      row.status == places::LocationStatus::Missing ? pal.danger
                                                                    : pal.text_secondary);
    }
    if (row.usage) {
        // A thin usage bar under the icon.
        const float bw = std::round(p.px(22.0F));
        const float bh = std::max(2.0F, std::round(p.px(2.5F)));
        const RectF track =
            snap(RectF{rr.center_x() - (0.5F * bw), rr.bottom() - p.px(4.0F), bw, bh});
        r.fill_rounded_rect(track, 0.5F * bh, pal.control_pressed);
        const auto frac = static_cast<float>(std::clamp(*row.usage, 0.0, 1.0));
        r.fill_rounded_rect(RectF{track.x, track.y, std::max(bh, track.w * frac), track.h},
                            0.5F * bh, usage_color(pal, frac));
    }
}

// ---------------------------------------------------------------------------------------------
// Resize edge
// ---------------------------------------------------------------------------------------------

void SidebarView::handle_resize(const SidebarFrame& f) {
    const Painter& p = f.painter;
    const PointerInput& ptr = f.pointer;
    const float edge = f.rect.right();
    const float half = p.px(kResizeHit) * 0.5F + p.px(1.0F);
    const RectF hit{edge - half, f.rect.y, 2.0F * half, f.rect.h};
    resize_hot_ = false;
    if (f.icon_only || f.popup_open) {
        resizing_ = false;
        return;
    }
    if (resizing_) {
        if (!ptr.down) {
            resizing_ = false;
            return;
        }
        Intent in;
        in.kind = Intent::Kind::SidebarResize;
        in.position = static_cast<double>((ptr.x - f.rect.x) / p.scale());
        f.intents.push_back(in);
        return;
    }
    resize_hot_ = ptr.inside && contains(hit, ptr.x, ptr.y) && active_ == WidgetId::None &&
                  !f.dragged_tab.valid();
    if (resize_hot_ && ptr.pressed) {
        owns_pointer_ = true;
        if (ptr.clicks >= 2) {
            Intent in;
            in.kind = Intent::Kind::SidebarResize;
            in.position = layout::kSidebarDefaultWidth;
            f.intents.push_back(in);
            active_ = WidgetId::SidebarSplit;
            return;
        }
        resizing_ = true;
        active_ = WidgetId::SidebarSplit;
    }
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void SidebarView::draw(const SidebarFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const DensityMetrics dm = density_metrics(f.density);
    const std::vector<SidebarRow>& rows = f.rows;
    owns_pointer_ = !f.popup_open && contains(f.rect, ptr.x, ptr.y) &&
                    (ptr.pressed || ptr.middle_pressed || ptr.right_pressed);
    tooltip_row_.reset();
    placed_.clear();
    drop_index_.reset();

    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    // The sidebar sits on the window background; a hairline separates it from the panes.
    r.fill_rect(RectF{f.rect.right() - hair, f.rect.y, hair, f.rect.h}, pal.border);

    const RectF top{f.rect.x, f.rect.y, f.rect.w - hair, std::round(p.px(dm.tab_bar_height))};
    draw_top(f, top);
    handle_resize(f);

    const float pad_x = std::round(p.px(f.icon_only ? 8.0F : kInsetX));
    const RectF list{f.rect.x, top.bottom() + std::round(p.px(4.0F)), f.rect.w - hair,
                     std::max(f.rect.bottom() - top.bottom() - p.px(4.0F), 0.0F)};
    const bool over_list = !f.popup_open && ptr.inside && contains(list, ptr.x, ptr.y);

    // Layout pass (dp heights -> px), then scroll.
    struct Slot {
        std::size_t row;
        float y; // content px
        float h;
        bool clock = false; // icon-only Recents button
        bool separator = false;
    };
    std::vector<Slot> slots;
    slots.reserve(rows.size());
    float y = 0.0F;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const SidebarRow& row = rows[i];
        if (f.icon_only) {
            if (row.kind == SidebarRowKind::Header) {
                const float sep = std::round(p.px(i == 0 ? 2.0F : 13.0F));
                if (i != 0) {
                    slots.push_back(Slot{i, y, sep, false, true});
                }
                y += sep;
                if (row.section == layout::SidebarSection::Recents) {
                    const float h = std::round(p.px(kIconRow));
                    slots.push_back(Slot{i, y, h, true, false});
                    y += h;
                }
                continue;
            }
            if (row.kind == SidebarRowKind::Empty || row.kind == SidebarRowKind::Recent) {
                continue;
            }
            const float h = std::round(p.px(kIconRow));
            slots.push_back(Slot{i, y, h});
            y += h;
            continue;
        }
        if (row.kind == SidebarRowKind::Header && i != 0) {
            y += std::round(p.px(kSectionGap));
        }
        const float h = std::round(p.px(row_height(f, row)));
        slots.push_back(Slot{i, y, h});
        y += h;
    }
    content_h_ = y + std::round(p.px(8.0F));
    const float max_scroll = std::max(content_h_ - list.h, 0.0F);
    if (over_list && ptr.wheel_y != 0.0F) {
        scroll_ -= ptr.wheel_y * p.px(48.0F);
    }
    if (reveal_ != kNpos) {
        for (const Slot& s : slots) {
            if (s.row == reveal_ && !s.separator) {
                if (s.y < scroll_) {
                    scroll_ = s.y;
                } else if (s.y + s.h > scroll_ + list.h) {
                    scroll_ = s.y + s.h - list.h;
                }
            }
        }
        reveal_ = kNpos;
    }
    scroll_ = std::clamp(scroll_, 0.0F, max_scroll);

    // Interaction + drawing.
    r.set_clip(list);
    const float base = std::round(list.y - scroll_);
    std::optional<std::size_t> hovered;
    for (const Slot& s : slots) {
        if (s.separator) {
            continue;
        }
        const RectF rr = snap(RectF{list.x + pad_x, base + s.y, list.w - (2.0F * pad_x), s.h});
        if (contains(rr, ptr.x, ptr.y) && over_list) {
            hovered = s.row;
        }
    }
    if (hover_override_ != kNpos) {
        hovered = hover_override_;
    }
    // Presses.
    const bool can_press = active_ == WidgetId::None && !resize_hot_ && !resizing_;
    if (hovered && over_list && can_press && !f.dragged_tab.valid()) {
        const std::size_t i = *hovered;
        const SidebarRow& row = rows[i];
        if (ptr.pressed && row.focusable()) {
            pressed_row_ = i;
            active_ = WidgetId::Sidebar;
            active_index_ = i;
            press_x_ = ptr.x;
            press_y_ = ptr.y;
            drag_row_.reset();
            drag_moved_ = false;
            if (row.kind == SidebarRowKind::Bookmark && !f.icon_only && !rename_) {
                drag_row_ = i;
            }
        }
        if (ptr.middle_pressed && row.item()) {
            middle_row_ = i;
        }
        if (ptr.right_pressed && (row.item() || row.kind == SidebarRowKind::Header)) {
            Intent in;
            in.kind = Intent::Kind::SidebarMenu;
            in.row = i;
            in.x = ptr.x;
            in.y = ptr.y;
            f.intents.push_back(in);
        }
    }
    // Bookmark drag (reorder): find the insertion slot.
    const auto bookmark_slot = [&](float py, std::size_t& index, float& marker_y) {
        index = 0;
        marker_y = -1.0F;
        bool any = false;
        for (const Slot& s : slots) {
            const SidebarRow& row = rows[s.row];
            if (row.section != layout::SidebarSection::Bookmarks || s.separator || s.clock) {
                continue;
            }
            if (row.kind == SidebarRowKind::Header) {
                marker_y = base + s.y + s.h;
                continue;
            }
            if (row.kind != SidebarRowKind::Bookmark) {
                continue;
            }
            const float top_y = base + s.y;
            if (!any) {
                marker_y = top_y;
            }
            any = true;
            if (py >= top_y + (0.5F * s.h)) {
                ++index;
                marker_y = top_y + s.h;
            }
        }
    };
    if (drag_row_ && active_ == WidgetId::Sidebar && ptr.down) {
        if (!drag_moved_ &&
            std::abs(ptr.y - press_y_) + std::abs(ptr.x - press_x_) > p.px(kDragThreshold)) {
            drag_moved_ = true;
        }
        if (drag_moved_) {
            std::size_t idx = 0;
            float my = 0.0F;
            bookmark_slot(ptr.y, idx, my);
            drop_index_ = idx;
            drop_y_ = my;
        }
    }
    // A tab dragged over the Bookmarks section: insertion marker, drop on release.
    if (f.dragged_tab.valid() && ptr.inside && contains(list, ptr.x, ptr.y) && !f.icon_only) {
        const SidebarRow* over = hovered ? &rows[*hovered] : nullptr;
        if (over != nullptr && over->section == layout::SidebarSection::Bookmarks) {
            std::size_t idx = 0;
            float my = 0.0F;
            bookmark_slot(ptr.y, idx, my);
            drop_index_ = idx;
            drop_y_ = my;
            if (ptr.released) {
                Intent in;
                in.kind = Intent::Kind::BookmarkTab;
                in.tab = f.dragged_tab.value;
                in.row = idx;
                f.intents.push_back(in);
            }
        }
    }

    for (const Slot& s : slots) {
        const RectF rr = snap(RectF{list.x + pad_x, base + s.y, list.w - (2.0F * pad_x), s.h});
        if (rr.bottom() < list.y || rr.y > list.bottom()) {
            continue; // virtualized: off screen
        }
        if (s.separator) {
            r.fill_rect(
                RectF{rr.x + p.px(6.0F), std::round(rr.center_y()), rr.w - p.px(12.0F), hair},
                pal.border);
            continue;
        }
        const SidebarRow& row = rows[s.row];
        placed_.push_back(Placed{s.row, rr});
        const bool hover =
            hovered == s.row &&
            (active_ == WidgetId::None || (active_ == WidgetId::Sidebar && active_index_ == s.row));
        if (f.icon_only) {
            if (s.clock) {
                SidebarRow clock = row;
                clock.kind = SidebarRowKind::Recent;
                clock.status = places::LocationStatus::Online;
                clock.highlighted = false;
                draw_icon_row(f, clock, s.row, rr, hover);
            } else {
                draw_icon_row(f, row, s.row, rr, hover);
            }
            if (hover) {
                tooltip_row_ = s.row;
                tooltip_anchor_ = rr;
            }
        } else if (row.kind == SidebarRowKind::Header) {
            draw_header(f, row, s.row, rr, hover);
        } else if (row.kind == SidebarRowKind::Empty) {
            draw_empty(f, row, rr);
        } else {
            draw_item(f, row, s.row, rr, hover);
            // Free space and the full share name live in a tooltip (O5), not in the row.
            if (hover && (row.usage || row.network) && hover_override_ == kNpos) {
                tooltip_row_ = s.row;
                tooltip_anchor_ = rr;
            }
        }
        // Releases.
        const bool inside = contains(rr, ptr.x, ptr.y) && over_list;
        if (ptr.released && inside && active_ == WidgetId::Sidebar && active_index_ == s.row &&
            !(drag_row_ && drag_moved_)) {
            if (s.clock) {
                f.queue.enqueue(CommandId::GoTo);
            } else if (row.kind == SidebarRowKind::Header) {
                Intent in;
                in.kind = Intent::Kind::SidebarToggleSection;
                in.index = static_cast<std::size_t>(row.section);
                f.intents.push_back(in);
            } else if (row.item() && (!rename_ || rename_->id != row.bookmark ||
                                      row.kind != SidebarRowKind::Bookmark)) {
                Intent in;
                in.kind = Intent::Kind::SidebarOpen;
                in.row = s.row;
                in.additive = has(ptr.mods, Modifiers::Ctrl);
                f.intents.push_back(in);
            }
        }
        if (ptr.middle_released && inside && middle_row_ == s.row && row.item()) {
            Intent in;
            in.kind = Intent::Kind::SidebarOpen;
            in.row = s.row;
            in.additive = true;
            f.intents.push_back(in);
        }
    }
    // Drop marker (bookmark reorder or a tab dropped onto Bookmarks).
    if (drop_index_ && drop_y_ >= 0.0F) {
        const float th = std::max(2.0F, std::round(p.px(2.0F)));
        const float x0 = list.x + pad_x + p.px(4.0F);
        const float x1 = list.right() - pad_x - p.px(4.0F);
        r.fill_rounded_rect(snap(RectF{x0, drop_y_ - (0.5F * th), x1 - x0, th}), 0.5F * th,
                            pal.accent);
        r.fill_circle(x0, drop_y_, p.px(3.0F), pal.accent);
    }
    r.set_clip(std::nullopt);

    // Scroll affordance (design review O3): a shadow under the top bar once scrolled, a fade
    // into the background where more rows wait below, and a thin overlay scrollbar on hover.
    if (scroll_ > 0.5F) {
        r.fill_rect_vgradient(RectF{list.x, list.y, list.w, std::round(p.px(8.0F))},
                              faded(pal.shadow, 0.6F), pal.shadow.with_alpha(0.0F));
    }
    if (scroll_ < max_scroll - 0.5F) {
        const float fh = std::round(p.px(28.0F));
        r.fill_rect_vgradient(RectF{list.x, list.bottom() - fh, list.w, fh},
                              pal.background.with_alpha(0.0F), pal.background);
    }
    if (max_scroll > 0.5F && (over_list || hover_override_ != kNpos)) {
        const float tw = std::max(2.0F, std::round(p.px(metrics::scrollbar_width)));
        const float track_h = list.h - p.px(8.0F);
        const float th = std::max(p.px(24.0F), track_h * (list.h / content_h_));
        const float ty = list.y + p.px(4.0F) + ((track_h - th) * (scroll_ / max_scroll));
        r.fill_rounded_rect(snap(RectF{list.right() - tw - p.px(3.0F), ty, tw, th}), 0.5F * tw,
                            pal.scrollbar);
    }

    // Bookmark drag finished.
    if (ptr.released && drag_row_) {
        if (drag_moved_ && drop_index_ && *drag_row_ < rows.size()) {
            const SidebarRow& row = rows[*drag_row_];
            const std::size_t from = sidebar_bookmark_index(rows, *drag_row_);
            std::size_t to = *drop_index_;
            if (to > from) {
                --to;
            }
            if (to != from) {
                Intent in;
                in.kind = Intent::Kind::MoveBookmark;
                in.index = row.bookmark.value;
                in.row = to;
                f.intents.push_back(in);
            }
        }
        drag_row_.reset();
        drag_moved_ = false;
    }
    if (ptr.released || (!ptr.down && active_ != WidgetId::None)) {
        active_ = WidgetId::None;
        pressed_row_.reset();
    }
    if (ptr.middle_released) {
        middle_row_.reset();
    }
}

void SidebarView::draw_overlay(const SidebarFrame& f) {
    if (!tooltip_row_ || f.popup_open || dragging()) {
        return;
    }
    // Full mode: only storage details (free space, the share's source). Icon-only: labels.
    if (!f.icon_only && (*tooltip_row_ >= f.rows.size() ||
                         (!f.rows[*tooltip_row_].usage && !f.rows[*tooltip_row_].network))) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    std::string text;
    std::string detail;
    if (*tooltip_row_ == kNpos - 1) {
        text = std::string(tr("cmd.view.toggle_sidebar"));
    } else if (*tooltip_row_ < f.rows.size()) {
        const SidebarRow& row = f.rows[*tooltip_row_];
        if (row.kind == SidebarRowKind::Header) {
            text = row.label; // the Recents clock button
        } else {
            text = row.label;
            if (row.status == places::LocationStatus::Offline) {
                detail = std::string(tr("sidebar.badge_offline"));
            } else if (row.status == places::LocationStatus::Missing) {
                detail = std::string(tr("sidebar.badge_missing"));
            } else if (row.usage) {
                detail = f.icon_only ? percent(*row.usage) + "  \xC2\xB7  " : std::string();
                detail += substitute(
                    substitute(tr("sidebar.free_of"), "free", format_size(row.available_bytes)),
                    "total", format_size(row.total_bytes));
            }
            if (!f.icon_only && row.network && !row.source.empty()) {
                text = row.source; // the full share ("nas.local:/export/media")
            }
        }
    }
    if (text.empty()) {
        return;
    }
    const ShapedText t = p.shape(type::row, text);
    const ShapedText d = p.shape(type::caption, detail);
    const float h = std::round(p.px(28.0F));
    const float w =
        std::round(t.width() + (detail.empty() ? 0.0F : d.width() + p.px(10.0F)) + p.px(20.0F));
    const float x = std::round(f.rect.right() + p.px(6.0F));
    const float y = std::round(tooltip_anchor_.center_y() - (0.5F * h));
    const RectF box = snap(RectF{x, y, w, h});
    const float rad = std::round(p.px(radius::md - 1.0F));
    r.drop_shadow(box, rad, p.px(12.0F), p.px(3.0F), pal.shadow);
    r.fill_rounded_rect(box, rad, pal.surface_raised);
    r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    const float baseline = std::round(box.center_y() + (0.5F * t.cap_height()));
    p.draw(t, box.x + std::round(p.px(10.0F)), baseline, pal.text_primary);
    if (!detail.empty()) {
        p.draw(d, box.x + std::round(p.px(10.0F)) + t.width() + std::round(p.px(10.0F)), baseline,
               pal.text_muted);
    }
}

} // namespace navigator::ui

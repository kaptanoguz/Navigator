// src/ui/browser/browser_chrome.cpp — top bar, breadcrumbs, notice banner, empty states, status
// bar, key hints and the FPS overlay of BrowserView (see browser_view.hpp).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include <navigator/i18n.hpp>

#include "browser/browser_view.hpp"
#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "input/sdl_input.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

constexpr int kGlyphBack = 0;
constexpr int kGlyphForward = 1;
constexpr int kGlyphUp = 2;
constexpr int kGlyphTheme = 3;

struct CrumbPart {
    std::string label;
    Path path;
};

// True when `base` is a component-wise prefix of `p`.
bool has_prefix(const Path& p, const Path& base) {
    auto pi = p.begin();
    for (auto bi = base.begin(); bi != base.end(); ++bi, ++pi) {
        if (bi->empty()) {
            continue; // trailing separator
        }
        if (pi == p.end() || *pi != *bi) {
            return false;
        }
    }
    return true;
}

std::vector<CrumbPart> crumb_parts(const Path& dir, const Path* home) {
    std::vector<CrumbPart> out;
    Path base;
    Path rest;
    if (home != nullptr && !home->empty() && home->is_absolute() && *home != home->root_path() &&
        has_prefix(dir, *home)) {
        base = *home;
        out.push_back({std::string(tr("path.home")), base});
        rest = dir.lexically_relative(*home);
    } else {
        base = dir.root_path().empty() ? Path("/") : dir.root_path();
        out.push_back({std::string(tr("path.root")), base});
        rest = dir.relative_path();
    }
    Path walk = base;
    for (const Path& part : rest) {
        if (part.empty() || part == ".") {
            continue;
        }
        walk /= part;
        out.push_back({display_name(part.string()), walk});
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Top bar
// ---------------------------------------------------------------------------------------------

bool BrowserView::tool_button(const BrowserFrame& f, const RectF& rect, WidgetId id, bool enabled,
                              int glyph) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(rect, ptr.x, ptr.y);
    const bool hover = enabled && inside && (active_ == WidgetId::None || active_ == id);
    if (enabled && inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = id;
    }
    const bool pressed = active_ == id && inside;
    bool fired = false;
    if (active_ == id && ptr.released && inside && enabled) {
        fired = true;
    }
    const float rad = p.px(radius::md);
    Color bg{0.0F, 0.0F, 0.0F, 0.0F};
    if (pressed) {
        bg = pal.control_pressed;
        r.fill_rounded_rect(rect, rad, bg);
    } else if (hover) {
        bg = pal.control_hover;
        r.fill_rounded_rect(rect, rad, bg);
    }
    const Color ink = !enabled ? pal.text_muted.with_alpha(0.45F)
                      : hover  ? pal.text_primary
                               : pal.text_secondary;
    const float cx = rect.x + (0.5F * rect.w);
    const float cy = rect.y + (0.5F * rect.h);
    const float t = std::max(1.0F, p.px(1.6F));
    switch (glyph) {
    case kGlyphBack: draw_chevron(r, cx, cy, p.px(11.0F), Direction::Left, t, ink); break;
    case kGlyphForward: draw_chevron(r, cx, cy, p.px(11.0F), Direction::Right, t, ink); break;
    case kGlyphUp: draw_chevron(r, cx, cy, p.px(11.0F), Direction::Up, t, ink); break;
    case kGlyphTheme: {
        // The moon's bite is drawn in the colour behind the glyph.
        const Color behind =
            bg.a > 0.0F ? Color{pal.background.r + ((bg.r - pal.background.r) * bg.a),
                                pal.background.g + ((bg.g - pal.background.g) * bg.a),
                                pal.background.b + ((bg.b - pal.background.b) * bg.a), 1.0F}
                        : pal.background;
        draw_theme_glyph(r, cx, cy, p.px(7.5F), f.theme, ink, behind);
        break;
    }
    default: break;
    }
    return fired;
}

void BrowserView::draw_top_bar(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    const BrowserModel& m = f.model;
    const DensityMetrics dm = density_metrics(f.density);
    const float margin = std::round(p.px(dm.panel_margin));
    const float b = std::round(p.px(dm.tool_button));
    const float y = std::round(l.top.y + (0.5F * (l.top.h - b)));
    const float gap = std::round(p.px(2.0F));

    float x = l.top.x + margin;
    if (tool_button(f, RectF{x, y, b, b}, WidgetId::Back, m.can_back(), kGlyphBack)) {
        f.queue.enqueue(CommandId::NavigateBack);
    }
    x += b + gap;
    if (tool_button(f, RectF{x, y, b, b}, WidgetId::Forward, m.can_forward(), kGlyphForward)) {
        f.queue.enqueue(CommandId::NavigateForward);
    }
    x += b + gap;
    if (tool_button(f, RectF{x, y, b, b}, WidgetId::Up, m.can_up(), kGlyphUp)) {
        f.queue.enqueue(CommandId::NavigateUp);
    }
    x += b + std::round(p.px(10.0F));

    // Right end: the bookmark star (bookmarks.toggle).
    const float icon = std::round(p.px(16.0F));
    const float t = std::max(1.0F, p.px(1.35F));
    float right = l.top.right() - margin;
    const RectF star{right - b, y, b, b};
    {
        const PointerInput& ptr = f.pointer;
        const bool inside = contains(star, ptr.x, ptr.y);
        if (inside && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::Star;
        }
        if (active_ == WidgetId::Star && ptr.released && inside) {
            f.queue.enqueue(CommandId::ToggleBookmark);
        }
        Renderer& r = p.renderer();
        if (inside && (active_ == WidgetId::None || active_ == WidgetId::Star)) {
            r.fill_rounded_rect(star, p.px(radius::md),
                                active_ == WidgetId::Star ? f.palette.control_pressed
                                                          : f.palette.control_hover);
        }
        const Color ink = f.bookmarked ? f.palette.accent
                          : inside     ? f.palette.text_primary
                                       : f.palette.text_secondary;
        draw_line_icon(r, LineIcon::Star, std::round(star.center_x() - (0.5F * icon)),
                       std::round(star.center_y() - (0.5F * icon)), icon, t, ink,
                       f.palette.background, f.bookmarked);
    }
    // The view lock (view.toggle_view_lock) stays hidden until it does something (design
    // review Phase 4 O11).
    right = star.x - std::round(p.px(6.0F));

    const RectF area{x, y, std::max(right - x, 0.0F), b};
    if (f.address != nullptr) {
        draw_address_field(f, area);
    } else {
        address_rect_ = area;
        draw_breadcrumbs(f, area);
    }
}

void BrowserView::draw_address_field(const BrowserFrame& f, const RectF& area) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const RectF box = snap(RectF{area.x - p.px(4.0F), area.y, area.w + p.px(4.0F), area.h});
    address_rect_ = box;
    const float rad = std::round(p.px(radius::md - 1.0F));
    r.fill_rounded_rect(box, rad, pal.surface);
    r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(p.px(1.5F))),
                          f.address_error ? pal.danger : pal.focus_ring);
    TextFieldStyle st;
    st.text = &type::crumb;
    st.caret_height_dp = 16.0F;
    const float pad = std::round(p.px(10.0F));
    (void)draw_text_field(p, pal, *f.address,
                          RectF{box.x + pad, box.y, box.w - (2.0F * pad), box.h}, {},
                          address_scroll_, st);
}

void BrowserView::draw_breadcrumbs(const BrowserFrame& f, const RectF& area) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    if (area.w <= 0.0F || f.model.current_dir().empty()) {
        return;
    }
    const std::vector<CrumbPart> parts = crumb_parts(f.model.current_dir(), f.home);
    const float pad = std::round(p.px(8.0F));
    const float sep = std::round(p.px(14.0F)); // chevron slot between segments

    struct Seg {
        ShapedText text;
        std::size_t part; // index into parts; npos for the "…" segment
        float w;
    };
    std::vector<Seg> segs;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool current = i + 1 == parts.size();
        const TextStyle& current_style = f.pane_active ? type::crumb_current : type::crumb_quiet;
        ShapedText t = p.shape(current ? current_style : type::crumb, parts[i].label);
        segs.push_back(Seg{t, i, t.width() + (2.0F * pad)});
    }
    const auto total = [&] {
        float w = 0.0F;
        for (const Seg& s : segs) {
            w += s.w;
        }
        return w + (sep * static_cast<float>(segs.size() - 1));
    };
    // Collapse from the left (keeping the root) into one "…" until it fits.
    bool collapsed = false;
    while (total() > area.w && segs.size() > 2) {
        const std::size_t drop = collapsed ? 2 : 1;
        if (segs.size() <= drop + 1) {
            break;
        }
        segs.erase(segs.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!collapsed) {
            ShapedText dots = p.shape(type::crumb, "\xE2\x80\xA6");
            segs.insert(segs.begin() + 1,
                        Seg{dots, static_cast<std::size_t>(-1), dots.width() + (2.0F * pad)});
            collapsed = true;
        }
    }
    if (total() > area.w && !segs.empty()) {
        // Still too wide: elide the current folder's name.
        Seg& last = segs.back();
        const float others = total() - last.w;
        const float room = std::max(area.w - others - (2.0F * pad), p.px(24.0F));
        last.text = fitter_.fit(p, f.pane_active ? type::crumb_current : type::crumb_quiet,
                                parts[last.part].label, room, Elide::End);
        last.w = last.text.width() + (2.0F * pad);
    }

    float x = area.x;
    const float mid = area.y + (0.5F * area.h);
    const float h = std::round(p.px(28.0F));
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const Seg& s = segs[i];
        const bool current = i + 1 == segs.size();
        const bool clickable = !current && s.part != static_cast<std::size_t>(-1);
        const RectF hit = snap(RectF{x, mid - (0.5F * h), s.w, h});
        const bool inside = contains(hit, ptr.x, ptr.y) && contains(area, ptr.x, ptr.y);
        if (clickable && inside && ptr.pressed && active_ == WidgetId::None) {
            active_ = WidgetId::Crumb;
            active_index_ = s.part;
        }
        const bool pressed = active_ == WidgetId::Crumb && active_index_ == s.part && inside;
        const bool hover = clickable && inside && active_ == WidgetId::None;
        if (active_ == WidgetId::Crumb && active_index_ == s.part && ptr.released && inside) {
            Intent in;
            in.kind = Intent::Kind::NavigateTo;
            in.path = parts[s.part].path;
            f.intents.push_back(in);
        }
        // Middle-click: that folder in a new background tab (docs/keymap.md, Phase 4 mouse).
        if (s.part != static_cast<std::size_t>(-1) && inside && ptr.middle_pressed) {
            middle_crumb_ = s.part;
        }
        if (s.part != static_cast<std::size_t>(-1) && inside && ptr.middle_released &&
            middle_crumb_ == s.part) {
            Intent in;
            in.kind = Intent::Kind::OpenPath;
            in.path = parts[s.part].path;
            in.additive = true;
            f.intents.push_back(in);
        }
        if (pressed || hover) {
            r.fill_rounded_rect(hit, p.px(radius::sm + 1.0F),
                                pressed ? pal.control_pressed : pal.control_hover);
        }
        const float baseline = std::round(mid + (0.5F * s.text.cap_height()));
        const Color ink = current ? (f.pane_active ? pal.text_primary : pal.text_secondary)
                                  : (hover ? pal.text_primary : pal.text_secondary);
        p.draw(s.text, x + pad, baseline, ink);
        x += s.w;
        if (!current) {
            draw_chevron(r, x + (0.5F * sep), mid, p.px(7.0F), Direction::Right,
                         std::max(1.0F, p.px(1.2F)), pal.text_muted);
            x += sep;
        }
    }
    // Phase 4: a click on the empty part of the bar edits the path (nav.focus_address_bar).
    const RectF empty{x, mid - (0.5F * h), std::max(area.right() - x, 0.0F), h};
    const bool in_empty = empty.w > 0.0F && contains(empty, ptr.x, ptr.y);
    if (in_empty && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::AddressField;
    }
    if (active_ == WidgetId::AddressField && ptr.released && in_empty) {
        f.queue.enqueue(CommandId::FocusAddressBar);
    }
}

// ---------------------------------------------------------------------------------------------
// Notice banner
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_notice(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const std::optional<Notice>& notice = f.model.notice();
    if (!notice) {
        return;
    }
    const Notice& n = *notice;
    const RectF box = snap(l.notice);
    const float rad = p.px(radius::md);
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rounded_rect(box, rad, pal.danger_surface);
    r.stroke_rounded_rect(box, rad, hair, pal.danger_border);

    const float mid = box.y + (0.5F * box.h);
    const float icon_r = p.px(8.0F);
    const float icon_x = box.x + p.px(14.0F) + icon_r;
    draw_alert_glyph(r, icon_x, mid, icon_r, pal.danger, pal.danger_surface, p.scale());

    std::string_view title_key = "notice.open_folder_failed";
    switch (n.kind) {
    case NoticeKind::OpenFolderFailed: title_key = "notice.open_folder_failed"; break;
    case NoticeKind::OpenFileFailed: title_key = "notice.open_file_failed"; break;
    case NoticeKind::RefreshFailed: title_key = "notice.refresh_failed"; break;
    case NoticeKind::FolderGone: title_key = "notice.folder_gone"; break;
    }
    std::string name = display_name(n.path.filename().string());
    if (name.empty()) {
        name = display_name(n.path.string());
    }
    const std::string title = substitute(tr(title_key), "name", name);
    const std::string detail = n.kind == NoticeKind::FolderGone
                                   ? std::string(tr("notice.folder_gone_detail"))
                                   : std::string(tr("error." + std::string(to_string(n.code))));

    // Dismiss button at the right.
    const float b = std::round(p.px(28.0F));
    const RectF close = snap(RectF{box.right() - p.px(6.0F) - b, mid - (0.5F * b), b, b});
    const PointerInput& ptr = f.pointer;
    const bool inside = contains(close, ptr.x, ptr.y);
    if (inside && ptr.pressed && active_ == WidgetId::None) {
        active_ = WidgetId::DismissNotice;
    }
    if (active_ == WidgetId::DismissNotice && ptr.released && inside) {
        Intent in;
        in.kind = Intent::Kind::DismissNotice;
        f.intents.push_back(in);
    }
    if (inside && (active_ == WidgetId::None || active_ == WidgetId::DismissNotice)) {
        r.fill_rounded_rect(close, p.px(radius::sm + 1.0F),
                            active_ == WidgetId::DismissNotice ? pal.control_pressed
                                                               : pal.control_hover);
    }
    draw_cross(r, close.x + (0.5F * close.w), mid, p.px(4.5F), std::max(1.0F, p.px(1.4F)),
               inside ? pal.text_primary : pal.text_secondary);

    const float text_x = icon_x + icon_r + p.px(10.0F);
    const float room = close.x - p.px(12.0F) - text_x;
    const ShapedText t = fitter_.fit(p, type::notice_title, title, room * 0.6F, Elide::Middle);
    const float baseline = std::round(mid + (0.5F * t.cap_height()));
    p.draw(t, text_x, baseline, pal.text_primary);
    const float dx = text_x + t.width() + p.px(10.0F);
    const ShapedText d = fitter_.fit(p, type::row, detail, close.x - p.px(12.0F) - dx, Elide::End);
    p.draw(d, dx, baseline, pal.text_secondary);
}

// ---------------------------------------------------------------------------------------------
// Empty states and key hints
// ---------------------------------------------------------------------------------------------

float BrowserView::draw_key_hint(const BrowserFrame& f, std::string_view command,
                                 std::string_view label, float center_x, float center_y,
                                 bool measure_only) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const std::vector<KeyChord> chords = f.keymap.chords_for(command);
    std::vector<ShapedText> keys;
    if (!chords.empty()) {
        for (const std::string& legend : chord_labels(chords.front())) {
            keys.push_back(p.shape(type::keycap, legend));
        }
    }
    const ShapedText text = p.shape(type::caption, label);
    const float chip_h = std::round(p.px(22.0F));
    const float chip_pad = p.px(7.0F);
    const float chip_gap = p.px(4.0F);
    float width = 0.0F;
    for (const ShapedText& k : keys) {
        width += std::round(std::max(chip_h, k.width() + (2.0F * chip_pad))) + chip_gap;
    }
    if (!keys.empty()) {
        width += p.px(6.0F);
    }
    width += text.width();
    if (measure_only) {
        return width;
    }
    float x = std::round(center_x - (0.5F * width));
    const float edge = std::max(1.0F, std::round(p.px(1.0F)));
    for (const ShapedText& k : keys) {
        const float w = std::round(std::max(chip_h, k.width() + (2.0F * chip_pad)));
        const RectF cap = snap(RectF{x, center_y - (0.5F * chip_h), w, chip_h});
        const float rad = p.px(radius::sm);
        r.fill_rounded_rect(RectF{cap.x, cap.y + edge, cap.w, cap.h}, rad, pal.keycap_edge);
        r.fill_rounded_rect(cap, rad, pal.surface_raised);
        r.stroke_rounded_rect(cap, rad, edge, pal.keycap_border);
        const float baseline = std::round(cap.y + (0.5F * cap.h) + (0.5F * k.cap_height()));
        p.draw(k, cap.x + (0.5F * cap.w) - (0.5F * k.width()), baseline, pal.keycap_text);
        x += w + chip_gap;
    }
    if (!keys.empty()) {
        x += p.px(6.0F) - chip_gap;
    }
    p.draw(text, x, std::round(center_y + (0.5F * text.cap_height())), pal.text_secondary);
    return width;
}

void BrowserView::draw_empty_state(const BrowserFrame& f, const Layout& l) {
    const BrowserModel& m = f.model;
    if (!m.loaded()) {
        return; // first listing still running: keep the panel calm, no flash of "empty"
    }
    const Painter& p = f.painter;
    const Palette& pal = f.palette;
    std::string title;
    std::string detail;
    std::string_view hint_cmd;
    std::string_view hint_key;
    if (const std::optional<Error>& err = m.listing_error(); err.has_value()) {
        title = std::string(tr("empty.unreadable"));
        detail = std::string(tr("error." + std::string(to_string(err.value().code))));
        if (m.can_up()) {
            hint_cmd = "nav.up";
            hint_key = "hint.go_up";
        }
    } else if (m.hidden_count() > 0 && !m.show_hidden()) {
        title = tr_count("empty.hidden_only", m.hidden_count());
        hint_cmd = "view.toggle_hidden";
        hint_key = "hint.show_hidden";
    } else {
        title = std::string(tr("empty.folder"));
        if (m.can_up()) {
            hint_cmd = "nav.up";
            hint_key = "hint.go_up";
        }
    }
    const float cx = l.body.x + (0.5F * l.body.w);
    const float cy = l.body.y + (l.body.h * 0.42F);
    const ShapedText t =
        fitter_.fit(p, type::empty_title, title, l.body.w - p.px(48.0F), Elide::End);
    float baseline = std::round(cy);
    p.draw(t, std::round(cx - (0.5F * t.width())), baseline, pal.text_secondary);
    if (!detail.empty()) {
        const ShapedText d = fitter_.fit(p, type::row, detail, l.body.w - p.px(48.0F), Elide::End);
        baseline += std::round(p.px(22.0F));
        p.draw(d, std::round(cx - (0.5F * d.width())), baseline, pal.text_muted);
    }
    if (!hint_cmd.empty()) {
        draw_key_hint(f, hint_cmd, tr(hint_key), cx, baseline + std::round(p.px(30.0F)), false);
    }
}

// ---------------------------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------------------------

void BrowserView::draw_status(const BrowserFrame& f, const Layout& l) {
    const Painter& p = f.painter;
    const Palette& pal = f.palette;
    const BrowserModel& m = f.model;
    const float margin = std::round(p.px(density_metrics(f.density).panel_margin + 8.0F));
    const float mid = l.status.y + (0.5F * l.status.h);
    const std::string dot = "  \xC2\xB7  "; // middle dot

    struct Part {
        std::string text;
        bool strong;
    };
    std::vector<Part> left;
    if (m.loaded()) {
        left.push_back({tr_count("status.items", m.entries().size()), false});
        if (!m.show_hidden() && m.hidden_count() > 0) {
            left.push_back({tr_count("status.hidden", m.hidden_count()), false});
        }
    }
    if (m.busy()) {
        left.push_back({std::string(tr("status.loading")), false});
    }
    std::vector<Part> right;
    if (m.selected_count() > 0) {
        right.push_back({tr_count("status.selected", m.selected_count()), true});
        if (m.selected_bytes() > 0) {
            right.push_back({format_size(m.selected_bytes()), false});
        }
    }
    const auto draw_parts = [&](const std::vector<Part>& parts, float x, bool align_right) {
        std::vector<ShapedText> shaped;
        float width = 0.0F;
        const ShapedText sep = p.shape(type::status, dot);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            shaped.push_back(p.shape(type::status, parts[i].text));
            width += shaped.back().width() + (i > 0 ? sep.width() : 0.0F);
        }
        float cx = align_right ? x - width : x;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) {
                p.draw(sep, cx, std::round(mid + (0.5F * sep.cap_height())), pal.text_muted);
                cx += sep.width();
            }
            const ShapedText& s = shaped[i];
            p.draw(s, cx, std::round(mid + (0.5F * s.cap_height())),
                   parts[i].strong ? pal.text_primary : pal.text_secondary);
            cx += s.width();
        }
    };
    draw_parts(left, l.status.x + margin, false);
    draw_parts(right, l.status.right() - margin, true);
}

// ---------------------------------------------------------------------------------------------
// FPS overlay (Phase 0 design, F12)
// ---------------------------------------------------------------------------------------------

void draw_fps_overlay(const Painter& p, const Palette& pal, float width, float height,
                      Density density, double fps, double frame_ms, std::string_view driver) {
    std::array<char, 32> fps_buf{};
    std::array<char, 32> ms_buf{};
    std::snprintf(fps_buf.data(), fps_buf.size(), "%.0f", fps);
    std::snprintf(ms_buf.data(), ms_buf.size(), "%.1f %s", frame_ms,
                  std::string(tr("fps.unit_ms")).c_str());
    struct Row {
        ShapedText label;
        ShapedText value;
    };
    std::array rows{
        Row{p.shape(type::caption, tr("fps.label")), p.shape(type::numeric, fps_buf.data())},
        Row{p.shape(type::caption, tr("fps.frame_time")), p.shape(type::numeric, ms_buf.data())},
        Row{p.shape(type::caption, tr("fps.driver")), p.shape(type::numeric, driver)},
    };
    const float reserve = p.shape(type::numeric, "000.0 ms").width();
    float label_w = 0.0F;
    float value_w = reserve;
    for (const Row& row : rows) {
        label_w = std::max(label_w, row.label.width());
        value_w = std::max(value_w, row.value.width());
    }
    const float pad_x = p.px(space::md + space::xxs);
    const float pad_y = p.px(space::md);
    const float row_h = p.px(space::lg + space::xs);
    const float w = std::ceil(pad_x + label_w + p.px(space::xl) + value_w + pad_x);
    const float h = std::ceil((2.0F * pad_y) + (row_h * static_cast<float>(rows.size())));
    const float margin = p.px(space::lg);
    const RectF panel =
        snap(RectF{width - margin - w,
                   height - p.px(density_metrics(density).status_bar_height) - margin - h, w, h});
    const float rad = p.px(radius::lg);
    Renderer& r = p.renderer();
    r.drop_shadow(panel, rad, p.px(space::lg), p.px(space::xs), pal.shadow);
    r.fill_rounded_rect(panel, rad, pal.surface_raised.with_alpha(0.96F));
    r.stroke_rounded_rect(panel, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    float row_top = panel.y + pad_y;
    for (const Row& row : rows) {
        const float cap = std::max(row.label.cap_height(), row.value.cap_height());
        const float baseline = row_top + (0.5F * row_h) + (0.5F * cap);
        p.draw(row.label, panel.x + pad_x, baseline, pal.text_muted);
        p.draw(row.value, panel.right() - pad_x - row.value.width(), baseline, pal.text_primary);
        row_top += row_h;
    }
}

} // namespace navigator::ui

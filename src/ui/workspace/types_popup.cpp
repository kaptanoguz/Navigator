// src/ui/workspace/types_popup.cpp — the Phase 5 types popup (search.types, Ctrl+Shift+F): one
// row per type present in the active tab's view (folders, no extension, each extension) with
// its count and a visibility checkbox, and "All types" (search::toggle_all) on top. Anchored
// below the tab's types button. Toggles are Intents applied at frame end (the App writes the
// tab's TabState::type_filter, persisted with the session).
// Keys (docs/keymap.md "Popups"): list.cursor_* move the highlight, list.activate toggles it,
// Escape or search.types closes.
#include <algorithm>
#include <cmath>
#include <string>

#include <navigator/i18n.hpp>
#include <navigator/mime.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"
#include "widgets/icons.hpp"
#include "workspace/workspace_view.hpp"

namespace navigator::ui {
namespace {

constexpr float kTypesWidth = 304.0F;
constexpr float kTypesRow = 30.0F;
constexpr std::size_t kTypesMaxRows = 11;

void draw_checkbox(Renderer& r, const Palette& pal, const RectF& box, bool checked, bool mixed,
                   float scale) {
    const float rad = std::round(3.5F * scale);
    if (checked || mixed) {
        r.fill_rounded_rect(box, rad, pal.accent);
        const float t = std::max(1.0F, std::round(1.6F * scale));
        if (mixed) {
            r.fill_rounded_rect(RectF{box.x + (3.5F * scale), box.center_y() - (0.5F * t),
                                      box.w - (7.0F * scale), t},
                                0.5F * t, pal.on_accent);
        } else {
            r.stroke_line(box.x + (3.2F * scale), box.center_y() + (0.2F * scale),
                          box.x + (5.9F * scale), box.bottom() - (3.6F * scale), t, pal.on_accent);
            r.stroke_line(box.x + (5.9F * scale), box.bottom() - (3.6F * scale),
                          box.right() - (3.0F * scale), box.y + (3.6F * scale), t, pal.on_accent);
        }
    } else {
        r.stroke_rounded_rect(box, rad, std::max(1.0F, std::round(1.2F * scale)),
                              pal.control_border);
    }
}

std::string type_label(const search::TypeKey& key) {
    switch (key.kind) {
    case search::TypeKind::Folder: return std::string(tr("types.folders"));
    case search::TypeKind::NoExtension: return std::string(tr("types.no_extension"));
    case search::TypeKind::Extension: break;
    }
    return "." + display_name(key.extension);
}

// "PDF document", "JPEG image" ... from the name table; empty for unknown extensions.
std::string type_detail(const search::TypeKey& key) {
    if (key.kind != search::TypeKind::Extension) {
        return {};
    }
    const std::string_view mime = mime::guess_from_name("x." + key.extension);
    if (mime == mime::kOctetStream) {
        return {};
    }
    return std::string(tr(mime::type_label_key(mime)));
}

} // namespace

void WorkspaceView::open_types_popup(const Workspace& ws) {
    const layout::TabId tab = ws.active_tab();
    const BrowserModel* m = ws.model(tab);
    if (m == nullptr || !m->loaded()) {
        return;
    }
    drag_.cancel();
    close_address_bar();
    popup_ = Popup{};
    popup_.kind = PopupKind::Types;
    popup_.types_tab = tab;
    popup_.type_rows = m->type_summary();
    popup_.highlight = 0;
    popup_.pointer_moved = false;
}

void WorkspaceView::toggle_type_row(std::size_t row, std::vector<Intent>& out) const {
    Intent in;
    in.tab = popup_.types_tab.value;
    if (row == 0) {
        in.kind = Intent::Kind::ToggleAllTypes;
        out.push_back(std::move(in));
        return;
    }
    if (row - 1 >= popup_.type_rows.size()) {
        return;
    }
    const search::TypeCount& t = popup_.type_rows[row - 1];
    in.kind = Intent::Kind::SetTypeVisible;
    in.index = static_cast<std::size_t>(t.key.kind);
    in.text = t.key.extension;
    in.additive = !t.visible;
    out.push_back(std::move(in));
}

void WorkspaceView::draw_types(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const BrowserModel* m = f.workspace.model(popup_.types_tab);
    const BrowserView* bv = find_view(popup_.types_tab);
    if (m == nullptr || bv == nullptr || popup_.types_tab != f.workspace.active_tab()) {
        close_popup();
        return;
    }
    // Rows follow the model live (new files, a walk that is still streaming); the visibility
    // flags come from the CURRENT filter, so a toggle shows at once.
    popup_.type_rows = m->type_summary();
    const search::TypeFilter& filter = m->type_filter();
    for (search::TypeCount& t : popup_.type_rows) {
        t.visible = search::is_visible(filter, t.key);
    }
    const std::size_t n = popup_.type_rows.size();
    std::size_t visible_count = 0;
    for (const search::TypeCount& t : popup_.type_rows) {
        visible_count += t.visible ? 1 : 0;
    }
    if (popup_.highlight > n) {
        popup_.highlight = n;
    }

    const float row_h = std::round(p.px(kTypesRow));
    const float pad = std::round(p.px(5.0F));
    const float head_h = std::round(p.px(36.0F));
    const float sep_h = std::round(p.px(9.0F));
    const std::size_t shown = std::min(n, kTypesMaxRows);
    const float list_h = n == 0 ? std::round(p.px(44.0F)) : row_h * static_cast<float>(shown);
    const float h = head_h + row_h + sep_h + list_h + (2.0F * pad);
    const float w = std::min(std::round(p.px(kTypesWidth)), f.width - p.px(16.0F));
    const RectF anchor = bv->types_rect();
    float x = std::round(anchor.right() - w);
    float y = std::round(anchor.bottom() + p.px(6.0F));
    x = std::clamp(x, p.px(8.0F), f.width - w - p.px(8.0F));
    if (y + h > f.height - p.px(8.0F)) {
        y = std::max(p.px(8.0F), f.height - p.px(8.0F) - h);
    }
    const RectF card = snap(RectF{x, y, w, h});
    if ((ptr.pressed || ptr.right_pressed) && !contains(card, ptr.x, ptr.y) &&
        !popup_.just_opened) { // the types button too: a second click closes it
        close_popup();
        return;
    }
    const float rad = std::round(p.px(radius::md + 1.0F));
    r.drop_shadow(card, rad, p.px(22.0F), p.px(6.0F), pal.shadow);
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));

    // Header: title and how many entries the filter hides.
    const float inner = card.x + std::round(p.px(14.0F));
    {
        const ShapedText title = p.shape(type::popup_title, tr("types.title"));
        const float base = std::round(card.y + pad + (0.5F * head_h) + (0.5F * title.cap_height()));
        p.draw(title, inner, base, pal.text_primary);
        std::size_t hidden_items = 0;
        for (const search::TypeCount& t : popup_.type_rows) {
            hidden_items += t.visible ? 0 : t.count;
        }
        if (hidden_items > 0) {
            const ShapedText sub =
                p.shape(type::caption, tr_count("types.hidden_items", hidden_items));
            p.draw(sub, card.right() - std::round(p.px(14.0F)) - sub.width(), base, pal.chip_text);
        }
    }
    float yy = card.y + pad + head_h;
    r.fill_rect(RectF{card.x, std::round(yy - hair), card.w, hair}, pal.border);

    // Scroll so the highlight stays visible (row 0 is pinned above the list).
    const auto first_max = static_cast<float>(n > shown ? n - shown : 0);
    if (popup_.highlight >= 1 && !popup_.pointer_moved) { // the keyboard steers
        const auto hi = static_cast<float>(popup_.highlight - 1);
        if (hi < popup_.scroll) {
            popup_.scroll = hi;
        } else if (hi >= popup_.scroll + static_cast<float>(shown)) {
            popup_.scroll = hi - static_cast<float>(shown) + 1.0F;
        }
    }
    const RectF list_rect{card.x, yy + row_h + sep_h, card.w, list_h};
    if (contains(list_rect, ptr.x, ptr.y) && ptr.wheel_y != 0.0F) {
        popup_.scroll -= ptr.wheel_y * 3.0F;
        popup_.pointer_moved = true;
    }
    popup_.scroll = std::clamp(std::round(popup_.scroll), 0.0F, first_max);

    const auto row = [&](std::size_t index, const RectF& rr) {
        const bool inside = contains(rr, ptr.x, ptr.y);
        if (inside && popup_.pointer_moved) {
            popup_.highlight = index;
        }
        if (popup_.highlight == index) {
            r.fill_rounded_rect(snap(rr), std::round(p.px(radius::sm)), pal.control_hover);
        }
        const float box = std::round(p.px(14.0F));
        const RectF cb =
            snap(RectF{rr.x + std::round(p.px(9.0F)), rr.center_y() - (0.5F * box), box, box});
        float tx = cb.right() + std::round(p.px(10.0F));
        if (index == 0) {
            const bool all = visible_count == n;
            draw_checkbox(r, pal, cb, all && n > 0, !all && visible_count > 0, p.scale());
            const ShapedText t = p.shape(type::row, tr("types.all"));
            p.draw(t, tx, std::round(rr.center_y() + (0.5F * t.cap_height())), pal.text_primary);
        } else {
            const search::TypeCount& t = popup_.type_rows[index - 1];
            draw_checkbox(r, pal, cb, t.visible, false, p.scale());
            const float ic = std::round(p.px(16.0F));
            fs::DirEntry probe;
            if (t.key.kind == search::TypeKind::Folder) {
                probe.kind = fs::EntryKind::Directory;
            } else {
                probe.kind = fs::EntryKind::File;
                probe.name =
                    t.key.kind == search::TypeKind::Extension ? "x." + t.key.extension : "x";
            }
            draw_entry_glyph(r, pal, probe, tx, std::round(rr.center_y() - (0.5F * ic)), ic,
                             t.visible ? 1.0F : 0.45F, p.scale());
            tx += ic + std::round(p.px(9.0F));
            const ShapedText count =
                p.shape(type::row_numeric, t.count > 0 ? format_count(t.count) : "\xE2\x80\x93");
            const float count_x = rr.right() - std::round(p.px(10.0F)) - count.width();
            const std::string label = type_label(t.key);
            const ShapedText lt =
                fitter_.fit(p, type::row, label, count_x - tx - p.px(8.0F), Elide::Middle);
            const float base = std::round(rr.center_y() + (0.5F * lt.cap_height()));
            p.draw(lt, tx, base, t.visible ? pal.text_primary : pal.text_muted);
            const std::string detail = type_detail(t.key);
            const float dx = tx + lt.width() + std::round(p.px(8.0F));
            if (!detail.empty() && count_x - dx > p.px(40.0F)) {
                const ShapedText dt =
                    fitter_.fit(p, type::caption, detail, count_x - dx - p.px(10.0F), Elide::End);
                p.draw(dt, dx, base, pal.text_muted);
            }
            p.draw(count, count_x, base, pal.text_secondary);
        }
        if (inside && ptr.pressed) {
            popup_.pressed = WidgetId::TypesRow;
            popup_.pressed_index = index;
        }
        if (inside && ptr.released && popup_.pressed == WidgetId::TypesRow &&
            popup_.pressed_index == index) {
            popup_.highlight = index;
            toggle_type_row(index, f.intents);
        }
    };
    const float rx = card.x + pad;
    const float rw = card.w - (2.0F * pad);
    row(0, RectF{rx, yy, rw, row_h});
    yy += row_h;
    r.fill_rect(RectF{card.x + pad, std::round(yy + (0.5F * sep_h)), card.w - (2.0F * pad), hair},
                pal.border);
    yy += sep_h;
    if (n == 0) {
        const ShapedText t = p.shape(type::row, tr("types.none"));
        p.draw(t, std::round(card.center_x() - (0.5F * t.width())),
               std::round(yy + (0.5F * list_h) + (0.5F * t.cap_height())), pal.text_muted);
        return;
    }
    r.set_clip(RectF{card.x, yy, card.w, list_h});
    const auto first = static_cast<std::size_t>(popup_.scroll);
    for (std::size_t k = 0; k < shown && first + k < n; ++k) {
        row(first + k + 1, RectF{rx, yy + (row_h * static_cast<float>(k)), rw, row_h});
    }
    r.set_clip(std::nullopt);
    // Overflow cue: the list scrolls (wheel, arrows).
    if (n > shown) {
        const float track = list_h - p.px(8.0F);
        const float th =
            std::max(p.px(18.0F), track * (static_cast<float>(shown) / static_cast<float>(n)));
        const float t = first_max > 0.0F ? popup_.scroll / first_max : 0.0F;
        r.fill_rounded_rect(snap(RectF{card.right() - p.px(5.0F),
                                       yy + p.px(4.0F) + ((track - th) * t), p.px(3.0F), th}),
                            p.px(1.5F), pal.scrollbar);
    }
}

} // namespace navigator::ui

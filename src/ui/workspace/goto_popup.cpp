// src/ui/workspace/goto_popup.cpp — Phase 4 WorkspaceView parts: the GoTo popup (nav.goto) and
// the address bar's completion dropdown. State lives in the two PathEntry controllers; keys
// arrive through route_command() / route_address() and the App's field editing.
#include <algorithm>
#include <cmath>

#include <navigator/i18n.hpp>

#include "i18n/catalog.hpp"
#include "widgets/icons.hpp"
#include "widgets/text_field.hpp"
#include "workspace/workspace_view.hpp"

namespace navigator::ui {
namespace {

constexpr float kGotoWidth = 660.0F;
constexpr float kFieldHeight = 50.0F;
constexpr float kRowHeight = 36.0F;
constexpr float kSectionHeight = 26.0F;
constexpr float kFooterHeight = 36.0F;
constexpr std::size_t kMaxVisible = 9;
constexpr float kDropRow = 30.0F;
constexpr std::size_t kDropRows = 8;

LineIcon item_icon(const PathItem& it) {
    switch (it.kind) {
    case PathItem::Kind::Typed:
    case PathItem::Kind::Completion: return LineIcon::Folder;
    case PathItem::Kind::Recent: return LineIcon::Clock;
    case PathItem::Kind::Bookmark: return LineIcon::Star;
    case PathItem::Kind::Place:
        switch (it.place) {
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
    }
    return LineIcon::Folder;
}

std::string_view section_key(PathItem::Section s) {
    switch (s) {
    case PathItem::Section::Typed: return "";
    case PathItem::Section::Matches: return "goto.section_matches";
    case PathItem::Section::Recents: return "goto.section_recents";
    case PathItem::Section::Places: return "goto.section_places";
    }
    return "";
}

} // namespace

void WorkspaceView::draw_goto(const ShellFrame& f) {
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const std::vector<PathItem>& items = goto_.items();

    // Display rows: section headers (not for the Typed row) + items.
    struct Row {
        bool header = false;
        std::size_t item = 0;
        PathItem::Section section = PathItem::Section::Typed;
    };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const PathItem::Section sec = items[i].section;
        if (sec != PathItem::Section::Typed && (i == 0 || items[i - 1].section != sec)) {
            rows.push_back(Row{true, i, sec});
        }
        rows.push_back(Row{false, i, sec});
    }
    const float row_h = std::round(p.px(kRowHeight));
    const float sec_h = std::round(p.px(kSectionHeight));
    const float field_h = std::round(p.px(kFieldHeight));
    const float foot_h = std::round(p.px(kFooterHeight));
    const float pad = std::round(p.px(6.0F));
    const bool has_error = !goto_.error().empty();
    const float err_h = has_error ? std::round(p.px(34.0F)) : 0.0F;
    float list_h = 0.0F;
    std::size_t shown = 0;
    for (const Row& row : rows) {
        if (!row.header && shown >= kMaxVisible) {
            break;
        }
        list_h += row.header ? sec_h : row_h;
        shown += row.header ? 0 : 1;
    }
    if (rows.empty()) {
        list_h = std::round(p.px(64.0F));
    }
    const float total_h = field_h + err_h + list_h + (2.0F * pad) + foot_h;
    const float top = std::round(std::max(p.px(56.0F), f.height * 0.12F));
    const RectF card = draw_card(f, kGotoWidth, total_h, top);
    if (ptr.pressed && !contains(card, ptr.x, ptr.y) && !popup_.just_opened) {
        close_popup();
        return;
    }

    // Field: a "go" chevron glyph, the text, a spinner-free "resolving" state (quiet).
    const float inner_x = card.x + std::round(p.px(16.0F));
    const float mid = card.y + (0.5F * field_h);
    draw_chevron(r, inner_x + p.px(5.0F), mid, p.px(10.0F), Direction::Right,
                 std::max(1.0F, p.px(1.8F)), pal.accent);
    const float fx = inner_x + std::round(p.px(24.0F));
    (void)draw_text_field(p, pal, goto_.edit(),
                          RectF{fx, card.y, card.right() - fx - p.px(16.0F), field_h},
                          tr("goto.placeholder"), popup_.field_scroll);
    const float hair = std::max(1.0F, std::round(p.px(1.0F)));
    r.fill_rect(RectF{card.x, card.y + field_h, card.w, hair}, pal.border);

    float y = card.y + field_h;
    if (has_error) {
        const RectF eb = snap(RectF{card.x + pad, y + pad, card.w - (2.0F * pad), err_h - pad});
        r.fill_rounded_rect(eb, p.px(radius::sm + 1.0F), pal.danger_surface);
        const float ic = p.px(7.0F);
        draw_alert_glyph(r, eb.x + p.px(14.0F), eb.center_y(), ic, pal.danger, pal.danger_surface,
                         p.scale());
        const float tx = eb.x + p.px(28.0F);
        const ShapedText et =
            fitter_.fit(p, type::row, goto_.error(), eb.right() - tx - p.px(10.0F), Elide::Middle);
        p.draw(et, tx, std::round(eb.center_y() + (0.5F * et.cap_height())), pal.text_primary);
        y += err_h;
    }

    const RectF list{card.x + pad, y + pad, card.w - (2.0F * pad), list_h};
    if (rows.empty()) {
        const ShapedText t = p.shape(type::row, tr("goto.empty"));
        p.draw(t, std::round(list.center_x() - (0.5F * t.width())),
               std::round(list.center_y() + (0.5F * t.cap_height())), pal.text_muted);
    } else {
        std::size_t hi_row = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].header && rows[i].item == goto_.highlight()) {
                hi_row = i;
            }
        }
        auto first = static_cast<std::size_t>(popup_.scroll);
        const auto fits = [&](std::size_t from, std::size_t to) {
            float h = 0.0F;
            for (std::size_t i = from; i <= to && i < rows.size(); ++i) {
                h += rows[i].header ? sec_h : row_h;
            }
            return h <= list_h + 0.5F;
        };
        if (hi_row < first) {
            first = hi_row > 0 && rows[hi_row - 1].header ? hi_row - 1 : hi_row;
        }
        while (first < hi_row && !fits(first, hi_row)) {
            ++first;
        }
        if (ptr.wheel_y != 0.0F && contains(list, ptr.x, ptr.y)) {
            const auto delta = static_cast<std::ptrdiff_t>(-ptr.wheel_y * 3.0F);
            first = static_cast<std::size_t>(
                std::clamp(static_cast<std::ptrdiff_t>(first) + delta, std::ptrdiff_t{0},
                           static_cast<std::ptrdiff_t>(rows.size()) - 1));
        }
        popup_.scroll = static_cast<float>(first);
        r.set_clip(list);
        float yy = list.y;
        const float icon = std::round(p.px(16.0F));
        const float t = std::max(1.0F, p.px(1.35F));
        for (std::size_t i = first; i < rows.size() && yy < list.bottom(); ++i) {
            const Row& row = rows[i];
            if (row.header) {
                const ShapedText h = p.shape(type::section, tr(section_key(row.section)));
                p.draw(h, list.x + std::round(p.px(12.0F)), std::round(yy + sec_h - p.px(8.0F)),
                       pal.text_muted);
                yy += sec_h;
                continue;
            }
            const PathItem& it = items[row.item];
            const RectF rr = snap(RectF{list.x, yy, list.w, row_h});
            const bool inside = contains(rr, ptr.x, ptr.y) && contains(list, ptr.x, ptr.y);
            if (inside && popup_.pointer_moved) {
                goto_.set_highlight(row.item);
            }
            if (inside && ptr.pressed) {
                popup_.pressed = WidgetId::PopupRow;
                popup_.pressed_index = row.item;
            }
            const bool hi = goto_.highlight() == row.item;
            const float rad = std::round(p.px(radius::md - 2.0F));
            if (hi) {
                r.fill_rounded_rect(rr, rad, pal.selection);
                const float mw = std::max(2.0F, std::round(p.px(3.0F)));
                r.fill_rounded_rect(
                    snap(RectF{rr.x + p.px(3.0F), rr.y + p.px(9.0F), mw, rr.h - p.px(18.0F)}),
                    0.5F * mw, pal.selection_marker);
            }
            const float rm = rr.center_y();
            float x = rr.x + std::round(p.px(16.0F));
            const bool offline = it.status != places::LocationStatus::Online;
            if (it.kind == PathItem::Kind::Typed) {
                // The preview line: where Enter goes.
                draw_chevron(r, x + (0.5F * icon), rm, p.px(9.0F), Direction::Right,
                             std::max(1.0F, p.px(1.6F)), hi ? pal.accent : pal.text_secondary);
                x += icon + std::round(p.px(12.0F));
                const ShapedText go = p.shape(type::row, tr("goto.go_to"));
                const float base = std::round(rm + (0.5F * go.cap_height()));
                p.draw(go, x, base, pal.text_secondary);
                const float px0 = x + go.width() + std::round(p.px(8.0F));
                const std::string target =
                    it.path.empty() ? it.label : display_path(it.path, goto_.home());
                const bool unresolved = goto_.preview() && !goto_.preview()->unresolved.empty();
                const float right = rr.right() - std::round(p.px(14.0F));
                std::string note;
                if (unresolved) {
                    std::string names;
                    for (const std::string& u : goto_.preview()->unresolved) {
                        names += names.empty() ? u : ", " + u;
                    }
                    note = substitute(tr("goto.unknown_variable"), "name", names);
                }
                float note_w = 0.0F;
                if (!note.empty()) {
                    const ShapedText nt =
                        fitter_.fit(p, type::caption, note, (right - px0) * 0.5F, Elide::End);
                    note_w = nt.width() + std::round(p.px(12.0F));
                    p.draw(nt, right - nt.width(), base, pal.danger);
                }
                const ShapedText tt =
                    fitter_.fit(p, type::row, target, right - px0 - note_w, Elide::Middle);
                p.draw(tt, px0, base, pal.text_primary);
            } else {
                draw_line_icon(r, item_icon(it), x, std::round(rm - (0.5F * icon)), icon, t,
                               hi        ? pal.accent
                               : offline ? pal.text_muted
                                         : pal.text_secondary,
                               pal.surface_raised, false);
                x += icon + std::round(p.px(12.0F));
                float right = rr.right() - std::round(p.px(14.0F));
                if (offline) {
                    const bool missing = it.status == places::LocationStatus::Missing;
                    const ShapedText bt =
                        p.shape(type::caption,
                                tr(missing ? "sidebar.badge_missing" : "sidebar.badge_offline"));
                    const float bh = std::round(p.px(18.0F));
                    const float bw = std::round(bt.width() + p.px(12.0F));
                    const RectF badge = snap(RectF{right - bw, rm - (0.5F * bh), bw, bh});
                    r.stroke_rounded_rect(badge, 0.5F * bh, hair,
                                          missing ? pal.danger_border : pal.keycap_border);
                    p.draw(bt, badge.x + std::round(p.px(6.0F)),
                           std::round(rm + (0.5F * bt.cap_height())),
                           missing ? pal.danger : pal.text_secondary);
                    right = badge.x - std::round(p.px(10.0F));
                }
                const float room = right - x;
                const ShapedText title = fitter_.fit(
                    p, type::row, it.label, std::min(room * 0.5F, p.px(240.0F)), Elide::Middle);
                const float base = std::round(rm + (0.5F * title.cap_height()));
                p.draw(title, x, base, offline ? pal.text_secondary : pal.text_primary);
                const float dx = x + title.width() + std::round(p.px(12.0F));
                if (right - dx > p.px(24.0F)) {
                    const ShapedText detail =
                        fitter_.fit(p, type::caption, it.detail, right - dx, Elide::Middle);
                    p.draw(detail, dx, base, pal.text_muted);
                }
            }
            if (inside && ptr.released && popup_.pressed == WidgetId::PopupRow &&
                popup_.pressed_index == row.item) {
                goto_.set_highlight(row.item);
                goto_.submit(has(ptr.mods, Modifiers::Ctrl));
            }
            yy += row_h;
        }
        r.set_clip(std::nullopt);
    }
    draw_footer_hints(f, RectF{card.x, card.bottom() - foot_h, card.w, foot_h},
                      {{"\xE2\x86\x91 \xE2\x86\x93", std::string(tr("hint.move"))},
                       {"Enter", std::string(tr("hint.go"))},
                       {std::string(tr("key.ctrl")) + " Enter", std::string(tr("hint.new_tab"))},
                       {"Tab", std::string(tr("hint.complete"))},
                       {"Esc", std::string(tr("hint.close"))}});
}

void WorkspaceView::draw_address_dropdown(const ShellFrame& f) {
    address_dropdown_ = RectF{};
    if (!address_.is_open() || popup_open()) {
        return;
    }
    const BrowserView* bv = find_view(layout::TabId{address_.tab});
    if (bv == nullptr) {
        return;
    }
    const std::vector<PathItem>& items = address_.items();
    const bool has_error = !address_.error().empty();
    if (items.empty() && !has_error) {
        return;
    }
    const Painter& p = f.painter;
    Renderer& r = p.renderer();
    const Palette& pal = f.palette;
    const PointerInput& ptr = f.pointer;
    const RectF field = bv->address_rect();
    const float row_h = std::round(p.px(kDropRow));
    const float pad = std::round(p.px(5.0F));
    const float err_h = has_error ? std::round(p.px(32.0F)) : 0.0F;
    const std::size_t n = std::min(items.size(), kDropRows);
    const float h = (2.0F * pad) + err_h + (row_h * static_cast<float>(n));
    const float w = std::max(field.w, std::round(p.px(320.0F)));
    const RectF card = snap(RectF{field.x, field.bottom() + p.px(4.0F), w, h});
    address_dropdown_ = card;
    const float rad = std::round(p.px(radius::md + 1.0F));
    r.drop_shadow(card, rad, p.px(22.0F), p.px(6.0F), pal.shadow);
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    float y = card.y + pad;
    if (has_error) {
        const RectF eb = snap(RectF{card.x + pad, y, card.w - (2.0F * pad), err_h - p.px(4.0F)});
        r.fill_rounded_rect(eb, p.px(radius::sm), pal.danger_surface);
        draw_alert_glyph(r, eb.x + p.px(13.0F), eb.center_y(), p.px(6.5F), pal.danger,
                         pal.danger_surface, p.scale());
        const float tx = eb.x + p.px(26.0F);
        const ShapedText et = fitter_.fit(p, type::row, address_.error(),
                                          eb.right() - tx - p.px(8.0F), Elide::Middle);
        p.draw(et, tx, std::round(eb.center_y() + (0.5F * et.cap_height())), pal.text_primary);
        y += err_h;
    }
    // Keep the highlight in view (rows scroll in whole steps).
    std::size_t first = 0;
    if (address_.highlight() < items.size() && address_.highlight() >= kDropRows) {
        first = address_.highlight() + 1 - kDropRows;
    }
    const float icon = std::round(p.px(15.0F));
    for (std::size_t k = first; k < items.size() && k < first + kDropRows; ++k) {
        const PathItem& it = items[k];
        const RectF rr = snap(RectF{card.x + pad, y, card.w - (2.0F * pad), row_h});
        const bool inside = contains(rr, ptr.x, ptr.y);
        if (inside && ptr.pressed) {
            path_pressed_ = k;
        }
        const bool hi = address_.highlight() == k;
        if (hi) {
            r.fill_rounded_rect(rr, std::round(p.px(radius::sm)), pal.selection);
        } else if (inside) {
            r.fill_rounded_rect(rr, std::round(p.px(radius::sm)), pal.control_hover);
        }
        const float rm = rr.center_y();
        const float x = rr.x + std::round(p.px(10.0F));
        draw_line_icon(r, LineIcon::Folder, x, std::round(rm - (0.5F * icon)), icon,
                       std::max(1.0F, p.px(1.3F)), hi ? pal.accent : pal.text_secondary,
                       pal.surface_raised);
        const float tx = x + icon + std::round(p.px(10.0F));
        const ShapedText t =
            fitter_.fit(p, type::row, it.label, rr.right() - tx - p.px(10.0F), Elide::Middle);
        p.draw(t, tx, std::round(rm + (0.5F * t.cap_height())), pal.text_primary);
        if (inside && ptr.released && path_pressed_ == k) {
            address_.set_highlight(k);
            address_.submit(has(ptr.mods, Modifiers::Ctrl));
        }
        y += row_h;
    }
    if (ptr.released) {
        path_pressed_.reset();
    }
}

} // namespace navigator::ui

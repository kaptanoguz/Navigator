// src/ui/menu/context_menu_view.cpp — Phase 8 Searchable Context Menu widget.
#include "menu/context_menu_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <navigator/i18n.hpp>

#include "input/shortcut_text.hpp"
#include "widgets/text_field.hpp"
#include "widgets/text_fit.hpp"

namespace navigator::ui {
namespace {

constexpr float kMenuWidth = 300.0F;
constexpr float kSubmenuWidth = 240.0F;
constexpr float kSearchHeight = 34.0F;
constexpr float kRowHeight = 30.0F;
constexpr float kSepHeight = 9.0F;
constexpr float kPinButtonWidth = 26.0F;

void localize_items(std::vector<menu::Item>& items) {
    for (menu::Item& item : items) {
        if (item.label.empty() && !item.label_key.empty()) {
            item.label = std::string(tr(item.label_key));
        }
        if (item.kind == menu::Item::Kind::Submenu) {
            localize_items(item.children);
        }
    }
}

std::string lookup_shortcut_cb(void* user, std::string_view command, std::uint64_t arg) {
    const auto* km = static_cast<const Keymap*>(user);
    if (km == nullptr) {
        return {};
    }
    return primary_shortcut_text(*km, command, arg, arg == 0);
}

bool is_selectable_item(const menu::Item& item) noexcept {
    return item.enabled && item.kind != menu::Item::Kind::Separator &&
           item.kind != menu::Item::Kind::Note;
}

} // namespace

void ContextMenuOverlay::open(menu::MenuKind kind, const CommandRegistry& registry,
                              const Keymap& keymap, const CommandContext& context,
                              const menu::Target& target, const menu::Pins& pins, float anchor_x,
                              float anchor_y, bool has_open_with_targets) {
    open_ = true;
    anchor_x_ = anchor_x;
    anchor_y_ = anchor_y;
    submenu_open_ = false;
    sub_highlighted_.reset();
    query_.set({});
    field_scroll_ = 0.0F;

    raw_ = menu::build(kind, registry, context, target);
    for (menu::Item& item : raw_) {
        if (item.kind == menu::Item::Kind::Submenu && item.id == "submenu.open_with") {
            menu::Item note;
            note.kind = menu::Item::Kind::Note;
            note.label_key = has_open_with_targets ? "menu.loading" : "menu.no_apps";
            note.enabled = false;
            note.pinnable = false;
            item.children.push_back(std::move(note));
        }
    }
    localize_items(raw_);
    menu::apply_shortcuts(raw_, &lookup_shortcut_cb, const_cast<Keymap*>(&keymap));
    arranged_ = menu::arrange(raw_, pins);
    rebuild_filter();
}

void ContextMenuOverlay::close() noexcept {
    open_ = false;
    submenu_open_ = false;
    highlighted_.reset();
    sub_highlighted_.reset();
}

void ContextMenuOverlay::set_open_with_handlers(const std::vector<openwith::Handler>& handlers) {
    if (!open_) {
        return;
    }
    auto update_sub = [&](std::vector<menu::Item>& list) {
        for (menu::Item& item : list) {
            if (item.kind != menu::Item::Kind::Submenu || item.id != "submenu.open_with") {
                continue;
            }
            item.children.clear();
            if (handlers.empty()) {
                menu::Item note;
                note.kind = menu::Item::Kind::Note;
                note.label = std::string(tr("menu.no_apps"));
                note.enabled = false;
                note.pinnable = false;
                item.children.push_back(std::move(note));
            } else {
                for (const openwith::Handler& h : handlers) {
                    menu::Item child;
                    child.kind = menu::Item::Kind::OpenWith;
                    child.id = "open_with:" + h.desktop_id;
                    child.label = h.name;
                    if (h.is_default) {
                        child.label += " (";
                        child.label += tr("openwith.default_badge");
                        child.label += ")";
                    }
                    child.handler_id = h.desktop_id;
                    child.pinnable = false;
                    item.children.push_back(std::move(child));
                }
            }
        }
    };
    update_sub(raw_);
    update_sub(arranged_);
    if (filtered_.active) {
        rebuild_filter();
    }
}

void ContextMenuOverlay::reapply_pins(const menu::Pins& pins) {
    if (!open_) {
        return;
    }
    arranged_ = menu::arrange(raw_, pins);
    rebuild_filter();
}

void ContextMenuOverlay::query_changed() { rebuild_filter(); }

void ContextMenuOverlay::set_query(std::string_view text) {
    query_.set(text);
    rebuild_filter();
}

void ContextMenuOverlay::rebuild_filter() {
    submenu_open_ = false;
    sub_highlighted_.reset();
    filtered_ = menu::filter(arranged_, query_.text());
    if (filtered_.active) {
        highlighted_ = filtered_.default_row;
    } else {
        highlighted_.reset();
        for (std::size_t i = 0; i < arranged_.size(); ++i) {
            if (is_selectable_item(arranged_[i]) && arranged_[i].danger != Danger::Destructive) {
                highlighted_ = i;
                break;
            }
        }
    }
}

void ContextMenuOverlay::move_highlight(int delta) {
    if (filtered_.active) {
        const std::size_t n = filtered_.rows.size();
        if (n == 0) {
            return;
        }
        if (!highlighted_) {
            highlighted_ = delta > 0 ? 0 : n - 1;
        } else {
            highlighted_ = (*highlighted_ + n + static_cast<std::size_t>(delta > 0 ? 1 : -1)) % n;
        }
        return;
    }
    const std::size_t n = arranged_.size();
    if (n == 0) {
        return;
    }
    std::size_t cur = highlighted_.value_or(delta > 0 ? n - 1 : 0);
    for (std::size_t step = 0; step < n; ++step) {
        cur = (cur + n + static_cast<std::size_t>(delta > 0 ? 1 : -1)) % n;
        if (is_selectable_item(arranged_[cur])) {
            highlighted_ = cur;
            return;
        }
    }
}

std::optional<MenuAction> ContextMenuOverlay::activate_highlighted() {
    if (!highlighted_) {
        return std::nullopt;
    }
    if (filtered_.active) {
        if (*highlighted_ >= filtered_.rows.size()) {
            return std::nullopt;
        }
        const menu::Item& item = filtered_.rows[*highlighted_].item;
        MenuAction act;
        if (item.kind == menu::Item::Kind::OpenWith) {
            act.kind = MenuAction::Kind::OpenWith;
            act.handler_id = item.handler_id;
        } else {
            act.kind = MenuAction::Kind::Command;
            act.command = item.command;
        }
        close();
        return act;
    }
    if (*highlighted_ >= arranged_.size()) {
        return std::nullopt;
    }
    const menu::Item& item = arranged_[*highlighted_];
    if (item.kind == menu::Item::Kind::Submenu) {
        submenu_open_ = true;
        submenu_index_ = *highlighted_;
        sub_highlighted_.reset();
        for (std::size_t i = 0; i < item.children.size(); ++i) {
            if (is_selectable_item(item.children[i])) {
                sub_highlighted_ = i;
                break;
            }
        }
        return std::nullopt;
    }
    if (!is_selectable_item(item)) {
        return std::nullopt;
    }
    MenuAction act;
    if (item.kind == menu::Item::Kind::OpenWith) {
        act.kind = MenuAction::Kind::OpenWith;
        act.handler_id = item.handler_id;
    } else {
        act.kind = MenuAction::Kind::Command;
        act.command = item.command;
    }
    close();
    return act;
}

std::optional<MenuAction> ContextMenuOverlay::handle_key(KeyChord chord) {
    if (!open_) {
        return std::nullopt;
    }
    if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
        if (submenu_open_) {
            submenu_open_ = false;
            sub_highlighted_.reset();
        } else if (!query_.text().empty()) {
            query_.set({});
            rebuild_filter();
        } else {
            close();
        }
        return std::nullopt;
    }
    if (submenu_open_ && submenu_index_ < arranged_.size()) {
        const auto& kids = arranged_[submenu_index_].children;
        if (chord.key == Key::Left && chord.mods == Modifiers::None) {
            submenu_open_ = false;
            sub_highlighted_.reset();
            return std::nullopt;
        }
        if ((chord.key == Key::Down || chord.key == Key::Up) && chord.mods == Modifiers::None &&
            !kids.empty()) {
            const int d = chord.key == Key::Down ? 1 : -1;
            std::size_t cur = sub_highlighted_.value_or(d > 0 ? kids.size() - 1 : 0);
            for (std::size_t s = 0; s < kids.size(); ++s) {
                cur = (cur + kids.size() + static_cast<std::size_t>(d > 0 ? 1 : -1)) % kids.size();
                if (is_selectable_item(kids[cur])) {
                    sub_highlighted_ = cur;
                    break;
                }
            }
            return std::nullopt;
        }
        if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
            chord.mods == Modifiers::None && sub_highlighted_ && *sub_highlighted_ < kids.size()) {
            const menu::Item& c = kids[*sub_highlighted_];
            if (is_selectable_item(c)) {
                MenuAction act;
                if (c.kind == menu::Item::Kind::OpenWith) {
                    act.kind = MenuAction::Kind::OpenWith;
                    act.handler_id = c.handler_id;
                } else {
                    act.kind = MenuAction::Kind::Command;
                    act.command = c.command;
                }
                close();
                return act;
            }
        }
        return std::nullopt;
    }
    if (chord.key == Key::Down && chord.mods == Modifiers::None) {
        move_highlight(1);
        return std::nullopt;
    }
    if (chord.key == Key::Up && chord.mods == Modifiers::None) {
        move_highlight(-1);
        return std::nullopt;
    }
    if (chord.key == Key::Right && chord.mods == Modifiers::None && !filtered_.active &&
        highlighted_ && *highlighted_ < arranged_.size() &&
        arranged_[*highlighted_].kind == menu::Item::Kind::Submenu) {
        return activate_highlighted();
    }
    if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
        chord.mods == Modifiers::None) {
        return activate_highlighted();
    }
    return std::nullopt;
}

std::optional<MenuAction> ContextMenuOverlay::draw(const Painter& p, const Palette& pal,
                                                   const RectF& viewport,
                                                   const PointerInput& pointer) {
    if (!open_) {
        return std::nullopt;
    }
    Renderer& r = p.renderer();

    const float menu_w = p.px(kMenuWidth);
    const float search_h = p.px(kSearchHeight);
    const float row_h = p.px(kRowHeight);
    const float sep_h = p.px(kSepHeight);
    const float pad_v = p.px(6.0F);

    float content_h = pad_v * 2.0F;
    if (filtered_.active) {
        if (filtered_.rows.empty()) {
            content_h += row_h;
        } else {
            for (const menu::Row& row : filtered_.rows) {
                if (row.separated_before) {
                    content_h += sep_h;
                }
                content_h += row_h;
            }
        }
    } else {
        for (const menu::Item& item : arranged_) {
            content_h += (item.kind == menu::Item::Kind::Separator) ? sep_h : row_h;
        }
    }

    const float menu_h = std::min(search_h + content_h, viewport.h - p.px(16.0F));
    float mx = std::clamp(
        anchor_x_, viewport.x + p.px(8.0F),
        std::max(viewport.x + p.px(8.0F), viewport.x + viewport.w - menu_w - p.px(8.0F)));
    float my = std::clamp(
        anchor_y_, viewport.y + p.px(8.0F),
        std::max(viewport.y + p.px(8.0F), viewport.y + viewport.h - menu_h - p.px(8.0F)));
    const RectF box{std::floor(mx), std::floor(my), menu_w, menu_h};

    // Submenu rect if open.
    RectF sub_box{0.0F, 0.0F, 0.0F, 0.0F};
    float submenu_anchor_y = box.y + search_h;
    if (submenu_open_ && !filtered_.active && submenu_index_ < arranged_.size()) {
        float y = box.y + search_h + pad_v;
        for (std::size_t i = 0; i < submenu_index_; ++i) {
            y += (arranged_[i].kind == menu::Item::Kind::Separator) ? sep_h : row_h;
        }
        submenu_anchor_y = y;
        const auto& kids = arranged_[submenu_index_].children;
        const float sw = p.px(kSubmenuWidth);
        const float sh =
            pad_v * 2.0F + static_cast<float>(std::max<std::size_t>(1, kids.size())) * row_h;
        float sx = box.x + box.w + p.px(2.0F);
        if (sx + sw > viewport.x + viewport.w - p.px(8.0F)) {
            sx = std::max(viewport.x + p.px(8.0F), box.x - sw - p.px(2.0F));
        }
        const float sy = std::clamp(
            submenu_anchor_y, viewport.y + p.px(8.0F),
            std::max(viewport.y + p.px(8.0F), viewport.y + viewport.h - sh - p.px(8.0F)));
        sub_box = RectF{std::floor(sx), std::floor(sy), sw, sh};
    }

    // Click outside both main menu and submenu closes.
    if ((pointer.pressed || pointer.right_pressed) && !contains(box, pointer.x, pointer.y) &&
        !(submenu_open_ && contains(sub_box, pointer.x, pointer.y))) {
        close();
        return std::nullopt;
    }

    // Draw main menu card.
    const float rad = p.px(radius::lg);
    r.drop_shadow(box, rad, p.px(14.0F), p.px(4.0F), pal.shadow.with_alpha(0.32F));
    r.fill_rounded_rect(box, rad, pal.surface_raised);
    r.stroke_rounded_rect(box, rad, p.px(1.0F), pal.border);

    // Search field at top.
    const RectF search_box{box.x + p.px(10.0F), box.y + p.px(3.0F), box.w - p.px(20.0F),
                           search_h - p.px(6.0F)};
    TextFieldStyle fs;
    fs.text = &type::body;
    fs.caret_height_dp = 14.0F;
    draw_text_field(p, pal, query_, search_box, tr("menu.search_placeholder"), field_scroll_, fs);
    r.fill_rect(RectF{box.x, box.y + search_h, box.w, p.px(1.0F)}, pal.border);

    std::optional<MenuAction> action;
    float y = box.y + search_h + pad_v;

    auto draw_item_row = [&](const menu::Item& item, std::size_t idx, bool is_filtered,
                             std::string_view parent_label,
                             const std::vector<search::HighlightRange>& hl) {
        const RectF row_rect{box.x + p.px(4.0F), y, box.w - p.px(8.0F), row_h};
        const bool sel = is_selectable_item(item);
        const bool hov = sel && contains(row_rect, pointer.x, pointer.y);
        if (hov) {
            highlighted_ = idx;
            if (!is_filtered && item.kind == menu::Item::Kind::Submenu) {
                submenu_open_ = true;
                submenu_index_ = idx;
            } else if (!is_filtered &&
                       !(submenu_open_ && contains(sub_box, pointer.x, pointer.y))) {
                submenu_open_ = false;
            }
        }
        const bool hi = highlighted_.has_value() && *highlighted_ == idx;
        if (hi && sel) {
            r.fill_rounded_rect(row_rect, p.px(radius::sm), pal.selection);
        }

        // Pin toggle button on the right for pinnable rows (when not filtered).
        float right_edge = row_rect.x + row_rect.w - p.px(8.0F);
        bool clicked_pin = false;
        if (item.pinnable && item.kind == menu::Item::Kind::Command && (item.pinned || hov)) {
            const float pw = p.px(kPinButtonWidth);
            const RectF pin_rect{right_edge - pw + p.px(4.0F), row_rect.y + p.px(3.0F),
                                 pw - p.px(4.0F), row_rect.h - p.px(6.0F)};
            if (contains(pin_rect, pointer.x, pointer.y)) {
                r.fill_rounded_rect(pin_rect, p.px(4.0F), pal.control_hover);
                if (pointer.pressed) {
                    clicked_pin = true;
                    MenuAction pin_act;
                    pin_act.kind = MenuAction::Kind::TogglePin;
                    pin_act.pin_id = item.id;
                    action = std::move(pin_act);
                }
            }
            const ShapedText star = p.shape(type::caption, item.pinned ? "★" : "☆");
            p.draw(star, pin_rect.x + (pin_rect.w - star.width()) * 0.5F,
                   row_rect.y + row_h * 0.66F, item.pinned ? pal.accent : pal.text_muted);
            right_edge = pin_rect.x - p.px(6.0F);
        }

        // Right-aligned shortcut or submenu chevron.
        if (item.kind == menu::Item::Kind::Submenu) {
            const ShapedText chev = p.shape(type::body, "›");
            p.draw(chev, right_edge - chev.width(), row_rect.y + row_h * 0.66F, pal.text_muted);
            right_edge -= chev.width() + p.px(8.0F);
        } else if (!item.shortcut.empty()) {
            const ShapedText sc = p.shape(type::caption, item.shortcut);
            p.draw(sc, right_edge - sc.width(), row_rect.y + row_h * 0.66F, pal.text_muted);
            right_edge -= sc.width() + p.px(10.0F);
        }

        const Color fg = !item.enabled
                             ? pal.text_muted
                             : (item.danger == Danger::Destructive ? pal.danger : pal.text_primary);
        float lx = row_rect.x + p.px(10.0F);
        const float baseline = row_rect.y + row_h * 0.66F;
        if (!parent_label.empty()) {
            const std::string pre = std::string(parent_label) + " › ";
            const ShapedText pt = p.shape(type::caption, pre);
            p.draw(pt, lx, baseline, pal.text_muted);
            lx += pt.width();
        }
        const float avail = std::max(0.0F, right_edge - lx);
        const std::string fitted =
            TextFitter::elide(p, type::body, item.label, avail, Elide::Middle);
        const ShapedText lt = p.shape(type::body, fitted);
        if (!hl.empty() && fitted == item.label) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
            ranges.reserve(hl.size());
            for (const search::HighlightRange& hr : hl) {
                ranges.emplace_back(hr.begin, hr.end);
            }
            p.draw_highlighted(lt, lx, baseline, fg, pal.match, pal.match, ranges,
                               item.label.size());
        } else {
            p.draw(lt, lx, baseline, fg);
        }

        if (hov && pointer.pressed && !clicked_pin && item.kind != menu::Item::Kind::Submenu) {
            highlighted_ = idx;
            action = activate_highlighted();
        }
        y += row_h;
    };

    if (filtered_.active) {
        if (filtered_.rows.empty()) {
            const ShapedText et = p.shape(type::body, tr("menu.empty"));
            p.draw(et, box.x + p.px(14.0F), y + row_h * 0.66F, pal.text_muted);
        } else {
            for (std::size_t i = 0; i < filtered_.rows.size(); ++i) {
                const menu::Row& row = filtered_.rows[i];
                if (row.separated_before) {
                    r.fill_rect(RectF{box.x + p.px(8.0F), y + sep_h * 0.5F, box.w - p.px(16.0F),
                                      p.px(1.0F)},
                                pal.border);
                    y += sep_h;
                }
                draw_item_row(row.item, i, true, row.parent_label, row.highlights);
            }
        }
    } else {
        for (std::size_t i = 0; i < arranged_.size(); ++i) {
            const menu::Item& item = arranged_[i];
            if (item.kind == menu::Item::Kind::Separator) {
                r.fill_rect(
                    RectF{box.x + p.px(8.0F), y + sep_h * 0.5F, box.w - p.px(16.0F), p.px(1.0F)},
                    pal.border);
                y += sep_h;
                continue;
            }
            draw_item_row(item, i, false, {}, {});
        }
    }

    // Draw Open With submenu when expanded.
    if (submenu_open_ && !filtered_.active && submenu_index_ < arranged_.size()) {
        r.drop_shadow(sub_box, rad, p.px(14.0F), p.px(4.0F), pal.shadow.with_alpha(0.32F));
        r.fill_rounded_rect(sub_box, rad, pal.surface_raised);
        r.stroke_rounded_rect(sub_box, rad, p.px(1.0F), pal.border);
        float sy = sub_box.y + pad_v;
        const auto& kids = arranged_[submenu_index_].children;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            const menu::Item& c = kids[i];
            const RectF cr{sub_box.x + p.px(4.0F), sy, sub_box.w - p.px(8.0F), row_h};
            const bool sel = is_selectable_item(c);
            const bool hov = sel && contains(cr, pointer.x, pointer.y);
            if (hov) {
                sub_highlighted_ = i;
            }
            if (sel && sub_highlighted_ && *sub_highlighted_ == i) {
                r.fill_rounded_rect(cr, p.px(radius::sm), pal.selection);
            }
            const std::string fitted =
                TextFitter::elide(p, type::body, c.label, cr.w - p.px(16.0F), Elide::Middle);
            const ShapedText ct = p.shape(type::body, fitted);
            p.draw(ct, cr.x + p.px(10.0F), cr.y + row_h * 0.66F,
                   sel ? pal.text_primary : pal.text_muted);
            if (hov && pointer.pressed) {
                MenuAction act;
                act.kind = MenuAction::Kind::OpenWith;
                act.handler_id = c.handler_id;
                close();
                action = std::move(act);
            }
            sy += row_h;
        }
    }

    return action;
}

} // namespace navigator::ui

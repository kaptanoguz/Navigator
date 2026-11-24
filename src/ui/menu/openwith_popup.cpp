// src/ui/menu/openwith_popup.cpp — Phase 8 Open With popup.
#include "menu/openwith_popup.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "widgets/text_field.hpp"
#include "widgets/text_fit.hpp"

namespace navigator::ui {
namespace {

constexpr float kCardWidth = 460.0F;
constexpr float kHeaderHeight = 36.0F;
constexpr float kFieldHeight = 38.0F;
constexpr float kRowHeight = 36.0F;
constexpr float kFooterHeight = 38.0F;
constexpr std::size_t kVisibleRows = 8;

bool contains_folded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return fs::fold_name(haystack).find(fs::fold_name(needle)) != std::string::npos;
}

} // namespace

void OpenWithOverlay::open(std::vector<Path> paths, std::string primary_mime) {
    open_ = true;
    loading_ = true;
    always_default_ = false;
    paths_ = std::move(paths);
    primary_mime_ = std::move(primary_mime);
    handlers_.clear();
    filtered_.clear();
    selected_ = 0;
    scroll_ = 0;
    query_.set({});
    field_scroll_ = 0.0F;
}

void OpenWithOverlay::set_handlers(std::vector<openwith::Handler> handlers) {
    loading_ = false;
    handlers_ = std::move(handlers);
    refilter();
}

void OpenWithOverlay::close() noexcept {
    open_ = false;
    loading_ = false;
}

void OpenWithOverlay::query_changed() { refilter(); }

void OpenWithOverlay::refilter() {
    filtered_.clear();
    for (std::size_t i = 0; i < handlers_.size(); ++i) {
        if (contains_folded(handlers_[i].name, query_.text()) ||
            contains_folded(handlers_[i].desktop_id, query_.text()) ||
            contains_folded(handlers_[i].generic_name, query_.text())) {
            filtered_.push_back(i);
        }
    }
    selected_ = 0;
    scroll_ = 0;
}

std::optional<OpenWithAction> OpenWithOverlay::activate_selected() {
    if (loading_ || selected_ >= filtered_.size()) {
        return std::nullopt;
    }
    OpenWithAction act;
    act.handler = handlers_[filtered_[selected_]];
    act.paths = paths_;
    act.primary_mime = primary_mime_;
    act.set_as_default = always_default_;
    close();
    return act;
}

std::optional<OpenWithAction> OpenWithOverlay::handle_key(KeyChord chord) {
    if (!open_) {
        return std::nullopt;
    }
    if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
        close();
        return std::nullopt;
    }
    if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
        chord.mods == Modifiers::None) {
        return activate_selected();
    }
    const std::size_t n = filtered_.size();
    if (n > 0 && chord.mods == Modifiers::None) {
        if (chord.key == Key::Down) {
            selected_ = (selected_ + 1) % n;
            if (selected_ >= scroll_ + kVisibleRows) {
                scroll_ = selected_ - kVisibleRows + 1;
            } else if (selected_ < scroll_) {
                scroll_ = selected_;
            }
        } else if (chord.key == Key::Up) {
            selected_ = (selected_ == 0) ? (n - 1) : (selected_ - 1);
            if (selected_ < scroll_) {
                scroll_ = selected_;
            } else if (selected_ >= scroll_ + kVisibleRows) {
                scroll_ = selected_ - kVisibleRows + 1;
            }
        }
    }
    return std::nullopt;
}

std::optional<OpenWithAction> OpenWithOverlay::draw(const Painter& p, const Palette& pal,
                                                    const RectF& viewport,
                                                    const PointerInput& pointer) {
    if (!open_) {
        return std::nullopt;
    }
    Renderer& r = p.renderer();
    r.fill_rect(viewport, pal.shadow.with_alpha(0.42F));

    const float card_w = std::min(p.px(kCardWidth), viewport.w - p.px(32.0F));
    const float header_h = p.px(kHeaderHeight);
    const float field_h = p.px(kFieldHeight);
    const float row_h = p.px(kRowHeight);
    const float footer_h = p.px(kFooterHeight);
    const std::size_t rows = std::clamp<std::size_t>(filtered_.size(), 1, kVisibleRows);
    const float list_h = static_cast<float>(rows) * row_h + p.px(8.0F);
    const float card_h = header_h + field_h + list_h + footer_h;
    const RectF card{std::floor(viewport.x + (viewport.w - card_w) * 0.5F),
                     std::floor(viewport.y + std::min(p.px(96.0F), viewport.h * 0.16F)), card_w,
                     card_h};

    if (pointer.pressed && !contains(card, pointer.x, pointer.y)) {
        close();
        return std::nullopt;
    }

    const float rad = p.px(radius::lg);
    r.drop_shadow(card, rad, p.px(16.0F), p.px(5.0F), pal.shadow.with_alpha(0.35F));
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, p.px(1.0F), pal.border);

    // Header.
    const ShapedText title = p.shape(type::popup_title, tr("openwith.title"));
    p.draw(title, card.x + p.px(14.0F), card.y + header_h * 0.68F, pal.text_primary);
    if (!primary_mime_.empty()) {
        const ShapedText mt = p.shape(type::caption, primary_mime_);
        p.draw(mt, card.x + card.w - mt.width() - p.px(14.0F), card.y + header_h * 0.68F,
               pal.text_muted);
    }
    r.fill_rect(RectF{card.x, card.y + header_h, card.w, p.px(1.0F)}, pal.border);

    // Filter field.
    const RectF field_box{card.x + p.px(12.0F), card.y + header_h + p.px(3.0F),
                          card.w - p.px(24.0F), field_h - p.px(6.0F)};
    draw_text_field(p, pal, query_, field_box, tr("openwith.placeholder"), field_scroll_);
    r.fill_rect(RectF{card.x, card.y + header_h + field_h, card.w, p.px(1.0F)}, pal.border);

    // List.
    std::optional<OpenWithAction> action;
    const float list_top = card.y + header_h + field_h + p.px(4.0F);
    if (loading_) {
        const ShapedText lt = p.shape(type::body, tr("menu.loading"));
        p.draw(lt, card.x + p.px(14.0F), list_top + row_h * 0.65F, pal.text_muted);
    } else if (filtered_.empty()) {
        const ShapedText nt = p.shape(type::body, tr("menu.no_apps"));
        p.draw(nt, card.x + p.px(14.0F), list_top + row_h * 0.65F, pal.text_muted);
    } else {
        for (std::size_t vis = 0; vis < rows && scroll_ + vis < filtered_.size(); ++vis) {
            const std::size_t idx = scroll_ + vis;
            const openwith::Handler& h = handlers_[filtered_[idx]];
            const RectF rr{card.x + p.px(6.0F), list_top + static_cast<float>(vis) * row_h,
                           card.w - p.px(12.0F), row_h};
            const bool hov = contains(rr, pointer.x, pointer.y);
            if (idx == selected_) {
                r.fill_rounded_rect(rr, p.px(radius::sm), pal.selection);
            } else if (hov) {
                r.fill_rounded_rect(rr, p.px(radius::sm), pal.row_hover);
            }
            if (hov && pointer.pressed) {
                selected_ = idx;
                action = activate_selected();
            }
            const float base = rr.y + row_h * 0.65F;
            float right_edge = rr.x + rr.w - p.px(10.0F);
            if (h.is_default) {
                const ShapedText bt = p.shape(type::caption, tr("openwith.default_badge"));
                const float bw = bt.width() + p.px(10.0F);
                const float bh = p.px(18.0F);
                r.fill_rounded_rect(RectF{right_edge - bw, rr.y + (row_h - bh) * 0.5F, bw, bh},
                                    p.px(4.0F), pal.chip);
                p.draw(bt, right_edge - bw + p.px(5.0F), rr.y + (row_h + bh) * 0.5F - p.px(4.0F),
                       pal.chip_text);
                right_edge -= bw + p.px(8.0F);
            }
            const std::string fitted = TextFitter::elide(
                p, type::body, h.name, right_edge - rr.x - p.px(10.0F), Elide::Middle);
            p.draw(p.shape(type::body, fitted), rr.x + p.px(10.0F), base, pal.text_primary);
        }
    }

    // Footer: "Always open with this application" checkbox (when a primary MIME exists).
    const float footer_y = card.y + card.h - footer_h;
    r.fill_rect(RectF{card.x, footer_y, card.w, p.px(1.0F)}, pal.border);
    if (!primary_mime_.empty()) {
        const RectF cb_row{card.x + p.px(12.0F), footer_y + p.px(6.0F), card.w - p.px(24.0F),
                           footer_h - p.px(12.0F)};
        if (contains(cb_row, pointer.x, pointer.y) && pointer.pressed) {
            always_default_ = !always_default_;
        }
        const float cb_sz = p.px(16.0F);
        const RectF cb_box{cb_row.x, footer_y + (footer_h - cb_sz) * 0.5F, cb_sz, cb_sz};
        r.fill_rounded_rect(cb_box, p.px(3.0F), always_default_ ? pal.accent : pal.surface);
        r.stroke_rounded_rect(cb_box, p.px(3.0F), p.px(1.0F),
                              always_default_ ? pal.accent : pal.control_border);
        if (always_default_) {
            const ShapedText ck = p.shape(type::caption, "✓");
            p.draw(ck, cb_box.x + (cb_sz - ck.width()) * 0.5F, cb_box.y + cb_sz * 0.80F,
                   pal.on_accent);
        }
        p.draw(p.shape(type::caption, tr("openwith.always")), cb_row.x + cb_sz + p.px(8.0F),
               footer_y + footer_h * 0.65F, pal.text_secondary);
    }

    return action;
}

} // namespace navigator::ui

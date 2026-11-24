// src/ui/keymap/keymap_editor_view.cpp — Phase 8 Settings / Keymap Editor overlay.
#include "keymap/keymap_editor_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include <navigator/fs.hpp>
#include <navigator/i18n.hpp>

#include "input/shortcut_text.hpp"
#include "widgets/text_field.hpp"
#include "widgets/text_fit.hpp"

namespace navigator::ui {
namespace {

constexpr float kCardWidth = 760.0F;
constexpr float kCardHeight = 560.0F;
constexpr float kHeaderHeight = 44.0F;
constexpr float kSearchHeight = 40.0F;
constexpr float kRowHeight = 34.0F;
constexpr float kFooterHeight = 44.0F;

bool contains_folded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return fs::fold_name(haystack).find(fs::fold_name(needle)) != std::string::npos;
}

std::string_view warning_key(BindingWarning w) noexcept {
    switch (w) {
    case BindingWarning::DestructiveBareKey: return "keymap.warn.destructive_bare";
    case BindingWarning::ShadowsTypeToFilter: return "keymap.warn.shadows_filter";
    case BindingWarning::ConsumedByTextFields: return "keymap.warn.text_field";
    case BindingWarning::LayoutSensitivePunctuation: return "keymap.warn.layout_punct";
    }
    return "keymap.warn.layout_punct";
}

} // namespace

void KeymapSettingsOverlay::open(const CommandRegistry& registry, const Keymap& effective) {
    open_ = true;
    capturing_ = false;
    captured_seq_.chords.clear();
    check_.reset();
    search_.set({});
    field_scroll_ = 0.0F;
    selected_ = 0;
    scroll_ = 0;
    rebuild_rows(registry, effective);
}

void KeymapSettingsOverlay::close() noexcept {
    open_ = false;
    capturing_ = false;
    captured_seq_.chords.clear();
    check_.reset();
}

void KeymapSettingsOverlay::refresh(const CommandRegistry& registry, const Keymap& effective) {
    if (!open_) {
        return;
    }
    rebuild_rows(registry, effective);
}

void KeymapSettingsOverlay::rebuild_rows(const CommandRegistry& registry, const Keymap& effective) {
    rows_.clear();
    for (const CommandInfo& info : registry.all()) {
        if (!info.bindable) {
            continue;
        }
        Row row;
        row.command = std::string(info.name);
        row.arg = 0;
        row.title = std::string(tr(info.title_key));
        row.category = std::string(tr(category_title_key(info.category)));
        row.shortcut = primary_shortcut_text(effective, info.name, 0, false);
        rows_.push_back(std::move(row));
    }
    for (const CommandVariant& v : builtin_command_variants()) {
        const CommandInfo* info = registry.find(v.id);
        if (info == nullptr || !info->bindable) {
            continue;
        }
        Row row;
        row.command = std::string(info->name);
        row.arg = v.arg;
        row.title = std::string(tr(v.title_key));
        row.category = std::string(tr(category_title_key(info->category)));
        row.shortcut = primary_shortcut_text(effective, info->name, v.arg, false);
        rows_.push_back(std::move(row));
    }
    refilter();
}

void KeymapSettingsOverlay::search_changed() { refilter(); }

void KeymapSettingsOverlay::refilter() {
    filtered_.clear();
    const std::string_view q = search_.text();
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (contains_folded(rows_[i].title, q) || contains_folded(rows_[i].command, q) ||
            contains_folded(rows_[i].category, q) || contains_folded(rows_[i].shortcut, q)) {
            filtered_.push_back(i);
        }
    }
    if (selected_ >= filtered_.size()) {
        selected_ = 0;
    }
    scroll_ = 0;
}

void KeymapSettingsOverlay::begin_capture(std::size_t filtered_idx) {
    if (filtered_idx >= filtered_.size()) {
        return;
    }
    capturing_ = true;
    capture_row_ = filtered_[filtered_idx];
    captured_seq_.chords.clear();
    check_.reset();
}

void KeymapSettingsOverlay::cancel_capture() noexcept {
    capturing_ = false;
    captured_seq_.chords.clear();
    check_.reset();
}

KeymapEditorEvent KeymapSettingsOverlay::commit_capture(KeymapFile& file, const Keymap& effective,
                                                        const Keymap& defaults,
                                                        const CommandRegistry& registry,
                                                        bool replace_conflicts) {
    KeymapEditorEvent ev;
    if (!capturing_ || capture_row_ >= rows_.size() || captured_seq_.empty()) {
        cancel_capture();
        return ev;
    }
    const Row& row = rows_[capture_row_];
    const auto res = set_binding(file, effective, defaults, registry, captured_seq_, row.command,
                                 row.arg, replace_conflicts);
    if (res.has_value()) {
        ev.keymap_dirty = true;
        cancel_capture();
    } else {
        // Recompute check so conflicts/errors stay visible.
        check_ = check_binding(effective, registry, captured_seq_, row.command, row.arg);
    }
    return ev;
}

KeymapEditorEvent KeymapSettingsOverlay::handle_key(KeyChord chord, KeymapFile& file,
                                                    const Keymap& effective, const Keymap& defaults,
                                                    const CommandRegistry& registry) {
    KeymapEditorEvent ev;
    if (!open_) {
        return ev;
    }

    if (capturing_) {
        if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
            cancel_capture();
            return ev;
        }
        if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
            chord.mods == Modifiers::None && !captured_seq_.empty()) {
            const bool has_conflicts = check_.has_value() && !check_->conflicts.empty();
            return commit_capture(file, effective, defaults, registry, has_conflicts);
        }
        if (chord.key == Key::Backspace && chord.mods == Modifiers::None &&
            !captured_seq_.empty()) {
            captured_seq_.chords.pop_back();
            if (captured_seq_.empty()) {
                check_.reset();
            } else {
                const Row& row = rows_[capture_row_];
                check_ = check_binding(effective, registry, captured_seq_, row.command, row.arg);
            }
            return ev;
        }
        if (captured_seq_.size() < kMaxSequenceLength) {
            captured_seq_.chords.push_back(chord);
            const Row& row = rows_[capture_row_];
            check_ = check_binding(effective, registry, captured_seq_, row.command, row.arg);
        }
        return ev;
    }

    if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
        if (!search_.text().empty()) {
            search_.set({});
            refilter();
        } else {
            close();
        }
        return ev;
    }
    const std::size_t n = filtered_.size();
    if (n > 0 && chord.mods == Modifiers::None) {
        if (chord.key == Key::Down) {
            selected_ = (selected_ + 1) % n;
        } else if (chord.key == Key::Up) {
            selected_ = (selected_ == 0) ? (n - 1) : (selected_ - 1);
        } else if (chord.key == Key::Enter || chord.key == Key::KeypadEnter) {
            begin_capture(selected_);
        }
    }
    return ev;
}

KeymapEditorEvent KeymapSettingsOverlay::draw(const Painter& p, const Palette& pal,
                                              const RectF& viewport, const PointerInput& pointer,
                                              KeymapFile& file, const Keymap& effective,
                                              const Keymap& defaults,
                                              const CommandRegistry& registry) {
    KeymapEditorEvent ev;
    if (!open_) {
        return ev;
    }
    Renderer& r = p.renderer();
    r.fill_rect(viewport, pal.shadow.with_alpha(0.48F));

    const float card_w = std::min(p.px(kCardWidth), viewport.w - p.px(32.0F));
    const float card_h = std::min(p.px(kCardHeight), viewport.h - p.px(32.0F));
    const RectF card{std::floor(viewport.x + (viewport.w - card_w) * 0.5F),
                     std::floor(viewport.y + (viewport.h - card_h) * 0.5F), card_w, card_h};

    if (pointer.pressed && !contains(card, pointer.x, pointer.y)) {
        close();
        return ev;
    }

    const float rad = p.px(radius::lg);
    r.drop_shadow(card, rad, p.px(18.0F), p.px(6.0F), pal.shadow.with_alpha(0.35F));
    r.fill_rounded_rect(card, rad, pal.surface_raised);
    r.stroke_rounded_rect(card, rad, p.px(1.0F), pal.border);

    const float header_h = p.px(kHeaderHeight);
    const float search_h = p.px(kSearchHeight);
    const float footer_h = p.px(kFooterHeight);
    const float row_h = p.px(kRowHeight);

    // Header: "Settings — Keyboard" + Export + Reset Defaults buttons.
    const std::string hdr =
        std::string(tr("settings.title")) + " — " + std::string(tr("settings.page.keyboard"));
    p.draw(p.shape(type::popup_title, hdr), card.x + p.px(16.0F), card.y + header_h * 0.66F,
           pal.text_primary);

    auto draw_button = [&](std::string_view label, float right_x, float cy, bool danger) -> RectF {
        const ShapedText bt = p.shape(type::caption, label);
        const float bw = bt.width() + p.px(16.0F);
        const float bh = p.px(26.0F);
        const RectF br{right_x - bw, cy - bh * 0.5F, bw, bh};
        const bool hov = contains(br, pointer.x, pointer.y);
        r.fill_rounded_rect(br, p.px(5.0F), hov ? pal.control_hover : pal.surface);
        r.stroke_rounded_rect(br, p.px(5.0F), p.px(1.0F), danger ? pal.danger_border : pal.border);
        p.draw(bt, br.x + p.px(8.0F), br.y + bh * 0.68F, danger ? pal.danger : pal.text_primary);
        return br;
    };

    const RectF reset_all_btn = draw_button(tr("keymap.reset_all"), card.x + card.w - p.px(12.0F),
                                            card.y + header_h * 0.5F, true);
    if (contains(reset_all_btn, pointer.x, pointer.y) && pointer.pressed) {
        ev.enqueue_cmd = Command{CommandId::ResetKeymap, 0};
        close();
        return ev;
    }
    const RectF export_btn = draw_button(tr("cmd.keymap.export"), reset_all_btn.x - p.px(8.0F),
                                         card.y + header_h * 0.5F, false);
    if (contains(export_btn, pointer.x, pointer.y) && pointer.pressed) {
        ev.enqueue_cmd = Command{CommandId::ExportKeymap, 0};
    }

    r.fill_rect(RectF{card.x, card.y + header_h, card.w, p.px(1.0F)}, pal.border);

    // Search bar.
    const RectF search_box{card.x + p.px(14.0F), card.y + header_h + p.px(4.0F),
                           card.w - p.px(28.0F), search_h - p.px(8.0F)};
    draw_text_field(p, pal, search_, search_box, tr("keymap.search_placeholder"), field_scroll_);
    r.fill_rect(RectF{card.x, card.y + header_h + search_h, card.w, p.px(1.0F)}, pal.border);

    // Capture banner when capturing a new shortcut.
    float list_top = card.y + header_h + search_h + p.px(4.0F);
    if (capturing_ && capture_row_ < rows_.size()) {
        const float banner_h = p.px(66.0F);
        const RectF banner{card.x + p.px(12.0F), list_top + p.px(2.0F), card.w - p.px(24.0F),
                           banner_h};
        r.fill_rounded_rect(banner, p.px(6.0F), pal.surface);
        r.stroke_rounded_rect(banner, p.px(6.0F), p.px(1.5F), pal.accent);

        const std::string seq_str = captured_seq_.empty() ? std::string(tr("keymap.capture_prompt"))
                                                          : shortcut_text(captured_seq_);
        p.draw(p.shape(type::body, rows_[capture_row_].title + ": " + seq_str),
               banner.x + p.px(12.0F), banner.y + p.px(24.0F), pal.text_primary);

        // Status / conflict / warning line.
        if (check_) {
            std::string note;
            Color note_col = pal.text_secondary;
            if (check_->error) {
                note = check_->error->message;
                note_col = pal.danger;
            } else if (!check_->conflicts.empty()) {
                const SequenceBinding& c0 = check_->conflicts.front().existing;
                note = shortcut_text(c0.sequence) + " -> " + c0.command;
                note_col = pal.warning;
                const RectF rep_btn =
                    draw_button(tr("keymap.conflict_replace"), banner.x + banner.w - p.px(10.0F),
                                banner.y + p.px(44.0F), false);
                if (contains(rep_btn, pointer.x, pointer.y) && pointer.pressed) {
                    return commit_capture(file, effective, defaults, registry, true);
                }
            } else if (!check_->warnings.empty()) {
                note = std::string(tr(warning_key(check_->warnings.front())));
                note_col = pal.warning;
            }
            if (!note.empty()) {
                p.draw(p.shape(type::caption, note), banner.x + p.px(12.0F), banner.y + p.px(48.0F),
                       note_col);
            }
        }
        list_top += banner_h + p.px(8.0F);
    }

    // Command list.
    const float list_bottom = card.y + card.h - footer_h - p.px(4.0F);
    const std::size_t vis_rows =
        static_cast<std::size_t>(std::max(1.0F, std::floor((list_bottom - list_top) / row_h)));
    if (contains(card, pointer.x, pointer.y) && pointer.wheel_y != 0.0F &&
        filtered_.size() > vis_rows) {
        const std::size_t max_sc = filtered_.size() - vis_rows;
        if (pointer.wheel_y < 0.0F) {
            scroll_ = std::min(max_sc, scroll_ + 2);
        } else {
            scroll_ = scroll_ > 2 ? scroll_ - 2 : 0;
        }
    }
    if (selected_ < scroll_) {
        scroll_ = selected_;
    } else if (selected_ >= scroll_ + vis_rows) {
        scroll_ = selected_ - vis_rows + 1;
    }

    for (std::size_t vis = 0; vis < vis_rows && scroll_ + vis < filtered_.size(); ++vis) {
        const std::size_t idx = scroll_ + vis;
        const Row& row = rows_[filtered_[idx]];
        const RectF rr{card.x + p.px(8.0F), list_top + static_cast<float>(vis) * row_h,
                       card.w - p.px(16.0F), row_h};
        const bool hov = contains(rr, pointer.x, pointer.y);
        if (idx == selected_) {
            r.fill_rounded_rect(rr, p.px(radius::sm), pal.selection);
        } else if (hov) {
            r.fill_rounded_rect(rr, p.px(radius::sm), pal.row_hover);
        }

        // Per-command Reset button on hover/selection.
        float right_edge = rr.x + rr.w - p.px(8.0F);
        bool clicked_reset = false;
        if (hov || idx == selected_) {
            const RectF rbtn =
                draw_button(tr("keymap.reset_cmd"), right_edge, rr.y + row_h * 0.5F, false);
            if (contains(rbtn, pointer.x, pointer.y) && pointer.pressed) {
                clicked_reset = true;
                if (reset_command(file, row.command)) {
                    ev.keymap_dirty = true;
                }
            }
            right_edge = rbtn.x - p.px(8.0F);
        }

        // Shortcut chip.
        if (!row.shortcut.empty()) {
            const ShapedText sc = p.shape(type::caption, row.shortcut);
            const float cw = sc.width() + p.px(12.0F);
            const float ch = p.px(20.0F);
            const RectF cr{right_edge - cw, rr.y + (row_h - ch) * 0.5F, cw, ch};
            r.fill_rounded_rect(cr, p.px(4.0F), pal.surface);
            r.stroke_rounded_rect(cr, p.px(4.0F), p.px(1.0F), pal.keycap_border);
            p.draw(sc, cr.x + p.px(6.0F), cr.y + ch * 0.72F, pal.keycap_text);
            right_edge = cr.x - p.px(10.0F);
        }

        const float base = rr.y + row_h * 0.65F;
        float lx = rr.x + p.px(10.0F);
        const std::string cat = row.category + ": ";
        const ShapedText ct = p.shape(type::caption, cat);
        p.draw(ct, lx, base, pal.text_muted);
        lx += ct.width();
        const std::string fitted = TextFitter::elide(
            p, type::body, row.title, std::max(0.0F, right_edge - lx), Elide::Middle);
        p.draw(p.shape(type::body, fitted), lx, base, pal.text_primary);

        if (hov && pointer.pressed && !clicked_reset) {
            selected_ = idx;
            begin_capture(idx);
        }
    }

    // Footer: Sequence timeout stepper + aliases count.
    const float footer_y = card.y + card.h - footer_h;
    r.fill_rect(RectF{card.x, footer_y, card.w, p.px(1.0F)}, pal.border);
    const std::string timeout_str = std::string(tr("keymap.timeout_label")) + ": " +
                                    std::to_string(file.sequence_timeout.count()) + " ms";
    p.draw(p.shape(type::caption, timeout_str), card.x + p.px(14.0F), footer_y + footer_h * 0.64F,
           pal.text_secondary);

    const RectF plus_btn =
        draw_button("+", card.x + p.px(270.0F), footer_y + footer_h * 0.5F, false);
    const RectF minus_btn =
        draw_button("-", plus_btn.x - p.px(6.0F), footer_y + footer_h * 0.5F, false);
    if (contains(plus_btn, pointer.x, pointer.y) && pointer.pressed) {
        set_sequence_timeout(file, file.sequence_timeout + std::chrono::milliseconds{100});
        ev.keymap_dirty = true;
    } else if (contains(minus_btn, pointer.x, pointer.y) && pointer.pressed) {
        set_sequence_timeout(file, file.sequence_timeout - std::chrono::milliseconds{100});
        ev.keymap_dirty = true;
    }

    const std::string alias_info =
        std::string(tr("keymap.aliases")) + ": " + std::to_string(file.aliases.size());
    const ShapedText at = p.shape(type::caption, alias_info);
    p.draw(at, card.x + card.w - at.width() - p.px(14.0F), footer_y + footer_h * 0.64F,
           pal.text_muted);

    return ev;
}

} // namespace navigator::ui

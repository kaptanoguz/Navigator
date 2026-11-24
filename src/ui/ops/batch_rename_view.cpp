// src/ui/ops/batch_rename_view.cpp — Phase 9B interactive Batch Rename overlay.
#include "ops/batch_rename_view.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include <navigator/i18n.hpp>

#include "widgets/text_field.hpp"

namespace navigator::ui {
namespace {

constexpr std::array<std::string_view, 6> kQuickTokens = {
    "{stem}", "{ext}", "{counter:3}", "{date}", "{uid:6}", "{parent}",
};

[[nodiscard]] std::string_view case_label_key(ops::CaseTransform ct) noexcept {
    switch (ct) {
    case ops::CaseTransform::None: return "batch_rename.case_none";
    case ops::CaseTransform::Lower: return "batch_rename.case_lower";
    case ops::CaseTransform::Upper: return "batch_rename.case_upper";
    case ops::CaseTransform::Title: return "batch_rename.case_title";
    }
    return "batch_rename.case_none";
}

[[nodiscard]] ops::CaseTransform next_case(ops::CaseTransform ct) noexcept {
    switch (ct) {
    case ops::CaseTransform::None: return ops::CaseTransform::Lower;
    case ops::CaseTransform::Lower: return ops::CaseTransform::Upper;
    case ops::CaseTransform::Upper: return ops::CaseTransform::Title;
    case ops::CaseTransform::Title: return ops::CaseTransform::None;
    }
    return ops::CaseTransform::None;
}

[[nodiscard]] std::string_view status_key(ops::PreviewRowStatus s) noexcept {
    switch (s) {
    case ops::PreviewRowStatus::Unchanged: return "batch_rename.status_unchanged";
    case ops::PreviewRowStatus::Ready: return "batch_rename.status_ready";
    case ops::PreviewRowStatus::InvalidName: return "batch_rename.status_invalid";
    case ops::PreviewRowStatus::CollisionInternal:
    case ops::PreviewRowStatus::CollisionExisting: return "batch_rename.status_collision";
    }
    return "batch_rename.status_unchanged";
}

[[nodiscard]] constexpr RectF inset_rect(const RectF& r, float dx, float dy) noexcept {
    return RectF{r.x + dx, r.y + dy, std::max(0.0F, r.w - (2.0F * dx)),
                 std::max(0.0F, r.h - (2.0F * dy))};
}

void draw_chip_button(const Painter& p, const Palette& pal, const RectF& r, std::string_view label,
                      bool active, bool hovered, bool primary, bool disabled) {
    Renderer& ren = p.renderer();
    const float rad = p.px(p.radii().sm);
    Color bg = pal.surface_raised;
    Color fg = pal.text_primary;
    Color border = pal.border;
    if (disabled) {
        bg = pal.surface;
        fg = pal.text_muted;
    } else if (primary) {
        bg = hovered ? mix(pal.accent, pal.on_accent, 0.12F) : pal.accent;
        fg = pal.on_accent;
        border = bg;
    } else if (active) {
        bg = pal.chip;
        fg = pal.chip_text;
        border = pal.accent;
    } else if (hovered) {
        bg = mix(pal.surface_raised, pal.text_primary, 0.08F);
    }
    ren.fill_rounded_rect(r, rad, bg);
    ren.stroke_rounded_rect(r, rad, std::max(1.0F, std::round(p.px(1.0F))), border);
    const ShapedText t = p.shape(type::caption, label);
    const float tx = std::round(r.x + ((r.w - t.width()) * 0.5F));
    const float ty = std::round(r.y + ((r.h + t.cap_height()) * 0.5F));
    p.draw(t, tx, ty, fg);
}

} // namespace

TextEdit* BatchRenameOverlay::active_field() noexcept {
    if (!open_) {
        return nullptr;
    }
    switch (focus_) {
    case FocusField::Template: return &template_edit_;
    case FocusField::Find: return &find_edit_;
    case FocusField::Replace: return &replace_edit_;
    }
    return &template_edit_;
}

void BatchRenameOverlay::open(std::vector<ops::BatchRenameItem> items,
                              std::vector<std::string> existing_sibling_names) {
    open_ = true;
    items_ = std::move(items);
    siblings_ = std::move(existing_sibling_names);
    manual_overrides_.clear();
    rule_ = ops::BatchRenameRule{};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    rule_.uid_seed = static_cast<std::uint64_t>(now) ^ 0x9E3779B97F4A7C15ULL;
    template_edit_.set(rule_.template_str, true);
    find_edit_.set("", false);
    replace_edit_.set("", false);
    template_scroll_ = 0.0F;
    find_scroll_ = 0.0F;
    replace_scroll_ = 0.0F;
    focus_ = FocusField::Template;
    scroll_ = 0;
    rebuild_plan();
}

void BatchRenameOverlay::close() noexcept {
    open_ = false;
    items_.clear();
    siblings_.clear();
    manual_overrides_.clear();
    plan_ = {};
}

void BatchRenameOverlay::field_changed() {
    if (!open_) {
        return;
    }
    manual_overrides_.clear();
    rule_.template_str = template_edit_.text();
    rule_.find_text = find_edit_.text();
    rule_.replace_text = replace_edit_.text();
    rebuild_plan();
}

void BatchRenameOverlay::paste_text(std::string_view clipboard_text) {
    if (!open_ || clipboard_text.empty()) {
        return;
    }
    const ops::BatchPasteResult res = ops::interpret_batch_paste(clipboard_text, items_, true);
    if (res.is_single_line) {
        manual_overrides_.clear();
        rule_.template_str = res.single_line_template;
        template_edit_.set(rule_.template_str, false);
    } else if (!res.line_overrides.empty()) {
        manual_overrides_ = res.line_overrides;
    }
    rebuild_plan();
}

void BatchRenameOverlay::insert_token(std::string_view token) {
    manual_overrides_.clear();
    focus_ = FocusField::Template;
    template_edit_.insert(token);
    rule_.template_str = template_edit_.text();
    rebuild_plan();
}

void BatchRenameOverlay::reset_rule() {
    manual_overrides_.clear();
    const std::uint64_t seed = rule_.uid_seed;
    rule_ = ops::BatchRenameRule{};
    rule_.uid_seed = seed;
    template_edit_.set(rule_.template_str, true);
    find_edit_.set("", false);
    replace_edit_.set("", false);
    focus_ = FocusField::Template;
    scroll_ = 0;
    rebuild_plan();
}

void BatchRenameOverlay::rebuild_plan() {
    plan_ = ops::build_batch_rename_plan(items_, rule_, manual_overrides_, siblings_);
}

BatchRenameEvent BatchRenameOverlay::handle_key(KeyChord chord) {
    BatchRenameEvent ev;
    if (!open_) {
        return ev;
    }
    if (chord.key == Key::Escape && chord.mods == Modifiers::None) {
        close();
        return ev;
    }
    if ((chord.key == Key::Enter || chord.key == Key::KeypadEnter) &&
        has(chord.mods, Modifiers::Ctrl)) {
        if (plan_.can_apply()) {
            ev.apply_request = ops::make_batch_rename_request(items_, plan_);
            close();
        }
        return ev;
    }
    if (chord.key == Key::Tab && chord.mods == Modifiers::None) {
        if (focus_ == FocusField::Template) {
            focus_ = FocusField::Find;
        } else if (focus_ == FocusField::Find) {
            focus_ = FocusField::Replace;
        } else {
            focus_ = FocusField::Template;
        }
        return ev;
    }
    if (chord.key == Key::Tab && chord.mods == Modifiers::Shift) {
        if (focus_ == FocusField::Template) {
            focus_ = FocusField::Replace;
        } else if (focus_ == FocusField::Replace) {
            focus_ = FocusField::Find;
        } else {
            focus_ = FocusField::Template;
        }
        return ev;
    }
    if (chord.key == Key::V && chord.mods == (Modifiers::Ctrl | Modifiers::Shift)) {
        ev.request_paste_clipboard = true;
        return ev;
    }
    if (chord.key == Key::PageDown) {
        scroll_ = std::min(scroll_ + 10, items_.size() > 1 ? items_.size() - 1 : 0);
        return ev;
    }
    if (chord.key == Key::PageUp) {
        scroll_ = scroll_ > 10 ? scroll_ - 10 : 0;
        return ev;
    }
    if (chord.key == Key::Down && chord.mods == Modifiers::None) {
        if (scroll_ + 1 < items_.size()) {
            ++scroll_;
        }
        return ev;
    }
    if (chord.key == Key::Up && chord.mods == Modifiers::None) {
        if (scroll_ > 0) {
            --scroll_;
        }
        return ev;
    }
    return ev;
}

BatchRenameEvent BatchRenameOverlay::draw(const Painter& p, const Palette& pal,
                                          const RectF& viewport, const PointerInput& pointer) {
    BatchRenameEvent ev;
    if (!open_) {
        return ev;
    }

    Renderer& ren = p.renderer();
    ren.fill_rect(viewport, Color{0.0F, 0.0F, 0.0F, 0.50F});

    const float card_w = std::min(viewport.w - p.px(48.0F), p.px(860.0F));
    const float card_h = std::min(viewport.h - p.px(48.0F), p.px(620.0F));
    const RectF card{std::round(viewport.x + ((viewport.w - card_w) * 0.5F)),
                     std::round(viewport.y + ((viewport.h - card_h) * 0.5F)), card_w, card_h};

    const float pad = p.px(space::md);
    const float rad_lg = p.px(p.radii().lg);
    const float rad_sm = p.px(p.radii().sm);
    const float border_px = std::max(1.0F, std::round(p.px(1.0F)));

    ren.fill_rounded_rect(card, rad_lg, pal.surface);
    ren.stroke_rounded_rect(card, rad_lg, border_px, pal.border);

    // Click outside closes.
    if (pointer.pressed && !contains(card, pointer.x, pointer.y)) {
        close();
        return ev;
    }

    // Header bar.
    float y = card.y + pad;
    const std::string title_str =
        std::string(tr("batch_rename.title")) + " (" + std::to_string(items_.size()) + ")";
    const ShapedText title_t = p.shape(type::popup_title, title_str);
    p.draw(title_t, card.x + pad, y + p.px(16.0F), pal.text_primary);

    const float hdr_btn_h = p.px(26.0F);
    const float paste_w = p.px(108.0F);
    const float reset_w = p.px(74.0F);
    const RectF reset_btn{card.right() - pad - reset_w, y, reset_w, hdr_btn_h};
    const RectF paste_btn{reset_btn.x - p.px(8.0F) - paste_w, y, paste_w, hdr_btn_h};
    draw_chip_button(p, pal, paste_btn, tr("batch_rename.paste"), false,
                     contains(paste_btn, pointer.x, pointer.y), false, false);
    draw_chip_button(p, pal, reset_btn, tr("batch_rename.reset"), false,
                     contains(reset_btn, pointer.x, pointer.y), false, false);
    if (pointer.pressed) {
        if (contains(paste_btn, pointer.x, pointer.y)) {
            ev.request_paste_clipboard = true;
        } else if (contains(reset_btn, pointer.x, pointer.y)) {
            reset_rule();
        }
    }

    y += hdr_btn_h + p.px(10.0F);

    // Template field + quick token chips.
    const float label_w = p.px(76.0F);
    const float field_h = p.px(28.0F);
    const ShapedText tpl_lbl = p.shape(type::caption, tr("batch_rename.template"));
    p.draw(tpl_lbl, card.x + pad, y + p.px(18.0F), pal.text_secondary);

    const RectF tpl_box{card.x + pad + label_w, y, card.w - (2.0F * pad) - label_w, field_h};
    ren.fill_rounded_rect(tpl_box, rad_sm, pal.surface_raised);
    ren.stroke_rounded_rect(tpl_box, rad_sm, border_px,
                            focus_ == FocusField::Template ? pal.accent : pal.border);
    draw_text_field(p, pal, template_edit_, inset_rect(tpl_box, p.px(6.0F), 0.0F), "{name}",
                    template_scroll_, TextFieldStyle{&type::row, 15.0F, focus_ == FocusField::Template});
    if (pointer.pressed && contains(tpl_box, pointer.x, pointer.y)) {
        focus_ = FocusField::Template;
    }

    y += field_h + p.px(6.0F);

    // Quick token chips row.
    float cx = card.x + pad + label_w;
    const float chip_h = p.px(22.0F);
    for (std::string_view tok : kQuickTokens) {
        const ShapedText tt = p.shape(type::caption, tok);
        const float cw = tt.width() + p.px(14.0F);
        const RectF cr{cx, y, cw, chip_h};
        const bool hov = contains(cr, pointer.x, pointer.y);
        draw_chip_button(p, pal, cr, tok, false, hov, false, false);
        if (pointer.pressed && hov) {
            insert_token(tok);
        }
        cx += cw + p.px(6.0F);
    }

    y += chip_h + p.px(8.0F);

    // Find & Replace row.
    const float half_w = (card.w - (2.0F * pad) - (2.0F * label_w) - p.px(12.0F)) * 0.5F;
    const ShapedText find_lbl = p.shape(type::caption, tr("batch_rename.find"));
    p.draw(find_lbl, card.x + pad, y + p.px(18.0F), pal.text_secondary);
    const RectF find_box{card.x + pad + label_w, y, std::max(p.px(80.0F), half_w), field_h};
    ren.fill_rounded_rect(find_box, rad_sm, pal.surface_raised);
    ren.stroke_rounded_rect(find_box, rad_sm, border_px,
                            focus_ == FocusField::Find ? pal.accent : pal.border);
    draw_text_field(p, pal, find_edit_, inset_rect(find_box, p.px(6.0F), 0.0F),
                    tr("batch_rename.find_placeholder"), find_scroll_,
                    TextFieldStyle{&type::row, 15.0F, focus_ == FocusField::Find});
    if (pointer.pressed && contains(find_box, pointer.x, pointer.y)) {
        focus_ = FocusField::Find;
    }

    const float rep_lbl_x = find_box.right() + p.px(12.0F);
    const ShapedText rep_lbl = p.shape(type::caption, tr("batch_rename.replace"));
    p.draw(rep_lbl, rep_lbl_x, y + p.px(18.0F), pal.text_secondary);
    const RectF rep_box{rep_lbl_x + label_w, y, card.right() - pad - (rep_lbl_x + label_w), field_h};
    ren.fill_rounded_rect(rep_box, rad_sm, pal.surface_raised);
    ren.stroke_rounded_rect(rep_box, rad_sm, border_px,
                            focus_ == FocusField::Replace ? pal.accent : pal.border);
    draw_text_field(p, pal, replace_edit_, inset_rect(rep_box, p.px(6.0F), 0.0F),
                    tr("batch_rename.replace_placeholder"), replace_scroll_,
                    TextFieldStyle{&type::row, 15.0F, focus_ == FocusField::Replace});
    if (pointer.pressed && contains(rep_box, pointer.x, pointer.y)) {
        focus_ = FocusField::Replace;
    }

    y += field_h + p.px(8.0F);

    // Options row: Case transform, Trim spaces, Match case.
    float ox = card.x + pad;
    const float opt_h = p.px(24.0F);
    const std::string case_text =
        std::string(tr("batch_rename.case")) + ": " + std::string(tr(case_label_key(rule_.case_transform)));
    const RectF case_btn{ox, y, p.shape(type::caption, case_text).width() + p.px(18.0F), opt_h};
    draw_chip_button(p, pal, case_btn, case_text, rule_.case_transform != ops::CaseTransform::None,
                     contains(case_btn, pointer.x, pointer.y), false, false);
    if (pointer.pressed && contains(case_btn, pointer.x, pointer.y)) {
        rule_.case_transform = next_case(rule_.case_transform);
        rebuild_plan();
    }
    ox = case_btn.right() + p.px(8.0F);

    const std::string_view trim_text = tr("batch_rename.trim");
    const RectF trim_btn{ox, y, p.shape(type::caption, trim_text).width() + p.px(18.0F), opt_h};
    draw_chip_button(p, pal, trim_btn, trim_text, rule_.trim_whitespace,
                     contains(trim_btn, pointer.x, pointer.y), false, false);
    if (pointer.pressed && contains(trim_btn, pointer.x, pointer.y)) {
        rule_.trim_whitespace = !rule_.trim_whitespace;
        rebuild_plan();
    }
    ox = trim_btn.right() + p.px(8.0F);

    const std::string_view match_case_text = tr("batch_rename.match_case");
    const RectF mc_btn{ox, y, p.shape(type::caption, match_case_text).width() + p.px(18.0F), opt_h};
    draw_chip_button(p, pal, mc_btn, match_case_text, rule_.find_case_sensitive,
                     contains(mc_btn, pointer.x, pointer.y), false, false);
    if (pointer.pressed && contains(mc_btn, pointer.x, pointer.y)) {
        rule_.find_case_sensitive = !rule_.find_case_sensitive;
        rebuild_plan();
    }

    y += opt_h + p.px(10.0F);

    // Virtualized preview table.
    const float footer_h = p.px(44.0F);
    const RectF table_rect{card.x + pad, y, card.w - (2.0F * pad),
                           std::max(p.px(80.0F), card.bottom() - footer_h - y - p.px(6.0F))};
    ren.fill_rounded_rect(table_rect, rad_sm, pal.background);
    ren.stroke_rounded_rect(table_rect, rad_sm, border_px, pal.border);

    const float hdr_h = p.px(24.0F);
    const float row_h = p.px(24.0F);
    const float col_idx_w = p.px(38.0F);
    const float col_status_w = p.px(94.0F);
    const float col_arrow_w = p.px(24.0F);
    const float col_name_w =
        std::max(p.px(80.0F), (table_rect.w - col_idx_w - col_status_w - col_arrow_w) * 0.5F);

    // Table header.
    const RectF th{table_rect.x, table_rect.y, table_rect.w, hdr_h};
    ren.fill_rect(th, pal.surface_raised);
    ren.fill_rect(RectF{th.x, th.bottom() - border_px, th.w, border_px}, pal.border);
    const float th_base = th.y + p.px(16.0F);
    p.draw(p.shape(type::column_header, "#"), th.x + p.px(8.0F), th_base, pal.text_muted);
    p.draw(p.shape(type::column_header, tr("batch_rename.col_original")),
           th.x + col_idx_w + p.px(6.0F), th_base, pal.text_secondary);
    p.draw(p.shape(type::column_header, tr("batch_rename.col_proposed")),
           th.x + col_idx_w + col_name_w + col_arrow_w + p.px(6.0F), th_base, pal.text_secondary);
    p.draw(p.shape(type::column_header, tr("batch_rename.col_status")),
           th.right() - col_status_w + p.px(6.0F), th_base, pal.text_secondary);

    const RectF body_rect{table_rect.x, th.bottom(), table_rect.w, table_rect.h - hdr_h};
    const std::size_t visible_rows =
        static_cast<std::size_t>(std::max(1.0F, std::floor(body_rect.h / row_h)));
    if (contains(body_rect, pointer.x, pointer.y) && pointer.wheel_y != 0.0F) {
        const int delta = pointer.wheel_y > 0.0F ? -3 : 3;
        const int max_s = static_cast<int>(items_.size() > visible_rows ? items_.size() - visible_rows : 0);
        scroll_ = static_cast<std::size_t>(std::clamp(static_cast<int>(scroll_) + delta, 0, max_s));
    }
    if (items_.size() > visible_rows) {
        scroll_ = std::min(scroll_, items_.size() - visible_rows);
    } else {
        scroll_ = 0;
    }

    const auto prev_clip = ren.clip();
    ren.set_clip(body_rect);
    const std::size_t end_row = std::min(items_.size(), scroll_ + visible_rows + 1);
    for (std::size_t i = scroll_; i < end_row; ++i) {
        const float ry = body_rect.y + (static_cast<float>(i - scroll_) * row_h);
        const RectF rr{body_rect.x, ry, body_rect.w, row_h};
        const ops::BatchRenamePreviewRow& pr = plan_.rows[i];
        const bool is_err = pr.status == ops::PreviewRowStatus::CollisionInternal ||
                            pr.status == ops::PreviewRowStatus::CollisionExisting ||
                            pr.status == ops::PreviewRowStatus::InvalidName;
        if (is_err) {
            ren.fill_rect(rr, pal.danger_surface);
        } else if ((i & 1U) != 0U) {
            ren.fill_rect(rr, pal.row_alt);
        }
        const float base_y = ry + p.px(16.0F);
        p.draw(p.shape(type::row_numeric, std::to_string(i + 1)), rr.x + p.px(8.0F), base_y,
               pal.text_muted);

        const RectF orig_clip{rr.x + col_idx_w, ry, col_name_w - p.px(4.0F), row_h};
        ren.set_clip(orig_clip);
        p.draw(p.shape(type::row, items_[i].original_name), orig_clip.x + p.px(6.0F), base_y,
               pal.text_secondary);
        ren.set_clip(body_rect);

        p.draw(p.shape(type::row, "→"), rr.x + col_idx_w + col_name_w + p.px(4.0F), base_y,
               is_err ? pal.danger : pal.text_muted);

        const RectF prop_clip{rr.x + col_idx_w + col_name_w + col_arrow_w, ry,
                              col_name_w - p.px(4.0F), row_h};
        ren.set_clip(prop_clip);
        Color prop_col = pal.text_primary;
        if (is_err) {
            prop_col = pal.danger;
        } else if (pr.status == ops::PreviewRowStatus::Unchanged) {
            prop_col = pal.text_muted;
        }
        p.draw(p.shape(type::row, pr.proposed_name), prop_clip.x + p.px(6.0F), base_y, prop_col);
        ren.set_clip(body_rect);

        Color st_col = pal.text_muted;
        if (is_err) {
            st_col = pal.danger;
        } else if (pr.status == ops::PreviewRowStatus::Ready) {
            st_col = pal.accent;
        }
        p.draw(p.shape(type::caption, tr(status_key(pr.status))),
               rr.right() - col_status_w + p.px(6.0F), base_y, st_col);
    }
    ren.set_clip(prev_clip);

    // Footer bar: summary + Cancel / Apply buttons.
    const float fy = card.bottom() - footer_h + p.px(8.0F);
    std::string summary = std::to_string(plan_.changed_count) + " " +
                          std::string(tr("batch_rename.summary_ready"));
    if (plan_.collision_count > 0) {
        summary += " · " + std::to_string(plan_.collision_count) + " " +
                   std::string(tr("batch_rename.summary_collisions"));
    }
    if (plan_.invalid_count > 0) {
        summary += " · " + std::to_string(plan_.invalid_count) + " " +
                   std::string(tr("batch_rename.summary_invalid"));
    }
    p.draw(p.shape(type::caption, summary), card.x + pad, fy + p.px(17.0F),
           plan_.has_errors() ? pal.danger : pal.text_secondary);

    const float btn_h = p.px(28.0F);
    const float apply_w = p.px(110.0F);
    const float cancel_w = p.px(84.0F);
    const RectF apply_btn{card.right() - pad - apply_w, fy, apply_w, btn_h};
    const RectF cancel_btn{apply_btn.x - p.px(8.0F) - cancel_w, fy, cancel_w, btn_h};
    const bool can_apply = plan_.can_apply();
    draw_chip_button(p, pal, cancel_btn, tr("dialog.cancel"), false,
                     contains(cancel_btn, pointer.x, pointer.y), false, false);
    draw_chip_button(p, pal, apply_btn, tr("batch_rename.apply"), false,
                     contains(apply_btn, pointer.x, pointer.y), true, !can_apply);

    if (pointer.pressed) {
        if (contains(cancel_btn, pointer.x, pointer.y)) {
            close();
        } else if (can_apply && contains(apply_btn, pointer.x, pointer.y)) {
            ev.apply_request = ops::make_batch_rename_request(items_, plan_);
            close();
        }
    }

    return ev;
}

} // namespace navigator::ui

// src/ui/settings/appearance_page.cpp — Phase 9B Settings "Appearance" page.
#include "settings/appearance_page.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include <navigator/i18n.hpp>

namespace navigator::ui {
namespace {

bool draw_pill_btn(const Painter& p, const Palette& pal, const RectF& r, std::string_view label,
                   bool active, const PointerInput& pointer) {
    Renderer& ren = p.renderer();
    const bool hov = contains(r, pointer.x, pointer.y);
    const float rad = p.px(p.radii().sm);
    const float border_px = std::max(1.0F, std::round(p.px(1.0F)));
    Color bg = active ? pal.chip : (hov ? pal.control_hover : pal.surface);
    Color border = active ? pal.accent : pal.border;
    Color fg = active ? pal.chip_text : pal.text_primary;
    ren.fill_rounded_rect(r, rad, bg);
    ren.stroke_rounded_rect(r, rad, border_px, border);
    const ShapedText t = p.shape(type::caption, label);
    p.draw(t, std::round(r.x + ((r.w - t.width()) * 0.5F)),
           std::round(r.y + ((r.h + t.cap_height()) * 0.5F)), fg);
    return pointer.pressed && hov;
}

bool draw_stepper_row(const Painter& p, const Palette& pal, float x, float y, float w, float row_h,
                      std::string_view label, std::string_view value_text, bool can_dec,
                      bool can_inc, int& delta_out, const PointerInput& pointer) {
    delta_out = 0;
    const float base_y = y + (row_h * 0.65F);
    p.draw(p.shape(type::body, label), x, base_y, pal.text_primary);

    const float btn_w = p.px(28.0F);
    const float btn_h = p.px(26.0F);
    const float val_w = p.px(64.0F);
    const float by = y + ((row_h - btn_h) * 0.5F);
    const RectF inc_r{x + w - btn_w, by, btn_w, btn_h};
    const RectF val_r{inc_r.x - val_w - p.px(4.0F), by, val_w, btn_h};
    const RectF dec_r{val_r.x - btn_w - p.px(4.0F), by, btn_w, btn_h};

    if (can_dec && draw_pill_btn(p, pal, dec_r, "-", false, pointer)) {
        delta_out = -1;
        return true;
    }
    if (!can_dec) {
        (void)draw_pill_btn(p, pal, dec_r, "-", false, PointerInput{});
    }
    const ShapedText vt = p.shape(type::numeric, value_text);
    p.draw(vt, std::round(val_r.x + ((val_r.w - vt.width()) * 0.5F)),
           std::round(val_r.y + ((val_r.h + vt.cap_height()) * 0.5F)), pal.text_primary);
    if (can_inc && draw_pill_btn(p, pal, inc_r, "+", false, pointer)) {
        delta_out = +1;
        return true;
    }
    if (!can_inc) {
        (void)draw_pill_btn(p, pal, inc_r, "+", false, PointerInput{});
    }
    return false;
}

bool draw_toggle_row(const Painter& p, const Palette& pal, float x, float y, float w, float row_h,
                     std::string_view label, bool& value, const PointerInput& pointer) {
    const float base_y = y + (row_h * 0.65F);
    p.draw(p.shape(type::body, label), x, base_y, pal.text_primary);

    const float btn_w = p.px(74.0F);
    const float btn_h = p.px(26.0F);
    const RectF br{x + w - btn_w, y + ((row_h - btn_h) * 0.5F), btn_w, btn_h};
    const std::string_view state_label = value ? tr("settings.on") : tr("settings.off");
    if (draw_pill_btn(p, pal, br, state_label, value, pointer)) {
        value = !value;
        return true;
    }
    return false;
}

} // namespace

bool draw_appearance_page(const Painter& p, const Palette& pal, const RectF& body,
                          const PointerInput& pointer, appearance::AppearanceSettings& settings,
                          const std::vector<appearance::ThemeScheme>& custom_themes) {
    bool dirty = false;
    Renderer& ren = p.renderer();
    const float pad = p.px(18.0F);
    const float x = body.x + pad;
    const float w = body.w - (2.0F * pad);
    float y = body.y + p.px(14.0F);
    const float border_px = std::max(1.0F, std::round(p.px(1.0F)));

    // Section 1: Theme selector (Dark, Light, + custom JSON schemes).
    p.draw(p.shape(type::section, tr("settings.appearance.theme")), x, y + p.px(12.0F),
           pal.text_secondary);
    y += p.px(22.0F);

    struct ThemeChoice {
        std::string id;
        std::string name;
        Palette preview;
    };
    std::vector<ThemeChoice> choices;
    choices.push_back(ThemeChoice{std::string(appearance::kBuiltinDarkId),
                                  std::string(tr("settings.appearance.theme_dark")),
                                  palette(Theme::Dark)});
    choices.push_back(ThemeChoice{std::string(appearance::kBuiltinLightId),
                                  std::string(tr("settings.appearance.theme_light")),
                                  palette(Theme::Light)});
    for (const appearance::ThemeScheme& sc : custom_themes) {
        choices.push_back(ThemeChoice{sc.id, sc.name.empty() ? sc.id : sc.name,
                                      palette_from_scheme(sc)});
    }

    const float chip_h = p.px(34.0F);
    const float gap = p.px(8.0F);
    float cx = x;
    for (const ThemeChoice& ch : choices) {
        const ShapedText nt = p.shape(type::caption, ch.name);
        const float swatch_w = p.px(28.0F);
        const float cw = std::max(p.px(96.0F), nt.width() + swatch_w + p.px(22.0F));
        if (cx + cw > x + w && cx > x) {
            cx = x;
            y += chip_h + gap;
        }
        const RectF cr{cx, y, cw, chip_h};
        const bool active = settings.theme_id == ch.id;
        const bool hov = contains(cr, pointer.x, pointer.y);
        const float rad = p.px(p.radii().sm);
        ren.fill_rounded_rect(cr, rad, active ? pal.chip : (hov ? pal.control_hover : pal.surface));
        ren.stroke_rounded_rect(cr, rad, active ? p.px(1.5F) : border_px,
                                active ? pal.accent : pal.border);

        // Two small color swatch dots (surface + accent).
        const float dot = p.px(10.0F);
        const float dy = cr.y + ((cr.h - dot) * 0.5F);
        ren.fill_rounded_rect(RectF{cr.x + p.px(8.0F), dy, dot, dot}, dot * 0.5F,
                              ch.preview.background);
        ren.stroke_rounded_rect(RectF{cr.x + p.px(8.0F), dy, dot, dot}, dot * 0.5F, border_px,
                                pal.border);
        ren.fill_rounded_rect(RectF{cr.x + p.px(21.0F), dy, dot, dot}, dot * 0.5F,
                              ch.preview.accent);

        p.draw(nt, cr.x + swatch_w + p.px(10.0F),
               std::round(cr.y + ((cr.h + nt.cap_height()) * 0.5F)),
               active ? pal.chip_text : pal.text_primary);

        if (pointer.pressed && hov && !active) {
            settings.theme_id = ch.id;
            dirty = true;
        }
        cx += cw + gap;
    }

    y += chip_h + p.px(14.0F);
    ren.fill_rect(RectF{x, y, w, border_px}, pal.border);
    y += p.px(10.0F);

    // Section 2: Layout & Typography controls.
    const float row_h = p.px(36.0F);

    // Density: Compact / Comfortable.
    {
        p.draw(p.shape(type::body, tr("settings.appearance.density")), x, y + (row_h * 0.65F),
               pal.text_primary);
        const float bw = p.px(94.0F);
        const float bh = p.px(26.0F);
        const float by = y + ((row_h - bh) * 0.5F);
        const RectF comf_r{x + w - bw, by, bw, bh};
        const RectF comp_r{comf_r.x - bw - p.px(6.0F), by, bw, bh};
        if (draw_pill_btn(p, pal, comp_r, tr("settings.appearance.compact"),
                          settings.density == appearance::UiDensity::Compact, pointer)) {
            if (settings.density != appearance::UiDensity::Compact) {
                settings.density = appearance::UiDensity::Compact;
                dirty = true;
            }
        }
        if (draw_pill_btn(p, pal, comf_r, tr("settings.appearance.comfortable"),
                          settings.density == appearance::UiDensity::Comfortable, pointer)) {
            if (settings.density != appearance::UiDensity::Comfortable) {
                settings.density = appearance::UiDensity::Comfortable;
                dirty = true;
            }
        }
        y += row_h;
    }

    // Font scale.
    {
        int delta = 0;
        const std::string val = std::to_string(settings.font_scale_percent) + "%";
        if (draw_stepper_row(p, pal, x, y, w, row_h, tr("settings.appearance.font_size"), val,
                             settings.font_scale_percent > appearance::kMinFontScalePercent,
                             settings.font_scale_percent < appearance::kMaxFontScalePercent, delta,
                             pointer)) {
            settings.font_scale_percent = appearance::clamp_font_scale(
                settings.font_scale_percent + (delta * appearance::kFontScaleStepPercent));
            dirty = true;
        }
        y += row_h;
    }

    // Spacing scale.
    {
        int delta = 0;
        const std::string val = std::to_string(settings.spacing_scale_percent) + "%";
        if (draw_stepper_row(p, pal, x, y, w, row_h, tr("settings.appearance.spacing"), val,
                             settings.spacing_scale_percent > appearance::kMinSpacingScalePercent,
                             settings.spacing_scale_percent < appearance::kMaxSpacingScalePercent,
                             delta, pointer)) {
            settings.spacing_scale_percent = appearance::clamp_spacing_scale(
                settings.spacing_scale_percent + (delta * appearance::kSpacingScaleStepPercent));
            dirty = true;
        }
        y += row_h;
    }

    // Corner radius.
    {
        int delta = 0;
        const std::string val = std::to_string(settings.corner_radius_dp) + " dp";
        if (draw_stepper_row(p, pal, x, y, w, row_h, tr("settings.appearance.corner_radius"), val,
                             settings.corner_radius_dp > appearance::kMinCornerRadiusDp,
                             settings.corner_radius_dp < appearance::kMaxCornerRadiusDp, delta,
                             pointer)) {
            settings.corner_radius_dp =
                appearance::clamp_corner_radius(settings.corner_radius_dp + delta);
            dirty = true;
        }
        y += row_h;
    }

    y += p.px(6.0F);
    ren.fill_rect(RectF{x, y, w, border_px}, pal.border);
    y += p.px(6.0F);

    // Animations toggle.
    if (draw_toggle_row(p, pal, x, y, w, row_h, tr("settings.appearance.animations"),
                        settings.animations, pointer)) {
        dirty = true;
    }
    y += row_h;

    // Alternating row colors (zebra) toggle.
    if (draw_toggle_row(p, pal, x, y, w, row_h, tr("settings.appearance.alternating_rows"),
                        settings.alternating_rows, pointer)) {
        dirty = true;
    }
    y += row_h + p.px(10.0F);

    // Reset appearance defaults button.
    const float rst_w = p.px(148.0F);
    const float rst_h = p.px(28.0F);
    const RectF rst_r{x, y, rst_w, rst_h};
    if (draw_pill_btn(p, pal, rst_r, tr("settings.appearance.reset"), false, pointer)) {
        const appearance::AppearanceSettings def;
        if (settings != def) {
            settings = def;
            dirty = true;
        }
    }

    return dirty;
}

} // namespace navigator::ui

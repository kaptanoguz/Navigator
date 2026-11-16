// src/ui/shell/scene.cpp — Phase 0 scene layout and drawing. All sizes come from theme tokens.
#include "shell/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <navigator/i18n.hpp>

#include "input/keybindings.hpp"

namespace navigator::ui {
namespace {

// Layout tokens for this scene (dp). Derived from the spacing/radius scales.
namespace layout {
constexpr float content_center = 0.45F;               // optical centre, fraction of window height
constexpr float tagline_gap = space::xxl;             // wordmark baseline -> tagline cap top
constexpr float accent_gap = space::xxl;              // accent mark -> wordmark cap top
constexpr float accent_width = space::xl + space::xs; // 28
constexpr float accent_height = 3.0F;
constexpr float hints_bottom = space::xxxl - space::sm; // bottom margin to keycap row centre
constexpr float hint_gap = space::xxl;                  // between hint groups
constexpr float chip_height = space::xl;                // 24
constexpr float chip_pad_x = space::sm;                 // 8
constexpr float chip_gap = space::xs;
constexpr float chip_label_gap = space::sm + space::xxs; // keycaps -> label
constexpr float overlay_margin = space::lg;
constexpr float overlay_pad_x = space::md + space::xxs;
constexpr float overlay_pad_y = space::md;
constexpr float overlay_row = space::lg + space::xs; // 20
constexpr float overlay_col_gap = space::xl;
constexpr float shadow_blur = space::lg;
constexpr float shadow_dy = space::xs;
} // namespace layout

void draw_background(const Painter& p, const SceneModel& m) {
    Renderer& r = p.renderer();
    r.clear(m.palette.background);
    const RectF full{0.0F, 0.0F, m.width, m.height};
    if (m.background_from != nullptr && m.background_t < 1.0F) {
        r.draw_texture(m.background_from, full, 1.0F);
        r.draw_texture(m.background_to, full, m.background_t);
    } else {
        r.draw_texture(m.background_to, full, 1.0F);
    }
}

void draw_wordmark(const Painter& p, const SceneModel& m) {
    const ShapedText title = p.shape(type::display, tr("app.title"));
    const ShapedText tagline = p.shape(type::tagline, tr("app.tagline"));

    const float accent_h = p.px(layout::accent_height);
    const float group_h = accent_h + p.px(layout::accent_gap) + title.cap_height() +
                          p.px(layout::tagline_gap) + tagline.cap_height();
    const float top = std::round((m.height * layout::content_center) - (0.5F * group_h));

    // Accent mark: a short pill, the only saturated element on screen.
    const float accent_w = p.px(layout::accent_width);
    const RectF accent =
        snap(RectF{(0.5F * m.width) - (0.5F * accent_w), top, accent_w, std::max(accent_h, 2.0F)});
    p.renderer().fill_rounded_rect(accent, radius::pill, m.palette.accent);

    const float title_baseline = accent.bottom() + p.px(layout::accent_gap) + title.cap_height();
    p.draw(title, (0.5F * m.width) - (0.5F * title.width()), title_baseline,
           m.palette.text_primary);

    const float tagline_baseline =
        title_baseline + p.px(layout::tagline_gap) + tagline.cap_height();
    p.draw(tagline, (0.5F * m.width) - (0.5F * tagline.width()), tagline_baseline,
           m.palette.text_secondary);
}

struct Hint {
    std::vector<ShapedText> keys;
    ShapedText label;
    float width = 0.0F;
};

void draw_keycap(const Painter& p, const SceneModel& m, const ShapedText& key, float x,
                 float center_y, float width) {
    Renderer& r = p.renderer();
    const float h = p.px(layout::chip_height);
    const RectF cap = snap(RectF{x, center_y - (0.5F * h), width, h});
    const float rad = p.px(radius::sm);
    const float edge = std::max(1.0F, std::round(p.px(1.0F)));
    // Depth: a slightly darker copy peeking out under the cap, then the cap and its hairline.
    r.fill_rounded_rect(RectF{cap.x, cap.y + edge, cap.w, cap.h}, rad, m.palette.keycap_edge);
    r.fill_rounded_rect(cap, rad, m.palette.surface_raised);
    r.stroke_rounded_rect(cap, rad, edge, m.palette.keycap_border);
    const float baseline = cap.y + (0.5F * cap.h) + (0.5F * key.cap_height());
    p.draw(key, cap.x + (0.5F * cap.w) - (0.5F * key.width()), baseline, m.palette.keycap_text);
}

void draw_hints(const Painter& p, const SceneModel& m) {
    struct Source {
        std::string_view command;
        std::string_view label_key;
    };
    constexpr std::array sources{
        Source{"view.toggle_theme", "hint.theme"},
        Source{"view.toggle_fps_overlay", "hint.fps"},
        Source{"app.quit", "hint.quit"},
    };

    const float chip_min = p.px(layout::chip_height);
    const float pad = p.px(layout::chip_pad_x);
    std::vector<Hint> hints;
    float total = 0.0F;
    for (const Source& s : sources) {
        const KeyBinding* binding = binding_for(s.command);
        if (binding == nullptr) {
            continue;
        }
        Hint h;
        for (std::string_view label : chord_labels(*binding)) {
            h.keys.push_back(p.shape(type::keycap, label));
            h.width += std::max(chip_min, h.keys.back().width() + (2.0F * pad));
        }
        h.width += p.px(layout::chip_gap) * static_cast<float>(h.keys.size() - 1);
        h.label = p.shape(type::body, tr(s.label_key));
        h.width += p.px(layout::chip_label_gap) + h.label.width();
        total += h.width;
        hints.push_back(std::move(h));
    }
    if (hints.empty()) {
        return;
    }
    total += p.px(layout::hint_gap) * static_cast<float>(hints.size() - 1);

    const float center_y = std::round(m.height - p.px(layout::hints_bottom));
    float x = std::round((0.5F * m.width) - (0.5F * total));
    for (const Hint& h : hints) {
        for (const ShapedText& key : h.keys) {
            const float w = std::round(std::max(chip_min, key.width() + (2.0F * pad)));
            draw_keycap(p, m, key, x, center_y, w);
            x += w + p.px(layout::chip_gap);
        }
        x += p.px(layout::chip_label_gap) - p.px(layout::chip_gap);
        const float baseline = center_y + (0.5F * h.label.cap_height());
        p.draw(h.label, x, baseline, m.palette.text_secondary);
        x += h.label.width() + p.px(layout::hint_gap);
    }
}

void draw_fps_overlay(const Painter& p, const SceneModel& m) {
    std::array<char, 32> fps_buf{};
    std::array<char, 32> ms_buf{};
    std::snprintf(fps_buf.data(), fps_buf.size(), "%.0f", m.fps);
    std::snprintf(ms_buf.data(), ms_buf.size(), "%.1f %s", m.frame_ms,
                  std::string(tr("fps.unit_ms")).c_str());

    struct Row {
        ShapedText label;
        ShapedText value;
    };
    std::array rows{
        Row{p.shape(type::caption, tr("fps.label")), p.shape(type::numeric, fps_buf.data())},
        Row{p.shape(type::caption, tr("fps.frame_time")), p.shape(type::numeric, ms_buf.data())},
        Row{p.shape(type::caption, tr("fps.driver")), p.shape(type::numeric, m.render_driver)},
    };
    // Value column reserves room for the widest plausible numbers so the panel never jitters.
    const float reserve = p.shape(type::numeric, "000.0 ms").width();
    float label_w = 0.0F;
    float value_w = reserve;
    for (const Row& r : rows) {
        label_w = std::max(label_w, r.label.width());
        value_w = std::max(value_w, r.value.width());
    }
    const float pad_x = p.px(layout::overlay_pad_x);
    const float pad_y = p.px(layout::overlay_pad_y);
    const float row_h = p.px(layout::overlay_row);
    const float w = std::ceil(pad_x + label_w + p.px(layout::overlay_col_gap) + value_w + pad_x);
    const float h = std::ceil((2.0F * pad_y) + (row_h * static_cast<float>(rows.size())));
    const float margin = p.px(layout::overlay_margin);
    const RectF panel = snap(RectF{m.width - margin - w, margin, w, h});
    const float rad = p.px(radius::lg);

    Renderer& r = p.renderer();
    r.drop_shadow(panel, rad, p.px(layout::shadow_blur), p.px(layout::shadow_dy), m.palette.shadow);
    r.fill_rounded_rect(panel, rad, m.palette.surface_raised.with_alpha(0.94F));
    r.stroke_rounded_rect(panel, rad, std::max(1.0F, std::round(p.px(1.0F))), m.palette.border);

    float row_top = panel.y + pad_y;
    for (const Row& row : rows) {
        const float cap = std::max(row.label.cap_height(), row.value.cap_height());
        const float baseline = row_top + (0.5F * row_h) + (0.5F * cap);
        p.draw(row.label, panel.x + pad_x, baseline, m.palette.text_muted);
        p.draw(row.value, panel.right() - pad_x - row.value.width(), baseline,
               m.palette.text_primary);
        row_top += row_h;
    }
}

} // namespace

ShapedText Painter::shape(const TextStyle& style, std::string_view utf8) const {
    return ShapedText{fonts_->shape(style, scale_, utf8, language_)};
}

void Painter::draw(const ShapedText& text, float x, float baseline, const Color& color) const {
    if (text.layout) {
        text_->draw(*text.layout, x, baseline, color);
    }
}

void draw_scene(const Painter& painter, const SceneModel& model) {
    draw_background(painter, model);
    draw_wordmark(painter, model);
    draw_hints(painter, model);
    if (model.show_fps) {
        draw_fps_overlay(painter, model);
    }
}

} // namespace navigator::ui

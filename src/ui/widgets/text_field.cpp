// src/ui/widgets/text_field.cpp — see text_field.hpp.
#include "widgets/text_field.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace navigator::ui {

float draw_text_field(const Painter& p, const Palette& pal, const TextEdit& edit, const RectF& box,
                      std::string_view placeholder, float& scroll, const TextFieldStyle& style) {
    Renderer& r = p.renderer();
    const std::optional<RectF> prev = r.clip();
    const auto clip_to = [&](const RectF& c) { r.set_clip(prev ? intersect(*prev, c) : c); };
    const TextStyle& ts = style.text != nullptr ? *style.text : type::popup_field;
    const float mid = box.center_y();
    const std::string& text = edit.text();
    const float caret_h = std::round(p.px(style.caret_height_dp));
    const float cw = std::max(1.0F, std::round(p.px(1.5F)));
    if (text.empty()) {
        scroll = 0.0F;
        if (!placeholder.empty()) {
            const ShapedText ph = p.shape(ts, placeholder);
            clip_to(box);
            p.draw(ph, box.x, std::round(mid + (0.5F * ph.cap_height())), pal.text_muted);
            r.set_clip(prev);
        }
        if (style.show_caret) {
            r.fill_rect(snap(RectF{box.x, mid - (0.5F * caret_h), cw, caret_h}), pal.accent);
        }
        return box.x;
    }
    const ShapedText full = p.shape(ts, text);
    const float caret_off = p.shape(ts, std::string_view(text).substr(0, edit.caret())).width();
    // Keep the caret inside the box with a small margin.
    const float margin = std::min(p.px(12.0F), 0.25F * box.w);
    if (caret_off - scroll > box.w - margin) {
        scroll = caret_off - box.w + margin;
    }
    if (caret_off - scroll < margin) {
        scroll = caret_off - margin;
    }
    scroll = std::clamp(scroll, 0.0F, std::max(full.width() - box.w + cw + p.px(2.0F), 0.0F));
    const float x0 = std::round(box.x - scroll);
    clip_to(RectF{box.x - p.px(1.0F), box.y, box.w + p.px(2.0F), box.h});
    if (edit.has_selection()) {
        const auto [b, e] = edit.selection();
        const float sx = p.shape(ts, std::string_view(text).substr(0, b)).width();
        const float ex = p.shape(ts, std::string_view(text).substr(0, e)).width();
        const float sh = std::round(p.px(style.caret_height_dp + 2.0F));
        r.fill_rounded_rect(snap(RectF{x0 + sx, mid - (0.5F * sh), ex - sx, sh}), p.px(2.0F),
                            pal.selection);
    }
    p.draw(full, x0, std::round(mid + (0.5F * full.cap_height())), pal.text_primary);
    const float cx = x0 + caret_off;
    if (style.show_caret) {
        r.fill_rect(snap(RectF{cx, mid - (0.5F * caret_h), cw, caret_h}), pal.accent);
    }
    r.set_clip(prev);
    return cx;
}

} // namespace navigator::ui

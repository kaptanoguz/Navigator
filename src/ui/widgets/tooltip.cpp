// src/ui/widgets/tooltip.cpp — shared tooltip bubble. See tooltip.hpp.
#include "widgets/tooltip.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui {

void draw_tooltip_bubble(const Painter& p, const Palette& pal, std::string_view text,
                         const RectF& anchor, const RectF& bounds) {
    if (text.empty()) {
        return;
    }
    Renderer& r = p.renderer();
    const ShapedText t = p.shape(type::caption, text);
    const float h = std::round(p.px(26.0F));
    const float w = std::round(t.width() + p.px(20.0F));
    float x = std::round(anchor.center_x() - (0.5F * w));
    x = std::clamp(x, bounds.x + p.px(4.0F),
                   std::max(bounds.x + p.px(4.0F), bounds.right() - w - p.px(4.0F)));
    const RectF tip = snap(RectF{x, std::round(anchor.bottom() + p.px(8.0F)), w, h});
    const float rad = std::round(p.px(radius::md - 2.0F));
    r.drop_shadow(tip, rad, p.px(12.0F), p.px(3.0F), pal.shadow);
    r.fill_rounded_rect(tip, rad, pal.surface_raised);
    r.stroke_rounded_rect(tip, rad, std::max(1.0F, std::round(p.px(1.0F))), pal.border);
    p.draw(t, tip.x + std::round(p.px(10.0F)), std::round(tip.center_y() + (0.5F * t.cap_height())),
           pal.text_primary);
}

} // namespace navigator::ui

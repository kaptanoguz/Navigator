// src/ui/widgets/icons.cpp — see icons.hpp. Geometry is designed on a 16-unit grid.
#include "widgets/icons.hpp"

#include <algorithm>
#include <cmath>

#include "browser/format.hpp"

namespace navigator::ui {
namespace {

Color faded(const Color& c, float dim) { return c.with_alpha(c.a * dim); }

Color tint_color(const Palette& pal, FileTint tint) {
    switch (tint) {
    case FileTint::Image: return pal.tint_image;
    case FileTint::Audio: return pal.tint_audio;
    case FileTint::Video: return pal.tint_video;
    case FileTint::Archive: return pal.tint_archive;
    case FileTint::Code: return pal.tint_code;
    case FileTint::Document: return pal.tint_document;
    case FileTint::Pdf: return pal.tint_pdf;
    case FileTint::Sheet: return pal.tint_sheet;
    case FileTint::Slides: return pal.tint_slides;
    case FileTint::Program: return pal.tint_program;
    case FileTint::None: break;
    }
    return pal.icon_page_border;
}

struct Grid {
    float x;
    float y;
    float u; // px per unit

    [[nodiscard]] RectF rect(float gx, float gy, float gw, float gh) const {
        return RectF{x + (gx * u), y + (gy * u), gw * u, gh * u};
    }
    [[nodiscard]] float px(float g) const { return g * u; }
    [[nodiscard]] float at_x(float g) const { return x + (g * u); }
    [[nodiscard]] float at_y(float g) const { return y + (g * u); }
};

void draw_folder(Renderer& r, const Palette& pal, const Grid& g, float dim) {
    const Color back = faded(pal.icon_folder_back, dim);
    const Color front = faded(pal.icon_folder_front, dim);
    r.fill_rounded_rect(g.rect(1.0F, 2.0F, 6.5F, 3.5F), g.px(1.2F), back);
    r.fill_rounded_rect(g.rect(1.0F, 3.5F, 14.0F, 10.5F), g.px(1.8F), back);
    r.fill_rounded_rect(g.rect(1.0F, 5.5F, 14.0F, 8.5F), g.px(1.8F), front);
}

void draw_file(Renderer& r, const Palette& pal, const Grid& g, FileTint tint, float dim,
               float scale) {
    const float hair = std::max(1.0F, std::round(scale));
    const RectF page = snap(g.rect(3.0F, 1.0F, 10.0F, 14.0F));
    const float rad = g.px(1.8F);
    r.fill_rounded_rect(page, rad, faded(pal.icon_page, dim));
    const Color line = faded(pal.icon_page_border, 0.9F * dim);
    const float lx0 = page.x + g.px(2.2F);
    const float lx1 = page.right() - g.px(2.2F);
    const float t = hair;
    const auto text_line = [&](float gy, float frac) {
        const float yy = std::round(g.at_y(gy)) + (0.5F * t);
        r.stroke_line(lx0, yy, lx0 + ((lx1 - lx0) * frac), yy, t, line);
    };
    if (tint == FileTint::None) {
        text_line(4.5F, 1.0F);
        text_line(7.0F, 1.0F);
        text_line(9.5F, 0.6F);
    } else {
        text_line(4.5F, 1.0F);
        text_line(6.8F, 0.65F);
        // The lower part of the page carries the type colour.
        const RectF band = snap(RectF{page.x, std::round(g.at_y(9.5F)), page.w,
                                      page.bottom() - std::round(g.at_y(9.5F))});
        r.fill_rounded_rect(band, rad, faded(tint_color(pal, tint), dim));
        r.fill_rect(RectF{band.x, band.y, band.w, std::min(rad, band.h * 0.5F)},
                    faded(tint_color(pal, tint), dim));
    }
    r.stroke_rounded_rect(page, rad, hair, faded(pal.icon_page_border, dim));
}

void draw_link_badge(Renderer& r, const Palette& pal, const Grid& g, bool broken, float dim,
                     float scale) {
    const float cx = g.at_x(4.5F);
    const float cy = g.at_y(11.5F);
    const float rad = g.px(4.5F);
    const float hair = std::max(1.0F, std::round(scale));
    r.fill_circle(cx, cy, rad + hair, faded(pal.surface, dim)); // separation ring
    r.fill_circle(cx, cy, rad, faded(pal.surface_raised, dim));
    const Color ink = faded(broken ? pal.danger : pal.text_secondary, dim);
    r.stroke_rounded_rect(RectF{cx - rad, cy - rad, 2.0F * rad, 2.0F * rad}, rad, hair,
                          faded(broken ? pal.danger : pal.icon_page_border, dim));
    // Arrow pointing up-right.
    const float a = g.px(1.7F);
    const float t = std::max(1.0F, scale * 1.2F);
    r.stroke_line(cx - a, cy + a, cx + a, cy - a, t, ink);
    r.stroke_line(cx + a, cy - a, cx - (0.1F * a), cy - a, t, ink);
    r.stroke_line(cx + a, cy - a, cx + a, cy + (0.1F * a), t, ink);
}

} // namespace

void draw_entry_glyph(Renderer& r, const Palette& pal, const fs::DirEntry& entry, float x, float y,
                      float size, float dim, float scale) {
    const Grid g{x, y, size / 16.0F};
    const bool link = entry.kind == fs::EntryKind::Symlink;
    if (fs::is_directory_like(entry)) {
        draw_folder(r, pal, g, entry.readable || link ? dim : dim * 0.5F);
    } else {
        draw_file(r, pal, g, file_tint(entry), dim, scale);
    }
    if (link) {
        draw_link_badge(r, pal, g, entry.dangling, dim, scale);
    }
}

void draw_chevron(Renderer& r, float cx, float cy, float size, Direction dir, float thickness,
                  const Color& color) {
    const float h = 0.5F * size;
    switch (dir) {
    case Direction::Left:
        r.stroke_line(cx + (0.5F * h), cy - h, cx - (0.5F * h), cy, thickness, color);
        r.stroke_line(cx - (0.5F * h), cy, cx + (0.5F * h), cy + h, thickness, color);
        break;
    case Direction::Right:
        r.stroke_line(cx - (0.5F * h), cy - h, cx + (0.5F * h), cy, thickness, color);
        r.stroke_line(cx + (0.5F * h), cy, cx - (0.5F * h), cy + h, thickness, color);
        break;
    case Direction::Up:
        r.stroke_line(cx - h, cy + (0.5F * h), cx, cy - (0.5F * h), thickness, color);
        r.stroke_line(cx, cy - (0.5F * h), cx + h, cy + (0.5F * h), thickness, color);
        break;
    case Direction::Down:
        r.stroke_line(cx - h, cy - (0.5F * h), cx, cy + (0.5F * h), thickness, color);
        r.stroke_line(cx, cy + (0.5F * h), cx + h, cy - (0.5F * h), thickness, color);
        break;
    }
}

void draw_theme_glyph(Renderer& r, float cx, float cy, float radius, Theme current,
                      const Color& color, const Color& background) {
    if (current == Theme::Dark) {
        // Sun: disc + eight short rays.
        r.fill_circle(cx, cy, radius * 0.48F, color);
        const float t = std::max(1.0F, radius * 0.16F);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.785398F;
            const float c = std::cos(a);
            const float s = std::sin(a);
            r.stroke_line(cx + (c * radius * 0.75F), cy + (s * radius * 0.75F),
                          cx + (c * radius * 1.0F), cy + (s * radius * 1.0F), t, color);
        }
    } else {
        // Moon: disc with a background-coloured disc biting into it.
        r.fill_circle(cx, cy, radius * 0.85F, color);
        r.fill_circle(cx + (radius * 0.42F), cy - (radius * 0.32F), radius * 0.68F, background);
    }
}

void draw_alert_glyph(Renderer& r, float cx, float cy, float radius, const Color& color,
                      const Color& mark, float scale) {
    r.fill_circle(cx, cy, radius, color);
    const float t = std::max(1.5F, 1.6F * scale);
    r.stroke_line(cx, cy - (radius * 0.48F), cx, cy + (radius * 0.08F), t, mark);
    r.fill_circle(cx, cy + (radius * 0.45F), 0.5F * t + 0.25F, mark);
}

void draw_cross(Renderer& r, float cx, float cy, float size, float thickness, const Color& color) {
    r.stroke_line(cx - size, cy - size, cx + size, cy + size, thickness, color);
    r.stroke_line(cx - size, cy + size, cx + size, cy - size, thickness, color);
}

void draw_folder_glyph(Renderer& r, const Palette& pal, float x, float y, float size, float dim) {
    draw_folder(r, pal, Grid{x, y, size / 16.0F}, dim);
}

void draw_plus(Renderer& r, float cx, float cy, float size, float thickness, const Color& color) {
    r.stroke_line(cx - size, cy, cx + size, cy, thickness, color);
    r.stroke_line(cx, cy - size, cx, cy + size, thickness, color);
}

void draw_search_glyph(Renderer& r, float cx, float cy, float radius, float thickness,
                       const Color& color, const Color& background) {
    const float lx = cx - (0.18F * radius);
    const float ly = cy - (0.18F * radius);
    r.fill_circle(lx, ly, radius, color);
    r.fill_circle(lx, ly, std::max(radius - thickness, 0.5F), background);
    const float d = radius * 0.7071F;
    r.stroke_line(lx + d, ly + d, lx + (1.75F * d), ly + (1.75F * d), thickness, color);
}

void draw_layout_glyph(Renderer& r, float x, float y, float size, float thickness,
                       const Color& color) {
    const RectF box =
        snap(RectF{x + (0.06F * size), y + (0.14F * size), 0.88F * size, 0.72F * size});
    r.stroke_rounded_rect(box, 0.14F * size, thickness, color);
    const float mx = std::round(box.x + (0.45F * box.w));
    r.fill_rect(RectF{mx, box.y, thickness, box.h}, color);
    const float my = std::round(box.y + (0.5F * box.h));
    r.fill_rect(RectF{mx, my, box.right() - mx, thickness}, color);
}

} // namespace navigator::ui

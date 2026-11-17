// src/ui/widgets/icons.cpp — see icons.hpp. Geometry is designed on a 16-unit grid.
#include "widgets/icons.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

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

void draw_line_icon(Renderer& r, LineIcon icon, float x, float y, float size, float t,
                    const Color& color, const Color& background, bool filled) {
    const Grid g{x, y, size / 16.0F};
    const auto line = [&](float x0, float y0, float x1, float y1) {
        r.stroke_line(g.at_x(x0), g.at_y(y0), g.at_x(x1), g.at_y(y1), t, color);
    };
    const auto box = [&](float gx, float gy, float gw, float gh, float rad) {
        r.stroke_rounded_rect(g.rect(gx, gy, gw, gh), g.px(rad), t, color);
    };
    const auto dot = [&](float gx, float gy, float rad) {
        r.fill_circle(g.at_x(gx), g.at_y(gy), g.px(rad), color);
    };
    const auto poly = [&](std::initializer_list<std::pair<float, float>> pts) {
        std::vector<SDL_FPoint> v;
        v.reserve(pts.size());
        for (const auto& [px, py] : pts) {
            v.push_back(SDL_FPoint{g.at_x(px), g.at_y(py)});
        }
        r.fill_polygon(v, color);
    };
    switch (icon) {
    case LineIcon::Folder:
        r.fill_rounded_rect(g.rect(1.6F, 2.6F, 5.6F, 3.0F), g.px(1.0F), color);
        box(1.6F, 4.2F, 12.8F, 9.4F, 1.7F);
        break;
    case LineIcon::Clock:
        box(1.6F, 1.6F, 12.8F, 12.8F, 6.4F);
        line(8.0F, 8.0F, 8.0F, 4.6F);
        line(8.0F, 8.0F, 10.6F, 9.6F);
        break;
    case LineIcon::Home:
        line(1.9F, 7.7F, 8.0F, 2.3F);
        line(8.0F, 2.3F, 14.1F, 7.7F);
        line(3.6F, 6.3F, 3.6F, 13.7F);
        line(3.6F, 13.7F, 12.4F, 13.7F);
        line(12.4F, 13.7F, 12.4F, 6.3F);
        line(6.7F, 13.7F, 6.7F, 9.8F);
        line(6.7F, 9.8F, 9.3F, 9.8F);
        line(9.3F, 9.8F, 9.3F, 13.7F);
        break;
    case LineIcon::Desktop:
        box(1.5F, 2.4F, 13.0F, 9.0F, 1.6F);
        line(8.0F, 11.6F, 8.0F, 13.7F);
        line(5.2F, 13.7F, 10.8F, 13.7F);
        break;
    case LineIcon::Documents:
        box(3.0F, 1.5F, 10.0F, 13.0F, 1.6F);
        line(5.6F, 5.4F, 10.4F, 5.4F);
        line(5.6F, 8.0F, 10.4F, 8.0F);
        line(5.6F, 10.6F, 8.6F, 10.6F);
        break;
    case LineIcon::Downloads:
        line(8.0F, 1.9F, 8.0F, 9.8F);
        line(4.9F, 6.8F, 8.0F, 9.8F);
        line(8.0F, 9.8F, 11.1F, 6.8F);
        line(2.0F, 10.6F, 2.0F, 13.6F);
        line(2.0F, 13.6F, 14.0F, 13.6F);
        line(14.0F, 13.6F, 14.0F, 10.6F);
        break;
    case LineIcon::Music:
        dot(4.6F, 12.2F, 2.1F);
        dot(11.6F, 10.6F, 2.1F);
        line(6.5F, 12.0F, 6.5F, 3.4F);
        line(13.5F, 10.4F, 13.5F, 1.9F);
        line(6.5F, 3.4F, 13.5F, 1.9F);
        line(6.5F, 5.9F, 13.5F, 4.4F);
        break;
    case LineIcon::Pictures:
        box(1.5F, 2.5F, 13.0F, 11.0F, 1.6F);
        line(2.6F, 11.6F, 6.4F, 7.6F);
        line(6.4F, 7.6F, 9.6F, 10.8F);
        line(9.6F, 10.8F, 11.0F, 9.4F);
        line(11.0F, 9.4F, 13.4F, 11.6F);
        dot(10.6F, 5.8F, 1.3F);
        break;
    case LineIcon::Videos:
        box(1.5F, 3.0F, 13.0F, 10.0F, 2.0F);
        poly({{6.6F, 5.7F}, {10.9F, 8.0F}, {6.6F, 10.3F}});
        break;
    case LineIcon::Drive:
        box(1.5F, 4.2F, 13.0F, 7.6F, 2.0F);
        line(4.2F, 9.2F, 8.2F, 9.2F);
        dot(11.4F, 9.2F, 1.0F);
        break;
    case LineIcon::Removable:
        box(4.4F, 6.0F, 7.2F, 8.6F, 1.7F);
        box(6.0F, 1.6F, 4.0F, 4.6F, 0.7F);
        dot(7.2F, 3.6F, 0.6F);
        dot(8.8F, 3.6F, 0.6F);
        break;
    case LineIcon::Network:
        box(1.6F, 1.6F, 12.8F, 12.8F, 6.4F);
        box(5.0F, 1.6F, 6.0F, 12.8F, 3.0F);
        line(1.9F, 8.0F, 14.1F, 8.0F);
        break;
    case LineIcon::Star: {
        constexpr float cx = 8.0F;
        constexpr float cy = 8.4F;
        std::vector<SDL_FPoint> pts;
        for (int i = 0; i < 10; ++i) {
            const float a = (-1.5707963F) + (static_cast<float>(i) * 0.6283185F);
            const float rad = (i % 2 == 0) ? 7.0F : 2.9F;
            pts.push_back(
                SDL_FPoint{g.at_x(cx + (std::cos(a) * rad)), g.at_y(cy + (std::sin(a) * rad))});
        }
        if (filled) {
            r.fill_polygon(pts, color);
        } else {
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const SDL_FPoint& a = pts[i];
                const SDL_FPoint& b = pts[(i + 1) % pts.size()];
                r.stroke_line(a.x, a.y, b.x, b.y, t, color);
            }
        }
        break;
    }
    case LineIcon::Lock:
        box(4.6F, 1.8F, 6.8F, 9.0F, 3.4F);
        r.fill_rounded_rect(g.rect(3.0F, 7.0F, 10.0F, 7.6F), g.px(1.6F), background);
        box(3.0F, 7.0F, 10.0F, 7.6F, 1.6F);
        dot(8.0F, 10.8F, 0.9F);
        break;
    case LineIcon::Sidebar:
        box(1.5F, 2.5F, 13.0F, 11.0F, 2.0F);
        r.fill_rect(snap(RectF{g.at_x(5.8F), g.at_y(2.9F), std::max(t, 1.0F), g.px(10.2F)}), color);
        line(3.4F, 5.4F, 4.2F, 5.4F);
        line(3.4F, 7.6F, 4.2F, 7.6F);
        break;
    case LineIcon::Unplugged:
        line(3.0F, 13.0F, 13.0F, 3.0F);
        box(2.4F, 5.4F, 8.0F, 6.0F, 1.6F);
        break;
    }
}

} // namespace navigator::ui

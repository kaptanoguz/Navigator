// src/ui/render/renderer.cpp — see renderer.hpp.
#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace navigator::ui {
namespace {

SDL_Vertex vertex(SDL_FPoint p, SDL_FColor c) noexcept {
    return SDL_Vertex{p, c, SDL_FPoint{0.0F, 0.0F}};
}

// Appends quads joining two closed rings of equal length.
void band(const std::vector<SDL_FPoint>& outer, SDL_FColor outer_color,
          const std::vector<SDL_FPoint>& inner, SDL_FColor inner_color,
          std::vector<SDL_Vertex>& vertices, std::vector<int>& indices) {
    const auto n = static_cast<int>(outer.size());
    const auto base = static_cast<int>(vertices.size());
    for (int i = 0; i < n; ++i) {
        vertices.push_back(vertex(outer[static_cast<std::size_t>(i)], outer_color));
        vertices.push_back(vertex(inner[static_cast<std::size_t>(i)], inner_color));
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const int o0 = base + (2 * i);
        const int i0 = o0 + 1;
        const int o1 = base + (2 * j);
        const int i1 = o1 + 1;
        indices.insert(indices.end(), {o0, o1, i1, o0, i1, i0});
    }
}

} // namespace

RectF snap(const RectF& r) noexcept {
    const float x0 = std::round(r.x);
    const float y0 = std::round(r.y);
    const float x1 = std::round(r.x + r.w);
    const float y1 = std::round(r.y + r.h);
    return RectF{x0, y0, x1 - x0, y1 - y0};
}

SDL_FColor to_sdl(const Color& c) noexcept { return SDL_FColor{c.r, c.g, c.b, c.a}; }

void Renderer::clear(const Color& color) {
    SDL_SetRenderDrawColorFloat(renderer_, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer_);
}

void Renderer::fill_rect(const RectF& rect, const Color& color) {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColorFloat(renderer_, color.r, color.g, color.b, color.a);
    const SDL_FRect r{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderFillRect(renderer_, &r);
}

void Renderer::fill_rect_vgradient(const RectF& rect, const Color& top, const Color& bottom) {
    const SDL_FColor t = to_sdl(top);
    const SDL_FColor b = to_sdl(bottom);
    vertices_.clear();
    indices_.clear();
    vertices_.push_back(vertex({rect.x, rect.y}, t));
    vertices_.push_back(vertex({rect.right(), rect.y}, t));
    vertices_.push_back(vertex({rect.right(), rect.bottom()}, b));
    vertices_.push_back(vertex({rect.x, rect.bottom()}, b));
    indices_ = {0, 1, 2, 0, 2, 3};
    draw_geometry(nullptr, vertices_, indices_);
}

int Renderer::segments_for(float radius) noexcept {
    if (radius <= 0.5F) {
        return 1;
    }
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(radius) * 2.4F)), 2, 40);
}

void Renderer::contour(const RectF& rect, float radius, float inset, int segments,
                       std::vector<SDL_FPoint>& out) {
    out.clear();
    const float x0 = rect.x + inset;
    const float y0 = rect.y + inset;
    const float x1 = rect.right() - inset;
    const float y1 = rect.bottom() - inset;
    const float r = std::max(radius - inset, 0.0F);
    struct Corner {
        float cx;
        float cy;
        float start;
    };
    constexpr float pi = std::numbers::pi_v<float>;
    const Corner corners[4] = {
        {x0 + r, y0 + r, pi},        // top-left: left -> up
        {x1 - r, y0 + r, 1.5F * pi}, // top-right: up -> right
        {x1 - r, y1 - r, 0.0F},      // bottom-right: right -> down
        {x0 + r, y1 - r, 0.5F * pi}, // bottom-left: down -> left
    };
    const float step = (0.5F * pi) / static_cast<float>(segments);
    for (const Corner& c : corners) {
        for (int i = 0; i <= segments; ++i) {
            const float a = c.start + (step * static_cast<float>(i));
            out.push_back(SDL_FPoint{c.cx + (r * std::cos(a)), c.cy + (r * std::sin(a))});
        }
    }
}

void Renderer::fill_rounded_rect(const RectF& rect, float radius, const Color& color,
                                 float feather) {
    if (rect.w <= 0.0F || rect.h <= 0.0F || color.a <= 0.0F) {
        return;
    }
    const float half_min = 0.5F * std::min(rect.w, rect.h);
    radius = std::clamp(radius, 0.0F, half_min);
    feather = std::max(feather, 0.5F);
    const float inner_inset = std::min(0.5F * feather, half_min);
    const float outer_inset = -0.5F * feather;
    const int segments = segments_for(radius + (0.5F * feather));

    contour(rect, radius, inner_inset, segments, ring_a_);
    contour(rect, radius, outer_inset, segments, ring_b_);

    const SDL_FColor solid = to_sdl(color);
    const SDL_FColor clear = to_sdl(color.with_alpha(0.0F));
    vertices_.clear();
    indices_.clear();

    // Core: triangle fan around the centre (the inset contour is convex).
    vertices_.push_back(vertex({rect.x + (0.5F * rect.w), rect.y + (0.5F * rect.h)}, solid));
    const auto n = static_cast<int>(ring_a_.size());
    for (const SDL_FPoint& p : ring_a_) {
        vertices_.push_back(vertex(p, solid));
    }
    for (int i = 0; i < n; ++i) {
        indices_.insert(indices_.end(), {0, 1 + i, 1 + ((i + 1) % n)});
    }
    band(ring_b_, clear, ring_a_, solid, vertices_, indices_);
    draw_geometry(nullptr, vertices_, indices_);
}

void Renderer::stroke_rounded_rect(const RectF& rect, float radius, float thickness,
                                   const Color& color) {
    if (rect.w <= 0.0F || rect.h <= 0.0F || color.a <= 0.0F || thickness <= 0.0F) {
        return;
    }
    const float half_min = 0.5F * std::min(rect.w, rect.h);
    radius = std::clamp(radius, 0.0F, half_min);
    thickness = std::min(thickness, half_min);
    const int segments = segments_for(radius + 0.5F);

    // Coverage profile across the border, measured inward from the outer edge:
    // 0 at -0.5, 1 from +0.5 to thickness-0.5, 0 at thickness+0.5 (exact for pixel-aligned rects).
    const float mid_a = 0.5F;
    const float mid_b = std::max(thickness - 0.5F, mid_a);
    contour(rect, radius, -0.5F, segments, ring_a_);
    contour(rect, radius, mid_a, segments, ring_b_);
    contour(rect, radius, mid_b, segments, ring_c_);
    contour(rect, radius, thickness + 0.5F, segments, ring_d_);

    const SDL_FColor solid = to_sdl(color);
    const SDL_FColor clear = to_sdl(color.with_alpha(0.0F));
    vertices_.clear();
    indices_.clear();
    band(ring_a_, clear, ring_b_, solid, vertices_, indices_);
    if (mid_b > mid_a) {
        band(ring_b_, solid, ring_c_, solid, vertices_, indices_);
    }
    band(ring_c_, solid, ring_d_, clear, vertices_, indices_);
    draw_geometry(nullptr, vertices_, indices_);
}

void Renderer::drop_shadow(const RectF& rect, float radius, float blur, float dy,
                           const Color& color) {
    RectF r = rect;
    r.y += dy;
    fill_rounded_rect(r, radius, color, blur * 2.0F);
}

void Renderer::draw_texture(SDL_Texture* texture, const RectF& dst, float alpha) {
    if (texture == nullptr || alpha <= 0.0F) {
        return;
    }
    SDL_SetTextureAlphaModFloat(texture, alpha);
    const SDL_FRect r{dst.x, dst.y, dst.w, dst.h};
    SDL_RenderTexture(renderer_, texture, nullptr, &r);
    SDL_SetTextureAlphaModFloat(texture, 1.0F);
}

void Renderer::draw_geometry(SDL_Texture* texture, const std::vector<SDL_Vertex>& vertices,
                             const std::vector<int>& indices) {
    if (vertices.empty() || indices.empty()) {
        return;
    }
    if (texture == nullptr) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }
    SDL_RenderGeometry(renderer_, texture, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

} // namespace navigator::ui

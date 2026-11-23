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

RectF intersect(const RectF& a, const RectF& b) noexcept {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.right(), b.right());
    const float y1 = std::min(a.bottom(), b.bottom());
    return RectF{x0, y0, std::max(x1 - x0, 0.0F), std::max(y1 - y0, 0.0F)};
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
    if (feather <= 1.0F && radius >= 1.0F && fill_rounded_rect_aligned(rect, radius, color)) {
        return;
    }
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

bool Renderer::fill_rounded_rect_aligned(const RectF& rect, float radius, const Color& color) {
    // Fast path for large pixel-aligned rects (list rows, panels): the straight parts are plain
    // rect fills, which every backend (notably the software renderer) fills far faster than a
    // triangle fan; only the four corners are geometry. Pixel-aligned straight edges are crisp
    // in the general path too, so the result is the same apart from the radius rounding up to
    // a whole pixel.
    const auto whole = [](float v) { return std::abs(v - std::round(v)) < 0.001F; };
    const float ri = std::ceil(radius);
    if (!whole(rect.x) || !whole(rect.y) || !whole(rect.w) || !whole(rect.h) ||
        rect.w < (2.0F * ri) + 2.0F || rect.h < (2.0F * ri) + 2.0F || rect.w * rect.h < 2048.0F) {
        return false;
    }
    const float x0 = rect.x;
    const float y0 = rect.y;
    const float x1 = rect.right();
    const float y1 = rect.bottom();
    fill_rect(RectF{x0 + ri, y0, rect.w - (2.0F * ri), rect.h}, color);
    fill_rect(RectF{x0, y0 + ri, ri, rect.h - (2.0F * ri)}, color);
    fill_rect(RectF{x1 - ri, y0 + ri, ri, rect.h - (2.0F * ri)}, color);

    constexpr float pi = std::numbers::pi_v<float>;
    const int segments = segments_for(ri + 0.5F);
    const SDL_FColor solid = to_sdl(color);
    const SDL_FColor clear = to_sdl(color.with_alpha(0.0F));
    vertices_.clear();
    indices_.clear();
    struct Corner {
        float cx;
        float cy;
        float start;
    };
    const Corner corners[4] = {
        {x0 + ri, y0 + ri, pi},
        {x1 - ri, y0 + ri, 1.5F * pi},
        {x1 - ri, y1 - ri, 0.0F},
        {x0 + ri, y1 - ri, 0.5F * pi},
    };
    const float step = (0.5F * pi) / static_cast<float>(segments);
    const float r_in = ri - 0.5F;
    const float r_out = ri + 0.5F;
    for (const Corner& c : corners) {
        const auto base = static_cast<int>(vertices_.size());
        vertices_.push_back(vertex({c.cx, c.cy}, solid));
        for (int i = 0; i <= segments; ++i) {
            const float a = c.start + (step * static_cast<float>(i));
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            vertices_.push_back(vertex({c.cx + (r_in * ca), c.cy + (r_in * sa)}, solid));
            vertices_.push_back(vertex({c.cx + (r_out * ca), c.cy + (r_out * sa)}, clear));
        }
        for (int i = 0; i < segments; ++i) {
            const int in0 = base + 1 + (2 * i);
            const int out0 = in0 + 1;
            const int in1 = in0 + 2;
            const int out1 = in0 + 3;
            indices_.insert(indices_.end(), {base, in0, in1, in0, out0, out1, in0, out1, in1});
        }
    }
    draw_geometry(nullptr, vertices_, indices_);
    return true;
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

SDL_Texture* Renderer::create_image_texture(int width, int height) const {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                       width, height);
    if (t != nullptr) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    }
    return t;
}

bool Renderer::update_image_texture(SDL_Texture* texture, int y, int rows, const void* pixels,
                                    int pitch) const {
    if (texture == nullptr || rows <= 0 || pixels == nullptr) {
        return false;
    }
    float w = 0.0F;
    float h = 0.0F;
    SDL_GetTextureSize(texture, &w, &h);
    const SDL_Rect r{0, y, static_cast<int>(w), rows};
    return SDL_UpdateTexture(texture, &r, pixels, pitch);
}

void Renderer::destroy_texture(SDL_Texture* texture) noexcept {
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
    }
}

int Renderer::max_texture_size() const noexcept {
    const SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
    return static_cast<int>(
        SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
}

void Renderer::draw_image(SDL_Texture* texture, const RectF& dst, float alpha, bool nearest,
                          const std::optional<RectF>& src) {
    if (texture == nullptr || alpha <= 0.0F || dst.w <= 0.0F || dst.h <= 0.0F) {
        return;
    }
    const float a = std::min(alpha, 1.0F);
    SDL_SetTextureColorModFloat(texture, a, a, a);
    SDL_SetTextureAlphaModFloat(texture, a);
    SDL_SetTextureScaleMode(texture, nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    const SDL_FRect d{dst.x, dst.y, dst.w, dst.h};
    if (src) {
        const SDL_FRect s{src->x, src->y, src->w, src->h};
        SDL_RenderTexture(renderer_, texture, &s, &d);
    } else {
        SDL_RenderTexture(renderer_, texture, nullptr, &d);
    }
    SDL_SetTextureColorModFloat(texture, 1.0F, 1.0F, 1.0F);
    SDL_SetTextureAlphaModFloat(texture, 1.0F);
}

void Renderer::draw_image_rounded(SDL_Texture* texture, const RectF& dst, float radius,
                                  float alpha) {
    if (texture == nullptr || alpha <= 0.0F || dst.w <= 1.0F || dst.h <= 1.0F) {
        return;
    }
    const float rad = std::min({radius, 0.5F * dst.w, 0.5F * dst.h});
    const int seg = segments_for(rad);
    contour(dst, rad, 0.5F, seg, ring_a_);  // opaque edge
    contour(dst, rad, -0.5F, seg, ring_b_); // transparent fringe
    const float a = std::min(alpha, 1.0F);
    const auto vertex = [&dst](const SDL_FPoint& pt, float k) {
        SDL_Vertex v{};
        v.position = pt;
        v.color = SDL_FColor{k, k, k, k}; // premultiplied: colour and alpha scale together
        v.tex_coord = SDL_FPoint{std::clamp((pt.x - dst.x) / dst.w, 0.0F, 1.0F),
                                 std::clamp((pt.y - dst.y) / dst.h, 0.0F, 1.0F)};
        return v;
    };
    vertices_.clear();
    indices_.clear();
    const auto n = static_cast<int>(ring_a_.size());
    vertices_.push_back(vertex(SDL_FPoint{dst.center_x(), dst.center_y()}, a));
    for (const SDL_FPoint& pt : ring_a_) {
        vertices_.push_back(vertex(pt, a));
    }
    for (const SDL_FPoint& pt : ring_b_) {
        vertices_.push_back(vertex(pt, 0.0F));
    }
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        indices_.insert(indices_.end(), {0, 1 + i, 1 + j});
        indices_.insert(indices_.end(), {1 + i, 1 + n + i, 1 + n + j});
        indices_.insert(indices_.end(), {1 + i, 1 + n + j, 1 + j});
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_RenderGeometry(renderer_, texture, vertices_.data(), static_cast<int>(vertices_.size()),
                       indices_.data(), static_cast<int>(indices_.size()));
}

void Renderer::stroke_line(float x0, float y0, float x1, float y1, float thickness,
                           const Color& color) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt((dx * dx) + (dy * dy));
    if (len < 1.0e-4F || color.a <= 0.0F || thickness <= 0.0F) {
        return;
    }
    // Direction and normal; the core quad is (thickness - 1) wide and fully opaque, a 1 px
    // feather on both sides and both ends fades to transparent (round caps come from circles).
    const float ux = dx / len;
    const float uy = dy / len;
    const float nx = -uy;
    const float ny = ux;
    const float core = std::max((0.5F * thickness) - 0.5F, 0.0F);
    const float outer = (0.5F * thickness) + 0.5F;
    // Thin lines get proportionally less coverage so a 1 px line is not drawn at full strength
    // over 2 px.
    const float coverage = std::min(thickness, 1.0F);
    const SDL_FColor solid = to_sdl(color.with_alpha(color.a * coverage));
    const SDL_FColor clear = to_sdl(color.with_alpha(0.0F));
    vertices_.clear();
    indices_.clear();
    const auto push = [&](float off, const SDL_FColor& c) {
        vertices_.push_back(vertex({x0 + (nx * off), y0 + (ny * off)}, c));
        vertices_.push_back(vertex({x1 + (nx * off), y1 + (ny * off)}, c));
    };
    push(-outer, clear); // 0 1
    push(-core, solid);  // 2 3
    push(core, solid);   // 4 5
    push(outer, clear);  // 6 7
    for (int band_index = 0; band_index < 3; ++band_index) {
        const int a = band_index * 2;
        indices_.insert(indices_.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    draw_geometry(nullptr, vertices_, indices_);
    const float cap = 0.5F * thickness;
    if (cap >= 0.75F) {
        fill_circle(x0, y0, cap, color);
        fill_circle(x1, y1, cap, color);
    }
}

void Renderer::fill_circle(float cx, float cy, float radius, const Color& color) {
    fill_rounded_rect(RectF{cx - radius, cy - radius, 2.0F * radius, 2.0F * radius}, radius, color);
}

void Renderer::fill_polygon(const std::vector<SDL_FPoint>& points, const Color& color) {
    const std::size_t n = points.size();
    if (n < 3 || color.a <= 0.0F) {
        return;
    }
    float cx = 0.0F;
    float cy = 0.0F;
    for (const SDL_FPoint& p : points) {
        cx += p.x;
        cy += p.y;
    }
    cx /= static_cast<float>(n);
    cy /= static_cast<float>(n);
    // Signed area: flip the normals for clockwise input.
    float area = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const SDL_FPoint& a = points[i];
        const SDL_FPoint& b = points[(i + 1) % n];
        area += (a.x * b.y) - (b.x * a.y);
    }
    const float sign = area >= 0.0F ? 1.0F : -1.0F;
    ring_a_.assign(n, SDL_FPoint{});
    ring_b_.assign(n, SDL_FPoint{});
    for (std::size_t i = 0; i < n; ++i) {
        const SDL_FPoint& prev = points[(i + n - 1) % n];
        const SDL_FPoint& cur = points[i];
        const SDL_FPoint& next = points[(i + 1) % n];
        const auto normal = [sign](SDL_FPoint a, SDL_FPoint b) {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float len = std::max(std::sqrt((dx * dx) + (dy * dy)), 1.0e-4F);
            return SDL_FPoint{sign * dy / len, -sign * dx / len};
        };
        const SDL_FPoint n0 = normal(prev, cur);
        const SDL_FPoint n1 = normal(cur, next);
        float mx = n0.x + n1.x;
        float my = n0.y + n1.y;
        const float ml = std::sqrt((mx * mx) + (my * my));
        if (ml < 1.0e-3F) {
            mx = n0.x;
            my = n0.y;
        } else {
            // Miter length so both edges move by 0.5 px, capped for sharp tips.
            const float cosh = std::max(ml * 0.5F, 0.35F);
            mx = (mx / ml) * (0.5F / cosh);
            my = (my / ml) * (0.5F / cosh);
        }
        ring_a_[i] = SDL_FPoint{cur.x + mx, cur.y + my}; // outer fringe
        ring_b_[i] = SDL_FPoint{cur.x - mx, cur.y - my}; // solid edge
    }
    const SDL_FColor solid = to_sdl(color);
    const SDL_FColor clear = to_sdl(color.with_alpha(0.0F));
    vertices_.clear();
    indices_.clear();
    band(ring_a_, clear, ring_b_, solid, vertices_, indices_);
    const auto centre = static_cast<int>(vertices_.size());
    vertices_.push_back(vertex({cx, cy}, solid));
    for (std::size_t i = 0; i < n; ++i) {
        vertices_.push_back(vertex(ring_b_[i], solid));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const int a = centre + 1 + static_cast<int>(i);
        const int b = centre + 1 + static_cast<int>((i + 1) % n);
        indices_.insert(indices_.end(), {centre, a, b});
    }
    draw_geometry(nullptr, vertices_, indices_);
}

void Renderer::set_clip(const std::optional<RectF>& rect) {
    clip_ = rect;
    if (!rect) {
        SDL_SetRenderClipRect(renderer_, nullptr);
        return;
    }
    const auto x0 = static_cast<int>(std::floor(rect->x));
    const auto y0 = static_cast<int>(std::floor(rect->y));
    const auto x1 = static_cast<int>(std::ceil(rect->right()));
    const auto y1 = static_cast<int>(std::ceil(rect->bottom()));
    const SDL_Rect r{x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)};
    SDL_SetRenderClipRect(renderer_, &r);
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

// src/ui/render/renderer.hpp
//
// The project renderer: a thin immediate-mode drawing layer over SDL_Renderer. Every pixel the UI
// draws goes through here (or through text::TextRenderer, which uses it). Coordinates are physical
// pixels; callers convert from dp with the display scale. Shapes are anti-aliased analytically
// with a feathered fringe, so pixel-aligned edges stay crisp and curves stay smooth.
// Thread-safety: UI thread only.
#pragma once

#include <optional>
#include <vector>

#include <SDL3/SDL.h>

#include "theme/theme.hpp"

namespace navigator::ui {

struct RectF {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;

    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
};

// Rounds a rect's edges to whole pixels (keeps hairlines and fills crisp).
[[nodiscard]] RectF snap(const RectF& r) noexcept;

[[nodiscard]] constexpr bool contains(const RectF& r, float px, float py) noexcept {
    return px >= r.x && px < r.right() && py >= r.y && py < r.bottom();
}

// Intersection (zero-sized when disjoint).
[[nodiscard]] RectF intersect(const RectF& a, const RectF& b) noexcept;

[[nodiscard]] SDL_FColor to_sdl(const Color& c) noexcept;

class Renderer {
public:
    explicit Renderer(SDL_Renderer* renderer) noexcept : renderer_(renderer) {}

    [[nodiscard]] SDL_Renderer* sdl() const noexcept { return renderer_; }

    void clear(const Color& color);

    void fill_rect(const RectF& rect, const Color& color);
    void fill_rect_vgradient(const RectF& rect, const Color& top, const Color& bottom);

    // Filled rounded rectangle. `feather` is the width of the AA ramp in px (1 = crisp edge);
    // larger values give soft shapes (shadows, glows).
    void fill_rounded_rect(const RectF& rect, float radius, const Color& color,
                           float feather = 1.0F);

    // Border drawn inside `rect` with the given thickness (px).
    void stroke_rounded_rect(const RectF& rect, float radius, float thickness, const Color& color);

    // Soft drop shadow under a rounded rect: `blur` px falloff, offset by (dx, dy).
    void drop_shadow(const RectF& rect, float radius, float blur, float dy, const Color& color);

    void draw_texture(SDL_Texture* texture, const RectF& dst, float alpha = 1.0F);

    // Anti-aliased line segment from (x0, y0) to (x1, y1), `thickness` px, round caps.
    void stroke_line(float x0, float y0, float x1, float y1, float thickness, const Color& color);

    // Filled anti-aliased circle.
    void fill_circle(float cx, float cy, float radius, const Color& color);

    // Restricts drawing to `rect` (rounded outward to whole pixels); nullopt removes the clip.
    void set_clip(const std::optional<RectF>& rect);
    [[nodiscard]] const std::optional<RectF>& clip() const noexcept { return clip_; }

    // Raw textured/colored triangles (used by the text renderer).
    void draw_geometry(SDL_Texture* texture, const std::vector<SDL_Vertex>& vertices,
                       const std::vector<int>& indices);

private:
    // Appends the contour of `rect` inset by `inset` px (negative = outset) to `out`.
    static void contour(const RectF& rect, float radius, float inset, int segments,
                        std::vector<SDL_FPoint>& out);
    static int segments_for(float radius) noexcept;
    bool fill_rounded_rect_aligned(const RectF& rect, float radius, const Color& color);

    SDL_Renderer* renderer_ = nullptr;
    std::optional<RectF> clip_;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
    std::vector<SDL_FPoint> ring_a_;
    std::vector<SDL_FPoint> ring_b_;
    std::vector<SDL_FPoint> ring_c_;
    std::vector<SDL_FPoint> ring_d_;
};

} // namespace navigator::ui

// src/ui/text/text_renderer.hpp
//
// Glyph atlas (one RGBA texture, shelf-packed, coverage in alpha) and text drawing through the
// project renderer. Glyphs are cached by (font, glyph, subpixel bin, coverage curve). Text is
// positioned in physical pixels with 1/4 px horizontal subpixel bins and whole-pixel baselines.
//
// Coverage curves: SDL_Renderer blends in sRGB space, which makes light-on-dark text look thin.
// Light text therefore uses a contrast-boosted coverage curve; dark text uses linear coverage.
// Thread-safety: UI thread only.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <navigator/types.hpp>

#include "render/renderer.hpp"
#include "render/sdl_handles.hpp"
#include "text/font.hpp"
#include "text/font_cache.hpp"
#include "theme/theme.hpp"

namespace navigator::ui::text {

class GlyphAtlas {
public:
    static constexpr int kSubpixelBins = 4;

    struct Entry {
        float u = 0.0F; // atlas px
        float v = 0.0F;
        int width = 0;
        int height = 0;
        int left = 0;
        int top = 0;
    };

    // Errors: PlatformError (texture creation failed).
    [[nodiscard]] static Result<std::unique_ptr<GlyphAtlas>> create(SDL_Renderer* renderer,
                                                                    int size = 1024);

    // nullptr if the glyph cannot be rasterized. May clear the atlas when it is full.
    [[nodiscard]] const Entry* get(Font& font, std::uint32_t glyph, int bin, bool light_text);

    void clear();
    [[nodiscard]] SDL_Texture* texture() const noexcept { return texture_.get(); }
    [[nodiscard]] int size() const noexcept { return size_; }

private:
    GlyphAtlas() = default;
    bool allocate(int w, int h, int& x, int& y);

    SDL_Renderer* renderer_ = nullptr;
    TexturePtr texture_;
    int size_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int row_height_ = 0;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::array<std::uint8_t, 256> curve_dark_{};
    std::array<std::uint8_t, 256> curve_light_{};
    std::vector<std::uint8_t> upload_;
};

class TextRenderer {
public:
    TextRenderer(Renderer& renderer, GlyphAtlas& atlas) noexcept
        : renderer_(&renderer), atlas_(&atlas) {}

    // Draws `run` with its origin at (x, baseline) in px. The baseline is snapped to a pixel row.
    void draw(Font& font, const ShapedRun& run, float x, float baseline, const Color& color);

    // Draws every run of `layout` (each with its own font) with its origin at (x, baseline).
    void draw(const TextLayout& layout, float x, float baseline, const Color& color);

private:
    Renderer* renderer_;
    GlyphAtlas* atlas_;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

} // namespace navigator::ui::text

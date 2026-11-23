// src/ui/shell/painter.hpp
//
// Per-frame drawing context for widgets: dp -> px conversion, text shaping (cached, with font
// fallback) and drawing.
// Widgets draw only through a Painter (which uses the project renderer); they never touch SDL.
// Thread-safety: UI thread only; lives for one frame.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "render/renderer.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"

namespace navigator::ui {

// A shaped string from the FontCache's shape cache (shared, immutable). Valid for the frame it
// was shaped in; null when the UI font could not be opened (measures 0, draws nothing).
struct ShapedText {
    std::shared_ptr<const text::TextLayout> layout;

    [[nodiscard]] float width() const noexcept { return layout ? layout->advance : 0.0F; }
    [[nodiscard]] float cap_height() const noexcept { return layout ? layout->cap_height : 0.0F; }
};

class Painter {
public:
    Painter(Renderer& renderer, text::TextRenderer& text, text::FontCache& fonts, float scale,
            std::string_view language) noexcept
        : renderer_(&renderer), text_(&text), fonts_(&fonts), scale_(scale), language_(language) {}

    [[nodiscard]] float px(float dp) const noexcept { return dp * scale_; }
    [[nodiscard]] float scale() const noexcept { return scale_; }
    [[nodiscard]] Renderer& renderer() const noexcept { return *renderer_; }

    [[nodiscard]] ShapedText shape(const TextStyle& style, std::string_view utf8) const;
    void draw(const ShapedText& text, float x, float baseline, const Color& color) const;

    // Phase 5: `text` with the byte ranges `ranges` (sorted, half-open, offsets into the shaped
    // string) drawn in `highlight` and underlined by a hairline in `underline` (matched
    // characters: never colour alone).
    void draw_highlighted(const ShapedText& text, float x, float baseline, const Color& color,
                          const Color& highlight, const Color& underline,
                          const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges,
                          std::size_t text_size) const;
    // Pen x extents (px, from the text origin) of the byte ranges, merged per range.
    [[nodiscard]] static std::vector<std::pair<float, float>>
    range_extents(const ShapedText& text, std::size_t text_size,
                  const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges);

private:
    Renderer* renderer_;
    text::TextRenderer* text_;
    text::FontCache* fonts_;
    float scale_;
    std::string_view language_;
};

} // namespace navigator::ui

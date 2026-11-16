// src/ui/shell/painter.hpp
//
// Per-frame drawing context for widgets: dp -> px conversion, text shaping and drawing.
// Widgets draw only through a Painter (which uses the project renderer); they never touch SDL.
// Thread-safety: UI thread only; lives for one frame.
#pragma once

#include <string_view>

#include "render/renderer.hpp"
#include "text/text_renderer.hpp"
#include "theme/theme.hpp"

namespace navigator::ui {

struct ShapedText {
    text::Font* font = nullptr; // nullptr when the font could not be opened (draws nothing)
    text::ShapedRun run;

    [[nodiscard]] float width() const noexcept { return run.advance; }
    [[nodiscard]] float cap_height() const noexcept {
        return font != nullptr ? font->metrics().cap_height : 0.0F;
    }
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

private:
    Renderer* renderer_;
    text::TextRenderer* text_;
    text::FontCache* fonts_;
    float scale_;
    std::string_view language_;
};

} // namespace navigator::ui

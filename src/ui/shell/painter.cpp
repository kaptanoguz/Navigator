// src/ui/shell/painter.cpp — see painter.hpp.
#include "shell/painter.hpp"

namespace navigator::ui {

ShapedText Painter::shape(const TextStyle& style, std::string_view utf8) const {
    return ShapedText{fonts_->shape(style, scale_, utf8, language_)};
}

void Painter::draw(const ShapedText& text, float x, float baseline, const Color& color) const {
    if (text.layout) {
        text_->draw(*text.layout, x, baseline, color);
    }
}

} // namespace navigator::ui

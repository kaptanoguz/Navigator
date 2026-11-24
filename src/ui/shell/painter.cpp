// src/ui/shell/painter.cpp — see painter.hpp.
#include "shell/painter.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui {

ShapedText Painter::shape(const TextStyle& style, std::string_view utf8) const {
    if (font_scale_ == 1.0F) {
        return ShapedText{fonts_->shape(style, scale_, utf8, language_)};
    }
    TextStyle scaled = style;
    scaled.size = std::max(8.0F, style.size * font_scale_);
    return ShapedText{fonts_->shape(scaled, scale_, utf8, language_)};
}

void Painter::draw(const ShapedText& text, float x, float baseline, const Color& color) const {
    if (text.layout) {
        text_->draw(*text.layout, x, baseline, color);
    }
}

std::vector<std::pair<float, float>>
Painter::range_extents(const ShapedText& text, std::size_t text_size,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges) {
    std::vector<std::pair<float, float>> out;
    if (!text.layout || ranges.empty()) {
        return out;
    }
    // Cluster starts in logical order with their pen x (as TextFitter does).
    std::vector<std::pair<std::uint32_t, float>> b;
    for (const text::TextLayout::Run& run : text.layout->runs) {
        for (const text::ShapedGlyph& g : run.shaped.glyphs) {
            const float x = run.x + g.x;
            if (!b.empty() && b.back().first == g.cluster) {
                b.back().second = std::min(b.back().second, x);
                continue;
            }
            b.emplace_back(g.cluster, x);
        }
    }
    std::sort(b.begin(), b.end());
    b.emplace_back(static_cast<std::uint32_t>(text_size), text.layout->advance);
    for (const auto& [begin, end] : ranges) {
        float x0 = -1.0F;
        float x1 = -1.0F;
        for (const auto& [byte, x] : b) {
            if (x0 < 0.0F && byte >= begin) {
                x0 = x;
            }
            if (byte >= end) {
                x1 = x;
                break;
            }
        }
        if (x0 >= 0.0F && x1 > x0) {
            out.emplace_back(x0, x1);
        }
    }
    return out;
}

void Painter::draw_highlighted(const ShapedText& text, float x, float baseline, const Color& color,
                               const Color& highlight, const Color& underline,
                               const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ranges,
                               std::size_t text_size) const {
    if (!text.layout) {
        return;
    }
    if (ranges.empty()) {
        text_->draw(*text.layout, x, baseline, color);
        return;
    }
    text_->draw_ranges(*text.layout, x, baseline, color, highlight, ranges);
    const float t = std::max(1.0F, std::round(px(1.0F)));
    const float y = std::round(baseline + px(2.0F));
    for (const auto& [x0, x1] : range_extents(text, text_size, ranges)) {
        renderer_->fill_rect(RectF{std::round(x + x0), y, std::max(1.0F, std::round(x1 - x0)), t},
                             underline);
    }
}

} // namespace navigator::ui

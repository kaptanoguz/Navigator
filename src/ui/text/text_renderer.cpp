// src/ui/text/text_renderer.cpp — see text_renderer.hpp.
#include "text/text_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace navigator::ui::text {
namespace {

// Exponent applied to coverage for light-on-dark text (< 1 thickens AA edges). Partial gamma
// correction: full sRGB correction (1/2.2) looks bold; 1/1.45 matches perceived weight of the
// same text drawn dark-on-light.
constexpr float kLightTextGamma = 1.0F / 1.45F;
constexpr int kPadding = 1;

std::uint64_t key(std::uint32_t font, std::uint32_t glyph, int bin, bool light) noexcept {
    return (static_cast<std::uint64_t>(font & 0xFFFFFFU) << 40U) |
           (static_cast<std::uint64_t>(glyph) << 8U) |
           (static_cast<std::uint64_t>(static_cast<unsigned>(bin) & 0x7FU) << 1U) |
           (light ? 1U : 0U);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GlyphAtlas
// ---------------------------------------------------------------------------------------------
Result<std::unique_ptr<GlyphAtlas>> GlyphAtlas::create(SDL_Renderer* renderer, int size) {
    std::unique_ptr<GlyphAtlas> atlas(new GlyphAtlas());
    atlas->renderer_ = renderer;
    atlas->size_ = size;
    atlas->texture_.reset(
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size));
    if (!atlas->texture_) {
        return fail(ErrorCode::PlatformError,
                    std::string("glyph atlas texture creation failed: ") + SDL_GetError());
    }
    SDL_SetTextureBlendMode(atlas->texture_.get(), SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas->texture_.get(), SDL_SCALEMODE_NEAREST);
    for (int i = 0; i < 256; ++i) {
        const float c = static_cast<float>(i) / 255.0F;
        const auto idx = static_cast<std::size_t>(i);
        atlas->curve_dark_[idx] = static_cast<std::uint8_t>(i);
        atlas->curve_light_[idx] =
            static_cast<std::uint8_t>(std::lround(std::pow(c, kLightTextGamma) * 255.0F));
    }
    atlas->clear();
    return atlas;
}

void GlyphAtlas::clear() {
    entries_.clear();
    cursor_x_ = kPadding;
    cursor_y_ = kPadding;
    row_height_ = 0;
    const auto bytes = static_cast<std::size_t>(size_) * static_cast<std::size_t>(size_) * 4U;
    std::vector<std::uint8_t> zero(bytes, 0);
    // Keep RGB white so bilinear sampling (if ever enabled) never bleeds dark fringes.
    for (std::size_t i = 0; i < bytes; i += 4) {
        zero[i] = zero[i + 1] = zero[i + 2] = 255;
    }
    SDL_UpdateTexture(texture_.get(), nullptr, zero.data(), size_ * 4);
}

bool GlyphAtlas::allocate(int w, int h, int& x, int& y) {
    if (w + (2 * kPadding) > size_ || h + (2 * kPadding) > size_) {
        return false;
    }
    if (cursor_x_ + w + kPadding > size_) {
        cursor_x_ = kPadding;
        cursor_y_ += row_height_ + kPadding;
        row_height_ = 0;
    }
    if (cursor_y_ + h + kPadding > size_) {
        return false;
    }
    x = cursor_x_;
    y = cursor_y_;
    cursor_x_ += w + kPadding;
    row_height_ = std::max(row_height_, h);
    return true;
}

const GlyphAtlas::Entry* GlyphAtlas::get(Font& font, std::uint32_t glyph, int bin,
                                         bool light_text) {
    const std::uint64_t k = key(font.id(), glyph, bin, light_text);
    if (const auto it = entries_.find(k); it != entries_.end()) {
        return &it->second;
    }
    auto bitmap = font.rasterize(glyph, static_cast<float>(bin) / kSubpixelBins);
    if (!bitmap) {
        return nullptr;
    }
    Entry e;
    e.width = bitmap->width;
    e.height = bitmap->height;
    e.left = bitmap->left;
    e.top = bitmap->top;
    if (e.width > 0 && e.height > 0) {
        int x = 0;
        int y = 0;
        if (!allocate(e.width, e.height, x, y)) {
            clear(); // full: start over (earlier draws were already flushed by SDL)
            if (!allocate(e.width, e.height, x, y)) {
                return nullptr;
            }
        }
        e.u = static_cast<float>(x);
        e.v = static_cast<float>(y);
        const auto& curve = light_text ? curve_light_ : curve_dark_;
        upload_.resize(bitmap->coverage.size() * 4U);
        for (std::size_t i = 0; i < bitmap->coverage.size(); ++i) {
            upload_[(i * 4U) + 0U] = 255;
            upload_[(i * 4U) + 1U] = 255;
            upload_[(i * 4U) + 2U] = 255;
            upload_[(i * 4U) + 3U] = curve[bitmap->coverage[i]];
        }
        const SDL_Rect dst{x, y, e.width, e.height};
        SDL_UpdateTexture(texture_.get(), &dst, upload_.data(), e.width * 4);
    }
    return &entries_.emplace(k, e).first->second;
}

// ---------------------------------------------------------------------------------------------
// TextRenderer
// ---------------------------------------------------------------------------------------------
void TextRenderer::draw(Font& font, const ShapedRun& run, float x, float baseline,
                        const Color& color) {
    if (run.glyphs.empty() || color.a <= 0.0F) {
        return;
    }
    const bool light = relative_luminance(color) > 0.35F;
    const float base_y = std::round(baseline);
    const SDL_FColor c = to_sdl(color);
    const float inv = 1.0F / static_cast<float>(atlas_->size());
    vertices_.clear();
    indices_.clear();
    for (const ShapedGlyph& g : run.glyphs) {
        const float gx = x + g.x;
        const float whole = std::floor(gx);
        const int bin = std::min(static_cast<int>((gx - whole) * GlyphAtlas::kSubpixelBins),
                                 GlyphAtlas::kSubpixelBins - 1);
        const GlyphAtlas::Entry* e = atlas_->get(font, g.glyph, bin, light);
        if (e == nullptr || e->width == 0 || e->height == 0) {
            continue;
        }
        const float x0 = whole + static_cast<float>(e->left);
        const float y0 = base_y + std::round(g.y) - static_cast<float>(e->top);
        const float x1 = x0 + static_cast<float>(e->width);
        const float y1 = y0 + static_cast<float>(e->height);
        const float u0 = e->u * inv;
        const float v0 = e->v * inv;
        const float u1 = (e->u + static_cast<float>(e->width)) * inv;
        const float v1 = (e->v + static_cast<float>(e->height)) * inv;
        const auto base = static_cast<int>(vertices_.size());
        vertices_.push_back(SDL_Vertex{{x0, y0}, c, {u0, v0}});
        vertices_.push_back(SDL_Vertex{{x1, y0}, c, {u1, v0}});
        vertices_.push_back(SDL_Vertex{{x1, y1}, c, {u1, v1}});
        vertices_.push_back(SDL_Vertex{{x0, y1}, c, {u0, v1}});
        indices_.insert(indices_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    renderer_->draw_geometry(atlas_->texture(), vertices_, indices_);
}

void TextRenderer::draw(const TextLayout& layout, float x, float baseline, const Color& color) {
    for (const TextLayout::Run& run : layout.runs) {
        if (run.font != nullptr) {
            draw(*run.font, run.shaped, x + run.x, baseline, color);
        }
    }
}

} // namespace navigator::ui::text

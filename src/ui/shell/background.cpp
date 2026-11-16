// src/ui/shell/background.cpp — see background.hpp.
#include "shell/background.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace navigator::ui {
namespace {

// Deterministic integer hash -> [0, 1).
float hash01(std::uint32_t x) noexcept {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return static_cast<float>(x >> 8U) * (1.0F / 16777216.0F);
}

std::uint8_t quantize(float srgb, float noise) noexcept {
    const float v = (srgb * 255.0F) + noise;
    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

constexpr float kGlowCenterY = 0.40F; // fraction of height
constexpr float kGlowRadiusX = 0.70F; // fraction of width
constexpr float kGlowRadiusY = 0.85F; // fraction of height
constexpr int kDownscale = 2;

} // namespace

std::vector<std::uint8_t> render_backdrop(const Palette& palette, int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4U);
    const float base[3] = {srgb_to_linear(palette.background.r),
                           srgb_to_linear(palette.background.g),
                           srgb_to_linear(palette.background.b)};
    const float glow[3] = {srgb_to_linear(palette.background_alt.r),
                           srgb_to_linear(palette.background_alt.g),
                           srgb_to_linear(palette.background_alt.b)};
    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);
    for (int y = 0; y < height; ++y) {
        const float ny = ((static_cast<float>(y) + 0.5F) / fh - kGlowCenterY) / kGlowRadiusY;
        for (int x = 0; x < width; ++x) {
            const float nx = ((static_cast<float>(x) + 0.5F) / fw - 0.5F) / kGlowRadiusX;
            const float d = std::min(std::sqrt((nx * nx) + (ny * ny)), 1.0F);
            const float s = 1.0F - (d * d * (3.0F - (2.0F * d))); // 1 - smoothstep
            const float t = s * s;
            const auto idx = ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                              static_cast<std::size_t>(x)) *
                             4U;
            const auto seed = static_cast<std::uint32_t>((y * 7919) + x);
            const float noise = hash01(seed * 2U) - hash01((seed * 2U) + 1U); // TPDF, +-1 LSB
            for (std::size_t c = 0; c < 3; ++c) {
                const float lin = base[c] + ((glow[c] - base[c]) * t);
                pixels[idx + c] = quantize(linear_to_srgb(lin), noise);
            }
            pixels[idx + 3U] = 255;
        }
    }
    return pixels;
}

Result<TexturePtr> make_backdrop_texture(SDL_Renderer* renderer, const Palette& palette,
                                         int pixel_w, int pixel_h) {
    const int w = std::max(1, pixel_w / kDownscale);
    const int h = std::max(1, pixel_h / kDownscale);
    TexturePtr tex{
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h)};
    if (!tex) {
        return fail(ErrorCode::PlatformError,
                    std::string("backdrop texture creation failed: ") + SDL_GetError());
    }
    const std::vector<std::uint8_t> pixels = render_backdrop(palette, w, h);
    SDL_UpdateTexture(tex.get(), nullptr, pixels.data(), w * 4);
    SDL_SetTextureScaleMode(tex.get(), SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(tex.get(), SDL_BLENDMODE_BLEND);
    return tex;
}

} // namespace navigator::ui

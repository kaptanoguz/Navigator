// src/ui/theme/theme.cpp — palettes and color math. See theme.hpp.
#include "theme/theme.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui {
namespace {

// Dark: near-black with a faint blue cast; neutrals step up in small, even luminance increments
// so panels separate without hard edges. One restrained periwinkle accent.
constexpr Palette kDark{
    .background = rgb(0x111318),
    .background_alt = rgb(0x181C25),
    .surface = rgb(0x171A21),
    .surface_raised = rgb(0x1E222B),
    .text_primary = rgb(0xE9EBF1),
    .text_secondary = rgb(0xA4AAB8),
    .text_muted = rgb(0x737A89),
    .accent = rgb(0x8EA6FF),
    .border = rgb(0x2B303B),
    .shadow = Color{0.0F, 0.0F, 0.02F, 0.55F},
    .keycap_edge = rgb(0x0B0D11),
};

// Light: cool paper white, ink near-black, accent deepened for contrast on white.
constexpr Palette kLight{
    .background = rgb(0xF5F6F8),
    .background_alt = rgb(0xFFFFFF),
    .surface = rgb(0xFFFFFF),
    .surface_raised = rgb(0xFFFFFF),
    .text_primary = rgb(0x15171C),
    .text_secondary = rgb(0x535966),
    .text_muted = rgb(0x7D8391),
    .accent = rgb(0x3F63DB),
    .border = rgb(0xDDE1E8),
    .shadow = Color{0.07F, 0.09F, 0.16F, 0.16F},
    .keycap_edge = rgb(0xC9CED8),
};

} // namespace

float srgb_to_linear(float c) noexcept {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

float linear_to_srgb(float c) noexcept {
    c = std::clamp(c, 0.0F, 1.0F);
    return c <= 0.0031308F ? c * 12.92F : (1.055F * std::pow(c, 1.0F / 2.4F)) - 0.055F;
}

float relative_luminance(const Color& c) noexcept {
    return (0.2126F * srgb_to_linear(c.r)) + (0.7152F * srgb_to_linear(c.g)) +
           (0.0722F * srgb_to_linear(c.b));
}

float contrast_ratio(const Color& a, const Color& b) noexcept {
    const float la = relative_luminance(a);
    const float lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05F) / (std::min(la, lb) + 0.05F);
}

Color mix(const Color& a, const Color& b, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    const auto channel = [t](float x, float y) {
        const float lx = srgb_to_linear(x);
        const float ly = srgb_to_linear(y);
        return linear_to_srgb(lx + ((ly - lx) * t));
    };
    return Color{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), a.a + ((b.a - a.a) * t)};
}

const Palette& palette(Theme theme) noexcept { return theme == Theme::Light ? kLight : kDark; }

Palette mix(const Palette& a, const Palette& b, float t) noexcept {
    return Palette{
        .background = mix(a.background, b.background, t),
        .background_alt = mix(a.background_alt, b.background_alt, t),
        .surface = mix(a.surface, b.surface, t),
        .surface_raised = mix(a.surface_raised, b.surface_raised, t),
        .text_primary = mix(a.text_primary, b.text_primary, t),
        .text_secondary = mix(a.text_secondary, b.text_secondary, t),
        .text_muted = mix(a.text_muted, b.text_muted, t),
        .accent = mix(a.accent, b.accent, t),
        .border = mix(a.border, b.border, t),
        .shadow = mix(a.shadow, b.shadow, t),
        .keycap_edge = mix(a.keycap_edge, b.keycap_edge, t),
    };
}

Theme toggled(Theme theme) noexcept { return theme == Theme::Dark ? Theme::Light : Theme::Dark; }

} // namespace navigator::ui

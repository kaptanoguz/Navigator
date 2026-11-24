// src/ui/theme/theme.cpp — palettes and color math. See theme.hpp.
#include "theme/theme.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui {
namespace {

// Dark: near-black with a faint blue cast; neutrals step up in small, even luminance increments
// so panels separate without hard edges. One restrained periwinkle accent.
// Text tiers, worst case over every surface in either theme: primary >= 14, secondary >= 6.5,
// keycap legends >= 5.4, muted >= 4.7 (all WCAG AA body text). See tests/ui/theme_test.cpp.
constexpr Palette kDark{
    .background = rgb(0x111318),
    .background_alt = rgb(0x181C25),
    .surface = rgb(0x171A21),
    .surface_raised = rgb(0x1E222B),
    .text_primary = rgb(0xE9EBF1),
    .text_secondary = rgb(0xA4AAB8),
    .text_muted = rgb(0x868D9C),
    .accent = rgb(0x8EA6FF),
    .border = rgb(0x2B303B),
    .shadow = Color{0.0F, 0.0F, 0.02F, 0.55F},
    .keycap_edge = rgb(0x0B0D11),
    .keycap_border = rgb(0x434A5C),
    .keycap_text = rgb(0x959BAA),
    .row_alt = Color{1.0F, 1.0F, 1.0F, 0.032F},
    .row_hover = Color{1.0F, 1.0F, 1.0F, 0.065F},
    .selection = Color{0.557F, 0.651F, 1.0F, 0.20F},
    .selection_inactive = Color{1.0F, 1.0F, 1.0F, 0.085F},
    .selection_marker = rgb(0x8EA6FF),
    .focus_ring = Color{0.557F, 0.651F, 1.0F, 0.80F},
    .control_hover = Color{1.0F, 1.0F, 1.0F, 0.06F},
    .control_pressed = Color{1.0F, 1.0F, 1.0F, 0.10F},
    .danger = rgb(0xFF8F86),
    .danger_surface = rgb(0x2B1A1E),
    .danger_border = rgb(0x4D2A30),
    .scrollbar = Color{0.80F, 0.83F, 0.90F, 0.28F},
    .scrollbar_hover = Color{0.80F, 0.83F, 0.90F, 0.50F},
    .icon_folder_back = rgb(0x546AAE),
    .icon_folder_front = rgb(0x7D95E6),
    .icon_page = rgb(0x2F3644),
    .icon_page_border = rgb(0x758093),
    .tint_image = rgb(0x4CC2B0),
    .tint_audio = rgb(0xE57BB8),
    .tint_video = rgb(0xA88BF0),
    .tint_archive = rgb(0xE0B04E),
    .tint_code = rgb(0x6CC488),
    .tint_document = rgb(0x7FA0F0),
    .tint_pdf = rgb(0xEF6B62),
    .tint_sheet = rgb(0x55B96E),
    .tint_slides = rgb(0xEF925A),
    .tint_program = rgb(0x9AA3B5),
    .on_accent = rgb(0x0F1220),
    .warning = rgb(0xE5A93B),
    .match = rgb(0xA3B6FF),
    .chip = Color{0.557F, 0.651F, 1.0F, 0.16F},
    .chip_text = rgb(0xB4C3FF),
    .control_border = rgb(0x737A8C),
};

// Light: cool paper white, ink near-black, accent deepened for contrast on white.
constexpr Palette kLight{
    .background = rgb(0xF5F6F8),
    .background_alt = rgb(0xFFFFFF),
    .surface = rgb(0xFFFFFF),
    .surface_raised = rgb(0xFFFFFF),
    .text_primary = rgb(0x15171C),
    .text_secondary = rgb(0x535966),
    .text_muted = rgb(0x676D7A),
    .accent = rgb(0x3F63DB),
    .border = rgb(0xDDE1E8),
    .shadow = Color{0.07F, 0.09F, 0.16F, 0.16F},
    .keycap_edge = rgb(0xC9CED8),
    .keycap_border = rgb(0xB8BEC9),
    .keycap_text = rgb(0x5E6472),
    .row_alt = Color{0.08F, 0.10F, 0.20F, 0.026F},
    .row_hover = Color{0.08F, 0.10F, 0.20F, 0.060F},
    .selection = Color{0.247F, 0.388F, 0.859F, 0.14F},
    .selection_inactive = Color{0.08F, 0.10F, 0.20F, 0.08F},
    .selection_marker = rgb(0x3F63DB),
    .focus_ring = Color{0.247F, 0.388F, 0.859F, 0.85F},
    .control_hover = Color{0.08F, 0.10F, 0.20F, 0.06F},
    .control_pressed = Color{0.08F, 0.10F, 0.20F, 0.11F},
    .danger = rgb(0xC0322B),
    .danger_surface = rgb(0xFDEFEE),
    .danger_border = rgb(0xF2C9C6),
    .scrollbar = Color{0.10F, 0.12F, 0.20F, 0.26F},
    .scrollbar_hover = Color{0.10F, 0.12F, 0.20F, 0.45F},
    .icon_folder_back = rgb(0x5C7FE6),
    .icon_folder_front = rgb(0x8AA6F6),
    .icon_page = rgb(0xFFFFFF),
    .icon_page_border = rgb(0x9DA4B1),
    .tint_image = rgb(0x16958A),
    .tint_audio = rgb(0xC2428C),
    .tint_video = rgb(0x7656D6),
    .tint_archive = rgb(0xB7861A),
    .tint_code = rgb(0x2E9A55),
    .tint_document = rgb(0x4469D8),
    .tint_pdf = rgb(0xD33A31),
    .tint_sheet = rgb(0x22893F),
    .tint_slides = rgb(0xD9692A),
    .tint_program = rgb(0x6B7385),
    .on_accent = rgb(0xFFFFFF),
    .warning = rgb(0xB06F00),
    .match = rgb(0x2C49B8),
    .chip = Color{0.247F, 0.388F, 0.859F, 0.12F},
    .chip_text = rgb(0x2C49B8),
    .control_border = rgb(0x868C99),
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

Palette palette_from_scheme(const appearance::ThemeScheme& scheme) noexcept {
    Palette p = palette(scheme.base);
    constexpr float inv = 1.0F / 255.0F;
    auto to_color = [](appearance::Rgba8 c) noexcept {
        return Color{static_cast<float>(c.r) * inv, static_cast<float>(c.g) * inv,
                     static_cast<float>(c.b) * inv, static_cast<float>(c.a) * inv};
    };
    const auto apply = [&](const std::optional<appearance::Rgba8>& src, Color& dst) noexcept {
        if (src) {
            dst = to_color(*src);
        }
    };
    const appearance::ThemeColors& c = scheme.colors;
    apply(c.background, p.background);
    apply(c.background_alt, p.background_alt);
    apply(c.surface, p.surface);
    apply(c.surface_raised, p.surface_raised);
    apply(c.text_primary, p.text_primary);
    apply(c.text_secondary, p.text_secondary);
    apply(c.text_muted, p.text_muted);
    if (c.accent) {
        p.accent = to_color(*c.accent);
        p.selection_marker = p.accent;
        p.match = p.accent;
        p.chip = p.accent.with_alpha(scheme.base == Theme::Light ? 0.14F : 0.18F);
        p.chip_text = p.accent;
        if (!c.selection) {
            p.selection = p.accent.with_alpha(scheme.base == Theme::Light ? 0.14F : 0.20F);
        }
        if (!c.focus_ring) {
            p.focus_ring = p.accent.with_alpha(0.82F);
        }
    }
    apply(c.on_accent, p.on_accent);
    apply(c.border, p.border);
    apply(c.danger, p.danger);
    apply(c.warning, p.warning);
    apply(c.selection, p.selection);
    apply(c.row_alt, p.row_alt);
    apply(c.row_hover, p.row_hover);
    apply(c.focus_ring, p.focus_ring);
    apply(c.icon_folder_back, p.icon_folder_back);
    apply(c.icon_folder_front, p.icon_folder_front);
    return p;
}

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
        .keycap_border = mix(a.keycap_border, b.keycap_border, t),
        .keycap_text = mix(a.keycap_text, b.keycap_text, t),
        .row_alt = mix(a.row_alt, b.row_alt, t),
        .row_hover = mix(a.row_hover, b.row_hover, t),
        .selection = mix(a.selection, b.selection, t),
        .selection_inactive = mix(a.selection_inactive, b.selection_inactive, t),
        .selection_marker = mix(a.selection_marker, b.selection_marker, t),
        .focus_ring = mix(a.focus_ring, b.focus_ring, t),
        .control_hover = mix(a.control_hover, b.control_hover, t),
        .control_pressed = mix(a.control_pressed, b.control_pressed, t),
        .danger = mix(a.danger, b.danger, t),
        .danger_surface = mix(a.danger_surface, b.danger_surface, t),
        .danger_border = mix(a.danger_border, b.danger_border, t),
        .scrollbar = mix(a.scrollbar, b.scrollbar, t),
        .scrollbar_hover = mix(a.scrollbar_hover, b.scrollbar_hover, t),
        .icon_folder_back = mix(a.icon_folder_back, b.icon_folder_back, t),
        .icon_folder_front = mix(a.icon_folder_front, b.icon_folder_front, t),
        .icon_page = mix(a.icon_page, b.icon_page, t),
        .icon_page_border = mix(a.icon_page_border, b.icon_page_border, t),
        .tint_image = mix(a.tint_image, b.tint_image, t),
        .tint_audio = mix(a.tint_audio, b.tint_audio, t),
        .tint_video = mix(a.tint_video, b.tint_video, t),
        .tint_archive = mix(a.tint_archive, b.tint_archive, t),
        .tint_code = mix(a.tint_code, b.tint_code, t),
        .tint_document = mix(a.tint_document, b.tint_document, t),
        .tint_pdf = mix(a.tint_pdf, b.tint_pdf, t),
        .tint_sheet = mix(a.tint_sheet, b.tint_sheet, t),
        .tint_slides = mix(a.tint_slides, b.tint_slides, t),
        .tint_program = mix(a.tint_program, b.tint_program, t),
        .on_accent = mix(a.on_accent, b.on_accent, t),
        .warning = mix(a.warning, b.warning, t),
        .match = mix(a.match, b.match, t),
        .chip = mix(a.chip, b.chip, t),
        .chip_text = mix(a.chip_text, b.chip_text, t),
        .control_border = mix(a.control_border, b.control_border, t),
    };
}

Palette dimmed(const Palette& p, Theme theme) noexcept {
    Palette d = p;
    if (theme == Theme::Light) {
        // White cards would barely move toward #F5F6F8 (about 1.03:1): go a step below it.
        d.surface = mix(p.background, rgb(0xDCE0E7), 0.30F);
        d.surface_raised = mix(p.surface_raised, p.background, 0.6F);
        d.text_primary = mix(p.text_primary, p.text_secondary, 0.40F);
        d.text_secondary = mix(p.text_secondary, p.text_muted, 0.25F);
    } else {
        d.surface = mix(p.background, rgb(0x08090C), 0.25F);
        d.surface_raised = mix(p.surface_raised, p.background, 0.5F);
        d.text_primary = mix(p.text_primary, p.text_secondary, 0.40F);
        d.text_secondary = mix(p.text_secondary, p.text_muted, 0.30F);
    }
    d.icon_folder_back = mix(p.icon_folder_back, p.background, 0.22F);
    d.icon_folder_front = mix(p.icon_folder_front, p.background, 0.22F);
    d.icon_page_border = mix(p.icon_page_border, p.background, 0.15F);
    d.scrollbar = p.scrollbar.with_alpha(p.scrollbar.a * 0.7F);
    return d;
}

Theme toggled(Theme theme) noexcept { return theme == Theme::Dark ? Theme::Light : Theme::Dark; }

} // namespace navigator::ui

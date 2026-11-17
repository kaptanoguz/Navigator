// src/ui/theme/theme.hpp
//
// Design tokens: palettes (Dark/Light), spacing scale, type scale, radii, motion.
// Pure data and math, no SDL; safe to use from tests without a window.
// All sizes are in density-independent units (dp); multiply by the display scale to get pixels.
// Thread-safety: immutable data and pure functions; callable from any thread.
#pragma once

#include <cstdint>

#include <navigator/ui.hpp>

namespace navigator::ui {

// sRGB-encoded color, components in [0, 1], straight (non-premultiplied) alpha.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;

    [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
        return Color{r, g, b, alpha};
    }
};

// 0xRRGGBB to an opaque Color.
[[nodiscard]] constexpr Color rgb(unsigned int hex) noexcept {
    constexpr float inv = 1.0F / 255.0F;
    return Color{static_cast<float>((hex >> 16U) & 0xFFU) * inv,
                 static_cast<float>((hex >> 8U) & 0xFFU) * inv,
                 static_cast<float>(hex & 0xFFU) * inv, 1.0F};
}

// sRGB transfer functions (IEC 61966-2-1).
[[nodiscard]] float srgb_to_linear(float c) noexcept;
[[nodiscard]] float linear_to_srgb(float c) noexcept;

// WCAG 2.x relative luminance of an opaque color, and contrast ratio (1..21).
[[nodiscard]] float relative_luminance(const Color& c) noexcept;
[[nodiscard]] float contrast_ratio(const Color& a, const Color& b) noexcept;

// Component-wise interpolation in linear light (used for the theme crossfade).
[[nodiscard]] Color mix(const Color& a, const Color& b, float t) noexcept;

struct Palette {
    Color background;     // window background base
    Color background_alt; // soft glow behind the content (background gradient top)
    Color surface;        // panels
    Color surface_raised; // floating panels, keycaps
    Color text_primary;
    Color text_secondary;
    Color text_muted;
    Color accent;
    Color border;        // hairlines (panels); deliberately quiet, < 2:1 on background
    Color shadow;        // drop shadow tint (alpha carries strength)
    Color keycap_edge;   // bottom edge of keycap chips (depth cue)
    Color keycap_border; // keycap outline; >= 2:1 on background so chips survive dim panels
    Color keycap_text;   // keycap legends: one step quieter than text_secondary (action labels)

    // Phase 2 browser. Overlays (row_*, selection*, control_*) are translucent and composited
    // over `surface` (list) or `background` (chrome); their alpha carries the strength.
    Color row_alt;            // alternating row stripe
    Color row_hover;          // pointer over a row
    Color selection;          // selected rows, window focused
    Color selection_inactive; // selected rows, window not focused
    Color selection_marker;   // 3dp bar at the left of selected rows (not colour alone)
    Color focus_ring;         // cursor row outline
    Color control_hover;      // toolbar buttons and breadcrumb segments
    Color control_pressed;
    Color danger;         // notice icon and accents; >= 4.5:1 on surface as text
    Color danger_surface; // notice banner fill (opaque)
    Color danger_border;
    Color scrollbar; // overlay scrollbar thumb
    Color scrollbar_hover;
    Color icon_folder_back;  // folder glyph, back plate + tab
    Color icon_folder_front; // folder glyph, front plate
    Color icon_page;         // file glyph paper
    Color icon_page_border;
    Color tint_image; // file-type badges on the file glyph
    Color tint_audio;
    Color tint_video;
    Color tint_archive;
    Color tint_code;
    Color tint_document;
    Color tint_pdf;
    Color tint_sheet;
    Color tint_slides;
    Color tint_program;

    // Phase 3.
    Color on_accent; // text and glyphs on an accent fill (primary buttons); >= 4.5:1
};

[[nodiscard]] const Palette& palette(Theme theme) noexcept;
[[nodiscard]] Palette mix(const Palette& a, const Palette& b, float t) noexcept;
[[nodiscard]] Theme toggled(Theme theme) noexcept;

// Phase 3: the palette of an inactive pane, per theme. The list surface sinks to (dark) or just
// below (light) the window background, so active and inactive surfaces differ by a visible
// lightness step in both themes; text and glyphs step toward the next tier. Every text tier
// keeps WCAG AA (>= 4.5:1) on the dimmed surfaces (tests/ui/theme_test.cpp). Not the only
// active-pane cue: the active pane also gets an accent marker (never colour alone).
[[nodiscard]] Palette dimmed(const Palette& p, Theme theme) noexcept;

// Spacing scale (dp), 4-based.
namespace space {
inline constexpr float xxs = 2.0F;
inline constexpr float xs = 4.0F;
inline constexpr float sm = 8.0F;
inline constexpr float md = 12.0F;
inline constexpr float lg = 16.0F;
inline constexpr float xl = 24.0F;
inline constexpr float xxl = 32.0F;
inline constexpr float xxxl = 48.0F;
} // namespace space

// Type scale (dp, em-relative tracking).
enum class FontWeight {
    Regular,
    Medium,
    SemiBold,
};

struct TextStyle {
    float size = 14.0F; // dp
    FontWeight weight = FontWeight::Regular;
    float tracking = 0.0F; // em; added after every glyph
    bool tabular = false;  // OpenType tnum
};

namespace type {
inline constexpr TextStyle caption{12.0F, FontWeight::Regular, 0.01F, false};
inline constexpr TextStyle keycap{12.0F, FontWeight::Medium, 0.01F, false};
inline constexpr TextStyle body{14.0F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle numeric{13.0F, FontWeight::Medium, 0.0F, true};
inline constexpr TextStyle tagline{17.0F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle display{60.0F, FontWeight::SemiBold, -0.028F, false};
// Phase 2 browser.
inline constexpr TextStyle row{13.0F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle row_numeric{13.0F, FontWeight::Regular, 0.0F, true};
inline constexpr TextStyle column_header{12.0F, FontWeight::Medium, 0.01F, false};
inline constexpr TextStyle crumb{13.0F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle crumb_current{13.0F, FontWeight::SemiBold, 0.0F, false};
inline constexpr TextStyle crumb_quiet{13.0F, FontWeight::Medium, 0.0F, false}; // inactive pane
inline constexpr TextStyle status{12.0F, FontWeight::Regular, 0.01F, true};
inline constexpr TextStyle notice_title{13.0F, FontWeight::Medium, 0.0F, false};
inline constexpr TextStyle empty_title{15.0F, FontWeight::Medium, 0.0F, false};
// Phase 3: tabs and popups.
inline constexpr TextStyle tab{12.5F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle tab_active{12.5F, FontWeight::Medium, 0.0F, false};
inline constexpr TextStyle popup_title{14.0F, FontWeight::SemiBold, 0.0F, false};
inline constexpr TextStyle popup_field{14.0F, FontWeight::Regular, 0.0F, false};
inline constexpr TextStyle section{11.5F, FontWeight::Medium, 0.02F, false};
inline constexpr TextStyle button{13.0F, FontWeight::Medium, 0.0F, false};
} // namespace type

namespace radius {
inline constexpr float sm = 5.0F;
inline constexpr float md = 8.0F;
inline constexpr float lg = 12.0F;
inline constexpr float pill = 999.0F;
} // namespace radius

namespace motion {
inline constexpr double theme_crossfade_ms = 180.0;
inline constexpr double scroll_settle_ms = 90.0; // smooth-scroll time constant (exponential)
inline constexpr double pane_dim_ms = 140.0;     // inactive-pane dimming fade
} // namespace motion

// Browser metrics (dp) that do not depend on the density.
namespace metrics {
inline constexpr float row_inset = 6.0F; // row highlight inset from the panel edge
inline constexpr float icon_size = 16.0F;
inline constexpr float scrollbar_width = 4.0F;
inline constexpr float scrollbar_width_hover = 8.0F;
} // namespace metrics

// List density. Compact (the default) suits a dense, keyboard-first file manager; Comfortable is
// the roomier Phase 2 layout.
enum class Density : std::uint8_t {
    Compact,
    Comfortable,
};

// Density-dependent browser metrics (dp).
struct DensityMetrics {
    float top_bar_height;
    float tool_button;
    float status_bar_height;
    float panel_margin;
    float header_height;
    float row_height;
    float notice_height;
    float tab_bar_height; // Phase 3: a pane's tab strip
};

[[nodiscard]] constexpr DensityMetrics density_metrics(Density d) noexcept {
    if (d == Density::Comfortable) {
        return DensityMetrics{52.0F, 32.0F, 30.0F, 10.0F, 30.0F, 28.0F, 40.0F, 38.0F};
    }
    return DensityMetrics{44.0F, 30.0F, 26.0F, 8.0F, 26.0F, 24.0F, 36.0F, 34.0F};
}

} // namespace navigator::ui

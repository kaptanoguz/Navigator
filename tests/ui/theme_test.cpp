// Theme tokens: WCAG contrast of text tokens against every background they sit on.
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <navigator/ui.hpp>

#include "shell/background.hpp"
#include "theme/theme.hpp"

using navigator::Theme;
using namespace navigator::ui;
using Catch::Matchers::WithinAbs;

TEST_CASE("Contrast ratio matches WCAG reference values", "[theme]") {
    CHECK_THAT(contrast_ratio(rgb(0x000000), rgb(0xFFFFFF)), WithinAbs(21.0, 0.01));
    CHECK_THAT(contrast_ratio(rgb(0xFFFFFF), rgb(0xFFFFFF)), WithinAbs(1.0, 0.001));
    // #767676 on white is the classic 4.54:1 AA boundary.
    CHECK_THAT(contrast_ratio(rgb(0x767676), rgb(0xFFFFFF)), WithinAbs(4.54, 0.01));
}

TEST_CASE("Every text token meets its contrast target on every surface", "[theme]") {
    struct Target {
        const char* name;
        Color Palette::*token;
        float min;
    };
    constexpr std::array targets{
        Target{"text_primary", &Palette::text_primary, 7.0F},     // AAA
        Target{"text_secondary", &Palette::text_secondary, 4.5F}, // AA body text
        Target{"text_muted", &Palette::text_muted, 4.5F},         // AA body text (P2-1)
        Target{"keycap_text", &Palette::keycap_text, 4.5F},
        Target{"accent", &Palette::accent, 4.5F}, // accent used as text
    };
    struct Surface {
        const char* name;
        Color Palette::*token;
    };
    constexpr std::array surfaces{
        Surface{"background", &Palette::background},
        Surface{"background_alt", &Palette::background_alt},
        Surface{"surface", &Palette::surface},
        Surface{"surface_raised", &Palette::surface_raised},
    };
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        for (const Target& text : targets) {
            for (const Surface& bg : surfaces) {
                const float ratio = contrast_ratio(p.*text.token, p.*bg.token);
                INFO((t == Theme::Dark ? "dark " : "light ")
                     << text.name << " on " << bg.name << " = " << ratio);
                CHECK(ratio >= text.min);
            }
        }
    }
}

TEST_CASE("Text tiers keep their hierarchy", "[theme]") {
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        INFO((t == Theme::Dark ? "dark" : "light"));
        for (const Color& bg : {p.background, p.surface_raised}) {
            const float primary = contrast_ratio(p.text_primary, bg);
            const float secondary = contrast_ratio(p.text_secondary, bg);
            const float legend = contrast_ratio(p.keycap_text, bg);
            const float muted = contrast_ratio(p.text_muted, bg);
            CHECK(primary > secondary + 3.0F);
            // Legends read one step quieter than action labels, still above muted.
            CHECK(secondary > legend + 0.4F);
            CHECK(legend > muted + 0.4F);
            CHECK(secondary > muted + 1.2F); // muted stays visibly lighter than secondary
        }
    }
}

TEST_CASE("Keycaps separate from the background without getting heavy", "[theme]") {
    const Palette& d = palette(Theme::Dark);
    CHECK(contrast_ratio(d.keycap_border, d.background) >= 2.0F);
    CHECK(contrast_ratio(d.keycap_border, d.background) < 3.0F);
    CHECK(contrast_ratio(d.keycap_border, d.surface_raised) >= 1.6F);
    const Palette& l = palette(Theme::Light);
    CHECK(contrast_ratio(l.keycap_border, l.background) >= 1.6F);
    CHECK(contrast_ratio(l.keycap_border, l.background) < 3.0F);
}

TEST_CASE("Neutrals step up from background to raised surface", "[theme]") {
    const Palette& d = palette(Theme::Dark);
    CHECK(relative_luminance(d.background) < relative_luminance(d.surface));
    CHECK(relative_luminance(d.surface) < relative_luminance(d.surface_raised));
    CHECK(contrast_ratio(d.border, d.background) < 2.0F); // hairlines stay quiet
    const Palette& l = palette(Theme::Light);
    CHECK(contrast_ratio(l.border, l.background) < 2.0F);
    for (const Palette* p : {&d, &l}) {
        CHECK(contrast_ratio(p->accent, p->background) >= 3.0F); // non-text UI component
    }
}

TEST_CASE("Palette mix interpolates endpoints exactly", "[theme]") {
    const Palette& a = palette(Theme::Dark);
    const Palette& b = palette(Theme::Light);
    CHECK_THAT(mix(a, b, 0.0F).background.r, WithinAbs(a.background.r, 1e-4));
    CHECK_THAT(mix(a, b, 1.0F).text_primary.g, WithinAbs(b.text_primary.g, 1e-4));
    CHECK(toggled(Theme::Dark) == Theme::Light);
    CHECK(toggled(Theme::Light) == Theme::Dark);
}

TEST_CASE("Backdrop stays within one step of its palette and is opaque", "[theme]") {
    const Palette& p = palette(Theme::Dark);
    const auto px = render_backdrop(p, 64, 40);
    REQUIRE(px.size() == std::size_t{64} * 40U * 4U);
    const auto lo = static_cast<int>(p.background.r * 255.0F) - 2;
    const auto hi = static_cast<int>(p.background_alt.r * 255.0F) + 2;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        CHECK(px[i + 3] == 255);
        const int r = px[i];
        if (r < lo || r > hi) {
            FAIL("backdrop red channel out of range: " << r);
        }
    }
}

TEST_CASE("Browser tokens keep text readable on banners and selection", "[theme]") {
    const auto over = [](const Color& base, const Color& overlay) {
        const float a = overlay.a;
        return Color{(base.r * (1.0F - a)) + (overlay.r * a),
                     (base.g * (1.0F - a)) + (overlay.g * a),
                     (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
    };
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        INFO((t == Theme::Dark ? "dark" : "light"));
        // Notice banner: title, detail and the alert glyph.
        CHECK(contrast_ratio(p.text_primary, p.danger_surface) >= 7.0F);
        CHECK(contrast_ratio(p.text_secondary, p.danger_surface) >= 4.5F);
        CHECK(contrast_ratio(p.danger, p.danger_surface) >= 3.0F);
        CHECK(contrast_ratio(p.danger, p.surface) >= 4.5F);
        // Rows: secondary columns on stripes, hover and selection stay AA.
        for (const Color& overlay : {p.row_alt, p.row_hover, p.selection, p.selection_inactive}) {
            const Color bg = over(p.surface, overlay);
            CHECK(contrast_ratio(p.text_primary, bg) >= 7.0F);
            CHECK(contrast_ratio(p.text_secondary, bg) >= 4.5F);
        }
        // Alternating rows are perceptible but stay quiet.
        const float stripe = contrast_ratio(over(p.surface, p.row_alt), p.surface);
        CHECK(stripe >= 1.05F);
        CHECK(stripe <= 1.15F);
        // The selection marker and focus ring are visible UI components (>= 3:1).
        CHECK(contrast_ratio(p.selection_marker, over(p.surface, p.selection)) >= 3.0F);
        CHECK(contrast_ratio(over(p.surface, p.focus_ring), p.surface) >= 3.0F);
    }
}

TEST_CASE("Inactive-pane dimming keeps every text tier at WCAG AA", "[theme]") {
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        const Palette d = dimmed(p, t);
        for (const Color& bg : {d.surface, d.background, d.surface_raised}) {
            INFO((t == Theme::Dark ? "dark" : "light"));
            CHECK(contrast_ratio(d.text_primary, bg) >= 4.5F);
            CHECK(contrast_ratio(d.text_secondary, bg) >= 4.5F);
            CHECK(contrast_ratio(d.text_muted, bg) >= 4.5F);
        }
        // It is a visible step down from the active pane, and keeps the hierarchy.
        CHECK(contrast_ratio(d.text_primary, d.surface) <
              contrast_ratio(p.text_primary, p.surface));
        CHECK(contrast_ratio(d.text_primary, d.surface) >
              contrast_ratio(d.text_secondary, d.surface));
        // Text on an accent fill (primary buttons, drop labels).
        CHECK(contrast_ratio(p.on_accent, p.accent) >= 4.5F);
    }
}

TEST_CASE("Inactive panes differ visibly from the active one in both themes", "[theme]") {
    // CIE L* (0..100) from WCAG relative luminance.
    const auto lightness = [](const Color& c) {
        const double y = relative_luminance(c);
        return y > 0.008856 ? (116.0 * std::cbrt(y)) - 16.0 : 903.3 * y;
    };
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        const Palette d = dimmed(p, t);
        INFO((t == Theme::Dark ? "dark" : "light"));
        // Design review B1: the light list surface moved only ~1.03:1 before.
        CHECK(std::abs(lightness(p.surface) - lightness(d.surface)) >= 3.5);
        // The inactive list sits at or below the window background, never above it.
        CHECK(lightness(d.surface) <= lightness(p.background) + 0.01);
        // Text tiers also step down.
        CHECK(std::abs(lightness(p.text_primary) - lightness(d.text_primary)) >= 3.0);
    }
}

TEST_CASE("Phase 5 match highlight and filter chip stay AA everywhere they are drawn", "[theme]") {
    const auto over = [](const Color& base, const Color& overlay) {
        const float a = overlay.a;
        return Color{(base.r * (1.0F - a)) + (overlay.r * a),
                     (base.g * (1.0F - a)) + (overlay.g * a),
                     (base.b * (1.0F - a)) + (overlay.b * a), 1.0F};
    };
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        const Palette d = dimmed(p, t);
        INFO((t == Theme::Dark ? "dark" : "light"));
        // Matched characters on the list surface, its stripes, hover and both selection fills
        // (the selection fill already uses the accent: design review Phase 4 notes).
        for (const Palette* pal : {&p, &d}) {
            CHECK(contrast_ratio(pal->match, pal->surface) >= 4.5F);
            for (const Color& o :
                 {pal->row_alt, pal->row_hover, pal->selection, pal->selection_inactive}) {
                CHECK(contrast_ratio(pal->match, over(pal->surface, o)) >= 4.5F);
            }
        }
        // The match reads differently from plain names (it is not just primary text).
        CHECK(contrast_ratio(p.match, p.text_primary) >= 1.3F);
        // The count chip on the chrome background (active and dimmed panes).
        CHECK(contrast_ratio(p.chip_text, over(p.background, p.chip)) >= 4.5F);
        CHECK(contrast_ratio(d.chip_text, over(d.background, d.chip)) >= 4.5F);
        CHECK(contrast_ratio(p.chip_text, over(p.surface, p.chip)) >= 4.5F);
    }
}

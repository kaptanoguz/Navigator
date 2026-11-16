// Theme tokens: WCAG contrast of text tokens against every background they sit on.
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

TEST_CASE("Text tokens meet contrast targets in both themes", "[theme]") {
    for (const Theme t : {Theme::Dark, Theme::Light}) {
        const Palette& p = palette(t);
        INFO((t == Theme::Dark ? "dark" : "light"));
        for (const Color& bg : {p.background, p.background_alt, p.surface, p.surface_raised}) {
            CHECK(contrast_ratio(p.text_primary, bg) >= 7.0F);   // AAA
            CHECK(contrast_ratio(p.text_secondary, bg) >= 4.5F); // AA body text
            CHECK(contrast_ratio(p.text_muted, bg) >= 3.0F);     // AA large / UI labels
        }
        CHECK(contrast_ratio(p.accent, p.background) >= 3.0F); // non-text UI component
    }
}

TEST_CASE("Neutrals step up from background to raised surface", "[theme]") {
    const Palette& d = palette(Theme::Dark);
    CHECK(relative_luminance(d.background) < relative_luminance(d.surface));
    CHECK(relative_luminance(d.surface) < relative_luminance(d.surface_raised));
    CHECK(contrast_ratio(d.border, d.background) < 2.0F); // hairlines stay quiet
    const Palette& l = palette(Theme::Light);
    CHECK(contrast_ratio(l.border, l.background) < 2.0F);
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

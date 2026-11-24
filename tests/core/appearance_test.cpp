// tests/core/appearance_test.cpp — Phase 9B appearance settings and custom JSON theme tests.
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <navigator/appearance.hpp>

#include "temp_dir.hpp"

using namespace navigator;
using namespace navigator::appearance;

TEST_CASE("Hex color parsing and formatting", "[core][appearance]") {
    const auto c6 = parse_hex_color("#8EA6FF");
    REQUIRE(c6.has_value());
    if (c6.has_value()) {
        CHECK(c6->r == 0x8E);
        CHECK(c6->g == 0xA6);
        CHECK(c6->b == 0xFF);
        CHECK(c6->a == 0xFF);
        CHECK(format_hex_color(*c6) == "#8EA6FF");
    }

    const auto c3 = parse_hex_color("#abc");
    REQUIRE(c3.has_value());
    if (c3.has_value()) {
        CHECK(c3->r == 0xAA);
        CHECK(c3->g == 0xBB);
        CHECK(c3->b == 0xCC);
        CHECK(c3->a == 0xFF);
    }

    const auto c8 = parse_hex_color("#10203080");
    REQUIRE(c8.has_value());
    if (c8.has_value()) {
        CHECK(c8->a == 0x80);
        CHECK(format_hex_color(*c8) == "#10203080");
    }

    CHECK_FALSE(parse_hex_color("8EA6FF").has_value());
    CHECK_FALSE(parse_hex_color("#12345").has_value());
    CHECK_FALSE(parse_hex_color("#GGGGGG").has_value());
}

TEST_CASE("AppearanceSettings JSON round-trip, lenient load, and atomic file I/O",
          "[core][appearance]") {
    AppearanceSettings s;
    s.theme_id = "nord";
    s.density = UiDensity::Comfortable;
    s.font_scale_percent = 120;
    s.spacing_scale_percent = 90;
    s.corner_radius_dp = 10;
    s.animations = false;
    s.alternating_rows = false;

    const auto json = to_json(s);
    REQUIRE(json.has_value());
    const auto parsed = appearance_from_json(*json);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == s);

    // Lenient load ignores unknown keys and clamps out-of-range values.
    const auto lenient = appearance_from_json(
        R"({"version": 99, "theme": "light", "font_scale": 500, "spacing_scale": 10, "corner_radius": -4, "extra": true})");
    REQUIRE(lenient.has_value());
    CHECK(lenient->theme_id == "light");
    CHECK(lenient->font_scale_percent == kMaxFontScalePercent);
    CHECK(lenient->spacing_scale_percent == kMinSpacingScalePercent);
    CHECK(lenient->corner_radius_dp == kMinCornerRadiusDp);
    CHECK(lenient->animations == true);
    CHECK(lenient->alternating_rows == true);

    // Atomic file save & load.
    navigator::test::TempDir tmp;
    const Path file = settings_file(tmp.path());
    REQUIRE(save_appearance_settings(file, s).has_value());
    const auto reloaded = load_appearance_settings(file);
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == s);
}

TEST_CASE("Custom ThemeScheme JSON load, fallback, and directory scan", "[core][appearance]") {
    navigator::test::TempDir tmp;
    const Path dir = themes_dir(tmp.path());
    std::filesystem::create_directories(dir.native());

    // Valid custom theme.
    ThemeScheme nord;
    nord.id = "nord";
    nord.name = "Nord";
    nord.base = Theme::Dark;
    nord.colors.background = Rgba8{0x2E, 0x34, 0x40, 0xFF};
    nord.colors.accent = Rgba8{0x88, 0xC0, 0xD0, 0xFF};
    const auto nord_json = to_json(nord);
    REQUIRE(nord_json.has_value());
    {
        std::ofstream out((dir / "nord.json").native());
        out << *nord_json;
    }
    // Theme without explicit id derives id from stem; invalid color field is ignored leniently.
    {
        std::ofstream out((dir / "paper.json").native());
        out << R"({"name": "Warm Paper", "base": "light", "colors": {"background": "#FAF6EE", "accent": "not-a-color"}})";
    }
    // Corrupt JSON file is skipped without failing list_custom_themes.
    {
        std::ofstream out((dir / "broken.json").native());
        out << "{ not valid json";
    }

    const auto list = list_custom_themes(dir);
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == "nord");
    CHECK(list[0].name == "Nord");
    CHECK(list[0].base == Theme::Dark);
    CHECK(list[0].colors.accent == Rgba8{0x88, 0xC0, 0xD0, 0xFF});

    CHECK(list[1].id == "paper");
    CHECK(list[1].name == "Warm Paper");
    CHECK(list[1].base == Theme::Light);
    CHECK(list[1].colors.background == Rgba8{0xFA, 0xF6, 0xEE, 0xFF});
    CHECK_FALSE(list[1].colors.accent.has_value());
}

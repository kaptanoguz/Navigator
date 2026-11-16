// Text: font discovery and HarfBuzz shaping of Turkish text with the UI font.
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/types.hpp>

#include "text/font.hpp"

using namespace navigator::ui;
using namespace navigator::ui::text;

TEST_CASE("UI font shapes Turkish text without .notdef glyphs", "[text]") {
    auto file = find_font(preferred_families(), FontWeight::Regular);
    if (!file) {
        SKIP("no UI font available: " << file.error().message);
    }
    INFO("font: " << file->family << " (" << file->path << ")");
    auto lib = FontLibrary::create();
    REQUIRE(lib);
    auto font = Font::open(*lib, *file, 16.0F);
    REQUIRE(font);

    constexpr std::string_view sample = "Navigator ğüşıöçİ ĞÜŞÖÇ";
    const ShapedRun run = (*font)->shape(sample, ShapeOptions{.language = "tr"});
    REQUIRE_FALSE(run.glyphs.empty());
    CHECK(run.advance > 0.0F);
    for (const ShapedGlyph& g : run.glyphs) {
        CHECK(g.glyph != 0U);
    }
    // Pen positions increase monotonically for LTR text.
    for (std::size_t i = 1; i < run.glyphs.size(); ++i) {
        CHECK(run.glyphs[i].x >= run.glyphs[i - 1].x);
    }
    const FontMetrics& m = (*font)->metrics();
    CHECK(m.ascender > 0.0F);
    CHECK(m.cap_height > 0.0F);
    CHECK(m.cap_height < m.ascender);
}

TEST_CASE("Glyphs rasterize to non-empty coverage", "[text]") {
    auto file = find_font(preferred_families(), FontWeight::Regular);
    if (!file) {
        SKIP("no UI font available");
    }
    auto lib = FontLibrary::create();
    REQUIRE(lib);
    auto font = Font::open(*lib, *file, 24.0F);
    REQUIRE(font);
    const ShapedRun run = (*font)->shape("İ");
    REQUIRE(run.glyphs.size() == 1U);
    auto bitmap = (*font)->rasterize(run.glyphs[0].glyph, 0.25F);
    REQUIRE(bitmap);
    CHECK(bitmap->width > 0);
    CHECK(bitmap->height > 0);
    CHECK(bitmap->top > 0);
    bool any = false;
    for (const auto c : bitmap->coverage) {
        any = any || c > 0;
    }
    CHECK(any);
}

TEST_CASE("Tracking widens a run; unknown family falls through", "[text]") {
    auto file = find_font(preferred_families(), FontWeight::Regular);
    if (!file) {
        SKIP("no UI font available");
    }
    auto lib = FontLibrary::create();
    REQUIRE(lib);
    auto font = Font::open(*lib, *file, 16.0F);
    REQUIRE(font);
    const float plain = (*font)->shape("Navigator").advance;
    const float tracked = (*font)->shape("Navigator", ShapeOptions{.tracking_em = 0.1F}).advance;
    CHECK(tracked > plain);

    auto none = find_font({"Definitely Not A Real Font Family 1234"}, FontWeight::Regular);
    CHECK_FALSE(none);
    if (!none) {
        CHECK(none.error().code == navigator::ErrorCode::NotFound);
    }
}

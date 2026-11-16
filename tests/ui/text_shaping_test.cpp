// Text: font discovery, HarfBuzz shaping, glyph fallback and the shape cache.
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/types.hpp>

#include "text/font.hpp"
#include "text/font_cache.hpp"

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

namespace {

// True when fontconfig lists at least one font for `lang` (like `fc-list :lang=ja`).
bool system_has_lang(const char* lang) {
    const std::string cmd = std::string("fc-list :lang=") + lang + " family 2>/dev/null";
    // NOLINTNEXTLINE(cert-env33-c): fixed command line, test-only probe.
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) {
        return false;
    }
    std::array<char, 256> buf{};
    const bool any = std::fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr;
    pclose(p);
    return any;
}

} // namespace

TEST_CASE("UTF-8 decoding handles multibyte and invalid input", "[text]") {
    const std::string_view s = "aş日\xF0\x9F\x98\x80\xFF\xE6";
    std::size_t pos = 0;
    CHECK(decode_utf8(s, pos) == U'a');
    CHECK(decode_utf8(s, pos) == U'ş');
    CHECK(decode_utf8(s, pos) == U'日');
    CHECK(decode_utf8(s, pos) == U'\U0001F600');
    CHECK(decode_utf8(s, pos) == U'�'); // stray 0xFF
    CHECK(decode_utf8(s, pos) == U'�'); // truncated sequence
    CHECK(pos == s.size());
}

TEST_CASE("Mixed-script text falls back to a font that has the glyphs", "[text][fallback]") {
    if (!system_has_lang("ja")) {
        SKIP("no Japanese font installed (fc-list :lang=ja is empty)");
    }
    auto cache = FontCache::create();
    if (!cache) {
        SKIP("no UI font available: " << cache.error().message);
    }
    constexpr std::string_view sample = "Navigator 日本語 ğüş";
    const auto layout = (*cache)->shape(type::body, 1.0F, sample, "tr");
    REQUIRE(layout != nullptr);
    REQUIRE(layout->runs.size() >= 2U); // UI face + a CJK face (+ UI face again)
    bool used_fallback = false;
    std::size_t glyphs = 0;
    for (const auto& run : layout->runs) {
        REQUIRE(run.font != nullptr);
        INFO("run font: " << run.font->file().family << " (" << run.font->file().path << ")");
        used_fallback = used_fallback || run.font->file().path != (*cache)->regular_file().path;
        for (const ShapedGlyph& g : run.shaped.glyphs) {
            CHECK(g.glyph != 0U); // no .notdef
            ++glyphs;
        }
    }
    CHECK(used_fallback);
    CHECK(glyphs >= 15U);
    CHECK(layout->advance > 0.0F);
    // Runs are laid out left to right without overlap.
    for (std::size_t i = 1; i < layout->runs.size(); ++i) {
        CHECK(layout->runs[i].x >= layout->runs[i - 1].x + layout->runs[i - 1].shaped.advance);
    }
}

TEST_CASE("Shaped text is cached per style, size and string", "[text][cache]") {
    auto cache = FontCache::create();
    if (!cache) {
        SKIP("no UI font available: " << cache.error().message);
    }
    FontCache& fc = **cache;
    const auto a = fc.shape(type::body, 1.0F, "Navigator", "en");
    const auto b = fc.shape(type::body, 1.0F, "Navigator", "en");
    CHECK(a.get() == b.get()); // second call is a cache hit, no reshaping
    CHECK(fc.shape_cache().hits() == 1U);
    CHECK(fc.shape(type::body, 2.0F, "Navigator", "en").get() != a.get());    // size
    CHECK(fc.shape(type::caption, 1.0F, "Navigator", "en").get() != a.get()); // style
    CHECK(fc.shape(type::body, 1.0F, "Navigator", "tr").get() != a.get());    // language
    CHECK(fc.shape(type::body, 1.0F, "Navigatör", "en").get() != a.get());    // text
    CHECK(fc.shape_cache().size() == 5U);
    fc.clear();
    CHECK(fc.shape_cache().size() == 0U);
    CHECK(a->advance > 0.0F); // a held layout outlives clear() for its holder (fonts do not)
}

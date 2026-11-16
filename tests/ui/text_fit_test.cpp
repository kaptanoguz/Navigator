// Ellipsis fitting: end and middle elision (keeping the extension), cluster-safe cuts.
// Uses SDL's software renderer on an offscreen surface (no window, no video driver).
#include <memory>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include "render/renderer.hpp"
#include "shell/painter.hpp"
#include "text/font_cache.hpp"
#include "text/text_renderer.hpp"
#include "widgets/text_fit.hpp"

using namespace navigator::ui;

namespace {

struct Rig {
    SDL_Surface* surface = nullptr;
    SDL_Renderer* sdl = nullptr;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<text::FontCache> fonts;
    std::unique_ptr<text::GlyphAtlas> atlas;
    std::unique_ptr<text::TextRenderer> text;

    Rig() {
        surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        sdl = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        auto f = text::FontCache::create();
        if (sdl == nullptr || !f) {
            return;
        }
        fonts = std::move(*f);
        renderer = std::make_unique<Renderer>(sdl);
        auto a = text::GlyphAtlas::create(sdl, 256);
        if (a) {
            atlas = std::move(*a);
            text = std::make_unique<text::TextRenderer>(*renderer, *atlas);
        }
    }
    ~Rig() {
        text.reset();
        atlas.reset();
        if (sdl != nullptr) {
            SDL_DestroyRenderer(sdl);
        }
        if (surface != nullptr) {
            SDL_DestroySurface(surface);
        }
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    [[nodiscard]] bool ready() const { return text != nullptr; }
};

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

} // namespace

TEST_CASE("Text that fits is unchanged; long text gets an ellipsis", "[text][fit]") {
    Rig rig;
    if (!rig.ready()) {
        SKIP("no software renderer or UI font");
    }
    const Painter p(*rig.renderer, *rig.text, *rig.fonts, 1.0F, "en");
    CHECK(TextFitter::elide(p, type::row, "short.txt", 400.0F, Elide::Middle) == "short.txt");

    const std::string long_name =
        "Quarterly financial report for the board meeting final version.pdf";
    const float full = p.shape(type::row, long_name).width();
    const float max = full * 0.5F;

    const std::string end = TextFitter::elide(p, type::row, long_name, max, Elide::End);
    CHECK(ends_with(end, kEllipsis));
    CHECK(end.starts_with("Quarterly"));
    CHECK(p.shape(type::row, end).width() <= max);

    const std::string mid = TextFitter::elide(p, type::row, long_name, max, Elide::Middle);
    INFO(mid);
    CHECK(mid.find(kEllipsis) != std::string::npos);
    CHECK(ends_with(mid, ".pdf")); // the extension survives
    CHECK(mid.starts_with("Quarterly"));
    CHECK(p.shape(type::row, mid).width() <= max);

    TextFitter fitter;
    const ShapedText shaped = fitter.fit(p, type::row, long_name, max, Elide::Middle);
    CHECK(shaped.width() <= max);
    CHECK(shaped.width() > max * 0.6F); // uses the room it has
}

TEST_CASE("Elision never splits a combining sequence or a multibyte letter", "[text][fit]") {
    Rig rig;
    if (!rig.ready()) {
        SKIP("no software renderer or UI font");
    }
    const Painter p(*rig.renderer, *rig.text, *rig.fonts, 1.0F, "tr");
    // NFD e + U+0301 repeated, Turkish letters, then an extension.
    std::string name;
    for (int i = 0; i < 12; ++i) {
        name += "e\xCC\x81ğış";
    }
    name += ".txt";
    const float max = p.shape(type::row, name).width() * 0.4F;
    for (const Elide mode : {Elide::End, Elide::Middle}) {
        const std::string out = TextFitter::elide(p, type::row, name, max, mode);
        INFO(out);
        // Never ends a head with a bare 'e' that lost its accent: cuts are cluster starts, so
        // the byte before the ellipsis is not the start of a 2-byte sequence.
        const std::size_t cut = out.find(kEllipsis);
        REQUIRE(cut != std::string::npos);
        REQUIRE(cut > 0);
        const auto last = static_cast<unsigned char>(out[cut - 1]);
        CHECK(last != 'e');             // 'e' is always followed by its combining acute
        CHECK((last & 0xC0U) != 0xC0U); // not a dangling UTF-8 lead byte
        CHECK(p.shape(type::row, out).width() <= max);
    }
}

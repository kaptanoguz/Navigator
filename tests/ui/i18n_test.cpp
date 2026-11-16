// i18n: English default, Turkish lookup, fallbacks.
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

using navigator::Language;
using navigator::set_language;
using navigator::tr;

namespace {

// Restores English when a test ends, whatever it switched to.
struct LanguageGuard {
    LanguageGuard() = default;
    LanguageGuard(const LanguageGuard&) = delete;
    LanguageGuard& operator=(const LanguageGuard&) = delete;
    LanguageGuard(LanguageGuard&&) = delete;
    LanguageGuard& operator=(LanguageGuard&&) = delete;
    ~LanguageGuard() { set_language(Language::English); }
};

} // namespace

TEST_CASE("English is the default and covers every Phase 0 key", "[i18n]") {
    const LanguageGuard guard;
    set_language(Language::English);
    CHECK(navigator::language() == Language::English);
    for (std::string_view key :
         {"app.title", "app.tagline", "fps.label", "hint.quit", "hint.theme", "hint.fps",
          "cmd.app.quit", "cmd.view.toggle_fps_overlay", "cmd.view.toggle_theme"}) {
        INFO(key);
        const std::string_view value = tr(key);
        CHECK_FALSE(value.empty());
        CHECK(value != key);
    }
    CHECK(tr("hint.quit") == "Quit");
}

TEST_CASE("Turkish lookup returns Turkish strings", "[i18n]") {
    const LanguageGuard guard;
    set_language(Language::Turkish);
    CHECK(navigator::language() == Language::Turkish);
    CHECK(tr("hint.quit") == "Çıkış");
    CHECK(tr("hint.theme") == "Temayı değiştir");
    CHECK(tr("app.tagline") == "Linux için hızlı, klavye odaklı dosya yöneticisi");
    CHECK(tr("cmd.app.quit") == "Navigator'dan çık");
}

TEST_CASE("Unknown keys return the key itself", "[i18n]") {
    const LanguageGuard guard;
    for (const Language lang : {Language::English, Language::Turkish}) {
        set_language(lang);
        CHECK(tr("no.such.key") == "no.such.key");
        CHECK(tr("").empty());
    }
}

TEST_CASE("Untranslated-identical strings fall back sensibly", "[i18n]") {
    const LanguageGuard guard;
    set_language(Language::Turkish);
    CHECK(tr("app.title") == "Navigator");
    CHECK(tr("fps.label") == "FPS");
}

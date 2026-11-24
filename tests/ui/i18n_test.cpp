// i18n: English default, Turkish lookup, fallbacks.
#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

#include "i18n/catalog.hpp"

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
          "cmd.app.quit", "cmd.view.toggle_fps_overlay", "cmd.view.toggle_theme", "fps.frame_time",
          "fps.driver", "fps.renderer.software"}) {
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
    CHECK(tr("fps.frame_time") == "Kare süresi"); // "Kare" alone also means "square"
    CHECK(tr("fps.renderer.software") == "Yazılım");
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

// Every plural base the ui formats with tr_count() must have an ".other" entry in both languages,
// otherwise the raw key (e.g. "ops.chip.many.other") reaches the screen.
TEST_CASE("Every tr_count base resolves to real text in both languages", "[i18n]") {
    const LanguageGuard guard;
    for (std::string_view base : {"confirm.delete.title",
                                  "confirm.empty.body",
                                  "confirm.more",
                                  "confirm.stats",
                                  "confirm.untrashable.title",
                                  "conflict.apply_all_more",
                                  "conflict.waiting",
                                  "copy.copied",
                                  "empty.hidden_only",
                                  "index.items",
                                  "inspector.files",
                                  "inspector.folders",
                                  "inspector.hidden",
                                  "inspector.items",
                                  "inspector.lines",
                                  "inspector.more",
                                  "inspector.selected",
                                  "meta.channels",
                                  "meta.frames",
                                  "ops.chip.many",
                                  "quit.body",
                                  "size.bytes",
                                  "status.folders",
                                  "status.found",
                                  "status.hidden",
                                  "status.hidden_files",
                                  "status.index_matches",
                                  "status.items",
                                  "status.selected",
                                  "status.skipped",
                                  "status.types_hidden",
                                  "trash.items",
                                  "trash.orphans",
                                  "types.hidden_items"}) {
        for (const Language lang : {Language::English, Language::Turkish}) {
            set_language(lang);
            for (const std::uint64_t count : {std::uint64_t{1}, std::uint64_t{2}}) {
                const std::string text = navigator::ui::tr_count(base, count);
                INFO("base " << base << " count " << count);
                CHECK_FALSE(text.empty());
                CHECK(text.find(std::string(base)) == std::string::npos);
            }
        }
    }
}

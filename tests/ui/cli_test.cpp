// navigator command line and locale detection.
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

#include "cli.hpp"

using navigator::Language;
using navigator::Theme;
using navigator::app::detect_language;
using navigator::app::parse_cli;

TEST_CASE("Defaults come from AppConfig", "[cli]") {
    auto o = parse_cli({});
    REQUIRE(o);
    CHECK(o->config.max_frames == 0);
    CHECK(o->config.theme == Theme::Dark);
    CHECK_FALSE(o->config.show_fps); // overlay is opt-in (--fps); default launch idles
    CHECK(o->config.animations);
    CHECK(o->start_path.empty());
    CHECK(o->config.start_path.empty());
    CHECK(o->bench_scroll == 0);
    CHECK_FALSE(o->show_help);
    CHECK_FALSE(o->language.has_value());
}

TEST_CASE("Options parse", "[cli]") {
    auto o = parse_cli({"--frames", "60", "--theme", "light", "--lang", "tr", "--no-fps"});
    REQUIRE(o);
    CHECK(o->config.max_frames == 60);
    CHECK(o->config.theme == Theme::Light);
    CHECK(o->language == Language::Turkish);
    CHECK_FALSE(o->config.show_fps);
    CHECK(parse_cli({"--fps"})->config.show_fps);
    CHECK_FALSE(parse_cli({"--no-animations"})->config.animations);
    CHECK(parse_cli({"--help"})->show_help);
    CHECK(parse_cli({"-V"})->show_version);
}

TEST_CASE("Screenshot runs default to a few frames", "[cli]") {
    auto o = parse_cli({"--screenshot", "/tmp/x.bmp", "--scale", "2"});
    REQUIRE(o);
    CHECK(o->config.max_frames > 0);
    CHECK(o->scale == 2.0F);
}

TEST_CASE("Bad options are rejected", "[cli]") {
    CHECK_FALSE(parse_cli({"--frames"}));
    CHECK_FALSE(parse_cli({"--frames", "0"}));
    CHECK_FALSE(parse_cli({"--frames", "12x"}));
    CHECK_FALSE(parse_cli({"--theme", "blue"}));
    CHECK_FALSE(parse_cli({"--lang", "de"}));
    CHECK_FALSE(parse_cli({"--scale", "9"}));
    CHECK_FALSE(parse_cli({"--bogus"}));
    CHECK_FALSE(parse_cli({"--no-animation"})); // typo is reported, not ignored
}

TEST_CASE("Density and window size", "[cli]") {
    using navigator::app::DensityChoice;
    CHECK(parse_cli({})->density == DensityChoice::Compact); // compact is the default
    CHECK(parse_cli({"--density", "comfortable"})->density == DensityChoice::Comfortable);
    CHECK(parse_cli({"--density", "compact"})->density == DensityChoice::Compact);
    CHECK_FALSE(parse_cli({"--density", "dense"}));
    auto o = parse_cli({"--size", "760x600"});
    REQUIRE(o);
    CHECK(o->config.width == 760);
    CHECK(o->config.height == 600);
    CHECK(parse_cli({})->config.width == 1280);
    CHECK_FALSE(parse_cli({"--size", "760"}));
    CHECK_FALSE(parse_cli({"--size", "100x100"})); // below the window minimum
    CHECK_FALSE(parse_cli({"--size", "800x600x2"}));
    CHECK_FALSE(parse_cli({"--size", "x600"}));
}

TEST_CASE("A positional PATH becomes the start path", "[cli]") {
    auto o = parse_cli({"/tmp/some dir"});
    REQUIRE(o);
    CHECK(o->start_path == "/tmp/some dir");
    auto mixed = parse_cli({"--theme", "light", "relative/İş", "--fps"});
    REQUIRE(mixed);
    CHECK(mixed->start_path == "relative/İş");
    CHECK(mixed->config.show_fps);
    // "--" ends options, so a folder may start with a dash.
    auto dashed = parse_cli({"--", "--weird"});
    REQUIRE(dashed);
    CHECK(dashed->start_path == "--weird");
    CHECK(parse_cli({"-"})->start_path == "-");
    CHECK_FALSE(parse_cli({"a", "b"})); // one PATH only
    CHECK_FALSE(parse_cli({""}));
}

TEST_CASE("Development flags parse", "[cli]") {
    auto o = parse_cli({"--bench-scroll", "50000", "--debug-script", "cmd:list.select_all"});
    REQUIRE(o);
    CHECK(o->bench_scroll == 50000);
    CHECK(o->debug_script == "cmd:list.select_all");
    CHECK_FALSE(parse_cli({"--bench-scroll", "0"}));
    CHECK_FALSE(parse_cli({"--bench-scroll"}));
    // Hidden from --help.
    const std::string help = navigator::app::usage_text("navigator");
    CHECK(help.find("--bench-scroll") == std::string::npos);
    CHECK(help.find("--debug-script") == std::string::npos);
}

TEST_CASE("Help documents every user-facing flag", "[cli]") {
    const std::string help = navigator::app::usage_text("navigator");
    for (const char* flag :
         {"--frames", "--theme", "--lang", "--fps", "--no-fps", "--no-animations", "--version",
          "--help", "PATH", "--density", "--size"}) {
        INFO(flag);
        CHECK(help.find(flag) != std::string::npos);
    }
}

TEST_CASE("Language detection follows POSIX precedence", "[cli]") {
    CHECK(detect_language("", "", "", "") == Language::English);
    CHECK(detect_language("", "", "", "tr_TR.UTF-8") == Language::Turkish);
    CHECK(detect_language("", "", "", "en_US.UTF-8") == Language::English);
    CHECK(detect_language("tr:en", "", "", "en_US.UTF-8") == Language::Turkish);
    CHECK(detect_language("en_GB:tr", "", "", "tr_TR.UTF-8") == Language::English);
    CHECK(detect_language("", "tr_TR.UTF-8", "en_US.UTF-8", "") == Language::Turkish);
    CHECK(detect_language("", "", "tr_TR", "en_US") == Language::Turkish);
    CHECK(detect_language("", "", "", "tr") == Language::Turkish);
    CHECK(detect_language("", "", "", "trx") == Language::English);
    CHECK(detect_language("", "C", "", "tr_TR") == Language::English);
}

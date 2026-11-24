// Tests for navigator::palette::History, history_from_json / to_json, and load_history /
// save_history (navigator/palette.hpp "History").
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/palette.hpp>

#include "temp_dir.hpp"

namespace navigator::palette {
namespace {

void write_raw(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

TEST_CASE("palette::History record, boost, saturation and limit", "[search][palette][history]") {
    History h;
    CHECK(h.items().empty());
    CHECK_FALSE(h.rank_of("view.toggle_sidebar").has_value());
    CHECK(h.count_of("view.toggle_sidebar") == 0U);
    CHECK(history_boost(h, "view.toggle_sidebar") == 0);

    h.record("");
    CHECK(h.items().empty());

    h.record("view.toggle_sidebar");
    CHECK(h.items().size() == 1U);
    CHECK(h.rank_of("view.toggle_sidebar") == 0U);
    CHECK(h.count_of("view.toggle_sidebar") == 1U);
    CHECK(history_boost(h, "view.toggle_sidebar") == kHistoryRecentBoost + kHistoryCountBoost);

    h.record("nav.goto");
    CHECK(h.rank_of("nav.goto") == 0U);
    CHECK(h.rank_of("view.toggle_sidebar") == 1U);
    CHECK(history_boost(h, "view.toggle_sidebar") ==
          (kHistoryRecentBoost - kHistoryRecentStep) + kHistoryCountBoost);

    // Re-recording moves to front and increments count.
    h.record("view.toggle_sidebar");
    CHECK(h.rank_of("view.toggle_sidebar") == 0U);
    CHECK(h.count_of("view.toggle_sidebar") == 2U);
    CHECK(history_boost(h, "view.toggle_sidebar") ==
          kHistoryRecentBoost + (2 * kHistoryCountBoost));

    // Boost caps at kHistoryBoostMax.
    for (int i = 0; i < 30; ++i) {
        h.record("view.toggle_sidebar");
    }
    CHECK(history_boost(h, "view.toggle_sidebar") == kHistoryBoostMax);

    // Saturation at kMaxHistoryCount.
    h.assign({HistoryItem{"sat.cmd", kMaxHistoryCount}});
    CHECK(h.count_of("sat.cmd") == kMaxHistoryCount);
    h.record("sat.cmd");
    CHECK(h.count_of("sat.cmd") == kMaxHistoryCount);

    // Bounded at kMaxHistoryItems (oldest dropped).
    h.clear();
    for (std::size_t i = 0; i < kMaxHistoryItems + 15U; ++i) {
        h.record("cmd." + std::to_string(i));
    }
    REQUIRE(h.items().size() == kMaxHistoryItems);
    CHECK(h.items().front().key == "cmd." + std::to_string(kMaxHistoryItems + 14U));
    CHECK_FALSE(h.rank_of("cmd.0").has_value());
}

TEST_CASE("palette::History::assign filters invalid, duplicates and excess",
          "[search][palette][history]") {
    std::vector<HistoryItem> raw;
    raw.push_back(HistoryItem{"", 5});
    raw.push_back(HistoryItem{"zero.count", 0});
    raw.push_back(HistoryItem{"dup.cmd", 3});
    raw.push_back(HistoryItem{"dup.cmd", 99});
    raw.push_back(HistoryItem{"huge.cmd", kMaxHistoryCount + 500U});
    for (std::size_t i = 0; i < kMaxHistoryItems + 10U; ++i) {
        raw.push_back(HistoryItem{"extra." + std::to_string(i), 1});
    }

    History h;
    h.assign(std::move(raw));
    REQUIRE(h.items().size() == kMaxHistoryItems);
    CHECK(h.items()[0] == HistoryItem{"dup.cmd", 3});
    CHECK(h.items()[1] == HistoryItem{"huge.cmd", kMaxHistoryCount});
    CHECK_FALSE(h.rank_of("zero.count").has_value());
}

TEST_CASE("palette history JSON round-trip and determinism", "[search][palette][history]") {
    History h;
    h.record("file.new_folder");
    h.record("edit.copy_path#1");
    h.record("file.new_folder");

    const std::string json1 = to_json(h);
    const std::string json2 = to_json(h);
    CHECK(json1 == json2);
    CHECK(json1.ends_with('\n'));

    const Result<History> parsed = history_from_json(json1);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == h);
}

TEST_CASE("palette::history_from_json is lenient on malformed entries and rejects non-objects",
          "[search][palette][history]") {
    SECTION("non-object or invalid JSON fails with InvalidArgument") {
        CHECK(history_from_json("[]").error().code == ErrorCode::InvalidArgument);
        CHECK(history_from_json("\"str\"").error().code == ErrorCode::InvalidArgument);
        CHECK(history_from_json("{bad").error().code == ErrorCode::InvalidArgument);
    }

    SECTION("oversized input fails with InvalidArgument") {
        const std::string huge(std::size_t{1} << 20U, ' ');
        CHECK(history_from_json(huge + "{}").error().code == ErrorCode::InvalidArgument);
    }

    SECTION("missing or non-array items yields empty history") {
        const auto a = history_from_json(R"({"version": 1, "unknown": 42})");
        REQUIRE(a.has_value());
        CHECK(a->items().empty());

        const auto b = history_from_json(R"({"version": 1, "items": "not-an-array"})");
        REQUIRE(b.has_value());
        CHECK(b->items().empty());
    }

    SECTION(
        "bad entries dropped, missing count defaults to 1, duplicates keep first, count clamped") {
        const std::string doc = R"({
            "version": 99,
            "extra_root": true,
            "items": [
                123,
                {"command": ""},
                {"command": 42, "count": 2},
                {"command": "neg.count", "count": -1},
                {"command": "zero.count", "count": 0},
                {"command": "float.count", "count": 1.5},
                {"command": "str.count", "count": "2"},
                {"command": "default.count", "extra": "ignored"},
                {"command": "dup.cmd", "count": 4},
                {"command": "dup.cmd", "count": 9},
                {"command": "huge.cmd", "count": 999999999}
            ]
        })";
        const Result<History> parsed = history_from_json(doc);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->items().size() == 3U);
        CHECK(parsed->items()[0] == HistoryItem{"default.count", 1});
        CHECK(parsed->items()[1] == HistoryItem{"dup.cmd", 4});
        CHECK(parsed->items()[2] == HistoryItem{"huge.cmd", kMaxHistoryCount});
    }
}

TEST_CASE("palette load_history and save_history atomic I/O and .bad move-aside",
          "[search][palette][history]") {
    test::TempDir tmp;
    const Path config = tmp / "nested" / "config";
    const Path file = history_file(config);

    // Missing file -> NotFound.
    const auto missing = load_history(file);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    // Empty path -> InvalidArgument.
    CHECK(save_history(Path{}, History{}).error().code == ErrorCode::InvalidArgument);
    CHECK(load_history(Path{}).error().code == ErrorCode::InvalidArgument);

    // Save creates parent dirs (0700) and file (0600) and round-trips.
    History h;
    h.record("nav.goto");
    h.record("view.toggle_sidebar");
    REQUIRE(save_history(file, h).has_value());

    struct stat dir_st {};
    REQUIRE(::stat(config.c_str(), &dir_st) == 0);
    CHECK((dir_st.st_mode & 0777U) == 0700U);

    struct stat file_st {};
    REQUIRE(::stat(file.c_str(), &file_st) == 0);
    CHECK((file_st.st_mode & 0777U) == 0600U);

    const auto loaded = load_history(file);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == h);

    // Corrupt non-object file is moved aside to .bad.
    write_raw(file, "not json at all");
    const auto corrupt = load_history(file);
    REQUIRE_FALSE(corrupt.has_value());
    CHECK(corrupt.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(file));
    CHECK(std::filesystem::exists(Path(file.native() + ".bad")));
}

} // namespace
} // namespace navigator::palette

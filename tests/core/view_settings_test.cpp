// tests/core/view_settings_test.cpp — unit tests for ViewSettingsStore, zoom axis, column reorder,
// hierarchical resolution and JSON/file persistence.
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include <navigator/view_settings.hpp>

#include "temp_dir.hpp"

using namespace navigator;
using namespace navigator::view;

TEST_CASE("view.zoom axis transitions between Details and Icons", "[view_settings]") {
    REQUIRE(step_zoom(ViewMode::Details, 100, -1) == std::pair{ViewMode::Details, 100});
    REQUIRE(step_zoom(ViewMode::Details, 100, 1) == std::pair{ViewMode::Icons, kMinZoomPercent});
    REQUIRE(step_zoom(ViewMode::Icons, kMinZoomPercent, 1) ==
            std::pair{ViewMode::Icons, kMinZoomPercent + kZoomStepPercent});
    REQUIRE(step_zoom(ViewMode::Icons, kMinZoomPercent, -1) ==
            std::pair{ViewMode::Details, kDefaultZoomPercent});
    REQUIRE(step_zoom(ViewMode::Icons, kMaxZoomPercent, 2) ==
            std::pair{ViewMode::Icons, kMaxZoomPercent});
    REQUIRE(step_zoom(ViewMode::Tree, 100, 1) == std::pair{ViewMode::Tree, 125});
}

TEST_CASE("view.column reorder and normalization", "[view_settings]") {
    const auto moved = reorder_column(kDefaultColumnOrder, 0, 2);
    REQUIRE(moved ==
            ColumnOrder{ColumnId::Size, ColumnId::Type, ColumnId::Name, ColumnId::Modified});

    const auto back = reorder_column(moved, 2, 0);
    REQUIRE(back == kDefaultColumnOrder);

    const ColumnOrder dup{ColumnId::Type, ColumnId::Type, ColumnId::Name, ColumnId::Type};
    REQUIRE(normalize_columns(dup) ==
            ColumnOrder{ColumnId::Type, ColumnId::Name, ColumnId::Size, ColumnId::Modified});
}

TEST_CASE("view.hierarchical resolution precedence: Folder > deepest Hierarchy > Global",
          "[view_settings]") {
    ViewSettingsStore store;
    ViewSettings global_s;
    global_s.mode = ViewMode::Details;
    store.set_global(global_s);

    ViewSettings media_s;
    media_s.mode = ViewMode::Icons;
    media_s.zoom_percent = 150;
    store.set_hierarchy(Path{"/home/user/Media/"}, media_s); // trailing slash normalized

    ViewSettings photos_s;
    photos_s.mode = ViewMode::Icons;
    photos_s.zoom_percent = 250;
    photos_s.group_by = GroupBy::Date;
    store.set_hierarchy(Path{"/home/user/Media/Photos"}, photos_s);

    ViewSettings exact_s;
    exact_s.mode = ViewMode::Tree;
    store.set_folder(Path{"/home/user/Media/Photos/2026"}, exact_s);

    // Unmatched -> Global
    auto r = store.resolve(Path{"/etc"});
    REQUIRE(r.scope == ViewScope::Global);
    REQUIRE(r.settings.mode == ViewMode::Details);

    // Prefix guard: /home/user/Media2 must NOT match /home/user/Media
    r = store.resolve(Path{"/home/user/Media2"});
    REQUIRE(r.scope == ViewScope::Global);

    // Direct child of Media -> Media hierarchy
    r = store.resolve(Path{"/home/user/Media/Videos"});
    REQUIRE(r.scope == ViewScope::Hierarchy);
    REQUIRE(r.rule_path == Path{"/home/user/Media"});
    REQUIRE(r.settings.zoom_percent == 150);

    // Deeper hierarchy wins
    r = store.resolve(Path{"/home/user/Media/Photos/2025"});
    REQUIRE(r.scope == ViewScope::Hierarchy);
    REQUIRE(r.rule_path == Path{"/home/user/Media/Photos"});
    REQUIRE(r.settings.zoom_percent == 250);
    REQUIRE(r.settings.group_by == GroupBy::Date);

    // Exact folder wins over hierarchy
    r = store.resolve(Path{"/home/user/Media/Photos/2026"});
    REQUIRE(r.scope == ViewScope::Folder);
    REQUIRE(r.rule_path == Path{"/home/user/Media/Photos/2026"});
    REQUIRE(r.settings.mode == ViewMode::Tree);

    // Child of exact folder falls back to deeper hierarchy
    r = store.resolve(Path{"/home/user/Media/Photos/2026/Raw"});
    REQUIRE(r.scope == ViewScope::Hierarchy);
    REQUIRE(r.rule_path == Path{"/home/user/Media/Photos"});
}

TEST_CASE("view.cycle_lock and apply_change target the governing scope", "[view_settings]") {
    ViewSettingsStore store;
    const Path dir{"/tmp/proj"};
    ViewSettings s;
    s.mode = ViewMode::Icons;
    s.zoom_percent = 175;

    REQUIRE(store.cycle_lock(dir, s) == ViewScope::Folder);
    REQUIRE(store.resolve(dir).scope == ViewScope::Folder);

    s.mode = ViewMode::List;
    store.apply_change(dir, s);
    REQUIRE(store.resolve(dir).settings.mode == ViewMode::List);
    REQUIRE(store.global().mode == ViewMode::Details);

    REQUIRE(store.cycle_lock(dir, s) == ViewScope::Hierarchy);
    REQUIRE(store.resolve(Path{"/tmp/proj/sub"}).scope == ViewScope::Hierarchy);
    REQUIRE(store.resolve(Path{"/tmp/proj/sub"}).settings.mode == ViewMode::List);

    REQUIRE(store.cycle_lock(dir, s) == ViewScope::Global);
    REQUIRE(store.resolve(dir).scope == ViewScope::Global);
}

TEST_CASE("view.JSON round-trip, non-UTF-8 paths and atomic file persistence", "[view_settings]") {
    ViewSettingsStore store;
    ViewSettings g;
    g.mode = ViewMode::List;
    g.zoom_percent = 125;
    g.group_by = GroupBy::Size;
    g.show_hidden = true;
    g.columns = reorder_column(kDefaultColumnOrder, 3, 0);
    g.sort.key = fs::SortKey::Modified;
    g.sort.descending = true;
    store.set_global(g);

    ViewSettings f;
    f.mode = ViewMode::Icons;
    f.zoom_percent = 200;
    const Path raw_path{std::string("/tmp/raw-\xFF\xFE-dir")};
    store.set_folder(raw_path, f);
    store.set_hierarchy(Path{"/home/oguz/Docs"},
                        ViewSettings{ViewMode::Tree, 100, GroupBy::None, fs::SortOptions{}, false,
                                     kDefaultColumnOrder});

    const auto json = to_json(store);
    REQUIRE(json.has_value());

    const auto decoded = from_json(*json);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == store);

    // Deterministic serialization
    const auto json2 = to_json(*decoded);
    REQUIRE(json2.has_value());
    REQUIRE(*json2 == *json);

    // Atomic file save & load
    navigator::test::TempDir tmp;
    const Path file = view_settings_file(tmp.path());
    REQUIRE(save_view_settings(file, store).has_value());
    const auto loaded = load_view_settings(file);
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == store);

    // Lenient load ignores unknown fields and drops invalid rules
    const auto lenient = from_json(R"({
        "version": 99,
        "extra": true,
        "global": { "mode": "bogus", "zoom": 9999, "group_by": "type", "columns": ["size", "size"] },
        "rules": [
            { "path": "relative/bad", "scope": "folder", "settings": {} },
            { "path": "/ok", "scope": "hierarchy", "settings": { "mode": "icons", "zoom": 10 } }
        ]
    })");
    REQUIRE(lenient.has_value());
    REQUIRE(lenient->global().mode == ViewMode::Details);
    REQUIRE(lenient->global().zoom_percent == kMaxZoomPercent);
    REQUIRE(lenient->global().group_by == GroupBy::Type);
    REQUIRE(lenient->rules().size() == 1);
    REQUIRE(lenient->resolve(Path{"/ok/sub"}).settings.zoom_percent == kMinZoomPercent);
}

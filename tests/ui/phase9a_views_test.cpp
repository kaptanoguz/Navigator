// tests/ui/phase9a_views_test.cpp — Phase 9A unit tests: virtualized grouping, async TreeModel,
// BrowserModel state preservation across Grid <-> Tree, zoom axis and column reorder.
#include <chrono>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "browser/browser_model.hpp"
#include "browser/tree_model.hpp"
#include "browser/view_rows.hpp"
#include "ui_fixture.hpp"

using namespace navigator;
using namespace navigator::ui;

namespace {

fs::DirEntry make_entry(std::string name, fs::EntryKind kind, std::uint64_t size = 0,
                        Timestamp mtime = {}) {
    fs::DirEntry e;
    e.name = std::move(name);
    e.kind = kind;
    e.size = size;
    e.mtime = mtime;
    return e;
}

} // namespace

TEST_CASE("phase9a.virtualized grouping by Date, Type and Size + collapse", "[phase9a]") {
    const auto now_s = std::chrono::sys_seconds{std::chrono::seconds{1'750'000'000}};
    const Timestamp now = std::chrono::time_point_cast<std::chrono::nanoseconds>(now_s);
    const Timestamp today = now - std::chrono::hours{1};
    const Timestamp week = now - std::chrono::hours{24 * 4};
    const Timestamp old = now - std::chrono::hours{24 * 400};

    std::vector<fs::DirEntry> entries{
        make_entry("docs", fs::EntryKind::Directory, 0, today),
        make_entry("photo.png", fs::EntryKind::File, 500'000, today),
        make_entry("empty.txt", fs::EntryKind::File, 0, week),
        make_entry("big.bin", fs::EntryKind::File, 200'000'000, old),
        make_entry("nodate.dat", fs::EntryKind::File, 50, Timestamp{}),
    };
    auto getter = [](const void* ctx, std::size_t row) -> const fs::DirEntry* {
        const auto* vec = static_cast<const std::vector<fs::DirEntry>*>(ctx);
        return &(*vec)[row];
    };

    std::unordered_set<std::uint8_t> collapsed;

    // Group by Date
    const GroupedRows by_date =
        build_grouped_rows(entries.size(), getter, &entries, view::GroupBy::Date, now, collapsed);
    REQUIRE(by_date.group_count == 4); // Today, This week, Older, Unknown
    REQUIRE(by_date.items.size() == entries.size() + 4);
    REQUIRE(by_date.items[0].kind == GroupDisplayItem::Kind::Header);
    REQUIRE(by_date.items[0].count == 2); // docs + photo.png

    // Collapsing Today removes its 2 entries from the virtualized display list
    collapsed.insert(0);
    const GroupedRows by_date_collapsed =
        build_grouped_rows(entries.size(), getter, &entries, view::GroupBy::Date, now, collapsed);
    REQUIRE(by_date_collapsed.items.size() == by_date.items.size() - 2);
    REQUIRE(by_date_collapsed.items[0].collapsed);

    // Group by Type
    collapsed.clear();
    const GroupedRows by_type =
        build_grouped_rows(entries.size(), getter, &entries, view::GroupBy::Type, now, collapsed);
    REQUIRE(by_type.group_count >= 3); // Folders, Images, Documents/Other
    REQUIRE(group_bucket_key(view::GroupBy::Type, 0) == "group.type.folders");

    // Group by Size
    const GroupedRows by_size =
        build_grouped_rows(entries.size(), getter, &entries, view::GroupBy::Size, now, collapsed);
    REQUIRE(by_size.group_count == 5); // Folders, Empty, Tiny, Small, Large
    REQUIRE(group_bucket_key(view::GroupBy::Size, 1) == "group.size.empty");
}

TEST_CASE("phase9a.BrowserModel preserves selection, cursor and expanded Tree across Grid <-> Tree",
          "[phase9a]") {
    navigator::test::TempDir fx;
    fx.dir("alpha");
    fx.file("alpha/nested.txt", "hello");
    fx.dir("beta");
    fx.file("gamma.txt", "world");

    BrowserModel::Options opts;
    BrowserModel model(std::move(opts));
    model.open_start(fx.path());
    REQUIRE(model.wait_listing(std::chrono::seconds{5}));
    REQUIRE(model.frame_start());
    REQUIRE(model.row_count() == 3);

    // Select "beta" in Details, switch to Icons + zoom
    const std::size_t beta_row = model.find_row("beta");
    REQUIRE(beta_row != kNoRow);
    model.click_row(beta_row, Modifiers::None);
    REQUIRE(model.is_selected(beta_row));

    model.step_zoom(1);
    REQUIRE(model.view_mode() == view::ViewMode::Icons);
    REQUIRE(model.zoom_percent() == view::kMinZoomPercent);
    model.step_zoom(2);
    REQUIRE(model.zoom_percent() == view::kMinZoomPercent + (2 * view::kZoomStepPercent));

    // Switch to Tree, expand "alpha", wait for async child listing
    model.set_view_mode(view::ViewMode::Tree);
    REQUIRE(model.tree_rows().size() == 3);
    model.tree_toggle(fx.path() / "alpha");
    REQUIRE(model.wait_tree_idle(std::chrono::seconds{5}));
    REQUIRE(model.frame_start());
    REQUIRE(model.tree_rows().size() == 4);
    REQUIRE(model.tree_rows()[1].depth == 1);
    REQUIRE(model.tree_rows()[1].entry.name == "nested.txt");

    // Switch back to Icons and then back to Tree on the same folder:
    // selection, cursor AND expanded tree nodes are preserved!
    model.set_view_mode(view::ViewMode::Icons);
    REQUIRE(model.is_selected(beta_row));
    REQUIRE(model.cursor() == beta_row);

    model.set_view_mode(view::ViewMode::Tree);
    REQUIRE(model.tree_rows().size() == 4);
    REQUIRE(model.is_selected(beta_row));

    // Collapse all folds alpha back
    model.tree_collapse_all();
    REQUIRE(model.tree_rows().size() == 3);

    // Column reorder works and stays normalized
    model.reorder_column(0, 3);
    REQUIRE(model.column_order()[3] == view::ColumnId::Name);
}

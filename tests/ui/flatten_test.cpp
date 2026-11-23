// Phase 5 flattened view over scripted walks and a fake index: streaming, order, cancel,
// activation, suspension; the generated tree of the acceptance smoke.
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/platform.hpp>

#include "bench_fixture.hpp"
#include "search/filter_rules.hpp"
#include "search_test_util.hpp"
#include "ui_fixture.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::cursor_of;
using navigator::test::FakeIndex;
using navigator::test::kSearchWait;
using navigator::test::make_entry;
using navigator::test::open_listed;
using navigator::test::pump_until;
using navigator::test::rows_of;
using navigator::test::TempDir;
using navigator::test::WalkScript;
using Names = std::vector<std::string>;

namespace {

class FakeLauncher final : public platform::IAppLauncher {
public:
    Result<void> launch(const platform::LaunchRequest& request) override {
        calls.push_back(request);
        return ok();
    }
    std::vector<platform::LaunchRequest> calls;
};

// Depth 1: sub/, zeta.txt. Then sub/: notes.md, inner/.
fs::WalkBatch top_batch() {
    fs::WalkBatch b;
    b.first_dir = 1;
    b.new_dirs = {"sub"};
    b.entries.push_back(fs::WalkEntry{make_entry("sub", true), 0, 1, 1});
    b.entries.push_back(fs::WalkEntry{make_entry("zeta.txt"), 0, 1, 0});
    return b;
}

fs::WalkBatch sub_batch() {
    fs::WalkBatch b;
    b.first_dir = 2;
    b.new_dirs = {"sub/inner"};
    b.entries.push_back(fs::WalkEntry{make_entry("notes.md"), 1, 2, 0});
    b.entries.push_back(fs::WalkEntry{make_entry("inner", true), 1, 2, 2});
    return b;
}

fs::WalkBatch inner_batch() {
    fs::WalkBatch b;
    b.first_dir = 3;
    b.entries.push_back(fs::WalkEntry{make_entry("notes.md"), 2, 3, 0});
    b.entries.push_back(fs::WalkEntry{make_entry("alpha.txt"), 2, 3, 0});
    return b;
}

struct Flattened {
    TempDir dir;
    FakeLauncher launcher;
    std::shared_ptr<WalkScript> walk = std::make_shared<WalkScript>();
    std::unique_ptr<BrowserModel> model;

    Flattened() {
        dir.file("plain.txt");
        dir.dir("sub/inner");
        BrowserModel::Options o;
        o.launcher = &launcher;
        o.walk_factory = WalkScript::factory(walk);
        model = std::make_unique<BrowserModel>(std::move(o));
        open_listed(*model, dir.path());
        REQUIRE(model->execute(Command{CommandId::ToggleFlatten, 0}));
        REQUIRE(model->flattened());
    }
    ~Flattened() {
        model.reset();
        walk->end();
    }
    Flattened(const Flattened&) = delete;
    Flattened& operator=(const Flattened&) = delete;
    Flattened(Flattened&&) = delete;
    Flattened& operator=(Flattened&&) = delete;

    void feed(fs::WalkBatch batch, std::size_t rows) {
        walk->feed(std::move(batch));
        REQUIRE(pump_until(*model, [&] { return model->row_count() == rows; }));
    }
    void finish() {
        walk->end();
        REQUIRE(model->settle_search(kSearchWait));
    }
};

} // namespace

TEST_CASE("Flatten streams batches and sorts while the walk runs", "[flatten]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    CHECK(m.flatten_progress().walking);
    CHECK(rows_of(m) == Names{"sub", "zeta.txt"});

    f.feed(sub_batch(), 4);
    f.feed(inner_batch(), 6);
    f.finish();
    // Folders first, then names; equal names tie on the relative parent path.
    CHECK(rows_of(m) == Names{"inner", "sub", "alpha.txt", "notes.md", "notes.md", "zeta.txt"});
    CHECK(m.row_parent(3) == "sub");
    CHECK(m.row_parent(4) == "sub/inner");
    CHECK(m.row_path(4) == f.dir.path() / "sub" / "inner" / "notes.md");
    const FlattenProgress p = m.flatten_progress();
    CHECK(p.found == 6);
    CHECK_FALSE(p.walking);
    CHECK_FALSE(p.cancelled);
    CHECK(cursor_of(m) == "inner"); // follows the top until the user moves it
    CHECK(m.take_visits().empty()); // flatten never records Recents
}

TEST_CASE("A flatten walk stays on one volume and follows the hidden policy", "[flatten]") {
    Flattened f;
    REQUIRE(pump_until(*f.model, [&] { return f.walk->opened() == 1; }));
    const fs::WalkOptions o = f.walk->options();
    CHECK(o.root == f.dir.path());
    CHECK_FALSE(o.show_hidden);
    CHECK_FALSE(o.follow_symlinks);
    CHECK_FALSE(o.cross_filesystems);
    CHECK(o.metadata);
}

TEST_CASE("A query ranks by relevance until a header is clicked", "[flatten]") {
    Flattened f;
    BrowserModel& m = *f.model;
    CHECK(m.match_mode() == search::MatchMode::Fuzzy);
    f.feed(top_batch(), 2);
    f.feed(sub_batch(), 4);
    f.feed(inner_batch(), 6);
    f.finish();

    m.set_filter_text("nts");
    REQUIRE(m.settle_search(kSearchWait));
    REQUIRE(rows_of(m) == Names{"notes.md", "notes.md"});
    CHECK(m.row_parent(0) == "sub"); // same score: the shallower one first
    CHECK(m.source_count() == 6);

    m.sort_by(fs::SortKey::Name); // the same key again: descending
    CHECK(m.header_sorted());
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.row_parent(0) == "sub/inner");
    m.set_filter_text("note");
    CHECK_FALSE(m.header_sorted());
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.row_parent(0) == "sub");
}

TEST_CASE("A query typed mid-walk applies to later batches too", "[flatten]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    m.set_filter_text("notes");
    REQUIRE(pump_until(m, [&] { return m.row_count() == 0; }));
    f.feed(sub_batch(), 1);
    f.feed(inner_batch(), 2);
    CHECK(f.walk->opened() == 1); // the walk never restarts for a query
}

TEST_CASE("Cancel keeps the rows and never waits for the walk", "[flatten][cancel]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    const auto before = std::chrono::steady_clock::now();
    CHECK(m.execute(Command{CommandId::CancelSearch, 0}));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100));
    CHECK_FALSE(m.flatten_progress().walking);
    CHECK(m.flatten_progress().cancelled);
    CHECK(rows_of(m) == Names{"sub", "zeta.txt"});
    CHECK(FlattenWalk::wait_for_exit(kSearchWait));
    CHECK(f.walk->cancelled() == 1);
}

TEST_CASE("Escape stops the walk, clears the selection, leaves, then clears the text",
          "[flatten][escape]") {
    Flattened f;
    BrowserModel& m = *f.model;
    m.set_filter_text("t");
    f.feed(top_batch(), 1);
    m.select_all();
    m.escape();
    CHECK_FALSE(m.flatten_progress().walking);
    CHECK(m.selected_count() == 1);
    REQUIRE(m.settle_search(kSearchWait));
    m.escape();
    CHECK(m.selected_count() == 0);
    CHECK(m.flattened());
    m.escape();
    CHECK_FALSE(m.flattened());
    CHECK(m.filter_active());
    CHECK(rows_of(m) == Names{"plain.txt"});
    m.escape();
    CHECK_FALSE(m.filter_active());
    CHECK(m.row_count() == 2);
}

TEST_CASE("Activating a file opens it in place", "[flatten][activate]") {
    Flattened f;
    f.dir.file("sub/notes.md"); // open_with_default checks the path before launching
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    f.feed(sub_batch(), 4);
    f.finish();
    m.click_row(m.find_row("notes.md"), Modifiers::None);
    m.activate();
    CHECK(f.launcher.calls.size() == 1);
    CHECK(m.flattened());
    CHECK(m.current_dir() == f.dir.path());
}

TEST_CASE("Activating a folder goes into it and leaves flatten", "[flatten][activate]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    f.feed(sub_batch(), 4);
    f.finish();
    m.set_filter_text("inner");
    REQUIRE(m.settle_search(kSearchWait));
    REQUIRE(cursor_of(m) == "inner");
    m.activate();
    CHECK_FALSE(m.flattened());
    CHECK_FALSE(m.filter_active());
    REQUIRE(m.wait_listing(kSearchWait));
    (void)m.frame_start();
    CHECK(m.current_dir() == f.dir.path() / "sub" / "inner");
    CHECK(f.launcher.calls.empty());
}

TEST_CASE("A moved cursor follows its row through reordering", "[flatten]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    m.cursor_end(false);
    REQUIRE(cursor_of(m) == "zeta.txt");
    f.feed(sub_batch(), 4);
    f.feed(inner_batch(), 6);
    CHECK(cursor_of(m) == "zeta.txt");
    CHECK(m.selected_count() == 1);
}

TEST_CASE("Types hide rows of a flattened view and stay listed", "[flatten][types]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    f.feed(sub_batch(), 4);
    f.finish();
    search::TypeFilter types;
    types.hide_folders = true;
    m.set_type_filter(types);
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(rows_of(m) == Names{"notes.md", "zeta.txt"});
    REQUIRE(m.type_summary().size() == 3);
    CHECK(m.type_summary()[0].count == 2);
    CHECK_FALSE(m.type_summary()[0].visible);
}

TEST_CASE("The tab persists the folder under a flattened view", "[flatten]") {
    Flattened f;
    f.feed(top_batch(), 2);
    const layout::TabState s = f.model->tab_state(24.0F);
    CHECK(s.path == f.dir.path());
    CHECK(s.cursor_name == "sub");
}

TEST_CASE("Suspension stops a walk and resume walks again", "[flatten][suspend]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    m.suspend();
    CHECK(FlattenWalk::wait_for_exit(kSearchWait));
    CHECK(f.walk->cancelled() == 1);
    m.resume();
    REQUIRE(pump_until(m, [&] { return f.walk->opened() == 2; }));
    CHECK(m.flatten_progress().walking);
}

TEST_CASE("Suspension leaves a walk the user stopped alone", "[flatten][suspend]") {
    Flattened f;
    BrowserModel& m = *f.model;
    f.feed(top_batch(), 2);
    m.cancel_search();
    m.suspend();
    m.resume();
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(f.walk->opened() == 1);
    CHECK(m.row_count() == 2);
}

TEST_CASE("An unreadable flatten root reports its error", "[flatten]") {
    TempDir t;
    auto script = std::make_shared<WalkScript>();
    script->fail_open(ErrorCode::PermissionDenied);
    BrowserModel::Options o;
    o.walk_factory = WalkScript::factory(script);
    BrowserModel m(std::move(o));
    open_listed(m, t.path());
    m.flatten(FlattenRequest{});
    REQUIRE(m.settle_search(kSearchWait));
    const std::optional<Error> error = m.flatten_progress().error;
    REQUIRE(error.has_value());
    CHECK(error.value_or(Error{}).code == ErrorCode::PermissionDenied);
    CHECK(m.row_count() == 0);
}

TEST_CASE("An index refresh keeps cursor and selection by path", "[flatten][index]") {
    TempDir t;
    auto index = std::make_shared<FakeIndex>(t.path());
    index->set_names({"a/report.txt", "b/report.txt", "zebra.txt"});
    BrowserModel::Options o;
    o.index_debounce = std::chrono::milliseconds(0);
    BrowserModel m(std::move(o));
    open_listed(m, t.path());
    m.flatten(FlattenRequest{t.path(), true, index});
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.flatten_progress().from_index);
    m.set_filter_text("report");
    REQUIRE(m.settle_search(kSearchWait));
    REQUIRE(m.row_count() == 2);
    m.click_row(1, Modifiers::None);
    CHECK(m.row_parent(1) == "b");

    index->set_names({"0/report.txt", "a/report.txt", "b/report.txt"}); // a live update
    REQUIRE(pump_until(m, [&] { return m.row_count() == 3; }));
    CHECK(m.row_parent(m.cursor()) == "b");
    CHECK(m.selected_count() == 1);
    CHECK(m.is_selected(m.cursor()));
}

TEST_CASE("A drive search keeps its scope through typing and refreshes", "[flatten][scope]") {
    TempDir t;
    auto index = std::make_shared<FakeIndex>(t.path());
    index->set_names({"a/report.txt", "zebra.txt"});
    BrowserModel::Options o;
    o.index_debounce = std::chrono::milliseconds(0);
    BrowserModel m(std::move(o));
    open_listed(m, t.path());
    m.flatten(FlattenRequest{t.path(), true, index});
    REQUIRE(m.settle_search(kSearchWait));
    const auto scope = [&] { return search_scope(m.flattened(), m.flatten_is_volume()); };
    CHECK(scope() == SearchScope::Drive);
    m.set_filter_text("r");
    m.set_filter_text("rep");
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(scope() == SearchScope::Drive);
    index->set_names({"a/report.txt", "b/report.txt"});
    REQUIRE(pump_until(m, [&] { return m.row_count() == 2; }));
    CHECK(scope() == SearchScope::Drive);
    m.leave_flatten();
    CHECK(scope() == SearchScope::Folder);
    m.flatten(FlattenRequest{});
    CHECK(scope() == SearchScope::Subfolders);
    m.set_filter_text("x");
    CHECK(scope() == SearchScope::Subfolders);
}

TEST_CASE("The acceptance tree is parents-first and exactly N entries", "[flatten][fixture]") {
    std::vector<bool> is_dir;
    const std::vector<std::string> names = app::bench_tree_names(1000, is_dir);
    REQUIRE(names.size() == 1000);
    REQUIRE(is_dir.size() == 1000);
    std::vector<std::string> folders;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (const std::size_t slash = names[i].rfind('/'); slash != std::string::npos) {
            CHECK(std::find(folders.begin(), folders.end(), names[i].substr(0, slash)) !=
                  folders.end());
        }
        if (is_dir[i]) {
            folders.push_back(names[i]);
        }
    }
}

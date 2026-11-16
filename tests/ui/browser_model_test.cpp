// BrowserModel on fixture directories: navigation, history, cursor/selection, hidden items,
// sorting, re-listing, watch updates, activation and error notices. Headless (no SDL).
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

#include <navigator/commands.hpp>
#include <navigator/platform.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "ui_fixture.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::TempDir;

namespace {

constexpr std::chrono::milliseconds kWait{5000};

class FakeLauncher final : public platform::IAppLauncher {
public:
    Result<void> launch(const platform::LaunchRequest& request) override {
        calls.push_back(request);
        return ok();
    }
    std::vector<platform::LaunchRequest> calls;
};

void settle(BrowserModel& m) {
    REQUIRE(m.wait_listing(kWait));
    (void)m.frame_start();
    REQUIRE_FALSE(m.busy());
}

std::vector<std::string> names(const BrowserModel& m) {
    std::vector<std::string> out;
    for (const auto& e : m.entries()) {
        out.push_back(e.name);
    }
    return out;
}

std::vector<std::string> selected_names(const BrowserModel& m) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m.entries().size(); ++i) {
        if (m.is_selected(i)) {
            out.push_back(m.entries()[i].name);
        }
    }
    return out;
}

std::string cursor_name(const BrowserModel& m) {
    return m.cursor() < m.entries().size() ? m.entries()[m.cursor()].name : std::string();
}

// alpha/ (a1.txt, a2.txt), beta/ (deep/), gamma.txt, .hidden, delta.md
void populate(const TempDir& t) {
    t.file("alpha/a1.txt", "1");
    t.file("alpha/a2.txt", "22");
    t.dir("beta/deep");
    t.file("gamma.txt", "333");
    t.file(".hidden", "x");
    t.file("delta.md", "4444");
}

BrowserModel::Options no_watch() { return BrowserModel::Options{}; }

Notice notice_of(const BrowserModel& m) {
    if (const std::optional<Notice>& n = m.notice(); n.has_value()) {
        return *n;
    }
    FAIL("expected a notice");
    return {};
}

Error error_of(const BrowserModel& m) {
    if (const std::optional<Error>& e = m.listing_error(); e.has_value()) {
        return *e;
    }
    FAIL("expected a listing error");
    return {};
}

} // namespace

TEST_CASE("smoke_navigate_into_subfolder_and_back", "[browser][smoke]") {
    TempDir t;
    populate(t);
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    REQUIRE(m.current_dir() == t.path());
    REQUIRE(m.cursor() == 0);
    REQUIRE(cursor_name(m) == "alpha"); // folders first, natural name order

    m.execute(Command{CommandId::Activate, 0});
    settle(m);
    CHECK(m.current_dir() == t.path() / "alpha");
    CHECK(names(m) == std::vector<std::string>{"a1.txt", "a2.txt"});
    CHECK(m.can_back());

    m.execute(Command{CommandId::NavigateBack, 0});
    settle(m);
    CHECK(m.current_dir() == t.path());
    CHECK(cursor_name(m) == "alpha");
    CHECK(m.can_forward());
}

TEST_CASE("Back, forward and up keep history and cursor names", "[browser]") {
    TempDir t;
    populate(t);
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    CHECK(names(m) == std::vector<std::string>{"alpha", "beta", "delta.md", "gamma.txt"});
    CHECK_FALSE(m.can_back());
    CHECK_FALSE(m.can_forward());

    // Into beta (second row), then beta/deep.
    m.cursor_by(1, false);
    REQUIRE(cursor_name(m) == "beta");
    m.activate();
    settle(m);
    REQUIRE(m.current_dir() == t.path() / "beta");
    m.activate(); // cursor on "deep"
    settle(m);
    REQUIRE(m.current_dir() == t.path() / "beta" / "deep");
    CHECK(m.entries().empty());
    CHECK(m.cursor() == kNoRow);
    CHECK(m.back_stack().size() == 2);

    // Up selects the folder we came from and pushes history.
    m.up();
    settle(m);
    CHECK(m.current_dir() == t.path() / "beta");
    CHECK(cursor_name(m) == "deep");
    CHECK(m.back_stack().size() == 3);
    CHECK_FALSE(m.can_forward());

    m.up();
    settle(m);
    CHECK(m.current_dir() == t.path());
    CHECK(cursor_name(m) == "beta");
    CHECK(m.selected_count() == 0); // no selection on entry, only the cursor

    // Back twice -> beta/deep's parent visit order: root <- beta <- deep <- beta.
    m.back();
    settle(m);
    CHECK(m.current_dir() == t.path() / "beta");
    CHECK(cursor_name(m) == "deep");
    m.back();
    settle(m);
    CHECK(m.current_dir() == t.path() / "beta" / "deep");
    CHECK(m.forward_stack().size() == 2);
    m.forward();
    settle(m);
    CHECK(m.current_dir() == t.path() / "beta");
    CHECK(m.forward_stack().size() == 1);

    // A new navigation clears forward history.
    m.navigate_to(t.path() / "alpha");
    settle(m);
    CHECK_FALSE(m.can_forward());
    CHECK(m.current_dir() == t.path() / "alpha");
}

TEST_CASE("Up stops at the root and paths are normalized", "[browser]") {
    BrowserModel m(no_watch());
    m.open_start("/");
    settle(m);
    CHECK(m.current_dir() == "/");
    CHECK_FALSE(m.can_up());
    m.up();
    CHECK_FALSE(m.busy());

    TempDir t;
    t.dir("x");
    m.navigate_to(t.path() / "x" / ".." / "x" / "");
    settle(m);
    CHECK(m.current_dir() == t.path() / "x");
}

TEST_CASE("Cursor movement and keyboard selection", "[browser]") {
    TempDir t;
    for (int i = 1; i <= 30; ++i) {
        t.file("f" + std::to_string(i) + ".txt");
    }
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    REQUIRE(m.entries().size() == 30);
    CHECK(cursor_name(m) == "f1.txt");
    CHECK(m.selected_count() == 0);

    m.execute(Command{CommandId::CursorDown, 0});
    CHECK(cursor_name(m) == "f2.txt");
    CHECK(selected_names(m) == std::vector<std::string>{"f2.txt"});

    m.execute(Command{CommandId::CursorDown, CursorArg::Extend});
    m.execute(Command{CommandId::CursorDown, CursorArg::Extend});
    CHECK(selected_names(m) == std::vector<std::string>{"f2.txt", "f3.txt", "f4.txt"});
    CHECK(m.anchor() == 1);
    m.execute(Command{CommandId::CursorUp, CursorArg::Extend});
    CHECK(selected_names(m) == std::vector<std::string>{"f2.txt", "f3.txt"});

    m.set_page_rows(10);
    m.execute(Command{CommandId::CursorPageDown, 0});
    CHECK(m.cursor() == 2 + 9);
    m.execute(Command{CommandId::CursorEnd, CursorArg::Extend});
    CHECK(m.cursor() == 29);
    CHECK(m.selected_count() == 19); // rows 11..29
    m.execute(Command{CommandId::CursorHome, 0});
    CHECK(m.cursor() == 0);
    CHECK(m.selected_count() == 1);
    m.execute(Command{CommandId::CursorUp, 0}); // clamps
    CHECK(m.cursor() == 0);
    m.execute(Command{CommandId::CursorPageUp, 0});
    CHECK(m.cursor() == 0);

    m.execute(Command{CommandId::SelectAll, 0});
    CHECK(m.selected_count() == 30);
    CHECK(m.cursor() == 0);
    m.execute(Command{CommandId::ClearSelection, 0});
    CHECK(m.selected_count() == 0);
    CHECK(m.cursor() == 0); // cursor stays
}

TEST_CASE("Pointer clicks: plain, Ctrl, Shift, Ctrl+Shift, rubber band", "[browser]") {
    TempDir t;
    for (const char* n : {"a", "b", "c", "d", "e", "f"}) {
        t.file(std::string(n) + ".txt", "12345");
    }
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);

    m.click_row(1, Modifiers::None);
    CHECK(selected_names(m) == std::vector<std::string>{"b.txt"});
    CHECK(m.cursor() == 1);
    m.click_row(3, Modifiers::Ctrl);
    CHECK(selected_names(m) == std::vector<std::string>{"b.txt", "d.txt"});
    CHECK(m.anchor() == 3);
    m.click_row(3, Modifiers::Ctrl); // toggles off
    CHECK(selected_names(m) == std::vector<std::string>{"b.txt"});
    m.click_row(0, Modifiers::Shift); // anchor (3) .. 0, replaces
    CHECK(selected_names(m) == std::vector<std::string>{"a.txt", "b.txt", "c.txt", "d.txt"});
    CHECK(m.cursor() == 0);
    m.click_row(5, Modifiers::None);
    m.click_row(4, Modifiers::Ctrl);                    // anchor 4
    m.click_row(1, Modifiers::Ctrl | Modifiers::Shift); // adds 1..4
    CHECK(selected_names(m) ==
          std::vector<std::string>{"b.txt", "c.txt", "d.txt", "e.txt", "f.txt"});
    CHECK(m.selected_bytes() == 25U);

    m.clear_selection();
    m.click_row(0, Modifiers::None);
    m.band_begin(false);
    m.band_update(2, 4);
    CHECK(selected_names(m) == std::vector<std::string>{"c.txt", "d.txt", "e.txt"});
    m.band_update(3, 3);
    CHECK(selected_names(m) == std::vector<std::string>{"d.txt"});
    m.band_end();
    m.band_begin(true); // Ctrl: adds to the existing selection
    m.band_update(0, 1);
    CHECK(selected_names(m) == std::vector<std::string>{"a.txt", "b.txt", "d.txt"});
    m.band_update(1, 0); // empty band: back to the base selection
    CHECK(selected_names(m) == std::vector<std::string>{"d.txt"});
    m.band_end();
}

TEST_CASE("Hidden toggle re-lists, counts hidden items and keeps selection", "[browser]") {
    TempDir t;
    populate(t);
    t.file(".config2", "y");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    CHECK(m.hidden_count() == 2);
    CHECK_FALSE(m.show_hidden());
    CHECK(m.find(".hidden") == kNoRow);
    m.click_row(m.find("gamma.txt"), Modifiers::None);

    m.execute(Command{CommandId::ToggleHidden, 0});
    settle(m);
    CHECK(m.show_hidden());
    CHECK(m.find(".hidden") != kNoRow);
    CHECK(m.hidden_count() == 2);
    CHECK(selected_names(m) == std::vector<std::string>{"gamma.txt"});
    CHECK(cursor_name(m) == "gamma.txt");

    m.toggle_hidden();
    settle(m);
    CHECK(m.find(".hidden") == kNoRow);
    CHECK(selected_names(m) == std::vector<std::string>{"gamma.txt"});
}

TEST_CASE("Hidden-only folder is empty with a hidden count", "[browser]") {
    TempDir t;
    t.file(".only", "x");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    CHECK(m.entries().empty());
    CHECK(m.hidden_count() == 1);
    CHECK_FALSE(m.listing_error());
}

TEST_CASE("Refresh keeps selection and cursor by name across new entries", "[browser]") {
    TempDir t;
    t.file("b.txt");
    t.file("d.txt");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    m.click_row(1, Modifiers::None); // d.txt
    REQUIRE(cursor_name(m) == "d.txt");
    t.file("a.txt");
    t.file("c.txt");
    m.execute(Command{CommandId::Refresh, 0});
    settle(m);
    CHECK(names(m) == std::vector<std::string>{"a.txt", "b.txt", "c.txt", "d.txt"});
    CHECK(selected_names(m) == std::vector<std::string>{"d.txt"});
    CHECK(cursor_name(m) == "d.txt");
    CHECK(m.anchor() == 3);
    CHECK_FALSE(m.can_back()); // refresh is not history
}

TEST_CASE("Sorting by a column, reversing, size and date start descending", "[browser]") {
    TempDir t;
    t.file("small.txt", "1");
    t.file("large.txt", "1234567890");
    t.file("medium.txt", "12345");
    t.dir("folder");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    m.click_row(m.find("medium.txt"), Modifiers::None);
    CHECK(names(m) == std::vector<std::string>{"folder", "large.txt", "medium.txt", "small.txt"});

    m.sort_by(fs::SortKey::Size);
    CHECK(m.sort().descending);
    settle(m);
    CHECK(names(m) == std::vector<std::string>{"folder", "large.txt", "medium.txt", "small.txt"});
    m.sort_by(fs::SortKey::Size); // reverse
    settle(m);
    CHECK_FALSE(m.sort().descending);
    CHECK(names(m) == std::vector<std::string>{"folder", "small.txt", "medium.txt", "large.txt"});
    CHECK(selected_names(m) == std::vector<std::string>{"medium.txt"});

    m.sort_by(fs::SortKey::Name);
    CHECK_FALSE(m.sort().descending);
    settle(m);
    m.sort_by(fs::SortKey::Name);
    settle(m);
    CHECK(m.sort().descending);
    CHECK(names(m) == std::vector<std::string>{"folder", "small.txt", "medium.txt", "large.txt"});
}

TEST_CASE("Activating a file calls the launcher with its absolute path", "[browser]") {
    TempDir t;
    const auto doc = t.file("Rapor ğüşİ.pdf", "%PDF");
    FakeLauncher launcher;
    BrowserModel::Options o;
    o.launcher = &launcher;
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    REQUIRE(cursor_name(m) == "Rapor ğüşİ.pdf");
    m.execute(Command{CommandId::Activate, 0});
    CHECK_FALSE(m.busy()); // a file never navigates
    REQUIRE(launcher.calls.size() == 1);
    const auto& argv = launcher.calls[0].argv;
    REQUIRE_FALSE(argv.empty());
    CHECK(argv.back() == doc.string());
    CHECK(Path(argv.back()).is_absolute());
    CHECK_FALSE(m.notice());
}

TEST_CASE("Activating a link to a folder navigates; a dangling link raises a notice", "[browser]") {
    TempDir t;
    t.dir("real/inner");
    t.link("to-real", t.path() / "real");
    t.link("broken", t.path() / "missing");
    FakeLauncher launcher;
    BrowserModel::Options o;
    o.launcher = &launcher;
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    m.click_row(m.find("to-real"), Modifiers::None);
    m.activate();
    settle(m);
    CHECK(m.current_dir() == t.path() / "to-real");
    CHECK(names(m) == std::vector<std::string>{"inner"});
    m.back();
    settle(m);
    m.click_row(m.find("broken"), Modifiers::None);
    m.activate();
    CHECK(launcher.calls.empty());
    REQUIRE(m.notice());
    CHECK(notice_of(m).kind == NoticeKind::OpenFileFailed);
    CHECK(notice_of(m).code == ErrorCode::NotFound);
}

TEST_CASE("A folder without permission raises a notice and the browser stays", "[browser]") {
    if (::geteuid() == 0) {
        SKIP("running as root: permissions are not enforced");
    }
    TempDir t;
    t.dir("locked");
    t.file("locked/secret.txt");
    t.file("open.txt");
    ::chmod((t.path() / "locked").c_str(), 0);
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    m.click_row(m.find("open.txt"), Modifiers::None);
    m.click_row(m.find("locked"), Modifiers::None);
    m.activate();
    settle(m);
    CHECK(m.current_dir() == t.path());
    CHECK(cursor_name(m) == "locked");
    CHECK_FALSE(m.can_back()); // history unchanged
    REQUIRE(m.notice());
    CHECK(notice_of(m).code == ErrorCode::PermissionDenied);
    CHECK(notice_of(m).kind == NoticeKind::OpenFolderFailed);
    CHECK(notice_of(m).path == t.path() / "locked");
    CHECK(m.entries().size() == 2);

    // Refreshing keeps it; dismissing clears it; a successful navigation would too.
    m.navigate_to(t.path() / ".");
    settle(m);
    CHECK(m.notice());
    m.dismiss_notice();
    CHECK_FALSE(m.notice());
}

TEST_CASE("A start path that cannot be listed opens empty with a notice", "[browser]") {
    TempDir t;
    BrowserModel m(no_watch());
    m.open_start(t.path() / "does-not-exist");
    settle(m);
    CHECK(m.loaded());
    CHECK(m.current_dir() == t.path() / "does-not-exist");
    CHECK(m.entries().empty());
    REQUIRE(m.listing_error());
    CHECK(error_of(m).code == ErrorCode::NotFound);
    REQUIRE(m.notice());
    CHECK(notice_of(m).code == ErrorCode::NotFound);
    // Up still works from there.
    REQUIRE(m.can_up());
    m.up();
    settle(m);
    CHECK(m.current_dir() == t.path());
    CHECK_FALSE(m.listing_error());
}

TEST_CASE("Watch batches patch the list in sorted position", "[browser][watch]") {
    TempDir t;
    t.dir("dir1");
    t.file("b.txt", "bb");
    t.file("d.txt", "dddd");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    m.click_row(m.find("d.txt"), Modifiers::None);
    REQUIRE(m.cursor() == 2);

    const auto entry = [&](const std::string& name, std::uint64_t size, bool dir = false) {
        fs::DirEntry e;
        e.name = name;
        e.kind = dir ? fs::EntryKind::Directory : fs::EntryKind::File;
        e.size = size;
        e.hidden = fs::is_hidden_name(name);
        return e;
    };
    WatchBatch batch;
    batch.changes.push_back({WatchChange::Kind::Upsert, "c.txt", {}, entry("c.txt", 3)});
    batch.changes.push_back({WatchChange::Kind::Upsert, "a-dir", {}, entry("a-dir", 0, true)});
    batch.changes.push_back({WatchChange::Kind::Upsert, ".dot", {}, entry(".dot", 1)});
    CHECK(m.apply_watch_batch(batch));
    CHECK(names(m) == std::vector<std::string>{"a-dir", "dir1", "b.txt", "c.txt", "d.txt"});
    CHECK(cursor_name(m) == "d.txt");
    CHECK(selected_names(m) == std::vector<std::string>{"d.txt"});
    CHECK(m.hidden_count() == 1);

    WatchBatch rename;
    rename.changes.push_back({WatchChange::Kind::Rename, "a.txt", "d.txt", entry("a.txt", 4)});
    CHECK(m.apply_watch_batch(rename));
    CHECK(names(m) == std::vector<std::string>{"a-dir", "dir1", "a.txt", "b.txt", "c.txt"});
    CHECK(cursor_name(m) == "a.txt");
    CHECK(selected_names(m) == std::vector<std::string>{"a.txt"});
    CHECK(m.selected_bytes() == 4U);

    WatchBatch modify; // size change of a selected file updates the total
    modify.changes.push_back({WatchChange::Kind::Upsert, "a.txt", {}, entry("a.txt", 40)});
    CHECK(m.apply_watch_batch(modify));
    CHECK(m.selected_bytes() == 40U);
    CHECK(m.entries().size() == 5);

    WatchBatch remove;
    remove.changes.push_back({WatchChange::Kind::Remove, "a.txt", {}, {}});
    remove.changes.push_back({WatchChange::Kind::Remove, ".dot", {}, {}});
    CHECK(m.apply_watch_batch(remove));
    CHECK(names(m) == std::vector<std::string>{"a-dir", "dir1", "b.txt", "c.txt"});
    CHECK(cursor_name(m) == "b.txt"); // the next row takes the cursor
    CHECK(m.selected_count() == 0);
    CHECK(m.hidden_count() == 0);

    WatchBatch gone;
    gone.gone = true;
    CHECK(m.apply_watch_batch(gone));
    REQUIRE(m.notice());
    CHECK(notice_of(m).kind == NoticeKind::FolderGone);
    CHECK(m.entries().size() == 4); // stale list stays

    WatchBatch overflow;
    overflow.overflow = true;
    (void)m.apply_watch_batch(overflow);
    CHECK(m.busy()); // re-list requested
    settle(m);
    CHECK(names(m) == std::vector<std::string>{"dir1", "b.txt", "d.txt"});
}

TEST_CASE("Live inotify updates reach the list at frame start", "[browser][watch]") {
    TempDir t;
    t.file("keep.txt");
    BrowserModel::Options o;
    o.watcher_factory = [] { return create_inotify_watcher(); };
    BrowserModel m(std::move(o));
    m.open_start(t.path());
    settle(m);
    REQUIRE(m.wait_watch_ready(kWait));

    t.file("new file.txt", "hello");
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (m.find("new file.txt") == kNoRow && std::chrono::steady_clock::now() < deadline) {
        (void)m.wait_watch_batch(std::chrono::milliseconds(200));
        (void)m.frame_start();
    }
    REQUIRE(m.find("new file.txt") != kNoRow);
    CHECK(m.entries()[m.find("new file.txt")].size == 5U);

    std::filesystem::rename(t.path() / "keep.txt", t.path() / "zz renamed.txt");
    const auto deadline2 = std::chrono::steady_clock::now() + kWait;
    while (m.find("zz renamed.txt") == kNoRow && std::chrono::steady_clock::now() < deadline2) {
        (void)m.wait_watch_batch(std::chrono::milliseconds(200));
        (void)m.frame_start();
    }
    CHECK(m.find("zz renamed.txt") != kNoRow);
    CHECK(m.find("keep.txt") == kNoRow);
}

TEST_CASE("A navigation superseded by another only commits the newest", "[browser]") {
    TempDir t;
    t.dir("one");
    t.dir("two");
    BrowserModel m(no_watch());
    m.open_start(t.path());
    settle(m);
    m.navigate_to(t.path() / "one");
    m.navigate_to(t.path() / "two");
    settle(m);
    // A late result for "one" (if any) is dropped.
    (void)m.frame_start();
    CHECK(m.current_dir() == t.path() / "two");
    CHECK(m.back_stack().size() == 1);
}

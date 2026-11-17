// Phase 4 text fields and path entry: TextEdit editing and focus-scoped keys, the GoTo list
// (sections, typed row, completions), Enter outcomes ("~/Downloads" and "/" navigate; files
// reveal; missing paths keep the text with an error), address bar highlight rules, and the
// Copy as Path text. Synchronous (no worker attached); files only inside mkdtemp fixtures.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/i18n.hpp>

#include "browser/browser_model.hpp"
#include "path/copy_path.hpp"
#include "path/path_entry.hpp"
#include "ui_fixture.hpp"
#include "widgets/text_edit.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::TempDir;

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// The outcome after a REQUIRE(o.has_value()) (tidy's optional check does not see REQUIRE).
PathOutcome got(const std::optional<PathOutcome>& o) { return o.value_or(PathOutcome{}); }

void type(PathEntry& e, std::string_view s, double now = 0.0) {
    e.edit().set(s);
    e.changed(now);
    e.tick(now + PathEntry::kDebounceMs + 1.0); // synchronous completion without a worker
}

} // namespace

TEST_CASE("TextEdit: insert, caret moves, selection, words, undo", "[text_edit]") {
    TextEdit e;
    e.set("~/Music/Albums");
    CHECK(e.caret() == e.text().size());
    CHECK(e.backspace(true));
    CHECK(e.text() == "~/Music/");
    CHECK(e.undo());
    CHECK(e.text() == "~/Music/Albums");
    e.home(false);
    e.move_right(true, true); // select "~"
    CHECK(e.selected_text() == "~");
    CHECK(e.insert("/home/u"));
    CHECK(e.text() == "/home/u/Music/Albums");
    e.select_all();
    CHECK(e.cut() == "/home/u/Music/Albums");
    CHECK(e.empty());
    // UTF-8: one code point per step; control characters and bad bytes are dropped.
    CHECK(e.insert("İndirilenler\n"));
    CHECK(e.text() == "İndirilenler");
    e.home(false);
    e.move_right(false);
    CHECK(e.caret() == 2); // "İ" is two bytes
    CHECK(e.insert(std::string("\xFF") + "x"));
    CHECK(e.text() == "İxndirilenler");
    TextEdit small(4);
    small.set("abcdef");
    CHECK(small.text() == "abcd");
    CHECK_FALSE(small.insert("x"));
}

TEST_CASE("Focused fields take editing keys before the keymap", "[text_edit]") {
    TextEdit e;
    e.set("abc", true);
    FieldKeyResult r = apply_field_key(e, KeyChord{Key::Backspace, Modifiers::None});
    CHECK(r.handled);
    CHECK(r.changed);
    CHECK(e.empty());
    r = apply_field_key(e, KeyChord{Key::V, Modifiers::Ctrl}, [] { return std::string("/tmp"); });
    CHECK(e.text() == "/tmp");
    r = apply_field_key(e, KeyChord{Key::A, Modifiers::Ctrl});
    CHECK(r.handled);
    r = apply_field_key(e, KeyChord{Key::C, Modifiers::Ctrl});
    REQUIRE(r.clipboard.has_value());
    CHECK(r.clipboard.value_or("") == "/tmp");
    CHECK(apply_field_key(e, KeyChord{Key::Tab, Modifiers::None}).tab);
    // Not editing keys: they resolve through the keymap.
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::Enter, Modifiers::None}).handled);
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::Escape, Modifiers::None}).handled);
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::Up, Modifiers::None}).handled);
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::F6, Modifiers::None}).handled);
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::Left, Modifiers::Alt}).handled); // nav.back
    CHECK_FALSE(apply_field_key(e, KeyChord{Key::Q, Modifiers::Ctrl}).handled);
}

TEST_CASE("GoTo: ~/Downloads and / resolve to folders and navigate", "[goto]") {
    TempDir t;
    const Path home = t.dir("home");
    t.dir("home/Downloads");
    t.dir("home/Documents");
    PathEntry e(PathEntryMode::GoTo);
    e.open({}, false, home, home, {}, 0.0);
    type(e, "~/Downloads");
    REQUIRE_FALSE(e.items().empty());
    CHECK(e.items()[0].kind == PathItem::Kind::Typed);
    REQUIRE(e.preview().has_value());
    CHECK(e.preview().value_or(path_input::ExpandedPath{}).path == home / "Downloads");
    CHECK(e.highlight() == 0);
    e.submit(false);
    std::optional<PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).kind == PathOutcome::Kind::Navigate);
    CHECK(got(o).path == home / "Downloads");

    type(e, "/");
    REQUIRE_FALSE(e.items().empty());
    CHECK(e.items()[0].kind == PathItem::Kind::Typed); // not the first child of "/"
    e.submit(false);
    o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).path == Path("/"));

    // Case auto-correction (Enter on the Typed row; by default the "Downloads" completion is
    // highlighted, which goes to the same folder).
    type(e, "~/downloads");
    REQUIRE(e.items()[e.highlight()].path == home / "Downloads");
    e.set_highlight(0);
    e.submit(true);
    o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).path == home / "Downloads");
    CHECK(got(o).case_corrected);
    CHECK(got(o).new_tab);
}

TEST_CASE("GoTo: completions, Tab and the empty-text sources", "[goto]") {
    TempDir t;
    const Path home = t.dir("home");
    t.dir("home/Downloads");
    t.dir("home/Documents");
    t.file("home/notes.txt");
    std::vector<GotoSource> sources;
    GotoSource r;
    r.kind = GotoSource::Kind::Recent;
    r.label = "Documents";
    r.path = home / "Documents";
    sources.push_back(r);
    GotoSource p;
    p.kind = GotoSource::Kind::Place;
    p.label = "Home";
    p.path = home;
    sources.push_back(p);
    PathEntry e(PathEntryMode::GoTo);
    e.open({}, false, home, home, sources, 0.0);
    REQUIRE(e.items().size() == 2); // empty text: recents, then places & bookmarks
    CHECK(e.items()[0].section == PathItem::Section::Recents);
    CHECK(e.items()[1].section == PathItem::Section::Places);

    type(e, "~/Do");
    std::vector<std::string> names;
    for (const PathItem& it : e.items()) {
        if (it.kind == PathItem::Kind::Completion) {
            names.push_back(it.label);
        }
    }
    CHECK(names == std::vector<std::string>{"Documents", "Downloads"}); // folders only
    CHECK(e.accept_completion(10.0));
    CHECK(e.edit().text() == "~/Documents/");

    // Plain text searches the sources by label.
    type(e, "home");
    bool found = false;
    for (const PathItem& it : e.items()) {
        found = found || (it.kind == PathItem::Kind::Place && it.label == "Home");
    }
    CHECK(found);
}

TEST_CASE("Enter on a missing path keeps the text and shows an error", "[goto]") {
    set_language(Language::English);
    TempDir t;
    const Path home = t.dir("home");
    PathEntry e(PathEntryMode::GoTo);
    e.open({}, false, home, home, {}, 0.0);
    type(e, "$NAVIGATOR_TEST_SURELY_UNSET/x");
    e.submit(false);
    CHECK_FALSE(e.take_outcome().has_value());
    // Only the root cause, once (design review O2).
    CHECK(e.error() == "Unknown variable $NAVIGATOR_TEST_SURELY_UNSET");
    CHECK(e.edit().text() == "$NAVIGATOR_TEST_SURELY_UNSET/x");
    // Editing clears the error.
    e.edit().insert("y");
    e.changed(0.0);
    CHECK(e.error().empty());
}

TEST_CASE("GoTo: a partial path highlights its first completion; exact names stay typed",
          "[goto]") {
    TempDir t;
    const Path home = t.dir("home");
    t.dir("home/Belgeler");
    t.dir("home/Downloads");
    PathEntry e(PathEntryMode::GoTo);
    e.open({}, false, home, home, {}, 0.0);
    type(e, "~/B"); // does not exist, completes uniquely
    REQUIRE(e.highlight() < e.items().size());
    CHECK(e.items()[e.highlight()].kind == PathItem::Kind::Completion);
    e.submit(false);
    std::optional<PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).path == home / "Belgeler");
    type(e, "~/Downloads"); // exists as typed: the Typed row stays highlighted
    CHECK(e.highlight() == 0);
    type(e, "~/"); // a folder: Typed
    CHECK(e.highlight() == 0);
    // Moving the highlight by hand is kept when completions arrive.
    type(e, "~/B");
    e.move(-1);
    CHECK(e.highlight() == 0);
}

TEST_CASE("Enter on a file reveals it in its folder; Ctrl+Enter opens it", "[goto]") {
    TempDir t;
    const Path home = t.dir("home");
    t.file("home/a.txt", "x");
    PathEntry e(PathEntryMode::AddressBar);
    e.open((home / "a.txt").string(), true, home, home, {}, 0.0);
    e.submit(false);
    std::optional<PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).kind == PathOutcome::Kind::RevealFile);
    CHECK(got(o).path == home);
    CHECK(got(o).cursor_name == "a.txt");
    e.submit(true);
    o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).kind == PathOutcome::Kind::OpenFile);
}

TEST_CASE("Address bar: no dropdown until edited; highlight starts at none", "[goto]") {
    TempDir t;
    const Path home = t.dir("home");
    t.dir("home/Music");
    t.dir("home/Movies");
    PathEntry e(PathEntryMode::AddressBar);
    e.open(home.string(), true, home, home, {}, 0.0);
    e.tick(PathEntry::kDebounceMs + 1.0);
    CHECK(e.items().empty());
    CHECK(e.highlight() == kNpos);
    type(e, (home / "M").string());
    CHECK(e.items().size() == 2);
    CHECK(e.highlight() == kNpos);
    e.move(1);
    CHECK(e.highlight() == 0);
    e.move(-1);
    CHECK(e.highlight() == kNpos);
    e.move(1);
    e.submit(false);
    const std::optional<PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).path == home / e.items()[0].label);
}

TEST_CASE("looks_like_path and display_path", "[goto]") {
    CHECK(looks_like_path("/"));
    CHECK(looks_like_path("~"));
    CHECK(looks_like_path("$HOME"));
    CHECK(looks_like_path("a/b"));
    CHECK_FALSE(looks_like_path("downloads"));
    CHECK(display_path("/home/u/Music", "/home/u") == "~/Music");
    CHECK(display_path("/home/u", "/home/u") == "~");
    CHECK(display_path("/usr", "/home/u") == "/usr");
}

TEST_CASE("Copy as Path: selection in list order, else the current folder", "[copy_path]") {
    TempDir t;
    t.file("b c.txt");
    t.file("a.txt");
    BrowserModel m(BrowserModel::Options{});
    m.open_start(t.path());
    REQUIRE(m.wait_listing(std::chrono::milliseconds(5000)));
    (void)m.frame_start();
    REQUIRE(m.loaded());
    std::vector<Path> p = copy_path_targets(m, false);
    REQUIRE(p.size() == 1);
    CHECK(p[0] == t.path());
    CHECK(copy_path_text(p) == t.path().string());
    m.select_all();
    p = copy_path_targets(m, false);
    REQUIRE(p.size() == 2);
    CHECK(p[0].filename() == "a.txt");
    CHECK(copy_path_text(p) ==
          (t.path() / "a.txt").string() + " '" + (t.path() / "b c.txt").string() + "'");
    CHECK(copy_path_targets(m, true).size() == 1); // CopyPathArg::CurrentFolder
}

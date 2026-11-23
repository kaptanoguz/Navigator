// Phase 5 folder filter: type-to-filter and pass-through rules, the Escape ladders, the rows of
// BrowserModel (inline and worker), type filter persistence, highlight mapping.
#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/i18n.hpp>

#include "path/copy_path.hpp"
#include "search/filter_rules.hpp"
#include "search/highlight_map.hpp"
#include "search_test_util.hpp"
#include "ui_fixture.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::ui;
using navigator::test::cursor_of;
using navigator::test::kSearchWait;
using navigator::test::make_entry;
using navigator::test::open_listed;
using navigator::test::rows_of;
using navigator::test::TempDir;
using Names = std::vector<std::string>;

namespace {

void populate(const TempDir& t) {
    t.dir("reports/inner");
    t.file("alpha.txt", "1");
    t.file("report-a.pdf", "22");
    t.file("report-b.txt", "333");
    t.file("İstanbul.jpg", "4444");
    t.file(".secret-report", "x");
}

WatchBatch upsert(const std::string& name) {
    WatchChange c;
    c.kind = WatchChange::Kind::Upsert;
    c.name = name;
    c.entry = make_entry(name);
    WatchBatch b;
    b.changes.push_back(std::move(c));
    return b;
}

WatchBatch removal(const std::string& name) {
    WatchChange c;
    c.kind = WatchChange::Kind::Remove;
    c.name = name;
    WatchBatch b;
    b.changes.push_back(std::move(c));
    return b;
}

} // namespace

TEST_CASE("Type-to-filter starts only from a focused list", "[filter][keys]") {
    const Modifiers none = Modifiers::None;
    CHECK(starts_type_to_filter(KeyboardOwner::List, none, "r"));
    CHECK(starts_type_to_filter(KeyboardOwner::List, Modifiers::Shift, "R"));
    CHECK(starts_type_to_filter(KeyboardOwner::List, none, "ş"));
    CHECK(starts_type_to_filter(KeyboardOwner::List, none, "a b"));
    for (const KeyboardOwner o :
         {KeyboardOwner::Sidebar, KeyboardOwner::TextField, KeyboardOwner::Popup}) {
        CHECK_FALSE(starts_type_to_filter(o, none, "r"));
    }
    for (const Modifiers m : {Modifiers::Ctrl, Modifiers::Alt, Modifiers::Super}) {
        CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, m, "r"));
    }
    CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, none, " ")); // Inspector (Phase 6)
    CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, none, " r"));
    CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, none, ""));
    CHECK_FALSE(starts_type_to_filter(KeyboardOwner::List, none, "\t"));
}

TEST_CASE("The filter field takes its editing keys and nothing else", "[filter][keys]") {
    const auto route = [](const char* chord, bool selection = false) {
        const Result<KeyChord> c = parse_key_chord(chord);
        REQUIRE(c);
        return route_filter_key(*c, selection);
    };
    for (const char* k : {"Left", "Right", "Home", "End", "Shift+Left", "Ctrl+Right", "Delete",
                          "Ctrl+Shift+Left", "Backspace", "Ctrl+Backspace", "Ctrl+V", "Ctrl+Z"}) {
        INFO(k);
        CHECK(route(k) == FilterKey::Edit);
    }
    for (const char* k : {"Up", "Down", "Shift+Up", "Shift+Down", "PageUp", "PageDown", "Enter",
                          "KeypadEnter", "Ctrl+Enter", "Ctrl+A", "Ctrl+E", "Ctrl+Shift+E",
                          "Ctrl+Shift+F", "Ctrl+W", "F5", "F6", "Ctrl+Q", "Alt+Left"}) {
        INFO(k);
        CHECK(route(k) == FilterKey::Keymap);
    }
    CHECK(route("Escape") == FilterKey::Escape);
    CHECK(route("Tab") == FilterKey::ToList);
    // Copy and cut are the field's only with a text selection.
    CHECK(route("Ctrl+C") == FilterKey::Keymap);
    CHECK(route("Ctrl+C", true) == FilterKey::Edit);
    CHECK(route("Ctrl+X", true) == FilterKey::Edit);
}

TEST_CASE("Keys the field passes through act on the filtered rows", "[filter][keys]") {
    TempDir t;
    populate(t);
    Keymap keymap;
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    REQUIRE(load_default_keymap(keymap));
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    m.set_filter_text("report");
    REQUIRE(rows_of(m) == Names{"reports", "report-a.pdf", "report-b.txt"});
    const auto press = [&](const char* chord) {
        const Result<KeyChord> c = parse_key_chord(chord);
        REQUIRE(c);
        REQUIRE(route_filter_key(*c, false) == FilterKey::Keymap);
        const auto command = resolve_command(keymap, registry, *c);
        REQUIRE(command);
        CHECK(m.execute(*command));
    };
    CHECK(cursor_of(m) == "reports");
    press("Down");
    CHECK(cursor_of(m) == "report-a.pdf");
    press("Ctrl+A"); // the shown rows, not the text and not the whole folder
    CHECK(m.selected_count() == 3);
    CHECK(copy_path_targets(m, false).size() == 3);
    press("Up");
    press("Enter");
    REQUIRE(m.wait_listing(kSearchWait));
    (void)m.frame_start();
    CHECK(m.current_dir() == t.path() / "reports");
    CHECK_FALSE(m.filter_active());
    CHECK(rows_of(m) == Names{"inner"});
}

TEST_CASE("Escape ladders run innermost first", "[filter][escape]") {
    CHECK(filter_field_escape(true, true) == FieldEscape::CancelSearch);
    CHECK(filter_field_escape(true, false) == FieldEscape::CancelSearch);
    CHECK(filter_field_escape(false, true) == FieldEscape::ClearText);
    CHECK(filter_field_escape(false, false) == FieldEscape::ReturnFocus);

    using E = ListEscape;
    CHECK(list_escape_step(true, true, true, true, true) == E::DismissNotice);
    CHECK(list_escape_step(false, true, true, true, true) == E::CancelSearch);
    CHECK(list_escape_step(false, false, true, true, true) == E::ClearSelection);
    CHECK(list_escape_step(false, false, false, true, true) == E::LeaveFlatten);
    CHECK(list_escape_step(false, false, false, false, true) == E::ClearFilter);
    CHECK(list_escape_step(false, false, false, false, false) == E::Nothing);
}

TEST_CASE("The filter keeps list order and clearing keeps the cursor's entry", "[filter]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    const Names all = rows_of(m);
    CHECK(m.source_count() == 5); // the hidden policy applies first

    m.set_filter_text("istanbul");
    CHECK(rows_of(m) == Names{"İstanbul.jpg"});
    m.set_filter_text("REPORT");
    CHECK(rows_of(m) == Names{"reports", "report-a.pdf", "report-b.txt"});
    CHECK(m.source_count() == 5);
    m.set_filter_text("report b");
    CHECK(rows_of(m) == Names{"report-b.txt"});
    m.clear_filter();
    CHECK(rows_of(m) == all);
    CHECK(cursor_of(m) == "report-b.txt");
    m.set_filter_text("zzz");
    CHECK(m.row_count() == 0);
    CHECK(m.cursor() == kNoRow);
    CHECK_FALSE(m.rows_pending());
}

TEST_CASE("Entries the filter hides are deselected", "[filter]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    m.select_all();
    REQUIRE(m.selected_count() == 5);
    m.set_filter_text("report");
    CHECK(m.selected_count() == 3);
    m.clear_filter();
    CHECK(m.selected_count() == 3);
    CHECK_FALSE(m.is_selected(m.find_row("alpha.txt")));
}

TEST_CASE("Escape in the list clears the selection, then the filter text", "[filter][escape]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    search::TypeFilter types;
    types.hide_no_extension = true;
    m.set_type_filter(types);
    m.set_filter_text("report");
    m.select_all();
    m.escape();
    CHECK(m.selected_count() == 0);
    CHECK(m.filter_active());
    m.escape();
    CHECK_FALSE(m.filter_active());
    m.escape();
    CHECK(m.type_filter().active()); // a persisted view setting: never Escape's
}

TEST_CASE("Every kind of navigation clears the filter text, not the type filter", "[filter]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    search::TypeFilter types;
    types.hidden_extensions = {"pdf"};
    m.set_type_filter(types);
    const auto settle = [&] {
        REQUIRE(m.wait_listing(kSearchWait));
        (void)m.frame_start();
        CHECK_FALSE(m.filter_active());
        CHECK(m.type_filter() == types);
    };
    m.set_filter_text("r");
    m.navigate_to(t.path() / "reports");
    settle();
    m.set_filter_text("r");
    m.back();
    settle();
    CHECK(cursor_of(m) == "reports");
    CHECK(m.find_row("report-a.pdf") == kNoRow);
    m.set_filter_text("r");
    m.forward();
    settle();
    m.set_filter_text("r");
    m.up();
    settle();
}

TEST_CASE("A re-list keeps the filter, the cursor and the selection", "[filter]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    m.set_filter_text("report");
    m.click_row(2, Modifiers::None);
    m.refresh();
    REQUIRE(m.wait_listing(kSearchWait));
    (void)m.frame_start();
    CHECK(m.filter_active());
    CHECK(cursor_of(m) == "report-b.txt");
    CHECK(m.selected_count() == 1);
}

TEST_CASE("Watch changes patch filtered rows in place", "[filter][watch]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    m.set_filter_text("report");
    m.click_row(2, Modifiers::None); // report-b.txt
    CHECK(m.apply_watch_batch(upsert("report-aa.txt")));
    CHECK(rows_of(m) == Names{"reports", "report-a.pdf", "report-aa.txt", "report-b.txt"});
    CHECK(cursor_of(m) == "report-b.txt");
    CHECK(m.selected_count() == 1);
    CHECK(m.apply_watch_batch(upsert("aaa.txt")));
    CHECK(m.row_count() == 4);
    CHECK(m.source_count() == 7);
    CHECK(m.apply_watch_batch(removal("report-a.pdf")));
    CHECK(rows_of(m) == Names{"reports", "report-aa.txt", "report-b.txt"});
    CHECK(cursor_of(m) == "report-b.txt");
}

TEST_CASE("Listings above the limit filter on the worker", "[filter][worker]") {
    TempDir t;
    populate(t);
    BrowserModel::Options o;
    o.ui_filter_limit = 2;
    BrowserModel m(std::move(o));
    open_listed(m, t.path());
    m.set_filter_text("rep");
    CHECK(m.searching());
    CHECK(m.row_count() == 5); // the old rows stay until the answer
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(rows_of(m) == Names{"reports", "report-a.pdf", "report-b.txt"});
    // Only the newest of several keystrokes lands.
    m.set_filter_text("alp");
    m.set_filter_text("istanbul");
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(rows_of(m) == Names{"İstanbul.jpg"});
    m.set_filter_text("a");
    m.clear_filter();
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.row_count() == 5);
}

TEST_CASE("The match mode toggles between substring and fuzzy", "[filter]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    CHECK(m.match_mode() == search::MatchMode::Substring);
    m.set_filter_text("rptb");
    CHECK(m.row_count() == 0);
    CHECK(m.execute(Command{CommandId::ToggleFuzzy, 0}));
    CHECK(rows_of(m) == Names{"report-b.txt"});
}

TEST_CASE("Types popup rows respect the hidden policy and toggle", "[filter][types]") {
    TempDir t;
    populate(t);
    BrowserModel m(BrowserModel::Options{});
    open_listed(m, t.path());
    const std::vector<search::TypeCount> types = m.type_summary();
    REQUIRE(types.size() == 4); // folder, txt, jpg, pdf; no row for the hidden file
    CHECK(types[0].key.kind == search::TypeKind::Folder);
    CHECK(types[1].key.extension == "txt");
    CHECK(types[1].count == 2);
    CHECK(types[2].key.extension == "jpg");
    CHECK(types[3].key.extension == "pdf");

    search::TypeFilter f = m.type_filter();
    search::set_visible(f, types[1].key, false);
    m.set_type_filter(f);
    CHECK(rows_of(m) == Names{"reports", "İstanbul.jpg", "report-a.pdf"});
    CHECK(m.type_summary().size() == 4); // hidden types stay in the popup
    CHECK_FALSE(m.type_summary()[1].visible);

    search::toggle_all(f, m.type_summary()); // some hidden: show all
    m.set_type_filter(f);
    CHECK(m.row_count() == 5);
    search::toggle_all(f, m.type_summary()); // all shown: hide all
    m.set_type_filter(f);
    CHECK(m.row_count() == 0);
}

TEST_CASE("The type filter persists per tab, the filter text never", "[filter][types][workspace]") {
    TempDir t;
    populate(t);
    Workspace::Options o;
    o.home = t.path();
    Workspace ws(std::move(o));
    ws.start(std::nullopt, t.path());
    REQUIRE(ws.active_model().wait_listing(kSearchWait));
    (void)ws.frame_start();
    (void)ws.sync();
    const std::uint64_t before = ws.persist_serial();

    ws.active_model().set_filter_text("report");
    CHECK_FALSE(ws.sync()); // runtime only
    search::TypeFilter f;
    f.hidden_extensions = {"pdf"};
    ws.active_model().set_type_filter(f);
    CHECK(ws.sync());
    CHECK(ws.persist_serial() > before);
    const layout::TabState* state = ws.layout().tab_state(ws.active_tab());
    REQUIRE(state != nullptr);
    CHECK(state->type_filter == f);

    REQUIRE(ws.execute(Command{CommandId::DuplicateTab, 0}));
    CHECK(ws.active_model().type_filter() == f);
    CHECK_FALSE(ws.active_model().filter_active());
    REQUIRE(ws.execute(Command{CommandId::NewTab, NewTabArg::None}));
    CHECK(ws.active_model().type_filter() == f);
}

TEST_CASE("A narrow title bar collapses an idle filter before the breadcrumbs", "[filter]") {
    CHECK(layout_filter_bar(800.0F, 300.0F, false) == FilterBarLayout{kFilterFieldIdeal, false});
    CHECK(layout_filter_bar(300.0F, 200.0F, false) == FilterBarLayout{kFilterFieldMin, false});
    CHECK(layout_filter_bar(220.0F, 200.0F, false) == FilterBarLayout{kFilterCollapsed, true});
    // An active filter never collapses.
    CHECK(layout_filter_bar(220.0F, 200.0F, true) == FilterBarLayout{kFilterFieldMin, false});
    CHECK(layout_filter_bar(120.0F, 200.0F, true) == FilterBarLayout{120.0F, false});
}

TEST_CASE("The search scope chip names the scope and survives typing", "[filter][scope]") {
    CHECK(search_scope(false, false) == SearchScope::Folder);
    CHECK(search_scope(false, true) == SearchScope::Folder);
    CHECK(search_scope(true, false) == SearchScope::Subfolders);
    CHECK(search_scope(true, true) == SearchScope::Drive);
    CHECK(scope_label_key(SearchScope::Folder).empty());
    for (const SearchScope s : {SearchScope::Subfolders, SearchScope::Drive}) {
        for (const Language lang : {Language::English, Language::Turkish}) {
            set_language(lang);
            CHECK(tr(scope_label_key(s)) != scope_label_key(s));
        }
    }
    set_language(Language::English);
    CHECK(tr(scope_label_key(SearchScope::Drive)) == "Drive");
}

TEST_CASE("A narrow title bar hides idle toggles before it squeezes the folder name", "[filter]") {
    CHECK(fit_title_buttons(400.0F, 30.0F, false, false, false, false) == TitleButtons{});
    // 1366px thirds next to the sidebar keep everything while the filter is idle.
    CHECK(fit_title_buttons(264.0F, 30.0F, false, false, false, false) == TitleButtons{});
    CHECK(fit_title_buttons(180.0F, 30.0F, false, false, false, false) ==
          TitleButtons{false, true, true});
    CHECK(fit_title_buttons(150.0F, 30.0F, false, false, false, false) ==
          TitleButtons{false, false, true});
    CHECK(fit_title_buttons(264.0F, 30.0F, true, false, false, false) ==
          TitleButtons{false, true, false});
    CHECK(fit_title_buttons(120.0F, 30.0F, false, false, false, false) ==
          TitleButtons{false, false, false});
    // An active state never hides.
    CHECK(fit_title_buttons(120.0F, 30.0F, true, true, true, true) == TitleButtons{});
}

TEST_CASE("Match ranges follow replacement characters and elision", "[filter][highlight]") {
    const std::string bad = std::string("\xFF") + "ab";
    const std::string shown = std::string("\xEF\xBF\xBD") + "ab";
    CHECK(to_display_ranges(bad, shown, {{1, 3}}) == ByteRanges{{3, 5}});
    CHECK(to_display_ranges("abc", "abc", {{0, 2}}) == ByteRanges{{0, 2}});

    const std::string display = "abcdefghij";
    const std::string cut = "abc\xE2\x80\xA6hij";
    CHECK(to_shown_ranges(display, {}, {{4, 5}}) == ByteRanges{{4, 5}});
    CHECK(to_shown_ranges(display, cut, {{0, 2}}) == ByteRanges{{0, 2}});
    CHECK(to_shown_ranges(display, cut, {{4, 6}}) == ByteRanges{{3, 6}}); // lights the ellipsis
    CHECK(to_shown_ranges(display, cut, {{8, 10}}) == ByteRanges{{7, 9}});
    CHECK(to_shown_ranges(display, cut, {{2, 8}}) == ByteRanges{{2, 7}});
}

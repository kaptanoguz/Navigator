// ui::Workspace on fixture directories: session restore -> panes, tabs and BrowserModels
// (only active tabs live), tab/pane commands, closed tabs and reopen, duplicate, open in new
// tab, named layouts, `navigator DIR` with a session, and the background session writer.
// Files are created only inside mkdtemp fixtures (ui_fixture.hpp).
#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>
#include <navigator/layout.hpp>

#include "ui_fixture.hpp"
#include "workspace/session_writer.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::ui;
using layout::PaneId;
using layout::TabId;
using layout::TabState;
using navigator::test::TempDir;

namespace {

constexpr std::chrono::milliseconds kWait{5000};

Workspace::Options options(const TempDir& t, const Path& config = {}) {
    Workspace::Options o;
    o.home = t.path();
    o.row_dp = 24.0F;
    o.config_root = config;
    return o; // no watcher, no launcher
}

void settle(Workspace& ws) {
    for (int i = 0; i < 4; ++i) {
        for (const TabId tab : ws.visible_tabs()) {
            REQUIRE(ws.model(tab)->wait_listing(kWait));
        }
        (void)ws.frame_start();
    }
    REQUIRE(ws.settled());
}

TabState state(const Path& p) {
    TabState s;
    s.path = p;
    return s;
}

std::vector<std::string> pane_paths(const Workspace& ws, std::size_t pane_index) {
    std::vector<std::string> out;
    const std::vector<PaneId> order = ws.layout().pane_order();
    if (pane_index >= order.size()) {
        return out;
    }
    for (const layout::Tab& t : ws.layout().find_pane(order[pane_index])->tabs) {
        out.push_back(t.state.path.filename().string());
    }
    return out;
}

std::size_t live_models(const Workspace& ws) {
    std::size_t n = 0;
    for (const PaneId p : ws.layout().pane_order()) {
        for (const layout::Tab& t : ws.layout().find_pane(p)->tabs) {
            if (!ws.model(t.id)->suspended()) {
                ++n;
            }
        }
    }
    return n;
}

void populate(const TempDir& t) {
    t.file("a/one.txt", "1");
    t.dir("a/sub");
    t.file("b/two.txt", "22");
    t.file("c/three.txt", "333");
    t.dir("d");
}

} // namespace

TEST_CASE("Fresh start: one tab on the start folder", "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    CHECK(ws.layout().tab_count() == 1);
    CHECK(ws.active_model().current_dir() == t.path() / "a");
    CHECK(ws.active_model().entries().size() == 2);

    Workspace home(options(t));
    home.start(std::nullopt, Path());
    settle(home);
    CHECK(home.active_model().current_dir() == t.path());
}

TEST_CASE("Session restore wires panes, tabs and lazily live models", "[workspace]") {
    TempDir t;
    populate(t);
    // Session: [a, b*] | [c* (cursor three.txt), d]
    layout::Session s;
    s.layout = layout::Layout(state(t.path() / "a"));
    const PaneId p1 = s.layout.active_pane();
    const TabId tb = *s.layout.add_tab(p1, state(t.path() / "b"), 1, true);
    TabState c = state(t.path() / "c");
    c.cursor_name = "three.txt";
    c.show_hidden = true;
    const layout::SplitResult r = *s.layout.split_pane(p1, layout::SplitEdge::Right, c);
    (void)s.layout.add_tab(r.pane, state(t.path() / "d"), 1, false);
    s.closed_tabs.push(layout::ClosedTab{state(t.path() / "a" / "sub"), PaneId{}, 0});
    // Through JSON, as on disk: ids are fresh after loading.
    const Result<std::string> json = layout::to_json(s);
    REQUIRE(json);
    Result<layout::Session> loaded = layout::session_from_json(*json);
    REQUIRE(loaded);

    Workspace ws(options(t));
    ws.start(std::move(*loaded), Path());
    settle(ws);
    REQUIRE(ws.layout().pane_count() == 2);
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "b"});
    CHECK(pane_paths(ws, 1) == std::vector<std::string>{"c", "d"});
    CHECK(ws.closed_tabs().size() == 1);
    // One live model per pane; background tabs exist but are suspended and unlisted.
    CHECK(live_models(ws) == 2);
    const std::vector<TabId> visible = ws.visible_tabs();
    REQUIRE(visible.size() == 2);
    CHECK(ws.model(visible[0])->current_dir() == t.path() / "b");
    const BrowserModel* cm = ws.model(visible[1]);
    CHECK(cm->current_dir() == t.path() / "c");
    CHECK(cm->show_hidden());
    REQUIRE(cm->cursor_entry() != nullptr);
    CHECK(cm->cursor_entry()->name == "three.txt");
    const TabId first = ws.layout().find_pane(ws.layout().pane_order()[0])->tabs[0].id;
    CHECK(ws.model(first)->suspended());
    CHECK_FALSE(ws.model(first)->loaded());
    (void)tb;

    // Activating the background tab lists it; the previous one is suspended.
    ws.activate_tab(first);
    settle(ws);
    CHECK(ws.model(first)->loaded());
    CHECK(ws.model(first)->current_dir() == t.path() / "a");
    CHECK(ws.model(visible[0])->suspended());
    CHECK(live_models(ws) == 2);
}

TEST_CASE("navigator DIR keeps the session and opens DIR as a new active tab", "[workspace]") {
    TempDir t;
    populate(t);
    layout::Session s;
    s.layout = layout::Layout(state(t.path() / "a"));
    (void)s.layout.add_tab(s.layout.active_pane(), state(t.path() / "b"), 1, false);
    Workspace ws(options(t));
    ws.start(std::move(s), t.path() / "d");
    settle(ws);
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "d", "b"});
    CHECK(ws.active_model().current_dir() == t.path() / "d");
}

TEST_CASE("A session without valid tabs gets one tab on the start folder", "[workspace]") {
    TempDir t;
    populate(t);
    Result<layout::Session> s = layout::session_from_json(R"({"version":1,"layout":{}})");
    REQUIRE(s);
    Workspace ws(options(t));
    ws.start(std::move(*s), t.path() / "c");
    settle(ws);
    CHECK(ws.layout().tab_count() == 1);
    CHECK(ws.active_model().current_dir() == t.path() / "c");
}

TEST_CASE("Tab commands", "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);

    // Ctrl+W on the last tab of the last pane is a no-op.
    CHECK(ws.execute(Command{CommandId::CloseTab, 0}));
    CHECK(ws.layout().tab_count() == 1);

    // tab.new opens the active tab's folder with its options, after it, active.
    (void)ws.active_model().execute(Command{CommandId::ToggleHidden, 0});
    settle(ws);
    CHECK(ws.execute(Command{CommandId::NewTab, 0}));
    settle(ws);
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "a"});
    CHECK(ws.active_model().show_hidden());
    // ... or the home folder with NewTabArg::Home.
    (void)ws.execute(Command{CommandId::NewTab, NewTabArg::Home});
    settle(ws);
    CHECK(ws.active_model().current_dir() == t.path());
    CHECK_FALSE(ws.active_model().show_hidden());
    ws.active_model().navigate_to(t.path() / "b");
    settle(ws);
    CHECK(ws.sync()); // a folder change is persisted

    // Next/previous wrap.
    (void)ws.execute(Command{CommandId::NextTab, 0});
    CHECK(ws.layout()
              .find_tab(ws.active_tab())
              .value_or(layout::TabLocation{layout::PaneId{}, 99})
              .index == 0);
    (void)ws.execute(Command{CommandId::PreviousTab, 0});
    CHECK(ws.layout()
              .find_tab(ws.active_tab())
              .value_or(layout::TabLocation{layout::PaneId{}, 99})
              .index == 2);

    // Move left / right without wrapping.
    (void)ws.execute(Command{CommandId::MoveTabLeft, 0});
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "b", "a"});
    (void)ws.execute(Command{CommandId::MoveTabLeft, 0});
    (void)ws.execute(Command{CommandId::MoveTabLeft, 0});
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"b", "a", "a"});

    // Close the active tab ("b", index 0) and reopen it at its old index.
    const TabId b = ws.active_tab();
    CHECK(ws.execute(Command{CommandId::CloseTab, b.value}));
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "a"});
    CHECK(ws.closed_tabs().size() == 1);
    CHECK(ws.model(b) == nullptr); // its model is gone
    (void)ws.execute(Command{CommandId::ReopenClosedTab, 0});
    settle(ws);
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"b", "a", "a"});
    CHECK(ws.active_model().current_dir() == t.path() / "b");
    CHECK(ws.closed_tabs().empty());

    // Close tabs left of the last one.
    const TabId last = ws.layout().find_pane(ws.active_pane())->tabs[2].id;
    (void)ws.execute(Command{CommandId::CloseLeftTabs, last.value});
    CHECK(ws.layout().tab_count() == 1);
    CHECK(ws.closed_tabs().size() == 2);

    // A stale tab id is a no-op.
    CHECK(ws.execute(Command{CommandId::CloseTab, 9999}));
    CHECK(ws.layout().tab_count() == 1);
    CHECK(ws.layout().validate());
}

TEST_CASE("Duplicate copies folder, selection, cursor and history", "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path());
    settle(ws);
    ws.active_model().navigate_to(t.path() / "a");
    settle(ws);
    ws.active_model().click_row(1, Modifiers::None);
    const std::string cursor = ws.active_model().cursor_entry()->name;
    (void)ws.execute(Command{CommandId::DuplicateTab, 0});
    settle(ws);
    CHECK(ws.layout().tab_count() == 2);
    const BrowserModel& copy = ws.active_model();
    CHECK(ws.layout()
              .find_tab(ws.active_tab())
              .value_or(layout::TabLocation{layout::PaneId{}, 99})
              .index == 1);
    CHECK(copy.current_dir() == t.path() / "a");
    CHECK(copy.cursor_entry()->name == cursor);
    CHECK(copy.selected_count() == 1);
    CHECK(copy.can_back());
    CHECK(copy.back_stack().back().dir == t.path());
}

TEST_CASE("Open in new tab: folder in a background tab, file like Enter", "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    BrowserModel& m = ws.active_model();
    const std::size_t sub = m.find("sub");
    REQUIRE(sub != kNoRow);
    m.cursor_to(sub);
    const TabId before = ws.active_tab();
    (void)ws.execute(Command{CommandId::OpenInNewTab, 0});
    settle(ws);
    CHECK(ws.layout().tab_count() == 2);
    CHECK(ws.active_tab() == before); // background tab
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "sub"});
    const TabId bg = ws.layout().find_pane(ws.active_pane())->tabs[1].id;
    CHECK(ws.model(bg)->suspended());
    // On a file it opens the file (no launcher in tests: a notice, no new tab).
    m.cursor_to(m.find("one.txt"));
    (void)ws.execute(Command{CommandId::OpenInNewTab, 0});
    CHECK(ws.layout().tab_count() == 2);
    CHECK(ws.active_model().notice().has_value());
}

TEST_CASE("Pane commands and focus cycling", "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    (void)ws.execute(Command{CommandId::SplitRight, 0});
    (void)ws.execute(Command{CommandId::SplitDown, 0});
    settle(ws);
    REQUIRE(ws.layout().pane_count() == 3);
    CHECK(live_models(ws) == 3);
    const std::vector<PaneId> order = ws.layout().pane_order();
    CHECK(ws.active_pane() == order[2]);
    (void)ws.execute(Command{CommandId::FocusNextPane, 0});
    CHECK(ws.active_pane() == order[0]);
    (void)ws.execute(Command{CommandId::FocusPreviousPane, 0});
    CHECK(ws.active_pane() == order[2]);
    // Closing a pane pushes all its tabs onto the closed list.
    (void)ws.execute(Command{CommandId::NewTab, 0});
    (void)ws.execute(Command{CommandId::ClosePane, 0});
    CHECK(ws.layout().pane_count() == 2);
    CHECK(ws.closed_tabs().size() == 2);
    // Reopen goes back to the active pane when the old pane is gone.
    (void)ws.execute(Command{CommandId::ReopenClosedTab, 0});
    CHECK(ws.layout().pane_count() == 2);
    CHECK(ws.layout().tab_count() == 3);
    (void)ws.execute(Command{CommandId::ClosePane, 0});
    (void)ws.execute(Command{CommandId::ClosePane, 0}); // the only pane: no-op
    CHECK(ws.layout().pane_count() == 1);
    CHECK(ws.layout().validate());
    // Popup commands are left to the shell.
    CHECK_FALSE(ws.execute(Command{CommandId::SearchTabs, 0}));
    CHECK_FALSE(ws.execute(Command{CommandId::SaveLayout, 0}));
    CHECK_FALSE(ws.execute(Command{CommandId::Quit, 0}));
}

TEST_CASE("Named layouts: save, list, load into closed tabs, delete", "[workspace]") {
    TempDir t;
    populate(t);
    const Path cfg = t.dir("config");
    Workspace ws(options(t, cfg));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    (void)ws.execute(Command{CommandId::SplitRight, 0});
    settle(ws);
    ws.active_model().navigate_to(t.path() / "b");
    settle(ws);
    REQUIRE(ws.save_layout("Two panes"));
    CHECK(ws.layout_exists("Two panes"));
    CHECK_FALSE(ws.save_layout("../escape"));

    (void)ws.execute(Command{CommandId::ClosePane, 0});
    const std::uint64_t epoch = ws.layout_epoch();
    REQUIRE(ws.load_layout("Two panes"));
    settle(ws);
    CHECK(ws.layout_epoch() != epoch);
    CHECK(ws.layout().pane_count() == 2);
    CHECK(pane_paths(ws, 1) == std::vector<std::string>{"b"});
    CHECK(live_models(ws) == 2);
    // The replaced tab went to the closed list (plus the pane closed before).
    CHECK(ws.closed_tabs().size() == 2);
    CHECK_FALSE(ws.load_layout("missing"));
    REQUIRE(ws.delete_layout("Two panes"));
    CHECK(ws.list_layouts()->empty());

    Workspace none(options(t));
    none.start(std::nullopt, t.path());
    CHECK(none.save_layout("x").error().code == ErrorCode::Unsupported);
}

TEST_CASE("Session writer: background save, flush, synchronous save", "[workspace][session]") {
    TempDir t;
    populate(t);
    const Path file = layout::session_file(t.path() / "cfg");
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    (void)ws.execute(Command{CommandId::SplitRight, 0});
    settle(ws);
    {
        SessionWriter w(file);
        (void)ws.sync();
        w.submit(ws.snapshot());
        CHECK(w.flush());
        CHECK(w.writes() == 1);
        Result<layout::Session> back = layout::load_session(file);
        REQUIRE(back);
        CHECK(back->layout.pane_count() == 2);
        (void)ws.execute(Command{CommandId::ClosePane, 0});
        (void)ws.sync();
        REQUIRE(w.save_now(ws.snapshot()));
        CHECK(layout::load_session(file)->layout.pane_count() == 1);
        // Latest wins: a burst of submits writes at most a few times and ends on the last.
        for (int i = 0; i < 20; ++i) {
            (void)ws.execute(Command{CommandId::NewTab, 0});
            w.submit(ws.snapshot());
        }
    } // the destructor finishes the queued write
    CHECK(layout::load_session(file)->layout.tab_count() == ws.layout().tab_count());
}

TEST_CASE("Tabs closed together reopen in their old order (close_left, pane.close)",
          "[workspace]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t));
    ws.start(std::nullopt, t.path() / "a");
    settle(ws);
    for (const char* d : {"b", "c", "d"}) {
        (void)ws.execute(Command{CommandId::NewTab, 0});
        ws.active_model().navigate_to(t.path() / d);
        settle(ws);
    }
    (void)ws.sync();
    REQUIRE(pane_paths(ws, 0) == std::vector<std::string>{"a", "b", "c", "d"});

    // close_left on d, then reopen three times: a b c d.
    (void)ws.execute(Command{CommandId::CloseLeftTabs, 0});
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"d"});
    for (int i = 0; i < 3; ++i) {
        (void)ws.execute(Command{CommandId::ReopenClosedTab, 0});
    }
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "b", "c", "d"});

    // pane.close: the pane is gone, so its tabs come back after the active tab, in order.
    (void)ws.execute(Command{CommandId::SplitRight, 0}); // new pane on "c" (last reopened)
    settle(ws);
    (void)ws.execute(Command{CommandId::NewTab, 0});
    ws.active_model().navigate_to(t.path() / "a" / "sub");
    settle(ws);
    (void)ws.sync();
    REQUIRE(pane_paths(ws, 1) == std::vector<std::string>{"c", "sub"});
    (void)ws.execute(Command{CommandId::ClosePane, 0});
    REQUIRE(ws.layout().pane_count() == 1);
    const TabId first = ws.layout().find_pane(ws.active_pane())->tabs[0].id;
    ws.activate_tab(first); // reopen after "a"
    (void)ws.execute(Command{CommandId::ReopenClosedTab, 0});
    (void)ws.execute(Command{CommandId::ReopenClosedTab, 0});
    CHECK(pane_paths(ws, 0) == std::vector<std::string>{"a", "c", "sub", "b", "c", "d"});
}

TEST_CASE("A never-activated tab keeps its saved scroll, including 0", "[workspace][session]") {
    TempDir t;
    populate(t);
    layout::Session s;
    TabState top = state(t.path() / "a");
    top.scroll_rows = 0.0;
    top.cursor_name = "sub";
    s.layout = layout::Layout(state(t.path() / "b"));
    (void)s.layout.add_tab(s.layout.active_pane(), top, 1, false);
    TabState none = state(t.path() / "c");
    (void)s.layout.add_tab(s.layout.active_pane(), none, 2, false);
    Workspace ws(options(t));
    ws.start(std::move(s), Path());
    settle(ws);
    (void)ws.sync();
    const layout::PaneNode* p = ws.layout().find_pane(ws.active_pane());
    REQUIRE(p->tabs.size() == 3);
    CHECK(p->tabs[1].state.scroll_rows == std::optional<double>(0.0));
    CHECK(p->tabs[1].state.cursor_name == "sub");
    CHECK_FALSE(p->tabs[2].state.scroll_rows.has_value());
}

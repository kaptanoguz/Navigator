// Keymap end to end (owner: qa): every default chord in docs/keymap.md -> parse_key_chord ->
// load_default_keymap + register_builtin_commands -> resolve_command -> ui::Workspace::execute
// (tab, pane and list.open_in_new_tab commands; browser commands go on to the active tab's
// BrowserModel), and the effect on a fixture folder. The doc is parsed at test time (every
// "## Phase N defaults" table), so a row added to the doc without a binding (or a binding without
// a row, or a row without an effect check here) fails.
//
// Activate on a file goes to a recording launcher; nothing is ever spawned. The Workspace has no
// config root: nothing is written outside the fixture.
//
// Phase 4 chords (Ctrl+B, Ctrl+L, Ctrl+G, Ctrl+D, Ctrl+Shift+C) are App-level: the Workspace
// does not handle them (checked), and the SDL shell (src/ui/shell/app.cpp) dispatches them to
// Workspace::set_sidebar, the address-bar / GoTo PathEntry, PlacesModel::toggle_bookmark and
// copy_path_text. Their effect checks drive those same headless pieces the way the shell does;
// the shell's own key routing is covered end to end by tests/integration/places_app_smoke.sh
// (key:CHORD script steps on the real binary).
//
// Phase 5: Ctrl+E (search.flatten) goes through Workspace::execute to the tab's BrowserModel and
// walks the fixture with the real fs::Walker. Ctrl+F, Ctrl+Shift+E and Ctrl+Shift+F are the
// shell's (filter focus, volume table + index service, types popup); the Workspace does not
// handle them (checked) and their effect is driven on the BrowserModel calls the shell makes,
// with a real create_name_index() of the fixture for the index-backed Ctrl+Shift+E.
//
// Phase 6: Space and Alt+KeypadEnter (view.inspect) are the shell's: the Workspace does not
// handle them (checked); the effect is driven on the headless pieces App::Impl::inspector_command
// uses (Workspace::set_inspector, inspector::Controller with the same preview backend the app
// builds, no disk cache, no thumbnailers). A Space typed into a focused text field is consumed
// before the keymap (field_consumes_as_text) and becomes text; the shell's key routing itself
// is covered end to end by tests/integration/preview_app_smoke.sh (key:Space script steps).
//
// Phase 7: edit.* / file.* commands are the shell's (App::Impl::file_command): the Workspace does
// not handle them (checked). Their effects are driven through the same headless FileOps the shell
// owns (ui::FileOps over a REAL trash::create_trash() whose home trash lies in a fixture, an
// in-memory clipboard, a FileTarget built from the BrowserModel exactly like
// App::Impl::file_target) and checked on the fixture folder, the clipboard formats and the fixture
// trash. Shift+Delete never deletes without the confirmation sheet's accept. The text-field and
// filter-field consumption of the chords is checked on the routing functions the shell calls first
// (ui::apply_field_key, ui::route_filter_key); the shell's own focus scoping (sidebar, popups,
// address bar, Trash view) is covered by tests/integration/ops_app_smoke.sh on the real binary.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/layout.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/search.hpp>
#include <navigator/transfer.hpp>
#include <navigator/trash.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "inspector/inspector_controller.hpp"
#include "inspector/preview_backend.hpp"
#include "ops/clipboard.hpp"
#include "ops/file_ops.hpp"
#include "path/copy_path.hpp"
#include "path/path_entry.hpp"
#include "search/filter_rules.hpp"
#include "search_support.hpp"
#include "sidebar/places_model.hpp"
#include "sidebar/sidebar_model.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::layout::PaneId;
using navigator::layout::TabId;
using navigator::ui::BrowserModel;
using navigator::ui::kNoRow;
using navigator::ui::Workspace;
namespace inspector = navigator::ui::inspector;

#ifndef NAVIGATOR_KEYMAP_DOC
#error "NAVIGATOR_KEYMAP_DOC must name docs/keymap.md"
#endif

namespace {

struct DocRow {
    std::string chord; // empty for an "(unbound)" row
    std::string command;
    std::uint64_t arg = 0;
    int line = 0;
};

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

// "`x`" -> "x"; nullopt when the cell is not exactly one code span.
std::optional<std::string> code_span(const std::string& cell) {
    if (cell.size() < 3 || cell.front() != '`' || cell.back() != '`' ||
        cell.find('`', 1) != cell.size() - 1) {
        return std::nullopt;
    }
    return cell.substr(1, cell.size() - 2);
}

struct DocTables {
    std::vector<DocRow> bound;   // in doc order (Phase 0, Phase 2, Phase 3, ...)
    std::vector<DocRow> unbound; // "(unbound)" rows of every table
    std::map<int, std::size_t> per_phase;
};

// Every "## Phase N defaults ..." table (rows up to the next heading). Phase 0 has no arg
// column; later tables do.
DocTables read_doc() {
    std::ifstream in(NAVIGATOR_KEYMAP_DOC);
    REQUIRE(in.good());
    DocTables doc;
    int phase = -1; // -1: not in a defaults table
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.starts_with("#")) {
            phase = -1;
            constexpr std::string_view kHead = "## Phase ";
            if (line.starts_with(kHead) && line.find(" defaults") != std::string::npos) {
                phase = std::stoi(line.substr(kHead.size()));
            }
            continue;
        }
        if (phase < 0 || !line.starts_with("|")) {
            continue;
        }
        std::vector<std::string> cells;
        std::size_t pos = 1;
        while (pos < line.size()) {
            const std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) {
                break;
            }
            cells.push_back(trim(std::string_view(line).substr(pos, bar - pos)));
            pos = bar + 1;
        }
        if (cells.size() < 3) {
            continue;
        }
        const std::optional<std::string> command = code_span(cells[1]);
        if (!command) {
            continue; // header, separator
        }
        const std::optional<std::string> chord = code_span(cells[0]);
        DocRow row{chord.value_or(std::string()), *command, 0, number};
        if (phase > 0) {
            INFO("keymap.md line " << number);
            REQUIRE(cells.size() >= 4);
            if (cells[3] == "`Extend`") {
                row.arg = CursorArg::Extend;
            } else if (cells[3] == "`KeepSelection`") {
                row.arg = CursorArg::KeepSelection;
            } else {
                REQUIRE(cells[3] == "0");
            }
        }
        if (chord) {
            doc.bound.push_back(std::move(row));
            ++doc.per_phase[phase];
        } else if (cells[0] == "(unbound)") {
            doc.unbound.push_back(std::move(row));
        } // else: "window close" (not a chord)
    }
    return doc;
}

// Fixture: 2 folders, 30 files f01..f30, a 3-byte c.txt, a hidden file. One Workspace (one pane,
// one tab on the fixture root), no watcher, recording launcher.
struct Scene {
    TempDir t;
    NoSpawnLauncher launcher;
    std::unique_ptr<Workspace> ws;

    Scene() {
        std::filesystem::create_directories(t / "alpha" / "inner");
        std::filesystem::create_directories(t / "beta");
        for (int i = 1; i <= 30; ++i) {
            (void)touch(t / ((i < 10 ? "f0" : "f") + std::to_string(i)));
        }
        { std::ofstream(t / "c.txt") << "abc"; }
        (void)touch(t / ".hidden");
        Workspace::Options o;
        o.model.launcher = &launcher;
        o.home = t / "beta";
        ws = std::make_unique<Workspace>(std::move(o));
        ws->start(std::nullopt, t.path());
        settle();
        m().set_page_rows(5);
    }
    void settle() const {
        for (int round = 0; round < 3; ++round) {
            for (const TabId tab : ws->visible_tabs()) {
                REQUIRE(ws->model(tab) != nullptr);
                REQUIRE(ws->model(tab)->wait_listing(kBrowserWait));
            }
            (void)ws->frame_start();
        }
        REQUIRE(ws->settled());
        REQUIRE_FALSE(m().busy());
    }
    [[nodiscard]] bool exec(const Command& c) const { return ws->execute(c); }
    void run(const Command& c) const {
        (void)ws->execute(c);
        settle();
    }
    void go(const Path& dir) const {
        m().navigate_to(dir);
        settle();
    }
    [[nodiscard]] BrowserModel& m() const { return ws->active_model(); }
    [[nodiscard]] const layout::Layout& l() const { return ws->layout(); }
    [[nodiscard]] const layout::PaneNode& pane(std::size_t index) const {
        const std::vector<PaneId> order = l().pane_order();
        REQUIRE(index < order.size());
        return *l().find_pane(order[index]);
    }
    [[nodiscard]] const layout::PaneNode& active_pane() const {
        return *l().find_pane(l().active_pane());
    }
    [[nodiscard]] std::size_t active_pane_index() const {
        const std::vector<PaneId> order = l().pane_order();
        return static_cast<std::size_t>(std::find(order.begin(), order.end(), l().active_pane()) -
                                        order.begin());
    }
    // Folder of every tab of the active pane, in tab-bar order (live folder for loaded models).
    [[nodiscard]] std::vector<Path> tab_dirs(std::size_t pane_index) const {
        std::vector<Path> out;
        for (const layout::Tab& tab : pane(pane_index).tabs) {
            const BrowserModel* model = ws->model(tab.id);
            out.push_back(model != nullptr && model->loaded() ? model->current_dir()
                                                              : tab.state.path);
        }
        return out;
    }
    // Only the visible tab of each pane has a live model; every tab has one.
    [[nodiscard]] bool lazy_ok() const {
        const std::vector<TabId> visible = ws->visible_tabs();
        for (const PaneId p : l().pane_order()) {
            for (const layout::Tab& tab : l().find_pane(p)->tabs) {
                const BrowserModel* model = ws->model(tab.id);
                const bool shown =
                    std::find(visible.begin(), visible.end(), tab.id) != visible.end();
                if (model == nullptr || model->suspended() == shown) {
                    return false;
                }
            }
        }
        return l().validate().has_value();
    }
    // Three tabs in the one pane: root, alpha, beta; the last one active.
    void three_tabs() const {
        const Command new_tab{CommandId::NewTab, 0};
        run(new_tab);
        go(t / "alpha");
        run(new_tab);
        go(t / "beta");
        REQUIRE(tab_dirs(0) == std::vector<Path>{t.path(), t / "alpha", t / "beta"});
        REQUIRE(active_pane().active_tab == 2);
    }
};

// The chord of a canonical text, or a failed check and an Unknown chord.
KeyChord chord_of(std::string_view text) {
    if (Result<KeyChord> chord = parse_key_chord(text); chord.has_value()) {
        return *chord;
    }
    FAIL_CHECK("chord does not parse: " << text);
    return {};
}

// The resolved command, or a failed check and CommandId::None.
Command command_of(const Keymap& keymap, const CommandRegistry& registry, KeyChord chord) {
    if (std::optional<Command> c = resolve_command(keymap, registry, chord); c.has_value()) {
        return *c;
    }
    FAIL_CHECK("chord does not resolve: " << to_string(chord));
    return {};
}

using Check = std::function<void(Scene&, const Command&)>;

void check_cursor_command(Scene& s, const Command& c) {
    BrowserModel& m = s.m();
    REQUIRE(m.entries().size() == 33); // alpha, beta, c.txt, f01..f30
    m.click_row(10, Modifiers::None);  // cursor = anchor = 10
    const bool extend = (c.arg & CursorArg::Extend) != 0U;
    const std::size_t last = m.entries().size() - 1;
    const std::size_t step = m.page_rows() - 1; // a page keeps one row of context
    std::size_t want = 0;
    switch (c.id) {
    case CommandId::CursorUp: want = 9; break;
    case CommandId::CursorDown: want = 11; break;
    case CommandId::CursorPageUp: want = 10 - step; break;
    case CommandId::CursorPageDown: want = 10 + step; break;
    case CommandId::CursorHome: want = 0; break;
    case CommandId::CursorEnd: want = last; break;
    default: FAIL("not a cursor command");
    }
    REQUIRE(s.exec(c));
    CHECK(m.cursor() == want);
    std::set<std::size_t> selected;
    for (std::size_t i = 0; i < m.entries().size(); ++i) {
        if (m.is_selected(i)) {
            selected.insert(i);
        }
    }
    const bool keep_selection = !extend && (c.arg & CursorArg::KeepSelection) != 0U;
    std::set<std::size_t> expect;
    if (extend) {
        for (std::size_t i = std::min<std::size_t>(10, want); i <= std::max<std::size_t>(10, want);
             ++i) {
            expect.insert(i);
        }
        CHECK(m.anchor() == 10);
    } else if (keep_selection) {
        expect.insert(10);
        CHECK(m.anchor() == 10);
    } else {
        expect.insert(want);
        CHECK(m.anchor() == want);
    }
    CHECK(selected == expect);
    CHECK(invariant_violation(m).empty());
}

// Commands the Workspace does not handle: the App (quit, overlay, theme) or the shell's popups
// (Search Tabs, layout prompt and picker). Nothing changes.
void check_not_handled(Scene& s, const Command& c) {
    const auto before = entry_names(s.m());
    const std::size_t tabs = s.l().tab_count();
    const std::size_t panes = s.l().pane_count();
    const std::uint64_t serial = s.ws->persist_serial();
    CHECK_FALSE(s.exec(c));
    CHECK(entry_names(s.m()) == before);
    CHECK(s.l().tab_count() == tabs);
    CHECK(s.l().pane_count() == panes);
    CHECK(s.ws->persist_serial() == serial);
    CHECK_FALSE(s.m().busy());
}

void check_open_in_new_tab(Scene& s, const Command& c) {
    BrowserModel& m = s.m();
    const TabId origin = s.ws->active_tab();
    m.toggle_hidden(); // the new tab inherits sort and hidden
    s.settle();
    m.click_row(m.find("alpha"), Modifiers::None);
    REQUIRE(s.exec(c));
    s.settle();
    // Background tab right of the active one; the active tab and its folder stay.
    CHECK(s.ws->active_tab() == origin);
    CHECK(s.m().current_dir() == s.t.path());
    REQUIRE(s.pane(0).tabs.size() == 2);
    CHECK(s.pane(0).active_tab == 0);
    const layout::Tab& bg = s.pane(0).tabs[1];
    CHECK(bg.state.path == s.t / "alpha");
    CHECK(bg.state.show_hidden);
    REQUIRE(s.ws->model(bg.id) != nullptr);
    CHECK(s.ws->model(bg.id)->suspended());
    CHECK(s.lazy_ok());
    // First activation lists it.
    s.ws->activate_tab(bg.id);
    s.settle();
    CHECK(s.m().current_dir() == s.t / "alpha");
    CHECK(entry_names(s.m()) == std::vector<std::string>{"inner"});
    CHECK(s.ws->model(origin)->suspended());
    // On a file: same as Enter, no tab.
    s.ws->activate_tab(origin);
    s.settle();
    s.m().click_row(s.m().find("c.txt"), Modifiers::None);
    REQUIRE(s.exec(c));
    s.settle();
    REQUIRE(s.launcher.requests.size() == 1);
    CHECK(s.launcher.requests[0].argv.back() == (s.t / "c.txt").string());
    CHECK(s.l().tab_count() == 2);
}

void check_split(Scene& s, const Command& c, layout::SplitAxis axis) {
    s.go(s.t / "alpha");
    s.m().click_row(0, Modifiers::None);
    const TabId first = s.ws->active_tab();
    REQUIRE(s.exec(c));
    s.settle();
    REQUIRE(s.l().pane_count() == 2);
    const auto* split = std::get_if<layout::SplitNode>(&s.l().root().node);
    REQUIRE(split != nullptr);
    CHECK(split->axis == axis);
    CHECK(s.active_pane_index() == 1); // the new pane, after the old one
    CHECK(s.m().current_dir() == s.t / "alpha");
    CHECK(s.ws->active_tab() != first);
    CHECK_FALSE(s.ws->model(first)->suspended()); // both panes are visible
    CHECK(s.lazy_ok());
    // A second split of the same axis adds a third child to the same split (n-ary, I6).
    REQUIRE(s.exec(c));
    s.settle();
    CHECK(s.l().pane_count() == 3);
    split = std::get_if<layout::SplitNode>(&s.l().root().node);
    REQUIRE(split != nullptr);
    CHECK(split->children.size() == 3);
    CHECK(s.active_pane_index() == 2);
    CHECK(s.lazy_ok());
}

// ---- Phase 4 (App-level commands; see the header comment) -----------------------------------

Timestamp wall_now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

// The outcome after a REQUIRE(o.has_value()) (tidy's optional check does not see REQUIRE).
ui::PathOutcome got(const std::optional<ui::PathOutcome>& o) {
    return o.value_or(ui::PathOutcome{});
}

// Lets a PathEntry without a worker run its debounced completion request synchronously.
void settle_entry(ui::PathEntry& e, double& now_ms) {
    now_ms += ui::PathEntry::kDebounceMs + 1.0;
    e.tick(now_ms);
    (void)e.poll(now_ms);
}

void check_toggle_sidebar(Scene& s, const Command& c) {
    check_not_handled(s, c);
    // The shell's handler: flip SidebarState::visible through the Workspace (persisted).
    const layout::SidebarState before = s.ws->sidebar();
    CHECK(before.visible); // default
    for (int round = 0; round < 2; ++round) {
        const std::uint64_t serial = s.ws->persist_serial();
        layout::SidebarState st = s.ws->sidebar();
        st.visible = !st.visible;
        s.ws->set_sidebar(st);
        CHECK(s.ws->sidebar().visible == (round != 0));
        CHECK(s.ws->persist_serial() > serial);
        CHECK(s.ws->snapshot().sidebar == s.ws->sidebar());
        // F6 includes the sidebar only while it is visible.
        const ui::FocusStop next = ui::cycle_focus(1, s.ws->sidebar().visible, false, 0, 1);
        CHECK(next.sidebar == s.ws->sidebar().visible);
    }
    CHECK(s.ws->sidebar() == before);
    CHECK(s.l().tab_count() == 1);
}

void check_focus_address_bar(Scene& s, const Command& c) {
    check_not_handled(s, c);
    // The shell's handler: the active pane's breadcrumbs become a field with the folder path,
    // all selected; no dropdown until edited, nothing highlighted.
    double now_ms = 1000.0;
    ui::PathEntry e(ui::PathEntryMode::AddressBar);
    const std::string folder = s.m().current_dir().string();
    e.open(folder, true, s.m().current_dir(), s.t.path(), {}, now_ms);
    CHECK(e.edit().text() == folder);
    CHECK(e.edit().selected_text() == folder);
    CHECK(e.items().empty());
    CHECK(e.highlight() == static_cast<std::size_t>(-1));
    // Typing replaces the selection; the completion lists alpha; Enter goes there.
    CHECK(e.edit().insert(folder + "/al"));
    e.changed(now_ms);
    settle_entry(e, now_ms);
    REQUIRE(e.items().size() == 1);
    CHECK(e.items()[0].path == s.t / "alpha");
    CHECK(e.accept_completion(now_ms));
    CHECK(e.edit().text() == folder + "/alpha/");
    e.submit(false);
    std::optional<ui::PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).kind == ui::PathOutcome::Kind::Navigate);
    CHECK(got(o).path == s.t / "alpha");
    s.ws->navigate_active(got(o).path, got(o).cursor_name);
    s.settle();
    CHECK(s.m().current_dir() == s.t / "alpha");
    CHECK(s.l().tab_count() == 1);
}

void check_goto(Scene& s, const Command& c) {
    check_not_handled(s, c);
    // GoTo sources as the shell builds them from the PlacesModel: recents, bookmarks, places.
    ui::PlacesModel::Options po;
    po.home = s.t.path();
    po.probe_worker = false;
    ui::PlacesModel places(po);
    places.record_visit(s.t / "beta", wall_now(), 0.0);
    REQUIRE(places.add_bookmark(s.t / "alpha", wall_now()));
    std::vector<ui::GotoSource> sources;
    for (const places::RecentEntry& r : places.recents().entries()) {
        ui::GotoSource g;
        g.kind = ui::GotoSource::Kind::Recent;
        g.status = places.status(r.path, r.volume, &g.path);
        g.label = places::default_label(r.path);
        sources.push_back(std::move(g));
    }
    for (const places::Bookmark& b : places.bookmarks().list()) {
        ui::GotoSource g;
        g.kind = ui::GotoSource::Kind::Bookmark;
        g.status = places.status(b.path, b.volume, &g.path);
        g.label = places::default_label(b.path);
        sources.push_back(std::move(g));
    }
    double now_ms = 1000.0;
    ui::PathEntry e(ui::PathEntryMode::GoTo);
    e.open({}, false, s.m().current_dir(), s.t.path(), sources, now_ms);
    // Empty text: recents first, then bookmarks; the highlight on row 0.
    REQUIRE(e.items().size() >= 2);
    CHECK(e.items()[0].kind == ui::PathItem::Kind::Recent);
    CHECK(e.items()[0].path == s.t / "beta");
    CHECK(e.items()[1].kind == ui::PathItem::Kind::Bookmark);
    CHECK(e.items()[1].path == s.t / "alpha");
    CHECK(e.highlight() == 0);
    // Enter on the highlighted recent navigates the active tab.
    e.submit(false);
    std::optional<ui::PathOutcome> o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).path == s.t / "beta");
    s.ws->navigate_active(got(o).path);
    s.settle();
    CHECK(s.m().current_dir() == s.t / "beta");
    // Typed path: "./../alpha/inner" from beta resolves through the typed row.
    e.open({}, false, s.m().current_dir(), s.t.path(), sources, now_ms);
    CHECK(e.edit().insert("../alpha/inner"));
    e.changed(now_ms);
    settle_entry(e, now_ms);
    REQUIRE_FALSE(e.items().empty());
    CHECK(e.items()[0].kind == ui::PathItem::Kind::Typed);
    e.submit(true); // Ctrl+Enter: new background tab
    o = e.take_outcome();
    REQUIRE(o.has_value());
    CHECK(got(o).new_tab);
    CHECK(got(o).path == s.t / "alpha" / "inner");
    CHECK(s.ws->open_tab(got(o).path, false));
    s.settle();
    CHECK(s.l().tab_count() == 2);
    CHECK(s.m().current_dir() == s.t / "beta"); // background tab: the active one stays
}

void check_toggle_bookmark(Scene& s, const Command& c) {
    check_not_handled(s, c);
    // The shell's handler with arg 0: bookmark the active tab's folder, or remove it.
    ui::PlacesModel::Options po;
    po.home = s.t.path();
    po.probe_worker = false;
    ui::PlacesModel places(po);
    s.go(s.t / "alpha");
    const std::uint64_t serial = places.serial();
    CHECK(places.toggle_bookmark(s.m().current_dir(), wall_now()) ==
          ui::PlacesModel::Toggled::Added);
    CHECK(places.bookmarks().contains(s.t / "alpha"));
    CHECK(places.bookmarks().size() == 1);
    CHECK(places.serial() > serial);
    // The sidebar shows it (and highlights it: the active folder).
    const std::vector<ui::SidebarRow> rows =
        ui::build_sidebar_rows(places, s.ws->sidebar(), s.m().current_dir());
    const std::size_t row = ui::sidebar_row_of(rows, places.bookmarks().list()[0].id);
    REQUIRE(row < rows.size());
    CHECK(rows[row].path == s.t / "alpha");
    CHECK(rows[row].highlighted);
    // Again: removed. Trailing separator / "." spellings are the same folder.
    CHECK(places.toggle_bookmark(s.t.path() / "alpha" / ".", wall_now()) ==
          ui::PlacesModel::Toggled::Removed);
    CHECK(places.bookmarks().empty());
    CHECK(s.m().current_dir() == s.t / "alpha"); // the folder itself is untouched
}

void check_copy_path(Scene& s, const Command& c) {
    check_not_handled(s, c);
    BrowserModel& m = s.m();
    m.clear_selection();
    // Nothing selected: the current folder, unquoted.
    CHECK(ui::copy_path_targets(m, false) == std::vector<Path>{s.t.path()});
    CHECK(ui::copy_path_text(ui::copy_path_targets(m, false)) == s.t.path().string());
    // A selection in list order; CopyPathArg::CurrentFolder ignores it.
    m.click_row(m.find("f02"), Modifiers::None);
    m.click_row(m.find("c.txt"), Modifiers::Ctrl);
    const std::vector<Path> want{s.t / "c.txt", s.t / "f02"};
    CHECK(ui::copy_path_targets(m, false) == want);
    CHECK(ui::copy_path_text(want) == path_input::quote_for_shell((s.t / "c.txt").string()) + " " +
                                          path_input::quote_for_shell((s.t / "f02").string()));
    CHECK(ui::copy_path_targets(m, (c.arg | CopyPathArg::CurrentFolder) != 0U) ==
          std::vector<Path>{s.t.path()});
    // One selected name with a space: still unquoted (pastes into GoTo).
    (void)touch(s.t / "a b");
    s.run(Command{CommandId::Refresh, 0});
    m.click_row(m.find("a b"), Modifiers::None);
    CHECK(ui::copy_path_text(ui::copy_path_targets(m, false)) == (s.t / "a b").string());
    CHECK(selected_set(m) == std::set<std::string>{"a b"}); // the selection is untouched
}

void add_phase4_checks(std::map<std::string, Check>& checks) {
    checks["view.toggle_sidebar"] = check_toggle_sidebar;
    checks["nav.focus_address_bar"] = check_focus_address_bar;
    checks["nav.goto"] = check_goto;
    checks["bookmarks.toggle"] = check_toggle_bookmark;
    checks["edit.copy_path"] = check_copy_path;
}

// ---- Phase 5 ----------------------------------------------------------------------------------
// Ctrl+E is a tab command (Workspace -> BrowserModel, real fs::Walker). Ctrl+F, Ctrl+Shift+E and
// Ctrl+Shift+F are the shell's (focus, the volume table and index service, the types popup):
// the Workspace does not handle them (checked) and the effect is driven on the headless pieces
// the shell calls (App::Impl::focus_filter / handle_text, flatten_volume, the SetTypeVisible /
// ToggleAllTypes intents).

// Scene listing without hidden: alpha, beta, c.txt, f01..f30. The flattened subtree adds
// alpha/inner.
const std::set<std::string>& flat_scene() {
    static const std::set<std::string> rels = [] {
        std::set<std::string> s{"alpha", "beta", "c.txt", "alpha/inner"};
        for (int i = 1; i <= 30; ++i) {
            s.insert((i < 10 ? "f0" : "f") + std::to_string(i));
        }
        return s;
    }();
    return rels;
}

void check_focus_filter(Scene& s, const Command& c) {
    BrowserModel& m = s.m();
    m.set_filter_text("f1"); // an earlier filter
    REQUIRE(row_names(m) == std::vector<std::string>{"f10", "f11", "f12", "f13", "f14", "f15",
                                                     "f16", "f17", "f18", "f19"});
    check_not_handled(s, c);
    // The shell's focus_filter(select_all = true): the text is selected, so typing replaces it.
    m.filter_edit().select_all();
    CHECK(m.filter_edit().selected_text() == "f1");
    REQUIRE(m.filter_edit().insert("C.T"));
    m.filter_edited();
    CHECK(m.filter_edit().text() == "C.T");
    CHECK(m.match_mode() == search::MatchMode::Substring);    // folder filter default
    CHECK(row_names(m) == std::vector<std::string>{"c.txt"}); // inline (33 < 20k), same frame
    CHECK(m.source_count() == 33);
    CHECK(row_cursor_name(m) == "c.txt");
    // Enter on the cursor row opens the file (rows, not listing indices).
    REQUIRE(s.exec(Command{CommandId::Activate, 0}));
    REQUIRE(s.launcher.requests.size() == 1);
    CHECK(s.launcher.requests[0].argv.back() == (s.t / "c.txt").string());
    // Ctrl+A selects the shown rows only.
    REQUIRE(s.exec(Command{CommandId::SelectAll, 0}));
    CHECK(m.selected_count() == 1);
    // Navigation clears the filter text (docs/keymap.md).
    s.go(s.t / "alpha");
    CHECK_FALSE(m.filter_active());
    CHECK(row_names(m) == std::vector<std::string>{"inner"});
    s.go(s.t.path());
    CHECK(m.row_count() == 33);
}

void check_flatten(Scene& s, const Command& c) {
    BrowserModel& m = s.m();
    REQUIRE(s.exec(c));
    REQUIRE(m.flattened());
    CHECK(m.flatten_root() == s.t.path());
    CHECK_FALSE(m.flatten_is_volume());
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(row_rel_set(m) == flat_scene()); // .hidden is not shown
    const ui::FlattenProgress p = m.flatten_progress();
    CHECK_FALSE(p.from_index);
    CHECK_FALSE(p.walking);
    CHECK_FALSE(p.cancelled);
    CHECK(p.found == flat_scene().size());
    CHECK(m.match_mode() == search::MatchMode::Fuzzy); // flattened default
    // Empty query: folders first (alpha, beta, inner), then names.
    REQUIRE(m.row_count() == flat_scene().size());
    CHECK(row_rels(m)[0] == "alpha");
    CHECK(row_rels(m)[2] == "alpha/inner");
    CHECK(row_rels(m)[3] == "c.txt");
    // Fuzzy query "inr" finds the nested folder only.
    m.set_filter_text("inr");
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(row_rels(m) == std::vector<std::string>{"alpha/inner"});
    CHECK(m.source_count() == flat_scene().size());
    m.clear_filter();
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.take_visits().empty()); // flatten never records Recents
    // Again: leave; the folder listing is back, the folder unchanged.
    REQUIRE(s.exec(c));
    CHECK_FALSE(m.flattened());
    CHECK(m.current_dir() == s.t.path());
    CHECK(m.row_count() == 33);
    CHECK(s.l().tab_count() == 1);
}

void check_flatten_volume(Scene& s, const Command& c) {
    check_not_handled(s, c);
    BrowserModel& m = s.m();
    // The shell's flatten_volume() without a ready index: a walk of the volume root (the
    // fixture stands in for the mount point the volume table would give).
    m.flatten(ui::FlattenRequest{s.t.path(), true, nullptr});
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.flatten_is_volume());
    CHECK_FALSE(m.flatten_progress().from_index);
    CHECK(row_rel_set(m) == flat_scene());
    m.leave_flatten(); // the shell's toggle: same root again leaves
    CHECK_FALSE(m.flattened());
    // With a Ready index of that volume: answered by the index, the same rows.
    const std::shared_ptr<search::IIndex> index = ready_index(index_options(s.t.path()));
    m.flatten(ui::FlattenRequest{s.t.path(), true, index});
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.flatten_is_volume());
    CHECK(m.flatten_progress().from_index);
    CHECK(m.flatten_progress().index_complete);
    CHECK(row_rel_set(m) == flat_scene());
    m.set_filter_text("f3");
    REQUIRE(m.settle_search(kSearchWait));
    // Ranked: the contiguous prefix "f3" (88) before the gapped f?3 names (61), then by name.
    CHECK(row_names(m) == std::vector<std::string>{"f30", "f03", "f13", "f23"});
    m.clear_filter();
    REQUIRE(m.settle_search(kSearchWait));
    m.leave_flatten();
    CHECK(m.row_count() == 33);
    index->stop();
}

void check_types(Scene& s, const Command& c) {
    check_not_handled(s, c);
    BrowserModel& m = s.m();
    // The popup's rows: Folders, No extension, then .txt; the hidden file does not count.
    const std::vector<search::TypeCount> rows = m.type_summary();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].key.kind == search::TypeKind::Folder);
    CHECK(rows[0].count == 2);
    CHECK(rows[1].key.kind == search::TypeKind::NoExtension);
    CHECK(rows[1].count == 30);
    CHECK(rows[2].key.extension == "txt");
    CHECK(rows[2].count == 1);
    // SetTypeVisible(txt, false) as the shell applies it.
    search::TypeFilter tf = m.type_filter();
    search::set_visible(tf, rows[2].key, false);
    m.set_type_filter(tf);
    CHECK(m.row_count() == 32);
    CHECK(m.find_row("c.txt") == kNoRow);
    CHECK(m.source_count() == 33);
    // Persisted with the tab (session "hidden_types").
    (void)s.ws->sync();
    const layout::TabState* st = s.ws->session().layout.tab_state(s.ws->active_tab());
    REQUIRE(st != nullptr);
    CHECK(st->type_filter.hidden_extensions == std::vector<std::string>{"txt"});
    // Kept across navigation, like show_hidden.
    s.go(s.t / "alpha");
    CHECK(m.type_filter().hidden_extensions == std::vector<std::string>{"txt"});
    s.go(s.t.path());
    CHECK(m.find_row("c.txt") == kNoRow);
    // Escape never clears it.
    m.escape();
    m.escape();
    CHECK(m.type_filter().active());
    // ToggleAllTypes: not every present type is visible, so all become visible.
    tf = m.type_filter();
    const std::vector<search::TypeCount> present = m.type_summary();
    search::toggle_all(tf, Span<const search::TypeCount>(present));
    m.set_type_filter(tf);
    CHECK_FALSE(m.type_filter().active());
    CHECK(m.row_count() == 33);
}

void add_phase5_checks(std::map<std::string, Check>& checks) {
    checks["list.filter"] = check_focus_filter;
    checks["search.flatten"] = check_flatten;
    checks["search.flatten_volume"] = check_flatten_volume;
    checks["search.types"] = check_types;
}

// ---- Phase 6 ----------------------------------------------------------------------------------
// view.inspect: the shell's App::Impl::inspector_command flips InspectorState::visible through
// the Workspace (persisted) and shows / hides the inspector::Controller, which follows the
// cursor and asks the preview service. The service is the app's own headless backend
// (ui::create_preview_backend without a thumbnail root: no disk cache, no thumbnailers).

// The app's item for the cursor row (App::Impl::item_for, without the volume lookup).
inspector::Subject cursor_subject(const BrowserModel& m) {
    inspector::Subject subject;
    if (const fs::DirEntry* e = m.cursor_entry(); e != nullptr) {
        inspector::Item item;
        item.path = m.row_path(m.cursor());
        item.entry = *e;
        subject.cursor = std::move(item);
    }
    subject.selected_count = m.selected_count();
    subject.selected_bytes = m.selected_bytes();
    return subject;
}

// Ticks the controller and feeds it the service's completions until it is no longer busy.
void pump_inspector(inspector::Controller& ctl, preview::IPreviewService* service, double& now_ms) {
    std::vector<preview::Completion> done;
    for (int i = 0; i < 400 && (ctl.busy() || ctl.status() == inspector::Status::Loading); ++i) {
        now_ms += inspector::kFollowDebounceMs + 1.0;
        ctl.tick(now_ms, 1);
        if (service != nullptr) {
            done.clear();
            (void)service->poll(done);
            for (const preview::Completion& c : done) {
                (void)ctl.on_completion(c);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void check_inspect(Scene& s, const Command& c) {
    check_not_handled(s, c);
    std::unique_ptr<preview::IPreviewService> service =
        ui::create_preview_backend(ui::PreviewBackendOptions{});
    CHECK((service != nullptr) == ui::preview_available());
    inspector::Controller ctl(service.get());
    double now_ms = 1000.0;
    // The shell's toggle, as App::Impl::inspector_command(Inspect) does it.
    const auto toggle = [&] {
        layout::InspectorState st = s.ws->inspector();
        st.visible = !st.visible;
        s.ws->set_inspector(st);
        ctl.set_visible(st.visible, now_ms);
    };
    BrowserModel& m = s.m();
    m.click_row(m.find("c.txt"), Modifiers::None);
    CHECK_FALSE(s.ws->inspector().visible); // default
    const std::uint64_t serial = s.ws->persist_serial();
    toggle();
    CHECK(s.ws->inspector().visible);
    CHECK(s.ws->persist_serial() != serial); // saved with the session
    CHECK(ctl.visible());
    ctl.follow(cursor_subject(m), now_ms);
    pump_inspector(ctl, service.get(), now_ms);
    REQUIRE(ctl.item() != nullptr);
    CHECK(ctl.item()->path == s.t / "c.txt");
    if (service) {
        REQUIRE(ctl.status() == inspector::Status::Ready);
        REQUIRE(ctl.result() != nullptr);
        CHECK(ctl.result()->kind() == preview::PreviewKind::Text);
        CHECK(ctl.submitted() >= 1);
    }
    // The cursor moves: the Inspector follows (a folder now).
    m.click_row(m.find("alpha"), Modifiers::None);
    ctl.follow(cursor_subject(m), now_ms);
    pump_inspector(ctl, service.get(), now_ms);
    REQUIRE(ctl.item() != nullptr);
    CHECK(ctl.item()->path == s.t / "alpha");
    if (service) {
        REQUIRE(ctl.status() == inspector::Status::Ready);
        CHECK(ctl.result()->kind() == preview::PreviewKind::Folder);
    }
    // Again: hidden, nothing outstanding; the listing is untouched.
    toggle();
    CHECK_FALSE(s.ws->inspector().visible);
    CHECK_FALSE(ctl.visible());
    CHECK_FALSE(ctl.busy());
    CHECK(m.current_dir() == s.t.path());
    CHECK(m.row_count() == 33);
    if (service) {
        service->shutdown();
    }
}

void add_phase6_checks(std::map<std::string, Check>& checks) {
    checks["view.inspect"] = check_inspect;
}

// ---- Phase 7 ----------------------------------------------------------------------------------
// A FileOps like the shell's, headless: real trash inside a fixture (never the owner's), the
// in-memory clipboard, protected paths that name only fixture folders.
struct FileRig {
    Scene& s;
    TempDir side; // trash and fake home live here, not in the folder the Scene lists
    ui::MemoryClipboard clip;
    std::shared_ptr<trash::ITrash> trash;
    std::unique_ptr<ui::FileOps> ops;

    explicit FileRig(Scene& scene) : s(scene) {
        trash::TrashOptions o;
        o.home_trash = side / "Trash";
        o.use_topdir_trashes = false;
        Result<std::shared_ptr<trash::ITrash>> made = trash::create_trash(std::move(o));
        REQUIRE(made.has_value());
        trash = *made;
        ui::FileOpsOptions fo;
        fo.trash = trash;
        fo.clipboard = &clip;
        fo.new_folder_name = [] { return std::string("New folder"); };
        fo.protected_paths.home = side / "home";
        fo.protected_paths.trash_roots.push_back(side / "Trash");
        ops = std::make_unique<ui::FileOps>(std::move(fo));
    }
    // App::Impl::file_target(), on the active tab.
    [[nodiscard]] ui::FileTarget target() const {
        ui::FileTarget t;
        t.tab = s.ws->active_tab();
        const BrowserModel& m = s.m();
        t.dir = m.current_dir();
        for (std::size_t i = 0; i < m.row_count(); ++i) {
            if (m.is_selected(i)) {
                t.selected.push_back(m.row_path(i));
            }
        }
        if (m.cursor() < m.row_count()) {
            t.cursor = m.row_path(m.cursor());
            t.cursor_is_dir = fs::is_directory_like(m.row_entry(m.cursor()));
        }
        return t;
    }
    void idle() const {
        REQUIRE(ops->wait_idle(std::chrono::seconds(10)));
        (void)ops->take_reconcile();
        s.run(Command{CommandId::Refresh, 0});
    }
    [[nodiscard]] std::vector<std::string> trash_files() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(side / "Trash" / "files", ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            names.push_back(it->path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }
    [[nodiscard]] static std::string slurp(const Path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] static bool exists(const Path& p) {
        std::error_code ec;
        return std::filesystem::symlink_status(p, ec).type() !=
               std::filesystem::file_type::not_found;
    }
    void select(std::string_view name) const {
        REQUIRE(s.m().find(name) != kNoRow);
        s.m().click_row(s.m().find(name), Modifiers::None);
    }
    // The previously active pane's folder, as App::Impl::file_command picks it (F7 / Shift+F7).
    [[nodiscard]] std::optional<Path> other_pane_dir() const {
        const std::vector<PaneId> order = s.l().pane_order();
        if (order.size() < 2) {
            return std::nullopt;
        }
        const std::size_t at = s.active_pane_index();
        const layout::PaneNode* node = s.l().find_pane(order[(at + 1) % order.size()]);
        if (node == nullptr || node->active_tab >= node->tabs.size()) {
            return std::nullopt;
        }
        const BrowserModel* other = s.ws->model(node->tabs[node->active_tab].id);
        if (other == nullptr || !other->loaded()) {
            return std::nullopt;
        }
        return other->current_dir();
    }
};

void check_copy_cut(Scene& s, const Command& c) {
    check_not_handled(s, c); // the Workspace never handles file commands
    FileRig f(s);
    const bool cut = c.id == CommandId::Cut;
    const auto before = entry_names(s.m());
    f.select("c.txt");
    f.ops->copy(f.target(), cut);
    // The GNOME, uri-list and (cut only) KDE formats are offered; nothing on disk changed.
    const std::string gnome = f.clip.read(transfer::kMimeGnomeCopiedFiles).value_or(std::string());
    REQUIRE_FALSE(gnome.empty());
    CHECK(gnome.starts_with(cut ? "cut\nfile://" : "copy\nfile://"));
    CHECK(gnome.find("/c.txt") != std::string::npos);
    const std::string uris = f.clip.read(transfer::kMimeUriList).value_or(std::string());
    REQUIRE_FALSE(uris.empty());
    CHECK(uris.find("/c.txt") != std::string::npos);
    CHECK(f.clip.read(transfer::kMimeKdeCutSelection).has_value() == cut);
    CHECK(f.ops->has_cut() == cut);
    CHECK(f.ops->is_cut(s.t / "c.txt") == cut);
    CHECK_FALSE(f.ops->busy());
    CHECK(entry_names(s.m()) == before);
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
    CHECK(f.trash_files().empty());
    // Nothing selected: nothing offered.
    f.clip.clear_own();
    s.m().clear_selection();
    ui::FileTarget none = f.target();
    none.selected.clear();
    none.cursor.clear();
    f.ops->copy(none, cut);
    CHECK_FALSE(f.clip.owns_offer());
}

void check_paste(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    // Copy then paste into alpha: a second c.txt appears, the first stays.
    f.select("c.txt");
    f.ops->copy(f.target(), false);
    s.go(s.t / "alpha");
    f.ops->paste(f.target(), false);
    f.idle();
    CHECK(FileRig::slurp(s.t / "alpha" / "c.txt") == "abc");
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
    CHECK(s.m().find("c.txt") != kNoRow);
    // Pasting the same copy again: the name is taken, the conflict sheet opens (nothing is
    // overwritten while it is open), and Enter = Keep both.
    f.ops->paste(f.target(), false);
    double now = 2000.0;
    for (int i = 0; i < 500 && f.ops->sheet_kind() != ui::SheetKind::Conflict; ++i) {
        now += 10.0;
        (void)f.ops->frame_start(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(f.ops->sheet_kind() == ui::SheetKind::Conflict);
    CHECK(FileRig::slurp(s.t / "alpha" / "c.txt") == "abc");
    f.ops->sheet_activate(); // Enter on the default button
    f.idle();
    CHECK(FileRig::exists(s.t / "alpha" / "c (2).txt"));
    CHECK(FileRig::slurp(s.t / "alpha" / "c.txt") == "abc");
    // Cut then paste into beta moves the file and clears the clipboard.
    s.go(s.t.path());
    f.select("f01");
    f.ops->copy(f.target(), true);
    s.go(s.t / "beta");
    f.ops->paste(f.target(), false);
    f.idle();
    CHECK(FileRig::exists(s.t / "beta" / "f01"));
    CHECK_FALSE(FileRig::exists(s.t / "f01"));
    CHECK_FALSE(f.ops->has_cut());
    CHECK_FALSE(f.clip.owns_offer());
    // Paste into the cursor folder (context menu "Paste into folder").
    s.go(s.t.path());
    f.select("c.txt");
    f.ops->copy(f.target(), false);
    f.select("beta");
    f.ops->paste(f.target(), true);
    f.idle();
    CHECK(FileRig::slurp(s.t / "beta" / "c.txt") == "abc");
    CHECK(f.trash_files().empty()); // nothing was trashed or removed by pasting
}

void check_undo_redo(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    const bool redo = c.id == CommandId::Redo;
    // Nothing to undo: no change, no error state.
    const auto before = entry_names(s.m());
    f.ops->undo();
    f.ops->redo();
    f.idle();
    CHECK(entry_names(s.m()) == before);
    // paste -> undo -> redo
    f.select("c.txt");
    f.ops->copy(f.target(), false);
    s.go(s.t / "alpha");
    f.ops->paste(f.target(), false);
    f.idle();
    REQUIRE(FileRig::exists(s.t / "alpha" / "c.txt"));
    REQUIRE(f.ops->undo_stack().can_undo());
    f.ops->undo(); // Ctrl+Z: the copy goes to the trash, never unlinked
    f.idle();
    CHECK_FALSE(FileRig::exists(s.t / "alpha" / "c.txt"));
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
    CHECK(f.trash_files() == std::vector<std::string>{"c.txt"});
    CHECK(FileRig::slurp(f.side / "Trash" / "files" / "c.txt") == "abc");
    if (redo) {
        REQUIRE(f.ops->undo_stack().can_redo());
        f.ops->redo(); // Ctrl+Shift+Z / Ctrl+Y: copied again, the trashed copy stays trashed
        f.idle();
        CHECK(FileRig::slurp(s.t / "alpha" / "c.txt") == "abc");
        CHECK(f.trash_files() == std::vector<std::string>{"c.txt"});
        CHECK_FALSE(f.ops->undo_stack().can_redo());
    }
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
}

void check_trash(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    f.select("c.txt");
    f.ops->trash(f.target()); // Delete: trash, never delete
    f.idle();
    CHECK_FALSE(FileRig::exists(s.t / "c.txt"));
    CHECK(s.m().find("c.txt") == kNoRow);
    CHECK(f.trash_files() == std::vector<std::string>{"c.txt"});
    CHECK(FileRig::slurp(f.side / "Trash" / "files" / "c.txt") == "abc");
    // The notice offers Undo, and Undo brings the file back.
    REQUIRE(f.ops->notice().has_value());
    const ui::OpNotice notice = f.ops->notice().value_or(ui::OpNotice{});
    CHECK(notice.action == ui::NoticeAction::Undo);
    CHECK_FALSE(notice.error);
    CHECK_FALSE(f.ops->sheet_open()); // no confirmation for a trash
    f.ops->undo();
    f.idle();
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
    CHECK(f.trash_files().empty());
    // A folder goes whole.
    f.select("alpha");
    f.ops->trash(f.target());
    f.idle();
    CHECK_FALSE(FileRig::exists(s.t / "alpha"));
    CHECK(FileRig::exists(f.side / "Trash" / "files" / "alpha" / "inner"));
}

void check_delete_permanently(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    f.select("c.txt");
    f.ops->request_delete_permanently(f.target());
    // The sheet is open, focus starts on Cancel, nothing is deleted and nothing is queued.
    REQUIRE(f.ops->sheet_kind() == ui::SheetKind::DeletePermanently);
    REQUIRE(f.ops->confirm_sheet() != nullptr);
    CHECK(f.ops->confirm_sheet()->focus == 0);
    CHECK(f.ops->confirm_sheet()->count == 1);
    CHECK_FALSE(f.ops->busy());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    (void)f.ops->frame_start(1000.0);
    CHECK(FileRig::exists(s.t / "c.txt"));
    CHECK_FALSE(f.ops->busy());
    // Enter on the default focus is Cancel; Escape cancels; neither deletes.
    f.ops->sheet_activate();
    CHECK_FALSE(f.ops->sheet_open());
    CHECK(FileRig::exists(s.t / "c.txt"));
    f.ops->request_delete_permanently(f.target());
    f.ops->sheet_escape();
    CHECK_FALSE(f.ops->sheet_open());
    CHECK(FileRig::exists(s.t / "c.txt"));
    // A click on Cancel (button 0) too.
    f.ops->request_delete_permanently(f.target());
    f.ops->sheet_activate(0);
    CHECK(FileRig::exists(s.t / "c.txt"));
    // Only the explicit accept deletes (button 1), straight from the sheet: not into the trash.
    f.ops->request_delete_permanently(f.target());
    REQUIRE(f.ops->sheet_kind() == ui::SheetKind::DeletePermanently);
    f.ops->sheet_activate(1);
    f.idle();
    CHECK_FALSE(FileRig::exists(s.t / "c.txt"));
    CHECK(f.trash_files().empty());
    CHECK_FALSE(f.ops->undo_stack().can_undo()); // permanent deletes are not undoable
}

void check_rename(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    const auto taken = [&s](std::string_view name) { return s.m().find(name) != kNoRow; };
    f.select("c.txt");
    const fs::DirEntry& e = s.m().row_entry(s.m().cursor());
    f.ops->begin_rename(s.ws->active_tab(), s.m().current_dir(), e.name, false);
    ui::InlineEdit* edit = f.ops->inline_edit();
    REQUIRE(edit != nullptr);
    CHECK(edit->edit.selection() == std::pair<std::size_t, std::size_t>{0, 1}); // the stem
    // Escape: nothing renamed.
    f.ops->inline_cancel();
    CHECK(f.ops->inline_edit() == nullptr);
    CHECK(FileRig::exists(s.t / "c.txt"));
    // An invalid name keeps the editor open with a message and renames nothing.
    f.ops->begin_rename(s.ws->active_tab(), s.m().current_dir(), "c.txt", false);
    f.ops->inline_edit()->edit.insert("a/b");
    f.ops->inline_edited(taken);
    CHECK_FALSE(f.ops->inline_commit(taken));
    REQUIRE(f.ops->inline_edit() != nullptr);
    CHECK(f.ops->inline_edit()->message_error);
    CHECK(FileRig::exists(s.t / "c.txt"));
    f.ops->inline_cancel();
    // Typing replaces the stem and keeps the extension, Turkish letters included.
    f.ops->begin_rename(s.ws->active_tab(), s.m().current_dir(), "c.txt", false);
    f.ops->inline_edit()->edit.insert("Çalışma ğ");
    f.ops->inline_edited(taken);
    CHECK(f.ops->inline_commit(taken));
    f.idle();
    CHECK(FileRig::slurp(s.t / "Çalışma ğ.txt") == "abc");
    CHECK_FALSE(FileRig::exists(s.t / "c.txt"));
    // Renaming onto an existing name is refused inline (never a silent overwrite).
    f.ops->begin_rename(s.ws->active_tab(), s.m().current_dir(), "f02", false);
    f.ops->inline_edit()->edit.set("f03");
    f.ops->inline_edited(taken);
    CHECK_FALSE(f.ops->inline_commit(taken));
    f.ops->inline_cancel();
    CHECK(FileRig::exists(s.t / "f02"));
    CHECK(FileRig::exists(s.t / "f03"));
    // Undo: renamed back.
    f.ops->undo();
    f.idle();
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
}

void check_new_folder(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    f.ops->new_folder(f.target());
    REQUIRE(f.ops->wait_idle(std::chrono::seconds(10)));
    const ui::Reconcile rc = f.ops->take_reconcile();
    CHECK(FileRig::exists(s.t / "New folder"));
    REQUIRE(rc.rename_after_create.has_value()); // the inline editor opens on it
    const ui::Reconcile::Rename made = rc.rename_after_create.value_or(ui::Reconcile::Rename{});
    CHECK(made.name == "New folder");
    CHECK(made.dir == s.t.path());
    f.ops->new_folder(f.target());
    f.idle();
    CHECK(FileRig::exists(s.t / "New folder (2)")); // unique, never an overwrite
    f.ops->undo();                                  // an empty folder Navigator made is removed
    f.idle();
    CHECK_FALSE(FileRig::exists(s.t / "New folder (2)"));
    CHECK(FileRig::exists(s.t / "New folder"));
    CHECK(f.trash_files().empty());
}

void check_other_pane(Scene& s, const Command& c) {
    check_not_handled(s, c);
    FileRig f(s);
    const bool move = c.id == CommandId::MoveToOtherPane;
    // One pane: nothing to do (the shell posts a notice).
    CHECK_FALSE(f.other_pane_dir().has_value());
    REQUIRE(s.exec(Command{CommandId::SplitRight, 0}));
    s.settle();
    s.go(s.t / "beta"); // the new, active pane shows beta
    REQUIRE(s.exec(Command{CommandId::FocusPreviousPane, 0}));
    s.settle();
    REQUIRE(s.m().current_dir() == s.t.path());
    const std::optional<Path> other = f.other_pane_dir();
    REQUIRE(other.has_value());
    const Path other_dir = other.value_or(Path());
    CHECK(other_dir == s.t / "beta");
    f.select("c.txt");
    f.ops->copy_to_folder(f.target(), other_dir, move);
    f.idle();
    CHECK(FileRig::slurp(s.t / "beta" / "c.txt") == "abc");
    CHECK(FileRig::exists(s.t / "c.txt") == !move);
    CHECK(f.trash_files().empty());
    // Both are undoable.
    f.ops->undo();
    f.idle();
    CHECK(FileRig::slurp(s.t / "c.txt") == "abc");
    CHECK_FALSE(FileRig::exists(s.t / "beta" / "c.txt"));
}

void add_phase7_checks(std::map<std::string, Check>& checks) {
    checks["edit.copy"] = check_copy_cut;
    checks["edit.cut"] = check_copy_cut;
    checks["edit.paste"] = check_paste;
    checks["edit.undo"] = check_undo_redo;
    checks["edit.redo"] = check_undo_redo;
    checks["file.trash"] = check_trash;
    checks["file.delete_permanently"] = check_delete_permanently;
    checks["file.rename"] = check_rename;
    checks["file.new_folder"] = check_new_folder;
    checks["file.copy_to_other_pane"] = check_other_pane;
    checks["file.move_to_other_pane"] = check_other_pane;
}

std::map<std::string, Check> effect_checks() {
    std::map<std::string, Check> checks;
    checks["app.quit"] = check_not_handled;
    checks["view.toggle_fps_overlay"] = check_not_handled;
    checks["view.toggle_theme"] = check_not_handled;

    checks["list.activate"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.click_row(m.find("c.txt"), Modifiers::None);
        s.run(c); // file -> launcher, stays
        REQUIRE(s.launcher.requests.size() == 1);
        CHECK(s.launcher.requests[0].argv.back() == (s.t / "c.txt").string());
        CHECK(m.current_dir() == s.t.path());
        m.click_row(m.find("alpha"), Modifiers::None);
        s.run(c); // folder -> enter
        CHECK(m.current_dir() == s.t / "alpha");
        CHECK(entry_names(m) == std::vector<std::string>{"inner"});
        CHECK(s.launcher.requests.size() == 1);
        CHECK(s.l().tab_count() == 1);
    };
    checks["nav.back"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        s.go(s.t / "beta");
        s.run(c);
        CHECK(m.current_dir() == s.t.path());
        CHECK(m.can_forward());
        s.run(c); // no-op at the start of history
        CHECK(m.current_dir() == s.t.path());
    };
    checks["nav.forward"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        s.go(s.t / "beta");
        m.back();
        s.settle();
        s.run(c);
        CHECK(m.current_dir() == s.t / "beta");
        CHECK_FALSE(m.can_forward());
        s.run(c); // no-op at the end
        CHECK(m.current_dir() == s.t / "beta");
    };
    checks["nav.up"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        s.go(s.t / "alpha" / "inner");
        s.run(c);
        CHECK(m.current_dir() == s.t / "alpha");
        CHECK(cursor_entry(m) == "inner");
        s.run(c);
        CHECK(m.current_dir() == s.t.path());
        CHECK(cursor_entry(m) == "alpha");
    };
    checks["nav.refresh"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.click_row(m.find("f07"), Modifiers::None);
        (void)touch(s.t / "e-new"); // no watcher: only a re-list shows it
        CHECK(m.find("e-new") == kNoRow);
        s.run(c);
        CHECK(m.find("e-new") != kNoRow);
        CHECK(cursor_entry(m) == "f07");
        CHECK(selected_set(m) == std::set<std::string>{"f07"});
    };
    checks["list.select_all"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.click_row(4, Modifiers::None);
        REQUIRE(s.exec(c));
        CHECK(m.selected_count() == m.entries().size());
        CHECK(m.selected_bytes() == 3U); // c.txt; folders count 0
        CHECK(m.cursor() == 4);
    };
    checks["list.clear_selection"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.click_row(6, Modifiers::None);
        m.select_all();
        REQUIRE(s.exec(c));
        CHECK(m.selected_count() == 0);
        CHECK(m.cursor() == 6); // cursor stays
    };
    checks["view.toggle_hidden"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        CHECK(m.find(".hidden") == kNoRow);
        CHECK(m.hidden_count() == 1);
        s.run(c);
        CHECK(m.show_hidden());
        CHECK(m.find(".hidden") != kNoRow);
        s.run(c);
        CHECK_FALSE(m.show_hidden());
        CHECK(m.find(".hidden") == kNoRow);
    };
    for (const char* name : {"list.cursor_up", "list.cursor_down", "list.cursor_page_up",
                             "list.cursor_page_down", "list.cursor_home", "list.cursor_end"}) {
        checks[name] = check_cursor_command;
    }

    // ---- Phase 3 -------------------------------------------------------------------------------
    checks["tab.new"] = [](Scene& s, const Command& c) {
        s.go(s.t / "alpha");
        s.m().sort_by(fs::SortKey::Size);
        s.settle();
        const TabId origin = s.ws->active_tab();
        const fs::SortOptions sort = s.m().sort();
        REQUIRE(s.exec(c));
        s.settle();
        // New active tab right of the old one, on the active tab's folder and sort.
        REQUIRE(s.pane(0).tabs.size() == 2);
        CHECK(s.pane(0).active_tab == 1);
        CHECK(s.ws->active_tab() != origin);
        CHECK(s.m().current_dir() == s.t / "alpha");
        CHECK(s.m().sort() == sort);
        CHECK(entry_names(s.m()) == std::vector<std::string>{"inner"});
        CHECK(s.ws->model(origin)->suspended());
        CHECK(s.lazy_ok());
        // A new tab from the middle goes right after the active tab.
        s.ws->activate_tab(origin);
        s.settle();
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.pane(0).tabs.size() == 3);
        CHECK(s.pane(0).active_tab == 1);
    };
    const Check close_tab = [](Scene& s, const Command& c) {
        // The last tab of the last pane is never closed.
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.l().tab_count() == 1);
        CHECK(s.ws->closed_tabs().empty());
        s.three_tabs();
        const TabId root_tab = s.pane(0).tabs[0].id;
        const TabId beta_tab = s.pane(0).tabs[2].id;
        s.ws->activate_tab(s.pane(0).tabs[1].id); // alpha
        s.settle();
        s.m().click_row(s.m().find("inner"), Modifiers::None);
        REQUIRE(s.exec(c));
        s.settle();
        // Right neighbour becomes active; the closed tab carries its folder and cursor.
        CHECK(s.tab_dirs(0) == std::vector<Path>{s.t.path(), s.t / "beta"});
        CHECK(s.ws->active_tab() == beta_tab);
        REQUIRE(s.ws->closed_tabs().size() == 1);
        const layout::ClosedTab& closed = s.ws->closed_tabs().entries()[0];
        CHECK(closed.state.path == s.t / "alpha");
        CHECK(closed.state.cursor_name == "inner");
        CHECK(closed.index == 1);
        CHECK(s.lazy_ok());
        // Closing the last tab of the bar activates its left neighbour.
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.ws->active_tab() == root_tab);
        CHECK(s.ws->closed_tabs().size() == 2);
        CHECK(s.lazy_ok());
    };
    checks["tab.close"] = close_tab;
    checks["tab.reopen_closed"] = [](Scene& s, const Command& c) {
        REQUIRE(s.exec(c)); // nothing closed: no-op
        s.settle();
        CHECK(s.l().tab_count() == 1);
        s.three_tabs();
        s.ws->activate_tab(s.pane(0).tabs[1].id); // alpha
        s.settle();
        s.m().click_row(s.m().find("inner"), Modifiers::None);
        REQUIRE(s.exec(Command{CommandId::CloseTab, 0}));
        s.settle();
        REQUIRE(s.ws->closed_tabs().size() == 1);
        REQUIRE(s.exec(c));
        s.settle();
        // Back at its old index, active, on its folder and cursor.
        CHECK(s.tab_dirs(0) == std::vector<Path>{s.t.path(), s.t / "alpha", s.t / "beta"});
        CHECK(s.pane(0).active_tab == 1);
        CHECK(s.m().current_dir() == s.t / "alpha");
        CHECK(cursor_entry(s.m()) == "inner");
        CHECK(s.ws->closed_tabs().empty());
        CHECK(s.lazy_ok());
    };
    const Check next_previous = [](Scene& s, const Command& c) {
        s.three_tabs(); // active: index 2
        const bool next = c.id == CommandId::NextTab;
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.pane(0).active_tab == (next ? 0U : 1U)); // next wraps
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.pane(0).active_tab == (next ? 1U : 0U));
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.pane(0).active_tab == 2U); // previous wraps
        CHECK(s.m().current_dir() == s.t / "beta");
        CHECK(s.lazy_ok());
        // Only within the active pane.
        REQUIRE(s.exec(Command{CommandId::SplitRight, 0}));
        s.settle();
        const PaneId pane = s.l().active_pane();
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.l().active_pane() == pane);
        CHECK(s.active_pane().tabs.size() == 1);
    };
    checks["tab.next"] = next_previous;
    checks["tab.previous"] = next_previous;
    const Check move_tab = [](Scene& s, const Command& c) {
        s.three_tabs();
        const bool left = c.id == CommandId::MoveTabLeft;
        const std::size_t start = left ? 2 : 0;
        const TabId moving = s.pane(0).tabs[start].id;
        s.ws->activate_tab(moving);
        s.settle();
        const Path dir = s.m().current_dir();
        for (std::size_t step = 1; step <= 3; ++step) { // the third step hits the end: no wrap
            REQUIRE(s.exec(c));
            s.settle();
            const std::size_t want =
                left ? (step >= 2 ? 0 : 2 - step) : std::min<std::size_t>(step, 2);
            CHECK(s.pane(0).active_tab == want);
            CHECK(s.pane(0).tabs[want].id == moving);
            CHECK(s.ws->active_tab() == moving);
            CHECK(s.m().current_dir() == dir);
        }
        CHECK(s.lazy_ok());
    };
    checks["tab.move_left"] = move_tab;
    checks["tab.move_right"] = move_tab;
    checks["tab.duplicate"] = [](Scene& s, const Command& c) {
        s.go(s.t / "alpha");
        s.m().click_row(s.m().find("inner"), Modifiers::None);
        const TabId origin = s.ws->active_tab();
        REQUIRE(s.exec(c));
        s.settle();
        REQUIRE(s.pane(0).tabs.size() == 2);
        CHECK(s.pane(0).active_tab == 1);
        CHECK(s.ws->active_tab() != origin);
        CHECK(s.m().current_dir() == s.t / "alpha");
        CHECK(cursor_entry(s.m()) == "inner");
        CHECK(selected_set(s.m()) == std::set<std::string>{"inner"});
        CHECK(s.m().can_back()); // history is copied
        s.m().back();
        s.settle();
        CHECK(s.m().current_dir() == s.t.path());
        CHECK(s.ws->model(origin)->current_dir() == s.t / "alpha"); // the source is untouched
        CHECK(s.lazy_ok());
    };
    checks["tab.search"] = check_not_handled;
    checks["layout.save"] = check_not_handled;
    checks["layout.load"] = check_not_handled;
    checks["list.open_in_new_tab"] = check_open_in_new_tab;
    checks["pane.split_right"] = [](Scene& s, const Command& c) {
        check_split(s, c, layout::SplitAxis::Horizontal);
    };
    checks["pane.split_down"] = [](Scene& s, const Command& c) {
        check_split(s, c, layout::SplitAxis::Vertical);
    };
    const Check focus_pane = [](Scene& s, const Command& c) {
        REQUIRE(s.exec(c)); // one pane: no-op
        CHECK(s.active_pane_index() == 0);
        const Command split{CommandId::SplitRight, 0};
        REQUIRE(s.exec(split));
        s.settle();
        s.go(s.t / "alpha");
        REQUIRE(s.exec(Command{CommandId::SplitDown, 0}));
        s.settle();
        s.go(s.t / "beta");
        REQUIRE(s.l().pane_count() == 3);
        REQUIRE(s.active_pane_index() == 2);
        const bool next = c.id == CommandId::FocusNextPane;
        const std::vector<std::size_t> want =
            next ? std::vector<std::size_t>{0, 1, 2} : std::vector<std::size_t>{1, 0, 2};
        const std::vector<Path> dirs{s.t.path(), s.t / "alpha", s.t / "beta"};
        for (const std::size_t w : want) {
            REQUIRE(s.exec(c));
            s.settle();
            CHECK(s.active_pane_index() == w);
            CHECK(s.m().current_dir() == dirs[w]);
        }
        CHECK(s.lazy_ok());
    };
    checks["pane.focus_next"] = focus_pane;
    checks["pane.focus_previous"] = focus_pane;
    add_phase4_checks(checks);
    add_phase5_checks(checks);
    add_phase6_checks(checks);
    add_phase7_checks(checks);
    // Phase 8 checks: list.toggle_cursor_selected (workspace/model), and shell-level overlays.
    checks["list.toggle_cursor_selected"] = [](Scene& s, const Command& c) {
        s.m().click_row(0, Modifiers::None);
        CHECK(s.m().is_selected(0));
        REQUIRE(s.exec(c));
        CHECK_FALSE(s.m().is_selected(0));
        REQUIRE(s.exec(c));
        CHECK(s.m().is_selected(0));
    };
    const Check shell_only = [](Scene& s, const Command& c) { CHECK_FALSE(s.exec(c)); };
    checks["palette.open"] = shell_only;
    checks["settings.open"] = shell_only;
    checks["menu.show_context"] = shell_only;
    checks["ops.toggle_panel"] = shell_only;
    // Phase 9A: view mode & zoom commands run at shell level (App persists ViewSettingsStore).
    checks["view.mode_details"] = shell_only;
    checks["view.mode_icons"] = shell_only;
    checks["view.mode_list"] = shell_only;
    checks["view.mode_tree"] = shell_only;
    checks["view.zoom_in"] = shell_only;
    checks["view.zoom_out"] = shell_only;
    checks["view.zoom_reset"] = shell_only;
    checks["file.batch_rename"] = shell_only;
    checks["pane.close"] = [](Scene& s, const Command& c) {
        REQUIRE(s.exec(c)); // the only pane: no-op
        s.settle();
        CHECK(s.l().pane_count() == 1);
        CHECK(s.l().tab_count() == 1);
        REQUIRE(s.exec(Command{CommandId::SplitRight, 0}));
        s.settle();
        s.go(s.t / "alpha");
        REQUIRE(s.exec(Command{CommandId::NewTab, 0}));
        s.settle();
        s.go(s.t / "beta");
        REQUIRE(s.l().pane_count() == 2);
        REQUIRE(s.exec(c));
        s.settle();
        CHECK(s.l().pane_count() == 1);
        CHECK(std::holds_alternative<layout::PaneNode>(s.l().root().node));
        CHECK(s.m().current_dir() == s.t.path());
        // Both tabs of the closed pane, pushed right to left so reopening restores tab order.
        REQUIRE(s.ws->closed_tabs().size() == 2);
        CHECK(s.ws->closed_tabs().entries()[0].state.path == s.t / "beta");
        CHECK(s.ws->closed_tabs().entries()[1].state.path == s.t / "alpha");
        CHECK(s.lazy_ok());
    };
    return checks;
}

} // namespace

TEST_CASE("docs/keymap.md and load_default_keymap agree row for row", "[keymap][e2e]") {
    const DocTables doc = read_doc();
    // Phase 0, Phase 2, Phase 3, Phase 4, Phase 5, Phase 6, Phase 7, Phase 8 and Phase 9 tables.
    CHECK(doc.per_phase.size() == 9);
    CHECK(doc.per_phase.at(0) == 3);
    CHECK(doc.per_phase.at(2) == 25);
    CHECK(doc.per_phase.at(3) == 21);
    CHECK(doc.per_phase.at(4) == 5);
    CHECK(doc.per_phase.at(5) == 4);
    CHECK(doc.per_phase.at(6) == 2);
    CHECK(doc.per_phase.at(7) == 12);
    CHECK(doc.per_phase.at(8) == 8);
    CHECK(doc.per_phase.at(9) == 11);
    REQUIRE(doc.bound.size() == 91); // 80 through Phase 8 + 11 Phase 9 chords
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    const Span<const KeyBinding> bindings = keymap.bindings();
    REQUIRE(bindings.size() == doc.bound.size());
    for (std::size_t i = 0; i < doc.bound.size(); ++i) {
        const DocRow& row = doc.bound[i];
        INFO("keymap.md line " << row.line << ": " << row.chord);
        const KeyChord chord = chord_of(row.chord);
        REQUIRE(chord.key != Key::Unknown);
        CHECK(to_string(chord) == row.chord); // the doc uses canonical text
        CHECK(bindings[i].chord == chord);    // same order as the doc
        CHECK(bindings[i].command == row.command);
        CHECK(bindings[i].arg == row.arg);
    }
    // No chord is bound twice (resolve_command would pick only one).
    std::set<std::string> chords;
    for (const DocRow& row : doc.bound) {
        INFO("keymap.md line " << row.line);
        CHECK(chords.insert(row.chord).second);
    }
    // "(unbound)" rows are registered commands without a default chord.
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    // Phase 3: tab.close_left, layout.delete, window.new. Phase 4: sidebar.focus,
    // bookmarks.rename / move_up / move_down, recents.clear / toggle_tracking,
    // view.toggle_view_lock (nav.focus_address_bar became Ctrl+L). Phase 5: list.clear_filter,
    // search.toggle_fuzzy, search.cancel, search.rebuild_index. Phase 6: the seven inspector.*
    // header-button commands. Phase 7: file.open_terminal_here (reserved no-op), ops.cancel,
    // ops.toggle_pause, ops.toggle_panel, trash.open, trash.restore, trash.empty.
    std::set<std::string> unbound_names;
    for (const DocRow& row : doc.unbound) {
        unbound_names.insert(row.command);
    }
    // Phase 8 moved ops.toggle_panel from the historical Phase 7 unbound row to Ctrl+J.
    unbound_names.erase("ops.toggle_panel");
    CHECK(unbound_names == std::set<std::string>{"tab.close_left",
                                                 "layout.delete",
                                                 "window.new",
                                                 "sidebar.focus",
                                                 "bookmarks.rename",
                                                 "bookmarks.move_up",
                                                 "bookmarks.move_down",
                                                 "recents.clear",
                                                 "recents.toggle_tracking",
                                                 "view.toggle_view_lock",
                                                 "list.clear_filter",
                                                 "search.toggle_fuzzy",
                                                 "search.cancel",
                                                 "search.rebuild_index",
                                                 "inspector.toggle_lock",
                                                 "inspector.zoom_fit",
                                                 "inspector.zoom_1to1",
                                                 "inspector.play",
                                                 "inspector.cycle_placement",
                                                 "inspector.toggle_grid",
                                                 "inspector.toggle_minimap",
                                                 "file.open_terminal_here",
                                                 "ops.cancel",
                                                 "ops.toggle_pause",
                                                 "trash.open",
                                                 "trash.restore",
                                                 "trash.empty",
                                                 "keymap.edit",
                                                 "keymap.reset_defaults",
                                                 "keymap.export",
                                                 "open_with.show",
                                                 "view.cycle_mode",
                                                 "view.group_none",
                                                 "view.group_date",
                                                 "view.group_type",
                                                 "view.group_size",
                                                 "view.group_cycle",
                                                 "view.lock_folder",
                                                 "view.lock_hierarchy",
                                                 "view.lock_global",
                                                 "view.tree_expand",
                                                 "view.tree_collapse",
                                                 "view.tree_collapse_all",
                                                 "view.toggle_animations",
                                                 "view.toggle_alternating_rows",
                                                 "view.cycle_density",
                                                 "view.font_size_increase",
                                                 "view.font_size_decrease",
                                                 "view.font_size_reset",
                                                 "settings.appearance"});
    REQUIRE(doc.unbound.size() == 51);
    for (const DocRow& row : doc.unbound) {
        if (row.command == "ops.toggle_panel") {
            continue; // bound by Phase 8 table
        }
        INFO("keymap.md line " << row.line << ": " << row.command);
        CHECK(registry.find(row.command) != nullptr);
        CHECK(keymap.chords_for(row.command).empty());
    }
}

TEST_CASE("Every default chord resolves to its command and has its effect on the workspace",
          "[keymap][e2e][browser][workspace]") {
    const DocTables doc = read_doc();
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    const std::map<std::string, Check> checks = effect_checks();

    std::set<std::string> covered;
    for (const DocRow& row : doc.bound) {
        INFO("keymap.md line " << row.line << ": " << row.chord << " -> " << row.command);
        const Command command = command_of(keymap, registry, chord_of(row.chord));
        REQUIRE(command.id != CommandId::None);
        CHECK(to_string(command.id) == row.command);
        CHECK(command.arg == row.arg);
        const auto it = checks.find(row.command);
        REQUIRE(it != checks.end()); // add an effect check for a new default binding
        Scene scene;
        it->second(scene, command);
        CHECK(invariant_violation(scene.m()).empty());
        CHECK(scene.lazy_ok());
        covered.insert(row.command);
    }
    // Every registered command has a default chord or an "(unbound)" doc row, not both.
    std::set<std::string> unbound;
    for (const DocRow& row : doc.unbound) {
        unbound.insert(row.command);
    }
    unbound.erase("ops.toggle_panel"); // bound in Phase 8 table
    for (const CommandInfo& info : registry.all()) {
        const std::string name(info.name);
        INFO(name);
        CHECK(covered.contains(name) != unbound.contains(name));
    }

    // Near misses do not resolve: extra modifiers, chords reserved for later phases (Ctrl+N:
    // window.new, deferred; Ctrl+Shift+P, Ctrl+P; Ctrl+C and Ctrl+Shift+N are bound since
    // Phase 7) and unbound keys,
    // including Phase 4 and Phase 5 chords with an extra or a missing modifier (Ctrl+E is
    // search.flatten since Phase 5).
    for (const char* text :
         {"Ctrl+N", "Ctrl+P", "Ctrl+Alt+Left", "Alt+Shift+Left",
          // Phase 7 near misses: window-manager keys, a risky "delete" key,
          // modifier variants of the new file chords.
          "Alt+F7", "Alt+F8", "F8", "Ctrl+Delete", "Alt+Delete", "Ctrl+Shift+Y", "Ctrl+Shift+X",
          "Ctrl+Shift+V", "Ctrl+Alt+Z", "Ctrl+F7", "Ctrl+Shift+F7", "Ctrl+F2", "Ctrl+Shift+Delete",
          "Ctrl+Alt+N", "Ctrl+Shift+F3", "Ctrl+F6", "Ctrl+Alt+T", "Shift+Enter", "Ctrl+Shift+B",
          "Ctrl+Alt+L", "Ctrl+Shift+G", "Alt+D", "Ctrl+Alt+C", "Ctrl+Alt+E", "Alt+E", "Ctrl+Alt+F",
          "Alt+F", "Ctrl+Alt+Shift+E", "Shift+F", "Shift+Space", "Alt+Space", "Ctrl+Shift+Space",
          "Alt+Enter", "Shift+KeypadEnter", "Ctrl+Alt+KeypadEnter"}) {
        INFO(text);
        CHECK_FALSE(resolve_command(keymap, registry, chord_of(text)));
    }
}

TEST_CASE("Unbound Phase 3 commands through the workspace", "[keymap][e2e][workspace]") {
    Scene s;
    // window.new: reserved, handled as a no-op.
    const std::uint64_t serial = s.ws->persist_serial();
    CHECK(s.exec(Command{CommandId::NewWindow, 0}));
    CHECK(s.l().tab_count() == 1);
    CHECK(s.ws->persist_serial() == serial);
    // layout.delete: a popup of the shell.
    CHECK_FALSE(s.exec(Command{CommandId::DeleteLayout, 0}));
    // tab.close_left with a TabId arg (tab context menu).
    s.three_tabs();
    const TabId beta = s.pane(0).tabs[2].id;
    CHECK(s.exec(Command{CommandId::CloseLeftTabs, beta.value}));
    s.settle();
    CHECK(s.tab_dirs(0) == std::vector<Path>{s.t / "beta"});
    REQUIRE(s.ws->closed_tabs().size() == 2);
    // Pushed right to left (BUG-P3-2) so reopening restores the original order.
    CHECK(s.ws->closed_tabs().entries()[0].state.path == s.t / "alpha");
    CHECK(s.ws->closed_tabs().entries()[1].state.path == s.t.path());
    CHECK(s.lazy_ok());
    // Tab commands with a stale id do nothing.
    CHECK(s.exec(Command{CommandId::CloseTab, 0xFFFFFFF0U}));
    CHECK(s.exec(Command{CommandId::DuplicateTab, std::uint64_t{1} << 40}));
    CHECK(s.l().tab_count() == 1);
}

TEST_CASE("Unbound Phase 5 commands through the workspace", "[keymap][e2e][workspace][search]") {
    Scene s;
    BrowserModel& m = s.m();
    // list.clear_filter (chip button, Escape cascade).
    m.set_filter_text("f2");
    REQUIRE(m.row_count() == 10); // f20..f29 (f02 has no contiguous "f2")
    CHECK(s.exec(Command{CommandId::ClearFilter, 0}));
    CHECK_FALSE(m.filter_active());
    CHECK(m.row_count() == 33);
    // search.toggle_fuzzy: "f2" in Fuzzy also finds the gapped f?2 names.
    m.set_filter_text("f2");
    CHECK(s.exec(Command{CommandId::ToggleFuzzy, 0}));
    CHECK(m.match_mode() == search::MatchMode::Fuzzy);
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.row_count() == 12); // + f02 and f12
    CHECK(s.exec(Command{CommandId::ToggleFuzzy, 0}));
    CHECK(m.match_mode() == search::MatchMode::Substring);
    CHECK(m.row_count() == 10);
    m.clear_filter();
    // search.cancel: stops a running flatten walk and keeps the rows found so far.
    REQUIRE(s.exec(Command{CommandId::ToggleFlatten, 0}));
    CHECK(s.exec(Command{CommandId::CancelSearch, 0}));
    CHECK_FALSE(m.flatten_progress().walking);
    REQUIRE(m.settle_search(kSearchWait));
    CHECK(m.row_count() <= flat_scene().size());
    for (const std::string& rel : row_rels(m)) {
        CHECK(flat_scene().contains(rel));
    }
    CHECK(s.exec(Command{CommandId::ToggleFlatten, 0}));
    CHECK_FALSE(m.flattened());
    // search.rebuild_index needs the App's index service.
    CHECK_FALSE(s.exec(Command{CommandId::RebuildIndex, 0}));
    CHECK(m.row_count() == 33);
}

TEST_CASE("Space typed into a focused text field never inspects", "[keymap][e2e][inspector]") {
    Scene s;
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    const KeyChord space = chord_of("Space");
    const KeyChord shift_space = chord_of("Shift+Space");
    // With the list focused Space is view.inspect (the binding the fields must shadow).
    REQUIRE(command_of(keymap, registry, space).id == CommandId::Inspect);
    // App::Impl::handle_key step 0: every focused field consumes Space and Shift+Space before the
    // keymap; other Space chords are not text (Ctrl+Space is reserved and resolves to nothing).
    CHECK(ui::field_consumes_as_text(space));
    CHECK(ui::field_consumes_as_text(shift_space));
    for (const char* text : {"Ctrl+Space", "Alt+Space", "Ctrl+Shift+Space", "Enter", "S"}) {
        INFO(text);
        CHECK_FALSE(ui::field_consumes_as_text(chord_of(text)));
    }
    // Type-to-filter never starts with a Space from the list (Space stays view.inspect) but a
    // space inside the typed text is text.
    CHECK_FALSE(ui::starts_type_to_filter(ui::KeyboardOwner::List, Modifiers::None, " "));
    CHECK_FALSE(ui::starts_type_to_filter(ui::KeyboardOwner::List, Modifiers::Shift, " x"));
    CHECK(ui::starts_type_to_filter(ui::KeyboardOwner::List, Modifiers::None, "f 1"));
    CHECK_FALSE(ui::starts_type_to_filter(ui::KeyboardOwner::TextField, Modifiers::None, "f"));

    std::unique_ptr<preview::IPreviewService> service =
        ui::create_preview_backend(ui::PreviewBackendOptions{});
    inspector::Controller ctl(service.get());
    std::uint64_t inspects = 0;
    const std::uint64_t serial = s.ws->persist_serial();
    // The shell's key path for a focused field: consumed chords stop; anything else resolves.
    const auto key = [&](const KeyChord& chord, bool field_focused) {
        if (field_focused && ui::field_consumes_as_text(chord)) {
            return;
        }
        if (const std::optional<Command> c = resolve_command(keymap, registry, chord);
            c && c->id == CommandId::Inspect) {
            ++inspects;
            CHECK_FALSE(s.exec(*c));
        }
    };

    // The folder filter: "f", Space, "1" -> the filter text "f 1" (SDL text input arrives after
    // the key event; App::Impl::handle_text inserts it into the focused field).
    BrowserModel& m = s.m();
    m.filter_edit().set({});
    const std::vector<std::pair<std::string_view, std::string_view>> keys{
        {"F", "f"}, {"Space", " "}, {"1", "1"}};
    for (const auto& [chord, typed] : keys) {
        key(chord_of(chord), true);
        if (m.filter_edit().insert(typed)) {
            m.filter_edited();
        }
    }
    key(shift_space, true);
    CHECK(m.filter_edit().insert(" "));
    m.filter_edited();
    CHECK(m.filter_edit().text() == "f 1 ");
    // Space separates query terms (Phase 5): "f" and "1" -> f01, f10..f19, f21.
    CHECK(m.row_count() == 12);
    // The address bar and GoTo (PathEntry fields) take it as text too.
    double now_ms = 1000.0;
    for (const ui::PathEntryMode mode : {ui::PathEntryMode::AddressBar, ui::PathEntryMode::GoTo}) {
        ui::PathEntry e(mode);
        e.open("", false, s.m().current_dir(), s.t.path(), {}, now_ms);
        key(space, true);
        CHECK(e.edit().insert("a b"));
        e.changed(now_ms);
        CHECK(e.edit().text() == "a b");
    }
    CHECK(inspects == 0);
    CHECK_FALSE(s.ws->inspector().visible);
    CHECK(s.ws->persist_serial() == serial);
    CHECK_FALSE(ctl.visible());
    CHECK(ctl.submitted() == 0);
    // Control: the same Space with the list focused does reach view.inspect.
    key(space, false);
    CHECK(inspects == 1);
    if (service) {
        service->shutdown();
    }
}

TEST_CASE("Activate through the keymap never spawns a process", "[keymap][e2e][launcher]") {
    Scene s;
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    for (const char* chord : {"Enter", "KeypadEnter", "Ctrl+Enter", "Ctrl+KeypadEnter"}) {
        const Command c = command_of(keymap, registry, chord_of(chord));
        REQUIRE((c.id == CommandId::Activate || c.id == CommandId::OpenInNewTab));
        for (const char* name : {"c.txt", "f01", "f30"}) {
            s.m().click_row(s.m().find(name), Modifiers::None);
            s.run(c);
        }
    }
    CHECK(s.launcher.requests.size() == 12);
    CHECK(s.l().tab_count() == 1); // files never open a tab
    for (const platform::LaunchRequest& r : s.launcher.requests) {
        CHECK(r.argv.front() == "xdg-open"); // what would have run, recorded only
    }
    int status = 0;
    errno = 0;
    CHECK(::waitpid(-1, &status, WNOHANG) == -1); // no child, running or zombie
    CHECK(errno == ECHILD);
}

// ---- Phase 7: no file command fires while typing -----------------------------------------------

TEST_CASE("Text fields consume the Phase 7 chords, so no file command fires while typing",
          "[keymap][e2e][fileops][safety]") {
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    const std::function<std::string()> no_paste = [] { return std::string("PASTED"); };

    // Chords the field edits itself. Each one DOES resolve to a file command (that is the danger),
    // and the field's apply_field_key must report it handled so the shell never reaches the keymap.
    const std::map<std::string, std::string> consumed{
        {"Ctrl+C", "edit.copy"},  {"Ctrl+X", "edit.cut"},
        {"Ctrl+V", "edit.paste"}, {"Ctrl+Z", "edit.undo"},
        {"Ctrl+Y", "edit.redo"},  {"Ctrl+Shift+Z", "edit.redo"},
        {"Delete", "file.trash"}, {"Shift+Delete", "file.delete_permanently"},
    };
    for (const auto& [text, command] : consumed) {
        INFO(text);
        const KeyChord chord = chord_of(text);
        const Command resolved = command_of(keymap, registry, chord);
        CHECK(to_string(resolved.id) == command);
        ui::TextEdit field(64);
        field.set("hello world", true);
        const ui::FieldKeyResult r = ui::apply_field_key(field, chord, no_paste);
        CHECK(r.handled);
        if (text == "Shift+Delete" || text == "Delete") {
            // Delete / Shift+Delete delete TEXT here, never files.
            CHECK(field.text().empty());
        }
        if (text == "Ctrl+V") {
            CHECK(field.text() == "PASTED");
        }
    }
    // The filter field (Phase 5 routing): Delete, Shift+Delete, Ctrl+V/Z/Y, Ctrl+Shift+Z are the
    // field's; Ctrl+C / Ctrl+X only with a text selection, otherwise they copy / cut the files.
    for (const char* text :
         {"Delete", "Shift+Delete", "Ctrl+V", "Ctrl+Z", "Ctrl+Y", "Ctrl+Shift+Z"}) {
        INFO(text);
        CHECK(ui::route_filter_key(chord_of(text), false) == ui::FilterKey::Edit);
        CHECK(ui::route_filter_key(chord_of(text), true) == ui::FilterKey::Edit);
    }
    for (const char* text : {"Ctrl+C", "Ctrl+X"}) {
        INFO(text);
        CHECK(ui::route_filter_key(chord_of(text), true) == ui::FilterKey::Edit);
        CHECK(ui::route_filter_key(chord_of(text), false) == ui::FilterKey::Keymap);
    }
    // The chords that are NOT text editing go on to the keymap (F2, Ctrl+Shift+N, F7, Shift+F7):
    // the shell's focus scoping (docs/keymap.md) guards them for popups, the address bar and the
    // sidebar; the real key path is exercised in ops_app_smoke.sh.
    for (const char* text : {"F2", "Ctrl+Shift+N", "F7", "Shift+F7"}) {
        INFO(text);
        CHECK(ui::route_filter_key(chord_of(text), false) == ui::FilterKey::Keymap);
        ui::TextEdit field(64);
        field.set("keep", true);
        const ui::FieldKeyResult r = ui::apply_field_key(field, chord_of(text), no_paste);
        CHECK_FALSE(r.handled);
        CHECK(field.text() == "keep");
    }
    // Space-like safety from Phase 6 still holds.
    CHECK(ui::field_consumes_as_text(chord_of("Space")));
}

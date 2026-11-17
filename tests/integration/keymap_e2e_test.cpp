// Keymap end to end (owner: qa): every default chord in docs/keymap.md -> parse_key_chord ->
// load_default_keymap + register_builtin_commands -> resolve_command -> ui::Workspace::execute
// (tab, pane and list.open_in_new_tab commands; browser commands go on to the active tab's
// BrowserModel), and the effect on a fixture folder. The doc is parsed at test time (every
// "## Phase N defaults" table), so a row added to the doc without a binding (or a binding without
// a row, or a row without an effect check here) fails.
//
// Activate on a file goes to a recording launcher; nothing is ever spawned. The Workspace has no
// config root: nothing is written outside the fixture.
#include <algorithm>
#include <cerrno>
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
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::layout::PaneId;
using navigator::layout::TabId;
using navigator::ui::BrowserModel;
using navigator::ui::kNoRow;
using navigator::ui::Workspace;

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
    std::set<std::size_t> expect;
    if (extend) {
        for (std::size_t i = std::min<std::size_t>(10, want); i <= std::max<std::size_t>(10, want);
             ++i) {
            expect.insert(i);
        }
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
        REQUIRE(s.ws->closed_tabs().size() == 2); // both tabs of the closed pane, in tab order
        CHECK(s.ws->closed_tabs().entries()[0].state.path == s.t / "alpha");
        CHECK(s.ws->closed_tabs().entries()[1].state.path == s.t / "beta");
        CHECK(s.lazy_ok());
    };
    return checks;
}

} // namespace

TEST_CASE("docs/keymap.md and load_default_keymap agree row for row", "[keymap][e2e]") {
    const DocTables doc = read_doc();
    // Phase 0, Phase 2 and Phase 3 tables.
    CHECK(doc.per_phase.size() == 3);
    CHECK(doc.per_phase.at(0) == 3);
    CHECK(doc.per_phase.at(2) == 25);
    CHECK(doc.per_phase.at(3) == 21);
    REQUIRE(doc.bound.size() == 49);
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
    REQUIRE(doc.unbound.size() == 4);
    for (const DocRow& row : doc.unbound) {
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
    for (const CommandInfo& info : registry.all()) {
        const std::string name(info.name);
        INFO(name);
        CHECK(covered.contains(name) != unbound.contains(name));
    }

    // Near misses do not resolve: extra modifiers, chords reserved for later phases (Ctrl+N:
    // window.new, deferred; Ctrl+E, Ctrl+B, Ctrl+Shift+P, Ctrl+Shift+N) and unbound keys.
    for (const char* text :
         {"Ctrl+N", "Ctrl+E", "Ctrl+B", "Ctrl+Shift+P", "Ctrl+Shift+N", "Ctrl+Alt+Left",
          "Alt+Shift+Left", "Delete", "Ctrl+Shift+F3", "Ctrl+F6", "Ctrl+Alt+T", "Shift+Enter"}) {
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
    CHECK(s.ws->closed_tabs().entries()[0].state.path == s.t.path());
    CHECK(s.ws->closed_tabs().entries()[1].state.path == s.t / "alpha");
    CHECK(s.lazy_ok());
    // Tab commands with a stale id do nothing.
    CHECK(s.exec(Command{CommandId::CloseTab, 0xFFFFFFF0U}));
    CHECK(s.exec(Command{CommandId::DuplicateTab, std::uint64_t{1} << 40}));
    CHECK(s.l().tab_count() == 1);
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

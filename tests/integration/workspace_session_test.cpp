// Phase 3 workspace persistence and lazy tabs end to end (owner: qa): ui::Workspace +
// ui::SessionWriter + core's session / named-layout files on fixture directories.
//   - 2 panes x 3 tabs: Workspace -> SessionWriter -> session.json -> load_session -> a new
//     Workspace; paths, sort, hidden, cursor, active tab/pane, closed tabs and window come back.
//   - Workspace JSON and core JSON are interchangeable; save -> load -> save is a fixed point.
//   - A corrupt session.json is moved to .bad and the workspace starts on one tab.
//   - Hammer: the UI thread mutates the layout and submits snapshots while readers parse the
//     file: never torn, never invalid, no temporary files left behind.
//   - Named layouts with Turkish names round-trip through the Workspace.
//   - Lazy tabs: only visible tabs own threads and inotify instances (counted in /proc/self);
//     a background tab relists on activation and sees changes made while it was suspended.
// Files are created only inside mkdtemp fixtures. No launcher: nothing is ever spawned.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/watch.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"
#include "workspace/session_writer.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::layout::PaneId;
using navigator::layout::TabId;
using navigator::layout::TabState;
using navigator::ui::BrowserModel;
using navigator::ui::kNoRow;
using navigator::ui::SaveScheduler;
using navigator::ui::SessionWriter;
using navigator::ui::Workspace;

namespace {

std::string read_file(const Path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const Path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// Folders d1..d6 with three files and a hidden one each, sizes differ per file.
void populate(const TempDir& t) {
    for (int d = 1; d <= 6; ++d) {
        const Path dir = t / ("d" + std::to_string(d));
        std::filesystem::create_directories(dir / "sub");
        write_file(dir / "a.txt", "a");
        write_file(dir / "b.txt", "bbbbbbbb");
        write_file(dir / "c.txt", "cccc");
        write_file(dir / ".h", "");
    }
}

Workspace::Options options(const TempDir& t, const Path& config, WatcherFactory watcher = {}) {
    Workspace::Options o;
    o.home = t.path();
    o.config_root = config;
    o.model.watcher_factory = std::move(watcher);
    return o;
}

void settle(Workspace& ws) {
    for (int round = 0; round < 4; ++round) {
        for (const TabId tab : ws.visible_tabs()) {
            REQUIRE(ws.model(tab) != nullptr);
            REQUIRE(ws.model(tab)->wait_listing(kBrowserWait));
        }
        (void)ws.frame_start();
    }
    REQUIRE(ws.settled());
}

void go(Workspace& ws, const Path& dir) {
    ws.active_model().navigate_to(dir);
    settle(ws);
}

void cursor_to(Workspace& ws, std::string_view name) {
    BrowserModel& m = ws.active_model();
    const std::size_t row = m.find(name);
    REQUIRE(row != kNoRow);
    m.click_row(row, Modifiers::None);
}

bool exec(Workspace& ws, CommandId id, std::uint64_t arg = 0) {
    const bool handled = ws.execute(Command{id, arg});
    settle(ws);
    return handled;
}

const layout::PaneNode& pane_at(const Workspace& ws, std::size_t index) {
    const std::vector<PaneId> order = ws.layout().pane_order();
    REQUIRE(index < order.size());
    return *ws.layout().find_pane(order[index]);
}

std::size_t active_pane_index(const Workspace& ws) {
    const std::vector<PaneId> order = ws.layout().pane_order();
    return static_cast<std::size_t>(
        std::find(order.begin(), order.end(), ws.layout().active_pane()) - order.begin());
}

std::vector<Path> closed_paths(const Workspace& ws) {
    std::vector<Path> out;
    for (const layout::ClosedTab& c : ws.closed_tabs().entries()) {
        out.push_back(c.state.path);
    }
    return out;
}

// Only the visible tab of each pane has a live model; every tab has a model.
bool lazy_ok(const Workspace& ws) {
    const std::vector<TabId> visible = ws.visible_tabs();
    for (const PaneId p : ws.layout().pane_order()) {
        for (const layout::Tab& tab : ws.layout().find_pane(p)->tabs) {
            const BrowserModel* m = ws.model(tab.id);
            const bool shown = std::find(visible.begin(), visible.end(), tab.id) != visible.end();
            if (m == nullptr || m->suspended() == shown) {
                return false;
            }
        }
    }
    return true;
}

// The 2 x 3 scene of the roadmap, built through commands:
//   pane 0: d1 (size sort, cursor b.txt) | d2* (hidden on, cursor .h) | d3 (cursor c.txt)
//   pane 1 (active): d4* (cursor sub)   | d5 (modified, descending)  | d6 (cursor a.txt)
//   closed: x1, x2 (oldest first); window 1000x700 at (-20, 30).
void build_two_by_three(Workspace& ws, const TempDir& t) {
    ws.start(std::nullopt, t / "d1");
    settle(ws);
    for (const char* closed : {"x1", "x2"}) {
        std::filesystem::create_directories(t / closed);
        REQUIRE(exec(ws, CommandId::NewTab));
        go(ws, t / closed);
        REQUIRE(exec(ws, CommandId::CloseTab));
    }
    ws.active_model().sort_by(fs::SortKey::Size);
    settle(ws);
    cursor_to(ws, "b.txt");
    REQUIRE(exec(ws, CommandId::NewTab));
    go(ws, t / "d2");
    ws.active_model().sort_by(fs::SortKey::Name); // the new tab inherited Size: back to Name
    settle(ws);
    ws.active_model().toggle_hidden();
    settle(ws);
    cursor_to(ws, ".h");
    REQUIRE(exec(ws, CommandId::NewTab));
    go(ws, t / "d3");
    cursor_to(ws, "c.txt");
    ws.activate_tab(pane_at(ws, 0).tabs[1].id);
    settle(ws);

    REQUIRE(exec(ws, CommandId::SplitRight));
    go(ws, t / "d4");
    ws.active_model().toggle_hidden(); // inherited from d2: off again
    settle(ws);
    cursor_to(ws, "sub");
    REQUIRE(exec(ws, CommandId::NewTab));
    go(ws, t / "d5");
    ws.active_model().sort_by(fs::SortKey::Modified);
    settle(ws);
    REQUIRE(exec(ws, CommandId::NewTab));
    go(ws, t / "d6");
    cursor_to(ws, "a.txt");
    ws.activate_tab(pane_at(ws, 1).tabs[0].id);
    settle(ws);

    layout::WindowGeometry& w = ws.window();
    w.has_position = true;
    w.x = -20;
    w.y = 30;
    w.width = 1000;
    w.height = 700;
    (void)ws.sync();
}

void check_two_by_three(Workspace& ws, const TempDir& t) {
    REQUIRE(ws.layout().pane_count() == 2);
    REQUIRE(ws.layout().validate());
    const auto* split = std::get_if<layout::SplitNode>(&ws.layout().root().node);
    REQUIRE(split != nullptr);
    CHECK(split->axis == layout::SplitAxis::Horizontal);
    CHECK(active_pane_index(ws) == 1);
    CHECK(pane_at(ws, 0).active_tab == 1);
    CHECK(pane_at(ws, 1).active_tab == 0);
    CHECK(closed_paths(ws) == std::vector<Path>{t / "x1", t / "x2"});
    CHECK(ws.session().window.has_position);
    CHECK(ws.session().window.x == -20);
    CHECK(ws.session().window.width == 1000);
    CHECK(ws.session().window.height == 700);
    CHECK(lazy_ok(ws));

    struct Want {
        const char* dir;
        fs::SortKey key;
        bool descending;
        bool hidden;
        const char* cursor;
    };
    const Want want[2][3] = {
        {{"d1", fs::SortKey::Size, true, false, "b.txt"},
         {"d2", fs::SortKey::Name, false, true, ".h"},
         {"d3", fs::SortKey::Name, false, true, "c.txt"}},
        {{"d4", fs::SortKey::Name, false, false, "sub"},
         {"d5", fs::SortKey::Modified, true, false, nullptr},
         {"d6", fs::SortKey::Modified, true, false, "a.txt"}},
    };
    for (std::size_t p = 0; p < 2; ++p) {
        REQUIRE(pane_at(ws, p).tabs.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            const Want& w = want[p][i];
            INFO("pane " << p << " tab " << i << " (" << w.dir << ")");
            const TabState& s = pane_at(ws, p).tabs[i].state;
            CHECK(s.path == t / w.dir);
            CHECK(s.sort.key == w.key);
            CHECK(s.sort.descending == w.descending);
            CHECK(s.show_hidden == w.hidden);
            if (w.cursor != nullptr) {
                CHECK(s.cursor_name == w.cursor);
            }
        }
    }
    // Every tab lists its folder with its sort, hidden setting and cursor when activated.
    for (std::size_t p = 0; p < 2; ++p) {
        for (std::size_t i = 0; i < 3; ++i) {
            const Want& w = want[p][i];
            INFO("activate pane " << p << " tab " << i << " (" << w.dir << ")");
            ws.activate_tab(pane_at(ws, p).tabs[i].id);
            settle(ws);
            const BrowserModel& m = ws.active_model();
            CHECK(m.current_dir() == t / w.dir);
            CHECK(m.sort().key == w.key);
            CHECK(m.show_hidden() == w.hidden);
            CHECK((m.find(".h") != kNoRow) == w.hidden);
            if (w.cursor != nullptr) {
                CHECK(cursor_entry(m) == w.cursor);
            }
            CHECK(invariant_violation(m).empty());
            CHECK(lazy_ok(ws));
        }
    }
}

std::size_t count_tasks() {
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return n;
}

std::size_t count_inotify_fds() {
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code lec;
        const Path target = std::filesystem::read_symlink(it->path(), lec);
        if (!lec && target.string() == "anon_inode:inotify") {
            ++n;
        }
    }
    return n;
}

// Threads finish exiting shortly after join() returns in the kernel's view; poll briefly.
std::size_t tasks_settled(std::size_t want) {
    std::size_t n = count_tasks();
    for (int i = 0; i < 100 && n != want; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        n = count_tasks();
    }
    return n;
}

WatcherFactory inotify_factory() {
    return [] { return create_inotify_watcher(); };
}

// Names of everything in `dir` (no recursion).
std::set<std::string> dir_names(const Path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        out.insert(it->path().filename().string());
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------------------------

TEST_CASE("Two panes with three tabs each survive a save and a restart", "[workspace][session]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    std::string saved;
    {
        Workspace ws(options(t, cfg));
        build_two_by_three(ws, t);
        check_two_by_three(ws, t); // the live scene itself (activates every tab)
        ws.activate_tab(pane_at(ws, 0).tabs[1].id);
        ws.activate_tab(pane_at(ws, 1).tabs[0].id);
        settle(ws);
        (void)ws.sync();
        SessionWriter writer(layout::session_file(cfg));
        writer.submit(ws.snapshot());
        REQUIRE(writer.flush());
        CHECK(writer.writes() == 1);
        const Result<std::string> json = layout::to_json(ws.snapshot());
        REQUIRE(json);
        saved = read_file(layout::session_file(cfg));
        CHECK(saved == *json);
    }
    // Atomic write leaves only the session file; private modes.
    CHECK(dir_names(cfg) == std::set<std::string>{"session.json"});
    struct stat st {};
    REQUIRE(::stat(layout::session_file(cfg).c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(cfg.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);

    Result<layout::Session> loaded = layout::load_session(layout::session_file(cfg));
    REQUIRE(loaded);
    Workspace restored(options(t, cfg));
    restored.start(std::move(*loaded), Path());
    settle(restored);
    // Only the two visible tabs are live; the other four wait for activation.
    std::size_t live = 0;
    for (const PaneId p : restored.layout().pane_order()) {
        for (const layout::Tab& tab : restored.layout().find_pane(p)->tabs) {
            live += restored.model(tab.id)->suspended() ? 0U : 1U;
        }
    }
    CHECK(live == 2);
    CHECK(restored.active_model().current_dir() == t / "d4");
    CHECK(cursor_entry(restored.active_model()) == "sub");
    check_two_by_three(restored, t);
}

TEST_CASE("Workspace JSON and core JSON are interchangeable", "[workspace][session][json]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    // A core-built session: [d1 (2 tabs) | [d3 / d4 (raw non-UTF-8 name)]] with 0.3 / 0.7.
    const std::string raw_name = std::string("ham\xff\xfe") + "ad";
    std::filesystem::create_directories(t / "d4" / raw_name);
    layout::Session core;
    TabState s1;
    s1.path = t / "d1";
    s1.cursor_name = "c.txt";
    s1.scroll_rows = 0.0;
    core.layout = layout::Layout(s1);
    const PaneId p1 = core.layout.active_pane();
    TabState s2;
    s2.path = t / "d2";
    s2.show_hidden = true;
    s2.sort.key = fs::SortKey::Extension;
    s2.cursor_name = ".h"; // background tab: no scroll (see BUG-P3-1 below)
    REQUIRE(core.layout.add_tab(p1, s2, 1, false));
    TabState s3;
    s3.path = t / "d3";
    s3.cursor_name = "a.txt";
    s3.scroll_rows = 0.0;
    const Result<layout::SplitResult> right =
        core.layout.split_pane(p1, layout::SplitEdge::Right, s3);
    REQUIRE(right);
    TabState s4;
    s4.path = t / "d4";
    s4.cursor_name = raw_name;
    s4.scroll_rows = 0.0;
    const Result<layout::SplitResult> down =
        core.layout.split_pane(right->pane, layout::SplitEdge::Bottom, s4);
    REQUIRE(down);
    const auto* root = std::get_if<layout::SplitNode>(&core.layout.root().node);
    REQUIRE(root != nullptr);
    REQUIRE(core.layout.move_splitter(root->id, 0, 0.3));
    TabState c1;
    c1.path = t / "gone";
    core.closed_tabs.push(layout::ClosedTab{c1, PaneId{}, 0});
    core.window.width = 900;
    core.window.height = 600;
    core.window.maximized = true;
    REQUIRE(layout::save_session(layout::session_file(cfg), core));
    const Result<std::string> core_json = layout::to_json(core);
    REQUIRE(core_json);

    // core -> Workspace: the Workspace's own JSON (after its models have listed) is the same text.
    Result<layout::Session> loaded = layout::load_session(layout::session_file(cfg));
    REQUIRE(loaded);
    Workspace ws(options(t, cfg));
    ws.start(std::move(*loaded), Path());
    settle(ws);
    CHECK(ws.layout().pane_count() == 3);
    CHECK(ws.active_model().current_dir() == t / "d4");
    CHECK(cursor_entry(ws.active_model()) == raw_name);
    (void)ws.sync();
    const Result<std::string> ws_json = layout::to_json(ws.snapshot());
    REQUIRE(ws_json);
    CHECK(*ws_json == *core_json);

    // Workspace -> core: session_from_json accepts it and re-serializes to the same text.
    const Result<layout::Session> back = layout::session_from_json(*ws_json);
    REQUIRE(back);
    REQUIRE(back->layout.validate());
    const Result<std::string> again = layout::to_json(*back);
    REQUIRE(again);
    CHECK(*again == *ws_json);
}

// A tab that was restored from the session but never activated keeps an unloaded model; its
// TabState must survive a second save unchanged (save -> load -> save is a fixed point).
TEST_CASE("Save, restore, save again without activating background tabs is a fixed point",
          "[workspace][session][!mayfail]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    std::string first;
    {
        Workspace ws(options(t, cfg));
        build_two_by_three(ws, t);
        (void)ws.sync();
        REQUIRE(layout::save_session(layout::session_file(cfg), ws.snapshot()));
        first = read_file(layout::session_file(cfg));
    }
    Result<layout::Session> loaded = layout::load_session(layout::session_file(cfg));
    REQUIRE(loaded);
    Workspace ws(options(t, cfg));
    ws.start(std::move(*loaded), Path());
    settle(ws);
    (void)ws.sync();
    const Result<std::string> second = layout::to_json(ws.snapshot());
    REQUIRE(second);
    // BUG-P3-1: a never-activated tab saved with "scroll": 0 comes back without "scroll"
    // (BrowserModel::tab_state drops a zero scroll of an unloaded target).
    CHECK(*second == first);
}

// ---------------------------------------------------------------------------------------------
// Corrupt files
// ---------------------------------------------------------------------------------------------

TEST_CASE("A corrupt session.json is moved to .bad and the workspace starts on one tab",
          "[workspace][session][corrupt]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    std::filesystem::create_directories(cfg);
    const Path file = layout::session_file(cfg);
    const Path bad = Path(file.native() + ".bad");

    // A real session cut in half (a crash mid-write of a non-atomic writer).
    layout::Session three;
    TabState a;
    a.path = t / "d1";
    three.layout = layout::Layout(a);
    TabState b;
    b.path = t / "d2";
    REQUIRE(three.layout.split_pane(three.layout.active_pane(), layout::SplitEdge::Right, b));
    const Result<std::string> full = layout::to_json(three);
    REQUIRE(full);
    const std::vector<std::string> corrupt{
        full->substr(0, full->size() / 2), "{not json", "[1, 2, 3]", "\"a string\"", "",
        std::string("\x00\x01\x02", 3)};
    for (const std::string& text : corrupt) {
        INFO("corrupt text of " << text.size() << " bytes");
        write_file(file, text);
        const Result<layout::Session> loaded = layout::load_session(file);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(std::filesystem::exists(file));
        CHECK(read_file(bad) == text); // the newest corrupt file replaces an older .bad
        Workspace ws(options(t, cfg));
        ws.start(std::nullopt, t / "d3");
        settle(ws);
        CHECK(ws.layout().pane_count() == 1);
        CHECK(ws.layout().tab_count() == 1);
        CHECK(ws.active_model().current_dir() == t / "d3");
        // The next save writes a fresh, valid file next to the .bad one.
        (void)ws.sync();
        REQUIRE(layout::save_session(file, ws.snapshot()));
        REQUIRE(layout::load_session(file));
        CHECK(read_file(bad) == text);
    }

    // Valid JSON whose layout is unusable: loads (lenient), the workspace adds one tab.
    for (const char* text :
         {R"({"version": 1, "layout": {"root": {"type": "split", "children": []}}})",
          R"({"version": 99, "layout": {"root": {"type": "pane", "tabs": [{"path": "rel"}]}}})",
          R"({"layout": 7, "closed_tabs": [{"path": ""}, 3]})"}) {
        INFO(text);
        write_file(file, text);
        Result<layout::Session> loaded = layout::load_session(file);
        REQUIRE(loaded);
        CHECK(std::filesystem::exists(file)); // not moved aside
        Workspace ws(options(t, cfg));
        ws.start(std::move(*loaded), t / "d5");
        settle(ws);
        CHECK(ws.layout().tab_count() == 1);
        CHECK(ws.active_model().current_dir() == t / "d5");
        CHECK(ws.closed_tabs().empty());
    }
}

// ---------------------------------------------------------------------------------------------
// Hammer: debounced background saves while the UI thread edits the layout
// ---------------------------------------------------------------------------------------------

TEST_CASE("Background saves while the layout changes never leave a torn or invalid file",
          "[workspace][session][race]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    const Path file = layout::session_file(cfg);

    struct ReaderStats {
        std::uint64_t reads = 0;
        std::uint64_t missing = 0;
        std::uint64_t invalid = 0;
        std::uint64_t not_object = 0;
        std::string first_invalid;
    };
    ReaderStats stats[2];
    std::vector<std::jthread> readers;
    for (ReaderStats& st : stats) {
        // jthread: a failed REQUIRE below still stops and joins the readers.
        readers.emplace_back([&st, file](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    ++st.missing;
                    continue;
                }
                const std::string text{std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()};
                ++st.reads;
                // Complete documents end with "}\n"; a torn file would not.
                if (text.size() < 2 || text.compare(text.size() - 2, 2, "}\n") != 0) {
                    ++st.not_object;
                }
                const Result<layout::Session> s = layout::session_from_json(text);
                if (!s || !s->layout.validate()) {
                    if (st.invalid++ == 0) {
                        st.first_invalid = text.substr(0, 200);
                    }
                }
            }
        });
    }

    std::uint64_t submitted = 0;
    std::size_t invariant_failures = 0;
    layout::Session last; // the newest snapshot handed to a writer
    {
        SessionWriter writer(file);
        SessionWriter second(file); // two writers on one file: last rename wins, never torn
        Workspace ws(options(t, cfg));
        ws.start(std::nullopt, t / "d1");
        settle(ws);
        SaveScheduler scheduler;
        double now_ms = 0.0;
        std::mt19937 rng(20261117U);
        const std::vector<CommandId> ops{
            CommandId::NewTab,          CommandId::NewTab,           CommandId::CloseTab,
            CommandId::SplitRight,      CommandId::SplitDown,        CommandId::ClosePane,
            CommandId::NextTab,         CommandId::PreviousTab,      CommandId::FocusNextPane,
            CommandId::ReopenClosedTab, CommandId::MoveTabLeft,      CommandId::MoveTabRight,
            CommandId::DuplicateTab,    CommandId::FocusPreviousPane};
        std::uint64_t seen = ws.persist_serial();
        for (int i = 0; i < 600; ++i) {
            const unsigned pick = std::uniform_int_distribution<unsigned>(0, 99)(rng);
            if (pick < 70) {
                const CommandId id = ops[pick % ops.size()];
                // Keep the tree small enough to stay fast.
                if ((id == CommandId::SplitRight || id == CommandId::SplitDown) &&
                    ws.layout().pane_count() >= 6) {
                    (void)ws.execute(Command{CommandId::ClosePane, 0});
                } else {
                    (void)ws.execute(Command{id, 0});
                }
            } else if (pick < 85) {
                const int d = static_cast<int>(pick % 6) + 1;
                ws.active_model().navigate_to(t / ("d" + std::to_string(d)));
            } else if (pick < 92) {
                ws.active_model().toggle_hidden();
            } else {
                ws.active_model().sort_by(pick % 2 == 0 ? fs::SortKey::Size : fs::SortKey::Name);
            }
            (void)ws.frame_start(); // listings land whenever they land, as in the app
            if (!ws.layout().validate()) {
                ++invariant_failures;
            }
            // The app's persist_tick: sync, debounce, submit a copy.
            (void)ws.sync();
            if (ws.persist_serial() != seen) {
                seen = ws.persist_serial();
                scheduler.mark(now_ms);
            }
            now_ms += 700.0;
            if (scheduler.due(now_ms) || i % 3 == 0) { // i % 3: far more saves than the app
                scheduler.taken();
                last = ws.snapshot();
                (i % 2 == 0 ? writer : second).submit(last);
                ++submitted;
            }
        }
        settle(ws);
        (void)ws.sync();
        last = ws.snapshot();
        REQUIRE(second.flush());
        writer.submit(last);
        REQUIRE(writer.flush());
        CHECK(writer.writes() + second.writes() >= 2);
        CHECK(writer.writes() + second.writes() <= submitted + 1);
    }
    readers.clear(); // request_stop + join

    CHECK(invariant_failures == 0);
    CHECK(submitted > 100);
    for (const ReaderStats& st : stats) {
        INFO("first invalid read: " << st.first_invalid);
        CHECK(st.reads > 0);
        CHECK(st.invalid == 0);
        CHECK(st.not_object == 0);
    }
    // The file holds exactly the last snapshot; no temporary file is left behind.
    const Result<std::string> want = layout::to_json(last);
    REQUIRE(want);
    CHECK(read_file(file) == *want);
    CHECK(dir_names(cfg) == std::set<std::string>{"session.json"});
}

// ---------------------------------------------------------------------------------------------
// Named layouts
// ---------------------------------------------------------------------------------------------

TEST_CASE("Named layouts with Turkish names round-trip through the workspace",
          "[workspace][layouts][i18n]") {
    TempDir t;
    populate(t);
    const Path cfg = t / "cfg";
    const std::string work = "Çalışma düzeni — ğüşöç İı";
    const std::string upper = "İstanbul";
    const std::string lower = "istanbul";
    std::string dotless_64;
    for (int i = 0; i < 32; ++i) {
        dotless_64 += "ı"; // 2 bytes each: exactly the 64-byte limit
    }
    std::string saved_text;
    {
        Workspace ws(options(t, cfg));
        build_two_by_three(ws, t);
        REQUIRE(ws.save_layout(work));
        saved_text = read_file(layout::layouts_dir(cfg) / (work + ".json"));
        const Result<std::string> want = layout::layout_to_json(ws.layout());
        REQUIRE(want);
        CHECK(saved_text == *want);
        REQUIRE(exec(ws, CommandId::ClosePane));
        REQUIRE(ws.save_layout(upper));
        REQUIRE(ws.save_layout(lower)); // case-sensitive: a second file
        REQUIRE(ws.save_layout(dotless_64));
        // Invalid names fail without touching the filesystem.
        for (const std::string& name :
             {std::string(), std::string("a/b"), std::string(".gizli"), std::string(" baş"),
              std::string("son "), dotless_64 + "a", std::string("sekme\tadı"), std::string("\xc4"),
              std::string("ok\x7f")}) {
            INFO("name '" << name << "'");
            const Result<void> r = ws.save_layout(name);
            REQUIRE_FALSE(r);
            CHECK(r.error().code == ErrorCode::InvalidArgument);
        }
        const Result<std::vector<std::string>> names = ws.list_layouts();
        REQUIRE(names);
        CHECK(names->size() == 4);
        CHECK(ws.layout_exists(work));
        CHECK(ws.layout_exists(upper));
        CHECK(ws.layout_exists(lower));
        CHECK(ws.layout_exists(dotless_64));
        CHECK_FALSE(ws.layout_exists("Çalışma"));
    }
    // A fresh workspace (new process) on the same config root loads it.
    Workspace ws(options(t, cfg));
    ws.start(std::nullopt, t / "d6");
    settle(ws);
    const std::uint64_t epoch = ws.layout_epoch();
    REQUIRE(ws.load_layout(work));
    settle(ws);
    CHECK(ws.layout_epoch() != epoch);
    const Result<std::string> now = layout::layout_to_json(ws.layout());
    REQUIRE(now);
    CHECK(*now == saved_text);                              // before any tab state changes
    CHECK(closed_paths(ws) == std::vector<Path>{t / "d6"}); // the replaced tab
    CHECK(ws.layout().pane_count() == 2);
    CHECK(ws.layout().tab_count() == 6);
    CHECK(active_pane_index(ws) == 1);
    CHECK(ws.active_model().current_dir() == t / "d4");
    CHECK(lazy_ok(ws));
    // The upper/lower pair stay distinct layouts (one pane each, three tabs).
    REQUIRE(ws.load_layout(lower));
    settle(ws);
    CHECK(ws.layout().pane_count() == 1);
    REQUIRE(ws.delete_layout(lower));
    CHECK_FALSE(ws.layout_exists(lower));
    CHECK(ws.layout_exists(upper));
    const Result<void> missing = ws.load_layout(lower);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(ws.layout().pane_count() == 1); // a failed load changes nothing

    // Without a config root named layouts are unavailable.
    Workspace none(options(t, Path()));
    none.start(std::nullopt, t / "d1");
    settle(none);
    const Result<void> r = none.save_layout(work);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Unsupported);
    CHECK_FALSE(none.list_layouts());
}

// ---------------------------------------------------------------------------------------------
// Lazy tabs
// ---------------------------------------------------------------------------------------------

TEST_CASE("Only visible tabs own worker threads and watches", "[workspace][lazy]") {
    TempDir t;
    populate(t);
    for (int i = 0; i < 20; ++i) {
        std::filesystem::create_directories(
            t / "d1" / ("bg" + std::to_string(i / 10) + std::to_string(i % 10)));
    }
    // Sanitizer runtimes (TSan) start a helper thread with the first thread of the process: make
    // sure it exists before the baseline is taken.
    std::thread([] {}).join();
    const std::size_t tasks_before = count_tasks();
    const std::size_t inotify_before = count_inotify_fds();
    std::size_t tasks_one_tab = 0;
    {
        Workspace ws(options(t, Path(), inotify_factory()));
        ws.start(std::nullopt, t / "d1");
        settle(ws);
        REQUIRE(ws.active_model().wait_watch_ready(kBrowserWait));
        tasks_one_tab = count_tasks();
        CHECK(tasks_one_tab > tasks_before); // a live tab has workers
        CHECK(count_inotify_fds() == inotify_before + 1);

        // 20 background tabs (Ctrl+Enter on 20 folders): no new thread, no new watch.
        for (int i = 0; i < 20; ++i) {
            BrowserModel& m = ws.active_model();
            const std::string name = "bg" + std::to_string(i / 10) + std::to_string(i % 10);
            m.click_row(m.find(name), Modifiers::None);
            REQUIRE(ws.execute(Command{CommandId::OpenInNewTab, 0}));
            settle(ws);
        }
        CHECK(ws.layout().tab_count() == 21);
        CHECK(lazy_ok(ws));
        CHECK(tasks_settled(tasks_one_tab) == tasks_one_tab);
        CHECK(count_inotify_fds() == inotify_before + 1);

        // Switching tabs moves the workers, never adds any.
        for (int i = 0; i < 5; ++i) {
            REQUIRE(ws.execute(Command{CommandId::NextTab, 0}));
            settle(ws);
        }
        CHECK(lazy_ok(ws));
        CHECK(tasks_settled(tasks_one_tab) == tasks_one_tab);
        CHECK(count_inotify_fds() == inotify_before + 1);

        // A second pane: one more live tab.
        REQUIRE(ws.execute(Command{CommandId::SplitRight, 0}));
        settle(ws);
        REQUIRE(ws.active_model().wait_watch_ready(kBrowserWait));
        CHECK(count_inotify_fds() == inotify_before + 2);
        const std::size_t tasks_two = count_tasks();
        CHECK(tasks_two > tasks_one_tab);
        REQUIRE(ws.execute(Command{CommandId::ClosePane, 0}));
        settle(ws);
        CHECK(tasks_settled(tasks_one_tab) == tasks_one_tab);
        CHECK(count_inotify_fds() == inotify_before + 1);
    }
    // Everything is released with the workspace.
    CHECK(tasks_settled(tasks_before) == tasks_before);
    CHECK(count_inotify_fds() == inotify_before);
}

TEST_CASE("A background tab relists on activation and sees changes made while suspended",
          "[workspace][lazy]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t, Path(), inotify_factory()));
    ws.start(std::nullopt, t / "d1");
    settle(ws);
    cursor_to(ws, "c.txt");
    const TabId first = ws.active_tab();
    REQUIRE(exec(ws, CommandId::NewTab));
    go(ws, t / "d2");
    BrowserModel* bg = ws.model(first);
    REQUIRE(bg != nullptr);
    REQUIRE(bg->suspended());
    const std::vector<std::string> before = entry_names(*bg);
    CHECK(before == std::vector<std::string>{"sub", "a.txt", "b.txt", "c.txt"});

    // Changes while suspended: nothing reaches the model.
    write_file(t / "d1" / "new.txt", "n");
    std::filesystem::remove(t / "d1" / "a.txt");
    std::filesystem::create_directories(t / "d1" / "zfolder");
    for (int i = 0; i < 5; ++i) {
        (void)ws.frame_start();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(entry_names(*bg) == before);
    CHECK(bg->suspended());

    // Activation relists: the new state, the cursor kept on c.txt, the other tab suspended.
    const TabId second = ws.active_tab();
    ws.activate_tab(first);
    settle(ws);
    BrowserModel& m = ws.active_model();
    CHECK(&m == bg);
    CHECK_FALSE(m.suspended());
    CHECK(ws.model(second)->suspended());
    CHECK(entry_names(m) ==
          std::vector<std::string>{"sub", "zfolder", "b.txt", "c.txt", "new.txt"});
    CHECK(cursor_entry(m) == "c.txt");
    CHECK(invariant_violation(m).empty());

    // And it is watched again: a later change arrives without a refresh.
    REQUIRE(m.wait_watch_ready(kBrowserWait));
    write_file(t / "d1" / "later.txt", "l");
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        (void)m.wait_watch_batch(std::chrono::milliseconds(50));
        (void)ws.frame_start();
        seen = m.find("later.txt") != kNoRow;
    }
    CHECK(seen);

    // The suspended tab missed changes in its own folder too; it catches up on return.
    write_file(t / "d2" / "while-away.txt", "w");
    ws.activate_tab(second);
    settle(ws);
    CHECK(ws.active_model().find("while-away.txt") != kNoRow);
    CHECK(lazy_ok(ws));
}

// ---------------------------------------------------------------------------------------------
// Reopen order
// ---------------------------------------------------------------------------------------------

// One tab closed at a time, then reopened (Ctrl+Shift+T) as often: the tab bar is back to its
// old order, because each ClosedTab records its index in the bar at close time.
TEST_CASE("Tabs closed one by one reopen in their old order", "[workspace][closed]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t, Path()));
    ws.start(std::nullopt, t / "d1");
    settle(ws);
    for (const char* d : {"d2", "d3", "d4"}) {
        REQUIRE(exec(ws, CommandId::NewTab));
        go(ws, t / d);
    }
    const auto dirs = [&ws] {
        std::vector<Path> out;
        for (const layout::Tab& tab : pane_at(ws, 0).tabs) {
            out.push_back(tab.state.path);
        }
        return out;
    };
    (void)ws.sync();
    const std::vector<Path> original = dirs();
    REQUIRE(original == std::vector<Path>{t / "d1", t / "d2", t / "d3", t / "d4"});
    for (int i = 0; i < 2; ++i) { // close d2, then d3 (now at index 1)
        ws.activate_tab(pane_at(ws, 0).tabs[1].id);
        settle(ws);
        REQUIRE(exec(ws, CommandId::CloseTab));
    }
    REQUIRE(exec(ws, CommandId::ReopenClosedTab));
    REQUIRE(exec(ws, CommandId::ReopenClosedTab));
    (void)ws.sync();
    CHECK(dirs() == original);
}

// tab.close_left (tab context menu) closes A, B, C left of D; reopening three times should give
// A B C D back. BUG-P3-2: Workspace pushes them left to right with their indices in the old bar,
// so the newest (C, index 2) is reopened first into [D] and the bar ends up A D B C.
TEST_CASE("Tabs closed together by close_left reopen in their old order",
          "[workspace][closed][!mayfail]") {
    TempDir t;
    populate(t);
    Workspace ws(options(t, Path()));
    ws.start(std::nullopt, t / "d1");
    settle(ws);
    for (const char* d : {"d2", "d3", "d4"}) {
        REQUIRE(exec(ws, CommandId::NewTab));
        go(ws, t / d);
    }
    const auto dirs = [&ws] {
        std::vector<Path> out;
        for (const layout::Tab& tab : pane_at(ws, 0).tabs) {
            out.push_back(tab.state.path);
        }
        return out;
    };
    (void)ws.sync();
    const std::vector<Path> original = dirs();
    REQUIRE(exec(ws, CommandId::CloseLeftTabs, pane_at(ws, 0).tabs[3].id.value));
    REQUIRE(dirs() == std::vector<Path>{t / "d4"});
    for (int i = 0; i < 3; ++i) {
        REQUIRE(exec(ws, CommandId::ReopenClosedTab));
    }
    (void)ws.sync();
    CHECK(dirs() == original);
}

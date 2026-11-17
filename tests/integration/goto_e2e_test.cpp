// GoTo and the address bar end to end (owner: qa): ui::PathEntry (GoTo and AddressBar modes)
// with a real ui::PathWorker thread against a temporary HOME. Text goes in through TextEdit and
// changed(), the debounce is driven with a synthetic clock, Enter goes through submit() and the
// worker's resolve_input(), and the outcome is applied to a ui::Workspace like the shell does.
//
// Covered: "~/Downloads", "/", "$HOME/x", "${XDG_DOWNLOAD_DIR}" (only an environment variable:
// the XDG user-dirs file is not consulted for variables), %VAR% (on in the ui context, as
// docs/architecture.md "GoTo and the address bar" specifies; off when the context says so),
// case correction ("~/downloads" -> ~/Downloads only when unique), Turkish names (I/i/İ/ı),
// unknown variables (error, text kept), and the completion worker under rapid typing in a
// 20 000-entry folder (no stale completion is ever shown; the last text wins).
//
// Environment: each Catch2 case runs in its own process (catch_discover_tests), and HOME and the
// test variables are set before any worker thread starts and never changed while one runs.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/fs.hpp>
#include <navigator/path_input.hpp>

#include "browser_support.hpp"
#include "fixture.hpp"
#include "path/path_entry.hpp"
#include "path/path_worker.hpp"
#include "workspace/workspace.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::ui::PathEntry;
using navigator::ui::PathEntryMode;
using navigator::ui::PathItem;
using navigator::ui::PathOutcome;
using navigator::ui::PathWorker;

namespace {

constexpr std::chrono::milliseconds kIdle{10000};

// A temporary HOME with Downloads, Turkish folders, a file and two names that differ only in
// case; HOME and two test variables point into it.
struct Home {
    TempDir t;
    Path home;
    std::string old_home;
    bool had_home = false;

    Home() : home(t / "home") {
        for (const char* d : {"Downloads", "x", "İndirilenler", "Müzik", "Belgeler/Çalışma",
                              "Music", "music", "with space"}) {
            std::filesystem::create_directories(home / d);
        }
        (void)touch(home / "Downloads" / "report.pdf");
        // NOLINTBEGIN(concurrency-mt-unsafe): no other thread exists yet (see the header).
        if (const char* h = std::getenv("HOME"); h != nullptr) {
            old_home = h;
            had_home = true;
        }
        ::setenv("HOME", home.c_str(), 1);
        ::setenv("NAVIGATOR_QA_DIR", (home / "x").c_str(), 1);
        ::unsetenv("NAVIGATOR_QA_UNSET");
        ::unsetenv("XDG_DOWNLOAD_DIR");
        // NOLINTEND(concurrency-mt-unsafe)
    }
    Home(const Home&) = delete;
    Home& operator=(const Home&) = delete;
    Home(Home&&) = delete;
    Home& operator=(Home&&) = delete;
    ~Home() {
        // NOLINTBEGIN(concurrency-mt-unsafe): the workers of the case are joined by now.
        if (had_home) {
            ::setenv("HOME", old_home.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
        ::unsetenv("NAVIGATOR_QA_DIR");
        // NOLINTEND(concurrency-mt-unsafe)
    }
};

// The outcome after a REQUIRE(o.has_value()) (tidy's optional check does not see REQUIRE).
PathOutcome got(const std::optional<PathOutcome>& o) { return o.value_or(PathOutcome{}); }

// Drives the debounce and the worker until the entry has nothing pending.
void pump(PathEntry& e, PathWorker& w, double& now_ms) {
    for (int round = 0; round < 200 && e.busy(); ++round) {
        now_ms += PathEntry::kDebounceMs + 1.0;
        e.tick(now_ms);
        REQUIRE(w.wait_idle(kIdle));
        (void)e.poll(now_ms);
    }
    REQUIRE_FALSE(e.busy());
}

void type(PathEntry& e, PathWorker& w, double& now_ms, std::string_view text) {
    e.edit().set(text);
    e.changed(now_ms);
    pump(e, w, now_ms);
}

// Enter on the typed text (no highlighted row) and the outcome.
std::optional<PathOutcome> enter(PathEntry& e, PathWorker& w, double& now_ms, bool new_tab) {
    e.set_highlight(static_cast<std::size_t>(-1));
    e.submit(new_tab);
    pump(e, w, now_ms);
    return e.take_outcome();
}

struct Goto {
    Home h;
    PathWorker worker;
    PathEntry e{PathEntryMode::GoTo};
    double now_ms = 1000.0;

    Goto() {
        e.attach(&worker);
        e.open({}, false, h.home / "x", h.home, {}, now_ms);
    }
    std::optional<PathOutcome> go(std::string_view text, bool new_tab = false) {
        type(e, worker, now_ms, text);
        return enter(e, worker, now_ms, new_tab);
    }
};

std::vector<std::string> completion_names(const PathEntry& e) {
    std::vector<std::string> out;
    for (const PathItem& it : e.items()) {
        if (it.kind == PathItem::Kind::Completion) {
            out.push_back(it.label);
        }
    }
    return out;
}

} // namespace

TEST_CASE("GoTo resolves home, root, variables and relative input to folders",
          "[goto][e2e][path]") {
    Goto g;
    struct Case {
        const char* text;
        Path want;
    };
    const std::vector<Case> cases{
        {"~/Downloads", g.h.home / "Downloads"},
        {"~/Downloads/", g.h.home / "Downloads"},
        {"/", "/"},
        {"~", g.h.home},
        {"$HOME/x", g.h.home / "x"},
        {"${HOME}//x/./", g.h.home / "x"},
        {"$NAVIGATOR_QA_DIR", g.h.home / "x"},
        {"%HOME%/Downloads", g.h.home / "Downloads"}, // ui context: percent_variables on
        {"..", g.h.home},                             // relative to the active folder (~/x)
        {"  '~/with space'\n", g.h.home / "with space"},
        {"file:///", "/"},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        const std::optional<PathOutcome> o = g.go(c.text);
        REQUIRE(o.has_value());
        CHECK(got(o).kind == PathOutcome::Kind::Navigate);
        CHECK(got(o).path == c.want);
        CHECK_FALSE(got(o).case_corrected);
        CHECK(g.e.error().empty());
    }
    // A file: its folder with the cursor on it; Ctrl+Enter opens it.
    std::optional<PathOutcome> o = g.go("~/Downloads/report.pdf");
    REQUIRE(o.has_value());
    CHECK(got(o).kind == PathOutcome::Kind::RevealFile);
    CHECK(got(o).path == g.h.home / "Downloads");
    CHECK(got(o).cursor_name == "report.pdf");
    o = g.go("~/Downloads/report.pdf", true);
    REQUIRE(o.has_value());
    CHECK(got(o).kind == PathOutcome::Kind::OpenFile);
    CHECK(got(o).new_tab);

    // The ui context is the documented one: home, cwd, getenv, %VAR% on; only the worker
    // context resolves "~name".
    const path_input::ExpandContext ui = g.e.ui_context();
    CHECK(ui.percent_variables);
    CHECK(ui.home == g.h.home);
    CHECK(ui.cwd == g.h.home / "x");
    CHECK_FALSE(static_cast<bool>(ui.user_home));
    CHECK(static_cast<bool>(g.e.worker_context().user_home));
    // With percent_variables off (core contract), %HOME% stays literal and is not reported.
    path_input::ExpandContext off = ui;
    off.percent_variables = false;
    const Result<path_input::ExpandedPath> lit = path_input::expand_user_path("%HOME%", off);
    REQUIRE(lit.has_value());
    CHECK(lit->path == g.h.home / "x" / "%HOME%");
    CHECK(lit->unresolved.empty());
}

TEST_CASE("GoTo applied to the workspace: ~/Downloads and / navigate, Ctrl+Enter opens a tab",
          "[goto][e2e][workspace]") {
    Goto g;
    NoSpawnLauncher launcher;
    ui::Workspace::Options wo;
    wo.model.launcher = &launcher;
    wo.home = g.h.home;
    ui::Workspace ws(std::move(wo));
    ws.start(std::nullopt, g.h.home / "x");
    const auto settle = [&] {
        for (int round = 0; round < 3; ++round) {
            for (const layout::TabId tab : ws.visible_tabs()) {
                REQUIRE(ws.model(tab)->wait_listing(kBrowserWait));
            }
            (void)ws.frame_start();
        }
        REQUIRE(ws.settled());
    };
    settle();
    std::optional<PathOutcome> o = g.go("~/Downloads");
    REQUIRE(o.has_value());
    ws.navigate_active(got(o).path, got(o).cursor_name);
    settle();
    CHECK(ws.active_model().current_dir() == g.h.home / "Downloads");
    o = g.go("/");
    REQUIRE(o.has_value());
    ws.navigate_active(got(o).path, got(o).cursor_name);
    settle();
    CHECK(ws.active_model().current_dir() == Path("/"));
    o = g.go("~/Downloads/report.pdf");
    REQUIRE(o.has_value());
    ws.navigate_active(got(o).path, got(o).cursor_name);
    settle();
    CHECK(ws.active_model().current_dir() == g.h.home / "Downloads");
    CHECK(cursor_entry(ws.active_model()) == "report.pdf");
    o = g.go("~/x", true);
    REQUIRE(o.has_value());
    REQUIRE(got(o).new_tab);
    CHECK(ws.open_tab(got(o).path, false));
    settle();
    CHECK(ws.layout().tab_count() == 2);
    CHECK(ws.active_model().current_dir() == g.h.home / "Downloads");
    // The visits a recents list would record: Downloads, /, and the new tab's folder.
    const std::vector<Path> visits = ws.take_visits();
    CHECK(std::find(visits.begin(), visits.end(), g.h.home / "Downloads") != visits.end());
    CHECK(std::find(visits.begin(), visits.end(), Path("/")) != visits.end());
    CHECK(launcher.requests.empty());
}

TEST_CASE("GoTo corrects the case of unique names, including Turkish I", "[goto][e2e][case]") {
    Goto g;
    struct Case {
        const char* text;
        Path want;
    };
    const std::vector<Case> cases{
        {"~/downloads", g.h.home / "Downloads"},
        {"~/DOWNLOADS/", g.h.home / "Downloads"},
        {"$HOME/downloads", g.h.home / "Downloads"},
        {"~/indirilenler", g.h.home / "İndirilenler"}, // i -> İ
        {"~/INDIRILENLER", g.h.home / "İndirilenler"}, // I -> İ
        {"~/ındırılenler", g.h.home / "İndirilenler"}, // ı -> İ / i
        {"~/MÜZİK", g.h.home / "Müzik"},
        {"~/belgeler/ÇALIŞMA", g.h.home / "Belgeler" / "Çalışma"},
        {"~/X", g.h.home / "x"},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        const std::optional<PathOutcome> o = g.go(c.text);
        REQUIRE(o.has_value());
        CHECK(got(o).kind == PathOutcome::Kind::Navigate);
        CHECK(got(o).path == c.want);
        CHECK(got(o).case_corrected);
    }
    // Exact names win; "Music" and "music" both exist, so "~/MUSIC" is ambiguous: not found,
    // the text stays for editing.
    std::optional<PathOutcome> o = g.go("~/music");
    REQUIRE(o.has_value());
    CHECK(got(o).path == g.h.home / "music");
    CHECK_FALSE(got(o).case_corrected);
    o = g.go("~/MUSIC");
    CHECK_FALSE(o.has_value());
    CHECK_FALSE(g.e.error().empty());
    CHECK(g.e.edit().text() == "~/MUSIC");

    // Completion corrects the folder being completed: "~/downloads/" lists ~/Downloads and
    // offers absolute replacement text.
    type(g.e, g.worker, g.now_ms, "~/downloads/rep");
    CHECK(completion_names(g.e).empty()); // folders only; report.pdf is a file
    type(g.e, g.worker, g.now_ms, "~/belgeler/ça");
    REQUIRE(completion_names(g.e) == std::vector<std::string>{"Çalışma"});
    CHECK(g.e.accept_completion(g.now_ms));
    CHECK(g.e.edit().text() == (g.h.home / "Belgeler" / "Çalışma").string() + "/");
    type(g.e, g.worker, g.now_ms, "~/İN");
    CHECK(completion_names(g.e) == std::vector<std::string>{"İndirilenler"});
    type(g.e, g.worker, g.now_ms, "~/in");
    CHECK(completion_names(g.e) == std::vector<std::string>{"İndirilenler"});
}

TEST_CASE("GoTo: unknown variables and missing paths keep the text and name the variable",
          "[goto][e2e][path]") {
    Goto g;
    // XDG_DOWNLOAD_DIR is a key of user-dirs.dirs, not an environment variable: unset here, it
    // stays literal and is reported.
    for (const char* text : {"${XDG_DOWNLOAD_DIR}", "$XDG_DOWNLOAD_DIR/x", "$NAVIGATOR_QA_UNSET",
                             "%NAVIGATOR_QA_UNSET%"}) {
        INFO(text);
        CHECK_FALSE(g.go(text).has_value());
        CHECK(g.e.edit().text() == text);
        const std::string& err = g.e.error();
        CHECK_FALSE(err.empty());
        const std::string token = std::string(text).substr(0, std::string(text).find('/'));
        CHECK(err.find(token) != std::string::npos);
        CHECK_FALSE(g.e.resolving());
    }
    // A missing path without variables: an error, no variable named.
    CHECK_FALSE(g.go("~/nope/deeper").has_value());
    CHECK(g.e.edit().text() == "~/nope/deeper");
    CHECK_FALSE(g.e.error().empty());
    CHECK(g.e.error().find('$') == std::string::npos);
    // A remote file:// URI is invalid, not "not found".
    CHECK_FALSE(g.go("file://host/x").has_value());
    CHECK_FALSE(g.e.error().empty());
    // The same entry still works afterwards.
    const std::optional<PathOutcome> o = g.go("~/Downloads");
    REQUIRE(o.has_value());
    CHECK(got(o).path == g.h.home / "Downloads");
    CHECK(g.e.error().empty());
}

TEST_CASE("GoTo: ${XDG_DOWNLOAD_DIR} expands when the environment exports it",
          "[goto][e2e][path]") {
    Home h;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): before the worker starts.
    ::setenv("XDG_DOWNLOAD_DIR", (h.home / "Downloads").c_str(), 1);
    {
        PathWorker worker;
        PathEntry e(PathEntryMode::GoTo);
        e.attach(&worker);
        double now_ms = 0.0;
        e.open({}, false, h.home, h.home, {}, now_ms);
        type(e, worker, now_ms, "${XDG_DOWNLOAD_DIR}");
        const std::optional<PathOutcome> o = enter(e, worker, now_ms, false);
        REQUIRE(o.has_value());
        CHECK(got(o).path == h.home / "Downloads");
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the worker is joined.
    ::unsetenv("XDG_DOWNLOAD_DIR");
}

namespace {

// 20 000 folders "dNNNNN" (plus a few others) in big/, a small folder next to it.
struct BigTree {
    Home h;
    Path big;
    Path small;
    BigTree() : big(h.t / "big"), small(h.t / "small") {
        std::filesystem::create_directories(big);
        std::filesystem::create_directories(small / "alpha");
        std::filesystem::create_directories(small / "alps");
        std::filesystem::create_directories(small / "beta");
        for (int i = 0; i < 20000; ++i) {
            char name[16];
            (void)std::snprintf(name, sizeof name, "d%05d", i);
            REQUIRE(::mkdir((big / name).c_str(), 0755) == 0);
        }
        REQUIRE(::mkdir((big / "Zeta").c_str(), 0755) == 0);
    }
};

// Every completion row shown belongs to the text in the field right now: its folder is the
// expanded directory part and its name matches the prefix (folded prefix or substring).
std::string stale_completion(const PathEntry& e) {
    const Result<path_input::CompletionQuery> q =
        path_input::parse_completion_input(e.edit().text(), e.ui_context());
    for (const PathItem& it : e.items()) {
        if (it.kind != PathItem::Kind::Completion) {
            continue;
        }
        if (!q) {
            return "completion shown for unparsable text " + e.edit().text();
        }
        if (it.path.parent_path() != q->dir) {
            return "completion " + it.path.string() + " for text " + e.edit().text();
        }
        if (fs::fold_name(it.label).find(fs::fold_name(q->prefix)) == std::string::npos) {
            return "completion " + it.label + " does not match " + q->prefix;
        }
    }
    return {};
}

std::vector<std::string> expected_completions(const PathEntry& e) {
    const Result<std::vector<path_input::Completion>> want = path_input::complete_path(
        e.edit().text(), e.worker_context(), path_input::CompletionOptions{});
    std::vector<std::string> out;
    if (want) {
        for (const path_input::Completion& c : *want) {
            out.push_back(c.name);
        }
    }
    return out;
}

} // namespace

TEST_CASE("Completion under rapid typing in a 20k folder never shows a stale completion",
          "[goto][e2e][completion][slow]") {
    BigTree tree;
    for (const PathEntryMode mode : {PathEntryMode::GoTo, PathEntryMode::AddressBar}) {
        INFO((mode == PathEntryMode::GoTo ? "GoTo" : "address bar"));
        PathWorker worker;
        PathEntry e(mode);
        e.attach(&worker);
        double now_ms = 0.0;
        e.open({}, false, tree.h.t.path(), tree.h.home, {}, now_ms);
        // Keystroke scripts that hop between the big and the small folder; each keystroke comes
        // 5..90 ms after the previous one, so some requests go out while a 20k listing runs
        // (cancelled), others are replaced in the mailbox, others are cache hits.
        const std::string big = tree.big.string() + "/";
        const std::string small = tree.small.string() + "/";
        const std::vector<std::string> words{big + "d1", small + "al",  big + "d19", small + "b",
                                             big + "d0", small + "alp", big + "Z",   big + "d199",
                                             small,      big + "d1999"};
        std::mt19937 rng(20251117U);
        std::uniform_int_distribution<int> gap(5, 90);
        std::size_t stale = 0;
        std::string first_stale;
        int keystrokes = 0;
        for (int round = 0; round < 3; ++round) {
            for (const std::string& word : words) {
                // Type the word one byte at a time from its directory part on.
                const std::size_t from = word.rfind('/', word.size() - 2) + 1;
                for (std::size_t n = from; n <= word.size(); ++n) {
                    e.edit().set(word.substr(0, n));
                    e.changed(now_ms);
                    ++keystrokes;
                    now_ms += gap(rng);
                    e.tick(now_ms);
                    (void)e.poll(now_ms);
                    if (const std::string why = stale_completion(e); !why.empty()) {
                        if (stale++ == 0) {
                            first_stale = why;
                        }
                    }
                }
            }
        }
        INFO("keystrokes " << keystrokes << ", first stale: " << first_stale);
        CHECK(stale == 0);
        // The final text gets its own completions once the worker is idle.
        e.edit().set(big + "d1999");
        e.changed(now_ms);
        pump(e, worker, now_ms);
        CHECK(stale_completion(e).empty());
        CHECK(completion_names(e) == expected_completions(e));
        CHECK(completion_names(e).size() == 10); // d19990..d19999 (d01999 is no prefix match)
        e.edit().set(big + "d0199");
        e.changed(now_ms); // same folder: a cache hit, no worker round trip
        CHECK_FALSE(e.busy());
        CHECK(completion_names(e) == expected_completions(e));
        CHECK(completion_names(e).size() == 10); // d01990..d01999
    }
}

TEST_CASE("Completion: a superseded request's reply does not wipe completions served from the "
          "cache [BUG-P4-1]",
          "[goto][e2e][completion][!mayfail]") {
    // Repro: completions for folder B are cached; the user types into folder A (a request for
    // A goes out after the debounce) and, before it returns, back into B (cache hit, B's
    // completions shown). A's reply still carries the newest request id, so poll() applies it:
    // the cache becomes A and B's completions vanish, with no new request scheduled.
    BigTree tree;
    PathWorker worker;
    PathEntry e(PathEntryMode::GoTo);
    e.attach(&worker);
    double now_ms = 0.0;
    e.open({}, false, tree.h.t.path(), tree.h.home, {}, now_ms);
    const std::string small = tree.small.string() + "/";
    type(e, worker, now_ms, small + "al");
    REQUIRE(completion_names(e) == std::vector<std::string>{"alpha", "alps"});
    // Into the big folder: the request is sent, the listing runs.
    e.edit().set(tree.big.string() + "/d");
    e.changed(now_ms);
    now_ms += PathEntry::kDebounceMs + 1.0;
    e.tick(now_ms);
    // Back to the small folder before the reply: served from the cache at once.
    e.edit().set(small + "alp");
    e.changed(now_ms);
    CHECK(completion_names(e) == std::vector<std::string>{"alpha", "alps"});
    // The big folder's reply arrives.
    REQUIRE(worker.wait_idle(kIdle));
    (void)e.poll(now_ms);
    pump(e, worker, now_ms);
    CHECK(stale_completion(e).empty()); // nothing stale is shown either way
    CHECK(completion_names(e) == std::vector<std::string>{"alpha", "alps"});
}

// Keymap end to end (owner: qa): every default chord in docs/keymap.md -> parse_key_chord ->
// load_default_keymap + register_builtin_commands -> resolve_command -> BrowserModel::execute,
// and the effect on a fixture folder. The doc is parsed at test time, so a row added to the doc
// without a binding (or a binding without a row, or a row without an effect check here) fails.
//
// Activate on a file goes to a recording launcher; nothing is ever spawned.
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
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>

#include "browser/browser_model.hpp"
#include "browser_support.hpp"
#include "fixture.hpp"

using namespace navigator;
using namespace navigator::integration_test;
using navigator::ui::BrowserModel;
using navigator::ui::kNoRow;

#ifndef NAVIGATOR_KEYMAP_DOC
#error "NAVIGATOR_KEYMAP_DOC must name docs/keymap.md"
#endif

namespace {

struct DocRow {
    std::string chord;
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

// Binding rows of "## Phase 0 defaults" and "## Phase 2 defaults ..." (up to "### Mouse").
std::vector<DocRow> read_doc_rows() {
    std::ifstream in(NAVIGATOR_KEYMAP_DOC);
    REQUIRE(in.good());
    std::vector<DocRow> rows;
    enum class Section : std::uint8_t { None, Phase0, Phase2 } section = Section::None;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.starts_with("#")) {
            if (line.starts_with("## Phase 0 defaults")) {
                section = Section::Phase0;
            } else if (line.starts_with("## Phase 2 defaults")) {
                section = Section::Phase2;
            } else {
                section = Section::None;
            }
            continue;
        }
        if (section == Section::None || !line.starts_with("|")) {
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
        const std::optional<std::string> chord = code_span(cells[0]);
        const std::optional<std::string> command = code_span(cells[1]);
        if (!chord || !command) {
            continue; // header, separator, "window close", "(unbound)"
        }
        DocRow row{*chord, *command, 0, number};
        if (section == Section::Phase2) {
            REQUIRE(cells.size() >= 4);
            if (cells[3] == "`Extend`") {
                row.arg = CursorArg::Extend;
            } else {
                INFO("keymap.md line " << number);
                REQUIRE(cells[3] == "0");
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// Fixture: 2 folders, 30 files f01..f30, a 3-byte c.txt, a hidden file.
struct Scene {
    TempDir t;
    NoSpawnLauncher launcher;
    std::unique_ptr<BrowserModel> model;

    Scene() {
        std::filesystem::create_directories(t / "alpha" / "inner");
        std::filesystem::create_directories(t / "beta");
        for (int i = 1; i <= 30; ++i) {
            (void)touch(t / ((i < 10 ? "f0" : "f") + std::to_string(i)));
        }
        { std::ofstream(t / "c.txt") << "abc"; }
        (void)touch(t / ".hidden");
        BrowserModel::Options o;
        o.launcher = &launcher;
        model = std::make_unique<BrowserModel>(std::move(o));
        model->open_start(t.path());
        settle();
        model->set_page_rows(5);
    }
    void settle() const {
        REQUIRE(model->wait_listing(kBrowserWait));
        (void)model->frame_start();
        REQUIRE_FALSE(model->busy());
    }
    void run(const Command& c) const {
        model->execute(c);
        settle();
    }
    [[nodiscard]] BrowserModel& m() const { return *model; }
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
    REQUIRE(m.execute(c));
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

std::map<std::string, Check> effect_checks() {
    std::map<std::string, Check> checks;
    const Check app_level = [](Scene& s, const Command& c) {
        const auto before = entry_names(s.m());
        CHECK_FALSE(s.m().execute(c)); // handled by the App, not the model
        CHECK(entry_names(s.m()) == before);
        CHECK_FALSE(s.m().busy());
    };
    checks["app.quit"] = app_level;
    checks["view.toggle_fps_overlay"] = app_level;
    checks["view.toggle_theme"] = app_level;

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
    };
    checks["nav.back"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.navigate_to(s.t / "beta");
        s.settle();
        s.run(c);
        CHECK(m.current_dir() == s.t.path());
        CHECK(m.can_forward());
        s.run(c); // no-op at the start of history
        CHECK(m.current_dir() == s.t.path());
    };
    checks["nav.forward"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.navigate_to(s.t / "beta");
        s.settle();
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
        m.navigate_to(s.t / "alpha" / "inner");
        s.settle();
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
        REQUIRE(m.execute(c));
        CHECK(m.selected_count() == m.entries().size());
        CHECK(m.selected_bytes() == 3U); // c.txt; folders count 0
        CHECK(m.cursor() == 4);
    };
    checks["list.clear_selection"] = [](Scene& s, const Command& c) {
        BrowserModel& m = s.m();
        m.click_row(6, Modifiers::None);
        m.select_all();
        REQUIRE(m.execute(c));
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
    return checks;
}

} // namespace

TEST_CASE("docs/keymap.md and load_default_keymap agree row for row", "[keymap][e2e]") {
    const std::vector<DocRow> rows = read_doc_rows();
    REQUIRE(rows.size() == 3 + 25); // Phase 0 chords + Phase 2 chords
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    const Span<const KeyBinding> bindings = keymap.bindings();
    REQUIRE(bindings.size() == rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        INFO("keymap.md line " << rows[i].line << ": " << rows[i].chord);
        const KeyChord chord = chord_of(rows[i].chord);
        REQUIRE(chord.key != Key::Unknown);
        CHECK(to_string(chord) == rows[i].chord); // the doc uses canonical text
        CHECK(bindings[i].chord == chord);        // same order as the doc
        CHECK(bindings[i].command == rows[i].command);
        CHECK(bindings[i].arg == rows[i].arg);
    }
}

TEST_CASE("Every default chord resolves to its command and has its effect on the browser",
          "[keymap][e2e][browser]") {
    const std::vector<DocRow> rows = read_doc_rows();
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    const std::map<std::string, Check> checks = effect_checks();

    std::set<std::string> covered;
    for (const DocRow& row : rows) {
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
        covered.insert(row.command);
    }
    // Every registered command except the unbound Phase 4 placeholder has a default chord.
    for (const CommandInfo& info : registry.all()) {
        if (info.id != CommandId::FocusAddressBar) {
            CHECK(covered.contains(std::string(info.name)));
        }
    }
    CHECK(keymap.chords_for("nav.focus_address_bar").empty());

    // Near misses do not resolve: extra modifiers and unbound keys.
    CHECK_FALSE(resolve_command(keymap, registry, chord_of("Ctrl+Shift+A")));
    CHECK_FALSE(resolve_command(keymap, registry, chord_of("Ctrl+Enter")));
    CHECK_FALSE(resolve_command(keymap, registry, chord_of("Alt+Shift+Left")));
    CHECK_FALSE(resolve_command(keymap, registry, chord_of("Delete")));
}

TEST_CASE("Activate through the keymap never spawns a process", "[keymap][e2e][launcher]") {
    Scene s;
    Keymap keymap;
    REQUIRE(load_default_keymap(keymap));
    CommandRegistry registry;
    REQUIRE(register_builtin_commands(registry));
    for (const char* chord : {"Enter", "KeypadEnter"}) {
        const Command c = command_of(keymap, registry, chord_of(chord));
        REQUIRE(c.id == CommandId::Activate);
        for (const char* name : {"c.txt", "f01", "f30"}) {
            s.m().click_row(s.m().find(name), Modifiers::None);
            s.run(c);
        }
    }
    CHECK(s.launcher.requests.size() == 6);
    for (const platform::LaunchRequest& r : s.launcher.requests) {
        CHECK(r.argv.front() == "xdg-open"); // what would have run, recorded only
    }
    int status = 0;
    errno = 0;
    CHECK(::waitpid(-1, &status, WNOHANG) == -1); // no child, running or zombie
    CHECK(errno == ECHILD);
}

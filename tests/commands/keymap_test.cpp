// Keymap, key chord text and default-binding tests (owner: commands).
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>

using navigator::Command;
using navigator::CommandId;
using navigator::CommandInfo;
using navigator::CommandQueue;
using navigator::CommandRegistry;
using navigator::CursorArg;
using navigator::ErrorCode;
using navigator::Key;
using navigator::KeyBinding;
using navigator::KeyChord;
using navigator::Keymap;
using navigator::Modifiers;
using navigator::parse_key_chord;
using navigator::to_string;

namespace {

constexpr Modifiers kNone = Modifiers::None;
constexpr Modifiers kCtrl = Modifiers::Ctrl;
constexpr Modifiers kShift = Modifiers::Shift;
constexpr Modifiers kAlt = Modifiers::Alt;
constexpr Modifiers kSuper = Modifiers::Super;

// Every Key enumerator except Unknown, with its documented canonical name.
const std::vector<std::pair<Key, std::string_view>>& all_keys() {
    static const std::vector<std::pair<Key, std::string_view>> keys = [] {
        std::vector<std::pair<Key, std::string_view>> v;
        static constexpr std::string_view kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I",
                                                        "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                                        "S", "T", "U", "V", "W", "X", "Y", "Z"};
        for (std::uint16_t i = 0; i < 26; ++i) {
            v.emplace_back(static_cast<Key>(static_cast<std::uint16_t>(Key::A) + i), kLetters[i]);
        }
        static constexpr std::string_view kDigits[] = {"0", "1", "2", "3", "4",
                                                       "5", "6", "7", "8", "9"};
        for (std::uint16_t i = 0; i < 10; ++i) {
            v.emplace_back(static_cast<Key>(static_cast<std::uint16_t>(Key::Digit0) + i),
                           kDigits[i]);
        }
        static constexpr std::string_view kFn[] = {
            "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",  "F10", "F11", "F12",
            "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
        for (std::uint16_t i = 0; i < 24; ++i) {
            v.emplace_back(static_cast<Key>(static_cast<std::uint16_t>(Key::F1) + i), kFn[i]);
        }
        const std::pair<Key, std::string_view> rest[] = {
            {Key::Up, "Up"},
            {Key::Down, "Down"},
            {Key::Left, "Left"},
            {Key::Right, "Right"},
            {Key::PageUp, "PageUp"},
            {Key::PageDown, "PageDown"},
            {Key::Home, "Home"},
            {Key::End, "End"},
            {Key::Enter, "Enter"},
            {Key::KeypadEnter, "KeypadEnter"},
            {Key::Escape, "Escape"},
            {Key::Backspace, "Backspace"},
            {Key::Delete, "Delete"},
            {Key::Tab, "Tab"},
            {Key::Space, "Space"},
            {Key::Insert, "Insert"},
            {Key::Minus, "Minus"},
            {Key::Equals, "Equals"},
            {Key::Comma, "Comma"},
            {Key::Period, "Period"},
            {Key::Slash, "Slash"},
            {Key::Backslash, "Backslash"},
            {Key::Semicolon, "Semicolon"},
            {Key::Apostrophe, "Apostrophe"},
            {Key::LeftBracket, "LeftBracket"},
            {Key::RightBracket, "RightBracket"},
            {Key::Grave, "Grave"},
            {Key::MouseBack, "MouseBack"},
            {Key::MouseForward, "MouseForward"},
        };
        for (const auto& p : rest) {
            v.push_back(p);
        }
        return v;
    }();
    return keys;
}

std::string lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string uppercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

KeyChord chord(std::string_view text) {
    const auto r = parse_key_chord(text);
    REQUIRE(r);
    return *r;
}

CommandRegistry builtin_registry() {
    CommandRegistry registry;
    REQUIRE(navigator::register_builtin_commands(registry));
    return registry;
}

// One default binding row as documented in docs/keymap.md.
struct DocRow {
    std::string chord;
    std::string command;
    std::uint64_t arg = 0;
};

std::vector<std::string> split_cells(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (std::size_t i = 1; i < line.size(); ++i) { // skip leading '|'
        if (line[i] == '|') {
            std::size_t b = cell.find_first_not_of(' ');
            std::size_t e = cell.find_last_not_of(' ');
            cells.push_back(b == std::string::npos ? std::string{} : cell.substr(b, e - b + 1));
            cell.clear();
        } else {
            cell += line[i];
        }
    }
    return cells;
}

std::string unquote(const std::string& cell) {
    if (cell.size() >= 2 && cell.front() == '`' && cell.back() == '`') {
        return cell.substr(1, cell.size() - 2);
    }
    return cell;
}

// The chord tables of docs/keymap.md ("Phase 0 defaults", "Phase 2 defaults" ... "Phase 7
// defaults"), in document order. `rows` skips the rows that are not chords ("window close",
// "(unbound)"); `unbound` lists the command names of the "(unbound)" rows.
struct KeymapDoc {
    std::vector<DocRow> rows;
    std::vector<std::string> unbound;
    std::size_t phase_rows[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // chord rows per phase (1 unused)
};

KeymapDoc read_keymap_doc() {
    std::ifstream in(NAVIGATOR_KEYMAP_DOC);
    REQUIRE(in.good());
    KeymapDoc doc;
    std::string line;
    int section = 0; // the phase of the table being read (0, 2 .. 8); valid while in_table
    bool in_table = false;
    while (std::getline(in, line)) {
        if (line.starts_with("## ")) {
            in_table = true;
            if (line.starts_with("## Phase 0 defaults")) {
                section = 0;
            } else if (line.starts_with("## Phase 2 defaults")) {
                section = 2;
            } else if (line.starts_with("## Phase 3 defaults")) {
                section = 3;
            } else if (line.starts_with("## Phase 4 defaults")) {
                section = 4;
            } else if (line.starts_with("## Phase 5 defaults")) {
                section = 5;
            } else if (line.starts_with("## Phase 6 defaults")) {
                section = 6;
            } else if (line.starts_with("## Phase 7 defaults")) {
                section = 7;
            } else if (line.starts_with("## Phase 8 defaults")) {
                section = 8;
            } else if (line.starts_with("## Phase 9 defaults")) {
                section = 9;
            } else {
                in_table = false;
            }
            continue;
        }
        if (line.starts_with("### ")) {
            in_table = false; // sub-sections (Mouse, Popups, Default sequences) are separate
            continue;
        }
        if (!in_table || !line.starts_with("|")) {
            continue;
        }
        const std::vector<std::string> cells = split_cells(line);
        if (cells.size() < 3) {
            continue;
        }
        if (cells[0] == "(unbound)") {
            doc.unbound.push_back(unquote(cells[1]));
            continue;
        }
        if (cells[0].empty() || cells[0].front() != '`') {
            continue; // header, separator, or a non-chord row ("window close")
        }
        DocRow row;
        row.chord = unquote(cells[0]);
        row.command = unquote(cells[1]);
        if (section >= 2) {
            REQUIRE(cells.size() >= 4);
            const std::string arg = unquote(cells[3]);
            if (arg == "Extend") {
                row.arg = CursorArg::Extend;
            } else if (arg == "KeepSelection") {
                row.arg = CursorArg::KeepSelection;
            } else {
                REQUIRE(arg == "0");
            }
        }
        doc.rows.push_back(std::move(row));
        ++doc.phase_rows[section];
    }
    return doc;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Key names
// ---------------------------------------------------------------------------------------------

TEST_CASE("every Key round-trips through to_string and parse_key_chord", "[keymap][key]") {
    std::set<std::string> seen;
    for (const auto& [key, name] : all_keys()) {
        INFO("key " << name);
        CHECK(to_string(key) == name);
        CHECK(seen.insert(lowercase(name)).second); // names unique case-insensitively

        const auto parsed = parse_key_chord(name);
        REQUIRE(parsed);
        CHECK(parsed->key == key);
        CHECK(parsed->mods == kNone);

        const auto lower = parse_key_chord(lowercase(name));
        REQUIRE(lower);
        CHECK(lower->key == key);
        const auto upper = parse_key_chord(uppercase(name));
        REQUIRE(upper);
        CHECK(upper->key == key);
    }
    CHECK(all_keys().size() == 26 + 10 + 24 + 29);
}

TEST_CASE("Unknown and out-of-range keys are named Unknown", "[keymap][key]") {
    CHECK(to_string(Key::Unknown) == "Unknown");
    CHECK(to_string(static_cast<Key>(27)) == "Unknown");
    CHECK(to_string(static_cast<Key>(99)) == "Unknown");
    CHECK(to_string(static_cast<Key>(202)) == "Unknown");
    CHECK(to_string(static_cast<Key>(0xFFFF)) == "Unknown");
}

// ---------------------------------------------------------------------------------------------
// Chord text
// ---------------------------------------------------------------------------------------------

TEST_CASE("to_string(KeyChord) uses the canonical modifier order", "[keymap][chord]") {
    CHECK(to_string(KeyChord{Key::F5, kNone}) == "F5");
    CHECK(to_string(KeyChord{Key::L, kCtrl | kShift}) == "Ctrl+Shift+L");
    CHECK(to_string(KeyChord{Key::L, kShift | kCtrl}) == "Ctrl+Shift+L");
    CHECK(to_string(KeyChord{Key::Left, kAlt}) == "Alt+Left");
    CHECK(to_string(KeyChord{Key::Digit1, kSuper}) == "Super+1");
    CHECK(to_string(KeyChord{Key::A, kShift | kAlt | kCtrl | kSuper}) == "Super+Ctrl+Alt+Shift+A");
    CHECK(to_string(KeyChord{Key::Comma, kCtrl | kAlt}) == "Ctrl+Alt+Comma");
    CHECK(to_string(KeyChord{}) == "Unknown");
}

TEST_CASE("every modifier combination round-trips", "[keymap][chord]") {
    for (std::uint8_t bits = 0; bits <= navigator::kModifierMask; ++bits) {
        const KeyChord c{Key::K, static_cast<Modifiers>(bits)};
        const std::string text = to_string(c);
        INFO("chord " << text);
        const auto parsed = parse_key_chord(text);
        REQUIRE(parsed);
        CHECK(*parsed == c);
    }
}

TEST_CASE("parse_key_chord accepts any order, case, spacing and aliases", "[keymap][chord]") {
    const KeyChord ctrl_shift_l{Key::L, kCtrl | kShift};
    CHECK(chord("Ctrl+Shift+L") == ctrl_shift_l);
    CHECK(chord("shift+ctrl+l") == ctrl_shift_l);
    CHECK(chord("  CTRL + Shift +  L ") == ctrl_shift_l);
    CHECK(chord("Control+Shift+L") == ctrl_shift_l);
    CHECK(chord("cOnTrOl+sHiFt+l") == ctrl_shift_l);
    CHECK(chord("Meta+E") == KeyChord{Key::E, kSuper});
    CHECK(chord("win+E") == KeyChord{Key::E, kSuper});
    CHECK(chord("Super+E") == KeyChord{Key::E, kSuper});
    CHECK(chord("alt+pageup") == KeyChord{Key::PageUp, kAlt});
    CHECK(chord("keypadenter") == KeyChord{Key::KeypadEnter, kNone});
    CHECK(chord("\tF5\t") == KeyChord{Key::F5, kNone});
}

TEST_CASE("parse_key_chord rejects malformed text", "[keymap][chord]") {
    const char* bad[] = {
        "",               // empty
        "   ",            // empty after trimming
        "+",              // empty tokens
        "Ctrl+",          // empty trailing token
        "+A",             // empty leading token
        "Ctrl++A",        // empty middle token
        "Ctrl+ +A",       // blank token
        "Hyper+A",        // unknown modifier
        "Ctrl+Foo",       // unknown key
        "F25",            // unknown key
        "Ctrl+Ctrl+A",    // repeated modifier
        "Ctrl+Control+A", // repeated modifier via alias
        "Meta+Win+A",     // repeated Super via aliases
        "Ctrl",           // missing key
        "Ctrl+Shift",     // missing key
        "A+B",            // more than one key
        "A+Ctrl",         // key not last
        "Ctrl+A+Shift",   // key not last
        "Unknown",        // Unknown is not a key
        "Ctrl+Unknown",   // Unknown is not a key
        "AltGr+A",        // AltGr is never a modifier
        "Ctrl-A",         // wrong separator
    };
    for (const char* text : bad) {
        INFO("text '" << text << "'");
        const auto r = parse_key_chord(text);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(r.error().message.empty());
    }
}

// ---------------------------------------------------------------------------------------------
// Keymap
// ---------------------------------------------------------------------------------------------

TEST_CASE("bind validates its arguments", "[keymap][bind]") {
    Keymap keymap;
    SECTION("Key::Unknown") {
        const auto r = keymap.bind(KeyChord{Key::Unknown, kCtrl}, "app.quit");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("modifier bits outside the mask") {
        const auto r = keymap.bind(KeyChord{Key::Q, static_cast<Modifiers>(0x10U)}, "app.quit");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("empty command") {
        const auto r = keymap.bind(KeyChord{Key::Q, kCtrl}, "");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    CHECK(keymap.bindings().empty());
}

TEST_CASE("bind rejects a chord that is already bound", "[keymap][bind]") {
    Keymap keymap;
    const KeyChord ctrl_q{Key::Q, kCtrl};
    REQUIRE(keymap.bind(ctrl_q, "app.quit"));

    SECTION("to another command") {
        const auto r = keymap.bind(ctrl_q, "view.toggle_theme");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::AlreadyExists);
    }
    SECTION("to the same command") {
        const auto r = keymap.bind(ctrl_q, "app.quit");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::AlreadyExists);
    }
    SECTION("to the same command with another arg") {
        const auto r = keymap.bind(ctrl_q, "app.quit", 1);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::AlreadyExists);
    }
    REQUIRE(keymap.bindings().size() == 1);
    const KeyBinding* b = keymap.resolve(ctrl_q);
    REQUIRE(b != nullptr);
    CHECK(b->command == "app.quit");
    CHECK(b->arg == 0);
}

TEST_CASE("chords differing only in modifiers are distinct", "[keymap][bind]") {
    Keymap keymap;
    REQUIRE(keymap.bind(KeyChord{Key::A, kCtrl}, "list.select_all"));
    REQUIRE(keymap.bind(KeyChord{Key::A, kCtrl | kShift}, "test.other"));
    REQUIRE(keymap.bind(KeyChord{Key::A, kNone}, "test.plain"));
    CHECK(keymap.resolve(KeyChord{Key::A, kCtrl})->command == "list.select_all");
    CHECK(keymap.resolve(KeyChord{Key::A, kCtrl | kShift})->command == "test.other");
    CHECK(keymap.resolve(KeyChord{Key::A, kNone})->command == "test.plain");
    CHECK(keymap.resolve(KeyChord{Key::A, kAlt}) == nullptr); // exact match only
}

TEST_CASE("unbind and clear", "[keymap][bind]") {
    Keymap keymap;
    const KeyChord f5{Key::F5, kNone};
    const KeyChord ctrl_r{Key::R, kCtrl};
    REQUIRE(keymap.bind(f5, "nav.refresh"));
    REQUIRE(keymap.bind(ctrl_r, "nav.refresh"));

    CHECK(keymap.unbind(f5));
    CHECK_FALSE(keymap.unbind(f5));
    CHECK_FALSE(keymap.unbind(KeyChord{Key::F6, kNone}));
    CHECK(keymap.resolve(f5) == nullptr);
    REQUIRE(keymap.bindings().size() == 1);
    CHECK(keymap.bindings()[0].chord == ctrl_r);

    // A freed chord can be bound again, to anything.
    REQUIRE(keymap.bind(f5, "app.quit"));
    CHECK(keymap.resolve(f5)->command == "app.quit");

    keymap.clear();
    CHECK(keymap.bindings().empty());
    CHECK(keymap.resolve(ctrl_r) == nullptr);
    CHECK(keymap.chords_for("nav.refresh").empty());
}

TEST_CASE("chords_for returns chords in bind order, with and without arg", "[keymap][bind]") {
    Keymap keymap;
    const KeyChord down{Key::Down, kNone};
    const KeyChord shift_down{Key::Down, kShift};
    const KeyChord j{Key::J, kNone};
    const KeyChord shift_j{Key::J, kShift};
    REQUIRE(keymap.bind(shift_down, "list.cursor_down", CursorArg::Extend));
    REQUIRE(keymap.bind(down, "list.cursor_down"));
    REQUIRE(keymap.bind(KeyChord{Key::Up, kNone}, "list.cursor_up"));
    REQUIRE(keymap.bind(j, "list.cursor_down"));
    REQUIRE(keymap.bind(shift_j, "list.cursor_down", CursorArg::Extend));

    CHECK(keymap.chords_for("list.cursor_down") ==
          std::vector<KeyChord>{shift_down, down, j, shift_j});
    CHECK(keymap.chords_for("list.cursor_down", CursorArg::None) == std::vector<KeyChord>{down, j});
    CHECK(keymap.chords_for("list.cursor_down", CursorArg::Extend) ==
          std::vector<KeyChord>{shift_down, shift_j});
    CHECK(keymap.chords_for("list.cursor_down", 2).empty());
    CHECK(keymap.chords_for("list.cursor").empty()); // no prefix matching
    CHECK(keymap.chords_for("missing").empty());

    // Unbinding keeps the order of the rest.
    REQUIRE(keymap.unbind(down));
    CHECK(keymap.chords_for("list.cursor_down") == std::vector<KeyChord>{shift_down, j, shift_j});
}

// ---------------------------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------------------------

TEST_CASE("load_default_keymap binds exactly the docs/keymap.md tables, in order",
          "[keymap][defaults]") {
    const KeymapDoc parsed = read_keymap_doc();
    const std::vector<DocRow>& doc = parsed.rows;
    CHECK(parsed.phase_rows[0] == 3);  // Phase 0 chords
    CHECK(parsed.phase_rows[2] == 25); // Phase 2 chords
    CHECK(parsed.phase_rows[3] == 21); // Phase 3 chords
    CHECK(parsed.phase_rows[4] == 5);  // Phase 4 chords
    CHECK(parsed.phase_rows[5] == 4);  // Phase 5 chords
    CHECK(parsed.phase_rows[6] == 2);  // Phase 6 chords
    CHECK(parsed.phase_rows[7] == 12); // Phase 7 chords
    CHECK(parsed.phase_rows[8] == 8);  // Phase 8 chords
    CHECK(parsed.phase_rows[9] == 10); // Phase 9A chords
    REQUIRE(doc.size() == 3 + 25 + 21 + 5 + 4 + 2 + 12 + 8 + 10);
    CHECK(doc.size() == 90); // 80 through Phase 8 + 10 Phase 9A chords

    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));
    const auto bindings = keymap.bindings();
    REQUIRE(bindings.size() == doc.size());
    for (std::size_t i = 0; i < doc.size(); ++i) {
        INFO("row " << i << ": " << doc[i].chord << " -> " << doc[i].command);
        CHECK(to_string(bindings[i].chord) == doc[i].chord);
        CHECK(bindings[i].chord == chord(doc[i].chord));
        CHECK(bindings[i].command == doc[i].command);
        CHECK(bindings[i].arg == doc[i].arg);
    }
    // Phase 8 ships one default sequence: Ctrl+K Ctrl+S -> keymap.edit.
    REQUIRE(keymap.sequence_bindings().size() == 1);
    CHECK(to_string(keymap.sequence_bindings()[0].sequence) == "Ctrl+K Ctrl+S");
    CHECK(keymap.sequence_bindings()[0].command == "keymap.edit");
    CHECK(keymap.sequence_bindings()[0].arg == 0);
}

TEST_CASE("every default binding resolves to a registered built-in", "[keymap][defaults]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    std::set<CommandId> bound;
    for (const KeyBinding& b : keymap.bindings()) {
        INFO("binding " << to_string(b.chord) << " -> " << b.command);
        const CommandInfo* info = registry.find(std::string_view{b.command});
        REQUIRE(info != nullptr);
        const std::optional<Command> c = navigator::resolve_command(keymap, registry, b.chord);
        CHECK(c == Command{info->id, b.arg});
        bound.insert(info->id);
    }
    // Every registered command has a default chord, except the documented "(unbound)" rows.
    const std::set<CommandId> unbound{CommandId::CloseLeftTabs,
                                      CommandId::DeleteLayout,
                                      CommandId::NewWindow,
                                      CommandId::FocusSidebar,
                                      CommandId::RenameBookmark,
                                      CommandId::MoveBookmarkUp,
                                      CommandId::MoveBookmarkDown,
                                      CommandId::ClearRecents,
                                      CommandId::ToggleRecentsTracking,
                                      CommandId::ToggleViewLock,
                                      CommandId::ClearFilter,
                                      CommandId::ToggleFuzzy,
                                      CommandId::CancelSearch,
                                      CommandId::RebuildIndex,
                                      CommandId::ToggleInspectorLock,
                                      CommandId::InspectorZoomFit,
                                      CommandId::InspectorZoom1to1,
                                      CommandId::InspectorPlay,
                                      CommandId::CycleInspectorPlacement,
                                      CommandId::ToggleInspectorGrid,
                                      CommandId::ToggleInspectorMinimap,
                                      CommandId::OpenTerminalHere,
                                      CommandId::CancelOperation,
                                      CommandId::TogglePauseOperation,
                                      CommandId::OpenTrash,
                                      CommandId::RestoreFromTrash,
                                      CommandId::EmptyTrash,
                                      CommandId::EditKeymap,
                                      CommandId::ResetKeymap,
                                      CommandId::ExportKeymap,
                                      CommandId::OpenWith,
                                      CommandId::ViewCycleMode,
                                      CommandId::GroupByNone,
                                      CommandId::GroupByDate,
                                      CommandId::GroupByType,
                                      CommandId::GroupBySize,
                                      CommandId::GroupCycle,
                                      CommandId::ViewLockFolder,
                                      CommandId::ViewLockHierarchy,
                                      CommandId::ViewLockGlobal,
                                      CommandId::TreeExpand,
                                      CommandId::TreeCollapse,
                                      CommandId::TreeCollapseAll};
    for (const CommandInfo& info : registry.all()) {
        INFO("command " << info.name);
        CHECK(bound.contains(info.id) == !unbound.contains(info.id));
    }
    for (const CommandId id : unbound) {
        INFO("command " << to_string(id));
        CHECK(keymap.chords_for(to_string(id)).empty());
    }
}

TEST_CASE("docs/keymap.md (unbound) rows are exactly the unbound built-ins", "[keymap][defaults]") {
    const KeymapDoc doc = read_keymap_doc();
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    std::set<std::string> documented(doc.unbound.begin(), doc.unbound.end());
    CHECK(documented.size() == doc.unbound.size()); // no duplicate (unbound) rows
    // In Phase 8, ops.toggle_panel moved from "(unbound)" in the Phase 7 table to Ctrl+J in the
    // Phase 8 table, so remove it from the historical Phase 7 unbound rows.
    documented.erase("ops.toggle_panel");
    CHECK(documented == std::set<std::string>{"tab.close_left",
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
                                              "view.tree_collapse_all"});
    std::set<std::string> actual;
    for (const CommandInfo& info : registry.all()) {
        if (keymap.chords_for(info.name).empty()) {
            actual.insert(std::string(info.name));
        }
    }
    CHECK(actual == documented);
}

TEST_CASE("default chords are unique and leave reserved chords free", "[keymap][defaults]") {
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));
    std::set<std::string> seen;
    for (const KeyBinding& b : keymap.bindings()) {
        INFO("chord " << to_string(b.chord));
        CHECK(seen.insert(to_string(b.chord)).second);
    }
    // Reserved for later phases or avoided (docs/keymap.md, "Phase 3..7 defaults"): Ctrl+P
    // stays free for quick-open / search. Ctrl+E is bound since Phase 5 (search.flatten). Space
    // is bound since Phase 6 (view.inspect); Ctrl+Space stays reserved for cursor selection
    // (Phase 8). Ctrl+C and Ctrl+Shift+N are bound since Phase 7 (edit.copy, file.new_folder).
    // F8 stays free (Total Commander users read it as "delete"); Ctrl+F5 and Alt+F5/F7/F8 were
    // rejected for the other-pane commands.
    for (const char* text : {"Ctrl+P", "Ctrl+N", "Ctrl+Alt+Left", "Ctrl+Alt+Right", "Ctrl+Alt+Up",
                             "Ctrl+Alt+Down", "Ctrl+Backslash", "F8", "Shift+F8", "Ctrl+F5",
                             "Alt+F5", "Alt+F7", "Alt+F8", "Ctrl+Shift+Y", "Ctrl+Delete"}) {
        INFO("reserved chord " << text);
        CHECK(keymap.resolve(chord(text)) == nullptr);
    }
}

TEST_CASE("default primary chords", "[keymap][defaults]") {
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));
    const auto primary = [&](std::string_view command) {
        const std::vector<KeyChord> chords = keymap.chords_for(command, 0);
        REQUIRE_FALSE(chords.empty());
        return to_string(chords.front());
    };
    CHECK(primary("app.quit") == "Ctrl+Q");
    CHECK(primary("list.activate") == "Enter");
    CHECK(primary("nav.back") == "Alt+Left");
    CHECK(primary("nav.up") == "Alt+Up");
    CHECK(primary("nav.refresh") == "F5");
    CHECK(primary("list.cursor_down") == "Down");
    CHECK(to_string(keymap.chords_for("list.cursor_down", CursorArg::Extend).front()) ==
          "Shift+Down");
    // Phase 3
    CHECK(primary("tab.new") == "Ctrl+T");
    CHECK(primary("tab.close") == "Ctrl+W");
    CHECK(primary("tab.reopen_closed") == "Ctrl+Shift+T");
    CHECK(primary("tab.next") == "Ctrl+Tab");
    CHECK(primary("tab.previous") == "Ctrl+Shift+Tab");
    CHECK(primary("tab.move_left") == "Ctrl+Shift+PageUp");
    CHECK(primary("tab.move_right") == "Ctrl+Shift+PageDown");
    CHECK(primary("tab.duplicate") == "Ctrl+Shift+D");
    CHECK(primary("tab.search") == "Ctrl+Shift+A");
    CHECK(primary("list.open_in_new_tab") == "Ctrl+Enter");
    CHECK(primary("pane.split_right") == "F3");
    CHECK(primary("pane.split_down") == "Shift+F3");
    CHECK(primary("pane.focus_next") == "F6");
    CHECK(primary("pane.focus_previous") == "Shift+F6");
    CHECK(primary("pane.close") == "Ctrl+Shift+W");
    CHECK(primary("layout.save") == "Ctrl+Shift+S");
    CHECK(primary("layout.load") == "Ctrl+Shift+O");
    // Phase 4
    CHECK(primary("view.toggle_sidebar") == "Ctrl+B");
    CHECK(primary("nav.focus_address_bar") == "Ctrl+L");
    CHECK(primary("nav.goto") == "Ctrl+G");
    CHECK(primary("bookmarks.toggle") == "Ctrl+D");
    CHECK(primary("edit.copy_path") == "Ctrl+Shift+C");
    // Phase 5
    CHECK(primary("list.filter") == "Ctrl+F");
    CHECK(primary("search.flatten") == "Ctrl+E");
    CHECK(primary("search.flatten_volume") == "Ctrl+Shift+E");
    CHECK(primary("search.types") == "Ctrl+Shift+F");
    // Phase 6
    CHECK(primary("view.inspect") == "Space");
    CHECK(keymap.chords_for("view.inspect").size() == 2);
    CHECK(to_string(keymap.chords_for("view.inspect")[1]) == "Alt+KeypadEnter");
    // Phase 7
    CHECK(primary("edit.copy") == "Ctrl+C");
    CHECK(primary("edit.cut") == "Ctrl+X");
    CHECK(primary("edit.paste") == "Ctrl+V");
    CHECK(primary("edit.undo") == "Ctrl+Z");
    CHECK(primary("edit.redo") == "Ctrl+Shift+Z");
    CHECK(keymap.chords_for("edit.redo").size() == 2);
    CHECK(to_string(keymap.chords_for("edit.redo")[1]) == "Ctrl+Y");
    CHECK(primary("file.trash") == "Delete");
    CHECK(primary("file.delete_permanently") == "Shift+Delete");
    CHECK(primary("file.rename") == "F2");
    CHECK(primary("file.new_folder") == "Ctrl+Shift+N");
    CHECK(primary("file.copy_to_other_pane") == "F7");
    CHECK(primary("file.move_to_other_pane") == "Shift+F7");
    // Ctrl+Shift+C (copy path) and Ctrl+C (copy files) are distinct chords.
    CHECK(primary("edit.copy_path") == "Ctrl+Shift+C");
    // Ctrl+L (address bar) and Ctrl+Shift+L (theme) are distinct chords.
    CHECK(primary("view.toggle_theme") == "Ctrl+Shift+L");
}

TEST_CASE("default keymap loads once; a second load fails and changes nothing",
          "[keymap][defaults]") {
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));
    const std::size_t count = keymap.bindings().size();
    const auto again = navigator::load_default_keymap(keymap);
    REQUIRE_FALSE(again);
    CHECK(again.error().code == ErrorCode::AlreadyExists);
    CHECK(keymap.bindings().size() == count);
}

TEST_CASE("default keymap is all-or-nothing on a pre-bound chord", "[keymap][defaults]") {
    Keymap keymap;
    // Shift+F7 is the last default row, so a partial load would have bound everything else.
    const KeyChord shift_end{Key::F7, kShift};
    REQUIRE(keymap.bind(shift_end, "user.custom", 42));
    REQUIRE(keymap.bind(KeyChord{Key::F9, kNone}, "user.other"));

    const auto r = navigator::load_default_keymap(keymap);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    REQUIRE(keymap.bindings().size() == 2);
    CHECK(keymap.resolve(shift_end)->command == "user.custom");
    CHECK(keymap.resolve(shift_end)->arg == 42);
    CHECK(keymap.resolve(KeyChord{Key::Q, kCtrl}) == nullptr);
}

TEST_CASE("default keymap adds to unrelated existing bindings", "[keymap][defaults]") {
    Keymap keymap;
    REQUIRE(keymap.bind(KeyChord{Key::F9, kNone}, "user.other"));
    REQUIRE(navigator::load_default_keymap(keymap));
    CHECK(keymap.bindings()[0].command == "user.other");
    CHECK(keymap.resolve(KeyChord{Key::Q, kCtrl})->command == "app.quit");
}

// ---------------------------------------------------------------------------------------------
// resolve_command
// ---------------------------------------------------------------------------------------------

TEST_CASE("resolve_command carries the binding arg", "[keymap][resolve]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    CHECK(navigator::resolve_command(keymap, registry, chord("Shift+Down")) ==
          Command{CommandId::CursorDown, CursorArg::Extend});
    CHECK(navigator::resolve_command(keymap, registry, chord("Down")) ==
          Command{CommandId::CursorDown, CursorArg::None});
    CHECK(navigator::resolve_command(keymap, registry, chord("Shift+Home")) ==
          Command{CommandId::CursorHome, CursorArg::Extend});
    CHECK(navigator::resolve_command(keymap, registry, chord("MouseBack")) ==
          Command{CommandId::NavigateBack, 0});
}

TEST_CASE("resolve_command misses unbound chords and unregistered names", "[keymap][resolve]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(keymap.bind(KeyChord{Key::F9, kNone}, "plugin.not_registered"));
    REQUIRE(keymap.bind(KeyChord{Key::Q, kCtrl}, "app.quit"));

    CHECK_FALSE(navigator::resolve_command(keymap, registry, KeyChord{Key::F9, kNone}));
    CHECK_FALSE(navigator::resolve_command(keymap, registry, KeyChord{Key::F10, kNone}));
    CHECK_FALSE(navigator::resolve_command(keymap, registry, KeyChord{Key::Q, kCtrl | kShift}));
    CHECK(navigator::resolve_command(keymap, registry, KeyChord{Key::Q, kCtrl}) ==
          Command{CommandId::Quit, 0});

    const CommandRegistry empty;
    CHECK_FALSE(navigator::resolve_command(keymap, empty, KeyChord{Key::Q, kCtrl}));
}

TEST_CASE("a key sequence becomes the expected command list", "[keymap][resolve][sequence]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // As the ui does it: each chord goes through resolve_command, hits are enqueued, misses
    // (unbound chords, inexact modifiers) are dropped, and the queue drains at frame end. F8
    // and Ctrl+P stay unbound.
    const char* presses[] = {"Down",         "Down",   "Shift+Down", "F8",        "Ctrl+P",
                             "Ctrl+A",       "Escape", "Enter",      "Backspace", "Alt+Left",
                             "MouseForward", "Ctrl+H", "Shift+End",  "Ctrl+R",    "Ctrl+Q"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::CursorDown, 0},
        {CommandId::CursorDown, 0},
        {CommandId::CursorDown, CursorArg::Extend},
        {CommandId::SelectAll, 0},
        {CommandId::ClearSelection, 0},
        {CommandId::Activate, 0},
        {CommandId::NavigateUp, 0},
        {CommandId::NavigateBack, 0},
        {CommandId::NavigateForward, 0},
        {CommandId::ToggleHidden, 0},
        {CommandId::CursorEnd, CursorArg::Extend},
        {CommandId::Refresh, 0},
        {CommandId::Quit, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
}

TEST_CASE("a Phase 3 key sequence becomes the expected command list",
          "[keymap][resolve][sequence]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // Ctrl+N (window.new) stays unbound and is dropped; Ctrl+Shift+N is file.new_folder since
    // Phase 7.
    const char* presses[] = {"Ctrl+T",
                             "Ctrl+Tab",
                             "Ctrl+Shift+Tab",
                             "Ctrl+N",
                             "Ctrl+PageDown",
                             "Ctrl+Shift+PageUp",
                             "Ctrl+Enter",
                             "F3",
                             "Shift+F3",
                             "F6",
                             "Shift+F6",
                             "Ctrl+Shift+W",
                             "Ctrl+W",
                             "Ctrl+F4",
                             "Ctrl+Shift+T",
                             "Ctrl+Shift+D",
                             "Ctrl+Shift+A",
                             "Ctrl+Shift+N",
                             "Ctrl+Shift+S",
                             "Ctrl+Shift+O",
                             "Ctrl+Shift+PageDown",
                             "Ctrl+KeypadEnter",
                             "Ctrl+PageUp"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::NewTab, navigator::NewTabArg::None},
        {CommandId::NextTab, 0},
        {CommandId::PreviousTab, 0},
        {CommandId::NextTab, 0},
        {CommandId::MoveTabLeft, 0},
        {CommandId::OpenInNewTab, 0},
        {CommandId::SplitRight, 0},
        {CommandId::SplitDown, 0},
        {CommandId::FocusNextPane, 0},
        {CommandId::FocusPreviousPane, 0},
        {CommandId::ClosePane, 0},
        {CommandId::CloseTab, 0},
        {CommandId::CloseTab, 0},
        {CommandId::ReopenClosedTab, 0},
        {CommandId::DuplicateTab, 0},
        {CommandId::SearchTabs, 0},
        {CommandId::NewFolder, 0},
        {CommandId::SaveLayout, 0},
        {CommandId::LoadLayout, 0},
        {CommandId::MoveTabRight, 0},
        {CommandId::OpenInNewTab, 0},
        {CommandId::PreviousTab, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
}

TEST_CASE("a Phase 4 key sequence becomes the expected command list",
          "[keymap][resolve][sequence]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // Ctrl+P and Ctrl+N stay unbound and are dropped; Ctrl+C is edit.copy since Phase 7 and
    // distinct from Ctrl+Shift+C; Ctrl+Shift+L is still the theme.
    const char* presses[] = {"Ctrl+B",       "Ctrl+L", "Ctrl+P",       "Ctrl+G", "Ctrl+D", "Ctrl+C",
                             "Ctrl+Shift+C", "F6",     "Ctrl+Shift+L", "Ctrl+N", "Ctrl+B"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::ToggleSidebar, 0}, {CommandId::FocusAddressBar, 0},
        {CommandId::GoTo, 0},          {CommandId::ToggleBookmark, 0},
        {CommandId::Copy, 0},          {CommandId::CopyPath, navigator::CopyPathArg::None},
        {CommandId::FocusNextPane, 0}, {CommandId::ToggleTheme, 0},
        {CommandId::ToggleSidebar, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
}

TEST_CASE("a Phase 5 key sequence becomes the expected command list",
          "[keymap][resolve][sequence]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // Ctrl+F / Ctrl+Shift+F and Ctrl+E / Ctrl+Shift+E are distinct chords (exact modifiers);
    // Alt+F, Ctrl+Alt+E and Ctrl+P stay unbound and are dropped. Escape is still
    // list.clear_selection (the cascade runs in the ui, not the keymap).
    const char* presses[] = {"Ctrl+F",       "Down",   "Ctrl+E",       "Escape",
                             "Ctrl+Shift+E", "Alt+F",  "Ctrl+Shift+F", "Ctrl+Alt+E",
                             "Ctrl+P",       "Ctrl+E", "Enter"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::FocusFilter, 0},   {CommandId::CursorDown, 0},
        {CommandId::ToggleFlatten, 0}, {CommandId::ClearSelection, 0},
        {CommandId::FlattenVolume, 0}, {CommandId::ToggleTypesPopup, 0},
        {CommandId::ToggleFlatten, 0}, {CommandId::Activate, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
}

TEST_CASE("Phase 5 defaults conflict with user bindings all-or-nothing",
          "[keymap][defaults][phase5]") {
    // A user binding on a Phase 5 chord (e.g. an old IBus-style Ctrl+Shift+E habit) blocks the
    // whole default load and leaves the keymap as it was.
    Keymap keymap;
    REQUIRE(keymap.bind(chord("Ctrl+Shift+E"), "user.emoji"));
    const auto r = navigator::load_default_keymap(keymap);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    REQUIRE(keymap.bindings().size() == 1);
    CHECK(keymap.resolve(chord("Ctrl+Shift+E"))->command == "user.emoji");
    CHECK(keymap.resolve(chord("Ctrl+F")) == nullptr);
    CHECK(keymap.resolve(chord("Ctrl+E")) == nullptr);

    // After the defaults load, a user can rebind a Phase 5 chord explicitly.
    Keymap defaults;
    REQUIRE(navigator::load_default_keymap(defaults));
    const auto clash = defaults.bind(chord("Ctrl+F"), "user.find");
    REQUIRE_FALSE(clash);
    CHECK(clash.error().code == ErrorCode::AlreadyExists);
    CHECK(defaults.resolve(chord("Ctrl+F"))->command == "list.filter");
    REQUIRE(defaults.unbind(chord("Ctrl+F")));
    REQUIRE(defaults.bind(chord("Ctrl+F"), "user.find"));
    CHECK(defaults.resolve(chord("Ctrl+F"))->command == "user.find");
    CHECK(defaults.chords_for("list.filter").empty());
}

TEST_CASE("a Phase 6 key sequence becomes the expected command list",
          "[keymap][resolve][sequence][phase6]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // Space toggles the Inspector; chords match exactly, so Shift+Space and Ctrl+Space (reserved
    // for Phase 8 cursor selection) resolve to nothing. Alt+KeypadEnter is a second Inspect key,
    // distinct from KeypadEnter (list.activate) and Ctrl+KeypadEnter (list.open_in_new_tab).
    // Arrow keys keep moving the list cursor; the Inspector follows it.
    const char* presses[] = {
        "Space",           "Down",        "Shift+Space", "Ctrl+Space",     "Space",
        "Alt+KeypadEnter", "KeypadEnter", "Escape",      "Ctrl+Alt+Space", "Ctrl+KeypadEnter",
        "Alt+Space"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::Inspect, 0},
        {CommandId::CursorDown, 0},
        {CommandId::ToggleCursorSelected, 0},
        {CommandId::Inspect, 0},
        {CommandId::Inspect, 0},
        {CommandId::Activate, 0},
        {CommandId::ClearSelection, 0},
        {CommandId::OpenInNewTab, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
    CHECK_FALSE(navigator::resolve_command(keymap, registry, chord("Shift+Space")));
    CHECK(navigator::resolve_command(keymap, registry, chord("Ctrl+Space")) ==
          Command{CommandId::ToggleCursorSelected, 0});
    // No default binds any inspector.* command (header buttons and palette only).
    for (const KeyBinding& b : keymap.bindings()) {
        INFO("binding " << to_string(b.chord) << " -> " << b.command);
        CHECK_FALSE(b.command.starts_with("inspector."));
    }
}

TEST_CASE("Phase 6 defaults conflict with user bindings all-or-nothing",
          "[keymap][defaults][phase6]") {
    // A user binding on Space (e.g. a habit from another file manager) blocks the whole default
    // load and leaves the keymap as it was.
    Keymap keymap;
    REQUIRE(keymap.bind(chord("Space"), "user.preview"));
    const auto r = navigator::load_default_keymap(keymap);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    REQUIRE(keymap.bindings().size() == 1);
    CHECK(keymap.resolve(chord("Space"))->command == "user.preview");
    CHECK(keymap.resolve(chord("Ctrl+Q")) == nullptr);

    // After the defaults load, Space can be rebound explicitly; Alt+KeypadEnter keeps Inspect.
    Keymap defaults;
    REQUIRE(navigator::load_default_keymap(defaults));
    const auto clash = defaults.bind(chord("Space"), "user.preview");
    REQUIRE_FALSE(clash);
    CHECK(clash.error().code == ErrorCode::AlreadyExists);
    REQUIRE(defaults.unbind(chord("Space")));
    REQUIRE(defaults.bind(chord("Space"), "user.preview"));
    CHECK(defaults.resolve(chord("Space"))->command == "user.preview");
    const std::vector<KeyChord> left = defaults.chords_for("view.inspect");
    REQUIRE(left.size() == 1);
    CHECK(to_string(left.front()) == "Alt+KeypadEnter");
}

TEST_CASE("a Phase 7 key sequence becomes the expected command list",
          "[keymap][resolve][sequence][phase7]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // Copy, paste, undo, redo both ways, trash, permanent delete, rename, new folder, and the
    // other-pane commands. Chords match exactly: Ctrl+Delete, Alt+F7, F8 and Ctrl+Shift+Y
    // resolve to nothing; Ctrl+Shift+C is still edit.copy_path.
    const char* presses[] = {"Ctrl+C",       "Ctrl+V",       "Ctrl+Z",       "Ctrl+Shift+Z",
                             "Ctrl+Y",       "Ctrl+X",       "Ctrl+Delete",  "Delete",
                             "Ctrl+Z",       "Shift+Delete", "F2",           "Ctrl+Shift+N",
                             "F7",           "Alt+F7",       "Shift+F7",     "F8",
                             "Ctrl+Shift+Y", "Ctrl+Shift+C", "Ctrl+Shift+F7"};
    CommandQueue queue;
    for (const char* text : presses) {
        if (const std::optional<Command> c =
                navigator::resolve_command(keymap, registry, chord(text))) {
            queue.enqueue(*c);
        }
    }
    std::vector<Command> ran;
    queue.drain([&](const Command& c) { ran.push_back(c); });

    const std::vector<Command> expected{
        {CommandId::Copy, 0},
        {CommandId::Paste, navigator::PasteArg::None},
        {CommandId::Undo, 0},
        {CommandId::Redo, 0},
        {CommandId::Redo, 0},
        {CommandId::Cut, 0},
        {CommandId::MoveToTrash, 0},
        {CommandId::Undo, 0},
        {CommandId::DeletePermanently, 0},
        {CommandId::Rename, 0},
        {CommandId::NewFolder, 0},
        {CommandId::CopyToOtherPane, 0},
        {CommandId::MoveToOtherPane, 0},
        {CommandId::CopyPath, navigator::CopyPathArg::None},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
    // ops.cancel / ops.toggle_pause, trash.* and file.open_terminal_here remain unbound
    // (ops.toggle_panel gained Ctrl+J in Phase 8).
    for (const KeyBinding& b : keymap.bindings()) {
        INFO("binding " << to_string(b.chord) << " -> " << b.command);
        CHECK(b.command != "ops.cancel");
        CHECK(b.command != "ops.toggle_pause");
        CHECK_FALSE(b.command.starts_with("trash."));
        CHECK(b.command != "file.open_terminal_here");
        if (b.command == "edit.paste") {
            CHECK(b.arg == navigator::PasteArg::None);
        }
    }
}

TEST_CASE("edit.paste carries PasteArg flags through a binding", "[keymap][resolve][phase7]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(keymap.bind(chord("Ctrl+V"), "edit.paste", navigator::PasteArg::None));
    REQUIRE(keymap.bind(chord("Ctrl+Alt+V"), "edit.paste", navigator::PasteArg::IntoCursorFolder));
    // Unknown bits are reserved: the keymap passes them through untouched (the handler ignores
    // them).
    REQUIRE(keymap.bind(chord("Ctrl+Shift+V"), "edit.paste",
                        navigator::PasteArg::IntoCursorFolder | (std::uint64_t{1} << 5U)));
    CHECK(navigator::resolve_command(keymap, registry, chord("Ctrl+V")) ==
          Command{CommandId::Paste, navigator::PasteArg::None});
    CHECK(navigator::resolve_command(keymap, registry, chord("Ctrl+Alt+V")) ==
          Command{CommandId::Paste, navigator::PasteArg::IntoCursorFolder});
    const std::optional<Command> extra =
        navigator::resolve_command(keymap, registry, chord("Ctrl+Shift+V"));
    CHECK(extra.has_value());
    CHECK((extra.value_or(Command{}).arg & navigator::PasteArg::IntoCursorFolder) != 0);
    CHECK(keymap.chords_for("edit.paste", navigator::PasteArg::IntoCursorFolder) ==
          std::vector<KeyChord>{chord("Ctrl+Alt+V")});
}

TEST_CASE("Phase 7 defaults conflict with user bindings all-or-nothing",
          "[keymap][defaults][phase7]") {
    // A user binding on Delete (e.g. a habit of binding it to a custom command) blocks the
    // whole default load and leaves the keymap as it was: no file command lands half-bound.
    Keymap keymap;
    REQUIRE(keymap.bind(chord("Delete"), "user.delete"));
    const auto r = navigator::load_default_keymap(keymap);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::AlreadyExists);
    REQUIRE(keymap.bindings().size() == 1);
    CHECK(keymap.resolve(chord("Delete"))->command == "user.delete");
    CHECK(keymap.resolve(chord("Ctrl+C")) == nullptr);
    CHECK(keymap.resolve(chord("Shift+Delete")) == nullptr);

    // After the defaults load, Ctrl+Y can be rebound; Ctrl+Shift+Z keeps edit.redo.
    Keymap defaults;
    REQUIRE(navigator::load_default_keymap(defaults));
    const auto clash = defaults.bind(chord("Ctrl+Y"), "user.yank");
    REQUIRE_FALSE(clash);
    CHECK(clash.error().code == ErrorCode::AlreadyExists);
    REQUIRE(defaults.unbind(chord("Ctrl+Y")));
    REQUIRE(defaults.bind(chord("Ctrl+Y"), "user.yank"));
    CHECK(defaults.resolve(chord("Ctrl+Y"))->command == "user.yank");
    const std::vector<KeyChord> left = defaults.chords_for("edit.redo");
    REQUIRE(left.size() == 1);
    CHECK(to_string(left.front()) == "Ctrl+Shift+Z");
}

TEST_CASE("rebinding changes what a sequence produces", "[keymap][resolve][sequence]") {
    const CommandRegistry registry = builtin_registry();
    Keymap keymap;
    REQUIRE(navigator::load_default_keymap(keymap));

    // User swaps Backspace from nav.up to nav.back.
    REQUIRE(keymap.unbind(chord("Backspace")));
    REQUIRE(keymap.bind(chord("Backspace"), "nav.back"));

    std::vector<Command> got;
    for (const char* text : {"Backspace", "Alt+Up", "Backspace"}) {
        if (const auto c = navigator::resolve_command(keymap, registry, chord(text))) {
            got.push_back(*c);
        }
    }
    CHECK(got == std::vector<Command>{{CommandId::NavigateBack, 0},
                                      {CommandId::NavigateUp, 0},
                                      {CommandId::NavigateBack, 0}});
    // Backspace now comes after Alt+Left and MouseBack, so the primary hint is unchanged.
    CHECK(to_string(keymap.chords_for("nav.back").front()) == "Alt+Left");
    CHECK(to_string(keymap.chords_for("nav.up").front()) == "Alt+Up");
}

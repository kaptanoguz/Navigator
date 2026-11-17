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

// The chord tables of docs/keymap.md ("Phase 0 defaults", "Phase 2 defaults", "Phase 3
// defaults"), in document order. `rows` skips the rows that are not chords ("window close",
// "(unbound)"); `unbound` lists the command names of the "(unbound)" rows.
struct KeymapDoc {
    std::vector<DocRow> rows;
    std::vector<std::string> unbound;
    std::size_t phase_rows[4] = {0, 0, 0, 0}; // chord rows per phase (index 1 unused)
};

KeymapDoc read_keymap_doc() {
    std::ifstream in(NAVIGATOR_KEYMAP_DOC);
    REQUIRE(in.good());
    KeymapDoc doc;
    std::string line;
    int section = 0; // 0: elsewhere, otherwise the phase of the table being read (0, 2, 3)
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
            } else {
                in_table = false;
            }
            continue;
        }
        if (line.starts_with("### ")) {
            in_table = false; // sub-sections (Mouse, Popups) are not chord tables
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
    REQUIRE(doc.size() == 3 + 25 + 21);

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
    const std::set<CommandId> unbound{CommandId::FocusAddressBar, CommandId::CloseLeftTabs,
                                      CommandId::DeleteLayout, CommandId::NewWindow};
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
    CHECK(documented == std::set<std::string>{"nav.focus_address_bar", "tab.close_left",
                                              "layout.delete", "window.new"});
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
    // Reserved for later phases or avoided (docs/keymap.md, "Phase 3 defaults").
    for (const char* text :
         {"Ctrl+E", "Ctrl+B", "Ctrl+Shift+P", "Ctrl+Comma", "Ctrl+Space", "Ctrl+Shift+N", "Ctrl+N",
          "Ctrl+Alt+Left", "Ctrl+Alt+Right", "Ctrl+Alt+Up", "Ctrl+Alt+Down", "Ctrl+Backslash"}) {
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
    // Ctrl+Shift+O is the last default row, so a partial load would have bound everything else.
    const KeyChord shift_end{Key::O, kCtrl | kShift};
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
    // (unbound chords, inexact modifiers) are dropped, and the queue drains at frame end.
    const char* presses[] = {"Down",         "Down",   "Shift+Down", "F7",        "Ctrl+E",
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

    // Ctrl+N (window.new) and Ctrl+Shift+N stay unbound and are dropped.
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
        {CommandId::SaveLayout, 0},
        {CommandId::LoadLayout, 0},
        {CommandId::MoveTabRight, 0},
        {CommandId::OpenInNewTab, 0},
        {CommandId::PreviousTab, 0},
    };
    CHECK(ran == expected);
    CHECK(queue.empty());
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

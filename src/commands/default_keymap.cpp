// load_default_keymap(): Navigator's default bindings.
//
// This table must match docs/keymap.md exactly: the Phase 0 table (minus "window close", which
// is a window event, not a chord), then the Phase 2, 3, 4, 5, 6, 7 and 8 tables, in the same row
// order ("(unbound)" rows are skipped), then the default sequences (docs/keymap.md "Default
// sequences (Phase 8)"). The first chord of a command is its primary key hint, so order matters.
// tests/commands checks the table.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

struct DefaultBinding {
    KeyChord chord;
    std::string_view command;
    std::uint64_t arg;
};

constexpr Modifiers kNone = Modifiers::None;
constexpr Modifiers kCtrl = Modifiers::Ctrl;
constexpr Modifiers kShift = Modifiers::Shift;
constexpr Modifiers kAlt = Modifiers::Alt;
constexpr Modifiers kCtrlShift = Modifiers::Ctrl | Modifiers::Shift;
constexpr std::uint64_t kExtend = CursorArg::Extend;
constexpr std::uint64_t kKeepSelection = CursorArg::KeepSelection;

constexpr std::array kDefaults{
    // Phase 0
    DefaultBinding{{Key::Q, kCtrl}, "app.quit", 0},
    DefaultBinding{{Key::F12, kNone}, "view.toggle_fps_overlay", 0},
    DefaultBinding{{Key::L, kCtrl | kShift}, "view.toggle_theme", 0},
    // Phase 2
    DefaultBinding{{Key::Enter, kNone}, "list.activate", 0},
    DefaultBinding{{Key::KeypadEnter, kNone}, "list.activate", 0},
    DefaultBinding{{Key::Left, kAlt}, "nav.back", 0},
    DefaultBinding{{Key::MouseBack, kNone}, "nav.back", 0},
    DefaultBinding{{Key::Right, kAlt}, "nav.forward", 0},
    DefaultBinding{{Key::MouseForward, kNone}, "nav.forward", 0},
    DefaultBinding{{Key::Up, kAlt}, "nav.up", 0},
    DefaultBinding{{Key::Backspace, kNone}, "nav.up", 0},
    DefaultBinding{{Key::F5, kNone}, "nav.refresh", 0},
    DefaultBinding{{Key::R, kCtrl}, "nav.refresh", 0},
    DefaultBinding{{Key::A, kCtrl}, "list.select_all", 0},
    DefaultBinding{{Key::Escape, kNone}, "list.clear_selection", 0},
    DefaultBinding{{Key::H, kCtrl}, "view.toggle_hidden", 0},
    DefaultBinding{{Key::Up, kNone}, "list.cursor_up", 0},
    DefaultBinding{{Key::Up, kShift}, "list.cursor_up", kExtend},
    DefaultBinding{{Key::Down, kNone}, "list.cursor_down", 0},
    DefaultBinding{{Key::Down, kShift}, "list.cursor_down", kExtend},
    DefaultBinding{{Key::PageUp, kNone}, "list.cursor_page_up", 0},
    DefaultBinding{{Key::PageUp, kShift}, "list.cursor_page_up", kExtend},
    DefaultBinding{{Key::PageDown, kNone}, "list.cursor_page_down", 0},
    DefaultBinding{{Key::PageDown, kShift}, "list.cursor_page_down", kExtend},
    DefaultBinding{{Key::Home, kNone}, "list.cursor_home", 0},
    DefaultBinding{{Key::Home, kShift}, "list.cursor_home", kExtend},
    DefaultBinding{{Key::End, kNone}, "list.cursor_end", 0},
    DefaultBinding{{Key::End, kShift}, "list.cursor_end", kExtend},
    // Phase 3
    DefaultBinding{{Key::T, kCtrl}, "tab.new", 0},
    DefaultBinding{{Key::W, kCtrl}, "tab.close", 0},
    DefaultBinding{{Key::F4, kCtrl}, "tab.close", 0},
    DefaultBinding{{Key::T, kCtrlShift}, "tab.reopen_closed", 0},
    DefaultBinding{{Key::Tab, kCtrl}, "tab.next", 0},
    DefaultBinding{{Key::PageDown, kCtrl}, "tab.next", 0},
    DefaultBinding{{Key::Tab, kCtrlShift}, "tab.previous", 0},
    DefaultBinding{{Key::PageUp, kCtrl}, "tab.previous", 0},
    DefaultBinding{{Key::PageUp, kCtrlShift}, "tab.move_left", 0},
    DefaultBinding{{Key::PageDown, kCtrlShift}, "tab.move_right", 0},
    DefaultBinding{{Key::D, kCtrlShift}, "tab.duplicate", 0},
    DefaultBinding{{Key::A, kCtrlShift}, "tab.search", 0},
    DefaultBinding{{Key::Enter, kCtrl}, "list.open_in_new_tab", 0},
    DefaultBinding{{Key::KeypadEnter, kCtrl}, "list.open_in_new_tab", 0},
    DefaultBinding{{Key::F3, kNone}, "pane.split_right", 0},
    DefaultBinding{{Key::F3, kShift}, "pane.split_down", 0},
    DefaultBinding{{Key::F6, kNone}, "pane.focus_next", 0},
    DefaultBinding{{Key::F6, kShift}, "pane.focus_previous", 0},
    DefaultBinding{{Key::W, kCtrlShift}, "pane.close", 0},
    DefaultBinding{{Key::S, kCtrlShift}, "layout.save", 0},
    DefaultBinding{{Key::O, kCtrlShift}, "layout.load", 0},
    // Phase 4
    DefaultBinding{{Key::B, kCtrl}, "view.toggle_sidebar", 0},
    DefaultBinding{{Key::L, kCtrl}, "nav.focus_address_bar", 0},
    DefaultBinding{{Key::G, kCtrl}, "nav.goto", 0},
    DefaultBinding{{Key::D, kCtrl}, "bookmarks.toggle", 0},
    DefaultBinding{{Key::C, kCtrlShift}, "edit.copy_path", CopyPathArg::None},
    // Phase 5
    DefaultBinding{{Key::F, kCtrl}, "list.filter", 0},
    DefaultBinding{{Key::E, kCtrl}, "search.flatten", 0},
    DefaultBinding{{Key::E, kCtrlShift}, "search.flatten_volume", 0},
    DefaultBinding{{Key::F, kCtrlShift}, "search.types", 0},
    // Phase 6
    DefaultBinding{{Key::Space, kNone}, "view.inspect", 0},
    DefaultBinding{{Key::KeypadEnter, kAlt}, "view.inspect", 0},
    // Phase 7
    DefaultBinding{{Key::C, kCtrl}, "edit.copy", 0},
    DefaultBinding{{Key::X, kCtrl}, "edit.cut", 0},
    DefaultBinding{{Key::V, kCtrl}, "edit.paste", PasteArg::None},
    DefaultBinding{{Key::Z, kCtrl}, "edit.undo", 0},
    DefaultBinding{{Key::Z, kCtrlShift}, "edit.redo", 0},
    DefaultBinding{{Key::Y, kCtrl}, "edit.redo", 0},
    DefaultBinding{{Key::Delete, kNone}, "file.trash", 0},
    DefaultBinding{{Key::Delete, kShift}, "file.delete_permanently", 0},
    DefaultBinding{{Key::F2, kNone}, "file.rename", 0},
    DefaultBinding{{Key::N, kCtrlShift}, "file.new_folder", 0},
    DefaultBinding{{Key::F7, kNone}, "file.copy_to_other_pane", 0},
    DefaultBinding{{Key::F7, kShift}, "file.move_to_other_pane", 0},
    // Phase 8
    DefaultBinding{{Key::P, kCtrlShift}, "palette.open", 0},
    DefaultBinding{{Key::Comma, kCtrl}, "settings.open", 0},
    DefaultBinding{{Key::F10, kShift}, "menu.show_context", 0},
    DefaultBinding{{Key::Menu, kNone}, "menu.show_context", 0},
    DefaultBinding{{Key::Space, kCtrl}, "list.toggle_cursor_selected", 0},
    DefaultBinding{{Key::Up, kCtrl}, "list.cursor_up", kKeepSelection},
    DefaultBinding{{Key::Down, kCtrl}, "list.cursor_down", kKeepSelection},
    DefaultBinding{{Key::J, kCtrl}, "ops.toggle_panel", 0},
    // Phase 9A
    DefaultBinding{{Key::Digit1, kCtrl}, "view.mode_details", 0},
    DefaultBinding{{Key::Digit2, kCtrl}, "view.mode_icons", 0},
    DefaultBinding{{Key::Digit3, kCtrl}, "view.mode_list", 0},
    DefaultBinding{{Key::Digit4, kCtrl}, "view.mode_tree", 0},
    DefaultBinding{{Key::Equals, kCtrl}, "view.zoom_in", 0},
    DefaultBinding{{Key::KeypadPlus, kCtrl}, "view.zoom_in", 0},
    DefaultBinding{{Key::Minus, kCtrl}, "view.zoom_out", 0},
    DefaultBinding{{Key::KeypadMinus, kCtrl}, "view.zoom_out", 0},
    DefaultBinding{{Key::Digit0, kCtrl}, "view.zoom_reset", 0},
    DefaultBinding{{Key::Keypad0, kCtrl}, "view.zoom_reset", 0},
    // Phase 9B
    DefaultBinding{{Key::F2, kShift}, "file.batch_rename", 0},
    // Phase 10
    DefaultBinding{{Key::F4, kNone}, "file.open_terminal_here", 0},
    DefaultBinding{{Key::Digit5, kCtrl}, "view.mode_columns", 0},
    DefaultBinding{{Key::G, kCtrlShift}, "search.content", 0},
};

struct DefaultSequence {
    std::array<KeyChord, 2> chords;
    std::string_view command;
    std::uint64_t arg;
};

constexpr std::array kDefaultSequences{
    DefaultSequence{{KeyChord{Key::K, kCtrl}, KeyChord{Key::S, kCtrl}}, "keymap.edit", 0},
};

} // namespace

Result<void> load_default_keymap(Keymap& keymap) {
    // All-or-nothing: bind into a copy and replace the caller's map only when everything fit. The
    // tables themselves have no internal conflicts (tested), so only pre-existing bindings can
    // clash (identical chord, or a prefix relation with a sequence).
    Keymap work = keymap;
    for (const DefaultBinding& d : kDefaults) {
        if (auto r = work.bind(d.chord, d.command, d.arg); !r) {
            return fail(ErrorCode::AlreadyExists,
                        "default chord " + to_string(d.chord) + ": " + r.error().message);
        }
    }
    for (const DefaultSequence& d : kDefaultSequences) {
        const KeySequence sequence(std::vector<KeyChord>(d.chords.begin(), d.chords.end()));
        if (auto r = work.bind(sequence, d.command, d.arg); !r) {
            return fail(ErrorCode::AlreadyExists,
                        "default sequence " + to_string(sequence) + ": " + r.error().message);
        }
    }
    keymap = std::move(work);
    return ok();
}

} // namespace navigator

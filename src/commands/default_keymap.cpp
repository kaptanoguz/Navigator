// load_default_keymap(): Navigator's default bindings.
//
// This table must match docs/keymap.md exactly: the Phase 0 table (minus "window close", which
// is a window event, not a chord), then the Phase 2 table, then the Phase 3 table, in the same
// row order ("(unbound)" rows are skipped). The first
// chord of a command is its primary key hint, so order matters. tests/commands checks the table.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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
};

} // namespace

Result<void> load_default_keymap(Keymap& keymap) {
    // All-or-nothing: check every chord first so a conflict leaves the keymap unchanged. The
    // table itself has no duplicate chords (tested), so only pre-existing bindings can clash.
    for (const DefaultBinding& d : kDefaults) {
        if (const KeyBinding* existing = keymap.resolve(d.chord)) {
            return fail(ErrorCode::AlreadyExists, "default chord " + to_string(d.chord) +
                                                      " already bound to " + existing->command);
        }
    }
    for (const DefaultBinding& d : kDefaults) {
        if (auto r = keymap.bind(d.chord, d.command, d.arg); !r) {
            return r; // unreachable: chords validated above and the table is well-formed
        }
    }
    return ok();
}

} // namespace navigator

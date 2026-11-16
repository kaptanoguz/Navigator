// Built-in command table, register_builtin_commands(), to_string(CommandId).
//
// Adding a command: append a CommandId in navigator/commands.hpp (never renumber), add a case to
// to_string() and a row to kBuiltins, add its default binding to docs/keymap.md, and add the
// "cmd.<name>" title to the English and Turkish catalogs.
#include <array>
#include <string>
#include <string_view>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

struct Builtin {
    CommandId id;
    std::string_view name;
    std::string_view title_key;
};

// Order is registration order (and therefore palette order before any sorting).
constexpr std::array kBuiltins{
    Builtin{CommandId::Quit, "app.quit", "cmd.app.quit"},
    Builtin{CommandId::ToggleFpsOverlay, "view.toggle_fps_overlay", "cmd.view.toggle_fps_overlay"},
    Builtin{CommandId::ToggleTheme, "view.toggle_theme", "cmd.view.toggle_theme"},
    Builtin{CommandId::ToggleHidden, "view.toggle_hidden", "cmd.view.toggle_hidden"},
    Builtin{CommandId::NavigateBack, "nav.back", "cmd.nav.back"},
    Builtin{CommandId::NavigateForward, "nav.forward", "cmd.nav.forward"},
    Builtin{CommandId::NavigateUp, "nav.up", "cmd.nav.up"},
    Builtin{CommandId::Refresh, "nav.refresh", "cmd.nav.refresh"},
    Builtin{CommandId::FocusAddressBar, "nav.focus_address_bar", "cmd.nav.focus_address_bar"},
    Builtin{CommandId::Activate, "list.activate", "cmd.list.activate"},
    Builtin{CommandId::SelectAll, "list.select_all", "cmd.list.select_all"},
    Builtin{CommandId::ClearSelection, "list.clear_selection", "cmd.list.clear_selection"},
    Builtin{CommandId::CursorUp, "list.cursor_up", "cmd.list.cursor_up"},
    Builtin{CommandId::CursorDown, "list.cursor_down", "cmd.list.cursor_down"},
    Builtin{CommandId::CursorPageUp, "list.cursor_page_up", "cmd.list.cursor_page_up"},
    Builtin{CommandId::CursorPageDown, "list.cursor_page_down", "cmd.list.cursor_page_down"},
    Builtin{CommandId::CursorHome, "list.cursor_home", "cmd.list.cursor_home"},
    Builtin{CommandId::CursorEnd, "list.cursor_end", "cmd.list.cursor_end"},
    Builtin{CommandId::OpenInNewTab, "list.open_in_new_tab", "cmd.list.open_in_new_tab"},
    Builtin{CommandId::NewTab, "tab.new", "cmd.tab.new"},
    Builtin{CommandId::CloseTab, "tab.close", "cmd.tab.close"},
    Builtin{CommandId::DuplicateTab, "tab.duplicate", "cmd.tab.duplicate"},
    Builtin{CommandId::CloseLeftTabs, "tab.close_left", "cmd.tab.close_left"},
    Builtin{CommandId::ReopenClosedTab, "tab.reopen_closed", "cmd.tab.reopen_closed"},
    Builtin{CommandId::NextTab, "tab.next", "cmd.tab.next"},
    Builtin{CommandId::PreviousTab, "tab.previous", "cmd.tab.previous"},
    Builtin{CommandId::SearchTabs, "tab.search", "cmd.tab.search"},
    Builtin{CommandId::MoveTabLeft, "tab.move_left", "cmd.tab.move_left"},
    Builtin{CommandId::MoveTabRight, "tab.move_right", "cmd.tab.move_right"},
    Builtin{CommandId::SplitRight, "pane.split_right", "cmd.pane.split_right"},
    Builtin{CommandId::SplitDown, "pane.split_down", "cmd.pane.split_down"},
    Builtin{CommandId::FocusNextPane, "pane.focus_next", "cmd.pane.focus_next"},
    Builtin{CommandId::FocusPreviousPane, "pane.focus_previous", "cmd.pane.focus_previous"},
    Builtin{CommandId::ClosePane, "pane.close", "cmd.pane.close"},
    Builtin{CommandId::SaveLayout, "layout.save", "cmd.layout.save"},
    Builtin{CommandId::LoadLayout, "layout.load", "cmd.layout.load"},
    Builtin{CommandId::DeleteLayout, "layout.delete", "cmd.layout.delete"},
    Builtin{CommandId::NewWindow, "window.new", "cmd.window.new"},
};

} // namespace

std::string_view to_string(CommandId id) noexcept {
    switch (id) {
    case CommandId::None: return "none";
    case CommandId::Quit: return "app.quit";
    case CommandId::ToggleFpsOverlay: return "view.toggle_fps_overlay";
    case CommandId::ToggleTheme: return "view.toggle_theme";
    case CommandId::ToggleHidden: return "view.toggle_hidden";
    case CommandId::NavigateBack: return "nav.back";
    case CommandId::NavigateForward: return "nav.forward";
    case CommandId::NavigateUp: return "nav.up";
    case CommandId::Refresh: return "nav.refresh";
    case CommandId::FocusAddressBar: return "nav.focus_address_bar";
    case CommandId::Activate: return "list.activate";
    case CommandId::SelectAll: return "list.select_all";
    case CommandId::ClearSelection: return "list.clear_selection";
    case CommandId::CursorUp: return "list.cursor_up";
    case CommandId::CursorDown: return "list.cursor_down";
    case CommandId::CursorPageUp: return "list.cursor_page_up";
    case CommandId::CursorPageDown: return "list.cursor_page_down";
    case CommandId::CursorHome: return "list.cursor_home";
    case CommandId::CursorEnd: return "list.cursor_end";
    case CommandId::OpenInNewTab: return "list.open_in_new_tab";
    case CommandId::NewTab: return "tab.new";
    case CommandId::CloseTab: return "tab.close";
    case CommandId::DuplicateTab: return "tab.duplicate";
    case CommandId::CloseLeftTabs: return "tab.close_left";
    case CommandId::ReopenClosedTab: return "tab.reopen_closed";
    case CommandId::NextTab: return "tab.next";
    case CommandId::PreviousTab: return "tab.previous";
    case CommandId::SearchTabs: return "tab.search";
    case CommandId::MoveTabLeft: return "tab.move_left";
    case CommandId::MoveTabRight: return "tab.move_right";
    case CommandId::SplitRight: return "pane.split_right";
    case CommandId::SplitDown: return "pane.split_down";
    case CommandId::FocusNextPane: return "pane.focus_next";
    case CommandId::FocusPreviousPane: return "pane.focus_previous";
    case CommandId::ClosePane: return "pane.close";
    case CommandId::SaveLayout: return "layout.save";
    case CommandId::LoadLayout: return "layout.load";
    case CommandId::DeleteLayout: return "layout.delete";
    case CommandId::NewWindow: return "window.new";
    }
    return "unknown";
}

Result<void> register_builtin_commands(CommandRegistry& registry) {
    // Validate first so a failure leaves the registry unchanged (no partial registration).
    for (const Builtin& b : kBuiltins) {
        if (registry.find(b.id) != nullptr || registry.find(b.name) != nullptr) {
            return fail(ErrorCode::AlreadyExists,
                        "built-in command already registered: " + std::string(b.name));
        }
    }
    for (const Builtin& b : kBuiltins) {
        if (auto r = registry.add(CommandInfo{b.id, b.name, b.title_key}); !r) {
            return r;
        }
    }
    return ok();
}

} // namespace navigator

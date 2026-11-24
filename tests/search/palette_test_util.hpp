// tests/search: the builtin-like entry set for the palette ranking tests (plain structs; the
// real ui builds the same shape from the registry, keymap and tr()).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/palette.hpp>

namespace palette_test {

using namespace navigator;
using namespace navigator::palette;

struct Spec {
    const char* name;
    const char* title;
    CommandCategory category;
    Danger danger = Danger::Safe;
    ContextFlags requires_all = ContextFlags::None;
    ContextFlags hidden_if_any = ContextFlags::None;
    const char* shortcut = "";
    const char* description = "";
};

inline std::string category_title(CommandCategory c) {
    switch (c) {
    case CommandCategory::App: return "App";
    case CommandCategory::View: return "View";
    case CommandCategory::Navigation: return "Navigation";
    case CommandCategory::List: return "List";
    case CommandCategory::Tabs: return "Tabs";
    case CommandCategory::Panes: return "Panes";
    case CommandCategory::Layouts: return "Layouts";
    case CommandCategory::Windows: return "Windows";
    case CommandCategory::Sidebar: return "Sidebar";
    case CommandCategory::Edit: return "Edit";
    case CommandCategory::Files: return "Files";
    case CommandCategory::Operations: return "Operations";
    case CommandCategory::Search: return "Search";
    case CommandCategory::Inspector: return "Inspector";
    case CommandCategory::Trash: return "Trash";
    case CommandCategory::Settings: return "Settings";
    case CommandCategory::OpenWith: return "Open With";
    case CommandCategory::Plugins: return "Plugins";
    }
    return "App";
}

inline Entry make(const Spec& s, std::uint32_t id) {
    Entry e;
    e.command = Command{static_cast<CommandId>(id), 0};
    e.name = s.name;
    e.title = s.title;
    e.category = category_title(s.category);
    e.description = s.description;
    e.shortcut = s.shortcut;
    e.category_id = s.category;
    e.danger = s.danger;
    e.requires_all = s.requires_all;
    e.hidden_if_any = s.hidden_if_any;
    return e;
}

// About 60 builtin-like commands. Titles are distinct (the permutation test relies on it).
inline std::vector<Entry> builtin_entries() {
    using C = CommandCategory;
    using F = ContextFlags;
    const Danger d = Danger::Destructive;
    const std::vector<Spec> specs = {
        {"app.quit", "Quit Navigator", C::App, Danger::Safe, F::None, F::None, "Ctrl+Q"},
        {"view.toggle_theme", "Toggle Theme", C::App},
        {"view.toggle_fps_overlay", "Toggle FPS Overlay", C::App},
        {"view.toggle_sidebar", "Toggle Sidebar", C::View, Danger::Safe, F::None, F::None,
         "Ctrl+B"},
        {"view.toggle_hidden", "Show Hidden Files", C::View, Danger::Safe, F::None, F::None,
         "Ctrl+H", "Shows or hides dotfiles in the list"},
        {"view.toggle_view_lock", "Lock View", C::View},
        {"nav.goto", "Go To Folder", C::Navigation, Danger::Safe, F::None, F::None, "Ctrl+L"},
        {"nav.up", "Go Up", C::Navigation, Danger::Safe, F::None, F::None, "Alt+Up"},
        {"nav.back", "Go Back", C::Navigation, Danger::Safe, F::CanGoBack},
        {"nav.forward", "Go Forward", C::Navigation, Danger::Safe, F::CanGoForward},
        {"nav.home", "Go Home", C::Navigation},
        {"list.select_all", "Select All", C::List, Danger::Safe, F::None, F::None, "Ctrl+A"},
        {"list.clear_selection", "Deselect All", C::List, Danger::Safe, F::HasSelection},
        {"list.filter", "Filter Names", C::List, Danger::Safe, F::None, F::None, "Ctrl+F"},
        {"tab.new", "New Tab", C::Tabs, Danger::Safe, F::None, F::None, "Ctrl+T"},
        {"tab.close", "Close Tab", C::Tabs, Danger::Safe, F::None, F::None, "Ctrl+W"},
        {"tab.reopen", "Reopen Closed Tab", C::Tabs, Danger::Safe, F::ClosedTabsAvailable},
        {"pane.split", "Split Pane", C::Panes},
        {"pane.close", "Close Pane", C::Panes, Danger::Safe, F::MultiplePanes},
        {"layout.save", "Save Layout", C::Layouts},
        {"layout.delete", "Delete Layout", C::Layouts, d},
        {"window.new", "New Window", C::Windows},
        {"bookmarks.toggle", "Bookmark This Folder", C::Sidebar},
        {"bookmarks.rename", "Rename Bookmark", C::Sidebar, Danger::Safe, F::BookmarkRowFocused},
        {"edit.undo", "Undo", C::Edit, Danger::Safe, F::CanUndo, F::None, "Ctrl+Z"},
        {"edit.redo", "Redo", C::Edit, Danger::Safe, F::CanRedo, F::None, "Ctrl+Shift+Z"},
        {"edit.copy", "Copy", C::Edit, Danger::Safe, F::HasSelection, F::None, "Ctrl+C"},
        {"edit.cut", "Cut", C::Edit, Danger::Safe, F::HasSelection, F::None, "Ctrl+X"},
        {"edit.paste", "Paste", C::Edit, Danger::Safe, F::None, F::None, "Ctrl+V"},
        {"file.new_folder", "New Folder", C::Files, Danger::Safe, F::None, F::TrashView,
         "Ctrl+Shift+N"},
        {"file.new_file", "New File", C::Files, Danger::Safe, F::None, F::TrashView},
        {"file.rename", "Rename", C::Files, Danger::Safe, F::HasCursorRow, F::None, "F2"},
        {"file.duplicate", "Duplicate", C::Files, Danger::Safe, F::HasSelection},
        {"file.trash", "Move to Trash", C::Files, Danger::Safe, F::HasSelection, F::None, "Delete"},
        {"file.delete_permanently", "Delete Permanently", C::Files, d, F::HasSelection, F::None,
         "Shift+Delete"},
        {"file.copy_path", "Copy Path", C::Files, Danger::Safe, F::HasCursorRow},
        {"file.open_terminal", "Open Terminal Here", C::Files},
        {"ops.cancel", "Cancel Operation", C::Operations, Danger::Safe, F::OperationsRunning},
        {"ops.toggle_pause", "Pause or Resume Operation", C::Operations, Danger::Safe,
         F::OperationsRunning},
        {"search.flatten", "Flatten Folder", C::Search, Danger::Safe, F::None, F::TrashView,
         "Ctrl+E"},
        {"search.index", "Search Everywhere", C::Search},
        {"inspector.toggle", "Toggle Inspector", C::Inspector, Danger::Safe, F::None, F::None,
         "Ctrl+I"},
        {"inspector.zoom_fit", "Zoom to Fit", C::Inspector, Danger::Safe, F::InspectorShowsImage},
        {"trash.open", "Open Trash", C::Trash},
        {"trash.restore", "Restore from Trash", C::Trash, Danger::Safe, F::TrashView},
        {"trash.empty", "Empty Trash", C::Trash, d},
        {"settings.open", "Settings", C::Settings, Danger::Safe, F::None, F::None, "Ctrl+,"},
        {"palette.open", "Command Palette", C::Settings, Danger::Safe, F::None, F::None,
         "Ctrl+Shift+P"},
        {"keymap.edit", "Keyboard Shortcuts", C::Settings, Danger::Safe, F::None, F::None,
         "Ctrl+K Ctrl+S"},
        {"keymap.reset_defaults", "Reset Keyboard Shortcuts to Defaults", C::Settings, d},
        {"open_with.choose", "Open With", C::OpenWith, Danger::Safe, F::HasCursorRow},
    };
    std::vector<Entry> entries;
    std::uint32_t id = 1;
    for (const Spec& s : specs) {
        entries.push_back(make(s, id++));
    }
    return entries;
}

inline Entry* find(std::vector<Entry>& entries, std::string_view name) {
    for (Entry& e : entries) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

inline CommandContext ctx(ContextFlags flags = ContextFlags::None) {
    CommandContext c;
    c.flags = flags;
    return c;
}

// A context where everything applies except the trash-only and hidden-in-trash rules.
inline CommandContext rich() {
    using F = ContextFlags;
    return ctx(F::HasSelection | F::HasCursorRow | F::CursorOnFolder | F::MultiplePanes |
               F::CanUndo | F::CanRedo | F::ClosedTabsAvailable | F::BookmarkRowFocused |
               F::OperationsRunning | F::InspectorShowsImage | F::CanGoBack | F::CanGoForward);
}

inline std::string title_at(const std::vector<Entry>& entries, const SearchResult& r,
                            std::size_t i) {
    return entries.at(r.hits.at(i).entry).title;
}

} // namespace palette_test

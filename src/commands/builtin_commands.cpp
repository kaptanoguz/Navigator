// Built-in command table, register_builtin_commands(), to_string(CommandId), command metadata
// (docs/architecture.md "Phase 8: command metadata"), category names and the built-in variants.
//
// Adding a command: append a CommandId in navigator/commands.hpp (never renumber), add a case to
// to_string() and a row to kBuiltins (with its metadata), add its default binding to
// docs/keymap.md, and add the "cmd.<name>" title to the English and Turkish catalogs.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/commands.hpp>

namespace navigator {
namespace {

// A table row: the metadata of one built-in command. The i18n keys derive from the name; their
// are views into builtin_storage().
struct Builtin {
    CommandId id = CommandId::None;
    std::string_view name;
    CommandCategory category = CommandCategory::App;
    ArgKind arg_kind = ArgKind::None;
    Danger danger = Danger::Safe;
    bool palette_visible = true;
    bool bindable = true;
    ContextFlags requires_all = ContextFlags::None;
    ContextFlags hidden_if_any = ContextFlags::None;

    [[nodiscard]] constexpr Builtin arg(ArgKind kind) const {
        Builtin b = *this;
        b.arg_kind = kind;
        return b;
    }
    [[nodiscard]] constexpr Builtin destructive() const {
        Builtin b = *this;
        b.danger = Danger::Destructive;
        return b;
    }
    [[nodiscard]] constexpr Builtin not_in_palette() const {
        Builtin b = *this;
        b.palette_visible = false;
        return b;
    }
    // Registered so names and ids are stable, but a no-op for now: neither offered nor bindable.
    [[nodiscard]] constexpr Builtin reserved() const {
        Builtin b = *this;
        b.palette_visible = false;
        b.bindable = false;
        return b;
    }
    [[nodiscard]] constexpr Builtin needs(ContextFlags flags) const {
        Builtin b = *this;
        b.requires_all = flags;
        return b;
    }
    [[nodiscard]] constexpr Builtin hidden_when(ContextFlags flags) const {
        Builtin b = *this;
        b.hidden_if_any = flags;
        return b;
    }
};

constexpr Builtin entry(CommandId id, std::string_view name, CommandCategory category) {
    Builtin b;
    b.id = id;
    b.name = name;
    b.category = category;
    return b;
}

// Order is registration order: ascending CommandId (tested).
constexpr std::array kBuiltins{
    entry(CommandId::Quit, "app.quit", CommandCategory::App),
    entry(CommandId::ToggleFpsOverlay, "view.toggle_fps_overlay", CommandCategory::App),
    entry(CommandId::ToggleTheme, "view.toggle_theme", CommandCategory::App),
    entry(CommandId::ToggleHidden, "view.toggle_hidden", CommandCategory::View),
    entry(CommandId::ToggleSidebar, "view.toggle_sidebar", CommandCategory::View),
    entry(CommandId::ToggleViewLock, "view.toggle_view_lock", CommandCategory::View).reserved(),
    entry(CommandId::NavigateBack, "nav.back", CommandCategory::Navigation)
        .needs(ContextFlags::CanGoBack),
    entry(CommandId::NavigateForward, "nav.forward", CommandCategory::Navigation)
        .needs(ContextFlags::CanGoForward),
    entry(CommandId::NavigateUp, "nav.up", CommandCategory::Navigation),
    entry(CommandId::Refresh, "nav.refresh", CommandCategory::Navigation),
    entry(CommandId::FocusAddressBar, "nav.focus_address_bar", CommandCategory::Navigation),
    entry(CommandId::GoTo, "nav.goto", CommandCategory::Navigation),
    entry(CommandId::Activate, "list.activate", CommandCategory::List)
        .needs(ContextFlags::HasCursorRow),
    entry(CommandId::SelectAll, "list.select_all", CommandCategory::List),
    entry(CommandId::ClearSelection, "list.clear_selection", CommandCategory::List),
    entry(CommandId::ToggleCursorSelected, "list.toggle_cursor_selected", CommandCategory::List)
        .needs(ContextFlags::HasCursorRow),
    entry(CommandId::CursorUp, "list.cursor_up", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::CursorDown, "list.cursor_down", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::CursorPageUp, "list.cursor_page_up", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::CursorPageDown, "list.cursor_page_down", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::CursorHome, "list.cursor_home", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::CursorEnd, "list.cursor_end", CommandCategory::List)
        .arg(ArgKind::Flags)
        .not_in_palette(),
    entry(CommandId::OpenInNewTab, "list.open_in_new_tab", CommandCategory::List)
        .needs(ContextFlags::HasCursorRow),
    entry(CommandId::NewTab, "tab.new", CommandCategory::Tabs).arg(ArgKind::Flags),
    entry(CommandId::CloseTab, "tab.close", CommandCategory::Tabs).arg(ArgKind::TabId),
    entry(CommandId::DuplicateTab, "tab.duplicate", CommandCategory::Tabs).arg(ArgKind::TabId),
    entry(CommandId::CloseLeftTabs, "tab.close_left", CommandCategory::Tabs).arg(ArgKind::TabId),
    entry(CommandId::ReopenClosedTab, "tab.reopen_closed", CommandCategory::Tabs)
        .needs(ContextFlags::ClosedTabsAvailable),
    entry(CommandId::NextTab, "tab.next", CommandCategory::Tabs),
    entry(CommandId::PreviousTab, "tab.previous", CommandCategory::Tabs),
    entry(CommandId::SearchTabs, "tab.search", CommandCategory::Tabs),
    entry(CommandId::MoveTabLeft, "tab.move_left", CommandCategory::Tabs).arg(ArgKind::TabId),
    entry(CommandId::MoveTabRight, "tab.move_right", CommandCategory::Tabs).arg(ArgKind::TabId),
    entry(CommandId::SplitRight, "pane.split_right", CommandCategory::Panes).arg(ArgKind::PaneId),
    entry(CommandId::SplitDown, "pane.split_down", CommandCategory::Panes).arg(ArgKind::PaneId),
    entry(CommandId::FocusNextPane, "pane.focus_next", CommandCategory::Panes),
    entry(CommandId::FocusPreviousPane, "pane.focus_previous", CommandCategory::Panes),
    entry(CommandId::ClosePane, "pane.close", CommandCategory::Panes)
        .arg(ArgKind::PaneId)
        .needs(ContextFlags::MultiplePanes),
    entry(CommandId::SaveLayout, "layout.save", CommandCategory::Layouts),
    entry(CommandId::LoadLayout, "layout.load", CommandCategory::Layouts),
    entry(CommandId::DeleteLayout, "layout.delete", CommandCategory::Layouts).destructive(),
    entry(CommandId::NewWindow, "window.new", CommandCategory::Windows).reserved(),
    entry(CommandId::FocusSidebar, "sidebar.focus", CommandCategory::Sidebar),
    entry(CommandId::ToggleBookmark, "bookmarks.toggle", CommandCategory::Sidebar)
        .arg(ArgKind::BookmarkId),
    entry(CommandId::RenameBookmark, "bookmarks.rename", CommandCategory::Sidebar)
        .arg(ArgKind::BookmarkRow),
    entry(CommandId::MoveBookmarkUp, "bookmarks.move_up", CommandCategory::Sidebar)
        .arg(ArgKind::BookmarkRow),
    entry(CommandId::MoveBookmarkDown, "bookmarks.move_down", CommandCategory::Sidebar)
        .arg(ArgKind::BookmarkRow),
    entry(CommandId::ClearRecents, "recents.clear", CommandCategory::Sidebar),
    entry(CommandId::ToggleRecentsTracking, "recents.toggle_tracking", CommandCategory::Sidebar),
    entry(CommandId::CopyPath, "edit.copy_path", CommandCategory::Edit).arg(ArgKind::Flags),
    entry(CommandId::Copy, "edit.copy", CommandCategory::Edit)
        .needs(ContextFlags::HasSelection)
        .hidden_when(ContextFlags::SidebarFocused),
    entry(CommandId::Cut, "edit.cut", CommandCategory::Edit)
        .needs(ContextFlags::HasSelection)
        .hidden_when(ContextFlags::SidebarFocused),
    entry(CommandId::Paste, "edit.paste", CommandCategory::Edit)
        .arg(ArgKind::Flags)
        .hidden_when(ContextFlags::SidebarFocused | ContextFlags::TrashView),
    entry(CommandId::Undo, "edit.undo", CommandCategory::Edit).needs(ContextFlags::CanUndo),
    entry(CommandId::Redo, "edit.redo", CommandCategory::Edit).needs(ContextFlags::CanRedo),
    entry(CommandId::FocusFilter, "list.filter", CommandCategory::Search),
    entry(CommandId::ClearFilter, "list.clear_filter", CommandCategory::Search)
        .needs(ContextFlags::FilterActive),
    entry(CommandId::ToggleFlatten, "search.flatten", CommandCategory::Search),
    entry(CommandId::FlattenVolume, "search.flatten_volume", CommandCategory::Search),
    entry(CommandId::ToggleTypesPopup, "search.types", CommandCategory::Search),
    entry(CommandId::ToggleFuzzy, "search.toggle_fuzzy", CommandCategory::Search),
    entry(CommandId::CancelSearch, "search.cancel", CommandCategory::Search)
        .needs(ContextFlags::SearchRunning),
    entry(CommandId::RebuildIndex, "search.rebuild_index", CommandCategory::Search),
    entry(CommandId::Inspect, "view.inspect", CommandCategory::Inspector),
    entry(CommandId::ToggleInspectorLock, "inspector.toggle_lock", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorVisible),
    entry(CommandId::InspectorZoomFit, "inspector.zoom_fit", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorShowsImage),
    entry(CommandId::InspectorZoom1to1, "inspector.zoom_1to1", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorShowsImage),
    entry(CommandId::InspectorPlay, "inspector.play", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorVisible),
    entry(CommandId::CycleInspectorPlacement, "inspector.cycle_placement",
          CommandCategory::Inspector)
        .needs(ContextFlags::InspectorVisible),
    entry(CommandId::ToggleInspectorGrid, "inspector.toggle_grid", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorShowsImage),
    entry(CommandId::ToggleInspectorMinimap, "inspector.toggle_minimap", CommandCategory::Inspector)
        .needs(ContextFlags::InspectorShowsImage),
    entry(CommandId::MoveToTrash, "file.trash", CommandCategory::Files)
        .needs(ContextFlags::HasSelection)
        .hidden_when(ContextFlags::SidebarFocused),
    entry(CommandId::DeletePermanently, "file.delete_permanently", CommandCategory::Files)
        .destructive()
        .needs(ContextFlags::HasSelection)
        .hidden_when(ContextFlags::SidebarFocused),
    entry(CommandId::Rename, "file.rename", CommandCategory::Files)
        .needs(ContextFlags::HasCursorRow)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::NewFolder, "file.new_folder", CommandCategory::Files)
        .hidden_when(ContextFlags::TrashView | ContextFlags::SidebarFocused),
    entry(CommandId::CopyToOtherPane, "file.copy_to_other_pane", CommandCategory::Files)
        .needs(ContextFlags::HasSelection | ContextFlags::MultiplePanes)
        .hidden_when(ContextFlags::SidebarFocused | ContextFlags::TrashView),
    entry(CommandId::MoveToOtherPane, "file.move_to_other_pane", CommandCategory::Files)
        .needs(ContextFlags::HasSelection | ContextFlags::MultiplePanes)
        .hidden_when(ContextFlags::SidebarFocused | ContextFlags::TrashView),
    entry(CommandId::OpenTerminalHere, "file.open_terminal_here", CommandCategory::Files)
        .reserved(),
    entry(CommandId::CancelOperation, "ops.cancel", CommandCategory::Operations)
        .arg(ArgKind::OpId)
        .needs(ContextFlags::OperationsRunning),
    entry(CommandId::TogglePauseOperation, "ops.toggle_pause", CommandCategory::Operations)
        .arg(ArgKind::OpId)
        .needs(ContextFlags::OperationsRunning),
    entry(CommandId::ToggleOperationsPanel, "ops.toggle_panel", CommandCategory::Operations),
    entry(CommandId::OpenTrash, "trash.open", CommandCategory::Trash)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::RestoreFromTrash, "trash.restore", CommandCategory::Trash)
        .needs(ContextFlags::TrashView | ContextFlags::HasSelection),
    entry(CommandId::EmptyTrash, "trash.empty", CommandCategory::Trash)
        .destructive()
        .needs(ContextFlags::TrashView),
    entry(CommandId::OpenPalette, "palette.open", CommandCategory::Settings).not_in_palette(),
    entry(CommandId::OpenSettings, "settings.open", CommandCategory::Settings),
    entry(CommandId::EditKeymap, "keymap.edit", CommandCategory::Settings),
    entry(CommandId::ResetKeymap, "keymap.reset_defaults", CommandCategory::Settings).destructive(),
    entry(CommandId::ExportKeymap, "keymap.export", CommandCategory::Settings),
    entry(CommandId::ShowContextMenu, "menu.show_context", CommandCategory::Settings)
        .not_in_palette(),
    entry(CommandId::OpenWith, "open_with.show", CommandCategory::OpenWith)
        .needs(ContextFlags::HasCursorRow)
        .hidden_when(ContextFlags::TrashView),
    // Phase 9A: view modes, zoom, grouping, view lock and tree.
    entry(CommandId::ViewModeDetails, "view.mode_details", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewModeIcons, "view.mode_icons", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewModeList, "view.mode_list", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewModeTree, "view.mode_tree", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewCycleMode, "view.cycle_mode", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewZoomIn, "view.zoom_in", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewZoomOut, "view.zoom_out", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewZoomReset, "view.zoom_reset", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::GroupByNone, "view.group_none", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::GroupByDate, "view.group_date", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::GroupByType, "view.group_type", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::GroupBySize, "view.group_size", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::GroupCycle, "view.group_cycle", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewLockFolder, "view.lock_folder", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewLockHierarchy, "view.lock_hierarchy", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::ViewLockGlobal, "view.lock_global", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::TreeExpand, "view.tree_expand", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::TreeCollapse, "view.tree_collapse", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
    entry(CommandId::TreeCollapseAll, "view.tree_collapse_all", CommandCategory::View)
        .hidden_when(ContextFlags::TrashView),
};

} // namespace

std::string_view to_string(CommandCategory category) noexcept {
    switch (category) {
    case CommandCategory::App: return "app";
    case CommandCategory::View: return "view";
    case CommandCategory::Navigation: return "navigation";
    case CommandCategory::List: return "list";
    case CommandCategory::Tabs: return "tabs";
    case CommandCategory::Panes: return "panes";
    case CommandCategory::Layouts: return "layouts";
    case CommandCategory::Windows: return "windows";
    case CommandCategory::Sidebar: return "sidebar";
    case CommandCategory::Edit: return "edit";
    case CommandCategory::Files: return "files";
    case CommandCategory::Operations: return "operations";
    case CommandCategory::Search: return "search";
    case CommandCategory::Inspector: return "inspector";
    case CommandCategory::Trash: return "trash";
    case CommandCategory::Settings: return "settings";
    case CommandCategory::OpenWith: return "open_with";
    case CommandCategory::Plugins: return "plugins";
    }
    return "app";
}

std::string_view category_title_key(CommandCategory category) noexcept {
    switch (category) {
    case CommandCategory::App: return "category.app";
    case CommandCategory::View: return "category.view";
    case CommandCategory::Navigation: return "category.navigation";
    case CommandCategory::List: return "category.list";
    case CommandCategory::Tabs: return "category.tabs";
    case CommandCategory::Panes: return "category.panes";
    case CommandCategory::Layouts: return "category.layouts";
    case CommandCategory::Windows: return "category.windows";
    case CommandCategory::Sidebar: return "category.sidebar";
    case CommandCategory::Edit: return "category.edit";
    case CommandCategory::Files: return "category.files";
    case CommandCategory::Operations: return "category.operations";
    case CommandCategory::Search: return "category.search";
    case CommandCategory::Inspector: return "category.inspector";
    case CommandCategory::Trash: return "category.trash";
    case CommandCategory::Settings: return "category.settings";
    case CommandCategory::OpenWith: return "category.open_with";
    case CommandCategory::Plugins: return "category.plugins";
    }
    return "category.app";
}

Span<const CommandVariant> builtin_command_variants() noexcept {
    static constexpr std::array kVariants{
        CommandVariant{CommandId::CopyPath, CopyPathArg::CurrentFolder, "cmd.edit.copy_path.folder",
                       ContextFlags::None},
        CommandVariant{CommandId::NewTab, NewTabArg::Home, "cmd.tab.new.home", ContextFlags::None},
        CommandVariant{CommandId::Paste, PasteArg::IntoCursorFolder, "cmd.edit.paste.into_folder",
                       ContextFlags::CursorOnFolder},
    };
    return kVariants;
}

std::string_view to_string(CommandId id) noexcept {
    switch (id) {
    case CommandId::None: return "none";
    case CommandId::Quit: return "app.quit";
    case CommandId::ToggleFpsOverlay: return "view.toggle_fps_overlay";
    case CommandId::ToggleTheme: return "view.toggle_theme";
    case CommandId::ToggleHidden: return "view.toggle_hidden";
    case CommandId::ToggleSidebar: return "view.toggle_sidebar";
    case CommandId::ToggleViewLock: return "view.toggle_view_lock";
    case CommandId::NavigateBack: return "nav.back";
    case CommandId::NavigateForward: return "nav.forward";
    case CommandId::NavigateUp: return "nav.up";
    case CommandId::Refresh: return "nav.refresh";
    case CommandId::FocusAddressBar: return "nav.focus_address_bar";
    case CommandId::GoTo: return "nav.goto";
    case CommandId::Activate: return "list.activate";
    case CommandId::SelectAll: return "list.select_all";
    case CommandId::ClearSelection: return "list.clear_selection";
    case CommandId::ToggleCursorSelected: return "list.toggle_cursor_selected";
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
    case CommandId::FocusSidebar: return "sidebar.focus";
    case CommandId::ToggleBookmark: return "bookmarks.toggle";
    case CommandId::RenameBookmark: return "bookmarks.rename";
    case CommandId::MoveBookmarkUp: return "bookmarks.move_up";
    case CommandId::MoveBookmarkDown: return "bookmarks.move_down";
    case CommandId::ClearRecents: return "recents.clear";
    case CommandId::ToggleRecentsTracking: return "recents.toggle_tracking";
    case CommandId::CopyPath: return "edit.copy_path";
    case CommandId::Copy: return "edit.copy";
    case CommandId::Cut: return "edit.cut";
    case CommandId::Paste: return "edit.paste";
    case CommandId::Undo: return "edit.undo";
    case CommandId::Redo: return "edit.redo";
    case CommandId::FocusFilter: return "list.filter";
    case CommandId::ClearFilter: return "list.clear_filter";
    case CommandId::ToggleFlatten: return "search.flatten";
    case CommandId::FlattenVolume: return "search.flatten_volume";
    case CommandId::ToggleTypesPopup: return "search.types";
    case CommandId::ToggleFuzzy: return "search.toggle_fuzzy";
    case CommandId::CancelSearch: return "search.cancel";
    case CommandId::RebuildIndex: return "search.rebuild_index";
    case CommandId::Inspect: return "view.inspect";
    case CommandId::ToggleInspectorLock: return "inspector.toggle_lock";
    case CommandId::InspectorZoomFit: return "inspector.zoom_fit";
    case CommandId::InspectorZoom1to1: return "inspector.zoom_1to1";
    case CommandId::InspectorPlay: return "inspector.play";
    case CommandId::CycleInspectorPlacement: return "inspector.cycle_placement";
    case CommandId::ToggleInspectorGrid: return "inspector.toggle_grid";
    case CommandId::ToggleInspectorMinimap: return "inspector.toggle_minimap";
    case CommandId::MoveToTrash: return "file.trash";
    case CommandId::DeletePermanently: return "file.delete_permanently";
    case CommandId::Rename: return "file.rename";
    case CommandId::NewFolder: return "file.new_folder";
    case CommandId::CopyToOtherPane: return "file.copy_to_other_pane";
    case CommandId::MoveToOtherPane: return "file.move_to_other_pane";
    case CommandId::OpenTerminalHere: return "file.open_terminal_here";
    case CommandId::CancelOperation: return "ops.cancel";
    case CommandId::TogglePauseOperation: return "ops.toggle_pause";
    case CommandId::ToggleOperationsPanel: return "ops.toggle_panel";
    case CommandId::OpenTrash: return "trash.open";
    case CommandId::RestoreFromTrash: return "trash.restore";
    case CommandId::EmptyTrash: return "trash.empty";
    case CommandId::OpenPalette: return "palette.open";
    case CommandId::OpenSettings: return "settings.open";
    case CommandId::EditKeymap: return "keymap.edit";
    case CommandId::ResetKeymap: return "keymap.reset_defaults";
    case CommandId::ExportKeymap: return "keymap.export";
    case CommandId::ShowContextMenu: return "menu.show_context";
    case CommandId::OpenWith: return "open_with.show";
    case CommandId::ViewModeDetails: return "view.mode_details";
    case CommandId::ViewModeIcons: return "view.mode_icons";
    case CommandId::ViewModeList: return "view.mode_list";
    case CommandId::ViewModeTree: return "view.mode_tree";
    case CommandId::ViewCycleMode: return "view.cycle_mode";
    case CommandId::ViewZoomIn: return "view.zoom_in";
    case CommandId::ViewZoomOut: return "view.zoom_out";
    case CommandId::ViewZoomReset: return "view.zoom_reset";
    case CommandId::GroupByNone: return "view.group_none";
    case CommandId::GroupByDate: return "view.group_date";
    case CommandId::GroupByType: return "view.group_type";
    case CommandId::GroupBySize: return "view.group_size";
    case CommandId::GroupCycle: return "view.group_cycle";
    case CommandId::ViewLockFolder: return "view.lock_folder";
    case CommandId::ViewLockHierarchy: return "view.lock_hierarchy";
    case CommandId::ViewLockGlobal: return "view.lock_global";
    case CommandId::TreeExpand: return "view.tree_expand";
    case CommandId::TreeCollapse: return "view.tree_collapse";
    case CommandId::TreeCollapseAll: return "view.tree_collapse_all";
    }
    return "unknown";
}

namespace {

// The CommandInfo rows with their i18n keys. Built once; the keys are std::strings owned by this
// static, and the views in the CommandInfo rows point into it (never reallocated after build).
struct BuiltinStorage {
    std::vector<std::string> title_keys;
    std::vector<std::string> description_keys;
    std::vector<CommandInfo> infos;
};

const BuiltinStorage& builtin_storage() {
    static const BuiltinStorage storage = [] {
        BuiltinStorage s;
        s.title_keys.reserve(kBuiltins.size());
        s.description_keys.reserve(kBuiltins.size());
        s.infos.reserve(kBuiltins.size());
        for (const Builtin& b : kBuiltins) {
            s.title_keys.push_back("cmd." + std::string(b.name));
            s.description_keys.push_back("cmd." + std::string(b.name) + ".desc");
        }
        for (std::size_t i = 0; i < kBuiltins.size(); ++i) {
            const Builtin& b = kBuiltins[i];
            CommandInfo info;
            info.id = b.id;
            info.name = b.name;
            info.title_key = s.title_keys[i];
            info.category = b.category;
            info.description_key = s.description_keys[i];
            info.danger = b.danger;
            info.arg_kind = b.arg_kind;
            info.palette_visible = b.palette_visible;
            info.bindable = b.bindable;
            info.requires_all = b.requires_all;
            info.hidden_if_any = b.hidden_if_any;
            s.infos.push_back(info);
        }
        return s;
    }();
    return storage;
}

} // namespace

Result<void> register_builtin_commands(CommandRegistry& registry) {
    const BuiltinStorage& storage = builtin_storage();
    // Validate first so a failure leaves the registry unchanged (no partial registration).
    for (const CommandInfo& info : storage.infos) {
        if (registry.find(info.id) != nullptr || registry.find(info.name) != nullptr) {
            return fail(ErrorCode::AlreadyExists,
                        "built-in command already registered: " + std::string(info.name));
        }
    }
    for (const CommandInfo& info : storage.infos) {
        if (auto r = registry.add(info); !r) {
            return r;
        }
    }
    return ok();
}

} // namespace navigator

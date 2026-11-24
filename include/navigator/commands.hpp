// navigator/commands.hpp
//
// Purpose: command identifiers, the per-frame command queue, the command registry that the
//          keymap and command palette are built on, and the keymap itself (key chords ->
//          command names). No SDL types: the ui layer translates SDL events into Key/Modifiers.
// Owner:   contracts (header); implementation in src/commands (role: commands).
// Thread-safety: NOT thread-safe. CommandQueue, CommandRegistry and Keymap are owned and used by
//                the UI thread only. Worker threads report back through their own channels and
//                the UI thread turns results into commands. The constexpr helpers (Modifiers
//                operators, KeyChord comparison) and to_string()/parse_key_chord() are pure and
//                may be called from any thread.
// Errors:  navigator::Result (types.hpp). No exceptions escape these functions.
//
// Frame model: widgets never mutate program state while drawing. A user action calls
// CommandQueue::enqueue(); at frame end the app calls drain() and runs every pending command in
// FIFO order. Commands enqueued while a drain is running (by a handler) are NOT run by that drain;
// they stay pending and run on the next drain (normally the next frame).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

// Stable numeric ids: never renumber or reuse a value; append new ids. Values may be persisted
// (keymap files, plugins). Ranges:
//      1- 999  app / view
//   1000-1099  navigation (history, location)
//   1100-1199  list: activation, cursor, selection
//   1200-1219  tabs            (Phase 3)
//   1220-1239  panes           (Phase 3)
//   1240-1249  named layouts   (Phase 3)
//   1250-1259  windows         (reserved: multi-window phase)
//   1260-1279  sidebar, bookmarks, recents (Phase 4)
//   1280-1299  edit / clipboard (Phase 4: copy path; Phase 7: copy/cut/paste, undo/redo)
//   1300-1319  filter, flatten, name search, index (Phase 5)
//   1320-1339  Inspector and previews (Phase 6)
//   1340-1359  file operations and the operations panel (Phase 7)
//   1360-1369  trash (Phase 7)
//   1370-1389  command palette, settings, keymap editor, context menu (Phase 8)
//   1390-1399  Open With (Phase 8)
//   1400-1429  view modes, zoom, grouping, tree and view lock (Phase 9A)
//   1430+      reserved for later phases
// Every id except None is registered by register_builtin_commands() and appears in the palette.
// `arg` meanings are listed per id; "arg: 0" means the command ignores it.
enum class CommandId : std::uint32_t {
    None = 0,             // "none"       no-op; never registered
    Quit = 1,             // "app.quit"
    ToggleFpsOverlay = 2, // "view.toggle_fps_overlay"
    ToggleTheme = 3,      // "view.toggle_theme"
    ToggleHidden = 4,     // "view.toggle_hidden"    show/hide dot-files; re-lists. arg: 0
    ToggleSidebar = 5,    // "view.toggle_sidebar"   Phase 4. Show/hide the window's sidebar
                          //   (layout::SidebarState::visible; persisted in the session). Hiding
                          //   it while it has keyboard focus returns focus to the active pane.
                          //   arg: 0
    ToggleViewLock = 6,   // "view.toggle_view_lock" Phase 9A: cycles the active folder's view
                          //   settings scope (Global -> Folder -> Hierarchy -> Global;
                          //   view_settings.hpp). arg: 0

    // Navigation. arg: 0 for all.
    NavigateBack = 1000,    // "nav.back"     previous history location (no-op at the start)
    NavigateForward = 1001, // "nav.forward"  next history location (no-op at the end)
    NavigateUp = 1002,      // "nav.up"       parent directory (no-op at "/")
    Refresh = 1003,         // "nav.refresh"  re-list the current directory
    FocusAddressBar = 1004, // "nav.focus_address_bar"  Phase 4: turns the active pane's
                            //   breadcrumb bar into a text field holding the folder's path
                            //   (all selected) and gives it keyboard focus. Enter: go (folder ->
                            //   navigate the tab; file -> navigate to its folder with the cursor
                            //   on it); Ctrl+Enter: as list.open_in_new_tab on the target; Tab:
                            //   accept the highlighted completion; Escape or focus loss: back to
                            //   breadcrumbs, nothing changes. Resolution: path_input.hpp.
    GoTo = 1005,            // "nav.goto"  Phase 4: opens the GoTo popup over the active pane:
                            //   a text field (path_input expansion, case correction, folder-name
                            //   completion) above a list of recents, bookmarks and places
                            //   filtered by the text. Enter / Ctrl+Enter as in the address bar.

    // List. Cursor commands take the CursorArg bit flags below as `arg`.
    Activate = 1100,       // "list.activate"  open the cursor entry: folder (or link to a folder)
                           //                  -> enter it; otherwise platform::open_with_default.
                           //                  arg: 0
    SelectAll = 1101,      // "list.select_all"       arg: 0
    ClearSelection = 1102, // "list.clear_selection"  arg: 0 (cursor stays)
    CursorUp = 1110,       // "list.cursor_up"        one row up
    CursorDown = 1111,     // "list.cursor_down"      one row down
    CursorPageUp = 1112,   // "list.cursor_page_up"   one visible page up
    CursorPageDown = 1113, // "list.cursor_page_down" one visible page down
    CursorHome = 1114,     // "list.cursor_home"      first row
    CursorEnd = 1115,      // "list.cursor_end"       last row
    OpenInNewTab = 1116,   // "list.open_in_new_tab"  Phase 3. Cursor entry is a folder (or link
                           //   to one): open it in a new background tab right after the active
                           //   tab (same pane, inherits sort/show_hidden; the active tab, cursor
                           //   and selection do not change). Otherwise: same as list.activate.
                           //   arg: 0

    // Phase 8 (Ctrl+Space, docs/features.md "toggles cursor selection").
    // "list.toggle_cursor_selected" toggles the cursor row in the selection exactly like
    // Ctrl+click: the anchor moves to the cursor row, the cursor stays. No-op without a cursor
    // row. arg: 0
    ToggleCursorSelected = 1103,

    // Tabs (Phase 3). Unless stated otherwise, arg selects the target tab: 0 = the active tab
    // of the active pane, otherwise layout::TabId::value of a specific tab (tab bar buttons and
    // the tab context menu pass it). An arg naming no existing tab makes the command a no-op.
    NewTab = 1200,          // "tab.new"  new tab right after the active tab of the active pane,
                            //   on that tab's folder with its sort/show_hidden, cursor on the
                            //   first row, scrolled to the top; it becomes active.
                            //   arg: NewTabArg flags (not a TabId)
    CloseTab = 1201,        // "tab.close"  pushes it onto the closed-tabs list. The last tab of
                            //   the last pane is never closed (no-op; Ctrl+Q quits)
    DuplicateTab = 1202,    // "tab.duplicate"  full copy (folder, sort, cursor, scroll,
                            //   selection and back/forward history) right after the tab; active
    CloseLeftTabs = 1203,   // "tab.close_left"  closes the tabs left of the tab in its pane
    ReopenClosedTab = 1204, // "tab.reopen_closed"  newest closed tab back to its pane and index
                            //   if that pane still exists, else after the active tab. arg: 0
    NextTab = 1205,         // "tab.next"      next tab of the active pane, wrapping. arg: 0
    PreviousTab = 1206,     // "tab.previous"  previous tab of the active pane, wrapping. arg: 0
    SearchTabs = 1207,      // "tab.search"  opens the Search Tabs list: open tabs of all panes
                            //   (pane order, then tab order) then recently closed tabs (newest
                            //   first), filtered by typed text; picking one activates or
                            //   reopens it. arg: 0
    MoveTabLeft = 1208,     // "tab.move_left"   swap with the left neighbour (no wrap)
    MoveTabRight = 1209,    // "tab.move_right"  swap with the right neighbour (no wrap)

    // Panes (Phase 3). arg: 0 = the active pane, otherwise layout::PaneId::value.
    SplitRight = 1220,        // "pane.split_right"  new pane right of the pane, showing a new
                              //   tab on the pane's active folder (like tab.new); it is active
    SplitDown = 1221,         // "pane.split_down"   same, below the pane
    FocusNextPane = 1222,     // "pane.focus_next"      next pane in reading order, wrapping.
                              //   Phase 4: while the sidebar is visible it is one more focus
                              //   stop before the first pane: sidebar -> panes -> sidebar
    FocusPreviousPane = 1223, // "pane.focus_previous"  previous pane, wrapping (same cycle)
    ClosePane = 1224,         // "pane.close"  closes the pane and all its tabs (all pushed onto
                              //   the closed-tabs list); no-op on the only pane

    // Named layouts (Phase 3). The name prompt / picker these open are ui popups; the typed name
    // or the picked row reaches the ui as a ui-internal intent (it carries text), not as a
    // command. arg: 0 for all.
    SaveLayout = 1240,   // "layout.save"    inline name prompt, then save the current layout
                         //   (asks before replacing an existing name)
    LoadLayout = 1241,   // "layout.load"    picker of saved layouts; the picked one replaces
                         //   the current layout (current tabs go to the closed-tabs list)
    DeleteLayout = 1242, // "layout.delete"  same picker; deletes the picked layout after a
                         //   confirmation

    // Windows (reserved). Registered so names and ids are stable; unbound no-ops until a
    // second window exists (multi-window phase).
    NewWindow = 1250, // "window.new"  duplicate the active tab into a new window (the source
                      //   tab stays); docs/features.md lists Ctrl+N for this. arg: 0

    // Sidebar, bookmarks, recents (Phase 4). Bookmark commands take arg 0 = the bookmark row
    // that has keyboard focus in the sidebar (no-op when none), except where stated, or
    // otherwise places::BookmarkId::value (sidebar context menu). An arg naming no bookmark is
    // a no-op.
    FocusSidebar = 1260,     // "sidebar.focus"  keyboard focus to the sidebar (shows it first
                             //   if hidden), on the row of the active pane's place or the first
                             //   row. Escape returns focus to the active pane. arg: 0
    ToggleBookmark = 1261,   // "bookmarks.toggle"  arg 0: bookmark the active tab's folder, or
                             //   remove its bookmark if it has one (bookmark button in the
                             //   address bar). arg = BookmarkId: remove that bookmark.
    RenameBookmark = 1262,   // "bookmarks.rename"  inline rename field on the row; the typed
                             //   name reaches the ui as an intent (it carries text); empty
                             //   restores the default label
    MoveBookmarkUp = 1263,   // "bookmarks.move_up"    one row up (no wrap)
    MoveBookmarkDown = 1264, // "bookmarks.move_down"  one row down (no wrap)
    ClearRecents = 1270,     // "recents.clear"  empties the Recents list (no confirmation; it
                             //   only forgets history). arg: 0
    ToggleRecentsTracking = 1271, // "recents.toggle_tracking"  flips
                                  //   places::RecentsOptions::enabled (persisted). arg: 0

    // Edit / clipboard.
    CopyPath = 1280, // "edit.copy_path"  Phase 4 ("Copy as Path", docs/features.md): puts the
                     //   absolute paths of the selected entries (list order) on the clipboard,
                     //   or the current folder when nothing is selected, formatted by
                     //   path_input::format_paths_for_copy(PathQuoting::Auto).
                     //   arg: CopyPathArg flags
    // Phase 7. File commands act on the active tab of the active pane while the list (or the
    // Inspector) has focus; with the sidebar or a popup focused they are not routed to the
    // pane (docs/keymap.md "Focus scoping (Phase 7)"). "Selection" means the selected rows in
    // list order; with no selection the commands are no-ops (a notice says why). arg: 0
    // unless stated.
    Copy = 1281,  // "edit.copy"  puts the selection on the clipboard as a copy
                  //   (transfer::make_clipboard_offer, ClipboardAction::Copy)
    Cut = 1282,   // "edit.cut"   same with ClipboardAction::Cut; the rows are drawn dimmed until
                  //   the paste or until another program takes the clipboard
    Paste = 1283, // "edit.paste" reads the clipboard (transfer::read_clipboard); copy -> OpKind
                  //   Copy, cut -> OpKind::Move into the tab's folder, ConflictPolicy::Ask.
                  //   After a cut is pasted the clipboard is cleared if Navigator still owns
                  //   it. Clipboard text that is not a file list does nothing (notice).
                  //   arg: PasteArg flags
    Undo = 1284,  // "edit.undo"  ops::UndoStack::take_undo -> OpKind::Undo. No-op (notice)
                  //   when empty or when a volume of the record is not mounted (named)
    Redo = 1285,  // "edit.redo"  ops::UndoStack::take_redo -> OpKind::Redo

    // Filter, flatten and index (Phase 5). docs/architecture.md "Phase 5" has the data flow and
    // docs/keymap.md "Focus scoping (Phase 5)" the keys of the filter field. All act on the
    // active tab of the active pane. arg: 0 for all.
    FocusFilter = 1300,      // "list.filter"  shows the tab's filter field in the pane title bar
                             //   (if collapsed) and gives it keyboard focus with its text
                             //   selected. Typing a printable character while the list has focus
                             //   does the same and inserts that character (except a leading
                             //   Space; see docs/keymap.md). The filter applies on every edit and
                             //   stays applied after the field loses focus.
    ClearFilter = 1301,      // "list.clear_filter"  empties the filter text (the type filter is
                             //   separate and stays); the chip's clear button. Focus unchanged.
    ToggleFlatten = 1302,    // "search.flatten"  enters the flattened view of the tab's folder
                             //   (recursive, streamed, cancellable; filter and type filter apply
                             //   to it), or leaves it when it is active (cancelling a running
                             //   walk). Ctrl+E (docs/features.md).
    FlattenVolume = 1303,    // "search.flatten_volume"  flattened view rooted at the mount point
                             //   of the volume holding the tab's folder ("whole drive"); served
                             //   by the name index when it is ready. When the tab is already
                             //   flattened from that root, leaves the flattened view.
    ToggleTypesPopup = 1304, // "search.types"  opens (or closes) the types popup of the tab:
                             //   one row per type present (folders, no extension, each
                             //   extension) with counts and visibility toggles, and "toggle all"
                             //   (search.hpp TypeFilter). Popup routing as in Phase 3/4.
    ToggleFuzzy = 1305,      // "search.toggle_fuzzy"  switches the tab's match mode between
                             //   Substring and Fuzzy for the current view (folder filter
                             //   default Substring, flattened view default Fuzzy; runtime only).
    CancelSearch = 1306,     // "search.cancel"  stops a running flatten walk or index query of
                             //   the tab, keeping the rows found so far (progress bar button).
                             //   Escape reaches the same effect through list.clear_selection
                             //   (cascade in docs/keymap.md).
    RebuildIndex = 1307,     // "search.rebuild_index"  RescanKind::Rebuild for the index of the
                             //   volume holding the tab's folder; no-op when it is not indexed.

    // Inspector (Phase 6). One Inspector per window, docked right or bottom of the pane tree
    // (layout::InspectorState, persisted in the session). Unless locked it shows the cursor
    // entry of the active tab of the active pane (a multi-selection: count and total size) and
    // follows the cursor; it never takes keyboard focus unless clicked (docs/keymap.md "Phase 6
    // defaults"). The image commands act on the Inspector's current image and are no-ops for
    // other content. docs/architecture.md "Phase 6" has the data flow. arg: 0 for all.
    Inspect = 1320,                 // "view.inspect"  shows the Inspector on the cursor entry,
                                    //   or hides it when visible (Space, Alt+KeypadEnter;
                                    //   docs/features.md). Hiding it while it has keyboard focus
                                    //   returns focus to the active pane.
    ToggleInspectorLock = 1321,     // "inspector.toggle_lock"  lock: keep showing the current
                                    //   item while the cursor moves (and across folder changes)
                                    //   until unlocked or the item is deleted; runtime only.
    InspectorZoomFit = 1322,        // "inspector.zoom_fit"   image fits the view (default; never
                                    //   upscaled past 1:1 for small images)
    InspectorZoom1to1 = 1323,       // "inspector.zoom_1to1"  one image pixel per DEVICE pixel
                                    //   (docs/review/phase-5.md); pan by drag / arrows when the
                                    //   Inspector has focus
    InspectorPlay = 1324,           // "inspector.play"  opens the inspected item with the default
                                    //   application (platform::open_with_default), exactly like
                                    //   list.activate on a file. The video "Play" button (a
                                    //   Navigator extra; no in-app playback). Folders: no-op.
    CycleInspectorPlacement = 1325, // "inspector.cycle_placement"  Auto -> Right -> Bottom ->
                                    //   Auto (layout::InspectorPlacement; persisted)
    ToggleInspectorGrid = 1326,     // "inspector.toggle_grid"  pixel grid over the image at
                                    //   zoom >= 4:1 device pixels (persisted)
    ToggleInspectorMinimap = 1327,  // "inspector.toggle_minimap"  overview with the visible
                                    //   rectangle while the image is larger than the view
                                    //   (persisted)

    // File operations (Phase 7). docs/architecture.md "Phase 7" has the data flow and the
    // safety rules; ops.hpp the engine contract.
    MoveToTrash = 1340,           // "file.trash"  OpKind::Trash of the selection, no confirmation;
                                  //   a notice with "Undo" follows. Items on a volume without a
                                  //   usable trash are NOT deleted: the permanent-delete sheet
                                  //   opens for exactly those (ops.hpp S2). In the Trash view it
                                  //   acts as file.delete_permanently
    DeletePermanently = 1341,     // "file.delete_permanently"  confirmation sheet first (count,
                                  //   "cannot be undone"; initial focus on Cancel), then
                                  //   OpKind::DeletePermanently with confirmed = true (S3)
    Rename = 1342,                // "file.rename"  inline editor on the cursor row with the stem
                                  //   selected (ops::stem_length); Enter -> OpKind::Rename, Escape
                                  //   cancels. Multi-selection: the cursor row only (batch rename
                                  //   is Phase 9)
    NewFolder = 1343,             // "file.new_folder"  inline editor on a new placeholder row
                                  //   holding the translated default name made unique with
                                  //   ops::keep_both_name; Enter -> OpKind::MakeDirectory with the
                                  //   typed name; Escape creates nothing
    CopyToOtherPane = 1344,       // "file.copy_to_other_pane"  OpKind::Copy of the selection into
                                  //   the active tab folder of the previously active pane
                                  //   ("previous pane", docs/features.md); no-op with one pane
    MoveToOtherPane = 1345,       // "file.move_to_other_pane"  same with OpKind::Move
    OpenTerminalHere = 1346,      // "file.open_terminal_here"  Phase 10: opens the user's
                                  //   terminal emulator in the active tab's folder (F4 / context
                                  //   menu / palette) via platform::open_terminal
    CancelOperation = 1347,       // "ops.cancel"  arg: ops::OpId::value; 0 = the most recently
                                  //   started unfinished operation (progress bar stop button)
    TogglePauseOperation = 1348,  // "ops.toggle_pause"  arg as ops.cancel
    ToggleOperationsPanel = 1349, // "ops.toggle_panel"  non-modal list of running and recent
                                  //   operations (status bar button)

    // Trash (Phase 7). The Trash view is a runtime view mode of a tab (like the flattened view;
    // not persisted: a session saves the folder the tab showed before). arg: 0 for all.
    OpenTrash = 1360,        // "trash.open"  shows the Trash view (all trash directories,
                             //   trash::ITrash::list) in the active tab; the sidebar Trash row
                             //   (places::PlaceKind::Trash) enqueues it
    RestoreFromTrash = 1361, // "trash.restore"  Trash view: OpKind::Restore of the selected items
                             //   to their original folders (conflicts: Ask)
    EmptyTrash = 1362,       // "trash.empty"  confirmation sheet (count, "cannot be undone"),
                             //   then OpKind::DeletePermanently of every listed item

    // Command palette, settings, keymap editor, context menu (Phase 8). docs/architecture.md
    // "Phase 8" has the data flow and the safety rules; palette.hpp, menu.hpp, keymap_file.hpp
    // the contracts. arg: 0 for all.
    OpenPalette = 1370,  // "palette.open"  Ctrl+Shift+P. Opens (or closes) the global command
                         //   palette overlay: a text field over a ranked list of every palette-
                         //   visible command that applies to the context captured at the moment
                         //   of opening (CommandContext). Works from every focus except inside
                         //   the palette itself (where it closes it); not while a confirmation or
                         //   conflict sheet is open. The palette never runs anything itself: the
                         //   picked entry enqueues the same Command a key binding would.
    OpenSettings = 1371, // "settings.open"  Ctrl+, (docs/features.md). Phase 8: opens the Settings
                         //   overlay whose only page is "Keyboard" (the keymap editor); Phase 9
                         //   adds the other pages. Closes it when open.
    EditKeymap = 1372,   // "keymap.edit"  opens the Settings overlay directly on the Keyboard
                         //   page (the keymap editor): every bindable command and variant with
                         //   its shortcuts, search, "press new shortcut" capture, conflict and
                         //   warning display (keymap_file.hpp check_binding), reset per command,
                         //   aliases. Default sequence Ctrl+K Ctrl+S.
    ResetKeymap = 1373,  // "keymap.reset_defaults"  DESTRUCTIVE (Danger::Destructive). Confirmation
                         //   sheet first (initial focus on Cancel; names the number of custom
                         //   shortcuts), then KeymapFile bindings and unbound lists are cleared
                         //   (reset_all) and keymap.json saved. Aliases and the sequence timeout
                         //   stay.
    ExportKeymap = 1374, // "keymap.export"  puts to_json(KeymapFile) (the user's overrides, the
                         //   file as saved) on the clipboard as text and shows a notice
    ShowContextMenu = 1375, // "menu.show_context"  Menu key and Shift+F10. Opens the context menu
                            //   of the focused element at its anchor: list cursor row (or the
                            //   folder background when the list is empty), sidebar row, tab (when
                            //   the tab bar has keyboard focus; Phase 6/7 handled Shift+F10 there
                            //   with a hardcoded key), Trash view row or background. Same menu as
                            //   the right click. Does nothing while a text field has focus.

    // Open With (Phase 8). openwith.hpp has the handler model.
    OpenWith = 1390, // "open_with.show"  opens the Open With popup for the selection (the cursor
                     //   row when nothing is selected): applications registered for the MIME type
                     //   (default first), the common ones when the selection mixes types, filtered
                     //   by typed text; Enter launches the highlighted one with all selected
                     //   paths (platform openwith::launch_with). Unbound by default; also the
                     //   "Open With" submenu of the row context menu. The handler picked is a
                     //   ui-internal intent (it carries a desktop id), not a command arg.

    // View modes, zoom, grouping, tree and view lock (Phase 9A; view_settings.hpp). arg: 0.
    ViewModeDetails = 1400, // "view.mode_details"   switch active tab to Details view (Ctrl+1)
    ViewModeIcons = 1401,   // "view.mode_icons"     switch active tab to Icon Grid view (Ctrl+2)
    ViewModeList = 1402,    // "view.mode_list"      switch active tab to List/Columns view (Ctrl+3)
    ViewModeTree = 1403,    // "view.mode_tree"      switch active tab to Tree view (Ctrl+4)
    ViewCycleMode = 1404,   // "view.cycle_mode"     cycle Details -> Icons -> List -> Tree
    ViewZoomIn = 1405,      // "view.zoom_in"        step zoom in (Details -> Icons -> larger)
    ViewZoomOut = 1406,     // "view.zoom_out"       step zoom out (smaller -> Details)
    ViewZoomReset = 1407,   // "view.zoom_reset"     reset zoom to 100%
    GroupByNone = 1410,     // "view.group_none"     disable grouping
    GroupByDate = 1411,     // "view.group_date"     group items by modification date
    GroupByType = 1412,     // "view.group_type"     group items by file type category
    GroupBySize = 1413,     // "view.group_size"     group items by file size bucket
    GroupCycle = 1414,      // "view.group_cycle"    cycle None -> Date -> Type -> Size
    ViewLockFolder = 1415,  // "view.lock_folder"    lock view settings to this folder only
    ViewLockHierarchy = 1416, // "view.lock_hierarchy" lock view settings to this folder + subtree
    ViewLockGlobal = 1417,    // "view.lock_global"    use global view settings (clear folder lock)
    TreeExpand = 1420,        // "view.tree_expand"    expand cursor folder in Tree view
    TreeCollapse = 1421,      // "view.tree_collapse"  collapse cursor folder (or move to parent)
    TreeCollapseAll = 1422,   // "view.tree_collapse_all" collapse all expanded folders in Tree view

    // Batch rename, appearance, themes, typography, animations (Phase 9B). arg: 0.
    BatchRename = 1425,      // "file.batch_rename"           interactive batch rename (Shift+F2)
    ToggleAnimations = 1430, // "view.toggle_animations"      toggle UI animations live
    ToggleAlternatingRows = 1431,  // "view.toggle_alternating_rows" toggle zebra striping
    CycleDensity = 1432,           // "view.cycle_density"          Compact <-> Comfortable
    FontSizeIncrease = 1433,       // "view.font_size_increase"     step UI font scale up
    FontSizeDecrease = 1434,       // "view.font_size_decrease"     step UI font scale down
    FontSizeReset = 1435,          // "view.font_size_reset"        reset UI font scale to 100%
    OpenAppearanceSettings = 1436, // "settings.appearance"        open Settings on Appearance page

    // Phase 10: Miller columns, content search, archives, plugins, GVfs unmount. arg: 0.
    ViewModeColumns = 1440,    // "view.mode_columns"     switch active tab to Miller Columns (Ctrl+5)
    ContentSearch = 1441,      // "search.content"        open/focus content search overlay (Ctrl+Shift+G)
    ArchiveExtractHere = 1442, // "archive.extract_here"  extract selected archive(s) into current dir
    ReloadPlugins = 1443,      // "plugins.reload"        reload C ABI plugins from plugin_dir
    UnmountVolume = 1444,      // "volume.unmount"        unmount selected GVfs mount in sidebar
};

// Bit flags for the `arg` of CopyPath. Unknown bits are ignored (reserved).
//   0:             selection, else the current folder (Ctrl+Shift+C).
//   CurrentFolder: always the current folder (breadcrumb / tab context menu).
struct CopyPathArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t CurrentFolder = std::uint64_t{1} << 0U;
};

// Bit flags for the `arg` of Paste. Unknown bits are ignored (reserved).
//   0:                into the active tab's folder (Ctrl+V).
//   IntoCursorFolder: into the folder of the cursor row when it is a folder (or a link to one),
//                     else as 0 (row context menu "Paste into folder").
struct PasteArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t IntoCursorFolder = std::uint64_t{1} << 0U;
};

// Bit flags for the `arg` of NewTab. Unknown bits are ignored (reserved).
//   0:    the new tab opens the active tab's folder (default; Ctrl+T, "+" button).
//   Home: the new tab opens platform::user_dir(Home) with default view options (middle-click
//         on the "+" button).
struct NewTabArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t Home = std::uint64_t{1} << 0U;
};

// Bit flags for the `arg` of CursorUp..CursorEnd. Unknown bits are ignored (reserved).
//   0 (no flags): move the cursor; the selection becomes exactly the new cursor row and the
//                 selection anchor moves to it.
//   Extend:       select the contiguous range anchor..new cursor (Shift+arrow); the anchor stays.
//   KeepSelection (Phase 8): move the cursor only; the selection and the anchor are not touched
//                 (Ctrl+Up / Ctrl+Down, File Pilot-style cursor selection; Ctrl+Space then
//                 toggles the row, list.toggle_cursor_selected). If Extend is also set, Extend
//                 wins.
struct CursorArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t Extend = std::uint64_t{1} << 0U;
    static constexpr std::uint64_t KeepSelection = std::uint64_t{1} << 1U;
};

// A queued command. `arg` is a command-specific payload; 0 when unused. Commands that need more
// data than one integer will carry an index/handle into UI-owned state (decided per command).
struct Command {
    CommandId id = CommandId::None;
    std::uint64_t arg = 0;

    friend constexpr bool operator==(const Command&, const Command&) = default;
};

class CommandQueue {
public:
    CommandQueue() = default;

    // Appends to the pending list. Enqueuing CommandId::None is allowed and is a no-op when run.
    void enqueue(Command command);
    void enqueue(CommandId id, std::uint64_t arg = 0) { enqueue(Command{id, arg}); }

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    // Moves all pending commands out (FIFO order) and leaves the queue empty.
    [[nodiscard]] std::vector<Command> take();

    // Calls handler(const Command&) for every command pending at the moment of the call, in FIFO
    // order. Commands the handler enqueues are kept for the next drain.
    template <class F> void drain(F&& handler) {
        const std::vector<Command> batch = take();
        for (const Command& command : batch) {
            handler(command);
        }
    }

private:
    std::vector<Command> pending_;
};

// ---------------------------------------------------------------------------------------------
// Command metadata (Phase 8): what the palette, the context menu and the keymap editor need
// ---------------------------------------------------------------------------------------------

// Grouping in the palette (category title shown next to the title; "category browse" order when
// the query is empty) and in the keymap editor. Order is the browse order. i18n key of the title:
// category_title_key(). Stable names (to_string) are for tests and logs.
enum class CommandCategory : std::uint8_t {
    App,        // "app"        app.quit, view.toggle_theme, view.toggle_fps_overlay
    View,       // "view"       view.toggle_hidden, view.toggle_sidebar, view.toggle_view_lock
    Navigation, // "navigation" nav.*
    List,       // "list"       list.* (activation, selection, cursor)
    Tabs,       // "tabs"       tab.*
    Panes,      // "panes"      pane.*
    Layouts,    // "layouts"    layout.*
    Windows,    // "windows"    window.*
    Sidebar,    // "sidebar"    sidebar.*, bookmarks.*, recents.*
    Edit,       // "edit"       edit.*
    Files,      // "files"      file.*
    Operations, // "operations" ops.*
    Search,     // "search"     search.*, list.filter, list.clear_filter
    Inspector,  // "inspector"  view.inspect, inspector.*
    Trash,      // "trash"      trash.*
    Settings,   // "settings"   palette.*, settings.*, keymap.*, menu.*
    OpenWith,   // "open_with"  open_with.*
    Plugins,    // "plugins"    reserved for the plugin ABI (no built-in command)
};

// "app", "view", "navigation", ... (the names in the enum comments). "app" for out-of-range.
[[nodiscard]] std::string_view to_string(CommandCategory category) noexcept;
// i18n key of the category title: "category." + to_string(category), e.g. "category.navigation".
[[nodiscard]] std::string_view category_title_key(CommandCategory category) noexcept;

// What a command's `arg` means (the non-zero values). It tells the palette whether a second
// step (a ui-internal picker; the picked value is the arg) is needed. The picker itself is a ui
// concern and carries no contract beyond this table:
//   None         arg is ignored (or reserved); the palette runs the command with arg 0.
//   Flags        arg is a bit set (CursorArg, CopyPathArg, PasteArg, NewTabArg). The palette runs
//                arg 0; named non-zero variants are offered as separate palette entries
//                (builtin_command_variants()).
//   OpId         ops::OpId::value (ops.cancel, ops.toggle_pause). 0 = the most recently started
//                unfinished operation. The palette runs arg 0 when at most one operation is
//                unfinished (CommandContext::running_operations <= 1), otherwise it first shows a
//                picker of the unfinished operations (label + progress) and passes the picked id.
//   TabId        layout::TabId::value; 0 = the active tab of the active pane. The palette never
//                asks (arg 0).
//   PaneId       layout::PaneId::value; 0 = the active pane. The palette never asks.
//   BookmarkId   places::BookmarkId::value; 0 has a meaning that does not need a row
//                (bookmarks.toggle: the active tab's folder). The palette never asks.
//   BookmarkRow  places::BookmarkId::value; 0 = the bookmark row that had keyboard focus in the
//                sidebar BEFORE the palette opened (bookmarks.rename / move_up / move_down). The
//                palette runs arg 0 when the captured context has ContextFlags::BookmarkRowFocused,
//                otherwise it first shows a picker of the bookmarks.
// Aliases and key bindings of a command whose kind is OpId, TabId, PaneId, BookmarkId or
// BookmarkRow can only carry arg 0 (the ids are runtime values; keymap_file.hpp drops entries
// with a non-zero arg for them). No arg value of ANY command skips a confirmation: the
// confirm-sheet commands (file.delete_permanently, trash.empty, layout.delete,
// keymap.reset_defaults) ignore `arg` entirely.
enum class ArgKind : std::uint8_t { None, Flags, OpId, TabId, PaneId, BookmarkId, BookmarkRow };

// Safe: the command does not destroy user data or settings without a way back, or it asks first
// on its own and is undoable (file.trash is Safe: the trash is undoable and has no sheet).
// Destructive: the command can lose data or settings for good, and ALWAYS routes through a
// confirmation sheet owned by its handler (initial focus on Cancel). Because palette entries,
// context-menu items, aliases and key sequences only enqueue the Command, every path reaches
// the same sheet; none can pass "confirmed". The destructive set (static, built-ins):
//   file.delete_permanently, trash.empty, layout.delete, keymap.reset_defaults
// plus, dynamically, file.trash in the Trash view where it acts as file.delete_permanently
// (effective_danger). Destructive commands: never the first palette result or default menu
// highlight while a safe one exists, never recorded in the palette history, never pinnable, and a
// binding to a bare key (no modifiers) draws a keymap-editor warning (keymap_file.hpp
// is_destructive_bare_binding).
enum class Danger : std::uint8_t { Safe, Destructive };

// Facts about the moment the palette or a menu opens, filled by the ui from its models (cheap,
// no I/O). Pure data. The palette captures it BEFORE taking keyboard focus, so "the focused
// sidebar row" and "the active pane's selection" describe what the user was looking at.
enum class ContextFlags : std::uint32_t {
    None = 0,
    HasSelection = 1U << 0U,         // at least one selected row in the active tab
    HasCursorRow = 1U << 1U,         // the active tab has a cursor row
    CursorOnFolder = 1U << 2U,       // the cursor row is a folder or a link to one
    TrashView = 1U << 3U,            // the active tab shows the Trash view
    FlattenedView = 1U << 4U,        // the active tab shows a flattened view
    SidebarFocused = 1U << 5U,       // keyboard focus was in the sidebar
    BookmarkRowFocused = 1U << 6U,   // ... on a bookmark row (implies SidebarFocused)
    MultiplePanes = 1U << 7U,        // the window has at least two panes
    OperationsRunning = 1U << 8U,    // at least one unfinished file operation
    CanUndo = 1U << 9U,              // ops::UndoStack has an undo record
    CanRedo = 1U << 10U,             // ... a redo record
    ClosedTabsAvailable = 1U << 11U, // the closed-tabs list is not empty
    FilterActive = 1U << 12U,        // the active tab has a non-empty name filter
    InspectorVisible = 1U << 13U,    // the Inspector is shown
    InspectorShowsImage = 1U << 14U, // ... and its current content is an image
    SearchRunning = 1U << 15U,       // a flatten walk or index query of the active tab runs
    CanGoBack = 1U << 16U,           // history has a previous location
    CanGoForward = 1U << 17U,        // ... a next location
};

inline constexpr std::uint32_t kContextFlagsMask = 0x3FFFFU; // all defined bits

[[nodiscard]] constexpr ContextFlags operator|(ContextFlags a, ContextFlags b) noexcept {
    return static_cast<ContextFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr ContextFlags operator&(ContextFlags a, ContextFlags b) noexcept {
    return static_cast<ContextFlags>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr ContextFlags& operator|=(ContextFlags& a, ContextFlags b) noexcept { return a = a | b; }
constexpr ContextFlags& operator&=(ContextFlags& a, ContextFlags b) noexcept { return a = a & b; }

// True when every bit of `flags` is set in `set` (has(x, ContextFlags::None) is true).
[[nodiscard]] constexpr bool has(ContextFlags set, ContextFlags flags) noexcept {
    return (set & flags) == flags;
}
// True when at least one bit of `flags` is set in `set` (false for ContextFlags::None).
[[nodiscard]] constexpr bool has_any(ContextFlags set, ContextFlags flags) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flags)) != 0U;
}

struct CommandContext {
    ContextFlags flags = ContextFlags::None;
    // Unfinished file operations (queued, running or paused): decides the OpId picker.
    std::uint32_t running_operations = 0;
};

// Static description of a command.
//   name:      stable dotted id, e.g. "app.quit" (keymap files, aliases and the palette use it)
//   title_key: i18n key for the palette title, e.g. "cmd.app.quit" (see i18n.hpp)
// Both views must point to storage that outlives the registry (string literals in practice).
//
// Phase 8 metadata (every field has a default so Phase 0-7 aggregate initialisers
// `CommandInfo{id, name, title_key}` still compile; register_builtin_commands() fills all of them
// from the table in docs/architecture.md "Phase 8: command metadata"; plugin commands, later,
// register their own):
//   category:         palette / editor grouping.
//   description_key:  i18n key of a one-line description, "cmd.<name>.desc" for built-ins. The
//                     catalogs need not contain it: the ui hides a description whose tr() result
//                     equals the key. Empty: no description.
//   danger:           see Danger.
//   arg_kind:         see ArgKind.
//   palette_visible:  false: never offered in the palette (still bindable when `bindable`):
//                     list.cursor_up..list.cursor_end (they flood the list) and the reserved
//                     no-ops (view.toggle_view_lock, window.new, file.open_terminal_here).
//   bindable:         false: the keymap editor does not list it and keymap files may not bind it
//                     (reserved no-ops only). Everything else is bindable: "nearly all actions
//                     bindable" (docs/features.md); the sheets' own keys are not commands.
//   requires_all:     the command is applicable only when every one of these context flags is set.
//   hidden_if_any:    ... and not when any of these is set (e.g. file.* with the sidebar focused:
//                     the ui does not route them there, so the palette hides them instead of
//                     showing a no-op).
struct CommandInfo {
    CommandId id = CommandId::None;
    std::string_view name;
    std::string_view title_key;
    CommandCategory category = CommandCategory::App;
    std::string_view description_key = {};
    Danger danger = Danger::Safe;
    ArgKind arg_kind = ArgKind::None;
    bool palette_visible = true;
    bool bindable = true;
    ContextFlags requires_all = ContextFlags::None;
    ContextFlags hidden_if_any = ContextFlags::None;
};

// Whether the command applies in `context` (pure, allocation-free): requires_all are all set and
// none of hidden_if_any is. The palette and menus show only applicable commands; a command that is
// not applicable is not greyed out but absent (docs/review/phase-7.md). Running a non-applicable
// command (a key binding, an alias, a stale menu) stays safe: handlers keep their own no-op rules.
[[nodiscard]] constexpr bool is_applicable(const CommandInfo& info,
                                           const CommandContext& context) noexcept {
    return has(context.flags, info.requires_all) && !has_any(context.flags, info.hidden_if_any);
}

// Danger in `context`: file.trash (MoveToTrash) is Destructive in the Trash view, where it acts
// as file.delete_permanently; every other command is info.danger.
[[nodiscard]] constexpr Danger effective_danger(const CommandInfo& info,
                                                const CommandContext& context) noexcept {
    if (info.id == CommandId::MoveToTrash && has(context.flags, ContextFlags::TrashView)) {
        return Danger::Destructive;
    }
    return info.danger;
}

// A named non-zero arg of a built-in command, offered as its own palette entry and its own row in
// the keymap editor ("Copy Folder Path", "New Tab at Home"). The default variant (arg 0) is the
// command itself. title_key: "cmd.<name>.<variant>" (listed below). Static storage.
//   id / arg                        title_key                       requires_all
//   CopyPath / CopyPathArg::CurrentFolder  cmd.edit.copy_path.folder   (none)
//   NewTab   / NewTabArg::Home             cmd.tab.new.home            (none)
//   Paste    / PasteArg::IntoCursorFolder  cmd.edit.paste.into_folder  CursorOnFolder
struct CommandVariant {
    CommandId id = CommandId::None;
    std::uint64_t arg = 0;
    std::string_view title_key = {};
    ContextFlags requires_all = ContextFlags::None;
};

// The three variants above, in that order. Valid for the program's lifetime.
[[nodiscard]] Span<const CommandVariant> builtin_command_variants() noexcept;

class CommandRegistry {
public:
    CommandRegistry() = default;

    // Errors: InvalidArgument (id None or empty name), AlreadyExists (duplicate id or name).
    Result<void> add(const CommandInfo& info);

    // nullptr when not registered. Pointers stay valid until the next add().
    [[nodiscard]] const CommandInfo* find(CommandId id) const noexcept;
    [[nodiscard]] const CommandInfo* find(std::string_view name) const noexcept;

    // All commands in registration order. Valid until the next add().
    [[nodiscard]] Span<const CommandInfo> all() const noexcept;

private:
    std::vector<CommandInfo> commands_;
};

// Registers every built-in CommandId (except None) with its name, title_key ("cmd." + name) and
// the Phase 8 metadata (docs/architecture.md "Phase 8: command metadata"). Registration order:
// ascending CommandId. A failure (AlreadyExists) leaves the registry unchanged.
Result<void> register_builtin_commands(CommandRegistry& registry);

// Stable name of a built-in id ("app.quit", ...); "none" for None, "unknown" for other values.
[[nodiscard]] std::string_view to_string(CommandId id) noexcept;

// =============================================================================================
// Keymap: key chords -> command names
// =============================================================================================
//
// Translation from the windowing system (done in src/ui, never here):
//   - Keys match by keycode, i.e. the legend the active layout puts on the key, so Ctrl+Q is
//     the key labelled Q on Turkish Q, Turkish F and US layouts alike. ASCII letters are folded
//     to lowercase first (with Shift held SDL may report the uppercase legend).
//   - If the keycode is not a Latin letter (Turkish ı ş ğ ü ö ç, Cyrillic, Greek, ...), the ui
//     falls back to the physical key position (scancode) so that Ctrl+<that key> still resolves
//     to the US-position letter. A Latin legend that has no Key value does NOT fall back (it
//     must not steal the binding of the letter that sits at its position on a US layout).
//   - Left/right modifiers are merged. CapsLock, NumLock and AltGr (ISO_Level3_Shift,
//     SDL_KMOD_MODE) are never reported as modifiers; AltGr is not Alt.
//   - Keypad keys (Phase 8): the ui maps SDL's keypad keycodes to Key::Keypad0..Keypad9,
//     KeypadPlus, KeypadMinus, KeypadMultiply, KeypadDivide, KeypadPeriod and KeypadEnter by the
//     identity of the PHYSICAL keypad key, independent of NumLock (NumLock is not a modifier, see
//     below): Keypad1 is reported for the key labelled 1 on the keypad whether or not NumLock is
//     on. The ui never also reports the navigation key such a key produces without NumLock
//     (End), and a keypad key never falls back to the digit row. X servers may rewrite
//     Shift+Keypad digits into navigation keys; such chords are accepted by the keymap but may
//     not fire on X11. The Menu key (context-menu key) maps to Key::Menu.
//   - Mouse side buttons (X1/X2) map to Key::MouseBack / Key::MouseForward so they bind through
//     the same keymap. Clicks, double-clicks and drags are not chords (see docs/architecture.md).
//
// Bindings are global: the Keymap has no focus contexts. Focus scoping (Phase 4) is done by the
// ui BEFORE and AFTER resolution, never by a second keymap (docs/keymap.md "Focus scoping"):
//   - A focused text field (address bar, GoTo, rename and name prompts) first consumes SDL text
//     input and its editing keys (Left/Right/Home/End with or without Shift, Backspace, Delete,
//     Ctrl+A/C/X/V/Z, Ctrl+Left/Right, Ctrl+Backspace) without consulting the keymap; Enter,
//     Ctrl+Enter, KeypadEnter, Tab, Up, Down, PageUp, PageDown and Escape have field meanings.
//     Every other chord resolves through the keymap as usual (Ctrl+Q, Ctrl+B, F6 still work).
//   - When the sidebar or a popup has focus, the ui routes the resolved list.cursor_*,
//     list.activate, list.open_in_new_tab and list.clear_selection commands to it instead of the
//     active pane (Phase 3 popups already do this).
//   - Phase 5 folder filter field: a text field that consumes FEWER keys, because most commands
//     must keep working while it has focus (docs/features.md). It takes text input and Left,
//     Right, Home, End (with Shift/Ctrl), Backspace (also when empty: never nav.up), Delete,
//     Ctrl+Backspace, Ctrl+V, Ctrl+Z, and Ctrl+C / Ctrl+X only while it has a text selection.
//     Escape is a field key (cancel a walk, then clear the text, then return focus to the list);
//     Tab returns focus to the list. Everything else resolves through the keymap and goes to
//     the active pane as usual: Up/Down/PageUp/PageDown (with Shift) move the list cursor,
//     Enter / Ctrl+Enter activate the cursor row, Ctrl+A selects all shown rows, Ctrl+E
//     flattens. Details: docs/keymap.md "Focus scoping (Phase 5)".
//   - Phase 6: every focused text field (Phase 4 fields and the Phase 5 filter field) also
//     consumes Space and Shift+Space as text, so typing "a b" never fires view.inspect. With
//     the list focused, Space resolves through the keymap (view.inspect); type-to-filter never
//     starts with a Space. When the Inspector has focus (it was clicked), the ui routes the
//     resolved list.cursor_* commands to it (scroll text, pan an image at 1:1),
//     list.activate to inspector.play, and list.clear_selection (Escape) returns focus to the
//     active pane; view.inspect (Space) hides it.
//   - Phase 7 (file operations; safety): every focused text field (all of the above,
//     including the inline rename / new-folder editors) also consumes Shift+Delete (as
//     Delete), Ctrl+Shift+Z and Ctrl+Y (text redo, or nothing), so no file command can fire
//     while typing; the Phase 4 fields already consume Delete, Ctrl+C/X/V/Z, and the Phase 5
//     filter field Delete, Ctrl+V, Ctrl+Z and Ctrl+C/X while it has a text selection. With the
//     sidebar or a popup focused, the resolved edit.copy/cut/paste, file.* and trash.restore
//     commands are not routed to the pane (no invisible action on a selection the user is
//     not looking at); file.rename on a bookmark row becomes bookmarks.rename. With the
//     Inspector focused they act on the active pane as usual. While a confirmation or
//     conflict sheet is open it takes the keyboard like a Phase 3 popup.
// Phase 8 (docs/keymap.md "Phase 8 defaults"):
//   - Multi-chord sequences ("Ctrl+K Ctrl+S"): KeySequence, Keymap::bind(KeySequence, ...) and
//     the SequenceMatcher state machine below. The single-chord API stays and means "a sequence
//     of length 1". The ui feeds the matcher with the chords that reach the keymap, i.e. AFTER
//     the focus scoping above (a chord a focused text field consumes is never fed, so typing
//     never starts or advances a sequence), exactly once per key press (no auto-repeat, no
//     modifier-only presses, no key releases). It calls SequenceMatcher::reset() when focus
//     moves, a popup, sheet or overlay opens, or the mouse is clicked, so a half-typed sequence
//     never survives a context change.
//   - Palette and menus run commands through the same queue, so every sheet still applies.

// Numeric values are not persisted anywhere (keymap files use to_string() names); they may be
// regrouped between versions. Append within a group when possible.
// clang-format off
enum class Key : std::uint16_t {
    Unknown = 0,

    // Letters (US-position legend), 1..26
    A = 1, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Top-row digits, 40..49
    Digit0 = 40, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,

    // Function keys, 60..83
    F1 = 60, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,

    // Navigation and editing, 100..115
    Up = 100, Down, Left, Right, PageUp, PageDown, Home, End,
    Enter, KeypadEnter, Escape, Backspace, Delete, Tab, Space, Insert,
    Menu = 116,                       // the context-menu key (Phase 8)

    // Punctuation by US legend, 140..150 (for later bindings such as Ctrl+, and Ctrl+=)
    Minus = 140, Equals, Comma, Period, Slash, Backslash, Semicolon, Apostrophe, LeftBracket,
    RightBracket, Grave,

    // Keypad keys by physical key (Phase 8; KeypadEnter is in the group above), 160..169
    // digits, 170..174 operators
    Keypad0 = 160, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8,
    Keypad9,
    KeypadPlus = 170, KeypadMinus, KeypadMultiply, KeypadDivide, KeypadPeriod,

    // Mouse side buttons (X1 / X2), 200..201
    MouseBack = 200, MouseForward,
};
// clang-format on

// Bit set of held modifiers.
enum class Modifiers : std::uint8_t {
    None = 0,
    Ctrl = 1U << 0U,
    Shift = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
};

inline constexpr std::uint8_t kModifierMask = 0x0FU; // all defined Modifiers bits

[[nodiscard]] constexpr Modifiers operator|(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator&(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator^(Modifiers a, Modifiers b) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(a) ^ static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr Modifiers operator~(Modifiers a) noexcept {
    return static_cast<Modifiers>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) &
                                  kModifierMask);
}
constexpr Modifiers& operator|=(Modifiers& a, Modifiers b) noexcept { return a = a | b; }
constexpr Modifiers& operator&=(Modifiers& a, Modifiers b) noexcept { return a = a & b; }
constexpr Modifiers& operator^=(Modifiers& a, Modifiers b) noexcept { return a = a ^ b; }

// True when every bit of `flags` is set in `set` (has(x, Modifiers::None) is true).
[[nodiscard]] constexpr bool has(Modifiers set, Modifiers flags) noexcept {
    return (set & flags) == flags;
}

// One key press with the exact set of modifiers held. Matching is exact: Ctrl+A does not fire
// for Ctrl+Shift+A.
struct KeyChord {
    Key key = Key::Unknown;
    Modifiers mods = Modifiers::None;

    friend constexpr bool operator==(const KeyChord&, const KeyChord&) = default;
};

// Canonical ASCII name of a key, identical in every UI language: "A".."Z", "0".."9", "F1".."F24",
// "Up", "Down", "Left", "Right", "PageUp", "PageDown", "Home", "End", "Enter", "KeypadEnter",
// "Escape", "Backspace", "Delete", "Tab", "Space", "Insert", "Menu", "Minus", "Equals", "Comma",
// "Period", "Slash", "Backslash", "Semicolon", "Apostrophe", "LeftBracket", "RightBracket",
// "Grave", "Keypad0".."Keypad9", "KeypadPlus", "KeypadMinus", "KeypadMultiply",
// "KeypadDivide", "KeypadPeriod", "MouseBack", "MouseForward"; "Unknown" for Key::Unknown and
// out-of-range values. F13..F24 are ordinary keys (docs/features.md: accepted).
[[nodiscard]] std::string_view to_string(Key key) noexcept;

// Canonical ASCII chord text: modifiers in the order Super, Ctrl, Alt, Shift, then the key, joined
// by '+': "Ctrl+Shift+L", "Alt+Left", "F5". This is the keymap-file / log / test form. The ui
// renders key hints from it with translated modifier names (i18n keys "key.ctrl", "key.shift",
// "key.alt", "key.super"; key legends themselves are not translated).
[[nodiscard]] std::string to_string(KeyChord chord);

// Inverse of to_string(KeyChord). Tokens are separated by '+', matched ASCII-case-insensitively,
// surrounding spaces ignored; modifiers may appear in any order and also as "Control" and
// "Meta"/"Win" (= Super). Exactly one key token, last. Phase 8 key synonyms (input only; output
// is always the canonical name): "Numpad0".."Numpad9", "NumpadEnter", "NumpadPlus",
// "NumpadMinus", "NumpadMultiply", "NumpadDivide", "NumpadPeriod" for the Keypad names;
// "ContextMenu" for "Menu"; "Esc" for "Escape"; "Return" for "Enter"; "Del" for "Delete";
// "Ins" for "Insert"; "PgUp" / "PgDn" for "PageUp" / "PageDown". Errors: InvalidArgument (empty
// text, empty token, unknown token, repeated modifier, missing key, more than one key, "Unknown").
[[nodiscard]] Result<KeyChord> parse_key_chord(std::string_view text);

// A chord bound to a command name with an optional arg (see CommandId comments), e.g.
// Shift+Down -> {"list.cursor_down", CursorArg::Extend}.
struct KeyBinding {
    KeyChord chord;
    std::string command; // CommandInfo::name; not validated against a registry by Keymap
    std::uint64_t arg = 0;
};

// ---------------------------------------------------------------------------------------------
// Key sequences (Phase 8)
// ---------------------------------------------------------------------------------------------

// Longest key sequence a binding can have. A sequence is 1..kMaxSequenceLength chords.
inline constexpr std::size_t kMaxSequenceLength = 4;

// Default, minimum and maximum time between two chords of one sequence. The timeout runs from the
// previous chord (not from the first); see SequenceMatcher. A keymap file may override the
// default (keymap_file.hpp "sequence_timeout_ms"); values outside [min, max] are clamped.
inline constexpr std::chrono::milliseconds kDefaultSequenceTimeout{1000};
inline constexpr std::chrono::milliseconds kMinSequenceTimeout{200};
inline constexpr std::chrono::milliseconds kMaxSequenceTimeout{5000};

// An ordered list of chords pressed one after the other: "Ctrl+K Ctrl+S" is Ctrl+K, then Ctrl+S.
// A sequence of one chord is an ordinary chord binding. A plain value; the constructor from a
// KeyChord is implicit so every API that takes a KeySequence also takes a KeyChord.
struct KeySequence {
    std::vector<KeyChord> chords;

    KeySequence() = default;
    KeySequence(KeyChord chord) : chords{chord} {} // NOLINT(google-explicit-constructor)
    explicit KeySequence(std::vector<KeyChord> list) : chords(std::move(list)) {}

    [[nodiscard]] std::size_t size() const noexcept { return chords.size(); }
    [[nodiscard]] bool empty() const noexcept { return chords.empty(); }

    friend bool operator==(const KeySequence&, const KeySequence&) = default;
};

// Canonical text: chord texts (to_string(KeyChord)) joined by ONE ASCII space, "Ctrl+K Ctrl+S".
// Empty sequence: "". This is the keymap-file / log / test form; the ui renders hints from it
// like chords (translated modifier names; a thin arrow or "then" between chords is ui styling).
[[nodiscard]] std::string to_string(const KeySequence& sequence);

// Inverse. Chords are separated by runs of ASCII whitespace (space, tab); each token is parsed by
// parse_key_chord, so "Ctrl + K" with spaces around '+' is NOT accepted (it would split into
// tokens): write "Ctrl+K". Errors: InvalidArgument (empty text, a bad chord token, more than
// kMaxSequenceLength chords). Does not apply validate_sequence() (the rules for binding).
[[nodiscard]] Result<KeySequence> parse_key_sequence(std::string_view text);

// True for a chord that a focused list turns into TYPE-TO-FILTER text when no binding claims it
// (docs/keymap.md "Type to filter"): a printable key (letters, top-row digits, punctuation keys,
// Space, keypad digits and operators) with no modifiers other than Shift. AltGr produces text
// too but is not a modifier (see above) so it needs no case. Pure, constexpr-friendly.
[[nodiscard]] bool is_text_input_chord(KeyChord chord) noexcept;

// Rules for a sequence to be bindable (checked by Keymap::bind and by keymap files):
//   V1  1..kMaxSequenceLength chords; every key is not Key::Unknown; mods within kModifierMask.
//   V2  A sequence of two or more chords: its FIRST chord must not be a text-input chord
//       (is_text_input_chord), so a user can never turn "g" or "Shift+G" into a sequence prefix
//       that steals type-to-filter (docs/review/phase-7.md). Ctrl, Alt or Super chords, function
//       keys, navigation keys and the like are fine.
//   V3  A sequence of two or more chords contains neither a bare Escape (Escape cancels a pending
//       sequence, so it could never be matched) nor a mouse key (Key::MouseBack/MouseForward).
// A single chord (length 1) is only subject to V1; bare printable single chords are allowed but
// warned about (keymap_file.hpp BindingWarning::ShadowsTypeToFilter).
// Errors: InvalidArgument naming the violated rule.
[[nodiscard]] Result<void> validate_sequence(const KeySequence& sequence);

// A sequence bound to a command name with an optional arg (see CommandId comments), e.g.
// {Ctrl+K Ctrl+S} -> {"keymap.edit", 0}.
struct SequenceBinding {
    KeySequence sequence;
    std::string command; // CommandInfo::name; not validated against a registry by Keymap
    std::uint64_t arg = 0;

    friend bool operator==(const SequenceBinding&, const SequenceBinding&) = default;
};

// How a candidate sequence relates to an existing binding (find_conflicts).
//   Identical         same sequence (the existing binding would be replaced)
//   PrefixOfExisting  the candidate is a strict prefix of the existing binding ("Ctrl+K" vs
//                     "Ctrl+K Ctrl+S")
//   ExtendsExisting   an existing binding is a strict prefix of the candidate
enum class ConflictKind : std::uint8_t { Identical, PrefixOfExisting, ExtendsExisting };

struct BindingConflict {
    ConflictKind kind = ConflictKind::Identical;
    SequenceBinding existing;

    friend bool operator==(const BindingConflict&, const BindingConflict&) = default;
};

// A chord that is a binding on its own and a prefix of a longer binding would be ambiguous (fire
// now, or wait for more?). A Keymap therefore keeps its bindings PREFIX-FREE: no binding is a
// strict prefix of another, and a sequence is bound at most once. There is no override (a later
// phase may add an explicit "allow prefix" if a real need appears); consequently a SequenceMatcher
// never sees a chord that is both complete and a prefix.
class Keymap {
public:
    Keymap() = default;

    // Adds a single-chord binding. A chord maps to at most one binding; one command may have many
    // chords. Errors: InvalidArgument (chord.key == Key::Unknown, mods outside kModifierMask, or
    // empty command), AlreadyExists (the chord is already bound, to anything, OR is a strict
    // prefix of an existing sequence binding; the map is unchanged and the message names the
    // conflicting binding as "<chord> conflicts with <existing sequence> (<command>)").
    Result<void> bind(KeyChord chord, std::string_view command, std::uint64_t arg = 0);

    // Adds a binding for a sequence of 1..kMaxSequenceLength chords. Length 1 is the chord
    // overload. Errors: InvalidArgument (validate_sequence fails, or empty command),
    // AlreadyExists (find_conflicts is not empty: identical, a strict prefix of an existing
    // binding, or extending one; the map is unchanged and the message names the FIRST
    // conflicting binding in bind order as above).
    Result<void> bind(const KeySequence& sequence, std::string_view command, std::uint64_t arg = 0);

    // Removes the binding of `chord` (a single-chord binding only). Returns false when the chord
    // was not bound as a single chord.
    bool unbind(KeyChord chord) noexcept;

    // Removes the binding whose sequence is exactly `sequence`. False when there is none.
    bool unbind(const KeySequence& sequence) noexcept;

    // Removes every binding.
    void clear() noexcept;

    // Binding for the exact chord (single-chord bindings only), or nullptr. A chord that is only
    // the first chord of a sequence resolves to nullptr here; use SequenceMatcher for it. Valid
    // until the next bind/unbind/clear.
    [[nodiscard]] const KeyBinding* resolve(KeyChord chord) const noexcept;

    // The binding of exactly this sequence (any length), by value. nullopt when none.
    [[nodiscard]] std::optional<SequenceBinding> find(const KeySequence& sequence) const;

    // Result of lookup(): `exact` is the binding whose sequence equals the chords; `is_prefix` is
    // true when at least one binding is strictly longer and starts with them. Never both (the
    // map is prefix-free).
    struct SequenceLookup {
        std::optional<SequenceBinding> exact;
        bool is_prefix = false;
    };
    // O(number of bindings); called once per key press, which is fine for hundreds of bindings.
    [[nodiscard]] SequenceLookup lookup(Span<const KeyChord> chords) const;

    // Chords bound to `command` as SINGLE chords (any arg), in bind order. The first one is the
    // "primary" chord shown as the key hint in menus and the palette (when there is one).
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command) const;

    // Chords bound to exactly (command, arg) as single chords, in bind order.
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command,
                                                   std::uint64_t arg) const;

    // Phase 8: every sequence bound to the command (any arg), single chords first (bind order)
    // then longer sequences (bind order). The first element is the primary shortcut the ui shows
    // (a single chord wins over a sequence). Empty when unbound.
    [[nodiscard]] std::vector<KeySequence> sequences_for(std::string_view command) const;
    [[nodiscard]] std::vector<KeySequence> sequences_for(std::string_view command,
                                                         std::uint64_t arg) const;

    // Single-chord bindings in bind order. Valid until the next bind/unbind/clear. (Unchanged
    // since Phase 2: sequences of two or more chords are NOT in this list.)
    [[nodiscard]] Span<const KeyBinding> bindings() const noexcept;

    // Bindings of sequences with two or more chords, in bind order.
    [[nodiscard]] Span<const SequenceBinding> sequence_bindings() const noexcept;

    // Every binding (single chords first, then sequences), by value. For the keymap editor.
    [[nodiscard]] std::vector<SequenceBinding> all_bindings() const;

private:
    std::vector<KeyBinding> bindings_;
    std::vector<SequenceBinding> sequences_;
};

// Existing bindings that bind(candidate) would collide with, in bind order (single chords first,
// then sequences): identical, strict prefixes of the candidate, and bindings the candidate is a
// strict prefix of. Empty when the candidate can be bound. Pure.
[[nodiscard]] std::vector<BindingConflict> find_conflicts(const Keymap& keymap,
                                                          const KeySequence& candidate);

// Adds Navigator's default bindings (docs/keymap.md, in the table order there; Phase 8 adds the
// single default sequence Ctrl+K Ctrl+S -> keymap.edit as the last binding). Binds into the
// given map without clearing it. Errors: AlreadyExists if any default chord is already bound or
// conflicts; in that case nothing is added.
Result<void> load_default_keymap(Keymap& keymap);

// Resolves a chord through the keymap and the registry: the Command {id, arg} to enqueue, or
// nullopt when the chord is unbound or its command name is not registered. Single chords only.
[[nodiscard]] std::optional<Command>
resolve_command(const Keymap& keymap, const CommandRegistry& registry, KeyChord chord) noexcept;

// The Command {id, arg} for a binding, or nullopt when its command name is not registered.
[[nodiscard]] std::optional<Command> to_command(const CommandRegistry& registry,
                                                const SequenceBinding& binding) noexcept;

// ---------------------------------------------------------------------------------------------
// SequenceMatcher: the state machine behind "Ctrl+K Ctrl+S" (Phase 8)
// ---------------------------------------------------------------------------------------------

// Outcome of SequenceMatcher::feed(). The matcher never runs anything: on Match the ui enqueues
// the Command (to_command) like any key binding, so confirmation sheets and focus scoping of the
// command apply unchanged.
struct SequenceResult {
    enum class Kind : std::uint8_t {
        // The chord is not part of any sequence (nothing was pending, and it does not start one).
        // NOT consumed: the ui resolves it as an ordinary chord (resolve_command).
        NoMatch,
        // The chord started or extended a sequence that is not complete yet. Consumed. `sequence`
        // holds the chords typed so far (the ui shows "Ctrl+K ..." in the status bar).
        Pending,
        // The chord completed a sequence. Consumed. `binding` is it; the matcher is idle again.
        Match,
        // Escape arrived while a sequence was pending. Consumed; the pending chords are
        // discarded and nothing runs. `sequence` holds the discarded chords.
        Cancelled,
        // A chord arrived while a sequence was pending but "pending chords + this chord" is
        // neither a sequence nor a prefix of one (including a chord that is itself bound alone:
        // pending Ctrl+K, then Ctrl+Q does NOT quit). Consumed; nothing runs; the matcher is
        // idle. `sequence` holds the pending chords followed by the offending chord (the ui may
        // show "Ctrl+K Ctrl+Q is not a command").
        Aborted,
    };
    Kind kind = Kind::NoMatch;
    bool consumed = false; // true for every kind except NoMatch
    // True when a pending sequence had timed out just before this chord was handled; the matcher
    // dropped it and handled the chord as a first chord (the ui should clear its hint).
    bool expired = false;
    std::vector<KeyChord> sequence;
    std::optional<SequenceBinding> binding; // Match only
};

// UI-thread only; plain value. Holds only the pending chords and the time of the last one, never
// a reference to the Keymap: the map is passed to every call, so rebinding in the keymap editor
// while a sequence is pending cannot dangle (the next feed re-checks the pending chords against
// the new map; if they are no longer a prefix the result is Aborted).
//
// Timing: `now` is a steady-clock time supplied by the caller (tests pass synthetic times). A
// pending sequence is EXPIRED when now - time_of_last_chord > timeout (strictly greater: a chord
// exactly `timeout` after the previous one still continues the sequence). Times that go
// backwards count as zero elapsed. The default timeout is kDefaultSequenceTimeout (1000 ms).
class SequenceMatcher {
public:
    using Clock = std::chrono::steady_clock;

    explicit SequenceMatcher(std::chrono::milliseconds timeout = kDefaultSequenceTimeout) noexcept;

    // Clamped to [kMinSequenceTimeout, kMaxSequenceTimeout]. Does not affect the pending chords.
    void set_timeout(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept;

    // Feeds one chord (see "Phase 8" in the Keymap comment for what may be fed).
    //   1. If a sequence is pending and has expired: drop it (SequenceResult::expired = true) and
    //      continue as if idle.
    //   2. Pending, chord == Escape without modifiers: Cancelled.
    //   3. Pending: candidate = pending chords + chord. keymap.lookup(candidate): exact -> Match
    //      (idle again; the binding's length is >= 2 by construction); is_prefix -> Pending;
    //      neither -> Aborted (idle again).
    //   4. Idle: keymap.lookup({chord}). is_prefix -> Pending (the chord is the first of at least
    //      one sequence). Otherwise NoMatch, including when the chord is itself a single-chord
    //      binding (the ui resolves it).
    [[nodiscard]] SequenceResult feed(const Keymap& keymap, KeyChord chord, Clock::time_point now);

    // Drops a pending sequence that has expired at `now` and returns true if it did. The ui calls
    // it every frame while pending() so the status-bar hint disappears on time even without a
    // key press; deadline() tells when to wake up.
    bool expire(Clock::time_point now) noexcept;

    // Forgets any pending chords (focus change, popup/sheet/overlay opens, mouse click, keymap
    // replaced).
    void reset() noexcept;

    [[nodiscard]] bool pending() const noexcept { return !pending_.empty(); }
    [[nodiscard]] Span<const KeyChord> prefix() const noexcept { return pending_; }
    // When the pending sequence expires (last chord time + timeout); nullopt when idle.
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;

private:
    std::chrono::milliseconds timeout_;
    std::vector<KeyChord> pending_;
    Clock::time_point last_{};
};

} // namespace navigator

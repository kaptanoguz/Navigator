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
//   1280-1299  edit / clipboard (Phase 4: copy path; file operations add copy/cut/paste here)
//   1300-1319  filter, flatten, name search, index (Phase 5)
//   1320-1339  Inspector and previews (Phase 6)
//   1340+      reserved for later phases (file operations, ...)
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
    ToggleViewLock = 6,   // "view.toggle_view_lock" Phase 4 STUB: registered, unbound, no-op.
                          //   Reserved for the address-bar lock (folder / hierarchy / global
                          //   view settings, docs/features.md). arg: 0 (scopes reserved)

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
};

// Bit flags for the `arg` of CopyPath. Unknown bits are ignored (reserved).
//   0:             selection, else the current folder (Ctrl+Shift+C).
//   CurrentFolder: always the current folder (breadcrumb / tab context menu).
struct CopyPathArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t CurrentFolder = std::uint64_t{1} << 0U;
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
// Later phases may add bits (e.g. "move cursor without touching the selection" for the File
// Pilot-style Ctrl+Space cursor selection, Phase 8) without new ids.
struct CursorArg {
    static constexpr std::uint64_t None = 0;
    static constexpr std::uint64_t Extend = std::uint64_t{1} << 0U;
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

// Static description of a command.
//   name:      stable dotted id, e.g. "app.quit" (keymap files and the palette use it)
//   title_key: i18n key for the palette title, e.g. "cmd.app.quit" (see i18n.hpp)
// Both views must point to storage that outlives the registry (string literals in practice).
struct CommandInfo {
    CommandId id = CommandId::None;
    std::string_view name;
    std::string_view title_key;
};

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

// Registers every built-in CommandId (except None) with its name and title_key
// ("cmd." + name). Registration order: ascending CommandId. A failure (AlreadyExists) leaves
// the registry unchanged.
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
//   - Keypad digits/operators map to Key::Unknown in Phase 2; only KeypadEnter is distinct.
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
// Multi-key sequences ("g g") and command aliases arrive in Phase 8 as an addition (e.g. a
// KeySequence type and Keymap::bind_sequence); the single-chord API below stays.

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

    // Punctuation by US legend, 140..150 (for later bindings such as Ctrl+, and Ctrl+=)
    Minus = 140, Equals, Comma, Period, Slash, Backslash, Semicolon, Apostrophe, LeftBracket,
    RightBracket, Grave,

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
// "Escape", "Backspace", "Delete", "Tab", "Space", "Insert", "Minus", "Equals", "Comma",
// "Period", "Slash", "Backslash", "Semicolon", "Apostrophe", "LeftBracket", "RightBracket",
// "Grave", "MouseBack", "MouseForward"; "Unknown" for Key::Unknown and out-of-range values.
[[nodiscard]] std::string_view to_string(Key key) noexcept;

// Canonical ASCII chord text: modifiers in the order Super, Ctrl, Alt, Shift, then the key, joined
// by '+': "Ctrl+Shift+L", "Alt+Left", "F5". This is the keymap-file / log / test form. The ui
// renders key hints from it with translated modifier names (i18n keys "key.ctrl", "key.shift",
// "key.alt", "key.super"; key legends themselves are not translated).
[[nodiscard]] std::string to_string(KeyChord chord);

// Inverse of to_string(KeyChord). Tokens are separated by '+', matched ASCII-case-insensitively,
// surrounding spaces ignored; modifiers may appear in any order and also as "Control" and
// "Meta"/"Win" (= Super). Exactly one key token, last. Errors: InvalidArgument (empty text, empty
// token, unknown token, repeated modifier, missing key, more than one key, "Unknown").
[[nodiscard]] Result<KeyChord> parse_key_chord(std::string_view text);

// A chord bound to a command name with an optional arg (see CommandId comments), e.g.
// Shift+Down -> {"list.cursor_down", CursorArg::Extend}.
struct KeyBinding {
    KeyChord chord;
    std::string command; // CommandInfo::name; not validated against a registry by Keymap
    std::uint64_t arg = 0;
};

class Keymap {
public:
    Keymap() = default;

    // Adds a binding. A chord maps to at most one binding; one command may have many chords.
    // Errors: InvalidArgument (chord.key == Key::Unknown, mods outside kModifierMask, or empty
    // command), AlreadyExists (the chord is already bound, to anything; the map is unchanged).
    Result<void> bind(KeyChord chord, std::string_view command, std::uint64_t arg = 0);

    // Removes the binding of `chord`. Returns false when the chord was not bound.
    bool unbind(KeyChord chord) noexcept;

    // Removes every binding.
    void clear() noexcept;

    // Binding for the exact chord, or nullptr. Valid until the next bind/unbind/clear.
    [[nodiscard]] const KeyBinding* resolve(KeyChord chord) const noexcept;

    // Chords bound to `command` (any arg), in bind order. The first one is the "primary" chord
    // shown as the key hint in menus and the palette.
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command) const;

    // Chords bound to exactly (command, arg), in bind order.
    [[nodiscard]] std::vector<KeyChord> chords_for(std::string_view command,
                                                   std::uint64_t arg) const;

    // All bindings in bind order. Valid until the next bind/unbind/clear.
    [[nodiscard]] Span<const KeyBinding> bindings() const noexcept;

private:
    std::vector<KeyBinding> bindings_;
};

// Adds Navigator's default bindings (docs/keymap.md, in the table order there). Binds into the
// given map without clearing it. Errors: AlreadyExists if any default chord is already bound; in
// that case nothing is added.
Result<void> load_default_keymap(Keymap& keymap);

// Resolves a chord through the keymap and the registry: the Command {id, arg} to enqueue, or
// nullopt when the chord is unbound or its command name is not registered.
[[nodiscard]] std::optional<Command>
resolve_command(const Keymap& keymap, const CommandRegistry& registry, KeyChord chord) noexcept;

} // namespace navigator

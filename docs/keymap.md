# Keymap

Owner: `commands`. Default key bindings for every user-facing command.

## How bindings work

- Every user action is a `CommandId` registered in the `CommandRegistry`
  (`include/navigator/commands.hpp`) with a stable dotted name (e.g. `app.quit`) and an i18n
  title key (`cmd.<name>`).
- A binding maps a key chord to a command **name**. The UI layer resolves the name through the
  registry (`CommandRegistry::find(std::string_view)`) and, when the chord fires, calls
  `CommandQueue::enqueue()`. The command runs at frame end, never mid-draw.
- Widgets never hardcode behavior behind a key. A hardcoded key is allowed only as a last-resort
  fallback, and even then it must enqueue a registered command.
- Every command listed here also appears in the command palette.

## Phase 0 defaults

| Chord          | Command name              | CommandId          | Notes                         |
|----------------|---------------------------|--------------------|-------------------------------|
| `Ctrl+Q`       | `app.quit`                | `Quit` (1)         |                               |
| `F12`          | `view.toggle_fps_overlay` | `ToggleFpsOverlay` (2) | Debug/perf overlay |
| `Ctrl+Shift+L` | `view.toggle_theme`       | `ToggleTheme` (3)  | Light/dark switch             |
| window close   | `app.quit`                | `Quit` (1)         | Window-manager close request  |

`Ctrl` is the primary modifier on Linux. Chords match by keycode (the legend of the active
layout), falling back to the physical position only when the legend is not a Latin letter, so
Turkish Q and F layouts both work (details in `include/navigator/commands.hpp`, "Keymap").

## Phase 2 defaults (single-pane browser)

These are Navigator's own defaults. File Pilot never published a full keymap (see
`docs/features.md`); none of the rows below is attributed to it. Conventions follow common Linux
file managers (GNOME Files, Nemo) where one exists.

`load_default_keymap()` binds exactly the rows of the Phase 0 table (except "window close", which
is a window event, not a chord) followed by this table, in this order. The first chord of a
command is its primary key hint.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Enter` | `list.activate` | `Activate` (1100) | 0 | Folder: enter; file: default app (`xdg-open`) |
| `KeypadEnter` | `list.activate` | `Activate` (1100) | 0 | |
| `Alt+Left` | `nav.back` | `NavigateBack` (1000) | 0 | |
| `MouseBack` | `nav.back` | `NavigateBack` (1000) | 0 | Mouse side button X1 |
| `Alt+Right` | `nav.forward` | `NavigateForward` (1001) | 0 | |
| `MouseForward` | `nav.forward` | `NavigateForward` (1001) | 0 | Mouse side button X2 |
| `Alt+Up` | `nav.up` | `NavigateUp` (1002) | 0 | |
| `Backspace` | `nav.up` | `NavigateUp` (1002) | 0 | |
| `F5` | `nav.refresh` | `Refresh` (1003) | 0 | |
| `Ctrl+R` | `nav.refresh` | `Refresh` (1003) | 0 | |
| `Ctrl+A` | `list.select_all` | `SelectAll` (1101) | 0 | |
| `Escape` | `list.clear_selection` | `ClearSelection` (1102) | 0 | Dismisses a visible notice first; otherwise clears the selection. Cursor stays |
| `Ctrl+H` | `view.toggle_hidden` | `ToggleHidden` (4) | 0 | GNOME convention |
| `Up` | `list.cursor_up` | `CursorUp` (1110) | 0 | Selection = cursor row |
| `Shift+Up` | `list.cursor_up` | `CursorUp` (1110) | `Extend` | Extend from anchor |
| `Down` | `list.cursor_down` | `CursorDown` (1111) | 0 | |
| `Shift+Down` | `list.cursor_down` | `CursorDown` (1111) | `Extend` | |
| `PageUp` | `list.cursor_page_up` | `CursorPageUp` (1112) | 0 | One visible page |
| `Shift+PageUp` | `list.cursor_page_up` | `CursorPageUp` (1112) | `Extend` | |
| `PageDown` | `list.cursor_page_down` | `CursorPageDown` (1113) | 0 | |
| `Shift+PageDown` | `list.cursor_page_down` | `CursorPageDown` (1113) | `Extend` | |
| `Home` | `list.cursor_home` | `CursorHome` (1114) | 0 | First row |
| `Shift+Home` | `list.cursor_home` | `CursorHome` (1114) | `Extend` | |
| `End` | `list.cursor_end` | `CursorEnd` (1115) | 0 | Last row |
| `Shift+End` | `list.cursor_end` | `CursorEnd` (1115) | `Extend` | |

`Extend` is `CursorArg::Extend` (bit 0 of `Command::arg`).

`nav.focus_address_bar` (`FocusAddressBar`, 1004) was registered unbound in Phase 2; Phase 4
binds it to `Ctrl+L` (see "Phase 4 defaults").

### Mouse (Phase 2)

Pointer gestures carry a row index or a rectangle, so they are not keymap chords. The list widget
records them during drawing and the ui applies them at frame end, before draining the command
queue (see `docs/architecture.md`, "Phase 2 browser data flow"). Where a gesture has a command
equivalent it enqueues that command.

| Gesture | Effect |
|---------|--------|
| Click on a row | Cursor and anchor to the row; selection = that row |
| `Ctrl`+click | Toggle the row in the selection; cursor and anchor to the row |
| `Shift`+click | Select anchor..row (replaces the selection); anchor stays |
| `Ctrl+Shift`+click | Add anchor..row to the selection |
| Double-click on a row | As click, then enqueue `list.activate` |
| Click on empty space below the rows | Clear the selection (cursor stays) |
| Drag from empty space (rubber band) | Select the rows the rectangle touches; with `Ctrl`, add to the selection |
| Mouse side buttons | `nav.back` / `nav.forward` through the keymap (`MouseBack` / `MouseForward`) |
| Wheel | Scroll the view; the cursor does not move |

Double-click on empty background to go to the parent folder is listed for File Pilot in
`docs/features.md` (Phase 2 row); it is optional in Phase 2 and, if implemented, enqueues
`nav.up`.

The `Ctrl`+click question raised here in Phase 2 is resolved in Phase 3 (see "Opening in a new
tab" below): `Ctrl`+click stays toggle-selection.

## Phase 3 defaults (tabs, split panes, layouts)

Navigator's own defaults. `docs/features.md` attributes only `Ctrl+T`, `Ctrl+W` and `Ctrl+N` to
File Pilot; every other chord below is Navigator's choice, following browser and Linux file
manager conventions (GNOME Files, Nemo and Dolphin use `F3` for a split view; GNOME uses `F6` to
cycle panes). Chords reserved for later phases by `docs/features.md` stay free: `Ctrl+E`
(flatten, bound in Phase 5), `Ctrl+B` (sidebar, bound in Phase 4), `Ctrl+Shift+P` (palette, Phase 8), `Ctrl+,`
(settings), `Ctrl+Space` (cursor selection, Phase 8), `Ctrl+Shift+N` (new folder, Phase 7).
`Ctrl+Alt+arrows` are avoided because GNOME and Cinnamon switch workspaces with them. Chords on
punctuation keys (such as `Ctrl+\`) are avoided because Turkish layouts put those legends
behind AltGr.

`load_default_keymap()` binds the Phase 0 table, then the Phase 2 table, then this table, in
this order.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Ctrl+T` | `tab.new` | `NewTab` (1200) | 0 | Opens the active tab's folder (File Pilot behaviour per `features.md`) |
| `Ctrl+W` | `tab.close` | `CloseTab` (1201) | 0 | No-op on the last tab of the last pane |
| `Ctrl+F4` | `tab.close` | `CloseTab` (1201) | 0 | |
| `Ctrl+Shift+T` | `tab.reopen_closed` | `ReopenClosedTab` (1204) | 0 | |
| `Ctrl+Tab` | `tab.next` | `NextTab` (1205) | 0 | Wraps within the active pane |
| `Ctrl+PageDown` | `tab.next` | `NextTab` (1205) | 0 | |
| `Ctrl+Shift+Tab` | `tab.previous` | `PreviousTab` (1206) | 0 | |
| `Ctrl+PageUp` | `tab.previous` | `PreviousTab` (1206) | 0 | |
| `Ctrl+Shift+PageUp` | `tab.move_left` | `MoveTabLeft` (1208) | 0 | Keyboard reorder |
| `Ctrl+Shift+PageDown` | `tab.move_right` | `MoveTabRight` (1209) | 0 | |
| `Ctrl+Shift+D` | `tab.duplicate` | `DuplicateTab` (1202) | 0 | |
| `Ctrl+Shift+A` | `tab.search` | `SearchTabs` (1207) | 0 | Open and recently closed tabs |
| `Ctrl+Enter` | `list.open_in_new_tab` | `OpenInNewTab` (1116) | 0 | Background tab; on a file same as `Enter`. Phase 4's focused address bar takes `Ctrl+Enter` first |
| `Ctrl+KeypadEnter` | `list.open_in_new_tab` | `OpenInNewTab` (1116) | 0 | |
| `F3` | `pane.split_right` | `SplitRight` (1220) | 0 | |
| `Shift+F3` | `pane.split_down` | `SplitDown` (1221) | 0 | |
| `F6` | `pane.focus_next` | `FocusNextPane` (1222) | 0 | Reading order, wraps |
| `Shift+F6` | `pane.focus_previous` | `FocusPreviousPane` (1223) | 0 | |
| `Ctrl+Shift+W` | `pane.close` | `ClosePane` (1224) | 0 | No-op on the only pane |
| `Ctrl+Shift+S` | `layout.save` | `SaveLayout` (1240) | 0 | Inline name prompt |
| `Ctrl+Shift+O` | `layout.load` | `LoadLayout` (1241) | 0 | Layout picker |
| (unbound) | `tab.close_left` | `CloseLeftTabs` (1203) | 0 | Tab context menu |
| (unbound) | `layout.delete` | `DeleteLayout` (1242) | 0 | Layout picker in delete mode |
| (unbound) | `window.new` | `NewWindow` (1250) | 0 | Reserved, no-op (see `Ctrl+N` below) |

Tab commands take `arg` = 0 for the active tab, or a `layout::TabId` value when a tab bar button
or the tab context menu targets a specific tab; pane commands likewise take 0 or a
`layout::PaneId` value (`include/navigator/commands.hpp`). `tab.new` takes `NewTabArg` flags
instead.

`Ctrl+N` (File Pilot: duplicate the tab into a new window, the source stays open) is **deferred**
to the multi-window phase. Navigator has one window until then; `window.new` is registered with a
stable id and stays unbound, and `Ctrl+N` is left free for it. Use `tab.duplicate`
(`Ctrl+Shift+D`) meanwhile.

### Popups (Phase 3)

The layout name prompt, the layout picker and the Search Tabs list are ui popups. While one is
open it takes keyboard input: printable text goes to its text field (SDL text input, not
chords), and the ui routes `list.cursor_up` / `list.cursor_down` (move the highlight),
`list.activate` (confirm) and `list.clear_selection` (`Escape`: close the popup) to it instead of
the browser. No extra chords are bound; focus-scoped bindings arrive in Phase 4.

### Mouse (Phase 3)

| Gesture | Effect |
|---------|--------|
| Click on a tab | Activate it (and its pane) |
| Middle-click on a tab | `tab.close` for that tab |
| Click on the `+` button | `tab.new` in that pane |
| Middle-click on the `+` button | `tab.new` with `NewTabArg::Home` (new tab on the home folder; Navigator's choice, `features.md` lists the gesture without its effect) |
| Double-click on empty tab-bar space | `tab.new` |
| Right-click on a tab | Tab context menu: New tab, Duplicate, Close, Close left tabs, Reopen closed tab |
| Drag a tab along a tab bar | Reorder; drop on another pane's tab bar moves it there at the drop position |
| Drag a tab onto a pane's edge zone | Split: new pane at that edge (left, right, top, bottom) holding the tab |
| Drag a tab onto a pane's centre | Move it to that pane (appended, active) |
| `Escape` during a tab drag | Cancel the drag |
| Drag a splitter | Resize the two adjacent panes (each keeps a minimum size) |
| Double-click a splitter | Equal sizes for that split |
| Any click in an inactive pane | Activate that pane first, then the click acts as usual |
| Middle-click on a folder row | Cursor and anchor to the row (selection unchanged), then `list.open_in_new_tab`. On a file: nothing |
| `Ctrl`+double-click on a row | `list.open_in_new_tab` for the row |

#### Opening in a new tab: the `Ctrl`+click conflict

`docs/features.md` lists "`Ctrl`+click / `Ctrl`+double-click opens in new tab" for File Pilot,
while Phase 2 (and the roadmap) use `Ctrl`+click to toggle a row in the selection. Navigator
keeps **`Ctrl`+click = toggle selection** and opens in a new tab with **`Ctrl`+double-click**
and **middle-click**:

- `Ctrl`+double-click is two `Ctrl`+clicks (each toggles the row, so the selection ends where it
  started) followed by `list.open_in_new_tab` instead of `list.activate`. On a file it behaves
  like a plain double-click (open with the default app).
- Middle-click on a folder row needs no modifier and never touches the selection.
- A single `Ctrl`+click never opens a tab.

## Phase 4 defaults (sidebar, GoTo, address bar, bookmarks)

`docs/features.md` attributes only `Ctrl+B` (toggle sidebar) and "`Ctrl+Enter` opens" in the
address bar to File Pilot; every other chord below is Navigator's choice. `Ctrl+L` (location)
and `Ctrl+D` (bookmark) follow GNOME Files and web browsers. GoTo takes `Ctrl+G` ("go"):
`Ctrl+P` stays free because it sits next to `Ctrl+Shift+P` (palette, Phase 8) and editors use it
for quick-open, which Phase 5 search may want. `Ctrl+Shift+C` (copy path) follows GNOME Files'
"Copy Location"; plain `Ctrl+C` stays reserved for copying files (Phase 7). None of these chords
was bound before. Chords on punctuation keys remain avoided (Turkish layouts, see Phase 3).

`load_default_keymap()` binds the Phase 0, Phase 2 and Phase 3 tables, then this table, in this
order.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Ctrl+B` | `view.toggle_sidebar` | `ToggleSidebar` (5) | 0 | File Pilot behaviour per `features.md` |
| `Ctrl+L` | `nav.focus_address_bar` | `FocusAddressBar` (1004) | 0 | Breadcrumbs become an editable path, all selected |
| `Ctrl+G` | `nav.goto` | `GoTo` (1005) | 0 | GoTo popup: path, recents, bookmarks, places, completion |
| `Ctrl+D` | `bookmarks.toggle` | `ToggleBookmark` (1261) | 0 | Bookmark the active folder, or remove its bookmark |
| `Ctrl+Shift+C` | `edit.copy_path` | `CopyPath` (1280) | 0 | Selection, else the current folder; context-aware quoting |
| (unbound) | `sidebar.focus` | `FocusSidebar` (1260) | 0 | `F6` reaches the sidebar too (see below) |
| (unbound) | `bookmarks.rename` | `RenameBookmark` (1262) | 0 | Sidebar context menu |
| (unbound) | `bookmarks.move_up` | `MoveBookmarkUp` (1263) | 0 | Sidebar context menu; dragging also reorders |
| (unbound) | `bookmarks.move_down` | `MoveBookmarkDown` (1264) | 0 | Sidebar context menu |
| (unbound) | `recents.clear` | `ClearRecents` (1270) | 0 | Recents header / context menu |
| (unbound) | `recents.toggle_tracking` | `ToggleRecentsTracking` (1271) | 0 | Recents context menu; persisted |
| (unbound) | `view.toggle_view_lock` | `ToggleViewLock` (6) | 0 | Stub: registered, no-op |

Bookmark commands take `arg` = 0 (the bookmark row focused in the sidebar; for
`bookmarks.toggle` the active tab's folder) or a `places::BookmarkId` value from the context
menu. `edit.copy_path` takes `CopyPathArg` flags (`CurrentFolder`: ignore the selection; used by
the breadcrumb and tab context menus). Quoting (`path_input::format_paths_for_copy`, `Auto`): one
path is copied as is; several paths are shell-quoted where needed and joined by spaces.

`pane.focus_next` / `pane.focus_previous` (`F6` / `Shift+F6`) keep their chords; while the
sidebar is visible it is one more stop in the cycle: sidebar, then the panes in reading order,
then the sidebar again (design review, Phase 3 notes).

### Focus scoping (Phase 4)

The keymap stays global (one `Keymap`, no contexts). The ui scopes keys around it:

1. **Text fields** (address bar, GoTo, bookmark rename, and the Phase 3 name prompt) take SDL text
   input and these editing keys before the keymap is consulted: `Left`, `Right`, `Home`, `End`
   (also with `Shift` and `Ctrl`), `Backspace`, `Delete`, `Ctrl+Backspace`, `Ctrl+A` (select all
   text), `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+Z`. They also give field meanings to:

   | Key | Address bar | GoTo popup |
   |-----|-------------|------------|
   | `Enter`, `KeypadEnter` | Go: folder -> navigate the tab; file -> its folder, cursor on it | Same, for the highlighted row or else the typed text |
   | `Ctrl+Enter` | Target in a new background tab (file: open with the default app), as `list.open_in_new_tab` | Same |
   | `Tab` | Accept the highlighted completion (text becomes `~/Downloads/`) | Same |
   | `Up`, `Down`, `PageUp`, `PageDown` | Move the completion highlight | Move the list highlight |
   | `Escape` | Cancel: breadcrumbs back, focus to the list | Close the popup |

   Every other chord resolves through the keymap as usual, so `Ctrl+Q`, `Ctrl+B`, `Ctrl+W`, `F6`
   keep working while typing. `Ctrl+A` in a field selects its text, not the list.
2. **Sidebar and popups**: when either has focus, the ui sends the resolved `list.cursor_*`
   (row focus), `list.activate` (open the row in the active pane; on a section header:
   collapse/expand it), `list.open_in_new_tab` (`Ctrl+Enter`: new tab in the active pane) and
   `list.clear_selection` (`Escape`: back to the last active pane) to it instead of the active
   pane, as Phase 3 popups already do. No key is hardcoded in the sidebar widget.

### Mouse (Phase 4)

| Gesture | Effect |
|---------|--------|
| Click on a sidebar row (place, bookmark, recent, volume) | Navigate the active pane there |
| `Ctrl`+click or middle-click on a sidebar row | Open it in a new background tab in the active pane |
| Click on an offline or missing row | No navigation; a notice says the volume is not mounted / the folder is missing |
| Click on a section header | Collapse or expand the section (persisted in the session) |
| Right-click on a row | Context menu: Open, Open in new tab, Copy path; bookmarks add Rename, Move up, Move down, Remove; recents add Clear recents, Stop/Resume tracking |
| Drag a bookmark row | Reorder (insertion marker as in the tab strip) |
| Drag a tab onto the Bookmarks section | Bookmark its folder at the drop position |
| Drag a sidebar row onto a pane / pane edge | Optional in Phase 4: open it there or split, reusing the Phase 3 drop zones |
| Drag the sidebar's right edge | Resize (160-480 dp); double-click restores 220 dp |
| Click on a breadcrumb segment | Navigate to that folder (up the hierarchy) |
| Middle-click on a breadcrumb segment | Open that folder in a new background tab |
| Click on the chevron after a segment | Optional in Phase 4: dropdown of that folder's subfolders; picking one navigates |
| Click on the empty part of the breadcrumb bar | `nav.focus_address_bar` |
| Bookmark (star) button in the address bar | `bookmarks.toggle` (filled when the folder is bookmarked) |
| Lock icon in the address bar | `view.toggle_view_lock` (stub; drawn disabled) |

## Phase 5 defaults (filter, flatten, types, index)

`docs/features.md` attributes only `Ctrl+E` (flatten the hierarchy) to File Pilot; every other
chord below is Navigator's choice. `Ctrl+F` for the folder filter follows GNOME Files and
browsers ("find"). `Ctrl+Shift+E` widens `Ctrl+E` to the whole volume; `Ctrl+Shift+F` opens
the types popup ("filter by type"), so Phase 10 content search must pick another chord. Older
IBus versions used `Ctrl+Shift+E` for emoji input; current ones use `Ctrl+.` and `Ctrl+;`, and
a user who still has the old binding loses only this chord (palette and title-bar button
remain). Still free: `Ctrl+P` (quick-open), `Ctrl+C` (Phase 7), `Ctrl+Shift+P`, `Ctrl+,`,
`Ctrl+Space`, `Ctrl+N`, `Ctrl+Shift+N`. None of these chords was bound before. Chords on
punctuation keys remain avoided (Turkish layouts, see Phase 3).

`load_default_keymap()` binds the Phase 0, 2, 3 and 4 tables, then this table, in this order.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Ctrl+F` | `list.filter` | `FocusFilter` (1300) | 0 | Focus the pane's filter field, text selected. Typing in the list also starts the filter (below) |
| `Ctrl+E` | `search.flatten` | `ToggleFlatten` (1302) | 0 | File Pilot behaviour per `features.md`: flatten the folder's subtree; again: leave |
| `Ctrl+Shift+E` | `search.flatten_volume` | `FlattenVolume` (1303) | 0 | Flatten the whole volume (drive) of the folder; index-backed when ready |
| `Ctrl+Shift+F` | `search.types` | `ToggleTypesPopup` (1304) | 0 | Types popup: per-type visibility, toggle all |
| (unbound) | `list.clear_filter` | `ClearFilter` (1301) | 0 | Clear button on the filter chip; `Escape` cascade |
| (unbound) | `search.toggle_fuzzy` | `ToggleFuzzy` (1305) | 0 | Mode button in the filter field |
| (unbound) | `search.cancel` | `CancelSearch` (1306) | 0 | Progress bar button; `Escape` cascade |
| (unbound) | `search.rebuild_index` | `RebuildIndex` (1307) | 0 | Palette; status-bar index menu |

### Type to filter (Phase 5)

While a pane's list has keyboard focus (no text field, sidebar or popup focused), SDL text
input that no chord consumed (no `Ctrl`, `Alt` or `Super` held) focuses that pane's filter
field and inserts the text, as `list.filter` would, without selecting the previous text: the
new character replaces it. A leading `Space` is not captured (reserved for the Inspector,
Phase 6). Plain-letter key sequences (Phase 8) must coexist with this rule; the Phase 8 design
revisits it. This is Navigator's choice, not attributed to File Pilot.

### Focus scoping (Phase 5)

`docs/features.md` says most commands still fire while the folder filter has focus, so the
filter field consumes fewer keys than the Phase 4 text fields:

| Keys | While the filter field has focus |
|------|----------------------------------|
| Text input, `Left`, `Right`, `Home`, `End` (also with `Shift` / `Ctrl`), `Delete`, `Ctrl+Backspace`, `Ctrl+V`, `Ctrl+Z` | Edit the filter text; the list refilters on every edit |
| `Backspace` | Delete a character; on an empty field it does nothing (never `nav.up`) |
| `Ctrl+C`, `Ctrl+X` | Copy / cut the field's text while it has a text selection; otherwise they go through the keymap |
| `Escape` | 1. cancel a running flatten walk, 2. else clear the text, 3. else return focus to the list |
| `Tab` | Return focus to the list (the filter stays applied) |
| `Up`, `Down`, `PageUp`, `PageDown` (also with `Shift`) | Through the keymap to the active pane: move the list cursor among the shown rows |
| `Enter`, `KeypadEnter`, `Ctrl+Enter` | Through the keymap: `list.activate` / `list.open_in_new_tab` on the cursor row |
| `Ctrl+A` | Through the keymap: select all shown rows (unlike the Phase 4 fields) |
| Every other chord (`Ctrl+E`, `Ctrl+W`, `F6`, `Ctrl+Q`, ...) | Through the keymap as usual |

The filter stays applied after the field loses focus; while the field has focus the list cursor
is drawn as inactive (design review B1).

`Escape` with the list focused (`list.clear_selection`) undoes one step per press, innermost
first: 1. dismiss a visible notice, 2. cancel a running flatten walk or index query (rows found
so far stay), 3. clear a non-empty selection, 4. leave the flattened view, 5. clear the filter
text. The types filter is never cleared by `Escape` (it is a persisted view setting).

Navigation (`nav.*`, `list.activate` on a folder, sidebar, GoTo) clears the filter text and
leaves the flattened view; `nav.refresh` (`F5`) re-runs a flatten walk.

### Mouse (Phase 5)

| Gesture | Effect |
|---------|--------|
| Click in the filter field | Focus it (`list.filter`) |
| Click on the filter chip's clear button | `list.clear_filter` |
| Click on the flatten button in the title bar | `search.flatten` |
| Click on the types button | `search.types` |
| Click in the types popup | Toggle that type; "All" row: toggle all |
| Click on the progress bar's stop button | `search.cancel` |
| Click on a column header in the flattened view | Sort by that column instead of relevance (until the query changes) |

## Phase 6 defaults (Inspector)

`docs/features.md` attributes `Space` (open the Inspector) and `Alt`+numpad `Enter` (Inspect)
to File Pilot; both bind `view.inspect`. Every other Inspector command is unbound: fit / 1:1,
grid, minimap and lock are Inspector header buttons and palette entries (design review: zoom
keys must not steal list keys while the list has focus), and "Play" is the button on a video
poster. `Space` was free: no earlier table binds it, `Ctrl+Space` stays reserved for cursor
selection (Phase 8; chords match exactly, so they do not clash), and type-to-filter never
starts with a `Space` (Phase 5). `Alt+KeypadEnter` was free (`KeypadEnter` and
`Ctrl+KeypadEnter` are Phase 2 / Phase 3 rows). `Escape` keeps its Phase 5 cascade and does not
close the Inspector; `Space` toggles it.

`load_default_keymap()` binds the Phase 0, 2, 3, 4 and 5 tables, then this table, in this order.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Space` | `view.inspect` | `Inspect` (1320) | 0 | Show the Inspector on the cursor entry; again: hide. File Pilot behaviour per `features.md` |
| `Alt+KeypadEnter` | `view.inspect` | `Inspect` (1320) | 0 | File Pilot behaviour per `features.md` |
| (unbound) | `inspector.toggle_lock` | `ToggleInspectorLock` (1321) | 0 | Lock button in the Inspector header |
| (unbound) | `inspector.zoom_fit` | `InspectorZoomFit` (1322) | 0 | Header button; default zoom |
| (unbound) | `inspector.zoom_1to1` | `InspectorZoom1to1` (1323) | 0 | Header button; one image pixel per device pixel |
| (unbound) | `inspector.play` | `InspectorPlay` (1324) | 0 | Play button on a video poster: opens the default player (Navigator extra) |
| (unbound) | `inspector.cycle_placement` | `CycleInspectorPlacement` (1325) | 0 | Auto, right, bottom; header menu |
| (unbound) | `inspector.toggle_grid` | `ToggleInspectorGrid` (1326) | 0 | Header button |
| (unbound) | `inspector.toggle_minimap` | `ToggleInspectorMinimap` (1327) | 0 | Header button |

### Focus scoping (Phase 6)

- The Inspector never takes keyboard focus by itself: it follows the list cursor, so arrow keys
  keep moving the list while it updates (design review).
- **Text fields** (address bar, GoTo, rename and name prompts, the Phase 5 filter field) consume
  `Space` and `Shift+Space` as text before the keymap, so a space typed into a filter never
  opens the Inspector. This extends the Phase 4 and Phase 5 field key lists.
- **List focused**: `Space` resolves through the keymap to `view.inspect`.
- **Inspector focused** (after a click inside it): the ui routes resolved commands to it, as the
  Phase 4 sidebar does:

  | Resolved command | Inspector effect |
  |------------------|------------------|
  | `list.cursor_up` / `_down` / `_page_up` / `_page_down` / `_home` / `_end` | Scroll text and folder previews; pan an image larger than the view |
  | `list.activate` (`Enter`, `KeypadEnter`) | `inspector.play` (open with the default application) |
  | `list.clear_selection` (`Escape`) | Focus back to the active pane |
  | `view.inspect` (`Space`) | Hide the Inspector; focus back to the active pane |

  Every other chord resolves as usual (`Ctrl+Q`, `F6`, `Ctrl+W`, ...).

### Mouse (Phase 6)

| Gesture | Effect |
|---------|--------|
| Click in the Inspector | Give it keyboard focus |
| Wheel over an image | Zoom about the pointer (between fit and 16:1) |
| `Ctrl`+wheel over text | Nothing in Phase 6 (font size is a Phase 9 setting) |
| Drag on an image larger than the view | Pan |
| Double-click on an image | Toggle `inspector.zoom_fit` / `inspector.zoom_1to1` |
| Click on the minimap | Centre the view there |
| Click on the play disc of a video poster | `inspector.play` |
| Click or double-click on a row of a folder preview | Nothing in Phase 6 (the preview never navigates) |
| Drag the Inspector's inner edge | Resize (`layout::kInspectorMin*` .. `kInspectorMax*` dp) |

## Later phases

- A user-configurable keymap file arrives in **Phase 8**. It binds chords (and multi-key
  sequences) to command names. Loading is lenient: unknown command names, malformed chords and
  duplicate bindings are reported and skipped, never fatal; the defaults above remain in effect
  for anything the file does not override.
- New commands append new `CommandId` values (ranges in `include/navigator/commands.hpp`) and add
  a row here.
- `Ctrl+N` (`window.new`) when a second window exists (multi-window phase).

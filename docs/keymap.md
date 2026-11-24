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

  Inspector-local keys (only while the Inspector has focus; not keymap rows, never start
  type-to-filter, rebinding arrives with the Phase 8 palette): `0` fit, `1` 1:1, `+` / `=` zoom
  in, `-` zoom out, `G` pixel grid, `M` minimap.

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

## Phase 7 defaults (file operations, trash, undo, drag and drop)

`docs/features.md` attributes only `Ctrl+Shift+N` (new folder) and "copy, cut, paste, delete,
rename" (without chords) to File Pilot. Every chord below except `Ctrl+Shift+N` is Navigator's
choice, following GNOME Files and Nemo: `Ctrl+C` / `Ctrl+X` / `Ctrl+V` clipboard, `Ctrl+Z` undo,
`Ctrl+Shift+Z` and `Ctrl+Y` redo, `Delete` to the trash, `Shift+Delete` permanent delete,
`F2` rename. Undo/redo is a Navigator extra (File Pilot has none today). None of these chords
was bound before (60 default chords through Phase 6); `Ctrl+Shift+N` had been reserved for this
since Phase 3 and `Ctrl+C` since Phase 4.

Copy / move to the other pane ("previous pane", `features.md`, chord not published): `F5` /
`F6` (the Total Commander and Midnight Commander keys) are taken by `nav.refresh` and
`pane.focus_next`, so Navigator uses **`F7` = copy to the other pane** and
**`Shift+F7` = move** (Shift = move, as when dragging). `Ctrl+F5` was rejected (browsers'
hard reload: refreshing by habit must not copy files), `Alt+F5`/`Alt+F7`/`Alt+F8` belong to
window managers, and `F8` stays free because Total Commander users read it as "delete". The
target is the previously active pane; with one pane the command is a no-op with a notice. Both
are undoable, so they do not confirm.

`load_default_keymap()` binds the Phase 0, 2, 3, 4, 5 and 6 tables, then this table, in this
order.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Ctrl+C` | `edit.copy` | `Copy` (1281) | 0 | Selection to the clipboard (GNOME/KDE formats, `transfer.hpp`) |
| `Ctrl+X` | `edit.cut` | `Cut` (1282) | 0 | Rows dimmed until pasted |
| `Ctrl+V` | `edit.paste` | `Paste` (1283) | 0 | Into the tab's folder; conflicts ask |
| `Ctrl+Z` | `edit.undo` | `Undo` (1284) | 0 | Navigator extra; never deletes (undo of a copy trashes it) |
| `Ctrl+Shift+Z` | `edit.redo` | `Redo` (1285) | 0 | |
| `Ctrl+Y` | `edit.redo` | `Redo` (1285) | 0 | |
| `Delete` | `file.trash` | `MoveToTrash` (1340) | 0 | No confirmation; notice with Undo. No trash on the volume: the permanent-delete sheet asks |
| `Shift+Delete` | `file.delete_permanently` | `DeletePermanently` (1341) | 0 | Always confirms (count, cannot be undone; focus starts on Cancel) |
| `F2` | `file.rename` | `Rename` (1342) | 0 | Inline editor, stem selected |
| `Ctrl+Shift+N` | `file.new_folder` | `NewFolder` (1343) | 0 | File Pilot behaviour per `features.md` |
| `F7` | `file.copy_to_other_pane` | `CopyToOtherPane` (1344) | 0 | Into the previously active pane's folder |
| `Shift+F7` | `file.move_to_other_pane` | `MoveToOtherPane` (1345) | 0 | |
| (unbound) | `file.open_terminal_here` | `OpenTerminalHere` (1346) | 0 | Reserved no-op until Phase 10 ("Open console here") |
| (unbound) | `ops.cancel` | `CancelOperation` (1347) | 0 | Stop button of the progress bar (arg: an operation id) |
| (unbound) | `ops.toggle_pause` | `TogglePauseOperation` (1348) | 0 | Pause button (arg: an operation id) |
| (unbound) | `ops.toggle_panel` | `ToggleOperationsPanel` (1349) | 0 | Status-bar operations button |
| (unbound) | `trash.open` | `OpenTrash` (1360) | 0 | Sidebar Trash row |
| (unbound) | `trash.restore` | `RestoreFromTrash` (1361) | 0 | Trash view toolbar and context menu |
| (unbound) | `trash.empty` | `EmptyTrash` (1362) | 0 | Trash view toolbar; always confirms |

`edit.paste` takes `PasteArg` flags (`IntoCursorFolder`: the row context menu's "Paste into
folder"); `ops.cancel` / `ops.toggle_pause` take an `ops::OpId` value from the progress bar or
panel (0: the most recently started unfinished operation).

In the Trash view `Delete` acts as `file.delete_permanently` (with its confirmation), `Enter`
restores the selected items to their original location (non-destructive; items are never opened from
the trash), and `edit.paste`, `file.rename`,
`file.new_folder` are no-ops.

### Focus scoping (Phase 7)

The keymap stays global; the ui scopes around it (safety: no file command may fire while
typing or act on a selection that does not have focus):

| Focus | Keys / commands |
|-------|-----------------|
| Any text field (address bar, GoTo, name prompts, filter, inline rename / new folder) | Also consumes `Shift+Delete` (as `Delete`), `Ctrl+Shift+Z` and `Ctrl+Y` (text redo, or nothing). Phase 4 fields already consume `Delete`, `Ctrl+C/X/V/Z`, `Backspace` |
| Phase 5 filter field | As in Phase 5: `Ctrl+V`, `Ctrl+Z`, `Delete` are the field's; `Ctrl+C` / `Ctrl+X` are the field's only while it has a text selection, otherwise they copy / cut the selected files |
| Inline rename / new-folder editor | A Phase 4 text field: `Enter` commits, `Escape` cancels (nothing renamed or created), focus loss commits the shown text (as GNOME Files). An invalid name (`ops::validate_new_name`) keeps the editor open with an inline message |
| Sidebar | `edit.*`, `file.*` and `trash.restore` are not routed to the pane (no-ops); `file.rename` on a bookmark row runs `bookmarks.rename` |
| Popups, confirmation and conflict sheets | Take the keyboard like Phase 3 popups; file commands are not routed |
| Inspector | File commands act on the active pane's selection as usual |
| Progress bar / operations panel | Never take keyboard focus (clicks only) |

Conflict sheet keys: `Enter` = Keep both (the safe default), `Escape` = Cancel operation (nothing
further is touched; items already completed stay done and undoable), `Tab` cycles the
buttons (Replace, Skip, Keep both, Merge for folders, Cancel operation), `Space` toggles "Apply
to all". Permanent-delete and empty-trash sheets: focus starts on Cancel; `Escape` cancels;
the destructive button needs `Tab` then `Enter`, or a click.

### Mouse and drag and drop (Phase 7)

Modifiers follow GNOME Files (`transfer::choose_drop_action`); the cursor badge shows the
resulting action and the item count with the first item's thumbnail.

| Gesture | Effect |
|---------|--------|
| Drag selected rows onto a folder row, a pane, a tab, a breadcrumb segment or a sidebar place/bookmark | Same filesystem: move; other filesystem: copy |
| ... with `Ctrl` | Copy |
| ... with `Shift` | Move |
| ... with `Ctrl+Shift` | Link here (symlinks) |
| ... with `Alt` | Drop menu: Copy here, Move here, Link here, Cancel |
| Drop onto the sidebar Trash row | `file.trash` for the dragged items |
| Drop from another program (SDL drop events) | Copy, unless `Shift` is held at drop time (move) |
| Drop back onto the source folder without a modifier | Nothing |
| `Escape` during a drag | Cancel the drag |
| Hover over a folder row while dragging for about 700 ms | Spring-loaded: the folder opens in that pane (off with `--no-animations`) |
| Drag near the top or bottom edge of a list | Auto-scroll, faster closer to the edge (`features.md`) |
| Wheel while dragging | Scrolls the list under the pointer (`features.md`) |
| Click on the progress bar's stop / pause buttons | `ops.cancel` / `ops.toggle_pause` for that operation |
| Click on "Undo" in the trash or operation notice | `edit.undo` |
| Right-click on a row | Context menu gains Cut, Copy, Paste into folder, Rename, Move to Trash, Delete permanently; Trash view: Restore, Delete permanently |
| Right-click on folder background | Paste, New folder (the full background menu is Phase 8) |
| Slow second click on the cursor row's name | Not a rename trigger in Phase 7 (avoids accidental renames); `F2` or the context menu |

## Phase 8 defaults (palette, settings, context menu, cursor selection)

`docs/features.md` attributes to File Pilot "Ctrl+Space toggles cursor selection" and, as
Navigator behaviour, `Ctrl+Shift+P` (palette) and `Ctrl+,` (settings). Every other chord below is
Navigator's choice. All eight chords were free (72 default chords through Phase 7; `Ctrl+Shift+P`,
`Ctrl+,` and `Ctrl+Space` had been reserved for this phase since Phase 3). `Ctrl+P` stays free.

`load_default_keymap()` binds the Phase 0, 2, 3, 4, 5, 6 and 7 tables, then this table, in this
order, then the default sequence below.

| Chord | Command name | CommandId | arg | Notes |
|-------|--------------|-----------|-----|-------|
| `Ctrl+Shift+P` | `palette.open` | `OpenPalette` (1370) | 0 | Global command palette. Works from every focus; inside the palette it closes it |
| `Ctrl+Comma` | `settings.open` | `OpenSettings` (1371) | 0 | `Ctrl+,`. Phase 8: the Settings overlay with the Keyboard page only |
| `Shift+F10` | `menu.show_context` | `ShowContextMenu` (1375) | 0 | Context menu of the focused element (Phase 6/7 handled it for tabs with a hardcoded key) |
| `Menu` | `menu.show_context` | `ShowContextMenu` (1375) | 0 | The context-menu key |
| `Ctrl+Space` | `list.toggle_cursor_selected` | `ToggleCursorSelected` (1103) | 0 | Toggle the cursor row in the selection (File Pilot behaviour per `features.md`) |
| `Ctrl+Up` | `list.cursor_up` | `CursorUp` (1110) | `KeepSelection` | Move the cursor only; selection and anchor stay |
| `Ctrl+Down` | `list.cursor_down` | `CursorDown` (1111) | `KeepSelection` | |
| `Ctrl+J` | `ops.toggle_panel` | `ToggleOperationsPanel` (1349) | 0 | Operations panel ("jobs"); a keyboard path to stop or pause an operation (docs/review/phase-7.md finding 10) |
| (unbound) | `keymap.edit` | `EditKeymap` (1372) | 0 | Bound by the default sequence below |
| (unbound) | `keymap.reset_defaults` | `ResetKeymap` (1373) | 0 | Destructive: confirmation sheet |
| (unbound) | `keymap.export` | `ExportKeymap` (1374) | 0 | Keymap JSON to the clipboard |
| (unbound) | `open_with.show` | `OpenWith` (1390) | 0 | Open With popup; also the row context menu's "Open With" submenu |

`KeepSelection` is `CursorArg::KeepSelection` (bit 1 of `Command::arg`); it wins over nothing
(`Extend` wins over it). `Ctrl+Space` is the input-method switch on some IBus / fcitx setups;
if the desktop takes it, rebind (the palette and menu reach the same command). `Ctrl+J` moved
`ops.toggle_panel` from the "(unbound)" list to a default chord.

### Default sequences (Phase 8)

Sequences are written as chords separated by one space. Exactly one ships; it exists so the
feature works out of the box and is covered by the end-to-end test.

| Sequence | Command name | CommandId | arg | Notes |
|----------|--------------|-----------|-----|-------|
| `Ctrl+K Ctrl+S` | `keymap.edit` | `EditKeymap` (1372) | 0 | "Keyboard shortcuts" (same as VS Code); also `keymap.edit` in the palette |

`Ctrl+K` is therefore a sequence prefix by default: it is not bindable on its own (see
"Conflicts and precedence"). A user who wants `Ctrl+K` for something else removes the sequence
with the keymap editor or an `unbound` entry.

### Key sequences

A binding may be a sequence of up to **4** chords (`kMaxSequenceLength`), e.g. `Ctrl+K Ctrl+S`.
The `SequenceMatcher` (`commands.hpp`) is a small state machine the ui feeds with the chords that
reach the keymap (after focus scoping; auto-repeat and modifier-only presses are not fed):

| State | Chord arrives | Result |
|-------|---------------|--------|
| idle | is not the first chord of any sequence | `NoMatch`: not consumed; resolved as an ordinary chord |
| idle | is the first chord of a sequence | `Pending`: consumed; the status bar shows `Ctrl+K ...` |
| pending | extends to a longer prefix | `Pending` |
| pending | completes a sequence | `Match`: consumed; the command is enqueued like any binding; idle |
| pending | `Escape` (no modifiers) | `Cancelled`: consumed, nothing runs; idle |
| pending | anything else, including a chord that is bound on its own (`Ctrl+Q`) | `Aborted`: consumed, nothing runs; idle. The status bar says `Ctrl+K Ctrl+Q is not a command` |
| pending | the timeout elapsed since the previous chord | the pending chords are dropped (`expired`); the chord is handled as if idle |

- **Timeout**: 1000 ms by default, measured from the previous chord (not from the first); a chord
  exactly 1000 ms later still counts. Configurable with `"sequence_timeout_ms"` in the keymap
  file, clamped to 200..5000. The status-bar hint disappears when it expires.
- **Prefix-free**: a chord cannot be a binding and the start of a longer one. Binding `Ctrl+K`
  while `Ctrl+K Ctrl+S` exists, or the reverse, is a conflict (below). There is deliberately no
  override.
- **Rules for sequences of two or more chords**: the first chord must not be a text-input chord
  (a printable key, with at most Shift: `G`, `Shift+5`, `Space`, `Keypad5`); this keeps
  type-to-filter intact (a user can never turn `g` into a prefix). No bare `Escape` and no mouse
  button inside a sequence. Use `Ctrl`, `Alt`, `Super`, function keys or navigation keys to
  start one.
- **Focus**: a chord a focused text field consumes (see the focus scoping sections) is never fed,
  so typing in a field cannot start or advance a sequence. The pending state is reset when focus
  moves, a popup, sheet or overlay opens, or the mouse is clicked.
- **Safety**: a sequence enqueues a command, nothing else: confirmation sheets are unaffected.
- **Turkish and other layouts**: sequences use the same keycode matching as chords, so
  `Ctrl+K Ctrl+S` is the key labelled K then S on Q, F and US layouts alike.

### Aliases

An alias is a short name typed in the palette that jumps to a command (and arg): `nf` for
`file.new_folder`. Aliases live in the keymap file.

- Name: 1..32 bytes of UTF-8, no whitespace or control characters, not starting with `>` or `/`;
  unique ASCII-case-insensitively.
- Typing exactly the alias ranks that command first (unless it is destructive, in which case a
  safe match is moved above it, see "Safe default"); typing a prefix of it ranks it high.
- The target is a command name, never another alias. The arg must be 0 for commands whose arg is
  a runtime id (`ops.cancel`, `tab.close`, `bookmarks.rename`, ...); flag args are fine
  (`edit.copy_path` with `1` = current folder).
- Aliases never skip anything: an alias to `file.delete_permanently` opens the permanent-delete
  sheet, an alias to `trash.empty` opens the empty-trash sheet.

### Keymap file

`<config>/navigator/keymap.json` (`~/.config/navigator/keymap.json`, `AppConfig::config_dir`).
It is the user's layer over the defaults, not a dump of them. Schema, with every field optional
except `keys` / `command` / `name`:

```json
{
  "version": 1,
  "sequence_timeout_ms": 1500,
  "bindings": [
    { "keys": "Ctrl+Alt+F", "command": "search.flatten" },
    { "keys": "Ctrl+K Ctrl+T", "command": "view.toggle_theme" },
    { "keys": "Keypad5", "command": "edit.copy_path", "arg": 1 }
  ],
  "unbound": [
    { "keys": "Ctrl+E", "command": "search.flatten" }
  ],
  "aliases": [
    { "name": "nf", "command": "file.new_folder" }
  ]
}
```

The example rebinds `Ctrl+E` (flatten) to `Ctrl+Alt+F`: the `unbound` entry removes the default
(only if `Ctrl+E` is bound to `search.flatten` by the default keymap; a stale entry is ignored),
the `bindings` entry adds the new chord. The editor writes both.

- **Lenient load**: unknown fields are ignored; an element that is not an object, whose `keys`
  does not parse, whose `command` is empty, whose `arg` is not a non-negative integer, whose alias
  name is invalid, or that duplicates an earlier entry is dropped and reported; the rest loads.
  A file that is not a JSON object is moved to `keymap.json.bad` and defaults are used. A file
  with a newer `version` is read best-effort and, on the next save, first copied to
  `keymap.json.bak`. The program never crashes on a bad file.
- **Unknown command names are kept** in the file (a plugin or a newer Navigator may know them);
  they are skipped when building the effective keymap.
- Saved atomically (temporary file, fsync, rename; directory 0700, file 0600) about 250 ms after
  each edit and on quit. `--no-session` (or `restore_session = false`) uses the defaults and
  `save_session = false` never writes.
- Key text is canonical (`Ctrl+Shift+L`, modifiers in the order Super, Ctrl, Alt, Shift); input
  accepts any case and the synonyms in `commands.hpp` (`Numpad5`, `ContextMenu`, `Esc`, ...).

### Conflicts and precedence

Two bindings conflict when their sequences are **identical**, or one is a **strict prefix** of
the other (`Ctrl+K` vs `Ctrl+K Ctrl+S`).

1. **User wins over defaults.** A user binding that conflicts with a default removes that default
   in the effective keymap; the displaced binding is reported (the editor shows "replaces
   Ctrl+E: Flatten Hierarchy"; `ApplyReport::DisplacedDefault`).
2. **Earlier user entry wins over a later one** in file order; the later entry is dropped and
   reported. The editor never produces such a file: it removes what it displaces.
3. In the editor, a conflict is shown before anything changes; "Replace" displaces the existing
   binding (a user binding is deleted from the file, a default gets an `unbound` entry), "Cancel"
   keeps both untouched.
4. Rebinding a default to its own chord again just deletes the `unbound` entry (the file does not
   grow). "Reset to default" for one command deletes its user bindings and `unbound` entries;
   "Reset all" (`keymap.reset_defaults`, destructive, with a confirmation sheet) clears bindings
   and `unbound`; aliases and the timeout stay.

### Warnings in the keymap editor

Allowed but flagged next to the shortcut field (`BindingWarning`):

- **Destructive command on a bare key**: `file.delete_permanently`, `trash.empty`,
  `layout.delete` or `keymap.reset_defaults` bound to a sequence where no chord has a modifier
  (`Delete`, `F8`). Rule: `is_destructive_bare_binding`. The shipped `Shift+Delete` counts as
  modified. Even then the confirmation sheet stands between the key and the data.
- **Shadows type to filter**: a bare printable key (`G`, `Shift+5`) as a single binding wins over
  type-to-filter while the list has focus, so typing that character no longer starts the filter.
- **Consumed by text fields**: the first chord is one text fields take (`Ctrl+A`, `Ctrl+C`,
  `Backspace`, `Home`, `Space`, ...): the binding does not fire while typing.
- **Layout-sensitive punctuation**: `Ctrl`/`Alt` with a punctuation key; on layouts where that
  character needs AltGr (Turkish, German, ...) it may not be reachable. `Ctrl+,` is shipped
  because `docs/features.md` lists it; on Turkish Q the key labelled `ö` sits at that position
  and matches through the scancode fallback.

Invalid bindings (unknown command, a reserved or unbindable command, a runtime-id arg other than
0, a sequence breaking the rules above) are refused with a reason. The sheets' own keys (`Tab`,
`Enter`, `Escape`, `Space`, `F2` inside confirmation, conflict and name prompts) are not resolved
through the keymap while those widgets are open, so remapping the commands they are bound to
outside never changes what a sheet does.

### Numpad and function keys (Phase 8)

- `Keypad0`..`Keypad9`, `KeypadPlus`, `KeypadMinus`, `KeypadMultiply`, `KeypadDivide`,
  `KeypadPeriod` and `KeypadEnter` are ordinary keys, matched by the physical keypad key whatever
  the NumLock state. `Numpad5` is accepted as input. They are text-input chords for the rules
  above (with NumLock they type digits into the filter).
- `Menu` is the context-menu key. `F13`..`F24` are accepted (`docs/features.md`). No default
  binds a keypad digit; `KeypadPlus` / `KeypadMinus` are kept free for the Phase 9 zoom.
- Not Phase 8: "separate shortcuts per zoom level" (`docs/features.md`, needs the Phase 9 views).

### Focus scoping (Phase 8)

| Focus | Palette, menu, sequences |
|-------|--------------------------|
| Any, including every text field | `Ctrl+Shift+P` opens the palette (a field does not consume it). Inside the palette it closes it |
| Palette open | Typing goes to its field; `Up`/`Down`/`PageUp`/`PageDown` move the highlight; `Enter` runs the highlighted entry; `Escape` closes; all other chords are the palette's (it does not resolve the keymap) except `Ctrl+Shift+P` |
| Context menu open | Typing goes to the menu's search field (no accelerator letters); `Up`/`Down` move; `Enter` runs the highlighted item; `Right`/`Left` open/close a submenu; `Escape` closes (clears the search first). The destructive item is never the Enter default |
| Confirmation and conflict sheets | Keep the keyboard (Phase 7). `Ctrl+Shift+P` is ignored while one is open, so nothing can bypass or stack on a sheet |
| Text fields | Consume what they consume (Phase 4-7 tables); sequences never start or advance inside them |
| Sequence pending | The next chord belongs to the sequence (see the state table) |

Palette and menu items run the same `Command` a binding would, at frame end, so every sheet
and rule above still applies. The palette captures its context (selection, focused sidebar row,
Trash view, running operations, undo state) before it takes focus.

## Phase 9 defaults

| Chord | Command | Arg | Behaviour |
|-------|---------|-----|-----------|
| `Ctrl+1` | `view.mode_details` | — | Switch active tab to Details view |
| `Ctrl+2` | `view.mode_icons` | — | Switch active tab to Icon Grid view |
| `Ctrl+3` | `view.mode_list` | — | Switch active tab to List/Columns view |
| `Ctrl+4` | `view.mode_tree` | — | Switch active tab to Tree view |
| `Ctrl+=` | `view.zoom_in` | — | Zoom in (Details -> Icons -> larger tiles) |
| `Ctrl+KeypadPlus` | `view.zoom_in` | — | Same (keypad) |
| `Ctrl+-` | `view.zoom_out` | — | Zoom out (smaller tiles -> Details) |
| `Ctrl+KeypadMinus` | `view.zoom_out` | — | Same (keypad) |
| `Ctrl+0` | `view.zoom_reset` | — | Reset zoom to 100% |
| `Ctrl+Keypad0` | `view.zoom_reset` | — | Same (keypad) |
| *(unbound)* | `view.cycle_mode` | — | Cycle Details -> Icons -> List -> Tree |
| *(unbound)* | `view.group_none` | — | Disable grouping |
| *(unbound)* | `view.group_date` | — | Group items by modification date |
| *(unbound)* | `view.group_type` | — | Group items by file type category |
| *(unbound)* | `view.group_size` | — | Group items by file size bucket |
| *(unbound)* | `view.group_cycle` | — | Cycle grouping (None -> Date -> Type -> Size) |
| *(unbound)* | `view.lock_folder` | — | Lock view settings to this folder |
| *(unbound)* | `view.lock_hierarchy` | — | Lock view settings to this folder and subfolders |
| *(unbound)* | `view.lock_global` | — | Use global view settings (clear folder lock) |
| *(unbound)* | `view.tree_expand` | — | Expand cursor folder in Tree view |
| *(unbound)* | `view.tree_collapse` | — | Collapse cursor folder (or move to parent) in Tree view |
| *(unbound)* | `view.tree_collapse_all` | — | Collapse all expanded folders in Tree view |

## Later phases

- A user-configurable keymap file landed in **Phase 8** (see above). Loading is lenient: unknown
  fields are ignored, invalid entries are reported and dropped, the defaults stay in effect for
  anything the file does not override.
- New commands append new `CommandId` values (ranges in `include/navigator/commands.hpp`) and add
  a row here.
- `Ctrl+N` (`window.new`) when a second window exists (multi-window phase).

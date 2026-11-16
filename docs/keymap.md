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
| (unbound) | `nav.focus_address_bar` | `FocusAddressBar` (1004) | 0 | Reserved for Phase 4; registered, no-op |

`Extend` is `CursorArg::Extend` (bit 0 of `Command::arg`).

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
(flatten, Phase 5), `Ctrl+B` (sidebar, Phase 4), `Ctrl+Shift+P` (palette, Phase 8), `Ctrl+,`
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

## Later phases

- A user-configurable keymap file arrives in **Phase 8**. It binds chords (and multi-key
  sequences) to command names. Loading is lenient: unknown command names, malformed chords and
  duplicate bindings are reported and skipped, never fatal; the defaults above remain in effect
  for anything the file does not override.
- New commands append new `CommandId` values (ranges in `include/navigator/commands.hpp`) and add
  a row here.
- `Ctrl+N` (`window.new`) when a second window exists (multi-window phase).

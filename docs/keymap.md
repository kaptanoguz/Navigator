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

Open question for Phase 3: `docs/features.md` lists `Ctrl`+click / `Ctrl`+double-click as "open
in new tab" (Phase 3), which collides with Phase 2's `Ctrl`+click multi-select (required by the
roadmap). Phase 2 ships `Ctrl`+click = toggle selection; Phase 3 must decide (for example
`Ctrl`+double-click only, or middle click) and update this table.

## Later phases

- A user-configurable keymap file arrives in **Phase 8**. It binds chords (and multi-key
  sequences) to command names. Loading is lenient: unknown command names, malformed chords and
  duplicate bindings are reported and skipped, never fatal; the defaults above remain in effect
  for anything the file does not override.
- New commands append new `CommandId` values (1000+ reserved for later phases) and add a row here.

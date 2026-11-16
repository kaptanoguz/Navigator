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

`Ctrl` is the primary modifier on Linux. Whether chords match by keycode (layout-aware) or
scancode (physical position) is decided by the ui layer; Turkish Q and F layouts must both work.

## Later phases

- A user-configurable keymap file arrives in **Phase 8**. It binds chords (and multi-key
  sequences) to command names. Loading is lenient: unknown command names, malformed chords and
  duplicate bindings are reported and skipped, never fatal; the defaults above remain in effect
  for anything the file does not override.
- New commands append new `CommandId` values (1000+ reserved for later phases) and add a row here.

# Drag and drop (Phase 7)

Owner: ui. Normative design: `docs/architecture.md` "Clipboard and drag and drop"; keys and
modifiers: `docs/keymap.md` "Mouse and drag and drop (Phase 7)".

## What exists

| Direction | Mechanism | Where |
|-----------|-----------|-------|
| Between panes, tabs, breadcrumb segments, folder rows, sidebar places, the Trash row | internal state machine (`FileDrag`, pure, tested) | `src/ui/dnd/file_drag.*`, `src/ui/shell/app.cpp` |
| Into Navigator from other programs | SDL3 `SDL_EVENT_DROP_BEGIN / POSITION / FILE / TEXT / COMPLETE` (`IncomingDrop`) | `src/ui/dnd/file_drag.*`, `handle_drop_event` |
| Out of Navigator to other programs, X11 | XDND source (protocol 5) on SDL's `Display*` / `Window` | `src/ui/platform/xdnd_source.cpp` |
| Out of Navigator, Wayland | attempt: second `wl_seat` / `wl_pointer` / `wl_data_device` on a private queue of SDL's `wl_display` | `src/ui/platform/wayland_drag.cpp` |
| Out of Navigator, fallback | `Ctrl+C` then paste in the other program (clipboard formats of `transfer.hpp`) | `FileOps::copy`, `SdlClipboard` |

Modifiers: `Ctrl` copy, `Shift` move, `Ctrl+Shift` link, `Alt` ask (drop menu); no modifier moves
within a filesystem and copies across filesystems; dropping back on the source folder does
nothing (`transfer::choose_drop_action`). Incoming drops copy unless `Shift` is held. The badge
shows the item count, the first item and the resulting action; the hovered folder row, pane, tab,
breadcrumb or sidebar row is highlighted (red when the drop is not allowed). Near the top or
bottom edge of a list the list scrolls (faster closer to the edge); the wheel scrolls the list
under the pointer. A folder row hovered for 700 ms opens in its pane (spring-loaded, off with
`--no-animations`). `Escape` cancels. Dropping on the sidebar Trash row is `file.trash`.

Outgoing drags only ever offer a COPY (`XdndActionCopy`). Navigator never deletes or moves a file
on the strength of another program's answer; a "move" requested by the receiver is a copy.
`text/uri-list`, `x-special/gnome-copied-files` and `text/plain` are offered.

## What was verified here, and how

All with fixtures under the scratch directory, a temporary `HOME`, `--config-dir`, `--cache-dir`
and `--trash-dir`, on a private Xvfb. Never on a real display.

* Internal drags, real pointer (xdotool) under Xvfb: a row dropped on a folder row of the other
  pane moves; `Ctrl` held at release copies; spring-loading opened the hovered folder after
  700 ms. Pure logic (slot priority, action choice for every modifier, same/other filesystem,
  into-itself, read-only, spring timing, cancel, incoming session) is covered by
  `tests/ui/file_drag_test.cpp`.
* Outgoing X11, protocol level (`ctest -R ui.smoke_outgoing_drag_xdnd`): a real xdotool drag from
  a Navigator row to a separate X11 window that speaks XDND (`tests/ui/xdnd_receiver.c`, our own
  Xlib program). The receiver sees XdndEnter, XdndPosition, XdndDrop, converts the XdndSelection
  and writes the `text/uri-list`: one file, two files (names percent-encoded, Turkish letters
  included), and "released outside any window delivers nothing"; the source files stay
  untouched.
* Clipboard formats round trip through SDL (`tests/ui/sdl_clipboard_test.cpp`, SDL dummy driver)
  and through the Nautilus / Nemo / Dolphin shapes (`tests/ui/file_ops_test.cpp`).

## What was NOT verified (needs the owner's manual check)

* A drag to **GNOME Files / Nemo / the desktop** on a real session. The XDND source follows the
  protocol and works against our test receiver; real file managers may differ in details (for
  example they may ask for `x-special/gnome-copied-files`, which is offered, or require
  XdndActionAsk). Steps (X11 session): start `navigator`, select a file, drag it onto an open
  Files / Nemo window and release; the file must appear there as a copy.
* **Wayland**: `wayland_drag.cpp` compiles and is selected on a Wayland session, but it was not
  run against a compositor: no input device exists in a headless weston, so no button serial can
  be produced. Whether Mutter / Muffin accept `wl_data_device.start_drag` from a second pointer
  object of the same client is exactly the open question of the roadmap. Steps (Wayland session):
  run `NAVIGATOR_DEBUG_DND=1 navigator`, drag a row out of the window towards Files; the log
  prints `outgoing drag started` or `refused (reason)`. When it is refused, or the backend could
  not be created ("outgoing drag: not available" at startup), the badge shows "Ctrl+C, then
  paste in the other app" and the clipboard path works. No portal offers drag and drop to
  unsandboxed apps.
* Incoming drops from other programs were not exercised with a real XDND/Wayland source; the
  SDL event handling is unit-tested and shared with the verified internal path. Steps: drag a
  file from Files onto a Navigator pane (copy), with `Shift` (move), with `Alt` (menu).
* Archives (XdndDirectSave) are not supported by SDL3 (Phase 10).

## Known behaviours

* `Ctrl+Shift+N` creates the folder and opens the inline editor on it (Escape keeps the
  default-named folder); `docs/architecture.md` describes a placeholder row instead.
* Modifiers are sampled at the button release, so releasing `Ctrl` a moment later does not turn
  a copy into a move.

# QA: Phase 7 findings (file operations, trash, undo, drag and drop)

Owner: qa. This is the first phase that mutates files, so the tests are dangerous too. The rules
that every test and script below obeys:

- Only `mkdtemp` fixtures (temp dir, or `/dev/shm` for a second filesystem). `tests/integration/
  ops_support.hpp` ABORTS the process when a path handed to the engine, the trash, an I/O hook or
  a request lies outside a live fixture, and when any engine path lies inside a "forbidden"
  symlink-sentinel tree (a symlink was followed).
- The trash is `trash::create_trash()` with `home_trash` inside the fixture and a fake mount table
  (wrapped in `GuardedTrash`); never `~/.local/share/Trash`.
- App runs use a temporary `HOME`, `XDG_DATA_HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME` and
  explicit `--config-dir`, `--cache-dir`, `--trash-dir`; after each run the temporary HOME and
  XDG data dir are asserted EMPTY and no trash may exist outside `--trash-dir`. SDL dummy driver
  or a private Xvfb, never display `:0`.

## Suites

| Suite / test (ctest) | What it checks |
|---|---|
| `integration.ops.*` (`ops_trash_e2e_test.cpp`) | The engine on the REAL platform trash: trash -> list -> restore with Turkish / space / `%` names, links and trees; restore recreates parents and undo rmdirs them only while empty; our `.trashinfo` bytes and trash file names equal `gio trash` output (GIO_USE_VFS=local, fixture HOME / XDG_DATA_HOME); collisions (`name.2` ...), names near NAME_MAX; undo of a copy goes to the trash, redo (the undo's own record) copies again; Replace goes through the trash, undo restores; without a trash a file is replaced atomically with a lossy record and a folder is never replaced permanently; restore and undo conflicts (Skip, Keep both, Cancel, Replace); untrashable volume (Unsupported listed, never deleted, `confirmed` required, queue rejects); cross-device refusal and per-volume `.Trash-$uid` on `/dev/shm`; the trash is emptied only by an explicit confirmed op; a stale listing never restores another item. |
| `integration.ops.*` (`ops_fault_test.cpp`) | Cancel storms (60 seeds x entry counter / write counter / timer thread) for copy, same-fs move, cross-fs move through the EXDEV seam: no `.part`, no truncated file, source checksum unchanged, every item whole at exactly one place, completed items undoable. Injected ENOSPC / EIO / EACCES at write, fsync, create, commit, mkdir (a fault inside a folder leaves its item `Done` with `partial`). NoSpace before writing (S14). Real read-only destination. REAL ENOSPC on a 1 MiB tmpfs in a private user+mount namespace (skips when unsharing is not allowed). Symlink traps (sentinel in another fixture, forbid guard): copy, move (also EXDEV), trash, restore, undo, redo, confirmed delete, and a folder swapped for a link mid-run. Hard links kept, FIFOs recreated without opening (30 s watchdog), sockets refused. Tree deeper than PATH_MAX. Protected paths (fake home, ancestor, mount point, trash) refused for Move / Rename / Trash / Delete even when confirmed, via `run_operation` and `submit`; copying them works. Copy into itself also through a link. 100k small files (20k under sanitizers) copy / undo / restore / trash / delete timings. Overlapping queue operations vs. a sequential twin with a hammer thread (TSan target). Queue cancel storms and shutdown mid-copy. |
| `integration.*` keymap cases (`keymap_e2e_test.cpp`) | 7 tables, 3 + 25 + 21 + 5 + 4 + 2 + 12 = 72 bound rows; 28 `(unbound)` rows (+7: `file.open_terminal_here`, `ops.cancel`, `ops.toggle_pause`, `ops.toggle_panel`, `trash.open`, `trash.restore`, `trash.empty`); near misses `Alt+F7`, `Alt+F8`, `F8`, `Ctrl+Delete`, `Ctrl+Shift+Y` ... free, `Ctrl+C` and `Ctrl+Shift+N` bound. Effect checks through a headless `FileOps` over the real fixture trash: Ctrl+C / Ctrl+X formats, Ctrl+V (conflict sheet, Keep both, cut clears the clipboard, paste into folder), Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, Delete (trash + Undo notice), Shift+Delete (sheet on Cancel; nothing deleted until the accept; not undoable), F2 (stem selected, invalid / taken names refused, Turkish names), Ctrl+Shift+N (unique names, undo), F7 / Shift+F7. Text fields consume Ctrl+C/X/V/Z/Y, Ctrl+Shift+Z, Delete, Shift+Delete (`apply_field_key`, `route_filter_key`). |
| `integration.app_ops_adversarial` (`ops_app_smoke.sh`) | The real binary, 51 runs: copy / cut-paste / F7 / Shift+F7 between two panes; **a move between two panes comes back with Undo** (acceptance); undo of a copy trashes it, redo; **Delete -> Trash view -> restore** (acceptance; Enter and `trash.restore`); Delete in the Trash view and `trash.empty` confirm with focus on Cancel; conflict sheet (Enter = Keep both, Escape cancels and touches nothing, Replace via the trash and undo, Skip all, unanswered sheet = untouched); permanent delete refused without the accept (Escape, Enter on Cancel, `confirm:no`, run ends with the sheet open, stray keys); a run without `--trash-dir` never trashes and never creates a trash, and Delete there only opens the permanent-delete sheet; quit while an operation runs asks (default keeps working, Escape too, cancel-and-quit leaves whole files and no `.part`); focus scoping (address bar, filter, rename editor, sidebar). Known open bugs print `XFAIL`; `NAVIGATOR_QA_STRICT=1` makes them failures. |
| `integration.app_dnd_incoming_xdnd` (`dnd_incoming_smoke.sh`, `xdnd_sender.c`) | INCOMING drops at protocol level under a private Xvfb: our own Xlib XDND source (protocol 5, `text/uri-list`) drops a Turkish-named file on the real window: it is accepted, copied into the active folder, the source stays; dropping a name that exists never overwrites. |
| `ui.smoke_outgoing_drag_xdnd` (ui's test, run as a gate) | Outgoing XDND to a separate X11 receiver. |

## Findings

- **BUG-P7-1** (ui, low to medium, FIXED in `471e11c`, verified by `ops_app_smoke.sh` as a plain
  check, 0 XFAIL): `F2` pressed
  inside the open inline rename editor restarts the editor and discards the typed text. Text
  fields do not consume `F2` (`apply_field_key` has no `F2` case), so `file.rename` runs
  `begin_inline_rename()` again. Repro: `navigator --lang en --trash-dir <fixture> --debug-script
  "rename:gamma.txt;type:zzz;key:F2;key:Enter;waitops" <dir>`: `gamma.txt` is not renamed (without
  the `key:F2` it becomes `zzz.txt`). The script keeps an `xfail` helper for future open bugs.
- Behaviour that differs from the brief's wording, not a bug: text fields consume Ctrl+C/X/V/Z/Y,
  Ctrl+Shift+Z, Delete and Shift+Delete, but NOT `F2`, `Ctrl+Shift+N`, `F7`, `Shift+F7`. For the
  address bar, popups and the sidebar `file_command` ignores them (focus scoping); in the Phase 5
  filter field they pass through (`F2` starts a rename of the cursor row), and `Ctrl+C` / `Ctrl+X`
  without a text selection copy / cut the files by design (documented in `docs/keymap.md`).
- Contract notes: a folder with failing entries is `Done` + `partial` and is not counted in
  `OpResult::failed`; an Undo's result record is the REDO record (use it with `OpKind::Redo`);
  `FileId` identity can be fooled by inode reuse in a stale trash listing (the test keeps the old
  inode alive); the default new-folder name follows the language ("Yeni klasör" under a Turkish
  locale), so scripts pass `--lang en`; a failing directory fsync is deliberately best effort.

## Results

Measured on the development machine (Linux 6.8, GCC 13.3, 8 threads; the machine was shared with
other builds, so timings are loose). Scratch build dirs only. Tree: `471e11c` plus the qa files.

| Run | Result |
|---|---|
| GCC Debug `-Werror`, `ctest -R '^integration\.ops\.\|keymap\|chord'` | 50/50 pass (100k-file case 55 s: copy, undo, restore, trash, delete) |
| Same new + keymap tests, `--repeat until-fail:10`, `-j4` | 35 tests x 10 clean passes |
| ALL `integration.*`, `-j8 --repeat until-fail:5` | 115 tests x 5 clean passes (includes the app scripts and the incoming-XDND test) |
| ASan + UBSan (GCC Debug, `NAVIGATOR_SANITIZE=ON`), `integration.ops.*` + keymap cases (app targets off) | 34/34 pass, 0 reports (real-ENOSPC namespace case ran) |
| TSan (GCC RelWithDebInfo, `setarch -R`, `halt_on_error=1`), same set | 34/34 pass, 0 `WARNING: ThreadSanitizer` (the namespace case reports "not run under ThreadSanitizer": `unshare(CLONE_NEWUSER)` needs one thread) |
| `ops_app_smoke.sh` | 51 app runs, all checks pass, 0 known bugs |
| `integration.app_dnd_incoming_xdnd` (new, own Xlib XDND source, private Xvfb) | passes (7 s) |
| `ui.smoke_outgoing_drag_xdnd` | passes (19 s) |
| clang-format-18 dry run, clang-tidy-18 on `ops_trash_e2e_test.cpp`, `ops_fault_test.cpp`, `keymap_e2e_test.cpp` | clean |

Not run here: the Wayland and GNOME Files / Nemo checks below (manual).

## Manual checks for the owner (drag and drop to other programs)

Nothing below could be run in CI: SDL 3.4 has no drag-source API, so Navigator's own XDND /
Wayland source can only be proven against a real file manager. Use throw-away folders only (for
example `~/nav-dnd-test/` with three small files, one named `şarkı ğ.txt`). Record the result for
each step in the phase summary.

**X11 session (log in with "Ubuntu on Xorg" / "Cinnamon" on X11)**

1. `NAVIGATOR_DEBUG_DND=1 ./build/dev/src/app/navigator ~/nav-dnd-test 2> /tmp/nav-dnd.log`
2. Open GNOME Files (Nautilus) or Nemo on a second throw-away folder `~/nav-dnd-target/`.
3. Drag `şarkı ğ.txt` from Navigator onto the Files window and release. Expected: the file
   appears there as a COPY (the source stays); the log says the drag started. Repeat with two
   files selected, and with the Files window on the desktop background (Mint: the desktop
   icons).
4. Drag a file onto the desktop background (Nemo desktop / GNOME Shell with the desktop-icons
   extension): expected a copy on the desktop, or "refused" in the log with the clipboard fallback
   noted.
5. Release outside any window: nothing happens and no file moves.
6. Press `Escape` during a drag: it cancels.

**Wayland session (GNOME on Wayland, Mint with Wayland if available)**

1. `NAVIGATOR_DEBUG_DND=1 SDL_VIDEODRIVER=wayland ./build/dev/src/app/navigator ~/nav-dnd-test`
   (check the first log line says `video driver 'wayland'`).
2. Start a drag from a row towards an open Files window. Read the log: it prints either
   `outgoing drag started` or `refused (reason)`; "outgoing drag: not available" at startup means
   the backend was not created.
3. If started: release over Files and check a copy arrives, and the same over the desktop. If
   refused: the drag badge must say "Ctrl+C, then paste in the other app"; then `Ctrl+C` in
   Navigator and `Ctrl+V` in Files / Nemo / the desktop: the file must be pasted as a copy
   (this path is the always-available one). Cut (`Ctrl+X`) in Navigator then paste in Files must
   MOVE it.
4. Note the compositor (Mutter / Muffin) version and the exact `refused (reason)` text for
   `docs/platform-limitations.md`; this is the roadmap's open question (whether a second
   `wl_pointer` of the same client may start a `wl_data_device` drag).

**Incoming drops from real programs (both sessions)**

1. Drag a file from Files / Nemo onto a Navigator pane: it is copied into that pane's folder.
2. With `Shift` held at the drop: moved (the source disappears from Files).
3. Drop onto a folder row, a tab, a breadcrumb segment and a sidebar place: copied into that
   folder. Drop a name that already exists: the conflict sheet opens and nothing is overwritten
   until you choose.
4. Drag an archive entry out of File Roller: XdndDirectSave is not supported (Phase 10); expect
   nothing or a temp path, and no crash.

**Clipboard interoperability (the supported route)**

`Ctrl+C` in Navigator, `Ctrl+V` in Nautilus / Nemo / Dolphin and vice versa, with a Turkish file
name, two files and a folder; `Ctrl+X` then paste elsewhere moves. Paste in a text editor must
paste the paths as text only.

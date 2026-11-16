# QA: Phase 2 browser findings

Owner: qa. Phase 2 is the single-pane browser. Suites:

- `navigator_browser_integration_tests` (`tests/integration/browser_race_test.cpp`,
  `keymap_e2e_test.cpp`, helpers in `browser_support.hpp`). The ctest prefix is `integration.`
  and the label is `integration`. It links `navigator_ui_browser` only, so it needs no
  SDL and opens no window.
- `integration.app_headless_start_paths` (`tests/integration/app_smoke.sh`).

The gates are in `docs/qa-gates.md`, "Phase 2 additions".

Measured on 2026-11-16 on the development machine: Linux 6.8, GCC 13.3, ext4 `/tmp`, 8 cores,
default inotify limits. Every build dir was a scratch dir, not `./build`.

## Tests

| Test (prefix `integration.`) | What it checks |
|---|---|
| `Back-to-back navigations commit only the newest request` | 60 rounds. Each round sends 1 to 5 `navigate_to` calls to distinct folders, with `frame_start()` called between some of them, so older results may already be in the mailbox. Only the last target commits. A late stale result never moves the browser. History grows by 1 to burst entries, forward is empty, and the invariants hold. |
| `Rapid navigation while a writer mutates the folders` | For 1.5 s, a writer thread creates, truncates, appends, unlinks, renames, moves across folders, and runs mkdir/rmdir/rename on folders in 4 folders. The test thread meanwhile sends random navigate/back/forward/up/refresh/hidden/sort/cursor/click/select-all/activate (files only, fake launcher) requests, with or without `frame_start`. The invariants are checked after every step. The browser never leaves the fixture. After the writer stops, the list must equal a fresh listing. Typical run: about 13k steps and 6k writer ops. |
| `Watch-driven updates match a fresh listing after a mutation storm` | Runs 4 configurations: hidden off, hidden on, 15% of the poll results replaced by Overflow (`LossyWatcher`), and `max_incremental_changes = 3` (most batches re-list). The watch is in place before the reference listing. For 1.5 s the writer churns the watched folder and an unwatched sibling, so moves arrive as MovedFrom and MovedTo. The UI thread keeps moving the cursor and Ctrl-clicking. Afterwards the rows (name, kind, size) and the hidden count must equal a fresh listing, with no notice. |
| `Selection, cursor and anchor survive re-lists and watch updates by name` | The selection and cursor are set first. They must survive each of these, with no index drift: Refresh, a watch create that sorts first, a delete, a rename of an unselected entry, sort by Size, sort by Name descending, hidden on/off, an Overflow re-list, and 25 refreshes during a noise storm. A deleted selected entry leaves the selection. |
| `Deleting the current folder raises a notice and the browser stays` | `rm -rf` of the shown folder gives a FolderGone notice. `current_dir` is unchanged. Refresh then gives RefreshFailed / NotFound and the browser stays. Up goes to the parent and clears the notice. Back into the deleted folder fails with OpenFolderFailed and the history is unchanged. |
| `A folder deleted while its navigation is pending fails cleanly` | 50 times, a navigation races an rmdir of its target. Either it commits an empty folder or it fails with OpenFolderFailed and stays put. The invariants hold and the end state matches a fresh listing. |
| `docs/keymap.md and load_default_keymap agree row for row` | Parses the Phase 0 and Phase 2 tables of the doc at test time. 28 chord rows must equal `Keymap::bindings()` in order (chord, command, arg). Every chord in the doc is canonical `to_string` text. |
| `Every default chord resolves to its command and has its effect on the browser` | Each row is resolved with `parse_key_chord`, `resolve_command`, then `BrowserModel::execute` on a 33-entry fixture, and its effect is checked. Enter and KeypadEnter: a file goes to the recording launcher, a folder is entered. Alt+Left and MouseBack: back. Alt+Right and MouseForward: forward. Alt+Up and Backspace: Up, with the cursor on the folder we came from. F5 and Ctrl+R: re-list, cursor and selection kept. Ctrl+A: select all, files' bytes only. Escape: clear, cursor stays. Ctrl+H: hidden on/off. Up, Down, PageUp, PageDown, Home and End, with and without Shift: cursor row, selection, anchor. Ctrl+Q, F12 and Ctrl+Shift+L: `execute` returns false, nothing changes. Every registered command except `nav.focus_address_bar` has a chord. Near misses (Ctrl+Shift+A, Ctrl+Enter, Alt+Shift+Left, Delete) resolve to nothing. |
| `Activate through the keymap never spawns a process` | Enter and KeypadEnter on 3 files make 6 recorded `xdg-open` requests. `waitpid(-1, WNOHANG)` reports ECHILD, so there is no child, running or zombie. |
| `app_headless_start_paths` | `navigator <dir> --frames 120` on `SDL_VIDEODRIVER=dummy`, `SDL_RENDER_DRIVER=software`, for 3 start paths: a fixture with 200 files, a non-ASCII folder and a hidden file; a `chmod 000` folder; a missing path. Each must exit 0, print the dummy-driver startup line, and print no sanitizer report. |
| `BUG-P2-1` … `BUG-P2-6` | `[!mayfail]` repros of the bugs below. |

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^integration\.'` (Phase 1 and Phase 2) | all pass; the 6 BUG cases fail as expected |
| `ctest -R '^integration\.' --repeat until-fail:10` (serial) | 9 clean passes, then the rapid-navigation test failed once: BUG-P2-4 fired naturally (see below). The test now waits for the last request before its final check. Final runs: 10 clean serial passes (all 24 `integration.` tests), and 3 more after the last test change. |
| `-j8 --repeat until-fail:5`, all `integration.` | first try: the selection test failed once (BUG-P2-6). Second try: `A folder deleted while its navigation is pending` failed once, showing a removed `doomed` (BUG-P2-2 fired naturally); that test now re-lists before its final comparison. Final run: 5 clean passes, 24/24. |
| ASan + UBSan (`NAVIGATOR_SANITIZE=ON`), `--repeat until-fail:3` serial and `-j4 --repeat until-fail:2`, including the app script | 24/24, no report |
| TSan (GCC 13, RelWithDebInfo, `setarch -R`, `halt_on_error=1`), `--repeat until-fail:3` | all browser and Phase 1 tests pass, 0 `WARNING: ThreadSanitizer`. The app script is not run there (the `navigator` target is not built in the TSan dir, and SDL is not instrumented) |
| clang-format-18 `--dry-run -Werror`, clang-tidy-18 on the new files | clean |

Timings (GCC Debug): the browser suite takes about 15 s per pass, mostly the 4 storm
configurations (7.8 s together). The app script takes about 6 s for 3 × 120 frames.

Launcher safety: the default launcher is built only in two places. One is
`tests/platform/launcher_test.cpp`, which runs `true`, the probe binary and missing programs,
never `xdg-open`. The other is `tests/contracts/phase2_usage.cpp`, which is compiled but never
linked. The one-argument `open_with_default(path)` is called only in `tests/platform/desktop_test.cpp`,
with a relative path and a missing path, which fail validation before anything is spawned. Every
`BrowserModel` in `tests/ui` that activates a file has a `FakeLauncher`. The others activate only
folders. Every browser model in `tests/integration` that activates anything gets a
`NoSpawnLauncher`.

Gap: `BrowserModel::Options::launcher == nullptr` silently falls back to the process-wide default
launcher. A future test that activates a file without a fake would run `xdg-open` on the
developer's desktop. The app's `--debug-script cmd:list.activate` path has the same risk. Possible
fix (ui): give tests a way to make a missing launcher an error, for example a `NAVIGATOR_TEST`
compile definition or a required launcher in `Options`.

The unreadable start path exits 0 and shows the empty state "this folder cannot be shown / no
permission" (checked with a `--screenshot` run). The app writes no log line for notices, so the
script checks only the exit code and the startup line.

## Bugs (owner: ui, `src/ui/browser`)

Each bug has a deterministic `[!mayfail]` test in `tests/integration/browser_race_test.cpp`.
When a fix lands, remove the tag.

### BUG-P2-1: changes right after entering a folder are lost (watch added after the listing)

`request()` calls `WatchWorker::retarget()` and then `ListingWorker::request()`. The watch worker
only notices the new target when its current `IWatcher::wait(kPollInterval)` returns, which can
take up to 100 ms. Listing a small folder takes about 1 ms. A file created, deleted or renamed in
that window appears in neither the listing nor any watch batch, and the list stays wrong until a
manual Refresh.

Repro: `GatedWatcher` holds `add()`. Navigate to `b`, settle (the listing has committed), touch
`b/during-gap`, release the gate, quiesce. The entry never appears.

Fix idea: list only after the watch for the target is in place. One way is for the watch worker
to request the listing, or to post a "ready" marker that triggers the listing. Another is to
re-list once when the watch becomes ready after the listing committed. Waking the worker from
`wait()` on retarget, for example with an eventfd, shortens the window but does not close it.

### BUG-P2-2: a failed navigation loses changes made meanwhile in the current folder

A navigation moves the watch to the target before the listing. `fail_request()` moves it back
(`retarget(current_dir)`) but does not re-list, so changes to the current folder while the watch
was away are lost.

Also seen naturally under `-j8`: navigating into a folder that is rmdir'd during the listing leaves the parent showing the removed folder.

Repro: watch on `t`, `navigate_to(t/locked)` (mode 000), `wait_watch_ready`, touch
`t/made-while-away`, settle (the navigation fails), quiesce. The entry is missing.

Fix idea: after moving the watch back, request a `Relist` of the current folder. Alternatively,
keep the old watch until the new listing succeeds.

### BUG-P2-3: after the folder is deleted and recreated, Refresh never watches it again

`WatchedDirGone` invalidates the worker's handle. `refresh()` of the same path keeps
`watch_tag_` (`same_dir && !watch_moved`) and never calls `retarget()`. After
`rm -rf build && mkdir build` and F5, the list is correct once, but live updates have stopped
until the user leaves the folder.

Repro: the test of the same name. After the recreate and refresh, a touched `live.o` never
appears within 2 s.

Fix idea: once the model has seen `gone` for the current tag, the next request for the same
folder retargets with a new tag. Alternatively, the worker re-adds the watch on the next
retarget, even with an unchanged path.

### BUG-P2-4: `navigate_to(current folder)` while another navigation is pending goes to the other folder

`navigate_to(current_dir)` turns into `refresh()`, and `request()` folds a Refresh into the
pending navigation. The older request (go to `b`) commits and the newer one (stay in `a`) is
lost. This breaks the "newest request wins" rule in docs/architecture.md.

Repro: in `a`, `navigate_to(b)`, then `navigate_to(a)`, then settle. `current_dir` is `b`. It
also fired naturally in 1 of 10 runs of the rapid-navigation storm, when the final
`navigate_to(d2)` came while a navigation away from `d2` was pending. Phase 2 has no UI path
that sends `navigate_to(current)` during a pending navigation, so the user-facing severity is
low today. The address bar (Phase 4) and the debug script `go:` will hit it.

Fix idea: in `navigate_to`, compare against the pending target too. When a navigation is pending
and the target is `current_dir`, treat it as a new Push/Refresh request, not a merge.

### BUG-P2-5: a sort or hidden toggle during a navigation that fails is never applied

`request()` folds a Relist (`sort_by`, `toggle_hidden`) into a pending navigation. If that
navigation fails, `fail_request()` keeps the old entries while `sort()` and `show_hidden()` have
already changed. The header arrow and the hidden toggle then contradict the list until the next
re-list. Hidden entries can show while hidden items are "off". Later watch inserts binary-search
a list that is not in `sort()` order, so rows land in wrong positions. The storm test found this
when it activated a folder that the writer had just removed.

Repro: in `t`, `navigate_to(t/missing)`, `toggle_hidden()`, `sort_by(Size)`, settle. Then
`show_hidden()` is true but `.hidden` is not listed, and the order is not Size-descending.

Fix idea: in `fail_request()`, when the failed request carried new options (or always for
Push/Back/Forward/Up), request a `Relist` of the current folder.

### BUG-P2-6: a rename that arrives as MovedFrom + MovedTo drops selection and cursor

The kernel queues IN_MOVED_FROM and IN_MOVED_TO one after the other. If a `read()` falls between
them, the inotify backend reports an unpaired MovedFrom and later a MovedTo. watch.hpp allows
this: an unpaired MovedFrom is never held for a later `poll()`. The watch worker turns the pair
into `Remove(old)` and `Upsert(new)`, and the model loses the selected flag, the cursor and the
anchor of the renamed row. The cursor jumps to a neighbour.

Found under `ctest -j8`: the selection test lost `kept-5` after `rename(keep-5, kept-5)`, in 1 of
5 parallel passes. The selection test now renames an unselected entry. The deterministic repro
uses `SplitRenameWatcher`, which delivers every Renamed as MovedFrom + MovedTo in the same poll.
After that rename, `{a, b}` with the cursor on `b` becomes `{a}` with the cursor on `c`.

Fix idea (ui): within one batch, pair a `Remove` with a later `Upsert` whose fresh stat has the
same `FileId` (st_dev/st_ino) as the removed row, and carry the selected, cursor and anchor
flags over. The removed row's `DirEntry::id` is still in `entries_` when the Remove is applied.
This cannot work across two batches, but the 60 ms coalescing window makes that case rare.

## Observations (not bugs, documented behaviour)

- While a re-sort or hidden toggle is pending, `sort()` and `show_hidden()` already show the new
  value and the old rows stay visible, as the "no flash to empty" rule requires. The invariant
  checker skips the order and hidden checks while `busy()`.
- Commands sent during a pending navigation are relative to the committed location. Two quick
  Alt+Left presses go back once (both target `back_.back()`), and two quick Backspaces go up
  once. Browsers behave the same way. If Phase 3 wants queued history steps, it needs a design
  change.

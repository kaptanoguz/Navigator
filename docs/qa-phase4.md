# QA: Phase 4 findings (sidebar, bookmarks, recents, GoTo, address bar, copy path)

Owner: qa. Suites:

- `navigator_browser_integration_tests` (ctest prefix `integration.`, label `integration`):
  `keymap_e2e_test.cpp` (now four phase tables), new `goto_e2e_test.cpp` and
  `places_e2e_test.cpp`. Headless: links `navigator_ui_browser`, no SDL, no window.
- `integration.app_places_chords` (`tests/integration/places_app_smoke.sh`) and the updated
  `integration.app_session_restart`: the real binary on `SDL_VIDEODRIVER=dummy`.

Gates: `docs/qa-gates.md`, "Phase 4 additions" (29 to 34).

Measured on 2026-11-17 on the development machine: Linux 6.8, GCC 13.3, clang 18, ext4 `/tmp`,
8 cores, default inotify limits. Scratch build dirs only, never `./build`.

## Tests

| Test (prefix `integration.`) | What it checks |
|---|---|
| `docs/keymap.md and load_default_keymap agree row for row` | 4 tables, 3 + 25 + 21 + 5 = 54 chord rows in `Keymap::bindings()` order; the exact set of 10 `(unbound)` rows (Phase 3: `tab.close_left`, `layout.delete`, `window.new`; Phase 4: `sidebar.focus`, `bookmarks.rename/move_up/move_down`, `recents.clear/toggle_tracking`, `view.toggle_view_lock`). |
| `Every default chord resolves ...` | Phase 4 chords are not handled by `Workspace::execute` (nothing changes), then their effect is checked on the headless pieces `App` dispatches to. `Ctrl+B`: `set_sidebar` flips `visible`, bumps `persist_serial`, lands in the snapshot; `cycle_focus` includes the sidebar only while it is visible. `Ctrl+L`: the address-bar `PathEntry` opens on the folder, all selected, no dropdown, no highlight; typing completes `alpha`, Tab, Enter navigates the tab. `Ctrl+G`: GoTo lists recents then bookmarks, Enter on the highlighted recent navigates, a typed `../alpha/inner` with Ctrl+Enter opens a background tab. `Ctrl+D`: `toggle_bookmark` adds (sidebar row present and highlighted), a `.` spelling removes it. `Ctrl+Shift+C`: the folder when nothing is selected, the selection in list order and shell-quoted, `CurrentFolder` ignores the selection, one name with a space stays unquoted. Near misses now include `Ctrl+P`, `Ctrl+C`, `Ctrl+Shift+B`, `Ctrl+Shift+G`, `Ctrl+Alt+L`, `Alt+D`, `Ctrl+Alt+C` (`Ctrl+B` left the list). |
| `GoTo resolves home, root, variables and relative input to folders` | Real `PathWorker`. `~/Downloads`, `~/Downloads/`, `/`, `~`, `$HOME/x`, `${HOME}//x/./`, `$NAVIGATOR_QA_DIR`, `%HOME%/Downloads`, `..`, `'~/with space'\n`, `file:///`; a file reveals (cursor) or opens (Ctrl+Enter). The ui context has `percent_variables = true`, home, cwd and no `~user` lookup (only the worker context has one); with the flag off, `%HOME%` stays literal and is not reported. |
| `GoTo applied to the workspace ...` | Outcomes through `navigate_active` / `open_tab`: `~/Downloads`, `/`, a file (cursor on it), a background tab; `take_visits` reports the folders; no launch. |
| `GoTo corrects the case of unique names, including Turkish I` | `~/downloads`, `~/DOWNLOADS/`, `$HOME/downloads`, `~/indirilenler` / `~/INDIRILENLER` / `~/ındırılenler` -> `İndirilenler`, `~/MÜZİK`, `~/belgeler/ÇALIŞMA`, `~/X`, all `case_corrected`. With both `Music` and `music`, the exact name wins and `~/MUSIC` is not found (text kept). Completion corrects the folder being completed (`~/belgeler/ça` -> absolute `.../Belgeler/Çalışma/`); `~/İN` and `~/in` both offer `İndirilenler`. |
| `GoTo: unknown variables and missing paths keep the text ...` | `${XDG_DOWNLOAD_DIR}`, `$XDG_DOWNLOAD_DIR/x`, `$NAVIGATOR_QA_UNSET`, `%NAVIGATOR_QA_UNSET%`: no outcome, text unchanged, the error names the token. A missing path has an error that names no variable; a remote `file://` URI is an error; the entry works afterwards. |
| `GoTo: ${XDG_DOWNLOAD_DIR} expands when the environment exports it` | Only as an environment variable: the user-dirs file is not consulted. |
| `Completion under rapid typing in a 20k folder never shows a stale completion` | 20 000 folders. GoTo and the address bar, 3 rounds of 10 words hopping between the big and a small folder, one byte per keystroke, 5 to 90 ms apart (cancelled listings, mailbox replacement, cache hits). After every keystroke every completion row belongs to the current text (folder and prefix). After the worker settles, the rows equal `complete_path()` of the final text (10 rows); a cache hit in the same folder needs no worker round trip. |
| `Completion: a superseded request's reply does not wipe ...` | `[!mayfail]` BUG-P4-1. |
| `Bookmarks and recents survive an unmount, restarts and a remount elsewhere` | Real `VolumeWorker` (an `IVolumeMonitor` over a mutex-guarded fake mount source with no `change_fd`, fake sysfs/devfs with `by-uuid/1234-ABCD`), real probe worker, real files. Run 1: the stick bookmark gets uuid `1234-ABCD`, the NFS one gets device `server:/export`, the `/` one no identity. A deleted local folder becomes Missing. `share/x` does not exist but stays Online, so network volumes are never probed. On unmount (folder moved away, table changed) the stick entries are Offline, not Missing. All 4 bookmarks and 3 recents stay, the sidebar row is Offline and not highlighted, there is no Storage row. Files keep everything. Run 2 (restart, still unmounted): still Offline. A rename (Turkish) and a move save and reload. The identity is untouched. Run 3 (stick at `STICK2`): rebased, Online, name and order kept, new path saved. The recent `STICK/data` lands on the existing `STICK2/data` entry and is merged (`visit_count` 2, newer position). The share unmounted gives Offline, still listed. The config dir holds only `bookmarks.json` and `recents.json`. |
| `Bookmark, recents and session writers race into one config dir without tearing` | 800 UI-thread steps: bookmark toggle, rename, move, navigation, new/close/next tab, clear recents, tracking toggle. Each step runs `frame_start`, `take_visits` -> `record_visit`, `tick`, and a `sync` + scheduled `SessionWriter::submit`. The probe worker runs, and 3 reader threads parse each file continuously. 0 torn or invalid reads, > 100 session submits. A recents change inside the debounce window right before quit is written only by `final_save`. After quit (`final_save`, `flush` + `save_now`), each file equals its last snapshot and the dir holds exactly the 3 files. |
| `PlacesModel without persist or restore never touches the config dir` | A corrupt `bookmarks.json` is neither loaded nor moved to `.bad`. Edits and `final_save` write nothing (`writes() == 0`). |
| `app_places_chords` | Real binary, `key:CHORD` steps (the same path as a key press). `Ctrl+B` writes `"visible": false`, and the next run writes `true` again. `Ctrl+D` adds the folder (mode 600), and the next run removes it. `Ctrl+L` + type + Enter navigates. `Ctrl+G` `~/downloads` gives `~/Downloads`, `~/indirilenler` gives `~/İndirilenler`, and Ctrl+Enter opens a background tab; `recents.json` records them. `Ctrl+Shift+C` exits 0 (the clipboard is not observable on the dummy driver). `NAVIGATOR_DEBUG_VOLUMES` fake stick: the bookmark is saved with uuid. Unmounted, `bookmarks.json` is byte-identical after the run. At `STICK2` it is rebased in the file. `--frames` without `--config-dir`, with `HOME` and `XDG_CONFIG_HOME` in the temp dir, navigates, bookmarks, toggles the sidebar and opens a tab, and nothing appears under either. |
| `app_session_restart` (updated) | Run 1 may leave `session.json` and `recents.json` (mode 600, holding the visited folders) and must not leave `bookmarks.json`. Anything else is still a leftover. `--no-session` on a fresh dir writes no file at all. |

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^integration\.'` | 47/47 pass (BUG-P4-1, the only `[!mayfail]` case, fails as expected) |
| clang 18 Debug `-Werror`, browser integration binary | builds clean, all cases pass |
| `--repeat until-fail:10`, serial | 10 clean passes (538 s) |
| `-j8 --repeat until-fail:5` | 5 clean passes (63 s) |
| ASan + UBSan, `-j2 --repeat until-fail:2`, app scripts included | 47/47, no report |
| TSan (GCC 13 RelWithDebInfo, `setarch -R`, `halt_on_error=1`), `--repeat until-fail:3`, `-E app_` | 44/44, 0 `WARNING: ThreadSanitizer` |
| clang-format-18 `--dry-run -Werror`, clang-tidy-18 on the three test files | clean |

Timings (GCC Debug): the 20k cases spend most of their 1 to 2 s on `mkdir`. The places e2e
takes about 2 s (volume poll steps of 250 ms). The app script takes about 10 s for 15 runs.

## Bugs

### BUG-P4-1 (low, ui): a superseded completion reply wipes completions served from the cache

Test: `Completion: a superseded request's reply does not wipe completions served from the cache`.

Repro (GoTo or address bar, real `PathWorker`):

1. Type `<dir B>/al`, wait: the B listing is cached and its completions are shown.
2. Type `<dir A>/d` and let the debounce fire: request R(A) is sent and `completion_id_ = R`.
3. Before R returns, type `<dir B>/alp`: `changed()` hits the B cache and shows B's completions,
   but `completion_id_` stays R.
4. R's reply arrives. `poll()` accepts it (`id == completion_id_`), and `apply_completion()` replaces the
   cache with A and re-matches. The typed dir B is not A, so the completion list becomes empty, and no new
   request is scheduled. The rows stay empty until the next keystroke.

Nothing stale is shown, but valid completions disappear. Cause: `PathEntry::changed()`
(src/ui/path/path_entry.cpp) does not drop the in-flight request when it serves the text from
the cache. Fix: set `completion_id_ = 0` (or compare `requested_key_` with `wanted_key_` before
applying) when the cache matches.

## Spec notes (no test change needed unless the decision changes)

- **`%VAR%` default.** The task brief said "%VAR% off by default". `path_input.hpp`
  (`ExpandContext::percent_variables = true`) and docs/architecture.md "GoTo and the address bar"
  (`percent_variables = true` in the ui context) say on, and the roadmap says "optional". The
  tests assert the documented behaviour. If it should be off, contracts changes the docs, ui
  changes `PathEntry::ui_context()`, and the `%HOME%/Downloads` case in
  `goto_e2e_test.cpp` flips.
- **`${XDG_DOWNLOAD_DIR}`** expands only when the environment exports it (most desktops do not).
  The XDG user-dirs values are not offered as variables. This is per contract, not a bug.

## Gaps

- `Ctrl+Shift+C` clipboard contents are checked only headlessly (`copy_path_text`). On the dummy
  driver the app's SDL clipboard is not observable.
- The volume worker's periodic space refresh (10 s) and a hung network `statvfs` are not
  exercised. The "never probed" rule is checked through a missing folder on a share staying
  Online.
- Sidebar pointer gestures (drag to reorder, drop a tab on Bookmarks) are covered by `tests/ui`,
  not end to end here.

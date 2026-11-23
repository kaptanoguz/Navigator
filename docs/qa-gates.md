# QA gates

Phase 0 gates are below. Phase 1 adds the gates in "Phase 1 additions", Phase 2 those in
"Phase 2 additions", Phase 3 those in "Phase 3 additions", Phase 4 those in "Phase 4
additions", Phase 5 those in "Phase 5 additions"; every earlier gate still applies.

## Phase 0

Owner: qa. Every gate must be green before the phase closes (CLAUDE.md, coordinator rule 6).
Run from the repository root. Tools default to the `-18` names; override with
`CLANG_FORMAT=...`, `CLANG_TIDY=...`, `RUN_CLANG_TIDY=...`.

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 1 | Configure | `cmake --preset dev` | exits 0, writes `build/dev/compile_commands.json` |
| 2 | Build | `cmake --build --preset dev` | exits 0, no warnings in project code |
| 3 | Tests | `ctest --preset dev` | all `smoke.*` tests pass (incl. `smoke.app_headless` when the app target exists) |
| 4 | Format | `scripts/check-format.sh` | exits 0 (`clang-format --dry-run -Werror` over `include/ src/ tests/`); fix with `scripts/check-format.sh --fix` |
| 5 | Tidy | `scripts/run-tidy.sh build/dev` | exits 0, zero findings (`.clang-tidy` has `WarningsAsErrors: '*'`) |
| 6 | Header self-containment | `cmake --build --preset dev --target navigator_header_check` | every public header compiles alone with `-Werror` |
| 7 | Headless run | `SDL_VIDEODRIVER=dummy ./build/dev/src/app/navigator --frames 60` | exits 0 after 60 frames |
| 8 | X11 under xvfb | `xvfb-run -a ./build/dev/src/app/navigator --frames 60` | exits 0 (this is what CI runs) |
| 9 | Contracts | review | a changed `include/navigator/*` diff comes with a `docs/architecture.md` update |

One-liner for gates 1–5:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev \
  && scripts/check-format.sh && scripts/run-tidy.sh build/dev
```

## Phase 1 additions (core, platform, integration)

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 10 | Module + race tests | `ctest --preset dev -R '^(core\|platform\|integration)\.'` | all pass; `integration.*` takes under ~30 s in total |
| 11 | 100k listing perf | `ctest --preset dev -R '^core\.100k entries' -V` | passes; record the timing it prints in the phase summary / docs |
| 12 | Flake repeat | `ctest --preset dev -R '^integration\.' --repeat until-fail:10` | 10 clean runs of every test (about 2 min) |
| 13 | Parallel stress | `ctest --preset dev -R '^(core\|platform\|integration)\.' -j8 --repeat until-fail:5` | all pass while tests compete for CPU and the inotify budget |
| 14 | ASan + UBSan | configure with `-DNAVIGATOR_SANITIZE=ON` into a separate build dir, then gate 10 there | no sanitizer report, all pass |
| 15 | TSan | separate build dir with `-DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread` (RelWithDebInfo), then `TSAN_OPTIONS=halt_on_error=1 setarch -R ctest -R '^(core\|platform\|integration)\.'` | no `WARNING: ThreadSanitizer`, all pass |
| 16 | Tool smoke | `./build/dev/tools/navigator-ls <dir>` (and its ctest smoke) | lists, sorts, exits 0 |

Sanitizer build dirs (example):

```sh
cmake --preset dev -B build/asan -DNAVIGATOR_SANITIZE=ON && cmake --build build/asan
cmake --preset dev -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread && cmake --build build/tsan
```

- **TSan needs `setarch -R`** (ASLR off) on kernel 6.x with GCC 13. Without it TSan stops
  with `FATAL: ThreadSanitizer: unexpected memory mapping`, because `vm.mmap_rnd_bits` is too
  high for that runtime. This is an environment issue, not a test failure.
- Only Navigator code is instrumented. Catch2 and libstdc++ are not. Worker threads in the tests never
  call Catch2 assertions: they record into plain structs, and the test thread asserts after
  `join()`.
- The integration tests need a writable `$TMPDIR` and the default inotify limits or higher
  (`max_queued_events` 16384, a few free instances and watches). The burst and lazy-sync tests
  then exercise both the Overflow path and the no-loss path. With a raised
  `max_queued_events`, they take the no-loss branch and still pass.
- Findings and timings: `docs/qa-phase1.md`.

## Phase 2 additions (single-pane browser)

Every Phase 0 and Phase 1 gate still applies. New suites: `navigator_browser_integration_tests`
(ctest prefix `integration.`, label `integration`; links `navigator_ui_browser` only,
no SDL, no window) and the app script `integration.app_headless_start_paths`
(`tests/integration/app_smoke.sh`).

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 17 | Browser races + keymap e2e | `ctest --preset dev -R '^integration\.'` | all pass; the `BUG-P2-*` cases are `[!mayfail]` and may fail (open bugs, `docs/qa-phase2.md`); remove the tag when a fix lands |
| 18 | Flake repeat | `ctest --preset dev -R '^integration\.' --repeat until-fail:10` | 10 clean runs |
| 19 | Keymap doc sync | `ctest --preset dev -R '^integration\..*(keymap\|chord)'` | `docs/keymap.md` rows equal `load_default_keymap()` row for row; every default chord has an effect check |
| 20 | Headless start paths | `ctest --preset dev -R app_headless_start_paths` | fixture folder, unreadable folder and missing path each exit 0 after 120 frames on `SDL_VIDEODRIVER=dummy` |
| 21 | ASan + UBSan, TSan | gates 14 and 15 with `-R '^integration\.'` (TSan still needs `setarch -R`) | no sanitizer report |
| 22 | No spawn in tests | `grep -rn "create_default_launcher\|open_with_default(" tests` | the default launcher is only built in `tests/platform/launcher_test.cpp` (probe programs, `true`) and in `tests/contracts` (compiled, never linked); a BrowserModel that activates a file always gets a recording launcher |

Notes:

- The browser tests use inotify (one instance per model) and a writer thread; they share the
  Phase 1 limits note above.
- `invariant_violation()` (tests/integration/browser_support.hpp) checks order and the hidden
  filter only while no listing is pending: `sort_by()` / `toggle_hidden()` switch `sort()` and
  `show_hidden()` at once and the old order stays visible until the re-list commits (documented
  "no flash to empty" behaviour, not a bug).
- `integration.app_headless_start_paths` needs the `navigator` target; build it in sanitizer
  build dirs too or exclude the test there.
- Findings: `docs/qa-phase2.md`.

## Phase 3 additions (tabs, split panes, layouts, session)

Every earlier gate still applies. `navigator_browser_integration_tests` gains
`workspace_session_test.cpp` (ui::Workspace + SessionWriter + core session files) and the keymap
e2e now drives `ui::Workspace`; the app script `integration.app_session_restart`
(`tests/integration/session_app_smoke.sh`) is new.

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 23 | Keymap doc sync, all phases | `ctest --preset dev -R '^integration\..*(keymap\|chord)'` | every `## Phase N defaults` table of `docs/keymap.md` (49 chords: 3 + 25 + 21) equals `load_default_keymap()` row for row; each chord has an effect check through `Workspace::execute`; every registered command has a chord or an `(unbound)` row, never both |
| 24 | Session round trip | `ctest --preset dev -R '^integration\..*(panes with three tabs\|interchangeable\|corrupt\|Named layouts)'` | 2 panes x 3 tabs restore with paths, sort, hidden, cursor, active tab/pane, closed tabs, window; Workspace JSON == core JSON; corrupt files go to `.bad`; Turkish layout names round-trip |
| 25 | Writer hammer | `ctest --preset dev -R 'integration\.Background saves'` | 0 torn or invalid reads by 2 reader threads, file == last snapshot, no temp files left |
| 26 | Lazy tabs | `ctest --preset dev -R 'integration\..*(visible tabs own\|background tab relists)'` | 20 background tabs add 0 threads (`/proc/self/task`) and 0 inotify instances (`/proc/self/fd`); a resumed tab shows changes made while suspended and is watched again |
| 27 | App restart | `ctest --preset dev -R app_session_restart` | run 1 saves 3 panes, run 2 restores the same dump, PATH adds a live tab, a corrupt file goes to `.bad` with a 1-pane start, `--no-session` reads and writes nothing, missing folders still restore 3 panes |
| 28 | Flake, parallel, sanitizers | gates 18, 13 (`-j8 --repeat until-fail:5`), 14 and 15 with `-R '^integration\.'` | all pass; `BUG-P3-*` cases are `[!mayfail]` (open bugs, `docs/qa-phase3.md`) |

Notes:

- The app scripts need the `navigator` target; in the TSan dir build `navigator` explicitly (the
  `navigator_header_check` contract targets do not build there with GCC 13 `-O2`, see
  `docs/qa-phase3.md`).
- Thread and inotify counts are process-wide: the lazy-tab tests must not run in a process that
  has other live workers (Catch2 runs test cases one after another, so this holds).
- Findings: `docs/qa-phase3.md`.

## Phase 4 additions (sidebar, bookmarks, recents, GoTo, address bar, copy path)

Every earlier gate still applies. `navigator_browser_integration_tests` gains
`goto_e2e_test.cpp` (PathEntry + the real PathWorker) and `places_e2e_test.cpp` (PlacesModel
with its real volume and probe workers over a fake mount table, the three config-dir writers);
the keymap e2e knows the Phase 4 table. New app script `integration.app_places_chords`
(`tests/integration/places_app_smoke.sh`); `integration.app_session_restart` now accepts
`recents.json` next to `session.json`.

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 29 | Keymap doc sync, all phases | `ctest --preset dev -R '^integration\..*(keymap\|chord)'` | 4 phase tables, 54 chords (3 + 25 + 21 + 5) equal `load_default_keymap()` row for row; 10 `(unbound)` rows (the exact set is checked); Phase 4 chords are not handled by `Workspace::execute` and their effect is checked on the headless pieces the shell calls (`set_sidebar`, address-bar / GoTo `PathEntry`, `PlacesModel::toggle_bookmark`, `copy_path_text`) |
| 30 | Phase 4 chords in the app | `ctest --preset dev -R app_places_chords` | `key:Ctrl+B` flips `"visible"` in session.json; `Ctrl+D` adds then removes the folder in bookmarks.json (mode 600); `Ctrl+L` + Enter and `Ctrl+G` + Enter navigate (`~/downloads` -> `~/Downloads`, `~/indirilenler` -> `~/İndirilenler`), `Ctrl+Enter` opens a background tab; a bookmark on a fake stick keeps its file while unmounted and is rebased when the stick returns elsewhere; `--frames` without `--config-dir` creates nothing under a temporary `HOME` / `XDG_CONFIG_HOME` |
| 31 | Offline tolerance | `ctest --preset dev -R 'integration\.Bookmarks and recents survive'` | unmount -> Offline (never dropped, never Missing), files keep the entries; restart offline, rename/move save and reload; remount at another mount point -> Online, rebased, saved; a recent landing on an existing path is merged (visit counts summed); a missing folder on a network share stays Online (never probed), a deleted local folder is Missing |
| 32 | GoTo / address bar | `ctest --preset dev -R 'integration\.(GoTo\|Completion)'` | `~/Downloads`, `/`, `$HOME/x`, `${HOME}`, `%HOME%`, `..`, quoted and `file://` input resolve; unique case correction incl. Turkish I; ambiguous names and unknown variables keep the text and name the variable; 20k-entry folder under rapid typing shows 0 stale completions in GoTo and the address bar; `BUG-P4-*` cases are `[!mayfail]` (`docs/qa-phase4.md`) |
| 33 | Config-dir writers | `ctest --preset dev -R 'integration\.Bookmark, recents and session writers'` | 3 reader threads see 0 torn or invalid bookmarks / recents / session files; each file == its last snapshot after the quit flush (a recents change inside the debounce window included); exactly the three files, no temp files |
| 34 | Flake, parallel, sanitizers | gates 18, 13, 14 and 15 with `-R '^integration\.'` | all pass |

Notes:

- `places_app_smoke.sh` feeds the fake volume table through the dev-only
  `NAVIGATOR_DEBUG_VOLUMES` (src/ui/shell/debug_volumes.hpp); it never reads the real mount
  table for the sidebar.
- The GoTo cases set `HOME` and a few test variables before any worker starts; ctest runs each
  Catch2 case in its own process, so this never leaks into other cases.
- Findings: `docs/qa-phase4.md`.

## Phase 5 additions (folder filter, flatten, fuzzy search, name index)

Every earlier gate still applies. `navigator_browser_integration_tests` gains
`search_gold_test.cpp`, `flatten_e2e_test.cpp` and `index_lifecycle_test.cpp` (helpers in
`search_support.hpp`; real `fs::Walker`, real `create_name_index`, real inotify); the keymap e2e
knows the Phase 5 table. New app script `integration.app_index_cache_rules`
(`tests/integration/index_app_smoke.sh`). The ui acceptance smoke
`ui.smoke_flatten_cancels_without_freezing` (owner ui) is part of the gate.

| # | Gate | Command | Pass condition |
|---|------|---------|----------------|
| 35 | Keymap doc sync, all phases | `ctest --preset dev -R '^integration\..*(keymap\|chord\|Unbound Phase 5)'` | 5 phase tables, 58 chords (3 + 25 + 21 + 5 + 4) equal `load_default_keymap()` row for row; 14 `(unbound)` rows (exact set); `Ctrl+E` flattens and leaves through `Workspace::execute`; `Ctrl+F`, `Ctrl+Shift+E`, `Ctrl+Shift+F` are not handled by the Workspace and their effect is checked on the BrowserModel calls the shell makes (filter text selected and replaced, walk- and index-backed volume flatten, types popup intents persisted in the tab); `list.clear_filter`, `search.toggle_fuzzy`, `search.cancel` are handled, `search.rebuild_index` is not |
| 36 | Ranking gold | `ctest --preset dev -R 'integration\.Gold'` | hand-computed orders for `report`, `notes`, `rep 2024`, Turkish `istanbul` / `ışık` spellings, `izmir`, NFC `café` vs NFD `cafe`, invalid UTF-8 `badname`, identical in the reference (`Walker` + `Matcher` + `ranks_before`), the walk flatten, `IIndex::query` and the index-backed flatten, Substring and Fuzzy |
| 37 | Hidden, permissions, types, cancel | `ctest --preset dev -R 'integration\.(Flatten\|The index-backed\|Permission\|An unreadable flatten\|Walk- and index\|Cancelling\|Typing during\|Index queries cancel)'` | no child of a hidden folder appears unless show_hidden (walk and index); unreadable folders listed, not entered, counted (2) with samples; walk- and index-backed rows and types popup agree; cancelled walks keep a breadth-first-complete, duplicate-free partial result and stop delivering; a cancelled index query is an error or the full result; `BUG-P5-*` cases are `[!mayfail]` (`docs/qa-phase5.md`) |
| 38 | Index lifecycle | `ctest --preset dev -R 'integration\.Index:'` | cold scan saves a valid file (FNV trailer, 0600 in a 0700 dir, no temporaries); a restart loads it and verify shows changes made while stopped; flipped byte, truncated, empty, garbage, other version, bad magic and other-volume files are rejected, rebuilt and rewritten; inotify creations, renames and deletions (also in a new folder) reach an index-backed flattened view |
| 39 | Cache rules in the app | `ctest --preset dev -R 'app_index_cache_rules\|smoke_flatten'` | headless runs without `--cache-dir` create nothing under a temporary `HOME` / `XDG_CACHE_HOME` / `XDG_CONFIG_HOME` (also with `NAVIGATOR_DEBUG_INDEX_ROOT` set); `--no-index` creates no `index/`; with `--cache-dir` the fixture-only index is one `.idx` (600, folder 700, magic), rebuilt after corruption; the 300k flatten smoke passes its frame and cancel bounds |
| 40 | Flake, parallel, sanitizers | gates 18, 13, 14 and 15 with `-R '^integration\.'` | all pass |

Notes:

- App runs that use `--cache-dir` always set `NAVIGATOR_DEBUG_INDEX_ROOT` to a fixture: without
  it the service would index the machine's real default volumes.
- The permission tests skip as root. Folders made unreadable get mode 0755 back before the
  fixture is removed.
- Findings: `docs/qa-phase5.md`.

## Manual check (developer machine, not CI)

Record the result in the phase summary:

- **Wayland** (GNOME / Mint session): `SDL_VIDEODRIVER=wayland ./build/dev/src/app/navigator` —
  the window opens, the "Navigator" text is crisp, resizing works, it closes cleanly.
- **X11**: `SDL_VIDEODRIVER=x11 ./build/dev/src/app/navigator` — the same checks.

## Notes

- Smoke tests live in `tests/smoke` (`navigator_smoke_tests`, registered with the `smoke.` prefix
  through `catch_discover_tests`). Filter them with `ctest --preset dev -R '^smoke\.'`.
- clang-tidy only reports on headers under this repo's `include/navigator/` and `src/`. No path
  segment in front of them may start with `_`, so `build/*/_deps` trees are skipped.
- Tests that touch the filesystem must use temporary fixture directories (CLAUDE.md rule 9).
- Race tests live in `tests/integration` (`navigator_integration_tests`, prefix `integration.`,
  label `integration`). Every test that links navigator libraries must build with the root's
  `CMAKE_CXX_STANDARD` (see docs/integration-notes.md, qa).

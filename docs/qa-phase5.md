# QA: Phase 5 findings (folder filter, flatten, fuzzy search, name index)

Owner: qa. Suites:

- `navigator_browser_integration_tests` (ctest prefix `integration.`, label `integration`):
  `keymap_e2e_test.cpp` (now five phase tables) and the new `search_gold_test.cpp`,
  `flatten_e2e_test.cpp` and `index_lifecycle_test.cpp`, with helpers in `search_support.hpp`.
  Headless: links `navigator_ui_browser` (which brings `navigator_search`), no SDL, no window.
  They use the real `fs::Walker`, real `create_name_index` indexes and real inotify, on
  temporary fixture folders only.
- `integration.app_index_cache_rules` (`tests/integration/index_app_smoke.sh`): the real binary
  on `SDL_VIDEODRIVER=dummy`.
- The ui acceptance smoke `ui.smoke_flatten_cancels_without_freezing` (owner ui), re-run here.

Gates: `docs/qa-gates.md`, "Phase 5 additions" (35 to 40).

Measured on 2026-11-23 on the development machine: Linux 6.8, GCC 13.3, clang 18, ext4 `/tmp`,
8 threads, default inotify limits. Scratch build dirs only, never `./build`.

## Tests

| Test (prefix `integration.`) | What it checks |
|---|---|
| `docs/keymap.md and load_default_keymap agree row for row` | 5 tables, 3 + 25 + 21 + 5 + 4 = 58 chord rows in `Keymap::bindings()` order; the exact set of 14 `(unbound)` rows (Phase 5 adds `list.clear_filter`, `search.toggle_fuzzy`, `search.cancel`, `search.rebuild_index`). |
| `Every default chord resolves ...` | `Ctrl+E` through `Workspace::execute`: a real walk of the fixture, 34 rows without `.hidden`, folders first, Fuzzy by default, `inr` finds `alpha/inner`, no Recents, again leaves. `Ctrl+F`: not handled by the Workspace; the shell's select-all + typing replaces the old text, the Substring filter shows `c.txt` inline, Enter opens the row, `Ctrl+A` selects the shown rows only, navigation clears the text. `Ctrl+Shift+E`: not handled; the volume walk and then a Ready real index give the same rows, and `f3` ranks `f30, f03, f13, f23` (88 vs 61). `Ctrl+Shift+F`: not handled; popup rows Folders 2 / No extension 30 / txt 1, hiding `.txt` hides `c.txt`, lands in `TabState::type_filter`, survives navigation and Escape, Toggle all restores. Near misses lose `Ctrl+E` and gain `Ctrl+Alt+E`, `Alt+E`, `Ctrl+Alt+F`, `Alt+F`, `Ctrl+Alt+Shift+E`, `Shift+F`, `Ctrl+Space`, `Ctrl+Comma`. |
| `Unbound Phase 5 commands through the workspace` | `list.clear_filter`, `search.toggle_fuzzy` (`f2`: 10 Substring rows, 12 Fuzzy), `search.cancel` (rows kept, all from the subtree) are handled; `search.rebuild_index` is not (App). |
| `Gold: 'report' ...` | Hand-computed order of 9 names (exact name 348 > exact stem 308 > camelCase 268 > prefix 248, shorter first > word start 233, shallower first > 229), Fuzzy adds `repo_root.txt` (211) last; `  REPORT ` gives the same. Never `rapport.txt` or anything hidden. Same order in all four paths: reference (`Walker` + `Matcher` + `ranks_before`), walk flatten, `IIndex::query`, index-backed flatten. |
| `Gold: ties break ...` | `a/notes.md` before `b/notes.md` (parent decides), then `istanbul-notes.md`; `rep 2024` ANDs two terms. |
| `Gold: Turkish dotted and dotless I ...` | `istanbul`, `ISTANBUL`, `İstanbul`, `ıstanbul`, `İSTANBUL` all give `ISTANBUL.log`, `İstanbul.txt` (shorter in bytes first), `istanbul-notes.md`; `ışık`, `IŞIK`, `işik` give `Işık.png`, `ışık.jpg`, never `ılık.txt`; ASCII `isik` finds nothing; `izmir` finds `çalışma/İzmir.txt`. |
| `Gold: the folder filter keeps the sort order ...` | Substring folder filter rows stay in `compare_names` order (no ranking). |
| `Gold: NFC and NFD ...` | `café` finds only the NFC name, `cafe` only the NFD one; `badname` matches `bad\xFFname.txt` only in Fuzzy; highlights are byte ranges on code point boundaries; walk and index list the same raw bytes, every `row_path` exists, activation launches the exact bytes. |
| `Gold: results are identical across runs and readdir orders` | The corpus rebuilt in reverse creation order ranks the same. |
| `Flatten never shows children of hidden folders unless show_hidden (walk)` | `.secret/`, `dir/.git/`, `dir/sub/.cache/` and their children never appear, `hidden_skipped` 5, queries that only hidden entries match are empty; `view.toggle_hidden` re-walks with everything (17 rows) and back. |
| `Flatten inside a hidden folder ...` | The root itself is walked even when hidden; an index query with `under` in a hidden folder behaves the same. |
| `The index-backed flatten (Ctrl+Shift+E) hides children of hidden folders too` | Hidden entries are indexed (17) but never shown without show_hidden, also for queries; toggling hidden re-queries. |
| `Permission-denied folders are skipped and counted ...` | Mode 000 and mode 0300 folders are listed, not entered; `skipped_permission` 2 with both samples, no error; the index counts 2 and shows the same rows. |
| `An unreadable flatten root fails cleanly` | `PermissionDenied`, no rows. |
| `Walk- and index-backed views agree on the type filter ...` | Folders, links to folders and files, a dangling link, `x.`, `.JPG` vs `.jpg`: identical rows for 7 filters and identical popup counts (3 folders including `linkdir`). |
| `Walk- and index-backed views agree on a link to a folder hidden by its type` | `[!mayfail]` BUG-P5-1. |
| `Cancelling a flatten walk mid-way keeps a consistent partial result` | 770-entry tree, real walker slowed in 8-entry batches; cancel after 1 / 30 / 300 / 600 found, with queries `""`, `x`, `s2`. `cancel_search` returns in < 50 ms, the walk thread exits, rows are duplicate-free and exist, they are breadth-first complete (every matching entry shallower than the deepest row is shown), `found` equals the rows for an empty query, nothing arrives afterwards, a later query edit re-matches without a new walk. |
| `Typing during a walk never shows rows of a stale query` | Worker-path re-matching (`ui_filter_limit` 64) while the walk streams: every frame consistent once nothing is organizing; the final rows are the 50 `x1.txt`. |
| `Index queries cancel cleanly ...` | 20k-entry index: a stopped token gives `Cancelled`; 40 random-delay cancels give either `Cancelled` or exactly the full result; index-backed view under rapid typing stays consistent and ends on `dir9/file_9_19.txt`; cancel during a query keeps consistent rows. |
| `Index: cold scan, save, restart ...` | File valid (magic, version, FNV-1a trailer), 0600 in a 0700 folder, no temporaries. Changes made while stopped (add, delete, folder rename, nested new folder) show after the restart, which reports `loaded_from_file` and no error; a third start loads the verified state. |
| `Index: a damaged or foreign file is rejected and rebuilt` | Flipped byte, truncated, empty, garbage, version 2, bad magic: `last_error` set, not loaded, full rebuild, file valid again after stop. Another volume's identity on the same path: rebuilt. |
| `Index: live inotify updates show in an index-backed flattened view` | A file created, a new folder with a file (and a later file in it: the new folder is watched), a rename and a recursive delete reach the view; a live hidden file is indexed but not shown; the live state is saved on stop. |
| `app_index_cache_rules` | Real binary with `Ctrl+E` / `Ctrl+Shift+E` / filters / Escape. Without `--cache-dir` (temporary `HOME`, `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, `NAVIGATOR_DEBUG_INDEX_ROOT` set): nothing created. `--no-index --cache-dir C`: no `C/index`. `--cache-dir C` with the fixture as the only root (`waitindex`): one `.idx`, 600, folder 700, magic. Corrupted mid-file: the next run rewrites it (same size, magic). |

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^integration\.'` | 68/68 pass (BUG-P5-1, the only Phase 5 `[!mayfail]` case, fails as expected) |
| `--repeat until-fail:10`, `-j4` | 10 clean passes of all 68 (476 s); again for the Phase 5 cases, the keymap cases and the lazy-tab case on the rebuilt binary (10 clean, 250 s) |
| `-j8 --repeat until-fail:5` | 5 clean passes (137 s) |
| clang 18 Debug `-Werror`, `-j8 --repeat until-fail:3` | 68/68, 3 clean passes |
| clang 18, `Only visible tabs own worker threads and watches` alone, 30 runs | 0/30 failures after the baseline fix |
| ASan + UBSan, `-j4 --repeat until-fail:2`, app scripts included | 68/68, no report |
| TSan (GCC 13 RelWithDebInfo, `setarch -R`, `halt_on_error=1`), `-j4 --repeat until-fail:2`, `-E app_` | 64/64, 0 `WARNING: ThreadSanitizer` |
| `ui.smoke_flatten_cancels_without_freezing` (GCC Debug, dummy driver, 300k tree) | found 90 112 before the cancel, 60 frames, frame avg 16.7 ms, p95 16.7 ms, max 20.0 ms (bound 400), cancel to thread exit 33 ms (bound 2000), 0 threads left, no index written |
| `scripts/check-format.sh`; clang-tidy-18 on the six touched test files | clean |

Timings (GCC Debug): the gold cases take about 1 s each (four paths per query); the cancel
cases about 3 s (16 slowed walks); the 20k-entry index cancel case about 2 s, mostly `touch`;
`app_index_cache_rules` about 24 s for 5 runs.

Item 4 (UI-thread responsiveness) is covered by the ui smoke above. It was not extended: it
already bounds the max frame and the cancel latency on a 300k tree.

## Bugs

### BUG-P5-1 (medium, search): a link to a folder is not a folder in the index, so type filters disagree with the walk

Test: `Walk- and index-backed views agree on a link to a folder hidden by its type`
(`[!mayfail]`).

Repro (real index, `tests/integration/flatten_e2e_test.cpp` `TypeTree`):

1. Make `pics/` and a symlink `linkdir -> pics`.
2. Index the folder (`create_name_index`, wait for Ready).
3. Flatten with the walk (Ctrl+E) and with the index (Ctrl+Shift+E), and hide "No extension" in
   the types popup of both.
4. The walk-backed view keeps `linkdir` (it is a Folder: `fs::is_directory_like`, search.hpp
   "Type visibility filter"). The index-backed view drops it. `IIndex::query` reports
   `directory_like == false` for it, and `IndexQuery{types.hide_no_extension = true}` excludes
   it.

Cause: the scan walks with `metadata = false`, so `target_kind` stays `Unknown` and
`kEntryLinkToDir` is never set (src/search/index.cpp, scan path near line 884). Only live
inotify inserts stat the link (line 1151), so the same link is a Folder or not depending on how it
entered the index. The popup counts still agree, because the ui stats each hit, and
`hide_folders` still agrees, because organize re-filters the stat'ed rows. Hiding "No
extension", or any extension a link-to-folder's name happens to have (`photos.2024`), loses
the link in the index view only. Fix options (search): stat symlinks during the scan (one
`fstatat` per link, cheap, symlinks are rare), or let the query treat `Symlink` with unknown
target as "type unknown" and leave the decision to the ui's organize pass.

### Note N-P5-1 (low, ui, doc): `BrowserModel::query()` is the newest query, not always "the query the rows reflect"

`browser_model.hpp` says `query()` is "The compiled query the rows reflect (highlights use it)".
While a worker re-match or a debounced index query is outstanding, the rows are still the
previous query's (by design: "old rows stay until the newest result"), but `query()` already
returns the new one. So for a frame or two, highlights are computed with a query that some
shown rows do not match (they draw unhighlighted). No wrong rows are shown. The
consistency checks here apply the query check only once `flatten_progress().organizing` is
false. Fix: keep the rows' query beside the rows for highlights, or reword the comment.

## Test fix: lazy-tab thread baseline (Phase 3 test)

`Only visible tabs own worker threads and watches` took its `/proc/self/task` baseline right
after joining a warm-up thread. On Clang builds that thread could still be listed, so the
baseline was one too high and the final "everything released" check failed (reported as 2/30
on Clang). The test now records the warm-up thread's tid and waits until
`/proc/self/task/<tid>` is gone before counting.

## Gaps

- UI frame times while an index-backed view refreshes are not measured. The 300k acceptance smoke covers the walk
  source only.
- A partial index scan stopped mid-way and resumed ("Resumable", index.hpp) is not exercised
  end to end. Only complete scans are saved here.
- The 2 s coarse-timestamp guard of the verify pass is not targeted. Changes in these tests land
  more than a few ms after the scan, and the verify picks them up either way.
- Watch-budget exhaustion and periodic rescans of unwatched folders are `tests/search`'s, not
  re-run against real inotify limits.

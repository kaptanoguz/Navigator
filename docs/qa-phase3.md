# QA: Phase 3 findings (tabs, split panes, layouts, session)

Owner: qa. Suites:

- `navigator_browser_integration_tests` (ctest prefix `integration.`, label `integration`):
  `keymap_e2e_test.cpp` now drives `ui::Workspace` (all phase tables of `docs/keymap.md`), and
  the new `workspace_session_test.cpp` (Workspace + `SessionWriter` + core session and
  named-layout files). Headless: links `navigator_ui_browser`, no SDL, no window.
- `integration.app_session_restart` (`tests/integration/session_app_smoke.sh`): the real binary
  on `SDL_VIDEODRIVER=dummy`.

Gates: `docs/qa-gates.md`, "Phase 3 additions" (23 to 28).

Measured on 2026-11-17 on the development machine: Linux 6.8, GCC 13.3, clang 18, ext4 `/tmp`,
8 cores, default inotify limits. Scratch build dirs only, never `./build`.

## Tests

| Test (prefix `integration.`) | What it checks |
|---|---|
| `docs/keymap.md and load_default_keymap agree row for row` | Parses every `## Phase N defaults` table: 3 + 25 + 21 = 49 chord rows, equal to `Keymap::bindings()` in order (chord, command, arg); canonical chord text; no chord bound twice; the 4 `(unbound)` rows are registered commands with no chord. |
| `Every default chord resolves to its command and has its effect on the workspace` | Each chord goes through `resolve_command` and `Workspace::execute` on a fresh fixture. Phase 2 checks as before (through the Workspace). Phase 3: `Ctrl+T` new tab right of the active one on its folder and sort; `Ctrl+W` / `Ctrl+F4` no-op on the last tab, then right/left neighbour activation and the closed entry (folder, cursor, index); `Ctrl+Shift+T` reopens at the old index with its cursor; `Ctrl+Tab` / `Ctrl+PageDown` / `Ctrl+Shift+Tab` / `Ctrl+PageUp` wrap within the active pane only; `Ctrl+Shift+PageUp/PageDown` reorder without wrap; `Ctrl+Shift+D` copies folder, cursor, selection and history, source untouched; `Ctrl+Enter` / `Ctrl+KeypadEnter` open a folder in a suspended background tab (sort, hidden inherited) and a file like `Enter`; `F3` / `Shift+F3` split horizontally / vertically, a second split joins the same n-ary split; `F6` / `Shift+F6` cycle 3 panes in reading order; `Ctrl+Shift+W` no-op on the only pane, else its tabs go to closed tabs in order; `Ctrl+Shift+A`, `Ctrl+Shift+S`, `Ctrl+Shift+O`, `Ctrl+Q`, `F12`, `Ctrl+Shift+L` are not handled (popups / App) and change nothing. After every check only visible tabs are live and `Layout::validate()` holds. Every registered command has a chord xor an `(unbound)` row. Near misses (`Ctrl+N`, `Ctrl+E`, `Ctrl+B`, `Ctrl+Shift+P`, `Ctrl+Shift+N`, `Ctrl+Alt+Left`, ...) resolve to nothing. |
| `Unbound Phase 3 commands through the workspace` | `window.new` handled as a no-op (no persist change), `layout.delete` not handled, `tab.close_left` with a TabId arg, stale ids are no-ops. |
| `Activate through the keymap never spawns a process` | Enter, KeypadEnter, Ctrl+Enter, Ctrl+KeypadEnter on 3 files: 12 recorded `xdg-open` requests, no tab, no child process. |
| `Two panes with three tabs each survive a save and a restart` | 2 panes x 3 tabs built through commands (per-tab sort, hidden, cursor; active tab 1 / 0; active pane 1; 2 closed tabs; window with position). `SessionWriter` writes exactly `to_json(snapshot)`; only `session.json` in the dir, mode 0600, dir 0700. `load_session` + a new Workspace: 2 live models of 6, every tab lists with its folder, sort, hidden and cursor on activation. |
| `Workspace JSON and core JSON are interchangeable` | A core-built 3-pane session (n-ary, 0.3/0.7 sizes, non-UTF-8 cursor name, closed tab, maximized) -> file -> Workspace; after listing, the Workspace's `to_json` is byte-identical to core's. Its JSON re-parses and re-serializes to the same text. |
| `Save, restore, save again without activating background tabs is a fixed point` | `[!mayfail]` BUG-P3-1. |
| `A corrupt session.json is moved to .bad and the workspace starts on one tab` | Truncated session, `{not json`, array, string, empty, binary: `InvalidArgument`, the file moves to `session.json.bad` (newest replaces older), the Workspace starts on one tab on the start folder, the next save is valid and keeps `.bad`. Valid JSON with an unusable layout (empty split, relative paths, `layout: 7`, junk closed tabs, version 99) loads leniently and gets one tab. |
| `Background saves while the layout changes never leave a torn or invalid file` | 600 random UI-thread steps (14 tab/pane commands, navigate, hidden, sort, `frame_start` without waiting), `sync()` + `SaveScheduler` + submit to two `SessionWriter`s on the same file (> 100 submits), 2 reader threads parse the file continuously. 0 invalid or torn reads, layout valid after every step, final file == last snapshot, no temp files. |
| `Named layouts with Turkish names round-trip through the workspace` | `Çalışma düzeni — ğüşöç İı`, `İstanbul` / `istanbul` (distinct files), 32 x `ı` (64 bytes, the limit). 9 invalid names fail with `InvalidArgument`. A new Workspace on the same root loads it byte-identically (`layout_to_json`), with a new epoch, the replaced tab in closed tabs, 2 x 3 tabs, active pane 1. Delete, missing load (NotFound, no change), no config root (Unsupported). |
| `Only visible tabs own worker threads and watches` | Inotify watcher. One live tab = +2 threads, +1 inotify fd. 20 background tabs via `Ctrl+Enter`, 5 tab switches: 0 extra threads (`/proc/self/task`), 0 extra inotify fds (`/proc/self/fd`). A split adds one live set, closing it removes it; everything is released with the Workspace. |
| `A background tab relists on activation and sees changes made while suspended` | Create, delete and mkdir in a suspended tab's folder: nothing reaches it while suspended; activation relists (new rows, cursor kept), the other tab suspends; the resumed tab is watched again (a later file arrives without refresh); the other tab catches up on return. |
| `Tabs closed one by one reopen in their old order` | Close two tabs from the middle, reopen twice: original order. |
| `Tabs closed together by close_left reopen in their old order` | `[!mayfail]` BUG-P3-2. |
| `app_session_restart` | Real binary, `--frames 20`, scripted. Run 1 (`--config-dir`, PATH): split right, split down, new tab, navigate; `session.json` has 3 panes, the 4 folders, mode 600, no leftover files. Run 2 (no PATH): dump identical to run 1, saved tab folders unchanged. Run 3: PATH with a session adds a live 5th tab. Run 4: truncated `session.json` -> exit 0, 1 pane on PATH, file moved to `.bad` byte-identical, a fresh 1-pane session written on exit. Run 5: `--no-session` does not restore (2 panes after one split) and leaves the existing file byte-identical; a fresh config dir gets no `session.json`. Run 6: session folders deleted -> exit 0, 3 panes restored. |

## Results

| Run | Result |
|---|---|
| GCC 13 Debug `-Werror`, `ctest -R '^integration\.'` | 36/36 pass; the 2 `BUG-P3-*` cases fail as expected |
| `--repeat until-fail:10`, serial | 10 clean passes (349 s) |
| `-j8 --repeat until-fail:5` | 5 clean passes, twice (once before, once after the last test edit) |
| ASan + UBSan (`NAVIGATOR_SANITIZE=ON`), `--repeat until-fail:3` serial and `-j4 --repeat until-fail:2`, app scripts included | 36/36, no report |
| TSan (GCC 13 RelWithDebInfo, `setarch -R`, `halt_on_error=1`), `--repeat until-fail:3`, `-E app_` | 34/34, 0 `WARNING: ThreadSanitizer` |
| clang-format-18 `--dry-run -Werror`, clang-tidy-18 (`WarningsAsErrors: '*'`) on the two test files | clean |

Timings (GCC Debug): the new Workspace cases take under 0.5 s together; the app script takes
about 3 to 4 s for 7 runs.

TSan notes:

- The app scripts are excluded under TSan: every run of the TSan-built `navigator` reports a
  lock-order inversion inside uninstrumented `libdbus-1` (SDL init in `App::create` vs. teardown
  in `App::~App`), so the binary exits 66. Not Navigator code; same reason the Phase 2 app script
  was not run there. A suppression file (`deadlock:libdbus-1.so`) would let it run.
- The thread-count test creates and joins one thread before taking its baseline: the TSan
  runtime starts a helper thread with the first thread of the process.
- In the TSan dir the `navigator_contract_phase3_cxx20/23` targets do not build: GCC 13 at `-O2`
  reports `-Werror=maybe-uninitialized` on `SplitNode::axis` (`include/navigator/layout.hpp:135`)
  inside a copied `std::variant` in `tests/contracts/phase3_usage.cpp`. Likely a GCC false
  positive; contracts may want an explicit initializer or a local pragma. Build the needed targets
  explicitly there (`ninja navigator_browser_integration_tests navigator_integration_tests navigator`).

## Bugs

### BUG-P3-1 (low, ui): a never-activated tab loses `"scroll": 0` on the next save

Test: `Save, restore, save again without activating background tabs is a fixed point`.

Repro: build any session whose background tabs were listed (so they are saved with
`"scroll": 0.0`), save, `load_session`, `Workspace::start`, settle, `sync()`, `to_json`. Every
background tab that was never activated is now written without `"scroll"`; the visible tabs keep
it. Save -> load -> save is not a fixed point.

Cause: `BrowserModel::tab_state` (src/ui/browser/browser_model.cpp) for an unloaded model writes
`scroll_rows` only when `unloaded->scroll > 0.0F`, and `restore()` turns `scroll_rows = 0` into
`target.scroll = 0`. So "top of the list" (0) becomes "scroll to the cursor" (nullopt) after one
restart without visiting the tab: a tab scrolled to the top with the cursor further down comes back
scrolled to the cursor on the second restart. Fix: keep `std::optional<float>` for the target
scroll (or a has-scroll flag) so 0 survives.

### BUG-P3-2 (low, ui): tabs closed together by `tab.close_left` reopen out of order

Test: `Tabs closed together by close_left reopen in their old order`.

Repro: one pane with tabs A B C D, `tab.close_left` on D (bar: D), then `tab.reopen_closed` three
times. Expected A B C D; actual A D B C.

Cause: `Workspace::execute(CloseLeftTabs)` (src/ui/workspace/workspace.cpp) pushes A@0, B@1, C@2
in that order, indices taken from the old bar. Reopen pops the newest first, so C@2 goes into [D]
(clamped to index 1), then B@1, then A@0. Single closes are fine because each index is recorded in
the bar as it was at that close. Fix: push right to left (C@2, B@1, A@0), which makes LIFO reopen
rebuild the bar exactly; Search Tabs then lists A as the newest closed tab. `pane.close` pushes in
tab order with the dead pane's id, so (from reading the code, not tested) its tabs reopen after the active tab in reverse order
(T(n-1) first, then T(n-2) after it, ...); no spec covers that case, noted here only.

## Gaps

- The dump of `--debug-script dump:` has folders and the live flag only; sort, hidden and cursor
  across a restart are covered by the Workspace test and by grepping `session.json`, not by the
  app dump.
- Pointer paths (drag-to-split, splitter drag, middle-click) are covered by `tests/ui`
  (`workspace_logic_test.cpp`), not end to end here.

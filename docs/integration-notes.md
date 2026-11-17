# Integration notes

Each role appends its own section. Never rewrite another role's section.

## qa

- Root `CMakeLists.txt`: call `enable_testing()` (or `include(CTest)`), then
  `add_subdirectory(tests/smoke)` **after** `add_subdirectory(src/app)`. Otherwise
  `if(TARGET navigator)` in `tests/smoke/CMakeLists.txt` is false and `smoke.app_headless` is
  silently not registered.
- `tests/smoke` expects these targets from the root: `Catch2::Catch2WithMain` (Catch2 v3, and its
  `Catch` CMake module on `CMAKE_MODULE_PATH`, which `find_package(Catch2 3)` provides),
  `navigator_headers` (INTERFACE, include dir `include/`), `navigator_options` (INTERFACE,
  warnings + C++ standard).
- The app target must be named `navigator` and accept `--frames N`; the test runs it with
  `SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software`, TIMEOUT 30.
- The `dev` preset must set `CMAKE_EXPORT_COMPILE_COMMANDS=ON` (needed by `scripts/run-tidy.sh`).
- CI: run `scripts/check-format.sh` and `scripts/run-tidy.sh build/dev`. Both exit non-zero on findings.
- Phase 1: add `navigator_add_subdir(tests/integration)` to the root `CMakeLists.txt` right after
  `navigator_add_subdir(tests/platform)`. It must stay inside the root project scope so it gets the
  root's `CMAKE_CXX_STANDARD 23`. A TU built with C++20 picks `detail::Expected`, not `std::expected`,
  and crashes at runtime against the C++23 libraries (types.hpp: one `Result` definition).

## ui (Phase 2)

- `src/ui/CMakeLists.txt` adds `navigator_ui_browser` (headless: `BrowserModel`, listing/watch
  workers, formatting, i18n catalog). It links `navigator::core`, `navigator::platform` and
  `Threads::Threads`; `navigator_ui` links it PUBLIC. So `src/core` and `src/platform` must be
  added (they are) before `src/ui` in the root `CMakeLists.txt`.
- `src/app/CMakeLists.txt`: `navigator_app_cli` now also builds `bench_fixture.cpp` (dev-only
  `--bench-scroll N` fixture under `$TMPDIR`, removed on exit).
- `src/app/main.cpp` (assigned to ui this phase): `--no-animations` is `AppConfig::animations`
  (no env export any more); a positional `PATH` becomes `AppConfig::start_path`; hidden dev flags
  `--debug-script` / `--bench-scroll` export `NAVIGATOR_DEBUG_SCRIPT` /
  `NAVIGATOR_DEBUG_BENCH_SCROLL`.
- Removed: `src/ui/input/keybindings.*`, `src/ui/shell/keyboard.*`, `src/ui/shell/scene.*`,
  `tests/ui/keybindings_test.cpp`. New tests in `tests/ui`; the gate smoke is
  `ui.smoke_navigate_into_subfolder_and_back`.
- Screenshots with `--scale 2` need a GPU-path renderer under xvfb (`SDL_RENDER_DRIVER=opengl`):
  the SDL software renderer on X11 keeps the pre-resize 1280x800 surface (also true of the
  Phase 0 binary).

## ui (Phase 3)

- `src/ui/CMakeLists.txt`: `navigator_ui_browser` gains the headless workspace sources
  (`workspace/{workspace,pane_geometry,tab_strip,drag_drop,tab_search,session_writer}.cpp`);
  `navigator_ui` gains `workspace/{workspace_view,popups}.cpp`. No new external dependency
  (the layout JSON stays inside `navigator::core`).
- `src/app/main.cpp` / `cli.cpp` (assigned to ui): `--no-session` (clears `restore_session` and
  `save_session`), `--config-dir DIR` (`AppConfig::config_dir`, a view into `CliOptions`),
  `--size` clears `restore_window_geometry`.
- Headless safety: `App` neither reads nor writes the session when `max_frames > 0` and no
  `config_dir` is given, so `smoke.app_headless`, `integration.app_headless_start_paths` and
  other `--frames` runs never touch `~/.config/navigator`. Dev runs that need a session pass
  `--config-dir`.
- New gate test `ui.smoke_session_restores_three_panes` (`tests/ui/session_smoke.sh`, needs the
  `navigator` target, dummy video driver, temp `--config-dir`).
- `--debug-script` gains `type:`, `focus:`, `tab:`, `hovertab:`, `dragpreview:` and `dump:` steps
  (documented at the top of `src/ui/shell/app.cpp`).
- For qa: `integration.docs/keymap.md and load_default_keymap agree row for row` and
  `integration.Every default chord resolves ...` (`tests/integration/keymap_e2e_test.cpp`) fail
  since the commands agent added the Phase 3 rows: the doc parser and the per-command effect
  table only know the Phase 2 rows. Not caused by src/ui.

## ui (Phase 4)

- `src/ui/CMakeLists.txt`: `navigator_ui_browser` gains `path/{copy_path,path_entry,path_worker}.cpp`,
  `sidebar/{places_model,sidebar_model}.cpp`, `widgets/text_edit.cpp`; `navigator_ui` gains
  `sidebar/sidebar_view.cpp`, `widgets/text_field.cpp`, `workspace/goto_popup.cpp`,
  `shell/debug_volumes.cpp`. No new external dependency.
- `tests/ui`: `sidebar_test.cpp`, `path_entry_test.cpp`, and the gate smoke
  `ui.smoke_goto_downloads_and_root` (`tests/ui/goto_smoke.sh`: temporary HOME and --config-dir).
- `bookmarks.json` / `recents.json` are written to the config dir on quit only when their store
  changed during the run (an untouched store never creates a file); headless `--frames` runs
  without `--config-dir` neither read nor write them (same rule as the session).
- For qa: `integration.app_session_restart` (run 1 navigates, so `recents.json` is now a legitimate
  file next to `session.json`) and the two keymap e2e tests (`docs/keymap.md` gained the Phase 4
  table: 4 phase tables, 54 bound rows; `view.toggle_sidebar` etc. need effect checks) need updates.
- `--debug-script` gains `key:CHORD`, `sbhover:ROW`, `sbfocus[:ROW]`, `sbmenu:ROW`; dev-only
  `NAVIGATOR_DEBUG_VOLUMES=<dir>` feeds a fake volume table (documented in `shell/app.cpp`).

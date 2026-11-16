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

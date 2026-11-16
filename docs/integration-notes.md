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

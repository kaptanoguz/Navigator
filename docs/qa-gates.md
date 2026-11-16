# QA gates

Phase 0 gates are below. Phase 1 adds the gates in "Phase 1 additions", Phase 2 those in
"Phase 2 additions"; every earlier gate still applies.

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

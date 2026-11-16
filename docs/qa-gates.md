# QA gates — Phase 0

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

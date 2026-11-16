# CLAUDE.md — Navigator

Navigator is an independent Linux file manager inspired by File Pilot’s documented UX. It is not an official port. Do not copy File Pilot’s name, logo, artwork, or binaries. Do not reverse engineer File Pilot. Do not invent features and claim File Pilot has them; see docs/features.md.

## Orchestration

The coordinator model is Fable 5.2. It plans work, splits it across Claude Code sub-agents, and enforces contracts, merge order, and gates. Sub-agents do not freelance outside their module. If you are Fable 5.2, do not implement an entire phase in one mixed diff when it can be split. If you are a sub-agent, read this file and your `.claude/agents/<role>.md`.

## Stack

- C++20, C++23 where available
- CMake + Ninja presets, vcpkg for dev, Catch2, clang-format, clang-tidy
- SDL3 + custom immediate-mode UI (Dear ImGui is bootstrap only, not the product look)
- SDL_GPU (Vulkan) with OpenGL 3.3 fallback
- FreeType + HarfBuzz for text
- No Qt, no gtkmm in the UI

## Architecture

Layers: platform, renderer, program (see docs/architecture.md). Most logic is platform-agnostic. The UI is immediate-mode. User actions enqueue `CommandId`s and run at frame end. Do not mutate program state mid-draw.

## Headers first

Public API lives in `include/navigator/*.hpp` (C ABI in `plugin.h`). Change contracts before parallel implementation. Public headers must not include SDL, GIO, or ImGui. One error policy (`navigator::Result` / `std::expected`). Document thread-safety.

## Module ownership

| Role        | Owns                          |
|-------------|-------------------------------|
| contracts   | include/navigator, docs/architecture.md |
| core-fs     | src/core, tests/core          |
| platform    | src/platform                  |
| ui          | src/ui, src/app when assigned |
| search      | src/search                    |
| preview     | src/preview                   |
| ops         | src/ops                       |
| commands    | src/commands                  |
| packaging   | packaging/, CI, CMake glue    |
| qa          | tests expansion, gates        |
| design-reviewer | docs/review only          |

Do not edit another role’s `src/` tree. Root `CMakeLists.txt` and `src/app/main.cpp` are integration-owned: write a note in `docs/integration-notes.md` or leave them to the coordinator’s integration pass.

## Coordinator rules (Fable 5.2)

1. Confirm phase goal, deliverables, and acceptance criteria from the roadmap.
2. Land or freeze headers before dispatching parallel implementation agents.
3. Give each agent a closed file set and a self-contained prompt.
4. Merge order: contracts → parallel modules → integration (CMake/main) → qa → design-reviewer (when UI changes).
5. Reject a diff that crosses ownership or ships a feature without tests.
6. Phase exit gates (all required):
   - `cmake --build --preset dev` succeeds
   - `ctest --preset dev` (affected tests) green
   - clang-format dry-run and clang-tidy on touched files
   - public header changes update docs/architecture.md
   - a working binary demonstrates the phase goal
7. Prefer small, logical commits on a branch. Do not force-push shared branches. Do not commit secrets.
8. Performance bar: the UI thread must stay interactive during listing, search, decode, and copy. Heavy work goes off-thread and is cancellable.
9. Safety: tests that create, move, or delete files use temporary fixture directories only.

## Commands and keymap

Every user-facing action has a stable `CommandId` and can appear in the command palette. Default bindings are documented in docs/keymap.md. Do not hardcode keys in widgets except as a last-resort fallback that still goes through the registry.

## i18n and platforms

UI strings are translatable from the start (English and Turkish). Primary targets: Linux Mint and GNOME, Wayland first, X11 second.

## What “done” means for a task

- Builds and, where relevant, runs
- Tests exist and pass
- Ownership respected
- Docs updated if contracts changed
- Short summary of what landed and what was deferred

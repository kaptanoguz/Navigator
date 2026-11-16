# Navigator architecture

Owner: `contracts`. Any change to `include/navigator/**` must update this file in the same diff.

## Layers

| Layer    | What lives there | Modules |
|----------|------------------|---------|
| program  | Platform-agnostic logic: commands, listing/sort model, search, ops queue, preview decoding, i18n. Most code. | `src/commands`, `src/core`, `src/search`, `src/ops`, `src/preview` |
| renderer | Window, GPU device, immediate-mode drawing, glyph atlas. Only layer that sees SDL/GPU/FreeType types. | `src/ui`, `src/app` |
| platform | Linux integration: inotify, mounts, trash, MIME, open-with, XDG, terminal. Only layer that sees GIO/D-Bus. | `src/platform` |

Dependency direction (arrows point at what a module may include):

```
app -> ui -> commands -> types
        \-> i18n
ui -> fs / watch / volume / search / index / ops / preview / platform   (via public headers only)
core-fs, platform, search, ops, preview -> types
```

Nothing below `ui` depends on `ui`. Modules talk only through `include/navigator/*.hpp`; no module includes another module's `src/` headers.

## Rendering decision (Phase 0)

- SDL3 window, `SDL_Renderer` created with the `"gpu"` (SDL_GPU) driver, which prefers Vulkan; fall back to `"opengl"`, then `"software"`. The chosen driver is logged.
- Text: FreeType rasterizes, HarfBuzz shapes, glyphs are packed into an atlas texture drawn as textured quads.
- Custom immediate-mode UI. Dear ImGui may be used only as a temporary debug/bootstrap aid and never defines the product look.
- Wayland first (`SDL_VIDEODRIVER` unset, SDL picks Wayland), X11 second. Headless tests run with `SDL_VIDEODRIVER=dummy` (or offscreen) and `AppConfig::max_frames > 0`.

## Frame model

1. Poll input/events.
2. Build UI (immediate mode). Widgets read state and call `CommandQueue::enqueue()`; they never mutate program state.
3. Render.
4. At frame end, `CommandQueue::drain()` runs all pending commands in FIFO order. Commands enqueued by a handler during the drain run on the next drain.

Heavy work (listing, search, decode, copy) runs on worker threads, is cancellable, and reports back to the UI thread, which turns results into state changes between frames.

## Error policy

- One policy: fallible functions return `navigator::Result<T>` (`types.hpp`). No exceptions across module boundaries.
- `Result<T>` = `std::expected<T, Error>` when `__cpp_lib_expected >= 202211L` (GCC 13 with `-std=c++23`); otherwise `navigator::detail::Expected<T, Error>` (C++20, and Clang 18 + libstdc++ 13, which clang-tidy uses). `NAVIGATOR_HAS_STD_EXPECTED` tells which.
- Use only the common subset: `has_value()`, `explicit operator bool`, `*`, `->`, `value()`, `error()`, `value_or()`. No monadic members, no comparisons.
- Create errors with `return navigator::fail(ErrorCode::X, "detail");` (returns `navigator::Unexpected<Error>`, convertible to any `Result<T>`). Void success: `return navigator::ok();`.
- `Error::message` is English developer detail; UI shows translated text chosen from `ErrorCode`.
- All TUs of one build use the same compiler and `-std` so `Result` has one definition.

## Thread-safety conventions

- Every public header states its rule in the top comment.
- UI-thread-only: `App`, `CommandQueue`, `CommandRegistry`, `set_language()`.
- Reentrant free functions (`fs`, `volume`, `platform`) may be called from workers and may block; never call them on the UI thread.
- Interfaces (`IWatcher`, `IIndex`, `IPreviewProvider`) document their own threading; cancellation uses `std::atomic<bool>` or a later token type.

## Public headers

| Header | Purpose | Status |
|--------|---------|--------|
| `types.hpp` | `Path`, `FileId`, `Span`, `Arena` (decl), `ErrorCode`, `Error`, `Result`, `fail`, `ok` | Phase 0, concrete |
| `detail/expected.hpp` | `std::expected` fallback used by `Result` | Phase 0, concrete |
| `commands.hpp` | `CommandId`, `Command`, `CommandQueue`, `CommandInfo`, `CommandRegistry` | Phase 0, implemented in `src/commands` |
| `ui.hpp` | `AppConfig`, `Theme`, `FrameStats`, `App` (pimpl) | Phase 0, implemented in `src/ui`/`src/app` |
| `i18n.hpp` | `Language`, `set_language`, `language`, `tr` | Phase 0, implemented in `src/ui` |
| `fs.hpp` | `fs::DirEntry`, `EntryKind`, `SortKey`, `ListOptions`, `list_directory`, `stat`, `sort_entries` | stub, Phase 1 |
| `watch.hpp` | `WatchEvent`, `WatchEventKind`, `IWatcher`, `create_watcher` | stub, Phase 1 |
| `volume.hpp` | `VolumeInfo`, `list_volumes`, `volume_for` | stub, Phase 1 |
| `platform.hpp` | `platform::open_with_default`, `move_to_trash`, `mime_type`, `open_terminal`, `user_dir` | stub, Phase 1+ |
| `search.hpp` | `SearchQuery`, `MatchMode`, `SearchHit` | stub, search phase |
| `index.hpp` | `IIndex` | stub, search phase |
| `ops.hpp` | `OpKind`, `ConflictPolicy`, `OpRequest`, `OpProgress`, `OpId` | stub, ops phase |
| `preview.hpp` | `PreviewRequest`, `PreviewResult`, `IPreviewProvider` | stub, preview phase |
| `plugin.h` | C ABI: `NAVIGATOR_PLUGIN_ABI_VERSION`, `NavigatorPluginInfo`, entry symbol | stub |

Stub functions are declared only; nothing may call them until their phase lands. Public headers never include SDL, GIO, ImGui, FreeType or HarfBuzz. `tests/contracts` compiles each header alone (C++20 and C++23, `-Wall -Wextra -Wpedantic -Werror`) plus `plugin.h` as C11; target `navigator_header_check`.

## Commands

- `CommandId` values are stable and never renumbered. Phase 0: `None = 0`, `Quit = 1` (`app.quit`), `ToggleFpsOverlay = 2` (`view.toggle_fps_overlay`), `ToggleTheme = 3` (`view.toggle_theme`). 1000+ reserved for later phases.
- i18n title keys: `cmd.` + name (`cmd.app.quit`, ...).
- Every user action has a `CommandId`, appears in the palette, and gets its default binding from `docs/keymap.md`.

## Module ownership

| Role | Owns |
|------|------|
| contracts | `include/navigator/**`, `docs/architecture.md`, `tests/contracts/**` |
| core-fs | `src/core`, `tests/core` |
| platform | `src/platform`, `tests/platform` |
| ui | `src/ui`, `src/app` when assigned, `assets/` |
| search | `src/search`, `tests/search` |
| preview | `src/preview`, `tests/preview` |
| ops | `src/ops`, `tests/ops` |
| commands | `src/commands`, `tests/commands` |
| packaging | `packaging/`, CI, CMake glue |
| qa | `tests/**` expansion, gates |
| design-reviewer | `docs/review/**` |

Root `CMakeLists.txt` and `src/app/main.cpp` are integration-owned (coordinator); others leave notes in `docs/integration-notes.md`.

## Merge rules

contracts -> parallel modules -> integration (root CMake, `main`) -> qa -> design-reviewer (UI phases). A diff that crosses ownership or ships a feature without tests is rejected. Breaking header changes need coordinator approval and a note here.

## Public types introduced in Phase 0

`Path`, `Span`, `FileId`, `Arena` (decl), `ErrorCode`, `Error`, `Result`, `Unexpected`, `fail`, `ok`, `to_string(ErrorCode)`, `detail::Expected`, `detail::Unexpected`;
`CommandId`, `Command`, `CommandQueue`, `CommandInfo`, `CommandRegistry`, `register_builtin_commands`, `to_string(CommandId)`;
`Theme`, `AppConfig`, `FrameStats`, `App`, `Pane`/`Tab`/`Layout` (decl);
`Language`, `set_language`, `language`, `tr`;
stubs: `fs::EntryKind`, `fs::DirEntry`, `fs::SortKey`, `fs::ListOptions`, `fs::list_directory`, `fs::stat`, `fs::sort_entries`, `WatchEventKind`, `WatchEvent`, `WatchHandle`, `IWatcher`, `create_watcher`, `VolumeInfo`, `list_volumes`, `volume_for`, `platform::open_with_default`, `platform::move_to_trash`, `platform::mime_type`, `platform::open_terminal`, `platform::UserDir`, `platform::user_dir`, `MatchMode`, `SearchQuery`, `SearchHit`, `IIndex`, `OpKind`, `ConflictPolicy`, `OpRequest`, `OpId`, `OpProgress`, `PreviewRequest`, `PreviewResult`, `IPreviewProvider`, `NavigatorPluginInfo`, `NavigatorPluginGetInfoFn`.

## Contract changelog

- Phase 0 follow-up: `AppConfig::show_fps` now defaults to `false` so a default launch idles on events (design review). `--fps` turns the overlay on.

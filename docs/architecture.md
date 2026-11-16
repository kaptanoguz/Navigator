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
ui -> fs / watch / volume / mime / search / index / ops / preview / platform   (via public headers only)
core-fs, platform, search, ops, preview -> types
platform (mime.hpp) -> fs.hpp (DirEntry / is_directory_like only, header-level; no src/core calls)
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

## Phase 2 browser data flow

The single-pane Details browser keeps its state in a **`BrowserModel`, internal to `src/ui`** (not a public header; it becomes one only if a second module needs it, e.g. tabs/panes in Phase 3). It has no SDL/renderer dependency and is unit-tested headless (`tests/ui`).

`BrowserModel` holds:

- `current_dir` (`Path`), the listing generation number, and the last listing error (`std::optional<Error>`).
- `entries`: `std::vector<fs::DirEntry>`, always sorted with `fs::sort_entries` and the current `fs::SortOptions` (default: Name, folders first). The listing worker lists with `show_hidden = true` and filters hidden entries itself, so the status bar and the hidden-only empty state know the hidden count (cost: one stat per hidden entry); `ToggleHidden` re-filters/re-lists.
- Selection: a per-entry flag (or bitset) indexed like `entries`, plus `cursor` and `anchor` row indices. Across re-sorts, refreshes and watch updates, selection, cursor and anchor are carried over **by entry name**, never by stale index.
- Scroll offset (in rows or pixels; the widget decides) and the visible page size used by `CursorPageUp/Down`.
- History: back and forward stacks of locations; each location remembers the cursor entry name so Back/Forward/Up restore the cursor (Up puts the cursor on the folder it came from).
- Derived values for the status bar: entry count, selected count, selected total size (files only; folders count as 0 until recursive sizes exist).

Per frame (extends "Frame model" below):

1. **Frame start, before input**: apply finished work. (a) A listing result from the worker is applied only if its generation matches the latest request (stale results are dropped). (b) Watch events for the current directory are applied (see below).
2. Poll input. Key events become `KeyChord`s and go through `resolve_command(keymap, registry, chord)`; a hit is enqueued. Mouse gestures with positions (click row *i*, Ctrl/Shift-click, rubber band rectangle) are recorded by the list widget during drawing as ui-internal **pointer intents** (not `CommandId`s: they carry a row or rectangle and are not palette actions).
3. Build UI from the model (read-only): the list is virtualized, only visible rows are laid out and shaped, so 50k entries cost the same per frame as 50. The Type column uses `mime::guess_for_entry` + `mime::type_label_key` + `tr()` for visible rows only (cache per entry if needed).
4. Render.
5. **Frame end**: apply pointer intents in order, then drain the command queue. Double-click = click intent + enqueued `list.activate`, so Activate sees the clicked row as cursor.

Listing runs off the UI thread: a ui-internal worker (`std::jthread`) calls `fs::list_directory(dir, options, stop_token)` and `fs::sort_entries`, then posts the result into a mutex-protected mailbox and wakes the event loop (an SDL user event; `SDL_PushEvent` is thread-safe). A new navigation requests stop on the previous listing and bumps the generation. While a listing is pending the old entries stay visible (no flash to empty).

Navigation rules: `NavigateBack/Forward/Up` and Activate on a folder request a listing of the target. If the listing **fails** (PermissionDenied, NotFound, ...) the browser stays on the current directory, history is unchanged, and an error notice is shown; the notice text is `tr("error." + to_string(code))` (e.g. `error.permission_denied`) plus the target path. A failed start path (`AppConfig::start_path`) instead opens an empty list with the notice. Activate on anything that is not `fs::is_directory_like` calls `platform::open_with_default(current_dir / name)` (absolute path); an error from it is shown as a notice too.

Watching: the ui owns one `IWatcher` (from `create_inotify_watcher`, injectable via `WatcherFactory`) on the current directory, re-targeted on navigation. Because `IWatcher` has no thread and is single-thread-at-a-time, a ui-internal watch worker owns it: it blocks in `wait()`, calls `poll()`, `fs::stat`s created/changed children (stat is blocking I/O and stays off the UI thread), coalesces a burst (implementation choice, e.g. 50-100 ms), and posts the batch to the mailbox. At frame start the UI thread applies it: Created/MovedTo insert (sorted position), Deleted/MovedFrom remove, Modified/Attrib replace, Renamed rename; Overflow, or a batch too large to patch cheaply, triggers a full re-list; WatchedDirGone shows a notice and keeps the stale list until the user navigates.

The UI never calls `std::filesystem` for directory contents or metadata; it uses `navigator::fs`, `navigator::platform` and `navigator::mime` (path arithmetic such as `parent_path()` and `operator/` on `Path` is fine).

Work split for Phase 2: **commands** implements the new ids, `to_string`, the keymap types, `parse_key_chord` and `load_default_keymap` (src/commands, tests/commands); **platform** implements `IAppLauncher`/`create_default_launcher`, `open_with_default`, `mime_type`, `user_dir` (Home/Config/Cache) and `mime.hpp` (src/platform, tests/platform); **ui** implements `BrowserModel`, the list widget, status bar, notices, SDL -> `Key` translation, i18n keys (`type.*`, `key.*`, `error.*`, `cmd.*`), `AppConfig::start_path`/`animations`, and removes its private `src/ui/input/keybindings.*` in favour of `Keymap`.

## Error policy

- One policy: fallible functions return `navigator::Result<T>` (`types.hpp`). No exceptions across module boundaries.
- `Result<T>` = `std::expected<T, Error>` when `__cpp_lib_expected >= 202211L` (GCC 13 with `-std=c++23`); otherwise `navigator::detail::Expected<T, Error>` (C++20, and Clang 18 + libstdc++ 13, which clang-tidy uses). `NAVIGATOR_HAS_STD_EXPECTED` tells which.
- Use only the common subset: `has_value()`, `explicit operator bool`, `*`, `->`, `value()`, `error()`, `value_or()`. No monadic members, no comparisons.
- Create errors with `return navigator::fail(ErrorCode::X, "detail");` (returns `navigator::Unexpected<Error>`, convertible to any `Result<T>`). Void success: `return navigator::ok();`.
- `Error::message` is English developer detail; UI shows translated text chosen from `ErrorCode`.
- All TUs of one build use the same compiler and `-std` so `Result` has one definition.

## Thread-safety conventions

- Every public header states its rule in the top comment.
- UI-thread-only: `App`, `CommandQueue`, `CommandRegistry`, `Keymap`, `set_language()`.
- Reentrant free functions (`fs`, `volume`, `platform`) may be called from workers and may block; never call them on the UI thread. One documented exception: `platform::open_with_default` may run in a frame-end command handler (one stat of a just-listed path + one detached spawn, never waits for the child).
- Pure functions (`mime::*`, `fs::compare_names`/`sort_entries`, `to_string`/`parse_key_chord`) are callable from any thread, including the UI thread.
- Interfaces (`IWatcher`, `IVolumeMonitor`, `IMountSource`, `IAppLauncher`, `IIndex`, `IPreviewProvider`) document their own threading. `IWatcher`/`IVolumeMonitor` are single-thread-at-a-time objects with no internal thread (drained with `poll()`, optional blocking `wait()` / pollable `native_handle()`).
- Cancellation uses `std::stop_token`; a cancelled call fails with `ErrorCode::Cancelled`.

## Public headers

| Header | Purpose | Status |
|--------|---------|--------|
| `types.hpp` | `Path`, `FileId`, `Timestamp`, `Span`, `Arena` (decl), `ErrorCode`, `Error`, `Result`, `fail`, `ok` | Phase 0, concrete (`Timestamp` Phase 1) |
| `detail/expected.hpp` | `std::expected` fallback used by `Result` | Phase 0, concrete |
| `commands.hpp` | `CommandId`, `CursorArg`, `Command`, `CommandQueue`, `CommandInfo`, `CommandRegistry`; keymap: `Key`, `Modifiers` (+ operators, `has`), `KeyChord`, `to_string(Key/KeyChord)`, `parse_key_chord`, `KeyBinding`, `Keymap`, `load_default_keymap`, `resolve_command` | Phase 0; keymap + Phase 2 ids are a Phase 2 contract, implemented in `src/commands` (commands) |
| `ui.hpp` | `AppConfig` (Phase 2: `animations`, `start_path`), `Theme`, `FrameStats`, `App` (pimpl) | Phase 0, implemented in `src/ui`/`src/app` |
| `i18n.hpp` | `Language`, `set_language`, `language`, `tr` | Phase 0, implemented in `src/ui` |
| `fs.hpp` | `fs::DirEntry`, `EntryKind`, `ListOptions`, `list_directory` (+ `std::stop_token` overload), `stat`, `SortKey`, `SortOptions`, `sort_entries`, `compare_names`, `fold_name`, `to_display_name`, inline `is_hidden_name`/`extension`/`is_directory_like` | Phase 1 contract, implemented in `src/core` (core-fs) |
| `watch.hpp` | `WatchHandle`, `WatchEventKind`, `WatchEvent`, `IWatcher`, `create_inotify_watcher`, `WatcherFactory` | Phase 1 contract, implemented in `src/platform` (platform) |
| `volume.hpp` | `MountEntry`, `parse_mountinfo`, `is_pseudo_filesystem`, `is_network_filesystem`, `VolumeKind`, `SpaceInfo`, `VolumeInfo`, `IMountSource`, `create_proc_mount_source`, `VolumeQuery`, `list_volumes`, `volume_for`, `VolumeEvent(Kind)`, `IVolumeMonitor`, `create_volume_monitor` | Phase 1 contract, implemented in `src/platform` (platform) |
| `platform.hpp` | `platform::LaunchRequest`, `IAppLauncher`, `create_default_launcher`, `open_with_default` (+ launcher overload), `mime_type`, `UserDir`, `user_dir`; stubs `move_to_trash`, `open_terminal` | Phase 2 contract (launcher, open, mime_type, user_dir Home/Config/Cache), implemented in `src/platform` (platform) |
| `mime.hpp` | `mime::k*` constants, `guess_from_name`, `guess_for_entry`, `type_label_key` (name table + Type-column i18n keys) | Phase 2 contract, implemented in `src/platform` (platform); label texts in the ui i18n catalogs |
| `search.hpp` | `SearchQuery`, `MatchMode`, `SearchHit` | stub, search phase |
| `index.hpp` | `IIndex` | stub, search phase |
| `ops.hpp` | `OpKind`, `ConflictPolicy`, `OpRequest`, `OpProgress`, `OpId` | stub, ops phase |
| `preview.hpp` | `PreviewRequest`, `PreviewResult`, `IPreviewProvider` | stub, preview phase |
| `plugin.h` | C ABI: `NAVIGATOR_PLUGIN_ABI_VERSION`, `NavigatorPluginInfo`, entry symbol | stub |

Stub functions are declared only; nothing may call them until their phase lands. Public headers never include SDL, GIO, ImGui, FreeType or HarfBuzz. `tests/contracts` compiles each header alone (C++20 and C++23, `-Wall -Wextra -Wpedantic -Werror`) plus `plugin.h` as C11, and compiles (never links) `result_usage.cpp`, `phase1_usage.cpp` (fake `IWatcher`/`IMountSource` implementations prove the interfaces are injectable) and `phase2_usage.cpp` (keymap, fake `IAppLauncher`, mime, `AppConfig`); target `navigator_header_check`.

## Commands

- `CommandId` values are stable and never renumbered. Ranges: 1-999 app/view, 1000-1099 navigation, 1100-1199 list (activation, cursor, selection), 1200+ later phases.
  - Phase 0: `None = 0`, `Quit = 1` (`app.quit`), `ToggleFpsOverlay = 2` (`view.toggle_fps_overlay`), `ToggleTheme = 3` (`view.toggle_theme`).
  - Phase 2: `ToggleHidden = 4` (`view.toggle_hidden`); `NavigateBack = 1000` (`nav.back`), `NavigateForward = 1001` (`nav.forward`), `NavigateUp = 1002` (`nav.up`), `Refresh = 1003` (`nav.refresh`), `FocusAddressBar = 1004` (`nav.focus_address_bar`, Phase 4, registered and unbound); `Activate = 1100` (`list.activate`), `SelectAll = 1101` (`list.select_all`), `ClearSelection = 1102` (`list.clear_selection`), `CursorUp/Down/PageUp/PageDown/Home/End = 1110..1115` (`list.cursor_up`, `list.cursor_down`, `list.cursor_page_up`, `list.cursor_page_down`, `list.cursor_home`, `list.cursor_end`).
- Variants are `Command::arg` bit flags, not extra ids: cursor commands take `CursorArg::Extend` (Shift: extend the selection from the anchor). The keymap binds a chord to (name, arg).
- The keymap (`Keymap`, `KeyChord`, `Key`, `Modifiers`) lives in the commands module, with no SDL types; src/ui translates SDL keycodes/scancodes (and mouse X1/X2) into `Key`/`Modifiers` with the layout rules documented in `commands.hpp`. Canonical chord text is ASCII (`Ctrl+Shift+L`); the ui translates modifier names for display (`key.ctrl`, `key.shift`, `key.alt`, `key.super`). Multi-key sequences and aliases: Phase 8.
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
- Phase 1 (contracts): `fs.hpp`, `watch.hpp`, `volume.hpp` turned from stubs into implementable contracts; `types.hpp` gains `Timestamp` (`std::chrono::sys_time<nanoseconds>`). No previous caller existed, so the breaking renames are free. `platform.hpp` unchanged (still Phase 1+ stubs).
  - **fs** (core-fs, `src/core`): `DirEntry::name` is a `std::string` of raw on-disk bytes (was `Path`), never normalized; `EntryKind` reduced to `Unknown/File/Directory/Symlink/Other` (exact type in `mode & S_IFMT`); new fields `dangling`, `uid`, `gid`, `readable` (mode-bit hint, no `access()`), `metadata_error` (per-entry stat failure; the listing still succeeds); `mtime_ns` -> `Timestamp mtime`. Sorting moved out of `ListOptions` (now `show_hidden`, `follow_symlinks`) into `SortOptions {key, descending, folders_first, natural, case_insensitive}`; `SortKey` is `Name, Size, Modified, Extension, Count` (Count reserved, sorts as Name; old `Kind` dropped, `Extension` is the Type column). `sort_entries` takes `Span<DirEntry>`.
  - **Symlink policy**: links are never resolved at listing time. Default metadata is the link's own (lstat) plus one target `stat()` to fill `target_kind`/`dangling`; `follow_symlinks` switches the metadata to the target. `is_directory_like()` (dir or link-to-dir) defines the folders-first group.
  - **Name order**: `compare_names` returns `std::weak_ordering`: code point compare after locale-independent folding (Unicode simple case folding C+S, plus U+0130 and U+0131 -> `i`, so I/i/İ/ı are equivalent), natural digit runs only where both sides have digits, invalid UTF-8 bytes as U+DC00+byte, no normalization. `sort_entries` chain: folders_first group (not reversed) -> key -> compare_names -> raw bytes (total, deterministic order); `descending` reverses the key chain.
  - **Errors** (`list_directory`): InvalidArgument (not a dir, ELOOP, ENAMETOOLONG, empty), NotFound, PermissionDenied, OutOfMemory, IoError, Cancelled (stop_token overload, checked at least every 1024 entries).
  - **watch** (platform, `src/platform`): `create_watcher` renamed `create_inotify_watcher`; `WatcherFactory` for injection. `IWatcher::poll(std::vector<WatchEvent>&) -> Result<size_t>` (append), `wait(ms)`, `native_handle()`; `remove` is `noexcept` and refcounted with `add`. Events carry `handle` + child `name` (+ `old_name`, `is_directory`) instead of full paths. Kinds: Created, Deleted, Modified, Attrib, MovedFrom, MovedTo, Renamed (same-handle cookie pairing within one poll), Overflow (re-list all), WatchedDirGone (handle auto-removed).
  - **volume** (platform, `src/platform`): everything flows through `IMountSource` (mountinfo text, statvfs, optional POLLPRI `change_fd`) plus `VolumeQuery::sysfs_root`/`devfs_root`/`home_dir`, so CI fakes the whole environment. The header documents the exact pipeline: stacked/bind de-duplication, pseudo-fs and system-path filter, removable rule (sysfs `removable`, USB in the device path, `/media` or `/run/media` mount point), label/uuid via `/dev/disk/by-label|by-uuid`, and kind order Virtual > Network > Root > Home > Removable > Internal > Other. `volume_for` uses longest mount-point prefix of the `weakly_canonical` path over all mounts. `IVolumeMonitor` diffs snapshots by (mount_point, mount_id) into Mounted/Unmounted/Changed.
- Phase 1 follow-up (volume classification, coordinator-approved, no signature change): rule R3 (`/media`, `/run/media` mount point) now applies only when R1 cannot resolve the device to a sysfs block device (non-`/dev/` source, device-mapper, or no sysfs entry); a resolved disk with `removable=0` and no USB path is not removable (internal NTFS partitions under `/media`). `is_pseudo_filesystem` adds `fuse.lxcfs`; `fuse.gvfsd-fuse` stays hidden until the GVfs phase. Pipeline step 3 hides everything at or below `/var/lib` (was `/var/lib/docker`) and mounts whose mount point exists and is not a directory (single-file bind mounts, e.g. Waydroid's `waydroid.prop`; stat(2) skipped for network filesystems). `include_hidden` still shows all of them.
- Phase 2 (contracts): single-pane browser contracts. No signature of an existing implemented function changed; additions only, except that `platform.hpp` stubs became concrete contracts (no previous caller).
  - **commands** (commands, `src/commands`): new stable ids `ToggleHidden = 4`, `NavigateBack = 1000`, `NavigateForward = 1001`, `NavigateUp = 1002`, `Refresh = 1003`, `FocusAddressBar = 1004` (reserved, unbound), `Activate = 1100`, `SelectAll = 1101`, `ClearSelection = 1102`, `CursorUp..CursorEnd = 1110..1115`; `CursorArg::{None, Extend}` bit flags for `Command::arg`; `Command` gains `operator==`. Keymap moved from the ui's private table into the contract: `Key` (`uint16_t`; letters, top-row digits, F1-F24, navigation/editing keys, KeypadEnter, punctuation, MouseBack/MouseForward), `Modifiers` (`uint8_t` flags Ctrl/Shift/Alt/Super with `| & ^ ~ |= &= ^=`, `has`, `kModifierMask`), `KeyChord` (exact-match, `==`), `to_string(Key)`, `to_string(KeyChord)` (canonical ASCII, order Super+Ctrl+Alt+Shift+key), `parse_key_chord`, `KeyBinding {chord, command, arg}`, `Keymap {bind (InvalidArgument / AlreadyExists), unbind, clear, resolve, chords_for(name), chords_for(name, arg), bindings}`, `load_default_keymap` (all-or-nothing), `resolve_command(keymap, registry, chord) -> optional<Command>`. Defaults in `docs/keymap.md`.
  - **platform** (platform, `src/platform`): `LaunchRequest {argv, working_dir}`, `IAppLauncher` (detached, setsid, no inherited fds except 0-2 -> /dev/null, never waits), `create_default_launcher`, `open_with_default(path, launcher)` (absolute + existing path; `xdg-open`, falling back to `gio open` only on NotFound; Unsupported if neither) and `open_with_default(path)` with a lazily created process-wide launcher; `mime_type` defined as lstat/stat + `mime::guess_for_entry` (no content sniffing in Phase 2); `user_dir` concrete for Home/Config/Cache, the XDG user dirs may return Unsupported until Phase 4. `move_to_trash`, `open_terminal` remain stubs.
  - **mime** (new header, platform, `src/platform`): `navigator::mime` constants, `guess_from_name` (fixed extension table in the header, ASCII case-insensitive, compound `tar.*` first), `guess_for_entry` (inode types, links, executables), `type_label_key` (i18n keys `type.*`, English texts listed in the header; ui adds both catalogs).
  - **ui** (ui, `src/ui`): `AppConfig::animations` (default true; `NAVIGATOR_NO_ANIMATIONS=1` still forces false) and `AppConfig::start_path` (empty = home). Phase 2 data flow documented above; `BrowserModel` is ui-internal.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` in `src/commands/builtin_commands.cpp` (`to_string`) and `src/ui/shell/app.cpp` (`App::Impl::execute`) fail `-Werror=switch`; `tests/ui/keybindings_test.cpp` gets an ambiguous `KeyBinding` (it has `using namespace navigator; using namespace navigator::ui;`) and goes away with `src/ui/input/keybindings.*`.
- Phase 2 integration: the browser counts hidden entries (worker lists with `show_hidden = true` and filters), and re-sorting re-lists off the UI thread.

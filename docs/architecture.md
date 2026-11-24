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
layout.hpp -> fs.hpp (SortOptions), types;  src/core (layout impl) -> nlohmann_json (PRIVATE)
path_input.hpp -> fs.hpp, types
places.hpp -> volume.hpp (VolumeInfo/VolumeIdentity, inline helpers), platform.hpp (UserDirs
              value type only), types;  src/core (places impl) -> nlohmann_json (PRIVATE)
walk.hpp -> fs.hpp, types                                    (Phase 5; src/core)
search.hpp -> fs.hpp, walk.hpp (WalkEntry), types            (Phase 5; src/search)
index.hpp -> search.hpp, volume.hpp (VolumeInfo/VolumeIdentity values, inline policy),
             watch.hpp (WatcherFactory), fs.hpp, types       (Phase 5; src/search)
layout.hpp -> search.hpp (TypeFilter value only; src/core calls nothing in src/search)
src/search -> navigator_core (fold_name_into, compare_names, Walker); never src/platform
preview.hpp -> fs.hpp, types                                 (Phase 6; src/preview)
thumbnail.hpp -> preview.hpp, platform.hpp (IProcessRunner seam only), fs.hpp, types
preview_service.hpp -> preview.hpp, thumbnail.hpp, platform.hpp (IProcessRunner), types
layout.hpp (InspectorState) and ui.hpp (AppConfig) do not include the preview headers
src/preview -> navigator_core (list_directory, sort_entries) + decoder libs PRIVATE; never
               src/platform (runner, cache root, data dirs injected; MIME filled by the ui)
trash.hpp -> volume.hpp (VolumeQuery), fs.hpp (EntryKind), types   (Phase 7; src/platform)
transfer.hpp -> types                                             (Phase 7; src/platform, pure)
ops.hpp -> trash.hpp (ITrash interface, TrashedItem values), fs.hpp, types (Phase 7; src/ops)
src/ops -> navigator_core only; ITrash injected (never src/platform headers)
```

`src/core` implements `places.hpp` and `path_input.hpp` without calling any `src/platform`
function: volume lists, user dirs, the environment and `~user` lookups come in as values or
callbacks.

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

## Phase 3: tabs, split panes, layouts, session

### Ownership

| Piece | Where | Role |
|-------|-------|------|
| Layout model: ids, `TabState`, tree, `Layout` operations, `ClosedTabs`, `Session` | `include/navigator/layout.hpp`, implemented in `src/core` (e.g. `layout.cpp`) | core-fs |
| JSON (de)serialization, session and named-layout files, atomic writes | `src/core` (e.g. `layout_json.cpp`, `layout_files.cpp`), links `nlohmann_json::nlohmann_json` PRIVATE | core-fs |
| Tests: tree ops, invariants, JSON round trip, lenient load, files in temp dirs | `tests/core` | core-fs (qa extends) |
| New ids, names, `to_string`, default chords | `src/commands`, `tests/commands` | commands |
| Tab bars, pane rectangles, splitters, drag and drop, popups, dimming, per-tab `BrowserModel`s, session timing, `AppConfig` fields, i18n (`cmd.*`, popup texts) | `src/ui`, `src/app` | ui |
| `--no-session`, `--config-dir DIR`, `--size` clears `restore_window_geometry` | `src/app/main.cpp` | integration |

`layout.hpp` is headless and pure data: no SDL, no JSON types, no XDG lookups. `src/core` keeps
depending on `types`/`fs` only; the caller passes the config root (`platform::user_dir(Config) /
"navigator"`, or `AppConfig::config_dir`). nlohmann_json appears only in `src/core/*.cpp`, never
in a public header.

### Model ownership at runtime

The UI thread owns one `layout::Session` (window geometry, `Layout`, `ClosedTabs`). The `Layout`
is the single source of truth for which panes and tabs exist, their order, the split sizes and
the active pane/tab. The ui keeps, next to it, a map `TabId -> BrowserModel` (still ui-internal)
and per-pane view state (tab-bar scroll, hover). Pixel rectangles are computed every frame from
the tree and the window size (sizes are fractions; minimum pane size is enforced by passing
`min_pane_px / extent_px` to `move_splitter` and by the renderer clamping when the window is
small).

Rules:

- Ids are fresh for every loaded or rebuilt layout. Replacing the layout (load named layout,
  restore) drops every `BrowserModel` and creates new ones from the `TabState`s.
- A `TabState` is the persisted subset of a `BrowserModel`: folder, `SortOptions`, show-hidden,
  cursor name, scroll in rows. The ui writes it back into `Layout::tab_state(id)` whenever those
  change (navigation, sort, toggle hidden, cursor move, scroll) and when a tab is deactivated.
  Back/forward history, selection and the listing are runtime-only (duplicate copies them,
  persistence does not).
- The layout never becomes visibly empty: `tab.close` on the last tab of the last pane is a
  no-op; if a loaded session or layout has no valid tab, the ui adds one tab on the start folder.

### One BrowserModel per tab, lazily active

- Only the **active tab of each pane** (one per visible pane) has a live listing and an
  `IWatcher` target. Background tabs keep their `BrowserModel` state (entries, selection,
  history, cursor, scroll) but their listing worker is stopped and they are not watched.
- Activating a background tab re-lists its folder off the UI thread (Phase 2 rules: old entries
  stay visible until the result arrives, selection/cursor carried by name, scroll preserved) and
  re-targets the watch. Background tabs created from a session start with no entries and list on
  first activation.
- A restored session therefore costs one listing per pane at startup, not one per tab.
- Each visible pane's model runs the Phase 2 data flow independently. Watch workers: one per
  visible pane (or one shared worker with one handle per pane; ui's choice). Listing results and
  watch batches carry the `TabId` and generation; a result for a tab that was closed or
  deactivated is dropped.
- Keyboard commands go to the active pane's active tab. Pointer intents carry the pane they were
  recorded in; any click in an inactive pane activates it first.
- The inactive panes are drawn slightly dimmed (design review: `selection_inactive` grey, no
  cursor ring, active pane marked by a 1-2px accent edge, never by colour alone).

### Frame-end order and drag and drop

Extends the Phase 2 per-frame order. During drawing, the tab bars, splitters and pane bodies
record ui-internal intents; nothing mutates the `Layout` mid-draw. At frame end the ui applies, in
order: (1) pointer intents (pane activation, tab click, drop, splitter move), (2) the command
queue drain. Each `Layout` edit is all-or-nothing, so an intent that refers to a tab or pane
removed earlier in the same frame fails with NotFound and is ignored.

Tab drag state machine (ui-internal, UI thread):

```
Idle --press on tab--> Pressed(tab, origin)
Pressed --move > 4dp--> Dragging(tab)            Pressed --release--> Idle (+ click: activate)
Dragging: every frame, hit-test the pointer -> DropTarget:
    TabBar(pane, index)      over a tab bar (insertion marker drawn)
    Edge(pane, edge)         in a pane's outer band (about 25% of the pane's width/height,
                             capped; the band nearest the pointer wins; preview rectangle drawn)
    Centre(pane)             elsewhere inside a pane
    None                     outside every pane
Dragging --Escape / window focus lost--> Idle (no change)
Dragging --release--> intent Drop(tab, target) -> Idle
```

At frame end a `Drop` becomes one `Layout` call: TabBar -> `move_tab(tab, pane, index)`; Edge ->
`split_pane(pane, edge, tab)` (InvalidArgument when the tab is the pane's only tab dropped on its
own edge: no-op); Centre -> `move_tab(tab, pane, end)` (same pane: no-op); None -> no-op. Splitter
drags record `SplitterMove(split_id, splitter, position_fraction, min_fraction)` and apply
`move_splitter`; a splitter double-click applies `equalize`. Hit areas: splitter 1px line, 8dp hit
area (as the Phase 4 sidebar).

### Persistence

Files (XDG; the root is overridable with `AppConfig::config_dir`):

| File | Content |
|------|---------|
| `$XDG_CONFIG_HOME/navigator/session.json` (default `~/.config/navigator/session.json`) | Session: window geometry, layout, active pane, closed tabs |
| `$XDG_CONFIG_HOME/navigator/layouts/<name>.json` | Named layout: tree and tabs only |
| `session.json.bad` | A session file that was not JSON, moved aside by `load_session` |

Format: JSON documented in `layout.hpp` ("JSON format (version 1)"), `"version": 1`. Writers
always write the current version; readers are lenient (unknown fields ignored, invalid parts
dropped or defaulted, never a crash, deep trees cut at `kMaxTreeDepth`, files over 16 MiB
rejected). Versioning rule: a later version only adds fields; an incompatible change must use new
field names, so an old Navigator can still read a newer file. Non-UTF-8 paths are stored as
`{"hex": ...}`.

Timing:

- **Load** once in `App::create` / before the first frame, on the UI thread (a few KB; the one
  blocking read the UI thread does, like Phase 2's `open_with_default` exception). NotFound: first
  start, one tab on the start folder. Any other error: same, plus a log line; `.bad` handling is in
  `load_session`. `restore_window_geometry` applies size/maximized (and the X11 position; Wayland
  cannot place windows, so `has_position` is false there).
- **Save, debounced**: every change that affects the persisted session (layout edit, tab
  navigation, sort/hidden toggle, closed-tabs change, window move/resize/maximize) marks the
  session dirty; 2 s after the last change the ui copies the `Session` and hands the copy to a
  background writer (ui-internal `std::jthread`, latest-wins mailbox) that calls `save_session`.
  Cursor and scroll changes alone do not start the timer (they are saved with the next save or on
  quit).
- **Save on quit**: `app.quit` / window close writes the current session synchronously (after
  letting a running background write finish), then exits. A failed save is logged, never blocks
  quitting.
- Window geometry is the restored (non-maximized) size, tracked from SDL window events.
- Named layouts: `layout.save` / `layout.load` / `layout.delete` run their file call on a worker
  (or synchronously: they are small, user-initiated, infrequent; ui's choice) and show errors as
  notices. Loading a named layout replaces the current layout; the replaced tabs are pushed onto
  the closed-tabs list, so `tab.reopen_closed` brings them back.
- `save_session = false` (CLI `--no-session`, smoke tests) never writes; `restore_session = false`
  never reads. Tests use `config_dir` or `XDG_CONFIG_HOME` pointing at a temporary directory.

### Startup with a path

`navigator DIR` (non-empty `AppConfig::start_path`) restores the session and opens DIR as a new
active tab after the active tab of the restored active pane, so the saved tabs survive. Without a
session it is the only tab.

### Deferred

- `Ctrl+N` / `window.new` (duplicate tab into a new window): multi-window phase. The id is
  reserved and unbound; one window until then.
- Sidebar (Phase 4), focus-scoped bindings (Phase 4), palette listing of these commands (Phase 8
  palette UI; they are registered now).
- Per-tab history persistence, pinned tabs, tab groups: not planned for Phase 3.

Work split for Phase 3: **contracts** (this diff) -> **core-fs** (`layout.hpp` in `src/core`,
`tests/core`) and **commands** (ids, names, keymap rows, `tests/commands`) in parallel with
**ui** (which can start against the header and a stub until core lands) -> integration (main.cpp
flags, CMake linking `navigator_core` into ui if not already) -> **qa** (layout ser/de, "restore
three panes", drag-to-split) -> **design-reviewer** (drop affordances, dimming, tab strip).

## Phase 4: sidebar, GoTo, address bar, bookmarks, recents

### Ownership

| Piece | Where | Role |
|-------|-------|------|
| `VolumeIdentity`, `identity_of`, `volume_matches`, `used_fraction` | `volume.hpp` (header-only, inline) | contracts |
| `UserDirs`, `parse_user_dirs`, `read_user_dirs`, `user_home`; `user_dir` for Desktop..Videos | `platform.hpp`, `src/platform`, `tests/platform` | platform |
| Places list, `BookmarkStore`, `RecentsStore`, location resolution/resync, highlight helper, JSON and files | `places.hpp`, `src/core` (e.g. `places.cpp`, `places_json.cpp`), `tests/core` | core-fs |
| `expand_user_path`, `normalize_absolute`, `fix_path_case`, `resolve_input`, completion, Copy as Path formatting | `path_input.hpp`, `src/core` (e.g. `path_input.cpp`), `tests/core` | core-fs |
| `SidebarState` in `Session` (JSON `"sidebar"`) | `layout.hpp`, `src/core/layout_json.cpp` | core-fs |
| New ids, names, `to_string`, default chords | `src/commands`, `tests/commands` | commands |
| Sidebar widget and model, GoTo popup, address-bar edit mode, breadcrumb dropdown, workers (volumes, probes, completion, GoTo resolution), clipboard, persistence timing, i18n (`place.*`, `sidebar.*`, `cmd.*`, notices) | `src/ui`, `src/app` | ui |

The atomic-write and `.bad` logic of `layout_files.cpp` is reused (factored inside `src/core`)
for `bookmarks.json` and `recents.json`.

### Sidebar model

One sidebar per window, left of the pane tree, not a layout node (design review). Its
persisted state is `layout::SidebarState` in the `Session` (`visible`, `width` in dp, collapsed
sections); named layouts never contain it, and loading a layout leaves it unchanged. The
icon-only collapse on narrow windows is derived from the window width each frame and not
persisted.

Sections, top to bottom (design review; ui may adjust after the design pass): **Bookmarks**,
**Recents** (newest first, at most 8 rows shown, "Clear" via `recents.clear`), **Places**
(`places::build_places`: Home, the XDG user dirs, File System), **Storage** (volumes from the
volume monitor except the `Root` volume and the `Home` volume, whose usage bars are drawn on the
File System and Home place rows, docs/features.md "Drive usage bar under Places"). Volume rows
show `label` (else a derived name), a usage bar and `round(100 * used_fraction(space))` percent
when `has_space`, and badges for `removable` and `VolumeKind::Network`. Eject is a later phase.
Row labels: places use `tr(places::label_key(kind))`; bookmarks their `name` or
`places::default_label(path)`; recents `default_label`.

The sidebar model is ui-internal (like `BrowserModel`) and rebuilt at frame start from: the
latest volume snapshot, the `BookmarkStore`, the `RecentsStore`, the Places list and the last
probe results. Widgets read it; clicks become intents/commands applied at frame end.

Highlight: the ui passes the effective paths of all visible rows to
`places::deepest_containing(active_folder, rows)` and marks only the winner with the selection
marker; it moves with `F6` / pane activation. Rows shown in other panes get no highlight.

Focus: `F6` / `Shift+F6` cycle sidebar -> panes (reading order) -> sidebar while the sidebar is
visible; `sidebar.focus` jumps there; `Escape` returns to the last active pane. Keyboard
routing is described in `docs/keymap.md` "Focus scoping".

### Volumes, bookmarks and recents data flow

```
volume worker (ui-internal jthread)                         UI thread
  create_volume_monitor(); loop:                              frame start:
    wait(1000 ms) -> poll() on change                           take latest snapshot (mailbox)
    every 10 s while the sidebar is visible:                    resolve_location() for every
      list_volumes(query_space = true)    -- snapshot -->        bookmark / recent (pure)
                                                                 BookmarkStore/RecentsStore::
probe worker                                                       resync(); save if changed
  for each Online location not on a Network volume:            post probe request on change
    fs::stat(path, follow) -> Missing on NotFound /  -- results -->  apply statuses
    not a directory
```

- `statvfs` and `stat` on a hung network mount can block for minutes: they only ever run on
  these workers; a stuck worker delays the sidebar's numbers, never a frame. Snapshots carry a
  generation; stale probe results are dropped.
- New bookmark (`bookmarks.toggle`, drag a tab onto Bookmarks): `places::identity_for(path,
  snapshot)` on the UI thread (pure), then `BookmarkStore::add`. Offline rule and rebasing:
  `places.hpp` "Location status". The acceptance case "a bookmark on an unmounted volume isn't
  lost and shows offline" is `resolve_location` returning Offline while the store keeps the
  entry and the file keeps it across restarts.
- Recents: the ui calls `RecentsStore::record(path, identity_for(path, snapshot), now)` when a
  tab's listing of a NEW folder succeeds (navigation, GoTo, sidebar, breadcrumb, new tab);
  refresh, tab activation and session restore do not record. `RecentsOptions::enabled == false`
  makes it a no-op. `highlight` / `sort_on_top` are persisted stubs with no list behaviour yet.

Persistence timing (same rules and flags as the session):

- **Load** `bookmarks.json` and `recents.json` once at startup on the UI thread, with the
  session (small files). NotFound: empty store. Other errors: empty store + log line (`.bad`
  handling in `load_*`). `AppConfig::restore_session = false` skips loading them.
- **Bookmarks save** right after each edit (add, remove, rename, move, resync change) through
  the background writer (copy of the store, latest-wins). **Recents save** debounced 2 s after
  the last change, like the session. Both are written synchronously on quit.
  `AppConfig::save_session = false` never writes them.

### GoTo and the address bar

Both use `path_input.hpp`. The ui builds an `ExpandContext` with `home = user_dir(Home)`,
`cwd` = the active tab's folder, `env` = a `getenv` wrapper, `user_home` = a wrapper of
`platform::user_home`, `percent_variables = true`.

- **Typing** (debounced ~50-100 ms): `parse_completion_input` on the UI thread (pure; `~user`
  is not resolved here: the ui passes a context without `user_home` and waits for the worker);
  when `query.dir` differs from the cached one, a worker runs `complete_path` (cancelling the
  previous job via `std::stop_token`); while the dir is unchanged, `match_completions` filters
  the cached listing on the UI thread per keystroke.
- **GoTo list** (`nav.goto`): with empty text, recents (newest first), then bookmarks, then
  places. With text that looks like a path (starts with `/`, `~`, `$`, `%`, `.`, or contains
  `/`): completions first, then recents/bookmarks whose path contains the text (`fs::fold_name`
  substring). Other text: recents, bookmarks and places whose label or path contains it, then
  completions relative to the active folder. No fuzzy ranking before Phase 5.
- **Enter / Ctrl+Enter**: a highlighted row goes to its path; otherwise a worker runs
  `resolve_input` (expand, case-correct, stat). Folder: navigate (or new tab); file: navigate to
  its folder with the cursor on it (Ctrl+Enter: open with the default app); missing: notice
  with the path and any `unresolved` variables, text kept for editing. Acceptance: `~/Downloads`
  and `/` resolve to folders and navigate.
- **Address bar** (`nav.focus_address_bar`, `Ctrl+L`, click on the empty bar part): the
  breadcrumb bar of the active pane turns into a text field with the folder's path; same
  completion and Enter rules; `Escape` or focus loss restores the breadcrumbs.
- **Breadcrumbs**: segment click navigates to that ancestor (acceptance: "breadcrumb navigates
  up"); middle-click opens it in a new tab; the optional chevron dropdown lists the segment's
  subfolders from a worker listing (`fs::list_directory`, folders only, sorted with the tab's
  sort options).
- **Copy as Path** (`edit.copy_path`, `Ctrl+Shift+C`): `format_paths_for_copy` on the UI thread
  (pure), clipboard set through SDL in `src/ui`.
- **View lock** (`view.toggle_view_lock`): stub; the lock icon may be drawn disabled.

### Deferred

Trash place (`PlaceKind::Trash`, ops phase), eject/unmount buttons, GVfs/network places
(network phase), recents highlighting and sort-to-top behaviour (stubs persisted now), fuzzy
GoTo ranking (Phase 5), compact-path toggle, multi-key sequences, a sidebar for a second
window.

Work split for Phase 4: **contracts** (this diff) -> **core-fs** (`places.hpp`,
`path_input.hpp`, `SidebarState` JSON, `tests/core`), **platform** (user dirs, `user_home`,
`tests/platform`) and **commands** (ids, names, keymap rows, `tests/commands`) in parallel with
**ui** (against the headers) -> integration -> **qa** (GoTo `~/Downloads` and `/`, offline
bookmark across restart with a fake volume table, breadcrumb up) -> **design-reviewer**
(sidebar, GoTo popup).

## Phase 5: folder filter, flatten (Ctrl+E), fuzzy name search, name index

### Ownership

| Piece | Where | Role |
|-------|-------|------|
| `fs::fold_name_into` (offset-preserving fold) | `fs.hpp`, `src/core/names.cpp` | core-fs |
| Recursive enumerator `fs::Walker` (BFS, batches, dev/ino cycle set, hidden policy, one volume, permission skips, cancellation) | `walk.hpp`, `src/core` (e.g. `walk.cpp`), `tests/core` | core-fs |
| `TabState::type_filter` JSON (`"hidden_types"`), normalization on load | `layout.hpp`, `src/core/layout_json.cpp` | core-fs |
| Query compilation, `Matcher` (substring + fuzzy DP, highlights), `ranks_before`, `TypeFilter` helpers, `summarize_types`, `filter_entries` | `search.hpp`, `src/search` (new module, library `navigator_search`), `tests/search` | search |
| `IIndex`, `create_name_index` (service thread, scan/resume, verify, inotify, file format, queries) | `index.hpp`, `src/search`, `tests/search` | search |
| Benchmark tool and `docs/perf/phase5-search.md` | `tools/search_bench` (CMake registration by integration) | search |
| New ids, names, `to_string`, default chords | `src/commands`, `tests/commands` | commands |
| Filter field, chip, empty state, flatten view (relative-path column, progress bar), types popup, match highlight, Escape cascade, type-to-filter, workers, index service wiring, status bar, i18n (`cmd.*`, `search.*`, `index.*`) | `src/ui`, `src/app` | ui |
| `--no-index`, `--cache-dir DIR`; `add_subdirectory(src/search)` and `tests/search`; link `navigator_search` into ui | root `CMakeLists.txt`, `src/app/main.cpp` | integration |

Split rationale: enumeration is filesystem policy (symlinks, mounts, permissions, hidden) and
belongs with `fs::list_directory` in core; matching, ranking, the type filter and the index are
search policy on top of it. `src/search` never calls `src/platform`: the index gets inotify
through an injected `WatcherFactory` and the cache root as a value, so `tests/search` runs on
fake watchers and temporary directories only.

### View model per tab (ui-internal, extends `BrowserModel`)

- **Filter**: text, compiled `search::Query`, mode (Substring by default), `Matcher`, and the
  shown rows as indices into the listing (`FilterHit`). Runtime only; cleared by navigation.
- **Type filter**: `TabState::type_filter` (persisted with the tab, copied by `tab.new` /
  `tab.duplicate`, kept across navigation like `show_hidden`).
- **Flatten**: root (the tab's folder, or the volume's mount point for `search.flatten_volume`),
  source (Walk or Index), the directory table, the flat entries, `WalkStats` or `IndexStatus`
  progress, mode (Fuzzy by default), and a generation. Runtime only.
- The shown rows are always `listing -> hidden policy (listing/walk) -> type filter -> query`.
  Status bar and chip: "12 of 340" where 340 counts the rows the hidden policy shows. No
  matches: the Phase 2 empty-state style with a "Clear filter" keycap hint.
- Order: the folder filter keeps the tab's sort order (rows never jump while typing). The
  flattened view sorts with the tab's `SortOptions` (ties: relative parent path) when the query
  is empty, and by `ranks_before` (relevance) when it is not; a column-header click switches to
  that sort until the query changes.

### Data flow

```
UI thread (per keystroke in the filter field)
  compile_query(text, mode)
  rows <= kUiThreadFilterLimit (20k): filter_entries() inline, same frame
  otherwise: post (generation, query, snapshot of listing) to the filter worker;
             old rows stay until the result for the latest generation arrives
  narrows(prev, next): refilter only the previous hits (inline or worker)
  highlights: Matcher::match() for VISIBLE rows only, cached per (row, generation)

flatten worker (ui-internal jthread per tab, started by search.flatten)
  source Walk:   Walker::open(root, show_hidden = tab, metadata = true, one volume)
                 loop next_batch(stop): filter the batch with a worker-local Matcher
                 -> mailbox {generation, new_dirs, entries, hits, stats}
  source Index:  IIndex::query({query, under = root, types, show_hidden, limit}) on every
                 query change (debounced ~50 ms, previous query cancelled); rows carry path +
                 kind; Size/Modified are filled by fs::stat on a worker for visible rows only
frame start (UI thread)
  apply mailbox batches whose generation is current: append dirs/entries, merge hits
  (relevance order: insert into the ranked list; the ui may cap the ranked list it keeps
  to the best 100k and say so), update progress; stale generations are dropped
```

- Source choice (as implemented): `search.flatten` (folder subtree) always walks the disk, so
  results are complete and carry full metadata. Only `search.flatten_volume` uses the index: when
  an `IIndex` whose root is that volume's mount point is Ready or Verifying it queries the index
  (hits capped, one stat per shown hit), otherwise it walks the volume root with progress. The
  index of `/` is never used for a volume mounted below it. The ui may start an index only for
  volumes `should_index_by_default` allows.
- Query edits during a Walk flatten re-run the match over the flat entries already collected
  (inline below 20k, else the filter worker) while the walk keeps appending; the walk is never
  restarted for a query change. `ToggleHidden` and `nav.refresh` restart the walk.
- Progress: a thin indeterminate bar under the pane title bar while a walk or index scan is
  running, the running count in the status bar ("48 210 found, 3 folders skipped"); never modal.
- Cancellation: `search.cancel` / the Escape cascade / leaving flatten / navigation /
  closing the tab request stop on the worker's `std::stop_source` and bump the generation. The
  walker stops within one directory chunk (walk.hpp "Cancellation latency"). The UI thread
  never joins a worker that may be blocked in a syscall: cancelled jobs move to a reaper list
  joined off the UI thread (or at quit with a bounded wait, then detached), as for listings.
- Watch events: the root folder keeps its Phase 2 watcher; events apply to the folder listing
  (and are matched against the active filter on insert). A flattened view is a snapshot; `F5`
  refreshes it. Flatten never records Recents (design review).

### Name index service

```
ui index service (ui-internal; UI thread owns the map, IIndex owns its threads)
  on each volume snapshot (Phase 4 volume worker):
    for v in volumes with should_index_by_default(v) (and AppConfig::enable_index):
      id = index_stable_id(identity_of(v)); start create_name_index({root = v.mount_point,
           identity, file = index_file(cache_root, identity), watcher_factory, budget, ...})
    volumes that disappeared: stop() on a reaper thread, drop
  on_status_change (service thread) -> SDL user event -> frame start reads status()
IIndex service thread (one per volume): Loading -> Verifying | Scanning -> Ready (+ inotify,
  periodic verify, debounced save) -> Stopped        (index.hpp "Lifecycle")
```

- Budgets: watch budget 8192 directories per index by default (shared user limit; the pane
  watchers need only a handful); memory about 60 MB per million entries, at most 2M entries
  per index by default; one service thread per index at background CPU/I/O priority; queries
  run on the caller's worker, never on the service thread.
- Cache: `$XDG_CACHE_HOME/navigator/index/<stable-id>.idx` (`AppConfig::cache_dir` overrides
  the `navigator` root). Versioned binary, FNV-1a trailer, atomic write, rebuild on any
  corruption, resumable partial scans (index.hpp "File format").
- Defaults: local Root, Home and writable Internal volumes; never network, removable, virtual or
  image filesystems (opt-in is a Phase 9 setting). `--no-index` disables everything.

### Benchmark plan (`docs/perf/phase5-search.md`, owner search; qa re-runs)

Generated tree in a temporary directory: 300k entries (about 30k directories, depth up to 12,
names mixing ASCII, Turkish and NFD forms, 2% hidden directories with children, a few
unreadable directories), Release build, warm and cold page cache (cold: `echo 3 >
/proc/sys/vm/drop_caches` where permitted, else a note). Measure and record:

1. Folder filter: `filter_entries` over 1k / 20k / 100k names, 1-, 3- and 8-character queries,
   Substring and Fuzzy (target: 20k under 2 ms, which justifies `kUiThreadFilterLimit`).
2. Walk: entries/s with `metadata` true and false; time to the first batch; cancel latency
   (request stop at random points, max and p99 until `next_batch` returns Cancelled).
3. Index: full scan time; save time and file size; **restart: load + verify versus a cold
   walk** of the same tree (acceptance: the index is faster; record both numbers);
   query latency for 1M synthetic entries (target under 100 ms).
4. UI: frame times while a 300k flatten streams in (no frame over 33 ms attributable to
   applying batches).

### Deferred

Content search (Phase 10), wildcard/regex syntax (never claimed), matching the relative path in
flattened mode, user opt-in for indexing other volumes and a persisted default match mode
(Phase 9 settings), recursive watching of a flattened view, GVfs/network indexing, fanotify.

Work split for Phase 5: **contracts** (this diff) -> **core-fs** (`fold_name_into`, `Walker`,
`hidden_types` JSON, `tests/core`), **search** (`src/search`, `tests/search`, benchmark) and
**commands** (ids, names, keymap rows, `tests/commands`) in parallel with **ui** (against the
headers; stubs until search lands) -> integration (`src/search` and `tests/search` subdirs,
link `navigator_search`, `--no-index`, `--cache-dir`) -> **qa** (ranking gold tests, Unicode
İ/ı and NFD, cancel, permission-denied skips, hidden-children exclusion in flattened mode,
index corruption/rebuild, restart timing) -> **design-reviewer** (filter field, chip, flatten
view, types popup, highlight contrast).

## Phase 6: Inspector, thumbnails, previews

Goal (roadmap "Faz 6"): `Space` opens the Inspector for images (EXIF orientation, fit / 1:1,
grid, minimap, metadata), text (virtualized long lines) and folders (inner listing without
navigating); list thumbnails through the Freedesktop thumbnail cache; a configurable cache
size. The owner also asked for video: poster frame, metadata and a "Play" hand-off to the
default player. Video is a **Navigator extra** (`docs/features.md`: File Pilot does not claim
video playback); there is no in-app playback in Phase 6. PDF has an interface and a "coming
soon" state; SVG renders only when built with librsvg.

### Ownership

| Piece | Where | Role |
|-------|-------|------|
| Value types, `make_image`, `sniff_image_format`, `decode_text`, `looks_binary`, `fit_size`, `select_jpeg_scale_denom`, `apply_exif_orientation`, `downscale`, `make_key`/`hash_value`, `approx_bytes`, `ProviderRegistry` | `preview.hpp`, `src/preview` (new module, library `navigator_preview`), `tests/preview` | preview |
| Decoders (JPEG with DCT scaling, PNG streaming, WebP, GIF first frame, BMP, HEIF/AVIF, SVG), EXIF parser, FFmpeg poster + `VideoInfo`, text and folder providers, placeholders, `register_builtin_providers`, `build_features` | `preview_service.hpp`, `src/preview` | preview |
| Freedesktop cache: `file_uri`, `md5_hex`, paths, `read_thumbnail`/`lookup_thumbnail`, `write_thumbnail`, failure markers, `decide` | `thumbnail.hpp`, `src/preview` | preview |
| `.thumbnailer` parsing, `split_exec`/`expand_exec`, `load_thumbnailers`, `choose_thumbnailer`, `run_thumbnailer` (temp dir, output checks) | `thumbnail.hpp`, `src/preview` | preview |
| `IPreviewService` (workers, priorities, coalescing, cancellation, LRU memory cache) | `preview_service.hpp`, `src/preview` | preview |
| `thumbnail_cache_dir`, `data_dirs`, `find_program`, `IProcessRunner` / `create_default_process_runner` (no shell, process group, timeout, rlimits, optional bubblewrap) | `platform.hpp`, `src/platform`, `tests/platform` | platform |
| `InspectorState` in `Session` (JSON `"inspector"`) | `layout.hpp`, `src/core/layout_json.cpp`, `tests/core` | core-fs |
| Ids 1320-1327, names, `to_string`, chords `Space`, `Alt+KeypadEnter` | `src/commands`, `tests/commands` | commands |
| Inspector pane (placement, resize, lock, header buttons, image view with zoom/pan/grid/minimap/checkerboard, metadata list, text view, folder view, video poster + play disc, states), list thumbnails (row icon replacement, fade), service wiring, GPU upload and texture budget, `Space` in text fields, i18n (`cmd.*`, `inspector.*`, `meta.*`), `AppConfig` fields | `src/ui`, `src/app` | ui |
| `--thumbnail-dir DIR`, `--no-thumbnails`, `--no-external-thumbnailers`, `--preview-cache-mb N`; `add_subdirectory(src/preview)` and `tests/preview`; link `navigator_preview` into ui | root `CMakeLists.txt`, `src/app/main.cpp` | integration |
| Optional decoder dependencies (libpng, libjpeg-turbo, libwebp, libexif, libheif, FFmpeg, librsvg), feature macros, CI matrix with and without the optional ones | `packaging/`, CI, CMake glue | packaging |

Split rationale: everything that reads untrusted file bytes (decoders, EXIF, thumbnail PNGs,
thumbnailer output) lives in one module with one hardening rule set and one test suite.
Platform owns only what touches the OS process model and XDG environment; preview receives
them as values (`Path` roots, data dirs) and an injected `IProcessRunner`, so `tests/preview`
never spawns a process or touches `~/.cache`. The ui owns every pixel on screen: it uploads
`ImageBuffer`s to textures on the UI thread and never decodes.

`src/preview` links `navigator_core` (folder listing, sort, `fs::is_directory_like`) and its
decoder libraries, nothing else. It does not link `navigator_platform`: the ui fills
`PreviewRequest::mime` with `mime::guess_for_entry` (already computed for the Type column), and
the cache root, data dirs and process runner come in as values and an injected object.

### Data flow

```
UI thread, every frame                                preview workers (IPreviewService)
  frame start: service.poll(completions)                pick highest priority job:
    for each completion: keep the shared result,          Inspector > Visible > Prefetch,
    upload pixels to a texture (budgeted, LRU),           newest epoch first
    start the fade (none with animations off)           Thumbnail purpose:
  build UI:                                               decide() -> Skip / ReadOnly / Generate
    visible rows needing a thumbnail:                     stat target -> SourceInfo
      key = make_key(request)                             lookup_thumbnail (wanted class, larger)
      texture? draw : lookup(key)? upload : submit        has_failure -> placeholder
        (Visible, epoch) and draw the type icon           produce at class size (registry:
    Inspector item (cursor or locked):                      native decoder | thumbnailer)
      submit(Inspector) on change; prefetch               write_thumbnail / write_failure
      neighbours (Prefetch, cursor +-2)                   downscale to the requested target
  on scroll / navigation / tab switch:                  Inspector purpose: produce directly
    ++epoch, resubmit visible, cancel_older()           memory LRU insert, Completion,
  watch events / F5: invalidate(path, Memory)             on_completion -> SDL user event
```

- Decoding never runs on the UI thread. The UI thread does: key computation, `lookup()` (a hash
  probe under a mutex), texture uploads of finished buffers (at most a per-frame byte budget,
  e.g. 8 MiB, the rest next frame so a burst of thumbnails cannot stall a frame), and drawing.
- The type icon stays until the thumbnail lands; the row height never changes; thumbnails keep
  the 1px `icon_page_border` frame (design review). The fade-in is skipped with
  `AppConfig::animations == false`.
- Inspector: a skeleton with the right aspect ratio appears at once when the size is known
  (cached `ImageInfo`, or the list thumbnail), then the image crossfades in. Fit is the default;
  1:1 is one image pixel per device pixel; images smaller than the view are not upscaled in fit
  mode. The Inspector requests `target` = view size in device pixels for fit and `{0, 0}` for
  1:1 (capped at `kMaxOutputPixels`, then labelled scaled). Window resizes request a new decode
  only when the view grows past the decoded size (bucketed by the ui; shrinking scales on the
  GPU).
- File changed while shown: a watch event with a new mtime/size makes a new key; the old
  texture stays until the new result arrives (refresh in place, no flash).
- Lock: the ui keeps the locked `PreviewRequest` and ignores cursor changes; deleting or
  renaming the item away unlocks with a notice.
- Multi-selection: the Inspector shows count and total size (ui only, no preview request).

### Memory and performance budgets

| Budget | Default | Where |
|--------|---------|-------|
| Result cache (CPU memory) | 256 MiB (`AppConfig::preview_cache_bytes`, min 16 MiB) | `IPreviewService` LRU by `approx_bytes` |
| Texture cache (GPU memory) | ui's choice, about the same; LRU of textures not drawn this frame | `src/ui` |
| One decode | 256 MiB peak (`kDefaultDecodeMemoryBytes`) | providers |
| Source limits | sides <= 65535, <= 1 GP; full-decode formats <= 64 MP | `preview.hpp` |
| Output buffer | <= 32 MP (128 MiB) | `preview.hpp` |
| Text | first 4 MiB, rows <= 1 KiB, <= 500k rows | `TextOptions` |
| Folder preview | 2000 entries | `FolderOptions` |
| Workers | clamp(cores / 2, 2, 6); at most 2 thumbnailer processes | service, `ThumbnailerLimits` |
| Thumbnailer | 15 s wall, 30 s CPU, 2 GiB address space, 32 MiB output | `ThumbnailerLimits` |

A 100 MP JPEG decodes at 1/8 scale (DCT) straight into a small buffer; a 100 MP PNG streams
rows and averages them into the target, never holding the full image. Formats that cannot
stream refuse above 64 MP (after trying an embedded thumbnail) instead of allocating 400 MB.
Acceptance "100 MP never crashes" and "no UI freeze on a large text file" are qa gates.

### Thumbnail cache rules

- Location: `AppConfig::thumbnail_dir`, else `<cache_dir>/thumbnails` when `cache_dir` is set,
  else `platform::thumbnail_cache_dir()` (`$XDG_CACHE_HOME/thumbnails`). Headless rule (same as
  session and index): `max_frames > 0` without `thumbnail_dir`/`cache_dir` disables the disk
  cache and external thumbnailers; `--no-thumbnails` disables both everywhere.
- Spec compliance: URI as GLib writes it, MD5 name, classes normal/large/x-large/xx-large,
  `Thumb::URI` + `Thumb::MTime` validation (and `Thumb::Size` when present), our failure
  markers in `fail/navigator-1/`, files 0600 and new directories 0700, atomic rename. Other
  programs' thumbnails are reused; ours are readable by them.
- Policy (`thumbnail::decide`): never for files inside the cache, folders, devices or dangling
  links; on network and removable volumes only existing thumbnails are used by default
  (`thumbnail::Policy`); the Inspector always decodes the inspected file.
- Size class from the device-pixel icon box (`size_for`); a valid larger class is used and
  downscaled when the wanted one is missing.
- Shared removable-media caches (`.sh_thumbnails`) and cache cleanup of old entries are
  deferred.

### Security (untrusted files)

- Every decoder bounds dimensions before allocating, checks the stop token, and maps library
  errors to `Result` (libjpeg `longjmp` and libpng `setjmp` contained in their TU). Fixtures in
  `tests/preview`: truncated, bit-flipped and huge-declared-size files of every format must
  return an error or a partial image, never crash; run under ASan/UBSan in CI.
- EXIF / tEXt parsing bounds-checks every offset, rejects IFD loops, and caps string lengths.
- External thumbnailers: argv only (`expand_exec`, field values never re-split, no shell),
  absolute input paths (never parsed as options), private 0700 temp dir per run, timeout and
  rlimits, process-group kill on timeout or cancel, output size and image checks, bubblewrap
  sandbox when `bwrap` exists (no network, read-only system, only the input and the temp dir
  visible), our own tEXt keys written after decoding their output.
- Nothing is fetched from the network (GPS is shown as numbers only).

### Deferred

PDF rendering (poppler/pdfium, after Phase 6), in-app video/audio playback, audio metadata and
cover art, tiled 1:1 decode for images above 32 MP, colour management (ICC), animated image
playback in the Inspector, `.sh_thumbnails`, thumbnail cache cleanup, grid view thumbnails if
the grid view does not land in Phase 6 (the list path is the gate), user settings for the
policy, timing and fit/fill (Phase 9), MIME subclass matching for thumbnailers.

Work split for Phase 6: **contracts** (this diff) -> **preview** (`src/preview`,
`tests/preview`), **platform** (`IProcessRunner`, data dirs, `find_program`, cache dir,
`tests/platform`), **core-fs** (`"inspector"` JSON, `tests/core`) and **commands** (ids,
names, chords, `tests/commands`) in parallel with **ui** (against the headers and a fake
`IPreviewService`) and **packaging** (decoder dependencies) -> integration (`src/preview` /
`tests/preview` subdirs, link, CLI flags) -> **qa** (cache key and EXIF
orientation tests, Freedesktop interop with a GLib-made thumbnail, 100 MP and corrupt
fixtures, large-text frame time, headless never writes `~/.cache/thumbnails`) ->
**design-reviewer** (Inspector, thumbnails, states).

## Phase 7: file operations, trash, undo, drag and drop

Goal (roadmap "Faz 7"): copy / move / rename / new folder / delete-to-trash with a conflict
sheet and non-modal progress, XDG Trash with restore, undo/redo, drag and drop between panes
and from other programs, clipboard interoperability. Acceptance: a move between two panes comes
back with undo; a trashed item is restored from the Trash view; a drag from Navigator to at
least one external target (GNOME Files or the desktop) is attempted on Wayland and the result
documented. Undo/redo and the Trash view are **Navigator extras** (`docs/features.md`); the
File Pilot behaviours are copy/cut/paste/delete/rename, `Ctrl+Shift+N`, copy/move to the
previous pane, drops from other programs, wheel scroll and edge auto-scroll while dragging.

This is the first phase that mutates user files. The safety rules below are normative; a diff
that weakens one needs coordinator approval and a changelog entry.

### Ownership

| Piece | Where | Role |
|-------|-------|------|
| Engine (`run_operation`), `IOperationQueue` / `create_operation_queue`, `UndoStack`, `make_label`, pure helpers (`split_extension`, `stem_length`, `keep_both_name`, `validate_new_name`, `is_same_or_inside`, `protection_of`) | `ops.hpp`, `src/ops` (new library `navigator_ops`), `tests/ops` (new) | ops |
| FreeDesktop Trash: `ITrash` / `create_trash`, `.trashinfo` format, `escape_path`, dates, topdir rules; `platform::move_to_trash` wrapper | `trash.hpp`, `platform.hpp`, `src/platform`, `tests/platform` | platform |
| `file_uri`, `path_from_file_uri`, uri-list, GNOME/Nautilus/KDE clipboard formats, `make_clipboard_offer`, `read_clipboard` (pure); `choose_drop_action` (header-only) | `transfer.hpp`, `src/platform`, `tests/platform` | platform |
| Ids 1281-1285, 1340-1349, 1360-1362, names, `PasteArg`, 12 chords | `src/commands`, `tests/commands` | commands |
| Progress bar and operations panel, conflict sheet, confirmation sheets, notices with Undo, inline rename / new-folder editors, Trash view, SDL clipboard offer/read, DnD state machine (internal and incoming), outgoing-drag attempt, cut dimming, Inspector following moves, i18n (`cmd.*`, `ops.*`, `undo.label.*`, `trash.*`, EN + TR) | `src/ui`, `src/app` | ui |
| `navigator_add_subdir(src/ops)` and `tests/ops`; link `navigator_ops` (and `navigator_platform` for `create_trash`) into the ui; quit-while-running prompt wiring | root `CMakeLists.txt`, `src/app/main.cpp` | integration |
| Atomicity / partial-failure / cancellation / undo tests on fixtures, Nautilus interop checks, manual DnD note | `tests/**`, `docs/qa-phase7.md`, `docs/platform-limitations.md` | qa |

`src/ops` links `navigator_core` (`fs::stat`, `fs::to_display_name`) and nothing else; the
trash is injected as `std::shared_ptr<trash::ITrash>` (EngineOptions), so `tests/ops` can use a
fake or the real `create_trash` with `TrashOptions::home_trash` inside the fixture (tests/ops may
link `navigator_platform` for that). `src/ops` never includes `src/platform` headers. Recursive
copy and deletion live only in `src/ops`; `src/platform`'s trash only renames within one
filesystem and writes / removes `.trashinfo` files.

### Safety rules (normative)

The full text is in `ops.hpp` (S1..S16) and `trash.hpp` (T1..T7). In short:

1. **No silent removal.** Only `OpKind::DeletePermanently` with `OpRequest::confirmed`
   removes user data; the engine otherwise unlinks only its own temp files, a verified
   cross-filesystem move source, an empty folder it created (undo), and the partial copy of the
   item being moved when a cross-filesystem move is cancelled.
2. **Delete = trash.** `Delete` submits `Trash`. A volume without a usable trash yields
   `Unsupported` per item and `OpResult::untrashable`; there is never a permanent fallback. The
   ui then offers the permanent-delete sheet for exactly those items.
3. **Permanent delete is separate and confirmed** (`file.delete_permanently`, `trash.empty`):
   the sheet names the count and says it cannot be undone; focus starts on Cancel; `submit()`
   rejects the request without `confirmed`. Undo, redo, paste, drops and conflict decisions
   never produce it.
4. **Never follow symlinks** when deleting, trashing, moving or walking trees (dirfd-relative
   calls, `O_NOFOLLOW`, `AT_SYMLINK_NOFOLLOW`).
5. **One filesystem per tree**: recursive walks stop at mount points inside a tree and report
   them.
6. **Copy = temp file + rename**: `.<name>.navigator-<random>.part` in the destination folder,
   metadata applied, then `renameat2(RENAME_NOREPLACE)`; fallback `link` + `unlink`, last
   resort check-then-rename on filesystems without either (vfat, exfat: a racing program that
   creates the same name in the microseconds between check and rename can lose that file; this
   is documented and accepted because those filesystems offer no atomic primitive). Leftover
   `.part` files after a crash keep that recognisable name; Phase 7 does not sweep them.
7. **Move**: same filesystem = `renameat2(RENAME_NOREPLACE)`; across filesystems = copy, fsync,
   verify each source unchanged during its copy, then remove only the scanned source entries
   bottom up. Changed or new source entries stay and are reported.
8. **Overwrite only after a decision**; Replace trashes the existing item first (undo restores
   it); when no trash exists, a file is replaced atomically and the conflict sheet says it is
   permanent before the user decides; a folder is never replaced permanently.
9. **Cancellation** leaves no temp file and no truncated file, keeps completed items, and
   reports them (they are undoable).
10. **Metadata**: mode (setuid/setgid dropped unless owner and group are kept) and times;
    `user.*` xattrs and POSIX ACLs best effort, `security.*` / `trusted.*` never; no chown
    unless root. FIFOs recreated, sockets and devices not copied, hard links within a tree
    kept, sparse holes kept.
11. **Undo never deletes and never overwrites**: undone copies and links go to the trash, an
    undone new folder is rmdir'ed only while empty, every inverse step checks identity
    (`ItemGuard`: FileId, size, mtime, kind) and skips changed items, and an occupied
    destination becomes a conflict. Permanent deletes are not recorded (not undoable).
12. **Protected sources**: `/`, the home directory and its ancestors, mount points and trash
    directories are refused as sources of Move, Rename, Trash and DeletePermanently
    (`protection_of`); copying them is allowed. Copy/move of a folder into itself is refused.
13. **Space** is checked before a copy writes anything (`NoSpace`).
14. **Tests** create, move and delete files only in temporary fixture directories, never
    touch `$HOME` or the real trash, and inject faults through `EngineOptions::fault_injector`
    instead of filling disks.

### Operation queue and data flow

```
UI thread (frame end, command handlers)              ops workers (IOperationQueue)
  edit.paste / F7 / drop / file.trash / ...            submit: lexical checks (S3, S13)
    -> OpRequest -> queue.submit()  ---------------->  Queued until no earlier op overlaps
  frame start: queue.poll(events)                       its paths (is_same_or_inside either
    Started / Progress -> progress bar + panel          way); then up to max_concurrent (2)
    ConflictRaised -> conflict sheet (first pending)    run: Scanning (totals, S14) ->
       user decision -> queue.resolve(id, decision)      Working -> Finishing
    ItemFinished -> Inspector follows moved/renamed     conflicts under Ask: raise, wait on
       items; cut dimming cleared                         a condition variable (slot kept)
    Finished -> notice ("Moved 3 items", "Undo"),       pause / cancel checked every
       UndoStack::record_done(result.record, origin),     copy_chunk_bytes and per entry
       refresh touched_folders not covered by watches   on_event() -> SDL user event wakes
  watch events (Phase 1) reconcile the listings          the frame loop
```

- One `IOperationQueue` and one `UndoStack` per App. The queue's workers never touch ui
  state; everything the ui needs comes through `poll()` / `snapshot()` /
  `pending_conflicts()`.
- Listings are not edited by the ops code. Watched folders update through inotify events
  (Phase 1); `OpResult::touched_folders` lists folders to re-list where no watch covers them
  (network filesystems, folders shown in a flattened view). After a paste or drop the ui
  selects the created items (`ItemOutcome::destination`) once they appear in the listing.
- The **Inspector follows** a moved or renamed item from `ItemFinished` (source ->
  destination), keeps its lock across a rename, and shows "The file no longer exists" when the
  item was trashed (docs/review/phase-6.md), never an error flash.
- **Quit** while operations run: the ui asks (continue in background is not possible:
  Navigator has no daemon) and then calls `shutdown()`, which cancels safely (S9).
- **Undo** (`edit.undo`): `take_undo()`, check `UndoRecord::volumes` are mounted (else a notice
  names the missing volume and the record is put back with `restore()`), submit
  `OpRequest{Undo, origin Undo, record}`. The undo's own result record arrives through
  `record_done(..., Undo)` and becomes the redo entry. Undo depth 100, runtime only (not
  persisted: inode identities and trash names can change while Navigator is closed).
- Labels: `make_label()` gives the i18n key and arguments; the palette shows "Undo: move 3
  items to Photos" / "Geri al: 3 öğeyi Photos klasörüne taşı" for `edit.undo`.

### Trash

- `trash::ITrash` follows the FreeDesktop Trash spec 1.0 exactly (`trash.hpp` T1..T7): home
  trash `$XDG_DATA_HOME/Trash`, per-volume `$topdir/.Trash/$uid` (only with the sticky bit and
  not a symlink) else `$topdir/.Trash-$uid`, `.trashinfo` with GIO's escaping and local
  `DeletionDate`, names reserved with `O_EXCL`, no `directorysizes`.
- **Cross-filesystem trashing is refused** (the spec permits copying to the home trash; we do
  not, because a copy + delete of a large tree is neither atomic nor what "Move to Trash"
  promises). Removable and network volumes get a per-volume trash when writable; read-only
  volumes and volumes where neither trash directory is usable report `Unsupported`, and the ui
  asks for permanent deletion explicitly.
- **Trash view**: `trash.open` (sidebar row `PlaceKind::Trash`) switches the active tab to a
  runtime view mode listing `ITrash::list()` on a worker (columns: name, original folder,
  deletion date, size), watching every `TrashDir::files_dir()`. It is not persisted (the session
  keeps the folder the tab showed before, like the flattened view). Restore =
  `OpKind::Restore` (conflicts Ask; missing parent folders are created and recorded);
  `Delete` / "Delete permanently" / `trash.empty` = confirmed `DeletePermanently` with
  `OpRequest::trashed`. Opening or previewing trashed files is read-only (Inspector works).
- Cross-device restore (an item in the home trash whose original folder is on another
  filesystem, e.g. trashed by another program that copies): `ITrash::restore` returns
  `CrossDevice`; the engine copies (S6), verifies, then erases the trashed copy and calls
  `forget()`.

### Clipboard and drag and drop

- **Clipboard** (`transfer.hpp`): `edit.copy` / `edit.cut` offer, through
  `SDL_SetClipboardData`, `x-special/gnome-copied-files`, `text/uri-list`,
  `application/x-kde-cutselection` (cut only), and text (paths). `edit.paste` reads
  `SDL_GetClipboardData` in the order gnome-copied-files, uri-list (+ KDE marker), Nautilus
  text form. Plain-text paths are never pasted as files. The ui keeps its own last offer so a
  cut can be cleared after the paste only while Navigator still owns the clipboard.
  `edit.copy_path` (Phase 4) stays separate and offers text only.
- **Incoming drops** (SDL3): `SDL_EVENT_DROP_BEGIN` starts a drop session, `DROP_POSITION`
  hit-tests targets each event (folder rows, panes, tabs, breadcrumbs, sidebar rows; the
  tab-split drop styling of Phase 3 is reused), `DROP_FILE` adds one path, `DROP_TEXT` is fed
  through `decode_uri_list()` (a uri-list delivered as text), `DROP_COMPLETE` decides with
  `choose_drop_action()` (external: Copy unless Shift) and submits. SDL3 does not expose the
  source's proposed action or the XDND / `wl_data_offer` action negotiation, so an external
  source always sees "copy". Archives dragged out of File Roller use XdndDirectSave, which
  SDL3 does not implement: such drops arrive as nothing or as a temp path, and Phase 7 treats
  them like any file drop (Phase 10 revisits archives).
- **Internal drags** (pane to pane, to tabs, breadcrumbs, sidebar) are a ui state machine:
  `Idle -> Pressed (threshold 4 dp) -> Dragging(items, origin, action from modifiers each
  frame) -> Dropped / Cancelled (Escape, or release outside any target)`. While dragging:
  wheel scrolls the list under the pointer, the list auto-scrolls within 24 dp of its top or
  bottom edge (speed grows towards the edge), a folder row hovered for about 700 ms opens
  (spring-loaded; disabled with animations off), the badge shows count + first thumbnail +
  action. Dropping runs at frame end (`OpRequest` submit), never mid-draw. Phase 3's tab drag
  shares the same machine with a different payload.
- **Drops onto executables** (`features.md`, phases 7 and 10): deferred to Phase 10 together
  with "Open console here"; in Phase 7 a drop onto a file row is `DropAction::None`.
- **Outgoing drags to other programs: limitation.** SDL 3.4 has no drag-source API (it only
  receives drops). What Phase 7 does:
  - Always available: `Ctrl+C` in Navigator, `Ctrl+V` in GNOME Files / Nemo / Dolphin / the
    desktop, through the formats above (tested in qa).
  - X11: a small XDND source in `src/ui/platform/` using the `Display*` / `Window` from
    `SDL_PROP_WINDOW_X11_*` (XdndSelection owner, `text/uri-list` and
    `x-special/gnome-copied-files` targets, XdndActionCopy / XdndActionMove). This works
    without compositor support and is the planned implementation.
  - Wayland: a drag source needs `wl_data_device.start_drag` with the serial of the button
    press that started the drag on Navigator's own seat. The attempt: bind a second
    `wl_data_device_manager` / `wl_seat` / `wl_pointer` on SDL's `wl_display`
    (`SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER`) with a private event queue, take the press
    serial from that pointer, and start the drag with SDL's `wl_surface` as origin. Whether
    Mutter and Muffin accept this (one client, two pointer objects) is exactly what the roadmap
    asks to test: qa / ui record the outcome for GNOME Files and the desktop in
    `docs/platform-limitations.md`. If it fails, Wayland users get the clipboard path above
    and the drag badge says "Drag to other apps is not available on this session" when the
    pointer leaves the window. No portal offers drag-and-drop for unsandboxed apps.

### Rename, new folder, conflict sheet, progress

- **Rename** (`F2`): inline editor on the row, stem selected (`stem_length`), Turkish input
  through SDL text input; `validate_new_name` errors are shown inline; Enter submits
  `OpKind::Rename`. A case-only rename (`a.txt` -> `A.txt`) on a case-insensitive filesystem
  is not a conflict (the engine sees the same FileId and renames through a temp name).
- **New folder** (`Ctrl+Shift+N`): as implemented, the folder is created at once with the
  translated default name ("New folder" / "Yeni klasör") made unique with `keep_both_name`, and the
  inline editor opens on it; Enter renames it, Escape keeps the default name (undoable like any
  `MakeDirectory`).
- **Conflict sheet**: Inspector-card style, not a system dialog; both sides with the Phase 6
  thumbnail (preview service, Thumbnail purpose), size, date; actions Replace (warns when
  `replace_is_permanent`, hidden when not `replace_allowed`), Skip, Keep both (editable name),
  Merge (folders), Cancel operation; "Apply to all"; `Enter` = Keep both, `Escape` = Skip.
  One sheet at a time; further conflicts queue.
- **Progress**: the Phase 5 pattern: a thin bar under the pane title bar of the destination
  pane (or the source pane for trash), stop and pause buttons in the status bar, bytes and
  items, the current name middle-elided, rate and ETA after about 2 s of stable samples. Short
  operations (< 300 ms) show only the finishing notice. `ops.toggle_panel` lists all running
  and the last 20 finished operations with their errors.
- **Notices**: "Moved 3 items to the trash. Undo" ("3 öğe çöp kutusuna taşındı. Geri al");
  failures summarise counts and open the panel for details. `Error::message` is never shown
  untranslated; `ErrorCode` picks the text (Phase 7 adds `NoSpace`, `ReadOnlyFilesystem`,
  `CrossDevice`, `NotEmpty`).

### Deferred

Persistent undo history; batch rename (Phase 9); drops onto executables and "Open console
here" (Phase 10); archives as sources or targets (Phase 10); GVfs / `trash://` of other
programs' mounts (Phase 10); `directorysizes`; sweeping stale `.part` files; copying
`security.*` xattrs; a "new empty file" command; KDE-style ".directory" handling; cross-window
drag (multi-window phase).

Work split for Phase 7: **contracts** (this diff) -> **ops** (`src/ops`, `tests/ops`, against
a fake `ITrash` first) and **platform** (`trash`, `transfer`, `tests/platform`) and
**commands** (ids, names, chords, `tests/commands`) in parallel -> **ui** (progress, sheets,
editors, Trash view, clipboard, DnD) -> integration (`src/ops` / `tests/ops` subdirs, links,
quit prompt) -> **qa** (atomicity: kill / cancel mid-copy leaves no truncated destination;
partial failures through the fault injector; cross-filesystem move with a tmpfs fixture when
available; undo/redo round trips including "changed since" and "appeared since"; Nautilus
clipboard interop; Wayland/X11 drag notes) -> **design-reviewer** (sheets, progress, DnD
feedback).

## Phase 8: command palette, remappable keymap, searchable context menu, Open With

### Ownership

| Module | Owns (Phase 8) |
|--------|----------------|
| contracts | `commands.hpp` additions, `keymap_file.hpp`, `palette.hpp`, `menu.hpp`, `openwith.hpp`, this section, `docs/keymap.md`, `tests/contracts/phase8_usage.cpp` |
| commands | `src/commands`: key sequences (`KeySequence`, prefix-free `Keymap`, `SequenceMatcher`), keypad/Menu key names, command metadata (`CommandInfo` fields, category, danger, arg kinds, context requirements, `builtin_command_variants`), the keymap file (`keymap_file.hpp`: JSON, layering, atomic I/O, editor helpers), the Phase 8 `CommandId`s and default chords, `tests/commands` |
| search | `src/search` (`tests/search`): `palette.hpp` (entry ranking with `search::Matcher`, history, `palette_history.json`). It lives here because `src/commands` must not depend on `src/search`; the header takes plain structs and needs `commands.hpp` for value types only (no link dependency) |
| platform | `src/platform` (`tests/platform`): `openwith.hpp` (Desktop Entry, MIME Applications, shared-mime-info aliases/subclasses, Exec expansion, launching through `IAppLauncher`, `mimeapps.list` writer) |
| ui | `src/ui` (`tests/ui`): palette overlay (and its pickers), Settings overlay with the Keyboard page (the keymap editor), searchable context menus (`menu.hpp` builders, pins, filter, drawn in `src/ui/menu`), key sequence feed and status-bar hint, shortcuts shown everywhere, Open With popup and submenu, SDL keypad / Menu key translation, i18n strings, focus scoping |
| qa | keymap load/save/layering/sequence tests, palette and menu ranking safety tests, Open With fixtures, the restart-persistence smoke test, extension of `tests/integration/keymap_e2e_test.cpp` |

`menu.hpp` is implemented in `src/ui` (the builders replace menus ui already draws; the filter
uses `search::Matcher`, which ui links). If ui is overloaded the coordinator may give the
builders and the pin file to `commands`; the header is the contract either way.

### Data flow

```
 startup:  load_keymap_file(<config>/keymap.json)  -> KeymapFile (lenient, LoadReport)
           build_keymap(KeymapFile, registry)      -> Keymap (defaults + user layer, ApplyReport)
           load_history / load_pins                -> palette::History, menu::Pins
 key press (after focus scoping)
           SequenceMatcher::feed(keymap, chord, now)
              NoMatch   -> resolve_command(keymap, registry, chord) -> CommandQueue::enqueue
              Pending   -> status-bar hint "Ctrl+K ..."; consumed
              Match     -> to_command(registry, binding)          -> CommandQueue::enqueue
              Cancelled / Aborted -> consumed, hint "... is not a command"
 palette.open (Ctrl+Shift+P)
           capture CommandContext  -> build palette::Entry list (registry, tr, keymap, aliases)
           per keystroke: palette::rank(query, entries, history, context)
           Enter: Command (+ picker for OpId / BookmarkRow) -> CommandQueue::enqueue; History::record
 right click / Menu / Shift+F10
           menu::build(kind, registry, context, target) -> localize -> apply_shortcuts -> arrange(pins)
           typing: menu::filter(...)  Enter / click: Item::command -> CommandQueue::enqueue
           Open With items: list_handlers on a worker -> Item::Kind::OpenWith rows
 Open With (menu, popup, open_with.show)
           platform::mime_type(path)  -> openwith::list_handlers(mime, system_options()) (worker)
           pick -> openwith::launch_with(handler, paths, launcher) at frame end (no waiting)
 keymap editor edit
           check_binding -> (user confirms replace) -> set_binding / remove_binding / reset_*
           -> build_keymap -> replace the Keymap, SequenceMatcher::reset() -> save (worker, ~250 ms)
```

Everything that changes program state goes through the `CommandQueue` and runs at frame end;
the palette, the menu, aliases and sequences are only different ways to enqueue a `Command`. The
Open With launch is the one non-command action (it needs a desktop id) and runs in the same
frame-end slot, like `platform::open_with_default`.

### Safety rules (normative)

- **X1 No bypass.** Palette entries, menu items, aliases and key sequences enqueue a `Command`
  and nothing else. No `arg` of any command skips a confirmation, and the confirm-sheet commands
  (`file.delete_permanently`, `trash.empty`, `layout.delete`, `keymap.reset_defaults`) ignore `arg`.
  Every path to a destructive command therefore reaches the same sheet (initial focus on Cancel).
- **X2 Destructive is never the default.** In the palette ranking and in the menu filter a
  destructive result is never first / highlighted while a safe one exists; if only destructive
  results exist, nothing is highlighted and Enter does nothing until the user moves with Down.
  Destructive items are last and separated in menus, have no accelerator letter, cannot be
  pinned, and are never recorded in the palette history (no frequency boost).
- **X3 Danger is data.** `CommandInfo::danger` is Destructive for exactly `file.delete_permanently`,
  `trash.empty`, `layout.delete`, `keymap.reset_defaults`; `effective_danger` makes `file.trash`
  destructive in the Trash view. A new destructive command must set the flag and have a sheet.
- **X4 Bare destructive bindings warn** (`is_destructive_bare_binding`); they are not refused.
- **X5 Sequences cannot steal typing.** A multi-chord sequence cannot start with a text-input chord
  (`is_text_input_chord`), cannot contain a bare Escape, and is never fed while a text field consumes
  the chord; a sheet or overlay resets it.
- **X6 Aliases and bindings carry only stable args.** For `ArgKind` OpId / TabId / PaneId /
  BookmarkId / BookmarkRow the arg must be 0 (runtime ids are not persistent).
- **X7 A bad file never hurts.** `keymap.json`, `palette_history.json` and `menu_pins.json` load
  leniently (unknown fields ignored, invalid entries dropped and reported, non-objects moved to
  `.bad`), never crash, and a destructive id in a pins file is ignored when arranging.
- **X8 No shell.** Open With builds argv from the Exec line (`expand_exec`), values are never
  re-split or re-expanded, `Terminal=true` needs an explicit terminal prefix.
- **X9 Tests touch only fixtures** (keymap, history, pins and Open With tests use temporary
  config / data directories through the injectable roots).

### Command metadata

`register_builtin_commands` fills `CommandInfo` from this table. Defaults: category by name
prefix as listed, `Danger::Safe`, `ArgKind::None`, palette visible, bindable, no requirements.
Only deviations are written. "Hidden when" is `hidden_if_any`; "Needs" is `requires_all`.
`description_key` is `cmd.<name>.desc` for every command (the catalogs may omit it).

| Category | Commands (name: deviations) |
|----------|-----------------------------|
| App | `app.quit`, `view.toggle_theme`, `view.toggle_fps_overlay` |
| View | `view.toggle_hidden`, `view.toggle_sidebar`; `view.toggle_view_lock`: reserved (not palette-visible, not bindable) |
| Navigation | `nav.back`: needs CanGoBack; `nav.forward`: needs CanGoForward; `nav.up`, `nav.refresh`, `nav.focus_address_bar`, `nav.goto` |
| List | `list.activate`: needs HasCursorRow; `list.open_in_new_tab`: needs HasCursorRow; `list.select_all`; `list.clear_selection`; `list.toggle_cursor_selected`: needs HasCursorRow; `list.cursor_up/down/page_up/page_down/home/end`: Flags, not palette-visible |
| Tabs | `tab.new`: Flags; `tab.close`, `tab.duplicate`, `tab.close_left`, `tab.move_left`, `tab.move_right`: TabId; `tab.reopen_closed`: needs ClosedTabsAvailable; `tab.next`, `tab.previous`, `tab.search` |
| Panes | `pane.split_right`, `pane.split_down`: PaneId; `pane.close`: PaneId, needs MultiplePanes; `pane.focus_next`, `pane.focus_previous` |
| Layouts | `layout.save`, `layout.load`; `layout.delete`: Destructive |
| Windows | `window.new`: reserved |
| Sidebar | `sidebar.focus`; `bookmarks.toggle`: BookmarkId; `bookmarks.rename`, `bookmarks.move_up`, `bookmarks.move_down`: BookmarkRow; `recents.clear`, `recents.toggle_tracking` |
| Edit | `edit.copy_path`: Flags; `edit.copy`, `edit.cut`: needs HasSelection, hidden when SidebarFocused; `edit.paste`: Flags, hidden when SidebarFocused or TrashView; `edit.undo`: needs CanUndo; `edit.redo`: needs CanRedo |
| Files | `file.trash`: needs HasSelection, hidden when SidebarFocused; `file.delete_permanently`: Destructive, needs HasSelection, hidden when SidebarFocused; `file.rename`: needs HasCursorRow, hidden when TrashView; `file.new_folder`: hidden when TrashView or SidebarFocused; `file.copy_to_other_pane`, `file.move_to_other_pane`: needs HasSelection and MultiplePanes, hidden when SidebarFocused or TrashView; `file.open_terminal_here`: reserved |
| Operations | `ops.cancel`, `ops.toggle_pause`: OpId, needs OperationsRunning; `ops.toggle_panel` |
| Search | `list.filter`; `list.clear_filter`: needs FilterActive; `search.flatten`, `search.flatten_volume`, `search.types`, `search.toggle_fuzzy`, `search.rebuild_index`; `search.cancel`: needs SearchRunning |
| Inspector | `view.inspect`; `inspector.toggle_lock`, `inspector.cycle_placement`: needs InspectorVisible; `inspector.play`: needs InspectorVisible; `inspector.zoom_fit`, `inspector.zoom_1to1`, `inspector.toggle_grid`, `inspector.toggle_minimap`: need InspectorShowsImage |
| Trash | `trash.open`: hidden when TrashView; `trash.restore`: needs TrashView and HasSelection; `trash.empty`: Destructive, needs TrashView |
| Settings | `palette.open`, `menu.show_context`: not palette-visible; `settings.open`; `keymap.edit`; `keymap.reset_defaults`: Destructive; `keymap.export` |
| OpenWith | `open_with.show`: needs HasCursorRow, hidden when TrashView |

`builtin_command_variants()`: `edit.copy_path` with `CopyPathArg::CurrentFolder` (`cmd.edit.copy_path.folder`),
`tab.new` with `NewTabArg::Home` (`cmd.tab.new.home`), `edit.paste` with `PasteArg::IntoCursorFolder`
(`cmd.edit.paste.into_folder`, needs CursorOnFolder).

Pickers (ui-internal, one level, same overlay): `ops.cancel` / `ops.toggle_pause` ask for an
operation when `running_operations >= 2` (rows from `IOperationQueue::snapshot`, label and
progress); `bookmarks.rename` / `move_up` / `move_down` ask for a bookmark unless the captured
context has `BookmarkRowFocused`. The picked value becomes `Command::arg`.

### Persistence

| File (under the config root, `AppConfig::config_dir`) | Content | Written | Owner |
|------------------------------------------------------|---------|---------|-------|
| `keymap.json` | `KeymapFile` v1 | about 250 ms after each editor change, on quit when dirty; atomic | commands (`save_keymap_file`) |
| `palette_history.json` | `palette::History` v1, at most 100 items | about 1 s after a recorded command, on quit; atomic | search (`save_history`) |
| `menu_pins.json` | `menu::Pins` v1, at most 16 ids | when pinned / unpinned, debounced; atomic | ui (`save_pins`) |

All three follow the session rules: dirs 0700, files 0600, temporary file + fsync + rename, loads
on the UI thread at startup only, writes from a worker with a copy, `restore_session` /
`save_session` govern them, and headless runs without `config_dir` never touch them. A rebind
made in one run is in `keymap.json` before the user can quit, so the next start (which calls
`build_keymap`) has it. Hot reload across instances is Phase 9.

### Open With

`openwith.hpp` reads the freedesktop files itself (no GIO). Order of precedence and the filters
are in the header; in short: `mimeapps.list` files (desktop-specific, config, data) for
`[Default Applications]`, `[Added Associations]`, `[Removed Associations]`, then the legacy
`defaults.list`, then `mimeinfo.cache`, for the file's MIME type and its parents
(`shared-mime-info` aliases and subclasses; never `application/octet-stream`). The MIME type
comes from Phase 2's name table (`platform::mime_type`); there is no glob database or content
sniffing, so unknown extensions have no handlers (documented limitation). `Exec` field codes
are expanded without a shell; one invocation per path for `%f` / `%u`, one for all paths for
`%F` / `%U` (split above 64 KiB of arguments). `Terminal=true` applications launch through a
terminal prefix (`default_terminal_prefix`) or fail with `Unsupported`. `set_default_handler`
writes `mimeapps.list` ("Always open with this").

### Settings overlay (Phase 8 scope)

`settings.open` (`Ctrl+,`) opens a Settings overlay with a single page, "Keyboard" (the keymap
editor), so the chord and the command are real; `keymap.edit` opens the same page directly.
Phase 9 adds the other pages (font, spacing, colour schemes, ...) behind the same command. There
is no `settings.json` in Phase 8.

### i18n keys the ui adds (English and Turkish)

`cmd.palette.open`, `cmd.settings.open`, `cmd.keymap.edit`, `cmd.keymap.reset_defaults`,
`cmd.keymap.export`, `cmd.menu.show_context`, `cmd.open_with.show`,
`cmd.list.toggle_cursor_selected`; the three variant keys above; `category.<name>` for the 18
`CommandCategory` values; optional `cmd.<name>.desc`; palette strings (placeholder, "no matching
command", "Pick an operation", "Pick a bookmark", recents header, alias hint); keymap editor
strings (conflict, the four warnings, "press the new shortcut", reset, replace); menu strings
("Search actions", pin / unpin, "Open With", "Loading", "No applications found", "Always open
with this"); the sheet text of `keymap.reset_defaults`; `key.menu` is not needed (key legends are
not translated, "Menu").

### Deferred

Hot-reloaded keymap across instances (Phase 9); a settings file and the other settings pages
(Phase 9); separate shortcuts per zoom level (Phase 9 views); a keymap import command and a
printable shortcut cheat sheet; third-party / plugin menu items and commands (Phase 10 plugin
ABI; `CommandCategory::Plugins` is reserved); right-drag menu; per-pane sequence status hint
styling; MIME globs and content sniffing for Open With; Desktop Actions; a notice centre
(docs/review/phase-7.md; the palette already reaches `edit.undo`).

Work split for Phase 8: **contracts** (this diff) -> **commands** (`src/commands`: sequences,
metadata, keymap file, ids and chords, `tests/commands`), **search** (`src/search`: palette ranking
and history, `tests/search`) and **platform** (`src/platform`: Open With, `tests/platform`) in
parallel -> **ui** (palette, Settings/keymap editor, menus and pins, sequence feed, keypad and
Menu key translation, Open With popup, i18n) -> integration (root `CMakeLists.txt` needs no new
subdirectory: all code goes into existing `src/commands`, `src/search`, `src/platform`, `src/ui`) ->
**qa** (the restart smoke test, sequence timing with synthetic clocks, lenient-load fuzz, palette
and menu "never first" invariants, Open With fixture trees, e2e keymap doc test) ->
**design-reviewer** (palette, editor, menu search).

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
- Pure functions (`mime::*`, `fs::compare_names`/`sort_entries`, `to_string`/`parse_key_chord`, `layout::Layout`/`ClosedTabs` members, `layout::to_json`/`*_from_json`) are callable from any thread, including the UI thread.
- Pure Phase 4 helpers (`places::*` store members, resolution and JSON functions, `path_input::expand_user_path` / `parse_completion_input` / `match_completions` / `format_paths_for_copy`, `platform::parse_user_dirs`, volume identity/usage helpers) are callable from any thread. `path_input::fix_path_case` / `resolve_input` / `complete_path`, `places::load_*` / `save_*`, `platform::read_user_dirs` / `user_home` block (worker threads; the startup load and save on quit are the UI-thread exceptions, as for the session).
- `layout::Session` is a value owned by the UI thread; the background session writer receives a copy. `layout::load_session`/`save_session`/`*_named_layout` block on file I/O; Phase 3's exceptions on the UI thread are the startup load and the save on quit.
- Interfaces (`IWatcher`, `IVolumeMonitor`, `IMountSource`, `IAppLauncher`, `IIndex`, `IPreviewProvider`, `IPreviewService`, `IProcessRunner`, Phase 7 `ops::IOperationQueue`, `trash::ITrash`) document their own threading. `IWatcher`/`IVolumeMonitor` are single-thread-at-a-time objects with no internal thread (drained with `poll()`, optional blocking `wait()` / pollable `native_handle()`).
- Phase 6: `preview::*` and `thumbnail::*` pure helpers (marked Pure in the headers) are callable from any thread, including the UI thread for key computation. `IPreviewProvider::produce` is called concurrently on preview workers (providers are reentrant); `ProviderRegistry` is built on one thread and then shared read-only. `IPreviewService` owns its workers, is internally synchronized, never blocks the caller (except `shutdown`), and is drained by one consumer (`poll`, UI thread); results are shared immutable `std::shared_ptr<const PreviewResult>`. Thumbnail file functions and `platform::IProcessRunner::run` block (workers only). Textures are created on the UI thread only.
- Phase 5: `search::*` free functions, `Query`, `TypeFilter` and the inline `index.hpp` helpers are pure (any thread, including the UI thread for the folder filter up to `kUiThreadFilterLimit` entries). A `search::Matcher` and an `fs::Walker` are single-thread-at-a-time objects; `Walker` blocks on I/O (workers only). An `IIndex` owns one service thread; `query()`/`status()` are callable from any thread concurrently, `start()`/`stop()`/`request_rescan()` from the owning (UI) thread.
- Phase 7: `ops::IOperationQueue` members are thread-safe and non-blocking (UI thread calls them every frame); its workers run `run_operation`, which blocks. `ops::UndoStack` is UI-thread only. `trash::ITrash` members are thread-safe and block (workers only), except the lexical `is_inside_trash`. `transfer::*`, `ops::` pure helpers and `trash::` format helpers are pure (any thread).
- Phase 8: `Keymap`, `SequenceMatcher`, `KeymapFile` and `palette::History` / `menu::Pins` values are UI-thread owned (the writers get copies). Pure functions (`keymap_from_json`, `to_json`, `apply_keymap_file`, `build_keymap`, `check_binding`, editor helpers, `palette::rank`, `menu::build` / `arrange` / `filter`, `openwith::parse_desktop_entry` / `parse_exec_tokens` / `expand_exec`) are callable from any thread. `load_keymap_file` / `save_keymap_file`, `palette::load_history` / `save_history`, `menu::load_pins` / `save_pins` and `openwith::list_handlers` / `default_handler` / `find_handler` / `mime_parents` / `set_default_handler` block (workers; the startup loads are the UI-thread exception). `openwith::launch_with` never waits (frame-end safe).
- Cancellation uses `std::stop_token`; a cancelled call fails with `ErrorCode::Cancelled`.

## Public headers

| Header | Purpose | Status |
|--------|---------|--------|
| `types.hpp` | `Path`, `FileId`, `Timestamp`, `Span`, `Arena` (decl), `ErrorCode`, `Error`, `Result`, `fail`, `ok` | Phase 0, concrete (`Timestamp` Phase 1) |
| `detail/expected.hpp` | `std::expected` fallback used by `Result` | Phase 0, concrete |
| `commands.hpp` | `CommandId`, `CursorArg`, `Command`, `CommandQueue`, `CommandInfo` (Phase 8: category, description, danger, arg kind, palette/bindable flags, context requirements), `CommandCategory`, `ArgKind`, `Danger`, `ContextFlags`, `CommandContext`, `is_applicable`, `effective_danger`, `CommandVariant`, `CommandRegistry`; keymap: `KeySequence`, `parse_key_sequence`, `validate_sequence`, `is_text_input_chord`, `SequenceBinding`, `BindingConflict`, `find_conflicts`, `SequenceMatcher`, `SequenceResult`, `to_command`, keypad / Menu `Key` values, `Key`, `Modifiers` (+ operators, `has`), `KeyChord`, `to_string(Key/KeyChord)`, `parse_key_chord`, `KeyBinding`, `Keymap`, `load_default_keymap`, `resolve_command` | Phase 0; keymap + Phase 2 ids are a Phase 2 contract, implemented in `src/commands` (commands) |
| `keymap_file.hpp` | `KeymapFile`, `UserBinding`, `UserUnbind`, `Alias`, `is_valid_alias_name`, `alias_names_equal`, `keymap_from_json`, `to_json`, `LoadReport`, `load_keymap_file`, `save_keymap_file`, `keymap_file_path`, `ApplyReport`, `apply_keymap_file`, `build_keymap`, `effective_aliases`, `BindingWarning`, `BindingCheck`, `check_binding`, `is_destructive_bare_binding`, `set_binding`, `remove_binding`, `reset_command`, `reset_all`, `set_alias`, `remove_alias`, `set_sequence_timeout` | Phase 8 contract, implemented in `src/commands` (commands) |
| `palette.hpp` | `palette::Entry`, `history_key`, `is_recordable`, `History`, `HistoryItem`, `history_boost`, `history_from_json`, `to_json`, `load_history`, `save_history`, `history_file`, `MatchedVia`, `Hit`, `SearchOptions`, `SearchResult`, `rank`, scoring constants | Phase 8 contract, implemented in `src/search` (search) |
| `menu.hpp` | `menu::Item`, `MenuKind`, `Target`, `build`, `apply_shortcuts`, `Pins`, `pins_from_json`, `to_json`, `load_pins`, `save_pins`, `pins_file`, `arrange`, `Row`, `FilterResult`, `filter` | Phase 8 contract, implemented in `src/ui` (ui) |
| `openwith.hpp` | `openwith::Options`, `system_options`, `Handler`, `parse_desktop_entry`, `shown_in`, `mime_parents`, `list_handlers`, `default_handler`, `find_handler`, `set_default_handler`, `parse_exec_tokens`, `ExpandOptions`, `expand_exec`, `default_terminal_prefix`, `LaunchOptions`, `launch_with` | Phase 8 contract, implemented in `src/platform` (platform) |
| `ui.hpp` | `AppConfig` (Phase 2: `animations`, `start_path`; Phase 3: `config_dir`, `restore_session`, `restore_window_geometry`, `save_session`; Phase 5: `cache_dir`, `enable_index`; Phase 6: `thumbnail_dir`, `preview_cache_bytes`, `enable_thumbnails`, `enable_external_thumbnailers`), `Theme`, `FrameStats`, `App` (pimpl) | Phase 0, implemented in `src/ui`/`src/app` |
| `places.hpp` | `places::LocationStatus`, `ResolvedLocation`, `is_tracked_volume`, `containing_volume`, `identity_for`, `resolve_location`, `deepest_containing`, `default_label`, `PlaceKind`, `label_key`, `Place`, `build_places`, `BookmarkId`, `Bookmark`, `BookmarkStore`, `RecentEntry`, `RecentsOptions`, `RecentsStore`, `bookmarks_to_json`/`_from_json`, `recents_to_json`/`_from_json`, `bookmarks_file`, `recents_file`, `load/save_bookmarks`, `load/save_recents`, constants | Phase 4 contract, implemented in `src/core` (core-fs) |
| `path_input.hpp` | `path_input::normalize_absolute`, `EnvLookup`, `UserHomeLookup`, `ExpandContext`, `ExpandedPath`, `expand_user_path`, `CaseFix`, `fix_path_case`, `ResolvedInput`, `resolve_input`, `CompletionQuery`, `parse_completion_input`, `CompletionMatch`, `CompletionOptions`, `Completion`, `match_completions`, `complete_path`, `PathQuoting`, `quote_for_shell`, `format_paths_for_copy` | Phase 4 contract, implemented in `src/core` (core-fs) |
| `layout.hpp` | `layout::PaneId`/`TabId`/`SplitId`, `TabState`, `SplitAxis`, `SplitEdge`, `axis_of`, `Tab`, `PaneNode`, `SplitNode`, `LayoutNode`, `TabLocation`, `SplitResult`, `Layout`, `ClosedTab`, `ClosedTabs`, `WindowGeometry`, `Session`, Phase 4 `SidebarSection`/`SidebarState` (+ width constants), Phase 5 `TabState::type_filter` (JSON `"hidden_types"`), Phase 6 `InspectorPlacement`/`InspectorState` (+ constants, `resolve_inspector_placement`, `next_placement`, JSON `"inspector"`), `to_json`, `session_from_json`, `layout_to_json`, `layout_from_json`, `session_file`, `layouts_dir`, `load_session`, `save_session`, `is_valid_layout_name`, `save/load/list/delete_named_layout`, constants | Phase 3 contract, implemented in `src/core` (core-fs) |
| `i18n.hpp` | `Language`, `set_language`, `language`, `tr` | Phase 0, implemented in `src/ui` |
| `fs.hpp` | `fs::DirEntry`, `EntryKind`, `ListOptions`, `list_directory` (+ `std::stop_token` overload), `stat`, `SortKey`, `SortOptions`, `sort_entries`, `compare_names`, `fold_name`, Phase 5 `fold_name_into`, `to_display_name`, inline `is_hidden_name`/`extension`/`is_directory_like` | Phase 1 contract, implemented in `src/core` (core-fs) |
| `watch.hpp` | `WatchHandle`, `WatchEventKind`, `WatchEvent`, `IWatcher`, `create_inotify_watcher`, `WatcherFactory` | Phase 1 contract, implemented in `src/platform` (platform) |
| `volume.hpp` | `MountEntry`, `parse_mountinfo`, `is_pseudo_filesystem`, `is_network_filesystem`, `VolumeKind`, `SpaceInfo`, `VolumeInfo`, `IMountSource`, `create_proc_mount_source`, `VolumeQuery`, `list_volumes`, `volume_for`, `VolumeEvent(Kind)`, `IVolumeMonitor`, `create_volume_monitor`; Phase 4 inline `used_fraction`, `VolumeIdentity`, `identity_of`, `volume_matches` | Phase 1 contract, implemented in `src/platform` (platform) |
| `platform.hpp` | `platform::LaunchRequest`, `IAppLauncher`, `create_default_launcher`, `open_with_default` (+ launcher overload), `mime_type`, `UserDir`, `user_dir`; Phase 4 `UserDirs`, `parse_user_dirs`, `read_user_dirs`, `user_home`; Phase 6 `thumbnail_cache_dir`, `data_dirs`, `find_program`, `ProcessRequest`, `ProcessResult`, `IProcessRunner`, `create_default_process_runner`; stubs `move_to_trash`, `open_terminal` | Phase 2 contract (launcher, open, mime_type, user_dir Home/Config/Cache), Phase 4 (XDG user dirs, `user_home`), Phase 6 (process runner, data dirs), implemented in `src/platform` (platform) |
| `mime.hpp` | `mime::k*` constants, `guess_from_name`, `guess_for_entry`, `type_label_key` (name table + Type-column i18n keys) | Phase 2 contract, implemented in `src/platform` (platform); label texts in the ui i18n catalogs |
| `walk.hpp` | `fs::WalkOptions`, `WalkEntry`, `WalkSkip`, `WalkStats`, `WalkBatch`, `Walker`, constants | Phase 5 contract, implemented in `src/core` (core-fs) |
| `search.hpp` | `search::MatchMode` (+ `to_string`), scoring constants, `Query`, `compile_query`, `narrows`, `HighlightRange`, `Match`, `Matcher`, `match`, `RankInfo`, `ranks_before`, `TypeKind`, `TypeKey`, `TypeFilter`, `TypeCount`, `type_key_of`, `allows`, `is_visible`, `set_visible`, `toggle_all`, `normalize`, `summarize_types`, `FilterHit`, `filter_entries` | Phase 5 contract, implemented in `src/search` (search) |
| `index.hpp` | `search::IndexState` (+ `to_string`), `IndexStatus`, `RescanKind`, `IndexQuery`, `IndexHit`, `IndexResult`, `IIndex`, `NameIndexOptions`, `create_name_index`; inline `should_index_by_default`, `fnv1a64`, `index_stable_id`, `index_dir`, `index_file`; file format v1 | Phase 5 contract, implemented in `src/search` (search) |
| `ops.hpp` | `ops::OpId`, `OpKind` (+ `to_string`), `ConflictPolicy`, `ConflictAction`, `ConflictDecision`, `ItemGuard`, `ConflictSide`, `ConflictId`, `Conflict`, `OpOptions`, `OpOrigin`, `OpRequest`, `OpPhase`, `OpProgress`, `ItemStatus`, `ItemOutcome`, `UndoStep`, `UndoId`, `UndoRecord`, `OpResult`, `UndoLabel`, `make_label`, `NameParts`, `split_extension`, `stem_length`, `kNameMaxBytes`, `keep_both_name`, `NameError`, `validate_new_name`, `is_same_or_inside`, `Protection`, `ProtectedPaths`, `protection_of`, `ConflictResolver`, `ProgressSink`, `EngineOptions`, `run_operation`, `OpEvent`, `QueueOptions`, `IOperationQueue`, `create_operation_queue`, `kDefaultUndoDepth`, `UndoStack`; safety rules S1..S16 | Phase 7 contract, implemented in `src/ops` (ops) |
| `trash.hpp` | `trash::LocalDateTime`, `format_deletion_date`, `parse_deletion_date`, `escape_path`, `unescape_path`, `TrashInfo`, `format_trashinfo`, `parse_trashinfo`, `shared_topdir_trash`, `user_topdir_trash`, `info_path_for`, `original_path_from`, `TrashDirKind`, `TrashDir`, `TrashedItem`, `TrashListing`, `TrashOptions`, `ITrash`, `create_trash`; rules T1..T7 | Phase 7 contract, implemented in `src/platform` (platform) |
| `transfer.hpp` | `transfer::kMime*`, `file_uri`, `path_from_file_uri`, `encode_uri_list` (2), `UriList`, `decode_uri_list`, `ClipboardAction`, `FileClipboard`, `encode_gnome_copied_files`, `decode_gnome_copied_files`, `decode_nautilus_text`, `make_clipboard_offer`, `ClipboardGetter`, `read_clipboard`, `DropAction`, `DropContext`, `choose_drop_action` (inline) | Phase 7 contract, implemented in `src/platform` (platform) |
| `preview.hpp` | `preview::PixelSize`, limits, `ImageBuffer`, `make_image`, `ExifOrientation` (+ `orientation_from_tag`, `swaps_axes`, `oriented_size`), `Rational`, `ExifDateTime`, `ExifInfo`, `ImageFormat` (+ `to_string`), `sniff_image_format`, `ImageInfo`, `MetadataSource`, `VideoInfo`, `TextEncoding`, `TextOptions`, `TextRow`, `TextPreview`, `looks_binary`, `decode_text`, `FolderOptions`, `FolderPreview`, `PreviewPurpose`, `ImageOrigin`, `PlaceholderReason`, `PlaceholderPreview`, `ImagePreview`, `VideoPreview`, `PreviewContent`, `PreviewKind`, `PreviewResult`, `approx_bytes`, `SourceLocation`, `PreviewRequest`, `PreviewKey`, `make_key`, `hash_value`, `PreviewKeyHash`, `fit_size`, `select_jpeg_scale_denom`, `apply_exif_orientation`, `downscale`, priorities, `IPreviewProvider`, `ProviderRegistry` | Phase 6 contract, implemented in `src/preview` (preview) |
| `thumbnail.hpp` | `thumbnail::ThumbnailSize` (+ `pixels`, `dir_name`, `size_for`), constants, `cache_root_for`, `file_uri`, `md5_hex`, `thumbnail_file_name`, `thumbnail_path`, `failure_path`, `is_inside_cache`, `mtime_seconds`, `SourceInfo`, `LookupStatus`, `ThumbnailRead`, `read_thumbnail`, `lookup_thumbnail`, `WriteOptions`, `write_thumbnail`, `write_failure`, `has_failure`, `Policy`, `Decision`, `decide`, `Thumbnailer`, `parse_thumbnailer`, `split_exec`, `ExecInputs`, `expand_exec`, `thumbnailer_dirs`, `ProgramExists`, `load_thumbnailers`, `choose_thumbnailer`, `ThumbnailerLimits`, `run_thumbnailer` | Phase 6 contract, implemented in `src/preview` (preview) |
| `preview_service.hpp` | `preview::BuildFeatures`, `build_features`, `BuiltinProviderOptions`, `register_builtin_providers`, `RequestPriority`, `Ticket`, `Completion`, `ThumbnailCacheOptions`, `PreviewServiceOptions`, `InvalidateMode`, `ServiceStats`, `IPreviewService`, `create_preview_service`, constants | Phase 6 contract, implemented in `src/preview` (preview) |
| `plugin.h` | C ABI: `NAVIGATOR_PLUGIN_ABI_VERSION`, `NavigatorPluginInfo`, entry symbol | stub |

Stub functions are declared only; nothing may call them until their phase lands. Public headers never include SDL, GIO, ImGui, FreeType or HarfBuzz. `tests/contracts` compiles each header alone (C++20 and C++23, `-Wall -Wextra -Wpedantic -Werror`) plus `plugin.h` as C11, and compiles (never links) `result_usage.cpp`, `phase1_usage.cpp` (fake `IWatcher`/`IMountSource` implementations prove the interfaces are injectable) `phase2_usage.cpp` (keymap, fake `IAppLauncher`, mime, `AppConfig`) and `phase3_usage.cpp` (layout operations, closed tabs, session JSON/files, tab/pane command ids, `AppConfig` session fields) and `phase4_usage.cpp` (bookmarks offline/resync, recents, places, user dirs, GoTo expansion/completion, copy path, sidebar session state, Phase 4 command ids) and `phase5_usage.cpp` (walker batches, matcher/ranking, types popup helpers, a fake `IIndex`, default index policy and file location, `TabState::type_filter`, `AppConfig` index fields, Phase 5 command ids) and `phase6_usage.cpp` (preview helpers, a fake provider and the registry, thumbnail keys/read/write/policy, thumbnailers through a fake `IProcessRunner`, a fake `IPreviewService`, `InspectorState`, `AppConfig` preview fields, Phase 6 command ids); target `navigator_header_check`.

## Commands

- `CommandId` values are stable and never renumbered. Ranges: 1-999 app/view, 1000-1099 navigation, 1100-1199 list (activation, cursor, selection), 1200-1219 tabs, 1220-1239 panes, 1240-1249 named layouts, 1250-1259 windows, 1260-1279 sidebar/bookmarks/recents, 1280-1299 edit/clipboard, 1300-1319 filter/flatten/search/index, 1320-1339 Inspector/previews, 1340-1359 file operations / operations panel, 1360-1369 trash, 1370+ later phases.
  - Phase 8: `ToggleCursorSelected = 1103` (`list.toggle_cursor_selected`, `Ctrl+Space`), `OpenPalette = 1370` (`palette.open`, `Ctrl+Shift+P`), `OpenSettings = 1371` (`settings.open`, `Ctrl+,`), `EditKeymap = 1372` (`keymap.edit`, sequence `Ctrl+K Ctrl+S`), `ResetKeymap = 1373` (`keymap.reset_defaults`, destructive), `ExportKeymap = 1374` (`keymap.export`), `ShowContextMenu = 1375` (`menu.show_context`, `Menu`, `Shift+F10`), `OpenWith = 1390` (`open_with.show`); `CursorArg::KeepSelection` (`Ctrl+Up` / `Ctrl+Down`); `ops.toggle_panel` gains `Ctrl+J`. New ranges 1370-1389 and 1390-1399. Details, metadata table and safety rules: "Phase 8" above.
  - Phase 7: `Copy = 1281` (`edit.copy`, `Ctrl+C`), `Cut = 1282` (`edit.cut`, `Ctrl+X`), `Paste = 1283` (`edit.paste`, `Ctrl+V`, arg `PasteArg`), `Undo = 1284` (`edit.undo`, `Ctrl+Z`), `Redo = 1285` (`edit.redo`, `Ctrl+Shift+Z`, `Ctrl+Y`), `MoveToTrash = 1340` (`file.trash`, `Delete`), `DeletePermanently = 1341` (`file.delete_permanently`, `Shift+Delete`), `Rename = 1342` (`file.rename`, `F2`), `NewFolder = 1343` (`file.new_folder`, `Ctrl+Shift+N`), `CopyToOtherPane = 1344` (`file.copy_to_other_pane`, `F7`), `MoveToOtherPane = 1345` (`file.move_to_other_pane`, `Shift+F7`), `OpenTerminalHere = 1346` (`file.open_terminal_here`, reserved no-op), `CancelOperation = 1347` (`ops.cancel`, arg OpId), `TogglePauseOperation = 1348` (`ops.toggle_pause`, arg OpId), `ToggleOperationsPanel = 1349` (`ops.toggle_panel`), `OpenTrash = 1360` (`trash.open`), `RestoreFromTrash = 1361` (`trash.restore`), `EmptyTrash = 1362` (`trash.empty`). Focus scoping: `docs/keymap.md` "Phase 7 defaults".
  - Phase 6: `Inspect = 1320` (`view.inspect`, `Space`, `Alt+KeypadEnter`), `ToggleInspectorLock = 1321` (`inspector.toggle_lock`), `InspectorZoomFit = 1322` (`inspector.zoom_fit`), `InspectorZoom1to1 = 1323` (`inspector.zoom_1to1`), `InspectorPlay = 1324` (`inspector.play`), `CycleInspectorPlacement = 1325` (`inspector.cycle_placement`), `ToggleInspectorGrid = 1326` (`inspector.toggle_grid`), `ToggleInspectorMinimap = 1327` (`inspector.toggle_minimap`). All arg 0. Space in text fields and Inspector focus routing: `docs/keymap.md` "Phase 6 defaults".
  - Phase 5: `FocusFilter = 1300` (`list.filter`, `Ctrl+F`), `ClearFilter = 1301` (`list.clear_filter`), `ToggleFlatten = 1302` (`search.flatten`, `Ctrl+E`), `FlattenVolume = 1303` (`search.flatten_volume`, `Ctrl+Shift+E`), `ToggleTypesPopup = 1304` (`search.types`, `Ctrl+Shift+F`), `ToggleFuzzy = 1305` (`search.toggle_fuzzy`), `CancelSearch = 1306` (`search.cancel`), `RebuildIndex = 1307` (`search.rebuild_index`). All arg 0. Filter-field focus scoping, type-to-filter and the Escape cascade: `docs/keymap.md` "Phase 5 defaults".
  - Phase 4: `ToggleSidebar = 5` (`view.toggle_sidebar`), `ToggleViewLock = 6` (`view.toggle_view_lock`, stub), `GoTo = 1005` (`nav.goto`), `FocusAddressBar = 1004` now defined (inline path edit, `Ctrl+L`); `FocusSidebar = 1260` (`sidebar.focus`), `ToggleBookmark = 1261` (`bookmarks.toggle`), `RenameBookmark = 1262` (`bookmarks.rename`), `MoveBookmarkUp = 1263` (`bookmarks.move_up`), `MoveBookmarkDown = 1264` (`bookmarks.move_down`), `ClearRecents = 1270` (`recents.clear`), `ToggleRecentsTracking = 1271` (`recents.toggle_tracking`), `CopyPath = 1280` (`edit.copy_path`, arg `CopyPathArg`). Bookmark commands take 0 (focused row / active folder) or a `places::BookmarkId` value. `pane.focus_next/previous` include the visible sidebar in their cycle.
  - Phase 0: `None = 0`, `Quit = 1` (`app.quit`), `ToggleFpsOverlay = 2` (`view.toggle_fps_overlay`), `ToggleTheme = 3` (`view.toggle_theme`).
  - Phase 3: `OpenInNewTab = 1116` (`list.open_in_new_tab`); tabs 1200-1209: `NewTab` (`tab.new`, arg `NewTabArg`), `CloseTab` (`tab.close`), `DuplicateTab` (`tab.duplicate`), `CloseLeftTabs` (`tab.close_left`), `ReopenClosedTab` (`tab.reopen_closed`), `NextTab` (`tab.next`), `PreviousTab` (`tab.previous`), `SearchTabs` (`tab.search`), `MoveTabLeft` (`tab.move_left`), `MoveTabRight` (`tab.move_right`); panes 1220-1224: `SplitRight` (`pane.split_right`), `SplitDown` (`pane.split_down`), `FocusNextPane` (`pane.focus_next`), `FocusPreviousPane` (`pane.focus_previous`), `ClosePane` (`pane.close`); layouts 1240-1242: `SaveLayout` (`layout.save`), `LoadLayout` (`layout.load`), `DeleteLayout` (`layout.delete`); windows: `NewWindow = 1250` (`window.new`, reserved, unbound). Tab commands take arg 0 (active tab) or a `TabId` value, pane commands 0 or a `PaneId` value.
  - Phase 2: `ToggleHidden = 4` (`view.toggle_hidden`); `NavigateBack = 1000` (`nav.back`), `NavigateForward = 1001` (`nav.forward`), `NavigateUp = 1002` (`nav.up`), `Refresh = 1003` (`nav.refresh`), `FocusAddressBar = 1004` (`nav.focus_address_bar`, Phase 4, registered and unbound); `Activate = 1100` (`list.activate`), `SelectAll = 1101` (`list.select_all`), `ClearSelection = 1102` (`list.clear_selection`), `CursorUp/Down/PageUp/PageDown/Home/End = 1110..1115` (`list.cursor_up`, `list.cursor_down`, `list.cursor_page_up`, `list.cursor_page_down`, `list.cursor_home`, `list.cursor_end`).
- Variants are `Command::arg` bit flags, not extra ids: cursor commands take `CursorArg::Extend` (Shift: extend the selection from the anchor). The keymap binds a chord to (name, arg).
- The keymap (`Keymap`, `KeyChord`, `Key`, `Modifiers`) lives in the commands module, with no SDL types; src/ui translates SDL keycodes/scancodes (and mouse X1/X2) into `Key`/`Modifiers` with the layout rules documented in `commands.hpp`. Canonical chord text is ASCII (`Ctrl+Shift+L`); the ui translates modifier names for display (`key.ctrl`, `key.shift`, `key.alt`, `key.super`). Phase 8 adds multi-chord sequences (`KeySequence`, `SequenceMatcher`), aliases and the keymap file (`keymap_file.hpp`).
- i18n title keys: `cmd.` + name (`cmd.app.quit`, ...).
- Every user action has a `CommandId`, appears in the palette (unless `CommandInfo::palette_visible` is false: cursor movement and reserved no-ops), is remappable (unless `bindable` is false) and gets its default binding from `docs/keymap.md`.

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
- Phase 3 (contracts): tabs, split panes, layouts, session. Additions only, except the removal of the unused Phase 0 forward declarations `navigator::Pane`, `Tab`, `Layout` from `ui.hpp` (no user; the model lives in `navigator::layout`).
  - **layout** (new header, core-fs, `src/core`): strong ids `PaneId`/`TabId`/`SplitId` (runtime only, never persisted, fresh on every load); `TabState {path, sort, show_hidden, cursor_name, scroll_rows}`; n-ary tree `LayoutNode = variant<PaneNode, SplitNode>` with fractional `sizes`; `Layout` with invariants I1-I9 (no empty pane except the only one, >= 2 children per split, no same-axis nesting, sizes sum to 1, unique ids, valid active pane, depth <= 32), all-or-nothing edits (`add_tab`, `close_tab`, `close_tabs_left_of`, `close_pane`, `move_tab`, `split_pane` by tab or new `TabState`, `activate_tab`/`activate_pane`, `move_splitter` with min fraction, `equalize`), queries (`find_*`, `tab_state`, `pane_order`, `neighbor_pane`/`neighbor_tab`, `validate`) and the repairing `from_tree`; the active pane lives in `Layout` (persisted in the session as an index into `pane_order`). `ClosedTabs` (bounded, default 25, oldest first). `Session {version, WindowGeometry, Layout, ClosedTabs}`; `WindowGeometry::has_position` false on Wayland. JSON v1 schema in the header, deterministic output, lenient load; `load_session` (moves a non-JSON file to `.bad`), atomic `save_session` (temp + fsync + rename, dirs 0700, file 0600), named layouts with `is_valid_layout_name` under `<root>/layouts/`. The caller passes the config root; core does not call `platform::user_dir`.
  - **fs**: `SortOptions` gains a defaulted `operator==` (needed by `TabState`).
  - **commands** (commands, `src/commands`): ids and names listed under "Commands"; `NewTabArg::{None, Home}`. Default chords in `docs/keymap.md` "Phase 3 defaults", which also resolves the Ctrl+click conflict (Ctrl+click stays toggle selection; Ctrl+double-click and middle-click open in a new tab) and defers `Ctrl+N`.
  - **ui** (ui, `src/ui`): `AppConfig::config_dir` (empty = `user_dir(Config)/navigator`), `restore_session` (default true), `restore_window_geometry` (default true), `save_session` (default true). Data flow, drag-and-drop state machine and persistence timing in "Phase 3" above.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` in `src/commands/builtin_commands.cpp` (`to_string`, line 46), `src/ui/browser/browser_model.cpp` (`BrowserModel::execute`, line 571) and `src/ui/shell/app.cpp` (`App::Impl::execute`, line 432): 20 new enumerators each. Everything else in the tree compiles unchanged.
- Phase 4 (contracts): sidebar, GoTo, address bar, bookmarks, recents. Additions only; no existing signature changed.
  - **volume** (header-only): `used_fraction(SpaceInfo)` (df "Use%"), `VolumeIdentity {uuid, root, device, mount_point}` with `identity_of` and `volume_matches` (uuid + subvolume root when the uuid is known, else device + mount point).
  - **platform** (platform, `src/platform`): `UserDirs`, `parse_user_dirs` (user-dirs.dirs(5) / GLib rules: `$HOME/...` or absolute, backslash escapes, Desktop default only), `read_user_dirs(home, config_home)` (explicit roots for tests; missing file = defaults), `user_home(name)` (getpwnam_r, for `~name`). `user_dir` becomes concrete for Desktop/Documents/Downloads/Music/Pictures/Videos (NotFound when not configured; no more Unsupported).
  - **places** (new header, core-fs, `src/core`): Places list from `UserDirs` (disabled = `$HOME` and duplicates dropped; Trash reserved), `BookmarkStore` (runtime `BookmarkId`s, add/remove/rename/move/find/contains/index_of/resync, max 1000), `RecentsStore` (bounded MRU, visit counts, `RecentsOptions {enabled, highlight stub, sort_on_top stub}`), `LocationStatus` Online/Offline/Missing with pure `resolve_location` (rebasing when a volume returns at another mount point) and a worker-side Missing probe, `identity_for` (Root/Home/Virtual volumes are not tracked), `containing_volume`, `deepest_containing` (sidebar highlight), `default_label`; JSON v1 schemas and `bookmarks.json` / `recents.json` with the session's lenient-load / `.bad` / atomic-save rules.
  - **path_input** (new header, core-fs, `src/core`): `normalize_absolute`; `expand_user_path` (trim + one pair of quotes, `file://` URIs, `~`, `~name`, `$VAR`, `${VAR}`, `%VAR%`, single pass, unresolved tokens reported and kept literal, relative to the active folder, lexical `..`); `fix_path_case` (unique fold-equivalent match per missing component); `resolve_input`; completion split into pure `parse_completion_input` + `match_completions` and blocking `complete_path`; `quote_for_shell` / `format_paths_for_copy` (`PathQuoting::Auto`: one path raw, several shell-quoted and space-joined).
  - **layout** (core-fs): `SidebarSection`, `SidebarState {visible, width, collapsed}` and width constants; `Session::sidebar`, JSON field `"sidebar"` (additive, version stays 1, lenient, absent in older files). Named layouts unchanged.
  - **commands** (commands, `src/commands`): ids and names under "Commands"; `CopyPathArg::{None, CurrentFolder}`; focus-scoping rules documented in `commands.hpp` and `docs/keymap.md` (the `Keymap` API is unchanged). Default chords: `Ctrl+B`, `Ctrl+L`, `Ctrl+G`, `Ctrl+D`, `Ctrl+Shift+C` (`docs/keymap.md` "Phase 4 defaults").
  - **ui** (`ui.hpp`, comment only): `restore_session` / `save_session` also govern `bookmarks.json` / `recents.json`.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` in `src/commands/builtin_commands.cpp` (`to_string`, line 66), `src/ui/browser/browser_model.cpp` (`BrowserModel::execute`, line 728) and `src/ui/workspace/workspace.cpp` (line 258): 11 new enumerators each. `src/ui/shell/app.cpp` and `src/ui/workspace/popups.cpp` have `default:` and compile. Everything else in the tree compiles unchanged. Runtime tests that need updates by their owners: `tests/commands/keymap_test.cpp` (`read_keymap_doc` must also read the `## Phase 4 defaults` table; row counts 3 + 25 + 21 become 3 + 25 + 21 + 5; the expected `(unbound)` set loses `nav.focus_address_bar`, now `Ctrl+L`, and gains the seven Phase 4 unbound commands; until then the "(unbound) rows" test fails because the Phase 2 `(unbound)` row for `nav.focus_address_bar` was moved into a note), `tests/platform/desktop_test.cpp:277` (expects Unsupported for the XDG user dirs), `tests/core/layout_json_test.cpp` golden session text (new `"sidebar"` field once core writes it).
- Phase 4 follow-up: `RecentsStore::restore(std::vector<RecentEntry>)` added so the JSON loader can restore visit counts without private access.
- Phase 5 (contracts): folder filter, flatten, fuzzy name search, name index. `search.hpp` and `index.hpp` turned from stubs into contracts (no caller existed, so the breaking changes are free: `SearchQuery`/`SearchHit` replaced by `search::Query`/`Match`/`IndexHit`, `MatchMode::Glob` and `case_sensitive` dropped because there is no wildcard language and matching is always folded, `IIndex::add_root` replaced by one index per volume via `create_name_index`; everything moved into `navigator::search`). Otherwise additions only.
  - **fs** (core-fs): `fold_name_into(name, out, offsets)`: buffer-reusing fold with folded-to-raw byte offsets; documents that the fold is 1:1 in code points.
  - **walk** (new header, core-fs, `src/core`): `fs::Walker` breadth-first streaming enumerator with a directory table (entries reference parents by index), `WalkOptions {root, max_depth, follow_symlinks = false, cross_filesystems = false, show_hidden = false, skip_unreadable = true, metadata = true, batch_size, batch_latency, max_entries = 1M}`, `WalkStats` (visited, pending, skipped by permission / vanished / error / other fs / cycle, truncated, samples), dev/ino cycle set, hidden directories not entered unless show_hidden (File Pilot v0.5 fix), cancellation checked before each directory and every 1024 entries.
  - **search** (search, new `src/search`): `compile_query` (trim, ASCII-space terms ANDed, cut at 256 bytes), `Matcher` substring/fuzzy with the documented scoring model and constants, byte highlight ranges, `ranks_before` deterministic ranking, `narrows`, `TypeFilter` (hidden types: folders, no extension, folded extensions), `summarize_types` (respects show_hidden), `toggle_all`, `filter_entries` over listings and walk batches, `kUiThreadFilterLimit = 20000`.
  - **index** (search): `IIndex` lifecycle (`start`, `stop`, `request_rescan(Verify|Rebuild)`, thread-safe `status`/`query`), states Idle/Loading/Scanning/Verifying/Ready/Failed/Stopped, inotify watch budget with unwatched-directory bookkeeping and periodic mtime verify, `NameIndexOptions`, binary file format v1 (header, volume block, 48-byte directory and 16-byte entry records, name pool, FNV-1a trailer; corrupt -> delete and rebuild), `should_index_by_default` (Root, Home, writable Internal; never removable/network/virtual/image fs), `index_stable_id`, `index_file` under `$XDG_CACHE_HOME/navigator/index/`.
  - **layout** (core-fs): `TabState::type_filter`; `<tab>` JSON gains optional `"hidden_types"` (additive, version stays 1, written only when active, lenient and normalized on load). `layout.hpp` now includes `search.hpp` for the value type only.
  - **ui** (`ui.hpp`): `AppConfig::cache_dir`, `AppConfig::enable_index` (`--no-index`).
  - **commands** (commands): ids 1300-1307 and names listed under "Commands"; new range 1300-1319. Default chords `Ctrl+F`, `Ctrl+E`, `Ctrl+Shift+E`, `Ctrl+Shift+F` (`docs/keymap.md` "Phase 5 defaults"); type-to-filter, filter-field focus scoping and the Escape cascade documented there and in `commands.hpp`.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` (GCC 13 and Clang 18) in `src/commands/builtin_commands.cpp` (`to_string`, line 78), `src/ui/browser/browser_model.cpp` (line 751) and `src/ui/workspace/workspace.cpp` (line 258): 8 new enumerators each. Everything else in the tree compiles unchanged. Runtime tests that need updates by their owners once commands binds the rows: `tests/commands/keymap_test.cpp` (`read_keymap_doc` must also read `## Phase 5 defaults` and `phase_rows` grows to index 5; counts 3 + 25 + 21 + 5 + 4; the expected `(unbound)` sets gain `list.clear_filter`, `search.toggle_fuzzy`, `search.cancel`, `search.rebuild_index`; `Ctrl+E` leaves the "reserved chords stay free" list) and `tests/integration/keymap_e2e_test.cpp` (line ~881: `per_phase.size()` 4 -> 5 with `per_phase.at(5) == 4`, `bound.size()` 54 -> 58; the `(unbound)` set and its size 10 -> 14; `Ctrl+E` leaves the near-miss list at line 969). Until commands lands, the doc-vs-keymap tests fail because the doc has 4 rows the keymap does not bind yet. `tests/core/layout_json_test.cpp` golden text is unaffected (`"hidden_types"` is only written when active).
- Phase 5 integration: `search.flatten` always walks; the index serves only `search.flatten_volume`. Headless `--frames`/`--bench-*` runs without `--cache-dir`/`--config-dir` never index and never read or write session, config or cache files.
- Phase 6 (contracts): Inspector, thumbnails, previews. `preview.hpp` turned from a stub into a contract (no caller existed, so the breaking change is free: the old `navigator::PreviewRequest` / `PreviewResult` / `IPreviewProvider` with `render(request, atomic<bool>)` and straight-alpha `rgba` are replaced by the `navigator::preview` types below). Otherwise additions only.
  - **preview** (preview, new `src/preview`, library `navigator_preview`): one pixel format (`ImageBuffer`, RGBA8 premultiplied, stride, owned bytes, display orientation); `ImageInfo` reports the EXIF orientation and the stored and display sizes, pixels already have it applied; `ExifInfo` (camera, lens, exposure, ISO, focal length, date taken with offset, GPS presence and coordinates), `VideoInfo` (duration, display size, rotation, frame rate, codecs, audio, streams; `MetadataSource::None` without FFmpeg), `TextPreview` (UTF-8 with U+FFFD repair, UTF-16 BOMs, rows of at most 1 KiB so long lines never cost frame time, first-4-MiB cap with `truncated`), `FolderPreview` (sorted, capped, counts), `PlaceholderPreview` reasons (no provider, coming soon, binary, no thumbnailer, known failure, not generated), `PreviewResult` variant whose index is `PreviewKind`; `PreviewRequest {path, entry snapshot, mime, purpose, target device px, location, folder/text options, stop}`, `PreviewKey`/`make_key`/`hash_value`; limits (65535 px sides, 1 GP sources, 64 MP full decodes, 32 MP outputs, 256 MiB per decode); pure `sniff_image_format`, `looks_binary`, `decode_text`, `fit_size`, `select_jpeg_scale_denom`, `apply_exif_orientation`, `downscale`, `make_image`, `approx_bytes`; `IPreviewProvider` (`id`, pure `can_handle` -> priority, reentrant `produce`; Unsupported falls through), `ProviderRegistry`.
  - **thumbnail** (new header, preview): Freedesktop size classes, GLib-compatible `file_uri`, `md5_hex`, paths, validated `read_thumbnail`/`lookup_thumbnail` (Valid/Stale/Missing/Invalid), atomic 0600 `write_thumbnail` with the spec's tEXt keys, failure markers in `fail/navigator-1/`, `decide` policy (never inside the cache; network and removable volumes read-only by default); `.thumbnailer` parsing, `split_exec`/`expand_exec` (no shell, values never re-split), `load_thumbnailers` (XDG precedence, TryExec), `choose_thumbnailer`, `ThumbnailerLimits`, `run_thumbnailer` through an injected `platform::IProcessRunner`.
  - **preview_service** (new header, preview): `build_features`, `register_builtin_providers` (ids and priorities listed in the header), `IPreviewService` (priorities Inspector > Visible > Prefetch, epochs and `cancel_older` for scrolling, coalescing tickets, `lookup`, `poll`, `invalidate` Memory/Regenerate, LRU budget, stats, `shutdown`), `create_preview_service`.
  - **platform** (platform): `thumbnail_cache_dir`, `data_dirs`, `find_program`, `ProcessRequest`/`ProcessResult`/`IProcessRunner`/`create_default_process_runner` (waits, argv only, stdin /dev/null, captured and capped output, own process group, SIGKILL on timeout or stop, rlimits, optional bubblewrap sandbox).
  - **layout** (core-fs): `InspectorPlacement`, `InspectorState {visible, placement, width, height, show_grid, show_minimap}` with constants, `resolve_inspector_placement`, `next_placement`; `Session::inspector`, JSON `"inspector"` (additive, version stays 1, written only when not default, lenient). Lock and zoom are runtime only.
  - **ui** (`ui.hpp`): `AppConfig::thumbnail_dir`, `preview_cache_bytes`, `enable_thumbnails`, `enable_external_thumbnailers`; headless runs without `thumbnail_dir`/`cache_dir` never touch `~/.cache/thumbnails`.
  - **commands** (commands): ids 1320-1327 and names under "Commands"; range 1320-1339. Default chords `Space` and `Alt+KeypadEnter` for `view.inspect` (`docs/keymap.md` "Phase 6 defaults"); text fields consume `Space`; Inspector focus routing documented there and in `commands.hpp`.
  - **features.md**: Navigator-extra rows for video (poster, metadata, Play hand-off).
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` (GCC 13) in `src/commands/builtin_commands.cpp` (`to_string`, line 86), `src/ui/browser/browser_model.cpp` (line 961) and `src/ui/workspace/workspace.cpp` (line 275): 8 new enumerators each. Everything else in the tree compiles unchanged (GCC 13 and Clang 18, header check in C++20 and C++23). Runtime tests that need updates by their owners once commands binds the rows: `tests/commands/keymap_test.cpp` (`read_keymap_doc` must also read `## Phase 6 defaults`, `phase_rows` grows to index 6; counts 3 + 25 + 21 + 5 + 4 + 2; the expected `(unbound)` sets gain the seven `inspector.*` names; `Space` and `Alt+KeypadEnter` leave any "free chord" lists) and `tests/integration/keymap_e2e_test.cpp` (`per_phase.size()` 5 -> 6 with `per_phase.at(6) == 2`, `bound.size()` 58 -> 60, the `(unbound)` set and its size 14 -> 21, per-command effect checks for `view.inspect`). `tests/commands/keymap_test.cpp` keeps passing until commands binds the rows (its parser ignores unknown sections), but `tests/integration/keymap_e2e_test.cpp` parses every `## Phase N defaults` table generically, so it fails as soon as this doc lands (2 documented rows the keymap does not bind yet, 7 new `(unbound)` rows) until commands and qa land. `tests/core/layout_json_test.cpp` golden text is unaffected (`"inspector"` is only written when not default).
- Phase 6 follow-up: process runner probes bwrap once and falls back unsandboxed when user namespaces are unavailable (reported via `sandboxed`); mime table gains 3gp, mts/m2ts, mpg/mpeg, ogv, wmv, flv.
- Phase 7 (contracts): file operations, trash, undo, drag and drop. `ops.hpp` turned from a stub into a contract in namespace `navigator::ops` (no caller existed, so the breaking change is free: the old `navigator::OpKind` / `ConflictPolicy` / `OpRequest` / `OpProgress` / `OpId` are replaced; `OpKind::Delete` is now `DeletePermanently`, `ConflictPolicy::Overwrite` is now `Replace`, `OpId` is a strong id). Two new headers, `trash.hpp` and `transfer.hpp`. No signature of an implemented function changed.
  - **types**: `ErrorCode::NoSpace = 10`, `ReadOnlyFilesystem = 11`, `CrossDevice = 12`, `NotEmpty = 13` with `to_string` names (`no_space`, `read_only_filesystem`, `cross_device`, `not_empty`). Earlier producers never return them; every existing `switch` over `ErrorCode` in `src/` has a `default`, so nothing breaks.
  - **ops** (ops, new `src/ops`, library `navigator_ops`): normative safety rules S1..S16 (no silent removal, delete = trash with no permanent fallback, permanent delete only with `confirmed`, never follow symlinks, one filesystem per tree, temp + `RENAME_NOREPLACE` copies, verified cross-fs moves, overwrite only after a decision with the replaced item trashed, cancellation without partial files, metadata/xattr/special-file rules, undo that never deletes or overwrites and verifies identity, protected sources, space check, fixture-only tests). Blocking `run_operation` for tests and workers, `IOperationQueue` (poll-based events, conflict handshake through `resolve`, pause/resume/cancel, overlapping path sets serialized, `max_concurrent` 2), `UndoStack` (UI thread, depth 100, runtime only; origin-driven redo handling), `make_label`, pure helpers `split_extension` / `stem_length` / `keep_both_name` (" (n)", compound `.tar.*`, dot-files, NAME_MAX) / `validate_new_name` / `is_same_or_inside` / `protection_of`. `EngineOptions::fault_injector` is the test seam for partial failures.
  - **trash** (new header, platform, `src/platform`): FreeDesktop Trash 1.0 rules T1..T7 (same-filesystem rename only, cross-filesystem trashing refused, `.Trash/$uid` only with sticky bit and no symlink, `.Trash-$uid` otherwise, GIO path escaping, local DeletionDate, `O_EXCL` name reservation, no `directorysizes`), `ITrash` (`trash_dir_for`, `trash`, `trash_dirs`, `list`, `restore` with `RENAME_NOREPLACE`, `forget`, lexical `is_inside_trash`), `create_trash(TrashOptions)` with injectable home trash, uid, mount table and clock. Recursive deletion stays in `src/ops`.
  - **transfer** (new header, platform, `src/platform`, pure): GLib-identical `file_uri` (same output as `thumbnail::file_uri`; qa asserts it), `path_from_file_uri`, RFC 2483 uri-list, `x-special/gnome-copied-files`, Nautilus text form, KDE cut marker, offer/accept order, `choose_drop_action` (GNOME modifiers; external drops copy unless Shift).
  - **platform**: `move_to_trash` is now a documented wrapper over `trash::create_trash` + `ITrash::trash` (was a stub returning Unsupported); `open_terminal` stays a stub (Phase 10). `tests/platform/desktop_test.cpp:344` expects Unsupported from `move_to_trash("/nonexistent")` and must change to NotFound when platform implements it.
  - **places**: comment only: `PlaceKind::Trash` is now the sidebar row for `trash.open` (`build_places` still does not emit it).
  - **commands** (commands): ids 1281-1285, 1340-1349, 1360-1362 and names under "Commands"; new ranges 1340-1359 and 1360-1369; `PasteArg::{None, IntoCursorFolder}`; 12 default chords (`docs/keymap.md` "Phase 7 defaults": `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+Z`, `Ctrl+Shift+Z`, `Ctrl+Y`, `Delete`, `Shift+Delete`, `F2`, `Ctrl+Shift+N`, `F7`, `Shift+F7`; none conflicts with the 60 earlier chords); text fields additionally consume `Shift+Delete`, `Ctrl+Shift+Z`, `Ctrl+Y`; file commands are not routed to the pane while the sidebar or a popup has focus.
  - **features.md**: Navigator-extra rows for undo/redo, Trash view, conflict sheet, background progress, clipboard interop.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` (GCC 13) in `src/commands/builtin_commands.cpp` (`to_string`, line 96), `src/ui/browser/browser_model.cpp` (line 961) and `src/ui/workspace/workspace.cpp` (line 276). `tests/commands/keymap_test.cpp` (`read_keymap_doc` must learn "## Phase 7 defaults", and the expected row counts gain `phase_rows[7] == 12`) and `tests/integration/keymap_e2e_test.cpp` (Phase 7 rows) need updates by commands / qa when the chords are bound. Root `CMakeLists.txt` needs `navigator_add_subdir(src/ops)` and `navigator_add_subdir(tests/ops)` (integration).
- Phase 7 integration: `AppConfig::trash_dir` (`--trash-dir`) added; headless `--frames` runs without it have no trash and refuse trashing/restoring/emptying (Unsupported). In the Trash view Enter restores; in the conflict sheet Escape cancels the operation; new folders are created first, then renamed inline. "Restore to…" is deferred (needs a destination field on `OpRequest`).
- Phase 8 (contracts): command palette, remappable keymap with key sequences, aliases and numpad, searchable context menu, Open With. Additions only; the single-chord `Keymap` API is source compatible (the new `bind(const KeySequence&, ...)` overload is selected only for sequences; `KeySequence` converts implicitly from `KeyChord`), `CommandInfo{id, name, title_key}` aggregate initialisers keep compiling. Four new headers: `keymap_file.hpp`, `palette.hpp`, `menu.hpp`, `openwith.hpp`; `tests/contracts` compiles them alone (C++20 and C++23) and `phase8_usage.cpp`.
  - **commands** (commands, `src/commands`): `Key::Menu = 116` and `Key::Keypad0..9 = 160..169`, `KeypadPlus/Minus/Multiply/Divide/Period = 170..174` with parse synonyms (`Numpad5`, `ContextMenu`, `Esc`, ...); F13..F24 were already present. `KeySequence` (1..4 chords), `to_string` / `parse_key_sequence`, `validate_sequence` (rules V1-V3), `is_text_input_chord`, `SequenceBinding`, `ConflictKind` / `BindingConflict` / `find_conflicts`. `Keymap` is now prefix-free: `bind(KeyChord)` can also fail with `AlreadyExists` when the chord is a strict prefix of a sequence; new `bind(const KeySequence&)`, `unbind(const KeySequence&)`, `find`, `lookup`, `sequences_for`, `sequence_bindings`, `all_bindings`; `bindings()` and `chords_for` still return single chords only. `SequenceMatcher` / `SequenceResult` (NoMatch, Pending, Match, Cancelled, Aborted; timeout default 1000 ms, range 200..5000, strictly-greater-than rule, `expire`, `deadline`, `reset`; no reference to the keymap), `to_command`. Command metadata: `CommandCategory`, `ArgKind`, `Danger`, `ContextFlags` (+ operators, `has`, `has_any`), `CommandContext`, `CommandInfo` gains `category`, `description_key`, `danger`, `arg_kind`, `palette_visible`, `bindable`, `requires_all`, `hidden_if_any`; `is_applicable`, `effective_danger`, `CommandVariant`, `builtin_command_variants`. New ids `1103`, `1370-1375`, `1390`; `CursorArg::KeepSelection`. `load_default_keymap` additionally binds `Ctrl+Shift+P`, `Ctrl+,`, `Shift+F10`, `Menu`, `Ctrl+Space`, `Ctrl+Up`, `Ctrl+Down`, `Ctrl+J` and the sequence `Ctrl+K Ctrl+S` (docs/keymap.md "Phase 8 defaults").
  - **keymap_file** (new header, commands): `KeymapFile` model, JSON v1 (lenient, `LoadReport`, deterministic `to_json`, `.bad` for non-objects, `.bak` before overwriting a newer-version file), `load_keymap_file` / `save_keymap_file` (atomic, like the session), layering (`apply_keymap_file`, `build_keymap`: user wins over defaults, displaced defaults reported, earlier user entry wins over later), `effective_aliases`, editor helpers (`check_binding`, `BindingWarning`, `is_destructive_bare_binding`, `set_binding`, `remove_binding`, `reset_command`, `reset_all`, `set_alias`, `remove_alias`, `set_sequence_timeout`), limits.
  - **palette** (new header, search): `Entry`, ranking rules R0-R7 over `search::Matcher` with documented constants, `History` (MRU, counts, `palette_history.json`, lenient, atomic), `rank`, safe-default rule (destructive never first, never recorded).
  - **menu** (new header, ui): `Item` / `MenuKind` / `Target` / `build`, `arrange` (pinned first, destructive last and separated, not pinnable), `Pins` + `menu_pins.json`, `filter` (flattened, safe default, destructive last), rules M1-M7.
  - **openwith** (new header, platform): `Handler`, `Options` / `system_options`, `parse_desktop_entry`, `mime_parents`, `list_handlers`, `default_handler`, `find_handler`, `set_default_handler`, `parse_exec_tokens`, `expand_exec`, `default_terminal_prefix`, `launch_with` (through `platform::IAppLauncher`, no shell, no GIO).
  - **ui** (`ui.hpp`, comment only): `restore_session` / `save_session` also govern `keymap.json`, `palette_history.json`, `menu_pins.json`.
  - **features.md**: Navigator-extra rows for the palette, Open With and the keymap editor warnings.
  - Known breakage until the parallel agents land: exhaustive `switch` over `CommandId` fails `-Werror=switch` (GCC 13; clang the same) in `src/commands/builtin_commands.cpp` (`to_string`, line 114), `src/ui/browser/browser_model.cpp` (line 974) and `src/ui/workspace/workspace.cpp` (line 276): 8 new enumerators each (`ToggleCursorSelected`, `OpenPalette`, `OpenSettings`, `EditKeymap`, `ResetKeymap`, `ExportKeymap`, `ShowContextMenu`, `OpenWith`). Everything else in the tree compiles unchanged (GCC 13, header check in C++20 and C++23). Runtime tests that need updates by their owners once commands binds the rows: `tests/commands/keymap_test.cpp` (`read_keymap_doc` must also read `## Phase 8 defaults`, `phase_rows` grows to index 8 with 8 rows; `arg` column accepts `KeepSelection`; counts become 3 + 25 + 21 + 5 + 4 + 2 + 12 + 8 = 80 single chords; `bindings().size()` is 80, the sequence is in `sequence_bindings()` only; the expected `(unbound)` set loses `ToggleOperationsPanel` and gains `ResetKeymap`, `ExportKeymap`, `OpenWith`, and `EditKeymap` is bound by the sequence, not a chord; `OpenPalette`, `OpenSettings`, `ShowContextMenu`, `ToggleCursorSelected` are bound), `tests/integration/keymap_e2e_test.cpp` (per-phase table 7 -> 8 with `per_phase.at(8) == 8`, `bound.size()` 72 -> 80, the unbound set, near-miss lists lose `Ctrl+Space`, `Ctrl+Shift+P`, `Ctrl+,`; it must tolerate a `### Default sequences` table, which the generic parser stops at). Root `CMakeLists.txt` needs no new subdirectory.

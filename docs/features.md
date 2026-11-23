# Navigator feature reference

Source of truth for "what File Pilot does" when Navigator work cites it. Derived from
`docs/yol_haritasi.md` sections 3–5, which hold the full source notes.

Rules:

- **R** = official File Pilot material (site, Starlog, roadmap, FAQ). **I** = independent review or
  community report (behaviour observed, not an official spec).
- Do not describe anything as a File Pilot feature unless it is listed here. Anything Navigator
  adds beyond this list goes under "Navigator extras" and is labelled as ours.
- Navigator is not affiliated with File Pilot and uses none of its name, logo, assets or binaries.
- File Pilot never published a full default keymap; only the shortcuts below are attributed to it.

Phase column = Navigator roadmap phase that delivers the behaviour.

## Performance and distribution

| Behaviour | Src | Phase |
|---|---|---|
| Very low UI latency; folders feel open before the click completes | R, I | all |
| Single small standalone executable, no background service | R | 11 |
| Optional system integration (default file manager, "open here" entries, PATH) | R, I | 11 |
| User data stored as JSON, hot-reloaded across instances, unknown fields ignored | R | 9 |

## Panes, tabs, layouts

| Behaviour | Src | Phase |
|---|---|---|
| Arbitrary pane layout, vertical and horizontal splits, each pane with its own tabs | R, I | 3 |
| Drag a tab to split left/right/top/bottom; drag splitters to resize | R, I | 3 |
| More than two panes | I | 3 |
| New / close tab, middle-click close, middle-click on new-tab button | R, I | 3 |
| `Ctrl+T` new tab, `Ctrl+W` close; new tab opens the current tab's folder | I / R | 3 |
| Duplicate tab; `Ctrl+N` duplicate tab into a new window (source stays open) | R | 3 |
| `Ctrl`+click / `Ctrl`+double-click opens in new tab | R | 3 |
| Session restore of tabs and layout; saved/loaded layouts | R, I | 3 |
| Search Tabs dropdown with open and recently closed tabs | R | 3 |
| Close Left Tabs; tab-area context menu | R | 3 |
| Window position remembered | R | 3 |
| Inactive panes slightly dimmed | I | 3 |

## Search and filter

| Behaviour | Src | Phase |
|---|---|---|
| Instant filter of the current folder (distinct from search) | R, I | 5 |
| `Ctrl+E` flatten hierarchy (subfolders, whole drive) | I, R | 5 |
| Fuzzy file-name search | R | 5 |
| Filter by extension/type (types popup, toggle all, respects hidden/system) | R, I | 5 |
| No `*` wildcard syntax | I | — |
| No file-content search | (not claimed) | — |

## Inspector (preview)

| Behaviour | Src | Phase |
|---|---|---|
| Opens with `Space`; `Alt`+numpad `Enter` = Inspect | I / R | 6 |
| Text, image and folder-content preview | R, I | 6 |
| Image: EXIF orientation, fit / 1:1, grid, minimap | R, I | 6 |
| Rich metadata, lockable inspector, right or bottom placement | I | 6 |
| No PDF/SVG preview; video playback not claimed | I | — |

## Batch rename

| Behaviour | Src | Phase |
|---|---|---|
| Interactive multi-file rename with preview | R | 9 |
| Unique ID generation, use file date | R | 9 |
| Single-line paste special case | R | 9 |
| Trim leading/trailing spaces | R | 9 |

## GoTo and address bar

| Behaviour | Src | Phase |
|---|---|---|
| GoTo with common paths, recents, autocompletion | R | 4 |
| Environment variable expansion | R | 4 |
| Path case auto-correction | R | 4 |
| Breadcrumbs with icons; dropdown preview and item count | I / R | 4 |
| Compact paths / breadcrumbs toggle in address bar menu | R | 4 |
| Lock icon: folder / hierarchy / global view settings | R, I | 4 |
| Back / forward / up; bookmark button; hover empty title area for GoTo | R, I | 4 |
| Type or paste a path; `Ctrl+Enter` opens | I | 4 |

## Command palette and shortcuts

| Behaviour | Src | Phase |
|---|---|---|
| Global command palette; search actions; assign shortcuts | R | 8 |
| Numpad keys, key sequences (chain hotkeys), aliases | R | 8 |
| `Ctrl+Shift+P` opens the palette | I | 8 |
| `Ctrl+,` settings | I | 8 |
| Nearly all actions bindable; UI shows shortcuts | I | 8 |
| F13–F24 accepted | R | 8 |
| Separate shortcuts per zoom level | R | 8 |
| `Ctrl+Space` toggles cursor selection | R | 8 |
| Numpad Enter = Activate | R | 8 |
| `Ctrl+B` toggles sidebar | I | 4 |
| Most commands still fire while the folder filter has focus | R | 8 |

## Context menu

| Behaviour | Src | Phase |
|---|---|---|
| System and third-party actions, searchable menu, pin favourites | R, I | 8 |
| Third-party items in right-drag menu | R | 8 |
| Folder-background context menu | R | 8 |

## Views and list

| Behaviour | Src | Phase |
|---|---|---|
| Details view with reorderable columns (Name included), empty meta columns | R | 2, 9 |
| Icon/grid view with instant `%` slider or `Ctrl`+wheel scale | I | 9 |
| List / Columns view; Tree view | R | 9 |
| Group by date, type, size; collapsible groups | R | 9 |
| Folders first, files first or mixed | R, I | 1, 2 |
| Sort by name, size, date, count, volume/folder usage; date and size default descending | R | 1, 2 |
| Type visibility filters saved in session and layout | R | 5 |
| Selected-size total; volume and folder usage percentage with bar | R | 2, 4 |
| Label for empty / hidden-only folders | R | 2 |
| Alternating row colours (default on) | R | 2 |
| Scroll beyond last item (default off) | R | 2 |
| Rectangle selection; checkbox drag multi-select | R | 2 |
| No selection on first entry into a folder; scroll position preserved | R | 2 |
| Drive usage bar under Places | R | 4 |

## Sidebar, bookmarks, recents

| Behaviour | Src | Phase |
|---|---|---|
| Sections: Recents, Bookmarks, Places, Storage; Quick Access | I, R | 4 |
| Volume usage bar / percentage | R, I | 4 |
| Collapsible sections; offline bookmarks and recents resync when the volume returns | R | 4 |
| Recents highlighting, "Sort Recents On Top", tracking can be disabled | R | 4 |
| Link overlay icons; updates on delete/rename | R | 4 |

## File operations and drag and drop

| Behaviour | Src | Phase |
|---|---|---|
| Copy, cut, paste, delete, rename, new folder (`Ctrl+Shift+N`) | I | 7 |
| Copy as Path (current folder when nothing selected, context-aware quoting) | R | 7 |
| Copy/move to previous pane shortcut | R | 7 |
| Drag and drop from external apps, out of ZIP, onto executables/scripts | R | 7, 10 |
| Wheel-scroll while dragging; edge auto-scroll | R | 7 |
| Open console here; open link target folder | R | 10 |
| No undo/redo in File Pilot today (planned for its 1.0) | R | — |

## Watching, volumes, network

| Behaviour | Src | Phase |
|---|---|---|
| Directory watching (drive-wide in File Pilot) | R | 1 |
| Volume eject / unplug / replug handling | R | 1 |
| Network shares; folder-size calculation setting for network shares | R | 10 |

## Thumbnails, icons, cache

| Behaviour | Src | Phase |
|---|---|---|
| Icon / thumbnail / preview cache with configurable size | R | 6 |
| Thumbnail load timing options; fit / fill; refresh forces re-cache | R | 6 |
| Overlay icons | R | 10 |
| Folder image prefetch for the inspector | R | 6 |
| Thumbnail fade disabled when animations are off | R | 6 |

## Customisation, theme, settings

| Behaviour | Src | Phase |
|---|---|---|
| Font size and spacing; one-switch animation disable; colour schemes | R | 9 |
| Custom colour scheme as JSON | R | 9 |
| Light / dark (default and auto-switch unverified) | I | 0, 9 |
| Panel transparency | I | 9 |
| Rounded corners; square window buttons option | I, R | 9 |
| Tooltip toggle and delay | R | 9 |
| Hidden / system / dot files; show extensions | R | 2 |
| Single-click open (folders only option), underline toggle | R | 9 |
| Double-click empty background goes to parent | R | 2 |
| Start location: home, root, last session; new-tab behaviour | I | 3 |
| Searchable settings panel (`Ctrl+,`) | I | 9 |

## Not claimed for File Pilot

Never describe these as File Pilot features; if Navigator builds them they are Navigator extras:
content search, regex or wildcard language, PDF/SVG/video inspector, built-in undo/redo,
plugin marketplace, a full default keymap, Peek/QuickLook integration, replacing system file
dialogs, opening external folders as tabs in the running window, touchpad gestures, official
benchmark tables.

## Navigator extras (beyond File Pilot)

Wayland first with X11 fallback, fractional scaling and IME; full Unicode via HarfBuzz (CJK, NFC/NFD);
GVfs (MTP, SMB, SFTP, FUSE); XDG Trash and undo/redo; xdg-open, MIME and default-file-manager
registration; FreeDesktop thumbnail cache; inotify and udisks2 volume events; git status overlays;
open terminal here; archive browse/create (tar, 7z, zip); persistent name index and content search;
single instance with IPC (external opens become tabs); plugin C ABI; PDF/SVG/media previews; Miller
columns; CLI (`navigator PATH`, `--new-tab`, `--select`); English and Turkish UI; .deb, AppImage and
Flatpak; checksum and permissions/xattr panel; AT-SPI accessibility (late phase).

Video in the Inspector and thumbnails is entirely Navigator's own (File Pilot does not claim
video playback, see "Inspector (preview)"):

| Navigator extra | Phase |
|---|---|
| Video poster frame as list thumbnail and in the Inspector (FFmpeg libraries when built with them, otherwise the system's Freedesktop `.thumbnailer` entries such as ffmpegthumbnailer, sandboxed and time-limited) | 6 |
| Video metadata in the Inspector: duration, resolution, frame rate, video and audio codecs, audio channels, container (FFmpeg builds) | 6 |
| "Play" button on the video poster opens the default player (`xdg-open`); no in-app playback | 6 |
| Page/first-page preview for other types (PDF, office documents, e-books, TIFF, RAW, …) through the system's Freedesktop `.thumbnailer` entries, shown as a "page preview" without image metadata; built-in PDF rendering remains future work | 6 |

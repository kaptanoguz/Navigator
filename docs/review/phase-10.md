# Design Review: Phase 10 (Linux Extras)

Reviewer: design-reviewer. Scope: Miller Columns v1, Content Search overlay, Git status & plugin
overlay badges, archive browsing, Open Terminal Here, single-instance IPC, GVfs mounts, EN/TR
wording, and UI responsiveness. Compared against `docs/yol_haritasi.md` (Faz 10),
`docs/architecture.md`, and `docs/keymap.md`.

## Verdict

**Ship Phase 10.** Every Phase 10 deliverable lands behind clean, platform-agnostic public
contracts (`include/navigator/*.hpp`, `include/navigator/plugin.h`) with optional build gates for
`gio-2.0` (`NAVIGATOR_HAVE_GVFS`) and `libarchive` (`NAVIGATOR_HAVE_LIBARCHIVE`). All heavy I/O
(archive listing, GVfs enumeration, Git status, content search) runs off the UI thread and honours
cancellation.

## BLOCKING

None.

## Review Notes
- **Miller Columns v1 (`Ctrl+5`):** Three-pane layout (clickable ancestor chain, virtualized active
  column with folder chevrons, right-hand metadata & plugin preview card) integrates into the
  existing footer mode bar and `ViewSettingsStore` persistence (`"columns"`).
- **Content Search (`Ctrl+Shift+G`):** Centered modal overlay with live file/match counter,
  case-sensitive toggle, inline Stop button during active scans, and Enter/click jump straight to
  the matching file's parent directory with the file selected.
- **Git & Plugin Badges:** Subtle tinted pills (`M`, `A`, `?`, `D`, `R`, `C`, `!`, plus plugin
  custom RGBA badges) render inline after the filename without shifting columns; active Git branch
  appears in the status bar (`git: <branch>`).
- **Archives:** Opening `.zip`/`.tar*`/`.7z` navigates smoothly into `<archive>::` via
  `ListingWorker`; activating an inner file extracts to a temp location and opens with the default
  application; Zip Slip traversal entries are rejected.
- **Single-Instance IPC & CLI:** `--new-tab`, `--select`, `--new-instance`, `--ipc-socket`, and
  `--help` work cleanly via a non-blocking Unix domain socket.
- **i18n:** All new commands, notices, and overlay labels ship in both English and Turkish.

## OPTIONAL (follow-up polish, non-blocking)
1. **Sidebar GVfs mount rows:** Render live `gvfs_mounts` directly as a collapsible "Network &
   Devices" section inside `SidebarModel` alongside local volumes with per-row eject buttons.
2. **Drag-out from archives:** When starting an outgoing drag from a virtual archive path, extract
   the dragged entries to a temporary staging directory before populating the `text/uri-list` offer.
3. **Plugin commands in Command Palette:** Merge `plugin_manager.commands()` directly into the
   Command Palette entry list alongside built-in commands.

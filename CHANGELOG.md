# Changelog

## [0.1.0] - 2026-11-24

Initial public `.deb` release for Linux (Ubuntu 24.04 / Linux Mint 22+, Wayland & X11).

### Highlights
- **Core & Platform**: Direct `getdents64` directory listing, `inotify` watching, `/proc/self/mountinfo` volume monitor, XDG user-dirs & Trash, `bwrap`-sandboxed external thumbnailers, optional `gio-2.0` GVfs (MTP/SMB/SFTP) backend, Unix socket single-instance IPC, `open_terminal` (`F4`).
- **Workspace & Views**: Drag-to-split panes, per-pane tabs, layout & session persistence, Details / Icons / List / Tree / Miller Columns (`Ctrl+1`–`Ctrl+5`), live zoom axis, grouping, column reordering, hierarchical View Lock.
- **Search & Previews**: Instant filter, recursive flatten (`Ctrl+E`), persistent volume name index, cancellable content search (`Ctrl+Shift+G`), docked Inspector (`Space`), Freedesktop thumbnails.
- **Operations & Editing**: Copy/Move/Trash/Delete/Undo/Redo engine, interactive Batch Rename (`Shift+F2`) with cyclic swap staging, `libarchive` virtual archive browsing & extraction.
- **Customization & Extensibility**: Command Palette (`Ctrl+Shift+P`), searchable context menu, remappable keymap & sequences, custom JSON themes, EN/TR UI translations, C11/C++ C ABI plugin loader (`include/navigator/plugin.h`).
- **Packaging**: CPack `.deb` package with `navigator.desktop` (`inode/directory`), scalable SVG icon, AppStream metainfo, and tag-triggered release CI.

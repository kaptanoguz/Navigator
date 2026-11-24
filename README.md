# Navigator

**Navigator** is an independent, fast, keyboard-first file manager for Linux written in C++20/23 with SDL3, FreeType, and HarfBuzz.

> Navigator is an independent Linux project inspired by File Pilot’s documented UX. It contains no third-party proprietary assets, branding, or binaries.

## Features

- **Workspace & Views**: Split panes, independent tabs per pane, saved layouts, session restore, virtualized **Details**, **Icons**, **List**, **Tree**, and **Miller Columns** views (`Ctrl+1`–`Ctrl+5`), live zoom axis (`Ctrl+=`/`Ctrl+-`/`Ctrl+wheel`), grouping, and hierarchical per-folder View Lock.
- **Fast Search & Filter**: Instant type-to-filter, fuzzy/substring matching, recursive flattened view (`Ctrl+E`), persistent per-volume name index, and cancellable content search (`Ctrl+Shift+G`).
- **Inspector & Previews**: Docked image/video/text/folder Inspector (`Space`), EXIF metadata, Freedesktop thumbnail cache interop, and bubblewrap-sandboxed external thumbnailers.
- **File Operations & Trash**: Off-thread copy/move/delete/rename with conflict resolution, XDG Trash (`~/.local/share/Trash`), multi-step Undo/Redo, and interactive Batch Rename (`Shift+F2`) with cyclic swap staging.
- **Customization**: Command Palette (`Ctrl+Shift+P`), searchable context menu, remappable keymap (`keymap.json`), built-in Dark/Light & custom JSON themes, font/spacing scaling, and EN/TR translations.
- **Linux Extras**: Optional GVfs/MTP/SMB/SFTP mounts (`gio-2.0`), cached Git status badges, Open Terminal Here (`F4`), virtual archive browsing/extraction (`libarchive`), single-instance IPC (`--new-tab`, `--select`), and C11/C++ C ABI plugins (`include/navigator/plugin.h`).

## Installing (`.deb` for Ubuntu 24.04 / Linux Mint 22+)

Download `navigator_0.1.0_amd64.deb` and `SHA256SUMS.txt` from the [Releases](https://github.com/kaptanoguz/Navigator/releases) page:

```bash
sha256sum -c SHA256SUMS.txt
sudo apt install ./navigator_0.1.0_amd64.deb
```

Or build the `.deb` locally:

```bash
./packaging/deb/build-deb.sh
sudo apt install ./dist/navigator_0.1.0_amd64.deb
```

## Setting Navigator as Default File Manager

To open folders (`inode/directory`) in Navigator by default on GNOME / Cinnamon / XDG desktops:

```bash
xdg-mime default navigator.desktop inode/directory
```

Or run the included interactive helper:

```bash
/usr/share/doc/navigator/set-default-file-manager.sh
```

To verify or revert (e.g., back to `nemo.desktop` or `org.gnome.Nautilus.desktop`):

```bash
xdg-mime query default inode/directory
xdg-mime default nemo.desktop inode/directory
```

## Building from Source

See [docs/building.md](docs/building.md) for full dependency lists and presets:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

## Documentation

- [User Manual](docs/manual.md)
- [Keymap Reference](docs/keymap.md)
- [Architecture](docs/architecture.md)
- [Changelog](CHANGELOG.md)

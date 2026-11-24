# Navigator User Manual

## 1. Quick Start
- Launch `navigator` from your application menu or terminal (`navigator [PATH]`).
- Open a new tab in an existing window: `navigator --new-tab ~/Downloads`.
- Reveal and select a file: `navigator --select ~/Documents/report.pdf`.

## 2. Navigation & Views
- **Go To** (`Ctrl+G`) / **Address Bar** (`Ctrl+L`): Jump to any path, `~` home, or GVfs URI (`smb://`, `sftp://`, `mtp://`).
- **View Modes**: `Ctrl+1` Details, `Ctrl+2` Icons, `Ctrl+3` List, `Ctrl+4` Tree, `Ctrl+5` Miller Columns.
- **Zoom**: `Ctrl+=` / `Ctrl+-` / `Ctrl+0` or `Ctrl+wheel` smoothly transitions between Details and Icons.
- **View Lock**: Click the lock icon in the address bar to set Global, Folder, or Hierarchy view rules.

## 3. Search & Filter
- Type any character in the file list to start filtering immediately (`Ctrl+F` to focus, `Esc` to clear).
- **Flatten** (`Ctrl+E`) / **Whole Volume** (`Ctrl+Shift+E`): Browse all nested files using the background name index.
- **Content Search** (`Ctrl+Shift+G`): Search text inside files; skips binary files and supports instant cancellation (`Esc`).

## 4. File Operations & Archives
- `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Delete` (XDG Trash), `Shift+Delete` (permanent delete with confirmation), `F2` (rename), `Shift+F2` (Batch Rename), `Ctrl+Z` / `Ctrl+Shift+Z` (Undo/Redo).
- **Archives**: Press `Enter` on `.zip`, `.tar*`, `.7z` archives to browse inside, or use `archive.extract_here` from the Command Palette.

## 5. Customization & Plugins
- **Command Palette** (`Ctrl+Shift+P`): Run any command or view its shortcut.
- **Settings** (`Ctrl+,`): Customize keybindings (`keymap.json`), Dark/Light or custom JSON themes (`~/.config/navigator/themes/*.json`), font scale, spacing, and animations.
- **Plugins**: Place `.so` plugins implementing `include/navigator/plugin.h` in `~/.config/navigator/plugins/`.

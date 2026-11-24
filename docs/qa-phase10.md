# QA Report & Manual Test Matrix — Phase 10 (Linux Extras)

## Automated Gates
- **Headers (`navigator_header_check`)**: C11 (`plugin.h`), C++20, and C++23 with `-Wall -Wextra -Wpedantic -Werror` — **PASS**.
- **Platform (`navigator_platform_tests [p10]`)**:
  - `open_terminal`: fallback order (`xdg-terminal-exec` → `$TERMINAL` → `x-terminal-emulator` → common Linux emulators), non-dir / relative path rejection — **PASS**.
  - `is_gvfs_uri` & `create_gvfs_backend`: scheme classification (`smb`, `sftp`, `mtp`, `davs`, etc.) and live/stub backend instantiation — **PASS**.
  - Single-instance IPC (`ipc.cpp`): Unix domain socket round-trip (`NewTab`, `SelectPath`), stale socket cleanup, `AlreadyExists` guard — **PASS**.
  - Git status (`git_status.cpp`): `git status --porcelain=v1 -z -b` parser (branch, modified/added/untracked/deleted/renamed/conflict/ignored, directory severity propagation) + off-thread `GitStatusService` caching — **PASS**.
- **Search (`navigator_search_tests [p10]`)**:
  - `ContentSearchSession` & `search_file_content`: case-sensitive / case-insensitive matching, binary NUL-byte skip, per-file size cap, `std::stop_token` cancellation — **PASS**.
- **Ops / Archives (`navigator_ops_tests [p10]`)**:
  - `archive.cpp`: virtual archive path parsing/formatting (`<archive>::<inner>`), `libarchive` virtual directory listing with implicit folder synthesis, single-entry & full extraction, Zip Slip (`../evil.txt`) protection — **PASS**.
- **Plugins (`navigator_plugin_tests`)**:
  - `PluginManager` + `plugins/sample_hello/sample_hello.c` (C11): `dlopen`/`dlsym`, ABI v1 & `struct_size` validation, duplicate ID rejection, command invocation, overlay badge, custom column, text preview, clean unload — **PASS**.
- **Commands & Keymap (`navigator_commands_tests`)**:
  - All 123 built-in commands registered, 94 default chords match `docs/keymap.md` 1:1 (`F4`, `Ctrl+5`, `Ctrl+Shift+G`), zero conflicts — **PASS**.
- **CLI & Two-Process Single-Instance Smoke**:
  - `navigator --help` documents `--new-tab`, `--select`, `--new-instance`, `--ipc-socket`, `--plugin-dir`, `--no-plugins`, `--no-git`, `--no-gvfs`.
  - Second `navigator --ipc-socket <sock> --new-tab <dir>` delivers message to primary instance and exits 0 without spawning a second window.

## Manual Verification Matrix
| Target / Scenario | Expected Result | Notes |
|---|---|---|
| **Wayland (GNOME / Mutter)** | Window opens, `F4` launches terminal in active folder, `Ctrl+5` switches to Miller Columns, `Ctrl+Shift+G` opens Content Search | Primary target |
| **X11 (Linux Mint / Cinnamon)** | Single-instance `navigator --new-tab /tmp` raises existing window and opens tab | Tested with `xvfb-run` in CI |
| **No-GVfs build (`-DNAVIGATOR_GVFS=OFF`)** | Builds and runs cleanly; `gvfs_available() == false`; local browsing unaffected | Optional dependency gate |
| **No-libarchive build (`-DNAVIGATOR_LIBARCHIVE=OFF`)** | Builds and runs cleanly; `libarchive_available() == false`; archives fall back to default app | Optional dependency gate |
| **Live MTP / SMB mount** | Mounted phone (`mtp://`) or NAS (`smb://`) lists entries off-thread without stalling UI | Requires live GVfs daemon + device/share |
| **Custom C11 Plugin** | Placing `sample_hello.so` in `~/.config/navigator/plugins/` loads on startup and `plugins.reload` | In-process v1; never called during FS writes |

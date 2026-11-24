# Release Checklist & QA Report — Phase 11 (`.deb` Packaging & Release)

## Automated Checks
- `desktop-file-validate packaging/linux/navigator.desktop`: **PASS**
- `packaging/deb/build-deb.sh` (CMake `release` preset with LTO + CPack `DEB` + `dpkg-shlibdeps`): **PASS**
- `dpkg-deb -I` / `dpkg-deb -c`: verifies `/usr/bin/navigator`, `/usr/lib/navigator/libSDL3.so.0`, `/usr/share/applications/navigator.desktop` (`MimeType=inode/directory`), `/usr/share/icons/hicolor/scalable/apps/navigator.svg`, `/usr/share/metainfo/io.github.kaptanoguz.Navigator.metainfo.xml`, `/usr/share/doc/navigator/set-default-file-manager.sh`.
- `sha256sum -c dist/SHA256SUMS.txt`: **PASS**
- `navigator --version` reports `navigator 0.1.0`.
- Full `ctest --preset dev` + `scripts/check-format.sh` + CI `format` / `build-test` / `tidy`.

## Release Verification Matrix
| Check | Target | Status / Instructions |
|---|---|---|
| `.deb` install | Ubuntu 24.04 / Linux Mint 22 | `sudo apt install ./dist/navigator_0.1.0_amd64.deb` |
| Desktop entry | GNOME / Cinnamon menu | Appears as **Navigator** with `navigator.svg` icon |
| Default file manager | `inode/directory` | User-confirmed `xdg-mime default navigator.desktop inode/directory` |
| Wayland / X11 launch | `navigator` in `$PATH` | Opens cleanly on Wayland and X11; `--new-tab` adopts into running instance |
| Trash & File Ops | XDG Trash | Moves to `~/.local/share/Trash`, undo/redo works |
| Search & Preview | Filter, Flatten, Content Search, Inspector | Off-thread, cancellable, thumbnails cached |
| Release CI | `.github/workflows/release.yml` | Triggered on `v*` tag push; attaches `.deb` + `SHA256SUMS.txt`; no secrets in repo |

# Packaging

Status: Phase 11 — `.deb` packaging active (AppImage and Flatpak deferred).

## Layout

| Path | Purpose |
|---|---|
| `linux/navigator.desktop` | Freedesktop desktop entry (`inode/directory`, installed by CMake) |
| `linux/navigator.svg` | Scalable hicolor application icon |
| `linux/io.github.kaptanoguz.Navigator.metainfo.xml` | AppStream metainfo |
| `deb/build-deb.sh` | Builds `navigator_<version>_<arch>.deb` + `SHA256SUMS.txt` into `dist/` |
| `deb/postinst`, `deb/postrm` | Updates desktop database and icon cache on install/remove |
| `deb/set-default-file-manager.sh` | Interactive helper for `xdg-mime default navigator.desktop inode/directory` |

Validate the desktop file with `desktop-file-validate packaging/linux/navigator.desktop`.

## `.deb` Format (Linux Mint 22 / Ubuntu 24.04+)

- Built with CMake `release` preset (LTO on) and CPack (`cmake/Packaging.cmake`).
- Links against distro libraries (`FreeType`, `HarfBuzz`, `Fontconfig`, `GIO/GVfs`, `libarchive`, `libpng`, `libjpeg`, `libwebp`, `libexif`, `libheif`, `FFmpeg`, `librsvg`) resolved via `dpkg-shlibdeps`.
- `libSDL3.so.0` (built via FetchContent) is installed privately under `/usr/lib/navigator/` with `$ORIGIN/../lib/navigator` `INSTALL_RPATH`.
- No signing keys or secrets are stored in this repository.

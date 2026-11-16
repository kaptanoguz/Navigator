# Packaging

Status: Phase 0 stubs. Only the desktop entry exists; package formats come in later phases.

## Layout

| Path                            | Purpose                                            |
|---------------------------------|----------------------------------------------------|
| `linux/navigator.desktop`       | Freedesktop desktop entry (installed by CMake)     |
| `linux/` (later)                | AppStream metainfo, icons (hicolor), MIME hints    |
| `deb/` (later)                  | Debian packaging                                   |
| `appimage/` (later)             | AppImage recipe                                    |
| `flatpak/` (later)              | Flatpak manifest                                   |

Validate the desktop file with `desktop-file-validate packaging/linux/navigator.desktop`.

## Planned formats

- **.deb** (Linux Mint 22 / Ubuntu 24.04 first): built with CPack or `debian/` tooling. Link
  against distro libraries (FreeType, HarfBuzz, Fontconfig, and SDL3 once the target distro ships
  it); until then SDL3 is bundled privately. Release preset, LTO on.
- **AppImage**: built from `cmake --install` into an AppDir, bundling SDL3 and anything not in the
  AppImage base set. Targets older glibc via an older build image.
- **Flatpak**: GNOME runtime; SDL3 built as a module in the manifest. Filesystem access needs
  `--filesystem=host` (a file manager), documented in the manifest; portals where possible.

All formats install the same desktop entry, AppStream metainfo, and icons. No signing keys or
other secrets live in this repository; release signing happens in CI with repository secrets
once release jobs exist.

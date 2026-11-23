# Building Navigator

Navigator uses CMake (>= 3.25) with Ninja presets. The compiler must support C++20; C++23 is used
when available (GCC 13 and Clang 18 on Ubuntu 24.04 both work).

## Prerequisites (Linux Mint 22 / Ubuntu 24.04)

```sh
sudo apt install build-essential g++-13 cmake ninja-build pkg-config git \
    libfreetype-dev libharfbuzz-dev libfontconfig-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
    libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols \
    libegl-dev libgl-dev libvulkan-dev libdecor-0-dev libdbus-1-dev libudev-dev \
    nlohmann-json3-dev \
    libpng-dev libjpeg-turbo8-dev libwebp-dev libexif-dev \
    libheif-dev libavformat-dev libavcodec-dev libswscale-dev libavutil-dev librsvg2-dev
```

The last line holds the optional preview decoders (HEIF/AVIF, video, SVG). Leave any of them out
and the matching feature is switched off automatically (see "Optional preview decoders" below).
`libjpeg-dev` works in place of `libjpeg-turbo8-dev`; only the libjpeg API is used.

The X11/Wayland/GL/Vulkan `-dev` packages are only needed when SDL3 is built from source (the
default on Ubuntu 24.04, which does not ship SDL3). Optional tooling:

```sh
sudo apt install clang-18 clang-format-18 clang-tidy-18 xvfb
```

## Build, test, run

```sh
cmake --preset dev            # configure into build/dev (Debug, -Werror)
cmake --build --preset dev    # build
ctest --preset dev            # run tests (output on failure)
./build/dev/src/app/navigator # run the app
```

Headless smoke run (what CI does on X11):

```sh
SDL_VIDEODRIVER=x11 xvfb-run -a ./build/dev/src/app/navigator --frames 120
```

## Presets

| Preset    | Build dir       | Notes                                                              |
|-----------|-----------------|--------------------------------------------------------------------|
| `dev`     | `build/dev`     | Debug, `NAVIGATOR_WERROR=ON`                                        |
| `release` | `build/release` | Release, IPO/LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`)          |
| `tidy`    | `build/tidy`    | clang/clang++, `CMAKE_CXX_CLANG_TIDY=clang-tidy-18`: build runs tidy |
| `vcpkg`   | `build/vcpkg`   | `dev` + vcpkg toolchain; only visible when `VCPKG_ROOT` is set      |

Each has matching build and test presets (`cmake --build --preset <name>`,
`ctest --preset <name>`). The format and tidy gates are also available as scripts:
`scripts/check-format.sh` and `scripts/run-tidy.sh <build-dir>` (see docs/qa-gates.md).

## Options

| Option                  | Default | Meaning                                                     |
|-------------------------|---------|-------------------------------------------------------------|
| `NAVIGATOR_BUILD_TESTS` | ON      | Build tests (needs Catch2 v3)                               |
| `NAVIGATOR_BUILD_APP`   | ON      | Build the `navigator` executable                            |
| `NAVIGATOR_WERROR`      | OFF     | `-Werror` on Navigator targets (ON in `dev`, `tidy`, CI)    |
| `NAVIGATOR_SANITIZE`    | OFF     | ASan + UBSan on Navigator targets                           |
| `NAVIGATOR_FETCH_DEPS`  | ON      | Allow FetchContent for SDL3 and Catch2 when not found       |
| `NAVIGATOR_WITH_HEIF`   | AUTO    | HEIF/HEIC/AVIF via libheif (`AUTO`, `ON`, `OFF`)            |
| `NAVIGATOR_WITH_FFMPEG` | AUTO    | Video thumbnails via FFmpeg (`AUTO`, `ON`, `OFF`)           |
| `NAVIGATOR_WITH_RSVG`   | AUTO    | SVG via librsvg-2.0 (`AUTO`, `ON`, `OFF`)                   |

### Optional preview decoders

Each `NAVIGATOR_WITH_*` option is tri-state: `AUTO` (default) enables the feature when the library
is found, `ON` makes it required (configure fails if missing), `OFF` never looks it up. Configure
prints one line per feature, e.g.:

```text
-- Navigator: HEIF   ON  (libheif 1.19.8)
-- Navigator: FFmpeg ON  (avformat 60.16.100, avcodec 60.31.102, swscale 7.5.100, avutil 58.29.100)
-- Navigator: SVG    OFF (NAVIGATOR_WITH_RSVG=OFF)
```

The outcome is exposed as the CMake variables `NAVIGATOR_HAVE_HEIF`, `NAVIGATOR_HAVE_FFMPEG` and
`NAVIGATOR_HAVE_RSVG` (`ON`/`OFF`). `src/preview` links the matching targets only when the variable
is `ON` and adds the same-named compile definition (`NAVIGATOR_HAVE_HEIF=1`, ...) to its own private
targets, so decoders compile out cleanly. FFmpeg is all-or-nothing: libavformat, libavcodec,
libswscale and libavutil must all be present.

Sanitizer build:

```sh
cmake --preset dev -DNAVIGATOR_SANITIZE=ON
cmake --build --preset dev && ctest --preset dev
```

Warnings and sanitizer flags live on the INTERFACE target `navigator::options`, which only
Navigator targets link. Third-party code (SDL3, Catch2) is compiled with its own flags and is never
subject to `-Werror` or clang-tidy.

## How dependencies are resolved

`cmake/Dependencies.cmake`:

1. **SDL3** and **Catch2 v3**: `find_package(... CONFIG QUIET)` first. This finds a vcpkg install
   (via the toolchain file) or any prefix on `CMAKE_PREFIX_PATH`. If not found and
   `NAVIGATOR_FETCH_DEPS=ON`, they are fetched from source with FetchContent at pinned tags
   (`NAVIGATOR_SDL3_TAG`, default `release-3.4.18`; `NAVIGATOR_CATCH2_TAG`, default `v3.16.0`).
   SDL3 is built as a **shared** library (`SDL_SHARED=ON`, `SDL_STATIC=OFF`, tests/examples off),
   matching how distributions package it. The first configure clones both; later configures reuse
   `build/<preset>/_deps`.
2. **FreeType**, **HarfBuzz**, **Fontconfig**: always from the system (apt) or vcpkg, never fetched.
   HarfBuzz uses its CMake package if present, otherwise an imported target is built from
   pkg-config.
3. **Preview decoders** (`cmake/PreviewDependencies.cmake`): always from the system or vcpkg,
   never fetched. libpng and libjpeg use CMake's `FindPNG` / `FindJPEG`; libwebp, libwebpdemux,
   libexif, libheif, FFmpeg and librsvg use pkg-config (with a CMake-config fallback for libwebp,
   libexif and libheif). Required: libpng, libjpeg, libwebp + libwebpdemux, libexif.

Targets available to modules: `SDL3::SDL3`, `Catch2::Catch2WithMain` (and `include(Catch)` /
`catch_discover_tests`), `Freetype::Freetype`, `harfbuzz::harfbuzz`, `Fontconfig::Fontconfig`,
`navigator::options`, `navigator::headers`, and for the preview module `PNG::PNG`, `JPEG::JPEG`,
`WebP::webp`, `WebP::webpdemux`, `Exif::Exif`, plus (only when the matching `NAVIGATOR_HAVE_*` is
`ON`) `Heif::heif`, `FFmpeg::avformat`, `FFmpeg::avcodec`, `FFmpeg::swscale`, `FFmpeg::avutil`,
`Rsvg::rsvg`. All decoder targets are `GLOBAL` imported targets.

To use a prebuilt SDL3/Catch2 (no network):

```sh
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/prefix -DNAVIGATOR_FETCH_DEPS=OFF
```

## vcpkg alternative

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
cmake --preset vcpkg && cmake --build --preset vcpkg && ctest --preset vcpkg
```

`vcpkg.json` lists `sdl3`, `catch2`, `freetype`, `harfbuzz`, `fontconfig`, `nlohmann-json`,
`libpng`, `libjpeg-turbo`, `libwebp`, `libexif`, pinned by `builtin-baseline`. The optional
decoders are manifest features, off by default:

| Feature  | Port(s)                                                 | CMake option            |
|----------|---------------------------------------------------------|-------------------------|
| `heif`   | `libheif`                                               | `NAVIGATOR_WITH_HEIF`   |
| `ffmpeg` | `ffmpeg[avcodec,avformat,swscale]` (no default features) | `NAVIGATOR_WITH_FFMPEG` |
| `svg`    | `librsvg` (legacy 2.40 C port; distro 2.5x preferred)   | `NAVIGATOR_WITH_RSVG`   |

```sh
cmake --preset vcpkg -DVCPKG_MANIFEST_FEATURES="heif;ffmpeg;svg"
```

Notes: with a feature disabled, `AUTO` may still pick the library up from the system via
pkg-config; pass `-DNAVIGATOR_WITH_<X>=OFF` to keep a vcpkg build hermetic. The default
`x64-linux` triplet is static, and pkg-config imported targets do not pull in `Libs.private`, so
static FFmpeg/librsvg can fail to link; use `-DVCPKG_TARGET_TRIPLET=x64-linux-dynamic` for those
features. The distro packages remain the primary path for `.deb` builds.

## Packaging

See `packaging/README.md`. `cmake --install build/release --prefix <dir>` installs the
`navigator` binary and `share/applications/navigator.desktop`.

# Dependency resolution for Navigator.
#
# Strategy:
#   1. find_package(<pkg> CONFIG QUIET) — picks up vcpkg (toolchain file) or any
#      CMAKE_PREFIX_PATH install.
#   2. SDL3 and Catch2 only: FetchContent from a pinned tag when
#      NAVIGATOR_FETCH_DEPS is ON.
#   3. FreeType / HarfBuzz / Fontconfig: system packages (apt) or vcpkg.
#
# Exported targets (stable names other modules rely on):
#   SDL3::SDL3, Catch2::Catch2WithMain (+ include(Catch) works),
#   Freetype::Freetype, harfbuzz::harfbuzz, Fontconfig::Fontconfig
#   Preview decoders (cmake/PreviewDependencies.cmake): PNG::PNG, JPEG::JPEG,
#   WebP::webp, WebP::webpdemux, Exif::Exif; optional Heif::heif,
#   FFmpeg::{avformat,avcodec,swscale,avutil}, Rsvg::rsvg gated by
#   NAVIGATOR_HAVE_HEIF / NAVIGATOR_HAVE_FFMPEG / NAVIGATOR_HAVE_RSVG.

include(FetchContent)

set(NAVIGATOR_SDL3_TAG "release-3.4.18" CACHE STRING "SDL3 git tag used by FetchContent")
set(NAVIGATOR_CATCH2_TAG "v3.16.0" CACHE STRING "Catch2 git tag used by FetchContent")
set(NAVIGATOR_JSON_TAG "v3.12.0" CACHE STRING "nlohmann/json git tag used by FetchContent")

# Do not run clang-tidy (tidy preset) over fetched third-party sources.
macro(_navigator_tidy_off)
    set(_navigator_saved_cxx_tidy "${CMAKE_CXX_CLANG_TIDY}")
    set(_navigator_saved_c_tidy "${CMAKE_C_CLANG_TIDY}")
    set(CMAKE_CXX_CLANG_TIDY "")
    set(CMAKE_C_CLANG_TIDY "")
endmacro()
macro(_navigator_tidy_restore)
    set(CMAKE_CXX_CLANG_TIDY "${_navigator_saved_cxx_tidy}")
    set(CMAKE_C_CLANG_TIDY "${_navigator_saved_c_tidy}")
endmacro()

# --------------------------------------------------------------------------
# SDL3 (shared library; matches distro SDL3 packaging and keeps link times low)
# --------------------------------------------------------------------------
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    message(STATUS "Navigator: SDL3 ${SDL3_VERSION} (find_package)")
elseif(NAVIGATOR_FETCH_DEPS)
    message(STATUS "Navigator: SDL3 via FetchContent (${NAVIGATOR_SDL3_TAG})")
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG ${NAVIGATOR_SDL3_TAG}
        GIT_SHALLOW TRUE
        SYSTEM)
    _navigator_tidy_off()
    FetchContent_MakeAvailable(SDL3)
    _navigator_tidy_restore()
else()
    message(FATAL_ERROR "SDL3 not found and NAVIGATOR_FETCH_DEPS is OFF. "
        "Install SDL3, use the vcpkg preset, or set CMAKE_PREFIX_PATH.")
endif()
if(NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 resolved but target SDL3::SDL3 is missing")
endif()

# --------------------------------------------------------------------------
# Catch2 v3 (tests only)
# --------------------------------------------------------------------------
if(NAVIGATOR_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        message(STATUS "Navigator: Catch2 ${Catch2_VERSION} (find_package)")
        # Catch2Config.cmake appends its dir to CMAKE_MODULE_PATH -> include(Catch) works.
    elseif(NAVIGATOR_FETCH_DEPS)
        message(STATUS "Navigator: Catch2 via FetchContent (${NAVIGATOR_CATCH2_TAG})")
        set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG ${NAVIGATOR_CATCH2_TAG}
            GIT_SHALLOW TRUE
            SYSTEM)
        _navigator_tidy_off()
        FetchContent_MakeAvailable(Catch2)
        _navigator_tidy_restore()
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    else()
        message(FATAL_ERROR "Catch2 v3 not found and NAVIGATOR_FETCH_DEPS is OFF.")
    endif()
    include(Catch)
endif()

# --------------------------------------------------------------------------
# nlohmann/json (header-only; layout/session/settings persistence, src/ only)
# --------------------------------------------------------------------------
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "Navigator: nlohmann_json ${nlohmann_json_VERSION} (find_package)")
elseif(NAVIGATOR_FETCH_DEPS)
    message(STATUS "Navigator: nlohmann_json via FetchContent (${NAVIGATOR_JSON_TAG})")
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG ${NAVIGATOR_JSON_TAG}
        GIT_SHALLOW TRUE
        SYSTEM)
    _navigator_tidy_off()
    FetchContent_MakeAvailable(nlohmann_json)
    _navigator_tidy_restore()
else()
    message(FATAL_ERROR "nlohmann_json not found and NAVIGATOR_FETCH_DEPS is OFF.")
endif()

# --------------------------------------------------------------------------
# FreeType, HarfBuzz, Fontconfig (system or vcpkg; never fetched)
# --------------------------------------------------------------------------
find_package(Freetype REQUIRED)   # FindFreetype / vcpkg wrapper -> Freetype::Freetype

find_package(harfbuzz CONFIG QUIET)
if(NOT TARGET harfbuzz::harfbuzz)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NAVIGATOR_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
    add_library(harfbuzz::harfbuzz INTERFACE IMPORTED)
    target_link_libraries(harfbuzz::harfbuzz INTERFACE PkgConfig::NAVIGATOR_HARFBUZZ)
    message(STATUS "Navigator: HarfBuzz ${NAVIGATOR_HARFBUZZ_VERSION} (pkg-config)")
else()
    message(STATUS "Navigator: HarfBuzz (find_package)")
endif()

find_package(Fontconfig REQUIRED) # CMake's FindFontconfig -> Fontconfig::Fontconfig

# --------------------------------------------------------------------------
# Image / video decoders for src/preview (system or vcpkg; never fetched)
# --------------------------------------------------------------------------
include(PreviewDependencies)

# --------------------------------------------------------------------------
# Optional Phase 10 dependencies: GIO/GVfs and libarchive
# --------------------------------------------------------------------------
include(Phase10Dependencies)

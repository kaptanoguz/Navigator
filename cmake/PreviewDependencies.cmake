# Image / video decoding dependencies for src/preview.
#
# Included from cmake/Dependencies.cmake (root scope). Everything here comes from
# the system (pkg-config / CMake find modules) or a vcpkg install; nothing is
# fetched from source.
#
# Required (configure fails if missing):
#   PNG::PNG            libpng            (CMake FindPNG)
#   JPEG::JPEG          libjpeg(-turbo)   (CMake FindJPEG; the libjpeg API, turbojpeg not needed)
#   WebP::webp          libwebp           (pkg-config, CMake config fallback)
#   WebP::webpdemux     libwebpdemux      (pkg-config, CMake config fallback)
#   Exif::Exif          libexif           (pkg-config, vcpkg unofficial config fallback)
#
# Optional, each a tri-state cache STRING: AUTO (default: on if found), ON
# (required: configure fails if missing), OFF (never looked up):
#   NAVIGATOR_WITH_HEIF    -> Heif::heif                                  libheif
#   NAVIGATOR_WITH_FFMPEG  -> FFmpeg::avformat FFmpeg::avcodec
#                             FFmpeg::swscale  FFmpeg::avutil             libav*
#   NAVIGATOR_WITH_RSVG    -> Rsvg::rsvg                                  librsvg-2.0
#
# Result variables (root scope, visible to every subdirectory):
#   NAVIGATOR_HAVE_HEIF, NAVIGATOR_HAVE_FFMPEG, NAVIGATOR_HAVE_RSVG  (ON/OFF)
# The targets above exist only when the matching NAVIGATOR_HAVE_* is ON.
# src/preview turns these into its own compile definitions
# (e.g. NAVIGATOR_HAVE_HEIF=1) on its private targets; nothing here adds
# global definitions.
#
# All imported targets are GLOBAL so any directory may link them.

find_package(PkgConfig REQUIRED)

# _navigator_pkg_target(<target> <prefix> <module> [REQUIRED])
# Creates INTERFACE IMPORTED GLOBAL <target> wrapping PkgConfig::<prefix>
# unless <target> already exists. Sets <prefix>_FOUND / <prefix>_VERSION in the
# caller's scope.
function(_navigator_pkg_target target prefix module)
    set(_req "")
    if("REQUIRED" IN_LIST ARGN)
        set(_req REQUIRED)
    endif()
    if(TARGET ${target})
        set(${prefix}_FOUND TRUE PARENT_SCOPE)
        return()
    endif()
    pkg_check_modules(${prefix} ${_req} QUIET IMPORTED_TARGET GLOBAL ${module})
    if(${prefix}_FOUND)
        add_library(${target} INTERFACE IMPORTED GLOBAL)
        target_link_libraries(${target} INTERFACE PkgConfig::${prefix})
    endif()
    set(${prefix}_FOUND "${${prefix}_FOUND}" PARENT_SCOPE)
    set(${prefix}_VERSION "${${prefix}_VERSION}" PARENT_SCOPE)
endfunction()

# _navigator_wrap_target(<target> <existing>): GLOBAL INTERFACE wrapper.
function(_navigator_wrap_target target existing)
    if(NOT TARGET ${target})
        add_library(${target} INTERFACE IMPORTED GLOBAL)
        target_link_libraries(${target} INTERFACE ${existing})
    endif()
endfunction()

# _navigator_feature_mode(<var> <doc>): declare the tri-state option and
# normalise its value to AUTO / ON / OFF in <var>_MODE (caller scope).
function(_navigator_feature_mode var doc)
    set(${var} "AUTO" CACHE STRING "${doc} (AUTO, ON, OFF)")
    set_property(CACHE ${var} PROPERTY STRINGS AUTO ON OFF)
    string(TOUPPER "${${var}}" _v)
    if(_v STREQUAL "AUTO" OR _v STREQUAL "")
        set(_m AUTO)
    elseif(_v MATCHES "^(ON|YES|TRUE|Y|1)$")
        set(_m ON)
    elseif(_v MATCHES "^(OFF|NO|FALSE|N|0)$")
        set(_m OFF)
    else()
        message(FATAL_ERROR "${var} must be AUTO, ON or OFF (got '${${var}}')")
    endif()
    set(${var}_MODE ${_m} PARENT_SCOPE)
endfunction()

# --------------------------------------------------------------------------
# Required decoders
# --------------------------------------------------------------------------
find_package(PNG REQUIRED)    # FindPNG / vcpkg wrapper -> PNG::PNG
message(STATUS "Navigator: libpng ${PNG_VERSION_STRING}")

find_package(JPEG REQUIRED)   # FindJPEG / vcpkg libjpeg-turbo wrapper -> JPEG::JPEG
message(STATUS "Navigator: libjpeg (API ${JPEG_VERSION})")

_navigator_pkg_target(WebP::webp NAVIGATOR_WEBP libwebp)
_navigator_pkg_target(WebP::webpdemux NAVIGATOR_WEBPDEMUX libwebpdemux)
if(NOT NAVIGATOR_WEBP_FOUND OR NOT NAVIGATOR_WEBPDEMUX_FOUND)
    find_package(WebP CONFIG QUIET)
endif()
if(NOT TARGET WebP::webp OR NOT TARGET WebP::webpdemux)
    message(FATAL_ERROR "libwebp + libwebpdemux not found (apt: libwebp-dev; vcpkg: libwebp)")
endif()
message(STATUS "Navigator: libwebp ${NAVIGATOR_WEBP_VERSION} (+ webpdemux)")

_navigator_pkg_target(Exif::Exif NAVIGATOR_EXIF libexif)
if(NOT NAVIGATOR_EXIF_FOUND)
    find_package(unofficial-libexif CONFIG QUIET)   # vcpkg libexif port
    if(TARGET unofficial::libexif::libexif)
        _navigator_wrap_target(Exif::Exif unofficial::libexif::libexif)
    endif()
endif()
if(NOT TARGET Exif::Exif)
    message(FATAL_ERROR "libexif not found (apt: libexif-dev; vcpkg: libexif)")
endif()
message(STATUS "Navigator: libexif ${NAVIGATOR_EXIF_VERSION}")

# --------------------------------------------------------------------------
# Optional: HEIF / AVIF via libheif
# --------------------------------------------------------------------------
_navigator_feature_mode(NAVIGATOR_WITH_HEIF "HEIF/HEIC/AVIF decoding via libheif")
set(NAVIGATOR_HAVE_HEIF OFF)
if(NOT NAVIGATOR_WITH_HEIF_MODE STREQUAL "OFF")
    _navigator_pkg_target(Heif::heif NAVIGATOR_HEIF libheif)
    if(NOT NAVIGATOR_HEIF_FOUND)
        find_package(libheif CONFIG QUIET)           # vcpkg / upstream config: target 'heif'
        if(TARGET heif)
            _navigator_wrap_target(Heif::heif heif)
        endif()
    endif()
    if(TARGET Heif::heif)
        set(NAVIGATOR_HAVE_HEIF ON)
    elseif(NAVIGATOR_WITH_HEIF_MODE STREQUAL "ON")
        message(FATAL_ERROR "NAVIGATOR_WITH_HEIF=ON but libheif was not found (apt: libheif-dev)")
    endif()
endif()
if(NAVIGATOR_HAVE_HEIF)
    message(STATUS "Navigator: HEIF   ON  (libheif ${NAVIGATOR_HEIF_VERSION})")
else()
    message(STATUS "Navigator: HEIF   OFF (NAVIGATOR_WITH_HEIF=${NAVIGATOR_WITH_HEIF})")
endif()

# --------------------------------------------------------------------------
# Optional: video thumbnails via FFmpeg (all four libraries or none)
# --------------------------------------------------------------------------
_navigator_feature_mode(NAVIGATOR_WITH_FFMPEG "Video thumbnails via FFmpeg (libavformat/libavcodec/libswscale/libavutil)")
set(NAVIGATOR_HAVE_FFMPEG OFF)
if(NOT NAVIGATOR_WITH_FFMPEG_MODE STREQUAL "OFF")
    pkg_check_modules(NAVIGATOR_FFMPEG QUIET IMPORTED_TARGET GLOBAL
        libavformat libavcodec libswscale libavutil)
    if(NAVIGATOR_FFMPEG_FOUND)
        set(NAVIGATOR_HAVE_FFMPEG ON)
        foreach(_lib avformat avcodec swscale avutil)
            # Per-library targets so consumers can link only what they need.
            _navigator_pkg_target(FFmpeg::${_lib} NAVIGATOR_FFMPEG_${_lib} lib${_lib})
        endforeach()
    elseif(NAVIGATOR_WITH_FFMPEG_MODE STREQUAL "ON")
        message(FATAL_ERROR "NAVIGATOR_WITH_FFMPEG=ON but FFmpeg was not found (apt: "
            "libavformat-dev libavcodec-dev libswscale-dev libavutil-dev)")
    endif()
endif()
if(NAVIGATOR_HAVE_FFMPEG)
    message(STATUS "Navigator: FFmpeg ON  (avformat ${NAVIGATOR_FFMPEG_libavformat_VERSION}, "
        "avcodec ${NAVIGATOR_FFMPEG_libavcodec_VERSION}, swscale ${NAVIGATOR_FFMPEG_libswscale_VERSION}, "
        "avutil ${NAVIGATOR_FFMPEG_libavutil_VERSION})")
else()
    message(STATUS "Navigator: FFmpeg OFF (NAVIGATOR_WITH_FFMPEG=${NAVIGATOR_WITH_FFMPEG})")
endif()

# --------------------------------------------------------------------------
# Optional: SVG via librsvg
# --------------------------------------------------------------------------
_navigator_feature_mode(NAVIGATOR_WITH_RSVG "SVG rendering via librsvg-2.0")
set(NAVIGATOR_HAVE_RSVG OFF)
if(NOT NAVIGATOR_WITH_RSVG_MODE STREQUAL "OFF")
    _navigator_pkg_target(Rsvg::rsvg NAVIGATOR_RSVG librsvg-2.0)
    if(NAVIGATOR_RSVG_FOUND)
        set(NAVIGATOR_HAVE_RSVG ON)
    elseif(NAVIGATOR_WITH_RSVG_MODE STREQUAL "ON")
        message(FATAL_ERROR "NAVIGATOR_WITH_RSVG=ON but librsvg-2.0 was not found (apt: librsvg2-dev)")
    endif()
endif()
if(NAVIGATOR_HAVE_RSVG)
    message(STATUS "Navigator: SVG    ON  (librsvg ${NAVIGATOR_RSVG_VERSION})")
else()
    message(STATUS "Navigator: SVG    OFF (NAVIGATOR_WITH_RSVG=${NAVIGATOR_WITH_RSVG})")
endif()

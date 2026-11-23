# navigator_preview (owner: preview): Phase 6 previews and thumbnails (navigator/preview.hpp,
# navigator/thumbnail.hpp, navigator/preview_service.hpp): pure helpers, the format decoders
# (JPEG, PNG, WebP, GIF, BMP in-tree; HEIF/AVIF, SVG, video when built with libheif, librsvg,
# FFmpeg), EXIF, text and folder previews, the Freedesktop thumbnail cache, system thumbnailers
# and the preview service. Links navigator_core and the decoder libraries only; never
# src/platform (the process runner, cache root and data dirs are injected).

add_library(navigator_preview STATIC
    decode_bmp.cpp
    decode_dispatch.cpp
    decode_gif.cpp
    decode_heif.cpp
    decode_jpeg.cpp
    decode_svg.cpp
    decode_video.cpp
    decode_webp.cpp
    exif.cpp
    file_util.cpp
    image_ops.cpp
    png_codec.cpp
    providers.cpp
    registry.cpp
    service.cpp
    text.cpp
    thumbnail.cpp
    thumbnailer.cpp
)
add_library(navigator::preview ALIAS navigator_preview)

find_package(Threads REQUIRED)
target_link_libraries(navigator_preview
    PUBLIC
        navigator::headers
        navigator::core
        Threads::Threads
    PRIVATE
        navigator::options
        PNG::PNG
        JPEG::JPEG
        WebP::webp
        WebP::webpdemux
        Exif::Exif
)

if(NAVIGATOR_HAVE_HEIF)
    target_link_libraries(navigator_preview PRIVATE Heif::heif)
    target_compile_definitions(navigator_preview PRIVATE NAVIGATOR_HAVE_HEIF=1)
endif()
if(NAVIGATOR_HAVE_FFMPEG)
    target_link_libraries(navigator_preview PRIVATE
        FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil)
    target_compile_definitions(navigator_preview PRIVATE NAVIGATOR_HAVE_FFMPEG=1)
endif()
if(NAVIGATOR_HAVE_RSVG)
    target_link_libraries(navigator_preview PRIVATE Rsvg::rsvg)
    target_compile_definitions(navigator_preview PRIVATE NAVIGATOR_HAVE_RSVG=1)
endif()

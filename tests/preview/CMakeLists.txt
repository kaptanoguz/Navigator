# navigator_preview tests (owner: preview). Every fixture is generated at run time in a fresh
# temp_directory_path()/navigator-preview-test-<random> directory (test_util.hpp) with the same
# decoder libraries; thumbnail tests use temporary cache roots only (never ~/.cache/thumbnails),
# and no test spawns a process (fake IProcessRunner). The hidden "[.perf]" case prints the
# timings recorded in docs/perf/phase6-preview.md.

add_executable(navigator_preview_tests
    decode_test.cpp
    fuzz_test.cpp
    helpers_test.cpp
    perf_test.cpp
    providers_test.cpp
    service_test.cpp
    test_util.cpp
    text_test.cpp
    thumbnail_test.cpp
    thumbnailer_test.cpp
    video_test.cpp
)
target_include_directories(navigator_preview_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/preview)
target_link_libraries(navigator_preview_tests
    PRIVATE
        navigator::preview
        navigator::options
        PNG::PNG
        JPEG::JPEG
        WebP::webp
        Exif::Exif
        Catch2::Catch2WithMain
)
if(NAVIGATOR_HAVE_HEIF)
    target_link_libraries(navigator_preview_tests PRIVATE Heif::heif)
    target_compile_definitions(navigator_preview_tests PRIVATE NAVIGATOR_HAVE_HEIF=1)
endif()
if(NAVIGATOR_HAVE_FFMPEG)
    target_link_libraries(navigator_preview_tests PRIVATE
        FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
    target_compile_definitions(navigator_preview_tests PRIVATE NAVIGATOR_HAVE_FFMPEG=1)
endif()
if(NAVIGATOR_HAVE_RSVG)
    target_compile_definitions(navigator_preview_tests PRIVATE NAVIGATOR_HAVE_RSVG=1)
endif()

include(Catch)
catch_discover_tests(navigator_preview_tests
    TEST_PREFIX "preview."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp"
)

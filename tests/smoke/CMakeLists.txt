# Navigator Phase 0 smoke tests (owner: qa).
# Expects from the root CMake: Catch2::Catch2WithMain, navigator_headers, navigator_options.
# Must be added AFTER src/app so that `if(TARGET navigator)` below sees the app target.

add_executable(navigator_smoke_tests
    sanity_test.cpp
    result_test.cpp
)
target_link_libraries(navigator_smoke_tests
    PRIVATE
        navigator_headers
        navigator_options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(navigator_smoke_tests
    TEST_PREFIX "smoke."
    DISCOVERY_MODE PRE_TEST
)

# Optional headless app run: opens the window on SDL's dummy driver and exits after 60 frames.
if(TARGET navigator)
    add_test(NAME smoke.app_headless
        COMMAND $<TARGET_FILE:navigator> --frames 60
    )
    set_tests_properties(smoke.app_headless PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software"
        TIMEOUT 30
        LABELS "smoke;app"
    )
endif()

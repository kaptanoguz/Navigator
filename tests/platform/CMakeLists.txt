# navigator_platform_tests (owner: platform). Temporary fixture directories only; no root, no
# hardware. Internal seams (src/platform/*.hpp) are reachable through the include path below.
add_executable(navigator_platform_tests
    mountinfo_test.cpp
    proc_source_test.cpp
    volume_monitor_test.cpp
    volume_test.cpp
    watch_test.cpp
)
target_include_directories(navigator_platform_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/platform)
target_link_libraries(navigator_platform_tests
    PRIVATE
        navigator::platform
        navigator::options
        Catch2::Catch2WithMain
)
catch_discover_tests(navigator_platform_tests
    TEST_PREFIX "platform."
    DISCOVERY_MODE PRE_TEST
)

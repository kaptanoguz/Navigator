# navigator_platform_tests (owner: platform). Temporary fixture directories only; no root, no
# hardware. Internal seams (src/platform/*.hpp) are reachable through the include path below.
# Helper started by launcher_test through the real launcher; reports what it inherited.
add_executable(navigator_launch_probe launch_probe.cpp)
target_link_libraries(navigator_launch_probe PRIVATE navigator::options)

add_executable(navigator_platform_tests
    desktop_test.cpp
    launcher_test.cpp
    mime_test.cpp
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
target_compile_definitions(navigator_platform_tests
    PRIVATE NAVIGATOR_LAUNCH_PROBE="$<TARGET_FILE:navigator_launch_probe>")
add_dependencies(navigator_platform_tests navigator_launch_probe)
catch_discover_tests(navigator_platform_tests
    TEST_PREFIX "platform."
    DISCOVERY_MODE PRE_TEST
)

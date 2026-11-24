add_executable(navigator_plugin_tests
    plugin_manager_test.cpp
)
target_link_libraries(navigator_plugin_tests
    PRIVATE
        navigator::plugins
        navigator::options
        Catch2::Catch2WithMain
)
target_compile_definitions(navigator_plugin_tests
    PRIVATE NAVIGATOR_SAMPLE_PLUGIN_PATH="$<TARGET_FILE:navigator_sample_hello>"
)
add_dependencies(navigator_plugin_tests navigator_sample_hello)

include(Catch)
catch_discover_tests(navigator_plugin_tests
    TEST_PREFIX "plugins."
    DISCOVERY_MODE PRE_TEST
)

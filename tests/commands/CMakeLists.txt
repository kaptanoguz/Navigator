# navigator_commands tests (owner: commands).

add_executable(navigator_commands_tests
    command_queue_test.cpp
    command_registry_test.cpp
)
target_link_libraries(navigator_commands_tests
    PRIVATE
        navigator::commands
        navigator::options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(navigator_commands_tests
    TEST_PREFIX "commands."
    DISCOVERY_MODE PRE_TEST
)

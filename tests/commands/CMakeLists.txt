# navigator_commands tests (owner: commands).

add_executable(navigator_commands_tests
    command_queue_test.cpp
    command_registry_test.cpp
    keymap_test.cpp
    keymap_file_test.cpp
)
target_link_libraries(navigator_commands_tests
    PRIVATE
        navigator::commands
        navigator::options
        Catch2::Catch2WithMain
)
# keymap_test.cpp checks load_default_keymap() against the tables in docs/keymap.md.
target_compile_definitions(navigator_commands_tests
    PRIVATE NAVIGATOR_KEYMAP_DOC="${PROJECT_SOURCE_DIR}/docs/keymap.md"
)

include(Catch)
catch_discover_tests(navigator_commands_tests
    TEST_PREFIX "commands."
    DISCOVERY_MODE PRE_TEST
)

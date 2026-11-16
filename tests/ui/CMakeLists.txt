# navigator_ui_tests (owner: ui): pure logic of src/ui and src/app, no window required.
add_executable(navigator_ui_tests
    cli_test.cpp
    i18n_test.cpp
    keybindings_test.cpp
    text_shaping_test.cpp
    theme_test.cpp
)
target_link_libraries(navigator_ui_tests
    PRIVATE
        navigator::ui
        navigator_app_cli
        navigator::commands
        navigator::options
        SDL3::SDL3  # internal ui headers (e.g. shell/background.hpp) include SDL
        Catch2::Catch2WithMain
)
catch_discover_tests(navigator_ui_tests
    TEST_PREFIX "ui."
    DISCOVERY_MODE PRE_TEST
)

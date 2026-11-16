# navigator_ui_tests (owner: ui): pure logic of src/ui and src/app, no window required.
# Browser tests create files only inside mkdtemp fixture directories (tests/ui/ui_fixture.hpp).
add_executable(navigator_ui_tests
    backend_name_test.cpp
    browser_model_test.cpp
    cli_test.cpp
    format_test.cpp
    i18n_test.cpp
    lru_cache_test.cpp
    sdl_input_test.cpp
    text_fit_test.cpp
    text_shaping_test.cpp
    theme_test.cpp
)
target_link_libraries(navigator_ui_tests
    PRIVATE
        navigator::ui
        navigator_app_cli
        navigator::commands
        navigator::options
        SDL3::SDL3  # internal ui headers (input, render, shell) include SDL
        Catch2::Catch2WithMain
)
catch_discover_tests(navigator_ui_tests
    TEST_PREFIX "ui."
    DISCOVERY_MODE PRE_TEST
)

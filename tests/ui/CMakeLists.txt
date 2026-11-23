# navigator_ui_tests (owner: ui): pure logic of src/ui and src/app, no window required.
# Browser tests create files only inside mkdtemp fixture directories (tests/ui/ui_fixture.hpp).
add_executable(navigator_ui_tests
    backend_name_test.cpp
    browser_model_test.cpp
    cli_test.cpp
    filter_test.cpp
    flatten_test.cpp
    format_test.cpp
    i18n_test.cpp
    index_ui_test.cpp
    inspector_logic_test.cpp
    inspector_math_test.cpp
    lru_cache_test.cpp
    path_entry_test.cpp
    sdl_input_test.cpp
    sidebar_test.cpp
    text_fit_test.cpp
    text_shaping_test.cpp
    theme_test.cpp
    workspace_logic_test.cpp
    workspace_test.cpp
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

# Phase 3 acceptance: a 3-pane layout is saved on exit and restored by the next run (headless,
# temporary --config-dir, never the user's ~/.config).
if(TARGET navigator)
    add_test(NAME ui.smoke_session_restores_three_panes
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/session_smoke.sh $<TARGET_FILE:navigator>)
    set_tests_properties(ui.smoke_session_restores_three_panes PROPERTIES
        TIMEOUT 60 LABELS "smoke;app")
    # Phase 4 acceptance: GoTo jumps to ~/Downloads and / (temporary HOME and --config-dir).
    add_test(NAME ui.smoke_goto_downloads_and_root
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/goto_smoke.sh $<TARGET_FILE:navigator>)
    set_tests_properties(ui.smoke_goto_downloads_and_root PROPERTIES
        TIMEOUT 90 LABELS "smoke;app")
    # Phase 5 acceptance: flatten of a generated 300k-entry tree streams, cancels within a
    # bound and keeps rendering (temporary HOME, config and cache; no index). Frame times are
    # measured, so it runs on its own.
    add_test(NAME ui.smoke_flatten_cancels_without_freezing
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/flatten_smoke.sh $<TARGET_FILE:navigator>)
    set_tests_properties(ui.smoke_flatten_cancels_without_freezing PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE LABELS "smoke;app")
    # Phase 6 acceptance: the Inspector on PNG / JPEG / WebP / text / folder, a corrupt 100 MP
    # PNG, a huge single-line text file within a frame-time bound, and no writes outside the
    # temporary --thumbnail-dir (tiny committed fixtures in fixtures/preview). Needs src/preview.
    if(TARGET navigator_preview)
        add_test(NAME ui.smoke_inspector_previews
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/inspector_smoke.sh $<TARGET_FILE:navigator>
                    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/preview)
        set_tests_properties(ui.smoke_inspector_previews PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE LABELS "smoke;app")
    endif()
endif()

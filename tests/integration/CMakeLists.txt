# navigator_integration_tests (owner: qa): cross-module race tests for navigator::core and
# navigator::platform against the real kernel (inotify, /proc/self/mountinfo). Temporary fixture
# directories only; no root, no hardware. Runtime target: < 30 s for the whole suite.
add_executable(navigator_integration_tests
    listing_race_test.cpp
    volume_race_test.cpp
    watch_race_test.cpp
)
target_link_libraries(navigator_integration_tests
    PRIVATE
        navigator::core
        navigator::platform
        navigator::options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(navigator_integration_tests
    TEST_PREFIX "integration."
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60 LABELS integration
)

# Phase 2: BrowserModel (headless, navigator_ui_browser: no SDL, no window) under concurrency,
# and the default keymap end to end (docs/keymap.md -> resolve_command -> ui::Workspace ->
# BrowserModel). Activate on files goes to a recording launcher; nothing is spawned.
# Phase 3: Workspace session / named-layout round trips, the session-writer hammer and lazy tabs
# (workspace_session_test.cpp; the Workspace sources are part of navigator_ui_browser).
if(TARGET navigator_ui_browser)
    add_executable(navigator_browser_integration_tests
        browser_race_test.cpp
        keymap_e2e_test.cpp
        workspace_session_test.cpp
    )
    target_link_libraries(navigator_browser_integration_tests
        PRIVATE
            navigator_ui_browser
            navigator::commands
            navigator::core
            navigator::platform
            navigator::options
            Catch2::Catch2WithMain
    )
    target_compile_definitions(navigator_browser_integration_tests PRIVATE
        NAVIGATOR_KEYMAP_DOC="${PROJECT_SOURCE_DIR}/docs/keymap.md")
    catch_discover_tests(navigator_browser_integration_tests
        TEST_PREFIX "integration."
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 120 LABELS integration
    )
endif()

# Headless app: fixture folder, unreadable start folder and missing start path, each for 120
# frames on the dummy video driver (tests/integration/app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_headless_start_paths
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/app_smoke.sh $<TARGET_FILE:navigator> 120
    )
    set_tests_properties(integration.app_headless_start_paths PROPERTIES
        TIMEOUT 120
        LABELS "integration;app"
    )
endif()

# Phase 3 headless app: two runs on one --config-dir (3 panes saved, then restored), a corrupt
# session.json (moved to .bad, default layout), --no-session, missing folders, PATH + session
# (tests/integration/session_app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_session_restart
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/session_app_smoke.sh $<TARGET_FILE:navigator>
    )
    set_tests_properties(integration.app_session_restart PROPERTIES
        TIMEOUT 120
        LABELS "integration;app"
    )
endif()

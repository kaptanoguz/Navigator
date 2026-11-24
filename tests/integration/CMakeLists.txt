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
# Phase 4: GoTo / address bar with the real path worker (goto_e2e_test.cpp), bookmarks and
# recents across unmount / restart / remount on a fake volume table, and the three config-dir
# writers racing (places_e2e_test.cpp).
# Phase 5: ranking gold tests through walk, index and both flattened views
# (search_gold_test.cpp), hidden policy / permission skips / type filter / cancel on real
# walks and indexes (flatten_e2e_test.cpp), index file lifecycle with inotify
# (index_lifecycle_test.cpp); helpers in search_support.hpp.
if(TARGET navigator_ui_browser)
    add_executable(navigator_browser_integration_tests
        browser_race_test.cpp
        goto_e2e_test.cpp
        keymap_e2e_test.cpp
        places_e2e_test.cpp
        flatten_e2e_test.cpp
        index_lifecycle_test.cpp
        search_gold_test.cpp
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

# Phase 4 headless app: Ctrl+B / Ctrl+D / Ctrl+L / Ctrl+G / Ctrl+Shift+C through the real key
# path, a bookmark on a fake volume table across unmount and remount elsewhere
# (NAVIGATOR_DEBUG_VOLUMES), and no writes under a temporary HOME / XDG_CONFIG_HOME without
# --config-dir (tests/integration/places_app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_places_chords
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/places_app_smoke.sh $<TARGET_FILE:navigator>
    )
    set_tests_properties(integration.app_places_chords PROPERTIES
        TIMEOUT 120
        LABELS "integration;app"
    )
endif()

# Phase 5 headless app: no cache writes in headless runs without --cache-dir (temporary HOME /
# XDG dirs), --no-index writes nothing, the fixture-only index (NAVIGATOR_DEBUG_INDEX_ROOT; never
# a real volume) is saved 0600 in a 0700 folder and rebuilt after corruption
# (tests/integration/index_app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_index_cache_rules
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/index_app_smoke.sh $<TARGET_FILE:navigator>
    )
    set_tests_properties(integration.app_index_cache_rules PROPERTIES
        TIMEOUT 120
        LABELS "integration;app"
    )
endif()

# Phase 6: the preview service end to end (navigator_preview_integration_tests): Freedesktop
# interop with thumbnails made by a system tool (thumbnail_interop_test.cpp), the installed
# .thumbnailer programs through the REAL process runner, timeouts and the output cap with fake
# thumbnailer scripts (thumbnailer_e2e_test.cpp), corrupt / truncated / huge-header fixtures
# through service + cache + thumbnail writer, cancellation storms and the memory budget
# (preview_robustness_test.cpp). Fixtures are made at run time with ffmpeg /
# gdk-pixbuf-thumbnailer (a case SKIPs without them); cache roots are temporary directories only.
if(TARGET navigator_preview)
    add_executable(navigator_preview_integration_tests
        thumbnail_interop_test.cpp
        thumbnailer_e2e_test.cpp
        preview_robustness_test.cpp
    )
    target_link_libraries(navigator_preview_integration_tests
        PRIVATE
            navigator::preview
            navigator::core
            navigator::platform
            navigator::options
            Catch2::Catch2WithMain
    )
    catch_discover_tests(navigator_preview_integration_tests
        TEST_PREFIX "integration."
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 180 LABELS integration
    )
endif()

# Phase 6 headless app: Inspector on image / video / text / folder, key:Space routing (Space in
# the filter field never inspects), no cache writes and no thumbnailer (or bwrap) spawns in
# headless runs without --thumbnail-dir / --cache-dir, checked with fake programs that leave
# markers; control runs with --thumbnail-dir (tests/integration/preview_app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_preview_rules
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/preview_app_smoke.sh $<TARGET_FILE:navigator>
    )
    set_tests_properties(integration.app_preview_rules PROPERTIES
        TIMEOUT 180
        SKIP_RETURN_CODE 77
        LABELS "integration;app;preview"
    )
endif()

# Phase 7: the engine against the REAL platform trash (ops_trash_e2e_test.cpp), atomicity, cancel
# storms, hostile trees, protected paths, 100k files and overlapping queue operations
# (ops_fault_test.cpp). Fixtures only: ops_support.hpp aborts the process on any path outside a
# mkdtemp fixture (and on any path inside a symlink-trap sentinel tree). The only private header
# used is src/ops/sys.hpp, for the I/O fault hook seam that tests/ops uses too.
if(TARGET navigator_ops)
    add_executable(navigator_ops_integration_tests
        ops_trash_e2e_test.cpp
        ops_fault_test.cpp
    )
    target_include_directories(navigator_ops_integration_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/ops)
    target_link_libraries(navigator_ops_integration_tests
        PRIVATE
            navigator::ops
            navigator::platform
            navigator::core
            navigator::options
            Catch2::Catch2WithMain
    )
    catch_discover_tests(navigator_ops_integration_tests
        TEST_PREFIX "integration.ops."
        DISCOVERY_MODE PRE_TEST
        PROPERTIES TIMEOUT 900 LABELS integration
    )
endif()

# Phase 7 headless app, adversarial: two-pane copy / move / undo, Trash view restore, conflict
# sheet decisions, permanent delete refused without the confirmation, no trash without
# --trash-dir, quit while operations run, focus scoping of the file chords. Temporary HOME / XDG
# dirs and fixture trash only (tests/integration/ops_app_smoke.sh).
if(TARGET navigator)
    add_test(NAME integration.app_ops_adversarial
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/ops_app_smoke.sh $<TARGET_FILE:navigator>
    )
    set_tests_properties(integration.app_ops_adversarial PROPERTIES
        TIMEOUT 600
        SKIP_RETURN_CODE 77
        LABELS "integration;app;ops"
    )
endif()

# Phase 7: INCOMING drops at protocol level: our own Xlib XDND source (xdnd_sender.c) against the
# real window under a private Xvfb (tests/integration/dnd_incoming_smoke.sh).
find_package(X11 QUIET)
if(TARGET navigator AND X11_FOUND AND X11_INCLUDE_DIR)
    add_executable(navigator_xdnd_sender xdnd_sender.c)
    target_include_directories(navigator_xdnd_sender PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(navigator_xdnd_sender PRIVATE ${X11_X11_LIB})
    add_test(NAME integration.app_dnd_incoming_xdnd
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dnd_incoming_smoke.sh $<TARGET_FILE:navigator>
                $<TARGET_FILE:navigator_xdnd_sender>)
    set_tests_properties(integration.app_dnd_incoming_xdnd PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "integration;app;dnd")
endif()

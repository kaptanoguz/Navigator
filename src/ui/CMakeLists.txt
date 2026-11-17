# navigator_ui (owner: ui): window, frame loop, project renderer, text, theme, i18n, and the
# Phase 2 browser (model, workers, view).
# SDL3 / FreeType / HarfBuzz / Fontconfig are PRIVATE: they never leak into include/navigator.

find_package(Threads REQUIRED)

# Headless browser logic (no SDL): model, listing/watch workers, formatting, i18n catalog, the
# Phase 3 workspace (tabs/panes state, pane geometry, tab strip math, drag and drop, Search
# Tabs filtering, session writer) and the Phase 4 places model (bookmarks, recents, volume and
# probe workers), sidebar rows, path entry (GoTo / address bar) with its worker, text editing.
# Unit-tested on fixture directories in tests/ui.
add_library(navigator_ui_browser STATIC
    browser/browser_model.cpp
    browser/format.cpp
    browser/listing_worker.cpp
    browser/watch_worker.cpp
    i18n/i18n.cpp
    path/copy_path.cpp
    path/path_entry.cpp
    path/path_worker.cpp
    sidebar/places_model.cpp
    sidebar/sidebar_model.cpp
    widgets/text_edit.cpp
    workspace/drag_drop.cpp
    workspace/pane_geometry.cpp
    workspace/session_writer.cpp
    workspace/tab_search.cpp
    workspace/tab_strip.cpp
    workspace/workspace.cpp
)
target_include_directories(navigator_ui_browser PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(navigator_ui_browser
    PUBLIC
        navigator::headers
        navigator::commands
        navigator::core
        navigator::platform
        Threads::Threads
    PRIVATE
        navigator::options
)

add_library(navigator_ui STATIC
    browser/browser_chrome.cpp
    browser/browser_view.cpp
    input/sdl_input.cpp
    render/backend_name.cpp
    render/renderer.cpp
    shell/app.cpp
    shell/background.cpp
    shell/debug_volumes.cpp
    shell/painter.cpp
    shell/ui_settings.cpp
    text/font.cpp
    text/font_cache.cpp
    sidebar/sidebar_view.cpp
    text/text_renderer.cpp
    theme/theme.cpp
    widgets/icons.cpp
    widgets/text_field.cpp
    widgets/text_fit.cpp
    workspace/goto_popup.cpp
    workspace/popups.cpp
    workspace/workspace_view.cpp
)
add_library(navigator::ui ALIAS navigator_ui)

# Internal headers are included as "theme/theme.hpp", "text/font.hpp", ... (tests use them too).
target_include_directories(navigator_ui PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_definitions(navigator_ui PRIVATE
    NAVIGATOR_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(navigator_ui
    PUBLIC
        navigator::headers
        navigator::commands
        navigator_ui_browser
    PRIVATE
        navigator::options
        SDL3::SDL3
        Freetype::Freetype
        harfbuzz::harfbuzz
        Fontconfig::Fontconfig
)

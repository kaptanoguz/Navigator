# navigator_ui (owner: ui): window, frame loop, project renderer, text, theme, i18n, and the
# Phase 2 browser (model, workers, view).
# SDL3 / FreeType / HarfBuzz / Fontconfig are PRIVATE: they never leak into include/navigator.

find_package(Threads REQUIRED)

# Headless browser logic (no SDL): model, listing/watch workers, formatting, i18n catalog, the
# Phase 3 workspace (tabs/panes state, pane geometry, tab strip math, drag and drop, Search
# Tabs filtering, session writer) and the Phase 4 places model (bookmarks, recents, volume and
# probe workers), sidebar rows, path entry (GoTo / address bar) with its worker, text editing.
# Phase 5: rows (folder filter, type filter, flattened view), the flatten walk thread, the
# search worker, the index service and the filter-field key rules (src/ui/search).
# Phase 6: Inspector logic (controller, zoom math, text wrapping, metadata), the thumbnail
# scheduler and the preview service glue (src/ui/inspector).
# Unit-tested on fixture directories in tests/ui.
add_library(navigator_ui_browser STATIC
    browser/browser_model.cpp
    browser/browser_model_search.cpp
    browser/format.cpp
    browser/listing_worker.cpp
    browser/tree_model.cpp
    browser/view_rows.cpp
    browser/watch_worker.cpp
    dnd/file_drag.cpp
    i18n/i18n.cpp
    input/shortcut_text.cpp
    inspector/inspector_controller.cpp
    inspector/metadata.cpp
    inspector/preview_backend.cpp
    inspector/text_layout.cpp
    inspector/thumbnail_scheduler.cpp
    inspector/zoom.cpp
    menu/menu_model.cpp
    menu/menu_pins.cpp
    ops/app_trash.cpp
    ops/file_ops.cpp
    ops/op_text.cpp
    path/copy_path.cpp
    path/path_entry.cpp
    path/path_worker.cpp
    search/flatten_walk.cpp
    search/highlight_map.cpp
    search/index_service.cpp
    search/index_status.cpp
    search/organize.cpp
    search/search_worker.cpp
    search/filter_rules.cpp
    sidebar/places_model.cpp
    sidebar/sidebar_model.cpp
    trash/trash_model.cpp
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
        navigator::search
        navigator::ops
        navigator::plugins
        Threads::Threads
    PRIVATE
        navigator::options
        nlohmann_json::nlohmann_json
)
# Phase 6: previews and thumbnails (src/preview). The ui codes against the frozen headers; a
# tree without the preview module still builds (no service: type icons and "No preview").
if(TARGET navigator_preview)
    target_link_libraries(navigator_ui_browser PUBLIC navigator_preview)
    target_compile_definitions(navigator_ui_browser PUBLIC NAVIGATOR_UI_HAVE_PREVIEW=1)
endif()

add_library(navigator_ui STATIC
    browser/browser_chrome.cpp
    browser/browser_modes_view.cpp
    browser/browser_search_view.cpp
    browser/browser_view.cpp
    input/sdl_input.cpp
    keymap/keymap_editor_view.cpp
    menu/context_menu_view.cpp
    menu/openwith_popup.cpp
    ops/batch_rename_view.cpp
    ops/ops_view.cpp
    palette/palette_view.cpp
    platform/outgoing_drag.cpp
    platform/wayland_drag.cpp
    platform/xdnd_source.cpp
    ops/sdl_clipboard.cpp
    inspector/inspector_view.cpp
    inspector/list_thumbnails.cpp
    inspector/texture_cache.cpp
    render/backend_name.cpp
    render/renderer.cpp
    search/content_search_overlay.cpp
    settings/appearance_page.cpp
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
    trash/trash_view.cpp
    widgets/icons.cpp
    widgets/text_field.cpp
    widgets/text_fit.cpp
    widgets/tooltip.cpp
    workspace/goto_popup.cpp
    workspace/popups.cpp
    workspace/types_popup.cpp
    workspace/workspace_view.cpp
)
add_library(navigator::ui ALIAS navigator_ui)

# Internal headers are included as "theme/theme.hpp", "text/font.hpp", ... (tests use them too).
target_include_directories(navigator_ui PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_compile_definitions(navigator_ui PRIVATE
    NAVIGATOR_VERSION_STRING="${PROJECT_VERSION}")

# Phase 7: outgoing drags (docs/dnd.md). Optional: without the X11 or Wayland development
# headers the backend is compiled out and the ui falls back to the clipboard path.
find_package(X11 QUIET)
if(X11_FOUND AND X11_INCLUDE_DIR)
    target_compile_definitions(navigator_ui PRIVATE NAVIGATOR_UI_HAVE_X11=1)
    target_include_directories(navigator_ui PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(navigator_ui PRIVATE ${X11_X11_LIB})
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(NAVIGATOR_WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    if(NAVIGATOR_WAYLAND_CLIENT_FOUND)
        target_compile_definitions(navigator_ui PRIVATE NAVIGATOR_UI_HAVE_WAYLAND=1)
        target_link_libraries(navigator_ui PRIVATE PkgConfig::NAVIGATOR_WAYLAND_CLIENT)
    endif()
endif()

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

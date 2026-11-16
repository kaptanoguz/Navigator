# navigator_ui (owner: ui): window, frame loop, project renderer, text, theme, i18n.
# SDL3 / FreeType / HarfBuzz / Fontconfig are PRIVATE: they never leak into include/navigator.

add_library(navigator_ui STATIC
    i18n/i18n.cpp
    input/keybindings.cpp
    render/renderer.cpp
    shell/app.cpp
    shell/background.cpp
    shell/keyboard.cpp
    shell/scene.cpp
    text/font.cpp
    text/text_renderer.cpp
    theme/theme.cpp
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
    PRIVATE
        navigator::options
        SDL3::SDL3
        Freetype::Freetype
        harfbuzz::harfbuzz
        Fontconfig::Fontconfig
)

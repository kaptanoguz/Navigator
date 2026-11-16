# navigator executable (owner: ui for Phase 0; integration reviews main.cpp).

# Command-line parsing and locale detection, kept out of main() so tests can cover them.
add_library(navigator_app_cli STATIC cli.cpp)
target_include_directories(navigator_app_cli PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_definitions(navigator_app_cli PRIVATE
    NAVIGATOR_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(navigator_app_cli
    PUBLIC navigator::headers
    PRIVATE navigator::options)

add_executable(navigator main.cpp)
target_link_libraries(navigator PRIVATE navigator_app_cli navigator::ui navigator::options)

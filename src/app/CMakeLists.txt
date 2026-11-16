# navigator executable (owner: ui when assigned; integration reviews main.cpp).

# Command-line parsing, locale detection and the dev benchmark fixture, kept out of main() so
# tests can cover them.
add_library(navigator_app_cli STATIC cli.cpp bench_fixture.cpp)
target_include_directories(navigator_app_cli PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_definitions(navigator_app_cli PRIVATE
    NAVIGATOR_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(navigator_app_cli
    PUBLIC navigator::headers
    PRIVATE navigator::options)

add_executable(navigator main.cpp)
target_link_libraries(navigator PRIVATE navigator_app_cli navigator::ui navigator::options)

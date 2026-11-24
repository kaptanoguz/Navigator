# navigator_search tests (owner: search). Filesystem tests work in fresh
# temp_directory_path()/navigator-test-<random> directories (temp_dir.hpp) and remove them.
# navigator_platform is linked here ONLY for the real-inotify index test; navigator_search itself
# never depends on src/platform.

add_executable(navigator_search_tests
    content_search_test.cpp
    index_file_test.cpp
    index_regression_test.cpp
    index_test.cpp
    match_test.cpp
    palette_history_test.cpp
    palette_test.cpp
    types_test.cpp
)
target_include_directories(navigator_search_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/search)
target_link_libraries(navigator_search_tests
    PRIVATE
        navigator::search
        navigator::platform
        navigator::options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(navigator_search_tests
    TEST_PREFIX "search."
    DISCOVERY_MODE PRE_TEST
)

# navigator_core tests (owner: core-fs). Every filesystem test works in a fresh
# temp_directory_path()/navigator-test-<random> directory (temp_dir.hpp) and removes it.

add_executable(navigator_core_tests
    fold_into_test.cpp
    layout_files_test.cpp
    layout_json_test.cpp
    layout_test.cpp
    listing_test.cpp
    names_test.cpp
    path_input_test.cpp
    perf_test.cpp
    places_json_test.cpp
    places_test.cpp
    sort_test.cpp
    walk_test.cpp
)
target_link_libraries(navigator_core_tests
    PRIVATE
        navigator::core
        navigator::options
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(navigator_core_tests
    TEST_PREFIX "core."
    DISCOVERY_MODE PRE_TEST
)

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

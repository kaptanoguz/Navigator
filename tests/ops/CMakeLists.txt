# navigator_ops_tests (owner: ops). EVERY test works inside temporary fixture directories
# (fixture.hpp: mkdtemp under temp_directory_path(), or under /dev/shm for the cross-filesystem
# cases) and asserts that every path it hands to the engine lies inside a fixture; the trash is
# the fixture-local FakeTrash (fake_trash.hpp), never the real XDG trash (S16).
add_executable(navigator_ops_tests
    archive_test.cpp
    batch_rename_test.cpp
    cancel_fault_test.cpp
    conflict_test.cpp
    copy_test.cpp
    move_test.cpp
    names_io_test.cpp
    names_test.cpp
    perf_test.cpp
    queue_test.cpp
    support.cpp
    trash_delete_test.cpp
    undo_test.cpp
)
target_include_directories(navigator_ops_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/ops)
target_link_libraries(navigator_ops_tests
    PRIVATE
        navigator::ops
        navigator::options
        Catch2::Catch2WithMain
)
include(Catch)
catch_discover_tests(navigator_ops_tests
    TEST_PREFIX "ops."
    DISCOVERY_MODE PRE_TEST
)

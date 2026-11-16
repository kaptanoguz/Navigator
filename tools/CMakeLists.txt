# Developer tools (integration-owned). navigator-ls: Phase 1 smoke for core + platform.
add_executable(navigator-ls navigator_ls.cpp)
target_link_libraries(navigator-ls
    PRIVATE
        navigator::core
        navigator::platform
        navigator::headers
        navigator::options
)

if(NAVIGATOR_BUILD_TESTS)
    add_test(NAME tools.navigator_ls_lists_docs
             COMMAND navigator-ls -a "${PROJECT_SOURCE_DIR}/docs")
    set_tests_properties(tools.navigator_ls_lists_docs PROPERTIES
        PASS_REGULAR_EXPRESSION "features\\.md")
    add_test(NAME tools.navigator_ls_volumes COMMAND navigator-ls --volumes)
    set_tests_properties(tools.navigator_ls_volumes PROPERTIES
        PASS_REGULAR_EXPRESSION "root ")
    add_test(NAME tools.navigator_ls_missing_path COMMAND navigator-ls /nonexistent/navigator)
    set_tests_properties(tools.navigator_ls_missing_path PROPERTIES WILL_FAIL TRUE)
endif()

# search_bench (owner: search): Phase 5 benchmark for filter_entries, fs::Walker and the name
# index. Results: docs/perf/phase5-search.md. Not a test (generates ~300k files); run manually
# from a Release build: search_bench [--entries N] [--dir DIR] [--only filter|walk|index|million].
add_executable(search_bench main.cpp)
# The synthetic 1M index and the save timing use the file codec of src/search directly.
target_include_directories(search_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/search)
target_link_libraries(search_bench
    PRIVATE
        navigator::search
        navigator::core
        navigator::options
)

# navigator_search (owner: search): Phase 5 name matching (compile_query, Matcher, ranking),
# the type visibility filter, the filter pipeline (navigator/search.hpp) and the persistent
# name index (navigator/index.hpp). Links navigator_core only: inotify arrives through the
# injected WatcherFactory, never through src/platform.

add_library(navigator_search STATIC
    index.cpp
    index_tree.cpp
    match.cpp
    types.cpp
)
add_library(navigator::search ALIAS navigator_search)

find_package(Threads REQUIRED)
target_link_libraries(navigator_search
    PUBLIC
        navigator::headers
        navigator::core
        Threads::Threads
    PRIVATE
        navigator::options
)

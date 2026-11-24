# navigator_search (owner: search): Phase 5 name matching (compile_query, Matcher, ranking),
# the type visibility filter, the filter pipeline (navigator/search.hpp) and the persistent
# name index (navigator/index.hpp). Links navigator_core only: inotify arrives through the
# injected WatcherFactory, never through src/platform.

add_library(navigator_search STATIC
    index.cpp
    index_tree.cpp
    match.cpp
    palette.cpp
    palette_files.cpp
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
        nlohmann_json::nlohmann_json
)
# palette_files.cpp reuses the shared document file rules and JSON helpers of navigator_core
# (src/core/document_files.hpp, json_util.hpp: internal headers, symbols come from the static
# navigator_core). Phase 8: the command palette (navigator/palette.hpp) ranking and history.
target_include_directories(navigator_search PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

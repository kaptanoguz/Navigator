# navigator_core (owner: core-fs): directory listing, metadata, name folding/comparison and
# sorting for navigator/fs.hpp, the tab/pane layout model, session JSON and session /
# named-layout files for navigator/layout.hpp, and (Phase 4) places / bookmarks / recents for
# navigator/places.hpp and typed-path handling for navigator/path_input.hpp, and (Phase 5) the
# recursive fs::Walker for navigator/walk.hpp (walk.cpp; entry_meta.cpp is the per-entry metadata
# shared with list_directory). POSIX only; no SDL, GIO, UI or src/platform
# dependency. nlohmann_json is a PRIVATE dependency (never visible in public headers).
#
# case_fold_data.inc is generated offline by tools/gen_case_fold.py from the system
# CaseFolding.txt (Ubuntu unicode-data); the build does not regenerate it.

add_library(navigator_core STATIC
    case_fold.cpp
    document_files.cpp
    entry_meta.cpp
    json_util.cpp
    layout.cpp
    layout_files.cpp
    layout_json.cpp
    listing.cpp
    names.cpp
    path_input.cpp
    places.cpp
    places_json.cpp
    sort.cpp
    view_settings.cpp
    view_settings_json.cpp
    walk.cpp
)
add_library(navigator::core ALIAS navigator_core)

target_link_libraries(navigator_core
    PUBLIC
        navigator::headers
    PRIVATE
        navigator::options
        nlohmann_json::nlohmann_json
)
